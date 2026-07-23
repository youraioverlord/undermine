/* UNDERMINE — main: raylib window, fixed 50 Hz loop, C64-constrained renderer.
 * The renderer is a read-only view of GameState; all logic lives in sim.c.
 *
 *   undermine             run the game
 *   undermine --selftest  headless sim smoke test, no window
 *   undermine --shot F    render one frame to PNG file F and exit
 */
#define _CRT_SECURE_NO_WARNINGS   /* fopen/strcpy are fine for our local score file */
#define GAME_VERSION "1.0"
#include "raylib.h"
#include "sim.h"
#include "synth.h"
#include "bot1.h"
#include "bot2.h"
#include "mole.h"
#include "filedlg.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>

/* The 16 C64 colors (Pepto palette) — the only colors allowed anywhere. */
static const Color C64[16] = {
    {0x00,0x00,0x00,255}, /*  0 black      */
    {0xFF,0xFF,0xFF,255}, /*  1 white      */
    {0x68,0x37,0x2B,255}, /*  2 red        */
    {0x70,0xA4,0xB2,255}, /*  3 cyan       */
    {0x6F,0x3D,0x86,255}, /*  4 purple     */
    {0x58,0x8D,0x43,255}, /*  5 green      */
    {0x35,0x28,0x79,255}, /*  6 blue       */
    {0xB8,0xC7,0x6F,255}, /*  7 yellow     */
    {0x6F,0x4F,0x25,255}, /*  8 orange     */
    {0x43,0x39,0x00,255}, /*  9 brown      */
    {0x9A,0x67,0x59,255}, /* 10 light red  */
    {0x44,0x44,0x44,255}, /* 11 dark grey  */
    {0x6C,0x6C,0x6C,255}, /* 12 grey       */
    {0x9A,0xD2,0x84,255}, /* 13 lt green   */
    {0x6C,0x5E,0xB5,255}, /* 14 lt blue    */
    {0x95,0x95,0x95,255}, /* 15 lt grey    */
};
enum { BLACK_=0, WHITE_, RED_, CYAN_, PURPLE_, GREEN_, BLUE_, YELLOW_,
       ORANGE_, BROWN_, LTRED_, DKGREY_, GREY_, LTGREEN_, LTBLUE_, LTGREY_ };

#define WINDOW_SCALE 3

/* Optional MoleScript bots loaded from .mole files (--bot1 / --bot2). When a
 * slot is loaded, it drives that screen in place of the built-in C bot; when
 * NULL, the default bot1/bot2 play. See mole.h / MOLESCRIPT.md. */
static MoleVM *g_mole1 = NULL;
static MoleVM *g_mole2 = NULL;
static char g_mole1Name[64] = "STEADY";    /* in-game label: built-in bot name, or the
                                            * loaded script's name (its file basename) */
static char g_mole2Name[64] = "RUSHER";
static char g_mole1Auth[16] = "";          /* loaded script's author, for the title line */
static char g_mole2Auth[16] = "";
static char g_loadMsg[80] = "";            /* transient load result banner */
static long g_loadMsgUntil = 0;            /* uiTicks until which g_loadMsg shows */
static char g_seedBuf[8] = "";             /* title-screen hex seed entry (S key) */
static int  g_seedLen = 0;
static bool g_seedEntry = false;           /* currently typing a seed? */
#define SEED_DIGITS 6                      /* seeds are at most 6 hex digits */

/* Every run seed fits in SEED_DIGITS hex digits, so what the HUD shows is
 * short enough to remember and retype. 0 means "no seed" internally. */
static uint64_t seed_clip(uint64_t v)
{
    v &= 0xFFFFFFULL;
    return v ? v : 1;
}
#define MOLE_RESET1(seed) do { if (g_mole1) mole_reset(g_mole1, (seed)); } while (0)
#define MOLE_RESET2(seed) do { if (g_mole2) mole_reset(g_mole2, (seed)); } while (0)

static const char *basename_of(const char *p)
{
    const char *b = p;
    for (; *p; p++) if (*p == '/' || *p == '\\') b = p + 1;
    return b;
}

/* Labels are capped at 10 visible characters so they fit their lines. The
 * in-game label is the SCRIPT's name (file basename, uppercased) — two bots
 * by the same author must still be tellable apart on screen; the author
 * (creator) is shown on the title line instead. */
#define BOT_LABEL_MAX 10
static void set_bot_label(char *dst, const char *author)
{
    int i;
    for (i = 0; i < BOT_LABEL_MAX && author[i]; i++) dst[i] = author[i];
    dst[i] = '\0';
}
static void set_bot_file_label(char *dst, const char *path)
{
    const char *b = basename_of(path);
    int i;
    for (i = 0; i < BOT_LABEL_MAX && b[i] && b[i] != '.'; i++)
        dst[i] = (char)toupper((unsigned char)b[i]);
    dst[i] = '\0';
    if (!dst[0]) { dst[0] = 'B'; dst[1] = 'O'; dst[2] = 'T'; dst[3] = '\0'; }
}

/* say logging — each slot's introspection lines go to bot1.log / bot2.log in
 * the working directory. A log is (re)opened fresh at every run start, so an
 * existing file is silently overwritten; lines are written whenever the
 * script's say message changes, stamped with depth and room tick. */
static FILE *g_sayLog[2]   = {NULL, NULL};
static char  g_sayLast[2][80] = {{0}, {0}};

static void say_log_open(int which)                 /* 0 -> bot1.log, 1 -> bot2.log */
{
    if (g_sayLog[which]) fclose(g_sayLog[which]);
    g_sayLog[which] = fopen(which ? "bot2.log" : "bot1.log", "w");
    g_sayLast[which][0] = '\0';
}

static void say_log_tick(int which, const char *msg, const GameState *g)
{
    if (!msg || !g_sayLog[which]) return;
    if (!msg[0] || strcmp(msg, g_sayLast[which]) == 0) return;
    snprintf(g_sayLast[which], sizeof g_sayLast[which], "%s", msg);
    fprintf(g_sayLog[which], "[d%d t%05ld] %s\n", g->depth, g->ticks, msg);
    fflush(g_sayLog[which]);
}

/* Open the file picker and load a .mole into slot `which` (1 or 2). On failure
 * the slot is left unchanged (still the previous script or the built-in bot). */
static void load_bot(int which, long now)
{
    char path[1024], err[160];
    MoleVM *vm;
    if (!filedlg_open_mole(path, sizeof path)) return;   /* cancelled */
    vm = mole_load_file(path, DEFAULT_SEED, err, sizeof err);
    if (!vm) {
        snprintf(g_loadMsg, sizeof g_loadMsg, "BOT%d LOAD FAILED: %s", which, err);
        g_loadMsgUntil = now + 300;
        return;
    }
    if (which == 1) {
        if (g_mole1) mole_free(g_mole1);
        g_mole1 = vm;
        set_bot_file_label(g_mole1Name, path);
        set_bot_label(g_mole1Auth, mole_author(vm));
    } else {
        if (g_mole2) mole_free(g_mole2);
        g_mole2 = vm;
        set_bot_file_label(g_mole2Name, path);
        set_bot_label(g_mole2Auth, mole_author(vm));
    }
    snprintf(g_loadMsg, sizeof g_loadMsg, "BOT%d: %s  BY %s", which,
             basename_of(path), mole_author(vm));
    g_loadMsgUntil = now + 300;
}

static void draw_tiles(const GameState *s)
{
    int tx, ty;
    for (ty = 0; ty < ROOM_H; ty++) {
        for (tx = 0; tx < ROOM_W; tx++) {
            int x = tx * TILE, y = HUD_H + ty * TILE;
            switch (s->tiles[ty][tx]) {   /* raw tile: crumble must stay distinct */
            case TILE_SOLID:
                DrawRectangle(x, y, TILE, TILE, C64[ORANGE_]);
                DrawRectangle(x + 1, y + 1, TILE - 2, 7, C64[BROWN_]);
                DrawRectangle(x + 2, y + 10, TILE - 4, 4, C64[BROWN_]);
                break;
            case TILE_CRUMBLE: {          /* weak floor: cracked; shudders when arming */
                int st = s->crumble[ty][tx], j;
                if (st < 0) break;        /* collapsed — draw nothing */
                j = (st > 0) ? (int)((s->ticks / 2) % 2) : 0;   /* shudder while arming */
                DrawRectangle(x + j, y, TILE, TILE, C64[BROWN_]);
                DrawRectangle(x + 1 + j, y + 1, TILE - 2, 5, C64[ORANGE_]);
                DrawRectangle(x + 3 + j, y, 2, TILE, C64[BLACK_]);      /* cracks */
                DrawRectangle(x + 9 + j, y, 1, TILE, C64[BLACK_]);
                DrawRectangle(x + 6 + j, y + 8, 2, 5, C64[BLACK_]);
                break;
            }
            case TILE_ONEWAY:
                DrawRectangle(x, y, TILE, 4, C64[YELLOW_]);
                DrawRectangle(x, y + 4, TILE, 1, C64[BROWN_]);
                break;
            case TILE_LADDER: {
                int r;
                DrawRectangle(x + 3, y, 2, TILE, C64[LTGREEN_]);
                DrawRectangle(x + 11, y, 2, TILE, C64[LTGREEN_]);
                for (r = 2; r < TILE; r += 5)
                    DrawRectangle(x + 3, y + r, 10, 1, C64[GREEN_]);
                break;
            }
            case TILE_SPIKE: {
                int i;
                for (i = 0; i < 4; i++) { /* 4 fat triangles-ish spikes */
                    DrawRectangle(x + i * 4 + 1, y + 8, 2, 8, C64[LTGREY_]);
                    DrawRectangle(x + i * 4,     y + 12, 4, 4, C64[GREY_]);
                }
                break;
            }
            case TILE_EXIT:
                DrawRectangle(x, y, TILE, TILE, C64[BLACK_]);
                DrawRectangle(x, y, TILE, 2, C64[YELLOW_]);
                DrawRectangle(x, y, 2, TILE, C64[YELLOW_]);
                DrawRectangle(x + TILE - 2, y, 2, TILE, C64[YELLOW_]);
                break;
            default:
                break;
            }
            if (s->coal[ty][tx]) {                 /* collectible lump */
                DrawRectangle(x + 4, y + 8, 8, 6, C64[DKGREY_]);
                DrawRectangle(x + 5, y + 9, 6, 4, C64[BLACK_]);
                if ((s->ticks / 25) % 2 == 0)
                    DrawRectangle(x + 6, y + 9, 2, 2, C64[WHITE_]); /* glint */
            }
        }
    }
    /* closed shaft cap: hint where the exit will open once quota (and key) met */
    if (!(s->coalGot >= s->coalTotal && (!s->keyRoom || s->keyGot))) {
        int x = s->exitCol * TILE, y = HUD_H + BEDROCK_ROW * TILE;
        DrawRectangle(x, y + 5, TILE, 3, C64[GREY_]);
        DrawRectangle(x + 2, y + 2, TILE - 4, 3, C64[LTGREY_]);
    }
}

static void draw_monty(const GameState *s)
{
    /* placeholder mole: 12x18 box aligned to the 10x18 hitbox center */
    int x = (int)s->px - 1;
    int y = HUD_H + (int)s->py;
    int frame = (s->animTimer / 6) % 2;
    int body = BROWN_, belly = ORANGE_;

    if (s->state == PS_DYING) {           /* flash white / red */
        if ((s->dyingTimer / 3) % 2) { body = WHITE_; belly = WHITE_; }
        else                         { body = RED_;   belly = LTRED_; }
    }

    DrawRectangle(x, y + 2, 12, 14, C64[body]);              /* body   */
    DrawRectangle(x + 3, y + 8, 6, 7, C64[belly]);           /* belly  */
    DrawRectangle(x + 2, y, 8, 4, C64[body]);                /* head   */
    if (s->facing > 0) {
        DrawRectangle(x + 8, y + 1, 2, 2, C64[WHITE_]);      /* eye    */
        DrawRectangle(x + 10, y + 2, 2, 2, C64[LTRED_]);     /* snout  */
    } else {
        DrawRectangle(x + 2, y + 1, 2, 2, C64[WHITE_]);
        DrawRectangle(x + 0, y + 2, 2, 2, C64[LTRED_]);
    }
    /* feet: 2-frame shuffle */
    if (frame == 0) {
        DrawRectangle(x + 1, y + 16, 4, 2, C64[BLACK_]);
        DrawRectangle(x + 8, y + 16, 3, 2, C64[BLACK_]);
    } else {
        DrawRectangle(x + 2, y + 16, 3, 2, C64[BLACK_]);
        DrawRectangle(x + 7, y + 16, 4, 2, C64[BLACK_]);
    }
}

static void draw_enemies(const GameState *s)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        const Enemy *e = &s->enemies[i];
        int x = (int)e->x, y = HUD_H + (int)e->y;
        switch (e->type) {
        case EN_CRUSHER: {
            int warn = (e->phase == 1);                 /* about to slam */
            int slam = (e->phase == 2);                 /* driving down / bottomed out */
            int sh = warn ? (((s->ticks / 2) % 2) ? 1 : -1) : 0;   /* warning shudder */
            int shaftTop = HUD_H + (int)e->baseY;
            int down = (int)(e->y - e->baseY);          /* how far the piston is extended */
            int tx = x + sh, j;
            int teeth = slam ? LTRED_ : (warn ? WHITE_ : LTGREY_);

            /* piston shaft: dark steel with light rivets */
            DrawRectangle(x + 6, shaftTop, 4, down, C64[DKGREY_]);
            for (j = shaftTop + 2; j < y; j += 4) DrawRectangle(x + 7, j, 2, 1, C64[GREY_]);

            /* heavy head, black-outlined slab of metal */
            DrawRectangle(tx, y, TILE, TILE, C64[BLACK_]);
            DrawRectangle(tx + 1, y + 1, TILE - 2, TILE - 3, C64[GREY_]);
            DrawRectangle(tx + 2, y + 1, TILE - 4, 3, C64[LTGREY_]);          /* top highlight */
            DrawRectangle(tx + 2, y + 5, 2, TILE - 8, C64[DKGREY_]);          /* bolts */
            DrawRectangle(tx + TILE - 4, y + 5, 2, TILE - 8, C64[DKGREY_]);
            /* flashing yellow hazard stripes while it winds up to slam */
            if (warn && (s->ticks / 3) % 2 == 0) {
                DrawRectangle(tx + 3, y + 5, TILE - 6, 2, C64[YELLOW_]);
                DrawRectangle(tx + 3, y + 9, TILE - 6, 2, C64[YELLOW_]);
            }
            /* serrated crushing jaw along the bottom, pointing down */
            for (j = 0; j < 4; j++) {
                int px2 = tx + 1 + j * 4;
                DrawRectangle(px2, y + TILE - 4, 3, 2, C64[teeth]);
                DrawRectangle(px2 + 1, y + TILE - 2, 1, 2, C64[teeth]);
            }
            if (slam) DrawRectangle(tx + 1, y + TILE - 1, TILE - 2, 1, C64[RED_]); /* red-hot edge */
            break;
        }
        case EN_FOREMAN: {
            int fr = (e->timer / 6) % 2;
            DrawRectangle(x, y + 2, 12, 14, C64[LTRED_]);
            DrawRectangle(x + 2, y, 8, 4, C64[RED_]);                       /* hard hat */
            DrawRectangle(x + (e->dir > 0 ? 8 : 2), y + 6, 2, 2, C64[BLACK_]);
            DrawRectangle(x + (fr ? 2 : 6), y + 16, 4, 2, C64[BLACK_]);     /* feet */
            if (e->carry > 0) {                                            /* hauling a coal lump */
                DrawRectangle(x + 3, y - 5, 6, 5, C64[DKGREY_]);
                DrawRectangle(x + 4, y - 4, 4, 3, C64[BLACK_]);
            }
            break;
        }
        case EN_BAT: {
            int fr = (e->timer / 8) % 2;
            DrawRectangle(x + 4, y + 2, 4, 6, C64[DKGREY_]);                /* body */
            if (fr) { DrawRectangle(x, y, 4, 4, C64[GREY_]);
                      DrawRectangle(x + 8, y, 4, 4, C64[GREY_]); }
            else    { DrawRectangle(x, y + 3, 4, 3, C64[GREY_]);
                      DrawRectangle(x + 8, y + 3, 4, 3, C64[GREY_]); }
            break;
        }
        case EN_SPIDER: {
            int threadTop = HUD_H + (int)e->baseY;                          /* thread */
            DrawRectangle(x + 5, threadTop, 1, y - threadTop, C64[LTGREY_]);
            DrawRectangle(x + 2, y + 3, 8, 6, C64[GREEN_]);                 /* body */
            DrawRectangle(x + 4, y + 4, 4, 3, C64[BLACK_]);                 /* mark */
            DrawRectangle(x, y + 4, 2, 1, C64[GREEN_]);                     /* legs */
            DrawRectangle(x + 10, y + 4, 2, 1, C64[GREEN_]);
            DrawRectangle(x, y + 7, 2, 1, C64[GREEN_]);
            DrawRectangle(x + 10, y + 7, 2, 1, C64[GREEN_]);
            break;
        }
        case EN_BOULDER: {
            int roll = (e->timer / 4) % 2;                                  /* tumble */
            DrawRectangle(x + 1, y + 1, TILE - 2, TILE - 2, C64[GREY_]);
            DrawRectangle(x + 2, y + 2, TILE - 4, TILE - 4, C64[LTGREY_]);
            DrawRectangle(x + (roll ? 4 : 8), y + 5, 3, 3, C64[GREY_]);     /* speckle */
            DrawRectangle(x + (roll ? 9 : 5), y + 9, 2, 2, C64[GREY_]);
            break;
        }
        default: break;
        }
    }
}

static void draw_pickups(const GameState *s)
{
    if (s->keyRoom && !s->keyGot && s->keyCol >= 0) {       /* yellow key */
        int x = s->keyCol * TILE, y = HUD_H + s->keyRow * TILE;
        DrawRectangle(x + 3, y + 4, 4, 4, C64[YELLOW_]);    /* bow */
        DrawRectangle(x + 4, y + 5, 2, 2, C64[BLACK_]);
        DrawRectangle(x + 7, y + 5, 5, 2, C64[YELLOW_]);    /* shaft */
        DrawRectangle(x + 10, y + 7, 2, 2, C64[YELLOW_]);   /* teeth */
    }
    if (s->darkRoom && !s->lampGot && s->lampCol >= 0) {    /* miner's lamp */
        int x = s->lampCol * TILE, y = HUD_H + s->lampRow * TILE;
        DrawRectangle(x + 6, y + 2, 2, 2, C64[LTGREY_]);    /* hook */
        DrawRectangle(x + 4, y + 4, 6, 8, C64[YELLOW_]);    /* glass */
        DrawRectangle(x + 5, y + 5, 4, 5, C64[WHITE_]);     /* glow */
    }
}

/* Dark rooms: black out everything beyond a square of sight around Monty
 * (blocky, palette-legal). Drawn before Monty so he stays visible. */
static void draw_darkness(const GameState *s)
{
    const int R = 44;
    int cx = (int)s->px + HITBOX_W / 2;
    int cy = HUD_H + (int)s->py + HITBOX_H / 2;
    int L = cx - R, Rr = cx + R, T = cy - R, B = cy + R;
    if (L < 0) L = 0;            if (Rr > SCREEN_W) Rr = SCREEN_W;
    if (T < HUD_H) T = HUD_H;    if (B > SCREEN_H) B = SCREEN_H;
    DrawRectangle(0, HUD_H, SCREEN_W, T - HUD_H, C64[BLACK_]);
    DrawRectangle(0, B, SCREEN_W, SCREEN_H - B, C64[BLACK_]);
    DrawRectangle(0, T, L, B - T, C64[BLACK_]);
    DrawRectangle(Rr, T, SCREEN_W - Rr, B - T, C64[BLACK_]);
}

static void draw_plats(const GameState *s)
{
    int i, k;
    for (i = 0; i < s->platCount; i++) {
        const MovePlat *p = &s->plats[i];
        int x = (int)p->x, y = HUD_H + (int)p->y;
        DrawRectangle(x, y, PLAT_W, PLAT_H, C64[LTGREY_]);
        DrawRectangle(x, y + PLAT_H - 2, PLAT_W, 2, C64[GREY_]);   /* underside */
        for (k = 2; k < PLAT_W - 1; k += 7)                        /* rivets */
            DrawRectangle(x + k, y + 1, 2, 2, C64[GREY_]);
    }
}

static void draw_frame(const GameState *s)
{
    bool open = s->coalGot >= s->coalTotal && (!s->keyRoom || s->keyGot);
    /* background darkens with depth: cycle every 10 rooms (DESIGN §5.3) */
    static const int bg[3] = { BLUE_, PURPLE_, DKGREY_ };
    ClearBackground(C64[bg[((s->depth - 1) / 10) % 3]]);
    DrawRectangle(0, 0, SCREEN_W, 8, C64[BLACK_]);   /* HUD strip, overlaid on the play field */
    DrawText(TextFormat("SC%06ld", s->score), 2, 0, 8, C64[WHITE_]);
    DrawText(TextFormat("COAL %d/%d", s->coalGot, s->coalTotal), 92, 0, 8,
             s->coalGot >= s->coalTotal ? C64[LTGREEN_] : C64[YELLOW_]);
    if (s->keyRoom)
        DrawText("KEY", 176, 0, 8, s->keyGot ? C64[LTGREEN_] : C64[GREY_]);
    DrawText(TextFormat("LV%d", s->lives), 214, 0, 8, C64[LTRED_]);
    DrawText(TextFormat("D%d", s->depth), 270, 0, 8, C64[LTBLUE_]);
    draw_tiles(s);
    draw_plats(s);
    draw_pickups(s);
    draw_enemies(s);
    if (s->darkRoom && !s->lampGot) draw_darkness(s);
    draw_monty(s);
}

/* Audio: the synth renders on raylib's audio thread via this callback. The
 * game thread only calls synth_trigger — a benign race at worst clips one
 * SFX attack, never corrupts the sim (which the synth never touches). */
static Synth g_synth;
static bool  g_soundOff = false;   /* M on the title toggles it */

static void audio_cb(void *buffer, unsigned int frames)
{
    /* render even when muted so songs/SFX keep their place in time —
     * unmuting rejoins the music mid-tune instead of resuming a freeze */
    synth_render(&g_synth, (float *)buffer, (int)frames);
    if (g_soundOff) memset(buffer, 0, frames * sizeof(float));
}

/* Fire SFX from what changed between two sim snapshots. */
static void audio_events(const GameState *prev, const GameState *cur)
{
    if (prev->state == PS_ALIVE && cur->state == PS_DYING) {
        synth_trigger(&g_synth, SFX_DEATH);
        return;
    }
    if (cur->depth > prev->depth)   synth_trigger(&g_synth, SFX_DESCEND);
    if (cur->coalGot > prev->coalGot)
        synth_trigger(&g_synth, cur->coalGot >= cur->coalTotal ? SFX_QUOTA : SFX_COAL);
    if (prev->onGround && !cur->onGround && cur->vy < 0) synth_trigger(&g_synth, SFX_JUMP);
    if (!prev->onGround && cur->onGround)                synth_trigger(&g_synth, SFX_LAND);
    if (cur->depth == prev->depth) {   /* a crusher bottoming out this tick thuds */
        int i;
        for (i = 0; i < cur->enemyCount; i++) {
            const Enemy *e = &cur->enemies[i];
            float bottom = e->baseY + (float)e->range;
            if (e->type == EN_CRUSHER &&
                prev->enemies[i].y < bottom && e->y >= bottom)
                synth_trigger(&g_synth, SFX_SLAM);
        }
    }
}

static Input read_input(void)
{
    Input in = {0};
    in.left  = IsKeyDown(KEY_LEFT)  || IsKeyDown(KEY_A);
    in.right = IsKeyDown(KEY_RIGHT) || IsKeyDown(KEY_D);
    in.up    = IsKeyDown(KEY_UP)    || IsKeyDown(KEY_W);
    in.down  = IsKeyDown(KEY_DOWN)  || IsKeyDown(KEY_S);
    in.jump  = IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_J);
    return in;
}

/* ============================================================= *
 *  Solver-bot integration test (DESIGN.md §7.2.4)
 *
 *  A bot that navigates each generated room by driving the REAL
 *  physics (sim_tick) along a path found over the reachability
 *  graph — walk / climb / drop only (the moves generated rooms
 *  need; no jumps). If a room the checker calls solvable can't be
 *  cleared by the actual physics, the checker's model and the
 *  physics have drifted apart — exactly the bug class §7.2.4 exists
 *  to catch (e.g. the ladder head-clearance regression).
 * ============================================================= */

static int g_botdebug = 0;

static void bot_cell(const GameState *s, int *c, int *r)
{
    *c = (int)((s->px + HITBOX_W * 0.5f) / TILE);
    *r = (int)((s->py + HITBOX_H - 1) / TILE);
}

static bool is_ladder_at(const GameState *s, int c, int r)
{
    return sim_tile_at(s, c, r) == TILE_LADDER;
}

/* BFS over standable cells; returns the first step from (sc,sr) toward the
 * target. Edges: walk (adjacent, same row), climb (ladder up/down), drop
 * (off a side ledge, fall <= 4). Mirrors the checker minus jumps. */
static bool bfs_first_step(const GameState *s, int sc, int sr, int tc, int tr,
                           int *fc, int *fr, int maxDrop)
{
    bool seen[ROOM_H][ROOM_W];
    int px[ROOM_H][ROOM_W], py[ROOM_H][ROOM_W];
    int qx[ROOM_H * ROOM_W], qy[ROOM_H * ROOM_W], head = 0, tail = 0;
    int c, r, dc, dd;

    if (sc == tc && sr == tr) return false;
    memset(seen, 0, sizeof seen);
    seen[sr][sc] = true; px[sr][sc] = -1; py[sr][sc] = -1;
    qx[tail] = sc; qy[tail] = sr; tail++;

    while (head < tail) {
        c = qx[head]; r = qy[head]; head++;
        if (c == tc && r == tr) {                    /* backtrack to first step */
            int cx = c, cy = r;
            while (!(px[cy][cx] == sc && py[cy][cx] == sr)) {
                int nx = px[cy][cx], ny = py[cy][cx];
                if (nx < 0) break;
                cx = nx; cy = ny;
            }
            *fc = cx; *fr = cy;
            if (g_botdebug && tr == 7) { static int n = 0;
                if (n++ < 3) printf("    BFS (%d,%d)->(%d,%d) firststep=(%d,%d)\n",
                                    sc, sr, tc, tr, cx, cy); }
            return true;
        }
        #define BOT_PUSH(NC, NR) do { \
            if (!seen[NR][NC]) { seen[NR][NC] = true; px[NR][NC] = c; py[NR][NC] = r; \
                                 qx[tail] = NC; qy[tail] = NR; tail++; } } while (0)
        for (dc = -1; dc <= 1; dc += 2)              /* walk */
            if (sim_standable(s, c + dc, r)) BOT_PUSH(c + dc, r);
        if ((is_ladder_at(s, c, r) || is_ladder_at(s, c, r - 1)) &&
            sim_standable(s, c, r - 1)) BOT_PUSH(c, r - 1);   /* climb up */
        if ((is_ladder_at(s, c, r) || is_ladder_at(s, c, r + 1)) &&
            sim_standable(s, c, r + 1)) BOT_PUSH(c, r + 1);   /* climb down */
        for (dc = -1; dc <= 1; dc += 2) {            /* drop off a ledge */
            int nc = c + dc;
            if (nc < 0 || nc >= ROOM_W) continue;
            if (sim_standable(s, nc, r)) continue;
            if (sim_tile_at(s, nc, r) == TILE_SOLID) continue;
            if (sim_tile_at(s, nc, r - 1) == TILE_SOLID) continue;  /* head clearance */
            for (dd = 1; dd <= maxDrop; dd++) {
                int nr = r + dd, below = nr + 1;
                if (nr >= ROOM_H) break;
                if (sim_standable(s, nc, nr)) {
                    /* only drop onto a WIDE, permanently-solid floor — never a
                     * crumbling tile or a 1-wide perch (both cause fall-throughs) */
                    bool solidFloor = (below >= ROOM_H) ||
                        (sim_tile_at(s, nc, below) == TILE_SOLID &&
                         s->tiles[below][nc] != TILE_CRUMBLE);
                    bool wide = sim_standable(s, nc - 1, nr) || sim_standable(s, nc + 1, nr);
                    if (solidFloor && wide) BOT_PUSH(nc, nr);
                    break;                        /* first foothold decides the drop */
                }
                if (sim_tile_at(s, nc, nr) == TILE_SOLID) break;
            }
        }
        #undef BOT_PUSH
    }
    return false;
}

/* Persistent per-target navigation state (drop-commit, ladder, facing).
 * ditherCount/lastHDir are used only by the demo to detect back-and-forth
 * flip-flopping (e.g. when a moving platform confuses the static pathfinder). */
typedef struct { int lastDir, commitCol, commitDir, sawAir; int ditherCount, lastHDir, hopping; } BotNav;
static void bot_nav_init(BotNav *n) { n->lastDir = 0; n->commitCol = -1; n->commitDir = 0;
                                      n->sawAir = 0; n->ditherCount = 0; n->lastHDir = 0; n->hopping = 0; }

/* One tick of the bot brain: the input to move toward cell (tc,tr). Pure — the
 * caller applies it via sim_tick, so this drives both the headless solver test
 * and the live demo mode. */
static Input bot_decide(GameState *s, int tc, int tr, BotNav *n, int maxDrop)
{
    Input in = {0};
    int cc, cr;
    bot_cell(s, &cc, &cr);

    /* Committing to a drop: walk off the ledge, then once airborne steer to the
     * target landing column so a narrow (1-column) platform isn't overshot. */
    if (n->commitCol >= 0) {
        if (!s->onGround && !s->onLadder) n->sawAir = 1;
        if (n->sawAir && (s->onGround || s->onLadder)) { n->commitCol = -1; n->sawAir = 0; }
        else {
            if (!n->sawAir) { if (n->commitDir > 0) in.right = true; else in.left = true; }
            else if (cc < n->commitCol) in.right = true;
            else if (cc > n->commitCol) in.left = true;
            return in;
        }
    }

    if (!s->onGround && !s->onLadder) {               /* airborne: keep drifting */
        if (n->lastDir > 0) in.right = true; else if (n->lastDir < 0) in.left = true;
    } else if (s->onLadder) {
        int nc, nr;
        if (!bfs_first_step(s, cc, cr, tc, tr, &nc, &nr, maxDrop)) in.down = true;
        else if (nr < cr) in.up = true;
        else if (nr > cr && nc == cc) in.down = true;
        else {                                        /* step off at the aligned row */
            float aligned = (float)((cr + 1) * TILE - HITBOX_H);
            if (s->py + 0.01f >= aligned) {
                if (nc > cc) { in.right = true; n->lastDir = 1; }
                else         { in.left  = true; n->lastDir = -1; }
            } else in.down = true;
        }
    } else {                                          /* grounded */
        int nc, nr;
        if (bfs_first_step(s, cc, cr, tc, tr, &nc, &nr, maxDrop)) {
            if (nr < cr) in.up = true;
            else if (nr > cr && nc == cc) in.down = true;
            else if (nr > cr && nc != cc) {           /* drop off a side ledge */
                n->commitCol = nc; n->commitDir = (nc > cc) ? 1 : -1; n->sawAir = 0;
                if (n->commitDir > 0) { in.right = true; n->lastDir = 1; }
                else                  { in.left  = true; n->lastDir = -1; }
            }
            else if (nc > cc) { in.right = true; n->lastDir = 1; }
            else if (nc < cc) { in.left = true;  n->lastDir = -1; }
        }
    }
    return in;
}

/* Drive the physics until the bot's cell reaches (tc,tr). */
static bool bot_navigate(GameState *s, int tc, int tr, int maxTicks)
{
    BotNav n; int step, d0 = s->depth;
    bot_nav_init(&n);
    for (step = 0; step < maxTicks; step++) {
        int cc, cr;
        Input in;
        if (s->depth != d0) return true;              /* fell into the exit = done */
        bot_cell(s, &cc, &cr);
        if (cc == tc && cr == tr) return true;
        in = bot_decide(s, tc, tr, &n, 4);
        /* grounded with no route found: give up (bfs failed) */
        if (s->onGround && !s->onLadder && n.commitCol < 0 &&
            !in.left && !in.right && !in.up && !in.down) return false;
        sim_tick(s, in);
        if (s->state == PS_DYING) return false;
    }
    return false;
}

/* NOTE: the live play bots have moved to bot1.c / bot2.c (fully independent
 * copies). What remains here (bot_cell/is_ladder_at/bfs_first_step/bot_decide/
 * bot_navigate/bot_clear_room) is the headless SOLVER used only to validate that
 * generated rooms are reachable (the --selftest clear-rate check). */

/* Collect every coal lump, then fall into the opened exit. */
static bool bot_clear_room(uint64_t seed, int depth)
{
    GameState s;
    int r, c, guard = 0, k, d0, side;
    memset(&s, 0, sizeof s);
    s.lives = 99;
    sim_gen_room(&s, seed, depth, sim_entry_col_for(seed, depth));
    s.enemyCount = 0;     /* geometry only — enemies & platforms are additive */
    s.platCount = 0;
    { int rr, cc2;         /* crumble is a timing hazard; treat as solid floor here */
      for (rr = 0; rr < ROOM_H; rr++)
          for (cc2 = 0; cc2 < ROOM_W; cc2++)
              if (s.tiles[rr][cc2] == TILE_CRUMBLE) s.tiles[rr][cc2] = TILE_SOLID; }
    if (g_botdebug) {
        int rr, cc2;
        printf("room seed=%llx depth=%d exitCol=%d spawnCol=%d\n",
               (unsigned long long)seed, depth, s.exitCol, s.spawnCol);
        for (rr = 0; rr < ROOM_H; rr++) {
            printf("  ");
            for (cc2 = 0; cc2 < ROOM_W; cc2++) {
                char ch = '.';
                switch (s.tiles[rr][cc2]) {
                case TILE_SOLID: ch = '#'; break; case TILE_LADDER: ch = 'H'; break;
                case TILE_ONEWAY: ch = '-'; break; default: break;
                }
                if (s.coal[rr][cc2]) ch = '*';
                printf("%c", ch);
            }
            printf("\n");
        }
    }

    while (s.coalGot < s.coalTotal) {
        int tc = -1, tr = -1;
        for (r = 0; r < ROOM_H && tc < 0; r++)
            for (c = 0; c < ROOM_W; c++)
                if (s.coal[r][c]) { tc = c; tr = r; break; }
        if (tc < 0) break;
        if (g_botdebug) { int cc, cr; bot_cell(&s, &cc, &cr);
            printf("  coal target (%d,%d); bot at (%d,%d) py=%.1f ground=%d coal %d/%d\n",
                   tc, tr, cc, cr, s.py, s.onGround, s.coalGot, s.coalTotal); }
        if (!bot_navigate(&s, tc, tr, 2500)) {
            if (g_botdebug) { int cc, cr; bot_cell(&s, &cc, &cr);
                printf("  NAVIGATE FAIL to (%d,%d); ended at (%d,%d) py=%.1f ground=%d ladder=%d\n",
                       tc, tr, cc, cr, s.py, s.onGround, s.onLadder); }
            return false;
        }
        if (++guard > 40) return false;
    }

    /* key rooms: the exit also needs the key, so fetch it before descending */
    if (s.keyRoom && !s.keyGot && s.keyCol >= 0)
        if (!bot_navigate(&s, s.keyCol, s.keyRow, 2500)) return false;

    /* The open exit is a 1-tile hole that splits the bedrock walkway in two.
     * Reach a bedrock cell on EITHER side of the shaft, then walk into it.
     * bot_navigate returns true if the bot descends en route, so falling in
     * while approaching also counts. */
    d0 = s.depth;
    for (side = 0; side < 2; side++) {
        int ap2 = (side == 0) ? s.exitCol - 1 : s.exitCol + 1;
        if (ap2 < 0 || ap2 >= ROOM_W) continue;
        if (bot_navigate(&s, ap2, BEDROCK_ROW - 1, 2500)) {
            if (s.depth > d0) return true;
            {   /* walk steadily into the 1-tile shaft until we drop */
                int into = (s.exitCol < ap2) ? -1 : 1;
                for (k = 0; k < 400 && s.depth == d0; k++) {
                    Input in = {0};
                    if (into < 0) in.left = true; else in.right = true;
                    sim_tick(&s, in);
                }
            }
        }
        if (s.depth > d0) return true;
    }
    if (g_botdebug) { int cc, cr; bot_cell(&s, &cc, &cr);
        printf("  EXIT FAIL; bot (%d,%d) exitCol=%d\n", cc, cr, s.exitCol); }
    return s.depth > d0;
}

/* ---------- headless smoke test (no window) ---------- */
static int fails = 0;
static void check(bool ok, const char *what)
{
    printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) fails++;
}
static void scores_selftest(void);   /* defined next to the score table below */

/* Build a controlled room for physics unit tests: a solid floor at
 * `floorRow` (full width) and the player standing at column `col`. */
static void stand_on_floor(GameState *s, int floorRow, int col)
{
    int c;
    memset(s, 0, sizeof *s);
    s->lives = 3; s->depth = 1;
    for (c = 0; c < ROOM_W; c++) s->tiles[floorRow][c] = TILE_SOLID;
    s->px = (float)(col * TILE + (TILE - HITBOX_W) / 2);
    s->py = (float)(floorRow * TILE - HITBOX_H);
    s->facing = 1;
    s->onGround = true;
}

static int selftest(void)
{
    GameState s;
    Input idle = {0}, right = {0}, jumpR = {0};
    int i;
    float groundY, apexY;

    right.right = true;
    jumpR.right = true; jumpR.jump = true;

    /* ---- physics unit tests on a controlled flat room ---- */
    stand_on_floor(&s, 10, 5);
    groundY = s.py;
    { float x0 = s.px;
      for (i = 0; i < 20; i++) sim_tick(&s, right);
      check(s.px > x0 + 28.0f && s.px < x0 + 31.0f, "walk: ~1.5 px/tick"); }

    stand_on_floor(&s, 10, 5);
    sim_tick(&s, jumpR);
    check(!s.onGround, "jump: leaves the ground");
    apexY = s.py;
    for (i = 0; i < 60 && !s.onGround; i++) {
        sim_tick(&s, idle);
        if (s.py < apexY) apexY = s.py;
    }
    check(s.onGround, "jump: lands again within 60 ticks");
    check(groundY - apexY > 20.0f && groundY - apexY < 26.0f, "jump: apex ~24 px");

    /* fall-death threshold: 3-tile drop survives, 8-tile drop kills.
     * Floor at row 10 stays intact; the player just starts airborne higher. */
    stand_on_floor(&s, 10, 5); s.onGround = false;
    s.py = (float)(7 * TILE - HITBOX_H);          /* land 3 tiles down */
    for (i = 0; i < 60 && s.state == PS_ALIVE && !s.onGround; i++) sim_tick(&s, idle);
    check(s.state == PS_ALIVE && s.onGround, "fall: 3-tile drop survives");

    stand_on_floor(&s, 10, 5); s.onGround = false;
    s.py = (float)(2 * TILE - HITBOX_H);          /* land 8 tiles down */
    for (i = 0; i < 60 && s.state == PS_ALIVE; i++) sim_tick(&s, idle);
    check(s.state == PS_DYING, "fall: 8-tile drop kills");

    /* ladder top: climbing up stops standing on top, then walks off — no fall.
     * Room: platform floor at row 3 (cols 3..7), ladder col 5 down to floor 9. */
    { Input up = {0}, rt = {0}; int r; float topY;
      up.up = true; rt.right = true;
      memset(&s, 0, sizeof s); s.lives = 3; s.depth = 1;
      for (r = 3; r <= 7; r++) s.tiles[3][r] = TILE_SOLID;   /* upper platform */
      for (r = 0; r < ROOM_W; r++) s.tiles[9][r] = TILE_SOLID; /* lower floor  */
      for (r = 3; r <= 8; r++) s.tiles[r][5] = TILE_LADDER;   /* the ladder    */
      s.px = (float)(5 * TILE + (TILE - HITBOX_W) / 2);
      s.py = (float)(9 * TILE - HITBOX_H);                    /* stand at base */
      s.onGround = true; s.facing = 1;
      for (i = 0; i < 200; i++) sim_tick(&s, up);             /* climb to top  */
      topY = (float)(3 * TILE - HITBOX_H);
      check(s.py == topY && s.onGround && !s.onLadder,
            "ladder: climbing up stops standing on top");
      for (i = 0; i < 6; i++) sim_tick(&s, idle);             /* let go: no fall */
      check(s.py == topY && s.onGround, "ladder: stays on top when idle");
      for (i = 0; i < 8; i++) sim_tick(&s, rt);               /* walk right off */
      check(s.px > (float)(5 * TILE) && s.py == topY,
            "ladder: can walk left/right off the top"); }

    /* ---- generation property tests: many seeds x depths ---- */
    { int depths[6] = {1, 2, 5, 10, 25, 50};
      int di, seedi, rooms = 0, fallbacks = 0;
      bool allSolvable = true, coalOk = true, alignOk = true, clearOk = true;
      bool budgetOk = true, spriteOk = true, spawnSafeOk = true, gateOk = true;
      for (seedi = 0; seedi < 200; seedi++) {
          uint64_t seed = 0x1000ULL + (uint64_t)seedi * 0x9E37ULL;
          for (di = 0; di < 6; di++) {
              int depth = depths[di];
              int entry = sim_entry_col_for(seed, depth);
              GameState g;
              int c;
              memset(&g, 0, sizeof g);
              sim_gen_room(&g, seed, depth, entry);
              rooms++;
              if (g.usedFallback) fallbacks++;
              if (!sim_room_solvable(&g)) allSolvable = false;
              if (g.coalTotal < 1 || g.coalTotal > 16) coalOk = false;
              /* the bedrock walkway (rows 9 head + 10 feet) must be clear of
               * solids so the lowest level is always fully traversable */
              for (c = 0; c < ROOM_W; c++)
                  if (g.tiles[BEDROCK_ROW - 2][c] == TILE_SOLID ||
                      g.tiles[BEDROCK_ROW - 1][c] == TILE_SOLID) clearOk = false;
              /* enemy invariants: budget, sprite limit, spawn safety, gating */
              if (g.enemyCount > MAX_ENEMIES) budgetOk = false;
              if (1 + g.enemyCount > 8) spriteOk = false;
              { int e;
                if (depth == 1 && g.enemyCount > 0) gateOk = false;  /* depth 1 is safe */
                for (e = 0; e < g.enemyCount; e++) {
                    int ecol = (int)((g.enemies[e].x + 6) / TILE);
                    int erow = (int)(g.enemies[e].y / TILE);
                    EnemyType t = g.enemies[e].type;
                    if (abs(ecol - g.spawnCol) < 3 && erow < 5) spawnSafeOk = false;
                    if (depth < 4  && t != EN_CRUSHER)  gateOk = false;
                    if (depth < 7  && t == EN_BAT)      gateOk = false;
                    if (depth < 10 && t == EN_SPIDER)   gateOk = false;
                    if (depth < 13 && t == EN_BOULDER)  gateOk = false;
                } }
              /* shaft alignment: this room's entry == prev room's exit */
              if (depth > 1) {
                  GameState prev;
                  memset(&prev, 0, sizeof prev);
                  sim_gen_room(&prev, seed, depth - 1,
                               sim_entry_col_for(seed, depth - 1));
                  if (prev.exitCol != g.entryCol) alignOk = false;
              }
          }
      }
      check(allSolvable, "gen: every room passes the solvability checker");
      check(coalOk,      "gen: coal quota within 1..16 per room");
      check(alignOk,     "gen: entry column matches previous room's exit");
      check(clearOk,     "gen: bedrock walkway clear (lowest level traversable)");
      check(budgetOk,    "gen: enemy count within MAX_ENEMIES");
      check(spriteOk,    "gen: player + enemies within 8-sprite budget");
      check(spawnSafeOk, "gen: no enemy inside the 3-tile spawn-safe zone");
      check(gateOk,      "gen: depth 1 safe; enemy types gated by depth (§5.4)");
      check(fallbacks * 100 <= rooms, "gen: fallback used in <=1% of rooms");
      printf("      (%d rooms generated, %d fallbacks)\n", rooms, fallbacks); }

    /* enemy contact kills the player */
    { GameState g;
      memset(&g, 0, sizeof g); g.lives = 3; g.state = PS_ALIVE;
      g.px = 80; g.py = 80; g.onGround = false;
      g.enemyCount = 1;
      g.enemies[0].type = EN_BAT;
      g.enemies[0].x = 80; g.enemies[0].y = 80;
      g.enemies[0].baseX = 80; g.enemies[0].baseY = 80; g.enemies[0].range = 0;
      sim_tick(&g, idle);
      check(g.state == PS_DYING, "enemy: contact kills the player"); }

    /* roaming enemies (level 15 = the menagerie, all types): in bounds,
     * deterministic, foreman climbs */
    { GameState a, b; long j; bool ok = true; int e, fi = -1;
      float fbase = 0, fmax = 0;
      memset(&a, 0, sizeof a); a.lives = 99;   /* survive the whole run */
      sim_gen_room(&a, 0x33, 15, sim_entry_col_for(0x33, 15));
      check(a.enemyCount == 5, "enemy: level 15 menagerie spawns all five types");
      for (e = 0; e < a.enemyCount; e++)
          if (a.enemies[e].type == EN_FOREMAN) { fi = e; fbase = a.enemies[e].y; }
      memcpy(&b, &a, sizeof b);
      for (j = 0; j < 4000; j++) {
          Input in = {0};
          in.right = (j / 40) % 2; in.left = !in.right; in.up = (j % 9) == 0;
          sim_tick(&a, in); sim_tick(&b, in);
          for (e = 0; e < a.enemyCount; e++) {
              if (a.enemies[e].type == EN_BOULDER && a.enemies[e].spawnDelay > 0) continue;  /* parked for its spawn delay */
              if (a.enemies[e].x < -TILE || a.enemies[e].x > SCREEN_W ||
                  a.enemies[e].y < -TILE || a.enemies[e].y > ROOM_H * TILE) ok = false;
          }
          if (fi >= 0) {
              float d = a.enemies[fi].y - fbase; if (d < 0) d = -d;
              if (d > fmax) fmax = d;
          }
      }
      check(ok, "enemy: roaming enemies stay inside the room");
      check(fi >= 0 && fmax >= TILE, "enemy: foreman climbs to another level");
      check(memcmp(&a, &b, sizeof a) == 0, "enemy: depth-15 run is deterministic"); }

    /* key rooms: exit stays shut until the key is held (as well as all coal) */
    { GameState g; int si, foundKey = 0, foundDark = 0;
      for (si = 0; si < 400 && !(foundKey && foundDark); si++) {
          uint64_t seed = 0x1000ULL + (uint64_t)si * 0x9E37ULL;
          if (!foundKey) {
              memset(&g, 0, sizeof g);
              sim_gen_room(&g, seed, 10, sim_entry_col_for(seed, 10));
              if (g.keyRoom && g.keyCol >= 0) {
                  foundKey = 1;
                  g.coalGot = g.coalTotal; memset(g.coal, 0, sizeof g.coal);
                  g.keyGot = false; sim_tick(&g, idle);
                  check(sim_tile_at(&g, g.exitCol, BEDROCK_ROW) != TILE_EXIT,
                        "key room: exit shut without the key");
                  g.keyGot = true; sim_tick(&g, idle);
                  check(sim_tile_at(&g, g.exitCol, BEDROCK_ROW) == TILE_EXIT,
                        "key room: exit opens once key held");
              }
          }
          if (!foundDark) {
              memset(&g, 0, sizeof g);
              sim_gen_room(&g, seed, 25, sim_entry_col_for(seed, 25));
              if (g.darkRoom) { foundDark = 1;
                  check(g.lampCol >= 0, "dark room: a lamp is placed"); }
          }
      }
      check(foundKey,  "gen: key rooms occur at depth 10");
      check(foundDark, "gen: dark rooms occur at depth 25"); }

    /* depth 1 is always a dark room with a lamp (the lamp mechanic on show
     * from the very first level; template fallback rooms stay simple) */
    { GameState g; int si; bool alwaysDark = true;
      for (si = 0; si < 30; si++) {
          uint64_t seed = 0x1000ULL + (uint64_t)si * 0x9E37ULL;
          memset(&g, 0, sizeof g);
          sim_gen_room(&g, seed, 1, sim_entry_col_for(seed, 1));
          if (g.usedFallback) continue;
          if (!g.darkRoom || g.lampCol < 0) alwaysDark = false;
      }
      check(alwaysDark, "gen: depth 1 is a dark room with a lamp"); }

    /* both built-in bots fetch the lamp first, so a dark room lights up
     * (keeps the attract demo watchable now that depth 1 is dark) */
    { GameState g; Bot1Nav n1; Bot2Nav n2; int i; bool ok1 = true, ok2 = true;
      sim_init_seed(&g, 0x5EEDULL); g.lives = 99; bot1_nav_init(&n1);
      if (g.darkRoom && g.lampCol >= 0) {
          for (i = 0; i < 3000 && !g.lampGot; i++) sim_tick(&g, bot1_input(&g, &n1));
          ok1 = g.lampGot;
      }
      sim_init_seed(&g, 0x5EEDULL); g.lives = 99; bot2_nav_init(&n2);
      if (g.darkRoom && g.lampCol >= 0) {
          for (i = 0; i < 3000 && !g.lampGot; i++) sim_tick(&g, bot2_input(&g, &n2));
          ok2 = g.lampGot;
      }
      check(ok1, "bot1: fetches the lamp in a dark room");
      check(ok2, "bot2: fetches the lamp in a dark room"); }

    /* moving platforms: a rider is carried horizontally and vertically */
    { GameState g; int i; float x0, y0;
      memset(&g, 0, sizeof g); g.lives = 3; g.state = PS_ALIVE; g.ridingPlat = -1;
      g.platCount = 1;
      g.plats[0].x = 100; g.plats[0].y = 100; g.plats[0].baseX = 100;
      g.plats[0].baseY = 100; g.plats[0].range = 40; g.plats[0].axis = 0; g.plats[0].dir = 1;
      g.px = 108; g.py = 100 - HITBOX_H; g.onGround = true; g.ridingPlat = 0; g.facing = 1;
      x0 = g.px;
      for (i = 0; i < 20; i++) sim_tick(&g, idle);
      check(g.px > x0 + 5.0f && g.onGround, "platform: rider carried horizontally");

      memset(&g, 0, sizeof g); g.lives = 3; g.state = PS_ALIVE; g.ridingPlat = -1;
      g.platCount = 1;
      g.plats[0].x = 100; g.plats[0].y = 100; g.plats[0].baseX = 100;
      g.plats[0].baseY = 60; g.plats[0].range = 40; g.plats[0].axis = 1; g.plats[0].dir = -1;
      g.px = 108; g.py = 100 - HITBOX_H; g.onGround = true; g.ridingPlat = 0; g.facing = 1;
      y0 = g.py;
      for (i = 0; i < 20; i++) sim_tick(&g, idle);
      check(g.py < y0 - 5.0f && g.onGround, "platform: rider carried vertically (up)"); }

    /* landing on a platform clears fall tracking, so a later step-off isn't
     * counted as one long lethal fall (regression: dropping off always killed) */
    { GameState g; int i;
      memset(&g, 0, sizeof g); g.lives = 3; g.state = PS_ALIVE; g.ridingPlat = -1;
      g.platCount = 1;
      g.plats[0].x = 80; g.plats[0].y = 64; g.plats[0].baseX = 80; g.plats[0].baseY = 64;
      g.plats[0].range = 0; g.plats[0].axis = 0; g.plats[0].dir = 1;
      g.px = 88; g.py = 0; g.onGround = false; g.vy = 0; g.facing = 1;
      for (i = 0; i < 60 && !g.onGround; i++) sim_tick(&g, idle);   /* fall onto it */
      check(g.onGround && g.state == PS_ALIVE, "platform: player lands on a platform");
      check(!g.fallTracking, "platform: landing on a platform clears fall tracking"); }

    /* crumbling floor: holds briefly, then collapses and drops the player */
    { GameState g; int i, fr = 7, fc = 5, c;
      memset(&g, 0, sizeof g); g.lives = 3; g.state = PS_ALIVE; g.ridingPlat = -1;
      for (c = 0; c < ROOM_W; c++) { g.tiles[fr][c] = TILE_SOLID;      /* weak floor row */
                                     g.tiles[fr + 3][c] = TILE_SOLID; } /* catch floor below */
      g.tiles[fr][fc] = TILE_CRUMBLE;
      g.px = (float)(fc * TILE + 3); g.py = (float)(fr * TILE - HITBOX_H);
      g.onGround = true; g.facing = 1;
      for (i = 0; i < 12; i++) sim_tick(&g, idle);
      check(g.onGround && sim_tile_at(&g, fc, fr) != TILE_EMPTY,
            "crumble: holds briefly when stood on");
      for (i = 0; i < 80; i++) sim_tick(&g, idle);
      check(sim_tile_at(&g, fc, fr) == TILE_EMPTY, "crumble: collapses after being stood on");
      check(g.state == PS_ALIVE && (int)g.py == (fr + 3) * TILE - HITBOX_H,
            "crumble: player drops through and lands below");
      /* respawn restores collapsed weak floors */
      g.state = PS_DYING; g.dyingTimer = 1; g.lives = 2;
      sim_tick(&g, idle);
      check(g.crumble[fr][fc] == 0 && sim_tile_at(&g, fc, fr) != TILE_EMPTY,
            "crumble: respawn restores collapsed floors"); }

    /* a ground sprite caught on a collapsing weak floor falls; a >4-tile drop
     * kills it and resets it to its level-start position (bats are unaffected). */
    { GameState g; int i; bool fell = false, landed = false; int fx = -1;
      sim_init_seed(&g, 1);
      memset(g.tiles, 0, sizeof g.tiles);       /* TILE_EMPTY == 0: clean room */
      memset(g.coal, 0, sizeof g.coal);
      memset(g.crumble, 0, sizeof g.crumble);
      for (i = 0; i < ROOM_W; i++) g.tiles[12][i] = TILE_SOLID;   /* one deep floor */
      g.tiles[4][5] = TILE_CRUMBLE; g.crumble[4][5] = 1;          /* a weak ledge, about to go */
      g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_FOREMAN; e->dir = 1; e->vdir = 1;
        e->baseX = (float)(2 * TILE); e->baseY = (float)(12 * TILE - HITBOX_H);
        e->x = (float)(5 * TILE); e->y = (float)(4 * TILE - HITBOX_H); }  /* perched on the ledge */
      g.enemyStart[0] = g.enemies[0];
      g.enemyStart[0].x = g.enemyStart[0].baseX;   /* spawn is the deep floor, col 2 */
      g.enemyStart[0].y = g.enemyStart[0].baseY;
      g.px = (float)(18 * TILE); g.py = (float)(12 * TILE - HITBOX_H);   /* player, well clear */
      g.vx = g.vy = 0; g.onGround = true; g.onLadder = false; g.ridingPlat = -1;
      g.state = PS_ALIVE; g.lives = 9;
      for (i = 0; i < 300; i++) {
          bool was = g.enemies[0].falling;
          sim_tick(&g, idle);
          if (g.enemies[0].falling) fell = true;
          if (was && !g.enemies[0].falling) { landed = true; fx = (int)g.enemies[0].x; break; }
      }
      check(fell, "crumble: a foreman on a collapsing tile falls");
      check(landed && fx == 2 * TILE,
            "crumble: a >4-tile fall kills the sprite and resets it to start"); }

    /* a crusher (or spider) above a collapsing floor extends its reach down to
     * the next platform, since the floor that limited it is gone. */
    { GameState g; int i, r0;
      sim_init_seed(&g, 2);
      memset(g.tiles, 0, sizeof g.tiles);
      memset(g.coal, 0, sizeof g.coal);
      memset(g.crumble, 0, sizeof g.crumble);
      for (i = 0; i < ROOM_W; i++) { g.tiles[2][i] = TILE_SOLID; g.tiles[12][i] = TILE_SOLID; }
      g.tiles[6][5] = TILE_CRUMBLE; g.crumble[6][5] = 1;   /* the floor it slams toward */
      g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_CRUSHER; e->dir = 1; e->vdir = 1; e->period = 120;
        e->baseX = (float)(5 * TILE); e->baseY = (float)(3 * TILE);   /* below the row-2 ceiling */
        e->range = 2 * TILE;                                         /* reach to just above row 6 */
        e->x = e->baseX; e->y = e->baseY; }
      g.enemyStart[0] = g.enemies[0];
      g.px = (float)(18 * TILE); g.py = (float)(12 * TILE - HITBOX_H);
      g.onGround = true; g.state = PS_ALIVE; g.lives = 9;
      r0 = g.enemies[0].range;
      sim_tick(&g, idle);   /* collapses row 6 -> reach extends to the row-12 floor */
      check(g.enemies[0].range > r0 && g.enemies[0].range == 8 * TILE,
            "crumble: a crusher extends its reach to the next platform"); }

    /* no two crushers ever occupy the same column (one-per-column guard) */
    { int seed, dep; bool ok = true;
      for (dep = 1; dep <= 50 && ok; dep += 3)
          for (seed = 0; seed < 40 && ok; seed++) {
              GameState g; int a, b;
              uint64_t sd = (uint64_t)seed * 2654435761u + (uint64_t)dep;
              memset(&g, 0, sizeof g); g.lives = 3;
              sim_gen_room(&g, sd, dep, sim_entry_col_for(sd, dep));
              for (a = 0; a < g.enemyCount && ok; a++)
                  for (b = a + 1; b < g.enemyCount && ok; b++)
                      if (g.enemies[a].type == EN_CRUSHER && g.enemies[b].type == EN_CRUSHER &&
                          (int)(g.enemies[a].baseX / TILE) == (int)(g.enemies[b].baseX / TILE)) ok = false;
          }
      check(ok, "gen: no two crushers share a column"); }

    /* a slamming crusher flattens a foreman caught under its head, knocking it
     * back to its spawn position. */
    { GameState g; int i;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (i = 0; i < ROOM_W; i++) g.tiles[9][i] = TILE_SOLID;
      g.px = (float)(18 * TILE); g.py = (float)(9 * TILE - HITBOX_H); g.onGround = true;
      g.enemyCount = 2;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
        e->baseX = (float)(5 * TILE); e->baseY = (float)(2 * TILE); e->range = 6 * TILE;
        e->period = 120; e->timer = 86;                 /* mid-slam, fully down (phase 2) */
        e->x = e->baseX; e->y = e->baseY; }
      { Enemy *e = &g.enemies[1]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN; e->dir = 1;
        e->x = (float)(5 * TILE); e->y = (float)(9 * TILE - HITBOX_H); }   /* under the crusher */
      g.enemyStart[0] = g.enemies[0];
      g.enemyStart[1] = g.enemies[1]; g.enemyStart[1].x = (float)(15 * TILE);  /* its spawn, elsewhere */
      sim_tick(&g, idle);
      check((int)g.enemies[1].x == 15 * TILE,
            "crusher: a slam flattens a foreman under it (reset to spawn)"); }

    /* foremen climb ladders 5% slower than the player (1.0 -> 0.95 px/tick) */
    { GameState g; float y0, dy; int r, c;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (r = 2; r <= 10; r++) g.tiles[r][5] = TILE_LADDER;
      for (c = 0; c < ROOM_W; c++) g.tiles[11][c] = TILE_SOLID;
      g.px = (float)(18 * TILE); g.py = (float)(11 * TILE - HITBOX_H); g.onGround = true;
      g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN;
        e->mode = 1; e->vdir = -1; e->x = (float)(5 * TILE); e->y = (float)(8 * TILE); }
      y0 = g.enemies[0].y;
      sim_tick(&g, idle);
      dy = y0 - g.enemies[0].y;
      check(dy > 0.9f && dy < 1.0f, "foreman: climbs ladders ~5% slower than the player"); }

    /* bots are ladder-aware: won't climb up into an enemy on the ladder above */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int r;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (r = 4; r <= 9; r++) g.tiles[r][5] = TILE_LADDER;
      g.tiles[10][5] = TILE_SOLID;
      g.coal[5][5] = true; g.coalGot = 0; g.coalTotal = 1;   /* coal up the ladder -> wants up */
      g.px = (float)(5 * TILE); g.py = (float)(10 * TILE - HITBOX_H);
      g.onGround = true; g.facing = 1; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN;
        e->mode = 1; e->vdir = 1; e->x = (float)(5 * TILE); e->y = g.py - (float)TILE; }
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check(!o1.up, "bot1: won't climb into an enemy on the ladder above");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(!o2.up, "bot2: won't climb into an enemy on the ladder above"); }

    /* standing at a ladder top with a foreman climbing up it, a bot steps off to a
     * side (evades) rather than waiting there to be hit. */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int c, r;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 3; c <= 7; c++) { g.tiles[5][c] = TILE_SOLID; g.tiles[10][c] = TILE_SOLID; }
      for (r = 5; r <= 9; r++) g.tiles[r][5] = TILE_LADDER;   /* ladder carved down from the top */
      g.px = (float)(5 * TILE); g.py = (float)(5 * TILE - HITBOX_H);   /* at the ladder top */
      g.onGround = true; g.facing = 1; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN;
        e->mode = 1; e->vdir = -1; e->x = (float)(5 * TILE); e->y = (float)(7 * TILE); }  /* climbing up */
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check((o1.left || o1.right) && !o1.down, "bot1: evades off a ladder as an enemy climbs toward it");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check((o2.left || o2.right) && !o2.down, "bot2: evades off a ladder as an enemy climbs toward it"); }

    /* a crusher can only reach its own column: beside a SLAMMING one, a bot
     * neither flees along the floor nor steps into its column — it simply
     * ignores it (which also kills the old vibrate-on-its-threat-boundary). */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int c, r;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 3; c <= 7; c++) g.tiles[10][c] = TILE_SOLID;   /* floor */
      for (r = 5; r <= 9; r++) g.tiles[r][5] = TILE_LADDER;   /* ladder up at col 5 */
      g.px = (float)(5 * TILE); g.py = (float)(10 * TILE - HITBOX_H);   /* at the ladder base */
      g.onGround = true; g.facing = 1; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
        e->baseX = (float)(6 * TILE); e->baseY = (float)(5 * TILE); e->range = 4 * TILE;
        e->period = 120; e->phase = 2;                                    /* mid-slam */
        e->x = e->baseX; e->y = e->baseY + (float)(4 * TILE); }           /* beside the ladder */
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check(!o1.right && !o1.jump, "bot1: ignores a slamming crusher beside it (column threat only)");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(!o2.right && !o2.jump, "bot2: ignores a slamming crusher beside it (column threat only)"); }

    /* a ladder is a safe haven from a spider too (same rule as the crusher) */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int c, r;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 3; c <= 7; c++) g.tiles[10][c] = TILE_SOLID;
      for (r = 5; r <= 9; r++) g.tiles[r][5] = TILE_LADDER;
      g.px = (float)(5 * TILE); g.py = (float)(10 * TILE - HITBOX_H);   /* at the ladder base */
      g.onGround = true; g.facing = 1; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_SPIDER;
        e->x = (float)(6 * TILE); e->y = (float)(9 * TILE); }   /* bobbing beside the ladder */
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check(o1.up && !o1.left && !o1.right, "bot1: takes the ladder haven beside a spider, doesn't flee");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(o2.up && !o2.left && !o2.right, "bot2: takes the ladder haven beside a spider, doesn't flee"); }

    /* bots never hop onto a spider (or crusher) — nothing can be jumped over it */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int c;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 0; c < ROOM_W; c++) if (c != 9) g.tiles[9][c] = TILE_SOLID;   /* 1-tile gap at col 9 */
      g.coal[8][11] = true; g.coalGot = 0; g.coalTotal = 1;                  /* coal past the gap */
      g.px = (float)(9 * TILE - HITBOX_W); g.py = (float)(9 * TILE - HITBOX_H);  /* at col 8's lip */
      g.onGround = true; g.facing = 1; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_SPIDER;
        e->x = (float)(10 * TILE); e->y = (float)(8 * TILE); }   /* sitting at the gap-jump landing */
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check(!o1.jump && !o1.right, "bot1: won't hop into a spider at the gap landing");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(!o2.jump && !o2.right, "bot2: won't hop into a spider at the gap landing"); }

    /* Drop off a platform edge that hugs the screen border: the coal sits in the
     * border column (col 0) below a platform whose left edge is at col 1, so the
     * only route is to walk off the lip into col 0 and fall. The bot used to wedge
     * on that lip — its center enters col 0 while col 1 still supports it, and the
     * cliff guard cancelled the walk-off and cleared the drop-commit, oscillating
     * forever. It must now complete the drop and collect the coal. */
    { GameState g1, g2; Bot1Nav n1; Bot2Nav n2; int i; bool got1, got2;
      memset(&g1, 0, sizeof g1); g1.state = PS_ALIVE; g1.lives = 9; g1.ridingPlat = -1;
      for (i = 1; i <= 6; i++) g1.tiles[2][i] = TILE_SOLID;   /* top platform, left edge at col 1 */
      g1.tiles[6][0] = TILE_SOLID; g1.tiles[6][1] = TILE_SOLID;  /* landing floor under col 0/1 */
      g1.coal[5][0] = true; g1.coalTotal = 1; g1.exitCol = 10;
      g1.px = (float)(4 * TILE); g1.py = (float)(2 * TILE - HITBOX_H); g1.onGround = true; g1.facing = -1;
      g2 = g1;
      bot1_nav_init(&n1);
      for (i = 0; i < 260 && g1.state == PS_ALIVE && g1.coalGot == 0; i++) sim_tick(&g1, bot1_input(&g1, &n1));
      got1 = (g1.coalGot == 1);
      bot2_nav_init(&n2);
      for (i = 0; i < 260 && g2.state == PS_ALIVE && g2.coalGot == 0; i++) sim_tick(&g2, bot2_input(&g2, &n2));
      got2 = (g2.coalGot == 1);
      check(got1, "bot1: drops off a border-hugging platform edge to reach the coal");
      check(got2, "bot2: drops off a border-hugging platform edge to reach the coal"); }

    /* the two built-in bots have distinct personalities (STEADY vs RUSHER):
     * on the same room, same seed, their paths visibly diverge */
    { GameState a, b; Bot1Nav n1; Bot2Nav n2; long j; bool differed = false;
      sim_init_seed(&a, 0x5EEDULL); a.lives = 99; bot1_nav_init(&n1);
      sim_init_seed(&b, 0x5EEDULL); b.lives = 99; bot2_nav_init(&n2);
      for (j = 0; j < 2000 && !differed; j++) {
          sim_tick(&a, bot1_input(&a, &n1));
          sim_tick(&b, bot2_input(&b, &n2));
          if (a.px != b.px || a.py != b.py) differed = true;
      }
      check(differed, "bots: STEADY and RUSHER personalities actually diverge"); }

    /* a foreman standing on a horizontal moving platform is carried along by it,
     * and never drops its coal while riding (no solid tile under it). */
    { GameState g; int i, cr, cc; float x0; bool carried, dropped = false;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      g.tiles[9][2] = TILE_SOLID; g.tiles[9][3] = TILE_SOLID;   /* a small fixed platform */
      g.tiles[9][18] = TILE_SOLID;                              /* floor for the (idle) player */
      g.px = (float)(18 * TILE); g.py = (float)(9 * TILE - HITBOX_H); g.onGround = true;
      /* a horizontal platform floating over the gap to the right of the fixed one */
      g.platCount = 1;
      g.plats[0].baseX = (float)(4 * TILE); g.plats[0].baseY = (float)(9 * TILE);
      g.plats[0].x = g.plats[0].baseX; g.plats[0].y = g.plats[0].baseY;
      g.plats[0].range = 6 * TILE; g.plats[0].axis = 0; g.plats[0].dir = 1;
      /* a foreman standing on the platform, carrying a coal lump */
      g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN; e->dir = 1;
        e->x = (float)(4 * TILE); e->y = (float)(9 * TILE - HITBOX_H); e->carry = 30; }
      g.enemyStart[0] = g.enemies[0];
      x0 = g.enemies[0].x;
      for (i = 0; i < 40; i++) sim_tick(&g, idle);
      carried = (g.enemies[0].x != x0);                /* the platform moved it */
      for (cr = 0; cr < ROOM_H; cr++) for (cc = 0; cc < ROOM_W; cc++)
          if (g.coal[cr][cc]) dropped = true;
      check(carried, "platform: a foreman is carried by a moving platform");
      check(!dropped, "platform: a foreman drops no coal while riding a platform"); }

    /* a foreman never picks up the LAST lump on the ground — the quota needs
     * it, and a carried lump reads as "no coal left" to a bot's sensors while
     * the exit stays shut. With two lumps down, grabbing one is fine. */
    { GameState g; int i, c; bool lastKept, oneTaken = false;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 2; c <= 12; c++) g.tiles[9][c] = TILE_SOLID;      /* a walkway */
      g.tiles[9][18] = TILE_SOLID;                               /* perch for the (idle) player */
      g.px = (float)(18 * TILE); g.py = (float)(9 * TILE - HITBOX_H); g.onGround = true;
      g.coal[8][7] = true; g.coalTotal = 1;                      /* ONE lump, on its path */
      g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN; e->dir = 1;
        e->x = (float)(3 * TILE); e->y = (float)(9 * TILE - HITBOX_H); e->range = 8 * TILE; }
      g.enemyStart[0] = g.enemies[0];
      for (i = 0; i < 600; i++) sim_tick(&g, idle);              /* patrols across it repeatedly */
      lastKept = g.coal[8][7] && g.enemies[0].carry == 0;
      g.coal[8][5] = true; g.coalTotal = 2;                      /* now a second lump */
      for (i = 0; i < 600 && !oneTaken; i++) { sim_tick(&g, idle);
          if (g.enemies[0].carry > 0) oneTaken = true; }
      check(lastKept, "foreman: never grabs the last lump on the ground");
      check(oneTaken, "foreman: still hauls coal when more than one lump is down"); }

    /* sim_sync_room lands a run on the identical room a player reaches by
     * descending there — how the bot is pulled to the player's room. */
    { GameState a, b; uint64_t sd = 0xABCDEFULL;
      sim_init_seed(&a, sd);
      sim_sync_room(&a, 4);
      memset(&b, 0, sizeof b);
      sim_gen_room(&b, sd, 4, sim_entry_col_for(sd, 4));
      check(a.depth == 4 && memcmp(a.tiles, b.tiles, sizeof a.tiles) == 0,
            "sync: advancing to a depth lands on that depth's room"); }

    /* out of air suffocates the player (the human-play oxygen consequence) */
    { GameState g; sim_init_seed(&g, 1); g.state = PS_ALIVE;
      sim_kill_player(&g);
      check(g.state == PS_DYING, "oxygen: running out kills the player"); }

    /* death memory: the cell where the player died is recorded (deaths reset
     * enemies, so bots that know the spot can respect it), survives the
     * respawn, and is wiped by the next room. */
    { GameState g; int i, dc, dr; bool rec, kept, wiped;
      sim_init_seed(&g, 1); g.lives = 3;
      check(g.deaths == 0 && g.deathCol < 0, "death: a fresh room holds no grudges");
      g.px = (float)(7 * TILE); g.py = (float)(5 * TILE - HITBOX_H);   /* die at (7,4) */
      dc = (int)((g.px + HITBOX_W * 0.5f) / TILE);
      dr = (int)((g.py + HITBOX_H - 1) / TILE);
      sim_kill_player(&g);
      rec = (g.deaths == 1 && g.deathCol == dc && g.deathRow == dr);
      for (i = 0; i < DYING_TICKS + 5; i++) sim_tick(&g, idle);       /* through the respawn */
      kept = (g.deaths == 1 && g.deathCol == dc);
      sim_sync_room(&g, 2);                                            /* next room */
      wiped = (g.deaths == 0 && g.deathCol < 0);
      check(rec,   "death: the killing cell is recorded at the moment of death");
      check(kept,  "death: the memory survives the respawn");
      check(wiped, "death: the next room clears it"); }

    /* mole: a walk step settles CENTRED in the next cell, so a bot flipping
     * direction paces in honest full tiles instead of vibrating in 2px
     * twitches on the boundary (the bug that shook every naive AI bot). */
    { char e2[160];
      MoleVM *v = mole_load("loop:\n walk right\n walk left\nloop_end\n", 1, e2, sizeof e2);
      check(v != NULL, "mole: pace probe compiles");
      if (v) {
          GameState g; int i; float minx, maxx;
          memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
          for (i = 0; i < ROOM_W; i++) g.tiles[10][i] = TILE_SOLID;
          g.px = (float)(5 * TILE + 3); g.py = (float)(10 * TILE - HITBOX_H);
          g.onGround = true;
          minx = maxx = g.px;
          mole_reset(v, 1);
          for (i = 0; i < 200; i++) {
              sim_tick(&g, mole_input(v, &g));
              if (g.px < minx) minx = g.px;
              if (g.px > maxx) maxx = g.px;
          }
          check(maxx - minx >= 12.0f, "mole: walk right/left paces full tiles, not 2px twitches");
          mole_free(v);
      } }

    /* mole: walk with a direction that is neither left nor right (side_of of
     * a zero delta gives `any`) stands still — silently picking a side made
     * bots vibrate at their own target column forever. */
    { char e2[160];
      MoleVM *v = mole_load("loop:\n walk side_of(0)\nloop_end\n", 1, e2, sizeof e2);
      check(v != NULL, "mole: walk-any probe compiles");
      if (v) {
          GameState g; int i; float x0;
          memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
          for (i = 0; i < ROOM_W; i++) g.tiles[10][i] = TILE_SOLID;
          g.px = (float)(5 * TILE + 3); g.py = (float)(10 * TILE - HITBOX_H);
          g.onGround = true; x0 = g.px;
          mole_reset(v, 1);
          for (i = 0; i < 100; i++) sim_tick(&g, mole_input(v, &g));
          check(g.px == x0, "mole: walk any/none stands still instead of drifting");
          mole_free(v);
      } }

    /* mole: go_to(exit) can climb UP a ladder when the only route to the
     * bedrock goes up first — it used to force `down` whenever on a ladder,
     * so the bot attached going up, got yanked back down, and bobbed at the
     * ladder foot forever. */
    { char e2[160];
      MoleVM *v = mole_load("loop:\n go_to(exit)\nloop_end\n", 1, e2, sizeof e2);
      check(v != NULL, "mole: exit-route probe compiles");
      if (v) {
          GameState g; int i, c, r, cr2 = 0;
          memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
          for (c = 0; c < ROOM_W; c++) g.tiles[BEDROCK_ROW][c] = TILE_SOLID;  /* bedrock */
          for (c = 4; c <= 6; c++)   g.tiles[10][c] = TILE_SOLID;  /* island (too high to drop off) */
          for (r = 7; r <= 9; r++)   g.tiles[r][5]  = TILE_LADDER; /* the ONLY way off: UP */
          for (c = 6; c <= 14; c++)  g.tiles[7][c]  = TILE_SOLID;  /* upper platform */
          for (c = 15; c <= 17; c++) g.tiles[10][c] = TILE_SOLID;  /* stairs of short drops... */
          for (c = 18; c <= 19; c++) g.tiles[13][c] = TILE_SOLID;  /* ...down to the walkway */
          g.coalTotal = 0; g.exitCol = 10;
          g.px = (float)(5 * TILE + 3); g.py = (float)(10 * TILE - HITBOX_H);
          g.onGround = true;
          mole_reset(v, 1);
          for (i = 0; i < 500; i++) {
              sim_tick(&g, mole_input(v, &g));
              cr2 = (int)((g.py + HITBOX_H - 1) / TILE);
              if (cr2 >= 14) break;                                /* reached the walkway */
          }
          check(cr2 >= 14, "mole: go_to(exit) climbs an up-ladder to reach the bedrock");
          mole_free(v);
      } }

    /* mole: safe_ticks(i) exposes a crusher's guaranteed-raised time (parity
     * with the built-in bots' passage timing); 0 once it's armed */
    { char e2[160];
      MoleVM *v = mole_load(
          "loop:\n"
          " if safe_ticks(0) > 100 then walk right else walk left\n"
          "loop_end\n", 1, e2, sizeof e2);
      check(v != NULL, "mole: safe_ticks probe compiles");
      if (v) {
          GameState g; Input in;
          memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
          g.px = 83; g.py = 66; g.onGround = true;
          g.enemyCount = 1;
          { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
            e->baseX = (float)(10 * TILE); e->baseY = (float)(3 * TILE);
            e->x = e->baseX; e->y = e->baseY; e->period = 6000; e->phase = 0; }
          mole_reset(v, 1); in = mole_input(v, &g);
          check(in.right && !in.left, "mole: safe_ticks reads a long idle");
          g.enemies[0].phase = 2;                       /* slamming: no guarantee */
          mole_reset(v, 1); in = mole_input(v, &g);
          check(in.left && !in.right, "mole: safe_ticks is 0 once the crusher is armed");
          mole_free(v);
      } }

    /* mole: hostile numbers — a script reaches infinity in a few ticks
     * (x = x * x) and can conjure NaN (inf * 0); every double->int
     * conversion must clamp, never hit C's undefined behaviour. The test is
     * simply surviving: feed inf/NaN into every sensor family and keep
     * ticking. */
    { char e2[160];
      MoleVM *v = mole_load(
          "# author: fuzz\n"
          "x = 9\n"
          "loop:\n"
          " x = x * x\n"
          " n = x * 0\n"
          " say x, n\n"
          " if tile(x, n) == WALL then idle\n"
          " if is_solid(n, x) then idle\n"
          " e = enemy(x)\n"
          " c = cell(x, n)\n"
          " r = random(x)\n"
          " wait limit(n, 1, 2)\n"
          "loop_end\n", 1, e2, sizeof e2);
      check(v != NULL, "mole: hostile-numbers fuzz compiles");
      if (v) {
          GameState g; int i;
          memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
          for (i = 0; i < ROOM_W; i++) g.tiles[10][i] = TILE_SOLID;
          g.px = (float)(5 * TILE); g.py = (float)(10 * TILE - HITBOX_H);
          g.onGround = true;
          mole_reset(v, 1);
          for (i = 0; i < 100; i++) sim_tick(&g, mole_input(v, &g));
          check(g.state == PS_ALIVE,
                "mole: infinite/NaN script values clamp instead of undefined behaviour"); }
      mole_free(v); }

    /* mole: the death/deaths sensors expose the spot, or none before any death */
    { char e2[160];
      MoleVM *v = mole_load(
          "loop:\n"
          " if deaths == 1 and death != none and death.col == 7 and death.row == 4"
          " then walk right else walk left\n"
          "loop_end\n", 1, e2, sizeof e2);
      check(v != NULL, "mole: death sensor probe compiles");
      if (v) {
          GameState g; Input in;
          memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
          g.px = 83; g.py = 66; g.onGround = true;
          g.deathCol = -1; g.deathRow = -1; g.deaths = 0;
          mole_reset(v, 1); in = mole_input(v, &g);
          check(in.left && !in.right, "mole: death reads none before any death");
          g.deathCol = 7; g.deathRow = 4; g.deaths = 1;
          mole_reset(v, 1); in = mole_input(v, &g);
          check(in.right && !in.left, "mole: death/deaths expose the killing cell");
          mole_free(v);
      } }

    /* Cyclic boulder: on the bottom level it rolls off a side edge and wraps (to
     * the other ledge, dropping in from the top); on an upper level it bounces off
     * the edge instead; it drops into a one-tile gap; and it stays off-screen for
     * a spawn delay before dropping in. The player sits on a small island the
     * top-dropping boulder never lands on. */
    { GameState g; int i, c; bool wrapped = false, bounced = false, dropped = false;
      sim_init_seed(&g, 3);
      memset(g.tiles, 0, sizeof g.tiles);
      memset(g.coal, 0, sizeof g.coal);
      memset(g.crumble, 0, sizeof g.crumble);
      for (i = 0; i < ROOM_W; i++) { g.tiles[6][i] = TILE_SOLID; g.tiles[10][i] = TILE_SOLID;
                                     g.tiles[BEDROCK_ROW][i] = TILE_SOLID; }
      for (c = 8; c <= 11; c++) g.tiles[2][c] = TILE_SOLID;        /* player island */
      g.mainLedges = 3; g.fixCount = 4;
      g.fixRow[0] = 2;  g.fixLo[0] = 0; g.fixHi[0] = ROOM_W - 1;
      g.fixRow[1] = 6;  g.fixLo[1] = 0; g.fixHi[1] = ROOM_W - 1;
      g.fixRow[2] = 10; g.fixLo[2] = 0; g.fixHi[2] = ROOM_W - 1;
      g.fixRow[3] = BEDROCK_ROW; g.fixLo[3] = 0; g.fixHi[3] = ROOM_W - 1;
      g.px = (float)(9 * TILE); g.py = (float)(2 * TILE - HITBOX_H);   /* player on the island */
      g.onGround = true; g.state = PS_ALIVE; g.lives = 9;
      g.enemyCount = 1;
      /* (a) bottom level -> rolls off-screen and wraps to the other ledge */
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_BOULDER; e->mode = 1; e->dir = 1;
        e->x = (float)((ROOM_W - 2) * TILE); e->y = (float)((BEDROCK_ROW - 1) * TILE); }
      g.enemyStart[0] = g.enemies[0];
      for (i = 0; i < 120 && !wrapped; i++) { sim_tick(&g, idle); if (g.enemies[0].mode == 2) wrapped = true; }
      check(wrapped, "boulder: on the bottom level it rolls off-screen and wraps to the other ledge");
      /* (b) upper level -> bounces off the edge (no wrap) */
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_BOULDER; e->mode = 1; e->dir = 1;
        e->x = (float)((ROOM_W - 2) * TILE); e->y = (float)((6 - 1) * TILE); }
      g.enemyStart[0] = g.enemies[0];
      for (i = 0; i < 60 && !bounced; i++) { sim_tick(&g, idle); if (g.enemies[0].dir == -1) bounced = true; }
      check(bounced && g.enemies[0].mode == 1 && (int)g.enemies[0].y == (6 - 1) * TILE,
            "boulder: on an upper level it bounces off the screen edge");
      /* (c) drops into a one-tile gap instead of rolling over it */
      g.tiles[6][10] = TILE_EMPTY;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_BOULDER; e->mode = 1; e->dir = 1;
        e->x = (float)(7 * TILE); e->y = (float)((6 - 1) * TILE); }
      g.enemyStart[0] = g.enemies[0];
      for (i = 0; i < 60 && !dropped; i++) { sim_tick(&g, idle);
          if (g.enemies[0].y > (float)((6 - 1) * TILE) + 1.0f) dropped = true; }
      check(dropped, "boulder: drops into a one-tile gap");
      /* (d) spawn delay: parked off-screen, then drops in from the top */
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_BOULDER; e->mode = 1; e->dir = 1;
        e->spawnDelay = BOULDER_SPAWN_DELAY; e->x = -1000.0f; e->y = -1000.0f; }
      g.enemyStart[0] = g.enemies[0];
      for (i = 0; i < 50; i++) sim_tick(&g, idle);
      check(g.enemies[0].spawnDelay > 0 && g.enemies[0].x < 0.0f,
            "boulder: stays off-screen during its spawn delay");
      for (i = 0; i < 80; i++) sim_tick(&g, idle);
      check(g.enemies[0].spawnDelay == 0 && g.enemies[0].x >= 0.0f,
            "boulder: drops in from the top after the delay"); }

    /* enemies start at randomized (but seed-deterministic) phases each level */
    { int si, vals[8], n = 0, i; bool varied = false;
      for (si = 0; si < 8; si++) {
          GameState g; uint64_t seed = 0x7000ULL + (uint64_t)si * 0x9E37ULL;
          memset(&g, 0, sizeof g);
          sim_gen_room(&g, seed, 2, sim_entry_col_for(seed, 2));   /* depth 2: a crusher */
          if (g.enemyCount > 0 && g.enemies[0].type == EN_CRUSHER)
              vals[n++] = g.enemies[0].timer;
      }
      for (i = 1; i < n; i++) if (vals[i] != vals[0]) varied = true;
      check(n > 0 && varied, "enemy: start phase randomized across levels"); }

    /* dying resets every enemy to its level-start position (even one that has
     * wandered far from the spawn — that's what looked broken before) */
    { GameState g; Input z = {0};
      sim_init_seed(&g, 0xC0FFEEULL);
      sim_sync_room(&g, 2);                      /* depth 1 is safe; depth 2 has the crusher */
      check(g.enemyCount >= 1, "enemy: depth 2 has an enemy");
      if (g.enemyCount >= 1) {
          Enemy start0 = g.enemyStart[0];
          g.enemies[0].x += 5 * TILE; g.enemies[0].y += 2 * TILE; /* wander far off */
          g.enemies[0].timer += 9;
          g.state = PS_DYING; g.dyingTimer = 1; g.lives = 2;
          sim_tick(&g, z);                        /* dies -> respawns */
          check(memcmp(&g.enemies[0], &start0, sizeof(Enemy)) == 0,
                "enemy: resets to start position when the player is hit"); } }

    /* helpers ramp with the dangers: no platform on the safe first level, at
     * most one early on, and up to three in the deep rooms (from depth 12) */
    { GameState g; int si, deep3 = 0; bool cap0 = true, cap1 = true;
      for (si = 0; si < 80; si++) {
          uint64_t seed = 0x2100ULL + (uint64_t)si * 0x9E37ULL;
          memset(&g, 0, sizeof g);
          sim_gen_room(&g, seed, 1, sim_entry_col_for(seed, 1));
          if (g.platCount != 0) cap0 = false;
          memset(&g, 0, sizeof g);
          sim_gen_room(&g, seed, 3, sim_entry_col_for(seed, 3));
          if (g.platCount > 1) cap1 = false;
          memset(&g, 0, sizeof g);
          sim_gen_room(&g, seed, 25, sim_entry_col_for(seed, 25));
          if (g.platCount == 3) deep3++;
      }
      check(cap0, "plats: the safe first level has no moving platforms");
      check(cap1, "plats: early rooms carry at most one helper");
      check(deep3 > 0, "plats: deep rooms reach three helpers"); }

    /* moving platforms are deterministic within a run */
    { GameState a, b; long j;
      sim_init_seed(&a, 0x9A7); sim_init_seed(&b, 0x9A7);
      for (j = 0; j < 3000; j++) {
          Input in = {0}; in.right = (j % 3) != 0; in.jump = (j % 37) == 0;
          sim_tick(&a, in); sim_tick(&b, in);
      }
      check(memcmp(&a, &b, sizeof a) == 0, "platform: runs stay deterministic"); }

    /* every moving platform is anchored to a fixed platform (boardable), and both
     * kinds occur: vertical lifts and horizontal shuttles. Anchoring: its board
     * level aligns to a fixed platform's surface, adjacent to that platform's edge
     * at one of its travel extremes. */
    { int depths[5] = {3, 6, 10, 25, 50};
      int di, si, e, plats = 0, lifts = 0, ledges = 0; bool allAnchored = true;
      for (si = 0; si < 120; si++) {
          uint64_t seed = 0x2100ULL + (uint64_t)si * 0x9E37ULL;
          for (di = 0; di < 5; di++) {
              GameState g; int p;
              memset(&g, 0, sizeof g);
              sim_gen_room(&g, seed, depths[di], sim_entry_col_for(seed, depths[di]));
              for (p = 0; p < g.platCount; p++) {
                  MovePlat *pl = &g.plats[p];
                  int lo = (int)(pl->baseX / TILE);          /* left edge at the near extreme */
                  int hi = lo + pl->range / TILE;            /* left edge at the far extreme (horizontal) */
                  bool anch = false;
                  plats++;
                  if (pl->axis == 1) lifts++; else ledges++;
                  for (e = 0; e < g.fixCount; e++) {
                      int fy = g.fixRow[e] * TILE;
                      if ((int)pl->baseY != fy) continue;      /* board level aligns to this platform */
                      if (pl->axis == 1) {                      /* lift: just off an edge */
                          if (lo == g.fixHi[e] + 1 || lo == g.fixLo[e] - 2) anch = true;
                      } else {                                  /* shuttle: adjacent at either travel extreme */
                          if (lo == g.fixHi[e] + 1 || hi == g.fixHi[e] + 1 ||
                              lo == g.fixLo[e] - 2 || hi == g.fixLo[e] - 2) anch = true;
                      }
                  }
                  if (!anch) allAnchored = false;
              }
          }
      }
      check(allAnchored, "platform: every platform is boardable from a fixed platform");
      check(lifts > 0 && ledges > 0, "platform: both vertical lifts and horizontal shuttles occur");
      printf("      (%d platforms: %d lifts, %d ledges)\n", plats, lifts, ledges); }

    /* Solver-bot: a ground-movement bot (walk/climb/drop, no jumps) drives the
     * REAL physics to clear rooms the checker approved. It is a partial oracle —
     * its own navigation isn't perfect — so the clear RATE is used as a
     * regression signal (a generator that started producing unclearable rooms
     * would collapse it), not as a strict solvability proof. Its real win was
     * already banked: it drove out the head-clearance-on-drops bug in both the
     * checker and the physics. Raising the rate toward 100% needs a more robust
     * navigator (see DESIGN.md §7.2.4) and is tracked as future work. */
    { int depths[5] = {1, 5, 10, 25, 50};
      int di, si, cleared = 0, total = 0, perDepth[5] = {0};
      for (si = 0; si < 60; si++) {
          uint64_t seed = 0x5EEDULL + (uint64_t)si * 0x1234ULL;
          for (di = 0; di < 5; di++) {
              total++;
              if (bot_clear_room(seed, depths[di])) { cleared++; perDepth[di]++; }
          }
      }
      check(cleared * 2 >= total, "solver-bot: clear rate above regression floor (50%)");
      printf("      (%d/%d cleared; by depth d1=%d/60 d5=%d/60 d10=%d/60 "
             "d25=%d/60 d50=%d/60)\n", cleared, total,
             perDepth[0], perDepth[1], perDepth[2], perDepth[3], perDepth[4]); }

    /* demo mode: each play bot drives the LIVE game (enemies and all) and descends */
    { int d1 = 0, d2 = 0, si;
      for (si = 0; si < 20 && (d1 == 0 || d2 == 0); si++) {
          GameState g; Bot1Nav n1; Bot2Nav n2; int i, d0;
          uint64_t seed = 0x5EEDULL + (uint64_t)si * 0x1234ULL;
          sim_init_seed(&g, seed); g.lives = 200; bot1_nav_init(&n1); d0 = g.depth;
          for (i = 0; i < 4000 && g.depth == d0; i++) sim_tick(&g, bot1_input(&g, &n1));
          if (g.depth > d0) d1++;
          sim_init_seed(&g, seed); g.lives = 200; bot2_nav_init(&n2); d0 = g.depth;
          for (i = 0; i < 4000 && g.depth == d0; i++) sim_tick(&g, bot2_input(&g, &n2));
          if (g.depth > d0) d2++;
      }
      check(d1 > 0, "bot1: plays the live game and descends a level");
      check(d2 > 0, "bot2: plays the live game and descends a level"); }

    /* built-in bots can jump a one-tile gap to reach a same-level platform across
     * it: a floor with a single missing tile, coal on the far side, no ladder. */
    { GameState g; Bot1Nav n1; Bot2Nav n2; int i, c; bool b1 = false, b2 = false;
      sim_init_seed(&g, 5);
      memset(g.tiles, 0, sizeof g.tiles); memset(g.coal, 0, sizeof g.coal); memset(g.crumble, 0, sizeof g.crumble);
      for (c = 0; c < ROOM_W; c++) if (c != 9) g.tiles[9][c] = TILE_SOLID;   /* floor, 1-tile gap at col 9 */
      g.coal[8][10] = true; g.coalGot = 0; g.coalTotal = 1;
      g.px = (float)(8 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.vx = g.vy = 0; g.onGround = true; g.onLadder = false; g.ridingPlat = -1;
      g.state = PS_ALIVE; g.lives = 9; g.enemyCount = 0;
      bot1_nav_init(&n1);
      for (i = 0; i < 200 && !b1; i++) { sim_tick(&g, bot1_input(&g, &n1)); if (g.coalGot > 0) b1 = true; }
      check(b1, "bot1: jumps a one-tile gap to a platform across it");
      sim_init_seed(&g, 5);
      memset(g.tiles, 0, sizeof g.tiles); memset(g.coal, 0, sizeof g.coal); memset(g.crumble, 0, sizeof g.crumble);
      for (c = 0; c < ROOM_W; c++) if (c != 9) g.tiles[9][c] = TILE_SOLID;
      g.coal[8][10] = true; g.coalGot = 0; g.coalTotal = 1;
      g.px = (float)(8 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.vx = g.vy = 0; g.onGround = true; g.onLadder = false; g.ridingPlat = -1;
      g.state = PS_ALIVE; g.lives = 9; g.enemyCount = 0;
      bot2_nav_init(&n2);
      for (i = 0; i < 200 && !b2; i++) { sim_tick(&g, bot2_input(&g, &n2)); if (g.coalGot > 0) b2 = true; }
      check(b2, "bot2: jumps a one-tile gap to a platform across it"); }

    /* With a hoppable enemy ahead AND a ladder right here, the bots take the
     * ladder (climb up) instead of hopping the enemy. */
    { GameState g; Bot1Nav n1; Bot2Nav n2; int c, r; Input o1, o2;
      sim_init_seed(&g, 6);
      memset(g.tiles, 0, sizeof g.tiles); memset(g.coal, 0, sizeof g.coal); memset(g.crumble, 0, sizeof g.crumble);
      for (c = 0; c < ROOM_W; c++) g.tiles[9][c] = TILE_SOLID;      /* floor */
      for (r = 5; r <= 8; r++) g.tiles[r][5] = TILE_LADDER;         /* a ladder at col 5 */
      g.coal[8][10] = true; g.coalGot = 0; g.coalTotal = 1;         /* coal to the right -> bot wants right */
      g.px = (float)(5 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.vx = g.vy = 0; g.onGround = true; g.onLadder = false; g.ridingPlat = -1;
      g.state = PS_ALIVE; g.lives = 9;
      g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_FOREMAN; e->dir = -1;                          /* approaching from the right,
                                                                     * inside the ~2-tile hop window */
        e->x = 98.0f; e->y = (float)(9 * TILE - HITBOX_H); }
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check(o1.up && !o1.jump, "bot1: prefers a nearby ladder over hopping the enemy");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(o2.up && !o2.jump, "bot2: prefers a nearby ladder over hopping the enemy"); }

    /* The ladder escape works downward too: hoppable enemy ahead, only a
     * down-ladder at our column -> the bots climb down instead of hopping. */
    { GameState g; Bot1Nav n1; Bot2Nav n2; int c, r; Input o1, o2;
      sim_init_seed(&g, 7);
      memset(g.tiles, 0, sizeof g.tiles); memset(g.coal, 0, sizeof g.coal); memset(g.crumble, 0, sizeof g.crumble);
      for (c = 0; c < ROOM_W; c++) g.tiles[9][c] = TILE_SOLID;
      for (r = 9; r <= 11; r++) g.tiles[r][5] = TILE_LADDER;      /* a ladder going down at col 5 */
      g.tiles[12][5] = TILE_SOLID;                                /* lower landing */
      g.coal[8][10] = true; g.coalGot = 0; g.coalTotal = 1;       /* coal to the right */
      g.px = (float)(5 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.vx = g.vy = 0; g.onGround = true; g.onLadder = false; g.ridingPlat = -1;
      g.state = PS_ALIVE; g.lives = 9; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_FOREMAN; e->dir = -1; e->x = 98.0f; e->y = (float)(9 * TILE - HITBOX_H); }
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check(o1.down && !o1.jump, "bot1: escapes down a ladder instead of hopping");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(o2.down && !o2.jump, "bot2: escapes down a ladder instead of hopping"); }

    /* Cornered: a threat we can't hop over (a bat), no room to step aside, no
     * ladder -> the bots jump AWAY from it, onto a survivable landing (they
     * no longer leap into a fatal fall — that's just a slower death). */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int c;
      sim_init_seed(&g, 8);
      memset(g.tiles, 0, sizeof g.tiles); memset(g.coal, 0, sizeof g.coal); memset(g.crumble, 0, sizeof g.crumble);
      g.tiles[9][5] = TILE_SOLID;                                 /* a lone 1-tile platform */
      for (c = 1; c <= 3; c++) g.tiles[12][c] = TILE_SOLID;      /* survivable landing to the left */
      g.coalGot = 0; g.coalTotal = 0;                             /* no coal: pure avoidance */
      g.px = (float)(5 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.vx = g.vy = 0; g.onGround = true; g.onLadder = false; g.ridingPlat = -1;
      g.state = PS_ALIVE; g.lives = 9; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e);
        e->type = EN_BAT; e->dir = -1; e->vdir = 1; e->x = 96.0f; e->y = (float)(9 * TILE - HITBOX_H); }
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      check(o1.jump && o1.left && !o1.right, "bot1: jumps away when cornered and can't hop over");
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(o2.jump && o2.left && !o2.right, "bot2: jumps away when cornered and can't hop over"); }

    /* an IDLE crusher parked over the route is a timing problem, not a flee
     * problem: the bots walk under it without vibrating on its threat
     * boundary — and never step INTO its column while it's armed. */
    { GameState g, g2; Bot1Nav n1; Bot2Nav n2; Input o; int i, c, hd, lastHd;
      int rev1 = 0, rev2 = 0; bool got1 = false, got2 = false;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 0; c < ROOM_W; c++) g.tiles[9][c] = TILE_SOLID;    /* long floor */
      g.coal[8][12] = true; g.coalTotal = 1; g.exitCol = 15;
      g.px = (float)(4 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.onGround = true; g.facing = 1; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
        e->baseX = (float)(8 * TILE); e->baseY = (float)(6 * TILE);   /* raised, above the path */
        e->x = e->baseX; e->y = e->baseY; e->range = 2 * TILE;
        e->period = 6000; e->phase = 0; }                             /* idling for a long time */
      g.enemyStart[0] = g.enemies[0];
      g2 = g;
      bot1_nav_init(&n1); lastHd = 0;
      for (i = 0; i < 400 && !got1; i++) {
          o = bot1_input(&g, &n1);
          hd = o.right ? 1 : (o.left ? -1 : 0);
          if (hd && lastHd && hd == -lastHd) rev1++;
          if (hd) lastHd = hd;
          sim_tick(&g, o);
          if (g.coalGot > 0) got1 = true;
      }
      bot2_nav_init(&n2); lastHd = 0;
      for (i = 0; i < 400 && !got2; i++) {
          o = bot2_input(&g2, &n2);
          hd = o.right ? 1 : (o.left ? -1 : 0);
          if (hd && lastHd && hd == -lastHd) rev2++;
          if (hd) lastHd = hd;
          sim_tick(&g2, o);
          if (g2.coalGot > 0) got2 = true;
      }
      check(got1 && rev1 <= 2, "bot1: passes under an idle crusher without vibrating");
      check(got2 && rev2 <= 2, "bot2: passes under an idle crusher without vibrating"); }

    /* ...but an ARMED (warning/slamming) crusher's column is never entered */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int c;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 0; c < ROOM_W; c++) g.tiles[9][c] = TILE_SOLID;
      g.coal[8][12] = true; g.coalTotal = 1; g.exitCol = 15;
      g.px = (float)(5 * TILE); g.py = (float)(9 * TILE - HITBOX_H);  /* beside its column */
      g.onGround = true; g.facing = 1; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
        e->baseX = (float)(6 * TILE); e->baseY = (float)(6 * TILE);
        e->x = e->baseX; e->y = e->baseY; e->range = 2 * TILE;
        e->period = 6000; e->phase = 1; }                             /* flashing: about to slam */
      g.enemyStart[0] = g.enemies[0];
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(!o1.right, "bot1: won't step into an armed crusher's column");
      check(!o2.right, "bot2: won't step into an armed crusher's column"); }

    /* leaping off a ladder (heel-leap) never launches through a crusher's
     * column — touching one kills even while idle, and the arc rises into
     * its raised body. With a crusher looming left, the leap goes RIGHT. */
    { GameState g, g2; Bot1Nav n1; Bot2Nav n2; Input o1, o2; int c, r;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      for (c = 2; c <= 8; c++) g.tiles[10][c] = TILE_SOLID;      /* floor under everything */
      for (r = 6; r <= 9; r++) g.tiles[r][5] = TILE_LADDER;      /* ladder col 5 */
      g.px = (float)(5 * TILE + 3); g.py = (float)(9 * TILE - HITBOX_H);  /* mid-ladder */
      g.onGround = false; g.onLadder = true; g.facing = 1;
      g.enemyCount = 2;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN;
        e->x = (float)(5 * TILE + 3); e->y = (float)(10 * TILE - HITBOX_H);
        e->mode = 1; e->vdir = -1; }                             /* at our heels below */
      { Enemy *e = &g.enemies[1]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
        e->baseX = (float)(3 * TILE); e->baseY = (float)(4 * TILE);
        e->x = e->baseX; e->y = e->baseY; e->range = 4 * TILE;
        e->period = 120; e->phase = 0; e->timer = 0; }           /* short-period: never long-idle */
      g.enemyStart[0] = g.enemies[0]; g.enemyStart[1] = g.enemies[1];
      g2 = g;
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      bot2_nav_init(&n2); o2 = bot2_input(&g2, &n2);
      /* STEADY's wide margin may refuse the leap entirely (racing up instead)
       * — also safe; the property under test is never leaping INTO the crusher */
      check(!(o1.jump && o1.left), "bot1: heel-leap never launches into the crusher column");
      check(o2.jump && o2.right && !o2.left, "bot2: heel-leap avoids the crusher column"); }

    /* truly cornered with NO landing anywhere and the foe right on us: leap
     * away regardless — a moving platform may drift under the arc, and
     * standing still is certain death. */
    { GameState g; Bot1Nav n1; Bot2Nav n2; Input o1, o2;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      g.tiles[9][5] = TILE_SOLID;                     /* a lone tile, void all around */
      g.px = (float)(5 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.onGround = true; g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_BAT; e->dir = -1;
        e->x = 98.0f; e->y = (float)(9 * TILE - HITBOX_H); }   /* closing in from the right */
      bot1_nav_init(&n1); o1 = bot1_input(&g, &n1);
      bot2_nav_init(&n2); o2 = bot2_input(&g, &n2);
      check(o1.jump && o1.left, "bot1: with no escape at all, leaps away and takes the chance");
      check(o2.jump && o2.left, "bot2: with no escape at all, leaps away and takes the chance"); }

    /* bot1 wedged on a lone platform at the left edge (target unreachable) breaks
     * out with an INWARD hop, never off the screen. */
    { GameState g; Bot1Nav n1; Input o = {0}; int i; bool jumped = false;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      g.tiles[9][1] = TILE_SOLID;                     /* a lone tile at col 1 */
      g.tiles[12][3] = TILE_SOLID;                    /* survivable landing inward */
      g.coal[3][15] = true; g.coalGot = 0; g.coalTotal = 1;   /* unreachable coal */
      g.px = (float)(1 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.onGround = true; g.facing = 1;
      bot1_nav_init(&n1);
      for (i = 0; i < 120 && !jumped; i++) { o = bot1_input(&g, &n1); if (o.jump) jumped = true; }
      check(jumped && o.right && !o.left, "bot1: breaks out of a left-edge lip inward, not off-screen"); }

    /* ...but it doesn't loop: with the target truly unreachable it gives up after a
     * couple of escape hops (so the arena's stuck watchdog resets it). */
    { GameState g; Bot1Nav n1; int i, jumps = 0;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
      g.tiles[9][1] = TILE_SOLID;
      g.coal[3][15] = true; g.coalGot = 0; g.coalTotal = 1;   /* unreachable coal */
      g.px = (float)(1 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
      g.onGround = true; g.facing = 1;
      bot1_nav_init(&n1);
      for (i = 0; i < 700; i++) { if (bot1_input(&g, &n1).jump) jumps++; }
      check(jumps <= 2, "bot1: gives up after a couple of escapes instead of looping"); }

    /* MoleScript: a loaded script drives the live game, descends, and is
     * deterministic; a malformed script is reported, not crashed. */
    { static const char *SRC =
        "loop:\n"
        "    if coal_remaining() > 0 then\n"
        "        go_to(nearest_coal())\n"
        "    else\n"
        "        go_to(exit)\n"
        "    end\n"
        "loop_end\n";
      char err[160]; int md = 0, si; bool determ = true;
      MoleVM *vm = mole_load(SRC, 0x1234ULL, err, sizeof err);
      check(vm != NULL, "mole: example script compiles");
      if (vm) {
          for (si = 0; si < 20 && md == 0; si++) {
              GameState g; int i, d0;
              uint64_t seed = 0x5EEDULL + (uint64_t)si * 0x1234ULL;
              sim_init_seed(&g, seed); g.lives = 200; mole_reset(vm, seed); d0 = g.depth;
              for (i = 0; i < 4000 && g.depth == d0; i++) sim_tick(&g, mole_input(vm, &g));
              if (g.depth > d0) md++;
          }
          check(md > 0, "mole: example script plays the live game and descends");
          { GameState a, b; MoleVM *v2 = mole_load(SRC, 0x1234ULL, err, sizeof err);
            long j; uint64_t seed = 0x5EEDULL;
            sim_init_seed(&a, seed); sim_init_seed(&b, seed); a.lives = b.lives = 200;
            mole_reset(vm, seed); mole_reset(v2, seed);
            for (j = 0; j < 3000; j++) { sim_tick(&a, mole_input(vm, &a));
                                         sim_tick(&b, mole_input(v2, &b)); }
            determ = (memcmp(&a, &b, sizeof a) == 0);
            mole_free(v2); }
          check(determ, "mole: identical script + seed play identically");
          mole_free(vm);
      }
      { char e[160]; MoleVM *bad = mole_load("loop:\n  walk\nloop_end\n", 0, e, sizeof e);
        check(bad == NULL && e[0] != '\0', "mole: malformed script is reported, not crashed"); }
      /* grammar: single-line if needs no 'end'; blocks use their own terminators */
      { char e[160]; MoleVM *v;
        v = mole_load("loop:\n if 1 == 1 then wait 1\nloop_end\n", 0, e, sizeof e);
        check(v != NULL, "mole: single-line if (no end) compiles"); mole_free(v);
        v = mole_load("loop:\n if 1 == 1 then wait 1 else wait 2\nloop_end\n", 0, e, sizeof e);
        check(v != NULL, "mole: single-line if/else (no end) compiles"); mole_free(v);
        v = mole_load("func f():\n return 1\nfunc_end\nloop:\n wait 1\nloop_end\n", 0, e, sizeof e);
        check(v != NULL, "mole: func_end / loop_end block terminators compile"); mole_free(v);
        v = mole_load("loop:\n wait 1\nend\n", 0, e, sizeof e);
        check(v == NULL, "mole: 'end' no longer closes a loop (needs loop_end)"); }
      /* elif chains: compile, and the matching arm runs (skipping earlier/later). */
      { char e[160]; MoleVM *v;
        v = mole_load("loop:\n if 1==2 then\n  wait 1\n elif 1==3 then\n  wait 2\n elif 1==1 then\n  wait 3\n else\n  wait 4\n end\nloop_end\n", 0, e, sizeof e);
        check(v != NULL, "mole: elif chain compiles"); mole_free(v);
        /* a false 'if' and false first 'elif' fall through to a true second 'elif',
         * whose 'jump' fires on tick 1 regardless of the world (edge-triggered). */
        v = mole_load("loop:\n if 1==2 then\n  wait 5\n elif 1==2 then\n  wait 5\n elif 1==1 then\n  jump\n else\n  wait 5\n end\nloop_end\n", 1, e, sizeof e);
        check(v != NULL, "mole: elif behaviour script compiles");
        if (v) { GameState g; Input in; sim_init_seed(&g, 1); mole_reset(v, 1);
                 in = mole_input(v, &g);
                 check(in.jump, "mole: elif runs the first matching arm, skips the rest"); }
        mole_free(v); }
      /* numeric + direction helpers compile and evaluate correctly. Each value
       * check uses jump on tick 1 (edge-triggered, world-independent) as the
       * observable: it fires only if every assertion in the condition holds. */
      { char e[160]; MoleVM *v; GameState g; Input in;
        v = mole_load("loop:\n x = abs(0-3) + distance(2,9) + lower(4,1) + higher(4,1) + limit(20,0,5)\n wait 1\nloop_end\n", 0, e, sizeof e);
        check(v != NULL, "mole: abs/distance/lower/higher/limit compile"); mole_free(v);
        v = mole_load("loop:\n if abs(0-3)==3 and distance(2,9)==7 and lower(4,1)==1 and higher(4,1)==4 and limit(20,0,5)==5 then\n  jump\n else\n  wait 5\n end\nloop_end\n", 1, e, sizeof e);
        check(v != NULL, "mole: helper-value script compiles");
        if (v) { sim_init_seed(&g, 1); mole_reset(v, 1); in = mole_input(v, &g);
                 check(in.jump, "mole: abs/distance/lower/higher/limit return correct values"); }
        mole_free(v);
        v = mole_load("loop:\n if side_of(0-5)==left and side_of(5)==right and side_of(0)==any then\n  jump\n else\n  wait 5\n end\nloop_end\n", 1, e, sizeof e);
        check(v != NULL, "mole: side_of script compiles");
        if (v) { sim_init_seed(&g, 1); mole_reset(v, 1); in = mole_input(v, &g);
                 check(in.jump, "mole: side_of maps sign to left/right/any"); }
        mole_free(v);
        v = mole_load("loop:\n if sign_of(0-5)==0-1 and sign_of(7)==1 and sign_of(0)==0 then\n  jump\n else\n  wait 5\n end\nloop_end\n", 1, e, sizeof e);
        check(v != NULL, "mole: sign_of script compiles");
        if (v) { sim_init_seed(&g, 1); mole_reset(v, 1); in = mole_input(v, &g);
                 check(in.jump, "mole: sign_of returns -1/0/+1"); }
        mole_free(v); }
      /* hardening: pathological nesting is rejected at compile time, not a crash. */
      { char e[160]; int k; char *deep = (char *)malloc(40000);
        char *p = deep;
        p += sprintf(p, "loop:\n x = ");
        for (k = 0; k < 5000; k++) *p++ = '(';
        *p++ = '1';
        for (k = 0; k < 5000; k++) *p++ = ')';
        sprintf(p, "\nloop_end\n");
        { MoleVM *v = mole_load(deep, 0, e, sizeof e);
          check(v == NULL && e[0] != '\0', "mole: deep nesting is rejected, not a crash"); }
        free(deep); }
      /* a crusher beside/below is not a threat (its ladder is safe): enemy_within
       * ignores crushers, but still sees a foreman in the same spot. */
      { char e[160];
        MoleVM *v = mole_load("loop:\n if enemy_within(3, below) then walk left else walk right\nloop_end\n",
                              1, e, sizeof e);
        check(v != NULL, "mole: crusher-safety probe compiles");
        if (v) {
            GameState g; Input in;
            memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
            g.px = 83; g.py = 64; g.facing = 1; g.onGround = true;   /* bot at cell (5,5) */
            g.enemyCount = 1; g.enemies[0].x = 96; g.enemies[0].y = 96;  /* enemy at (6,6): one col over, one row below */
            g.enemies[0].type = EN_CRUSHER;
            mole_reset(v, 1); in = mole_input(v, &g);
            check(in.right && !in.left, "mole: enemy_within ignores a crusher below (ladder beside it is safe)");
            g.enemies[0].type = EN_FOREMAN;
            mole_reset(v, 1); in = mole_input(v, &g);
            check(in.left && !in.right, "mole: enemy_within still sees a foreman below");
            mole_free(v);
        } }
      /* crusher .state: a bot can tell idle (safe to pass under) from warning
       * (flashing, about to slam) from moving. The probe walks left when idle,
       * right when warning, and jumps when moving. */
      { static const char *ST =
            "loop:\n"
            "  e = nearest_enemy()\n"
            "  if e != none and e.state == idle then walk left\n"
            "  if e != none and e.state == warning then walk right\n"
            "  if e != none and e.state == moving then jump\n"
            "  wait 1\n"
            "loop_end\n";
        char e2[160]; MoleVM *v = mole_load(ST, 1, e2, sizeof e2);
        check(v != NULL, "mole: crusher .state probe compiles");
        if (v) {
            int ph, ok = 1;
            int wantL[3] = {1, 0, 0}, wantR[3] = {0, 1, 0}, wantJ[3] = {0, 0, 1};
            for (ph = 0; ph <= 2; ph++) {
                GameState g; Input in;
                memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
                g.px = 83; g.py = 66; g.facing = 1;                 /* bot at cell (5,5) */
                g.enemyCount = 1; g.enemies[0].type = EN_CRUSHER;
                g.enemies[0].x = 96; g.enemies[0].y = 80;           /* crusher at (6,5) */
                g.enemies[0].phase = ph;
                mole_reset(v, 1); in = mole_input(v, &g);
                if (in.left != wantL[ph] || in.right != wantR[ph] || in.jump != wantJ[ph]) ok = 0;
            }
            check(ok, "mole: crusher .state reads idle / warning / moving from its phase");
            mole_free(v);
        } }
      /* say — the script's introspection line: string + value parts build one
       * message per tick, readable via mole_say (drawn under the bot in-game). */
      { char e2[160];
        MoleVM *v = mole_load("# author: t\nloop:\n say \"digging\", my_col\n idle\nloop_end\n",
                              1, e2, sizeof e2);
        check(v != NULL, "mole: say script compiles");
        if (v) {
            GameState g;
            memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
            g.px = 83; g.py = 66; g.onGround = true;      /* cell (5,5) */
            mole_reset(v, 1);
            check(mole_say(v)[0] == '\0', "mole: say starts empty");
            mole_input(v, &g);
            check(strcmp(mole_say(v), "digging 5") == 0,
                  "mole: say builds the introspection line");
            mole_free(v);
        } }
      /* the built-in bots narrate too — their nav state carries a say line
       * (goal announcements at minimum), streamed to bot1.log / bot2.log. */
      { GameState g; Bot1Nav n1; Bot2Nav n2; int c2;
        memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
        for (c2 = 0; c2 < ROOM_W; c2++) g.tiles[9][c2] = TILE_SOLID;
        g.coal[8][10] = true; g.coalTotal = 1; g.exitCol = 15;
        g.px = (float)(3 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
        g.onGround = true; g.facing = 1;
        bot1_nav_init(&n1); bot2_nav_init(&n2);
        check(n1.say[0] == '\0' && n2.say[0] == '\0', "bots: narration starts empty");
        bot1_input(&g, &n1);
        bot2_input(&g, &n2);
        check(strstr(n1.say, "coal") != NULL, "bot1: narrates its coal goal");
        check(strstr(n2.say, "coal") != NULL, "bot2: narrates its coal goal"); }

    /* §5.5 crusher-path safety: a crusher whose slam column severs the only
     * route is deleted at generation; one over open, jumpable floor stays. */
    { GameState g; int c2;
      memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 3; g.ridingPlat = -1;
      for (c2 = 0; c2 < ROOM_W; c2++) g.tiles[BEDROCK_ROW][c2] = TILE_SOLID;
      g.spawnCol = 2; g.exitCol = 13;
      g.coal[BEDROCK_ROW - 1][10] = true; g.coalTotal = 1;
      g.enemyCount = 1;
      { Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
        e->baseX = (float)(8 * TILE);                 /* slam floor = bedrock */
        e->baseY = (float)(10 * TILE); e->range = 4 * TILE; }
      sim_cull_blocking_crushers(&g);
      check(g.enemyCount == 1, "crusher cull: an open-floor crusher stays (jumpable around)");
      g.enemies[0].baseX = (float)(2 * TILE);         /* now it slams the only entry landing */
      sim_cull_blocking_crushers(&g);
      check(g.enemyCount == 0, "crusher cull: one severing the only path is deleted"); }
      /* a ladder does NOT shield from a climbing foreman: the retreat pattern
       * (as used by greedy/cautious.mole) must step OFF the rail once topped
       * out — standing on the top rung "feeling safe" gets a bot caught. */
      { static const char *LT =
            "loop:\n"
            " if on_ladder and enemy_within(2, below) then\n"
            "  if is_ladder(my_col, my_row - 1) then\n"
            "   climb up\n"
            "  elif is_standable(my_col - 1, my_row) then\n"
            "   walk left\n"
            "  else\n"
            "   walk right\n"
            "  end\n"
            "  continue\n"
            " end\n"
            " idle\n"
            "loop_end\n";
        char e2[160]; MoleVM *v = mole_load(LT, 1, e2, sizeof e2);
        check(v != NULL, "mole: ladder-top retreat probe compiles");
        if (v) {
            GameState g; Input in; int r2;
            memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
            for (r2 = 6; r2 <= 9; r2++) g.tiles[r2][5] = TILE_LADDER;   /* run in col 5 */
            g.tiles[7][3] = TILE_SOLID; g.tiles[7][4] = TILE_SOLID;     /* floor beside the top */
            g.tiles[7][6] = TILE_SOLID; g.tiles[7][7] = TILE_SOLID;
            g.px = (float)(5 * TILE + 3); g.py = (float)(7 * TILE - HITBOX_H);  /* on the top rung */
            g.onLadder = true; g.onGround = true;
            g.enemyCount = 1; g.enemies[0].type = EN_FOREMAN;           /* climbing up at us */
            g.enemies[0].x = (float)(5 * TILE); g.enemies[0].y = (float)(8 * TILE - HITBOX_H);
            mole_reset(v, 1); in = mole_input(v, &g);
            check((in.left || in.right) && !in.up,
                  "mole: topped-out retreat steps off the rail, not up");
            mole_free(v);
        } }
      /* parity: enemy_count/enemy(i) expose the whole roster — including foes
       * far beyond the perception range that nearest_enemy is capped to. */
      { char e2[160];
        MoleVM *v = mole_load(
            "loop:\n"
            " if enemy_count() == 2 and enemy(1) != none and enemy(1).type == BAT"
            " then walk right else walk left\n"
            "loop_end\n", 1, e2, sizeof e2);
        check(v != NULL, "mole: enemy roster probe compiles");
        if (v) {
            GameState g; Input in;
            memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
            g.px = 3; g.py = 66; g.onGround = true;              /* far left */
            g.enemyCount = 2;
            g.enemies[0].type = EN_FOREMAN; g.enemies[0].x = 96;  g.enemies[0].y = 96;
            g.enemies[1].type = EN_BAT;     g.enemies[1].x = 288; g.enemies[1].y = 200;  /* way out of sight */
            mole_reset(v, 1); in = mole_input(v, &g);
            check(in.right && !in.left, "mole: enemy(i) sees the whole roster (beyond sight)");
            mole_free(v);
        } }
      /* parity: moving platforms, riding state, armed crumble tiles and the
       * score are all sensable — everything the built-in bots could read. */
      { char e2[160];
        MoleVM *v = mole_load(
            "loop:\n"
            " if plat_count() == 1 and plat(0) != none and plat_dir(0) == right"
            " and on_platform and shaking(3, 6) and score() == 250"
            " then walk right else walk left\n"
            "loop_end\n", 1, e2, sizeof e2);
        check(v != NULL, "mole: platform/crumble/score sensors compile");
        if (v) {
            GameState g; Input in;
            memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1;
            g.px = 83; g.py = 66; g.onGround = true;
            g.platCount = 1; g.ridingPlat = 0;
            g.plats[0].x = 64; g.plats[0].y = 128; g.plats[0].axis = 0; g.plats[0].dir = 1;
            g.tiles[6][3] = TILE_CRUMBLE; g.crumble[6][3] = 5;   /* armed, about to fall */
            g.score = 250;
            mole_reset(v, 1); in = mole_input(v, &g);
            check(in.right && !in.left, "mole: platforms, shaking crumble and score are sensable");
            mole_free(v);
        } }
      /* jump-over: a hop clears a ground enemy only when launched ~2 tiles out at
       * an APPROACHING foe (a 1-tile hop dies on the way up). Verify a script that
       * uses .going to time it actually clears an approaching foreman alive. */
      { static const char *JS =
            "loop:\n"
            "  e = nearest_enemy()\n"
            "  if e != none and e.col == my_col + 2 and e.going == left then\n"
            "    jump right\n"
            "    continue\n"
            "  end\n"
            "  walk right\n"
            "loop_end\n";
        char e2[160]; MoleVM *v = mole_load(JS, 7, e2, sizeof e2);
        check(v != NULL, "mole: jump-over probe compiles");
        if (v) {
            GameState g; int i, jumped = 0, died = 0, cc, ecol; float ex, ey;
            memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 1; g.ridingPlat = -1;
            for (i = 0; i < ROOM_W; i++) g.tiles[10][i] = TILE_SOLID;
            g.px = (float)(4 * TILE + 3); g.py = (float)(10 * TILE - HITBOX_H);
            g.onGround = true; g.facing = 1;
            g.enemyCount = 1; g.enemies[0].type = EN_FOREMAN;
            ex = (float)(6 * TILE); ey = (float)(10 * TILE - TILE);
            g.enemies[0].x = ex; g.enemies[0].y = ey;
            g.enemies[0].baseX = ex; g.enemies[0].baseY = ey; g.enemies[0].dir = -1;
            mole_reset(v, 7);
            for (i = 0; i < 60; i++) {
                Input in = mole_input(v, &g);
                if (in.jump) jumped = 1;
                sim_tick(&g, in);
                if (g.state != PS_ALIVE) { died = 1; break; }
                cc = (int)((g.px + HITBOX_W * 0.5f) / TILE);
                ecol = (int)((g.enemies[0].x + 8) / TILE);
                if (cc > ecol + 1) break;   /* got safely past the foreman */
            }
            cc = (int)((g.px + HITBOX_W * 0.5f) / TILE);
            ecol = (int)((g.enemies[0].x + 8) / TILE);
            check(jumped && !died && cc > ecol,
                  "mole: a well-timed hop clears an approaching foreman");
            mole_free(v);
        } }
      /* path control: a script's go_to bias steers its own route. In a room with
       * two equally-short ways to a cell (a fork with a ladder on each side), a
       * left-biased script and a right-biased one take different sides — so two
       * bots diverge by their own logic, not by chance. */
      { static const char *SL = "loop:\n go_to(cell(5, 3), left)\nloop_end\n";
        static const char *SR = "loop:\n go_to(cell(5, 3), right)\nloop_end\n";
        char e[160]; MoleVM *vl = mole_load(SL, 1, e, sizeof e);
        MoleVM *vr = mole_load(SR, 1, e, sizeof e);
        check(vl && vr, "mole: go_to(cell, bias) and cell() compile");
        if (vl && vr) {
            GameState a, b; int i, amin = 99, bmin = 99, sl = 2;
            int rr, cc2;
            memset(&a, 0, sizeof a); a.state = PS_ALIVE; a.lives = 1; a.ridingPlat = -1;
            for (cc2 = 2; cc2 <= 8; cc2++) { a.tiles[7][cc2] = TILE_SOLID; a.tiles[4][cc2] = TILE_SOLID; }
            for (rr = 4; rr <= 6; rr++) { a.tiles[rr][3] = TILE_LADDER; a.tiles[rr][7] = TILE_LADDER; }
            a.px = (float)(5 * TILE + 3); a.py = 94.0f; a.onGround = true; a.facing = 1;
            b = a;
            mole_reset(vl, 1); mole_reset(vr, 1);
            for (i = 0; i < 90; i++) {
                int ca, cb;
                sim_tick(&a, mole_input(vl, &a));
                sim_tick(&b, mole_input(vr, &b));
                ca = (int)((a.px + HITBOX_W * 0.5f) / TILE);
                cb = (int)((b.px + HITBOX_W * 0.5f) / TILE);
                if (ca < amin) amin = ca;
                if (cb < bmin) bmin = cb;
            }
            (void)sl;
            check(amin < bmin, "mole: go_to bias lets a script pick its own path (left vs right diverge)");
            mole_free(vl); mole_free(vr);
        } }
      /* route(): a fully self-driven bot (asks the pathfinder for a direction,
       * then moves itself with walk/climb) can navigate a whole room and descend
       * — proving hand-driven bots aren't stuck at the top. */
      { static const char *RT =
            "loop:\n"
            "  if coal_remaining() == 0 then\n"
            "    go_to(exit)\n"
            "    continue\n"
            "  end\n"
            "  d = route(nearest_coal())\n"
            "  if d == up then climb up\n"
            "  if d == down then climb down\n"
            "  if d == left then walk left\n"
            "  if d == right then walk right\n"
            "  if d == none then wait 1\n"
            "loop_end\n";
        char err[160]; int md = 0, si; MoleVM *vm = mole_load(RT, 0x1234ULL, err, sizeof err);
        check(vm != NULL, "mole: route()-driven bot compiles");
        if (vm) {
            for (si = 0; si < 20 && md == 0; si++) {
                GameState g; int i, d0;
                uint64_t seed = 0x5EEDULL + (uint64_t)si * 0x1234ULL;
                sim_init_seed(&g, seed); g.lives = 200; mole_reset(vm, seed); d0 = g.depth;
                for (i = 0; i < 6000 && g.depth == d0; i++) sim_tick(&g, mole_input(vm, &g));
                if (g.depth > d0) md++;
            }
            check(md > 0, "mole: route()+walk/climb navigates and descends (not stuck at the top)");
            mole_free(vm);
        } } }

    /* generation determinism: same seed/depth -> identical layout */
    { GameState a, b;
      memset(&a, 0, sizeof a); memset(&b, 0, sizeof b);
      sim_gen_room(&a, 0xABCDEF, 7, sim_entry_col_for(0xABCDEF, 7));
      sim_gen_room(&b, 0xABCDEF, 7, sim_entry_col_for(0xABCDEF, 7));
      check(memcmp(a.tiles, b.tiles, sizeof a.tiles) == 0 &&
            memcmp(a.coal, b.coal, sizeof a.coal) == 0,
            "gen: identical seed/depth produces identical room"); }

    /* ---- fuzz: mixed input never leaves the room or crashes ---- */
    sim_init(&s);
    for (i = 0; i < 3000; i++) {
        Input in = {0};
        in.right = (i / 60) % 2 == 0;
        in.left  = !in.right;
        in.up    = (i % 5) == 0;
        in.down  = (i % 7) == 0;
        if (i % 45 == 0) in.jump = true;
        sim_tick(&s, in);
        if (s.px < -1 || s.px > SCREEN_W || s.py < -TILE || s.py > ROOM_H * TILE)
            break;
    }
    check(i == 3000, "fuzz: 3000 mixed-input ticks stay inside room bounds");

    /* ---- play determinism: same script twice -> byte-identical ---- */
    { GameState a, b; long j;
      sim_init(&a); sim_init(&b);
      for (j = 0; j < 5000; j++) {
          Input in = {0};
          in.right = (j % 3) != 0; in.jump = (j % 41) == 0;
          in.up = (j % 5) == 0; in.down = (j % 11) == 0;
          sim_tick(&a, in); sim_tick(&b, in);
      }
      check(memcmp(&a, &b, sizeof a) == 0, "determinism: identical runs match"); }

    /* synth: silent when idle, audible when triggered, and self-terminating */
    { Synth syn; float buf[512]; int i; bool silent = true, audible = false;
      synth_init(&syn);
      synth_render(&syn, buf, 512);
      for (i = 0; i < 512; i++) if (buf[i] != 0.0f) silent = false;
      check(silent, "synth: silent when nothing is playing");
      synth_trigger(&syn, SFX_COAL);
      synth_render(&syn, buf, 512);
      for (i = 0; i < 512; i++) if (buf[i] > 0.01f || buf[i] < -0.01f) audible = true;
      check(audible, "synth: produces sound when triggered");
      { int t; for (t = 0; t < SYNTH_RATE; t++) synth_render(&syn, buf, 1); /* 1s */ }
      synth_render(&syn, buf, 512);
      silent = true;
      for (i = 0; i < 512; i++) if (buf[i] != 0.0f) silent = false;
      check(silent, "synth: effect finishes and goes silent"); }

    /* music: loops audibly and (per §4.3) leaves a voice free for SFX */
    { Synth syn; float buf[4096]; int i; bool audible = false; long t;
      synth_init(&syn);
      synth_music_play(&syn, SONG_INGAME);
      for (t = 0; t < 2 * SYNTH_RATE / 4096; t++) {    /* ~2s of playback */
          synth_render(&syn, buf, 4096);
          for (i = 0; i < 4096; i++) if (buf[i] > 0.02f || buf[i] < -0.02f) audible = true;
      }
      check(audible, "synth: music loop is audible");
      check(syn.v[0].active == false && syn.v[1].active == false &&
            syn.v[2].active == false, "synth: music leaves SFX voices free");
      synth_trigger(&syn, SFX_COAL);
      check(syn.v[0].active, "synth: SFX still plays over music"); }

    /* game-over jingle (§4.2): minor-key, audible, and plays exactly once */
    { Synth syn; float buf[4096]; int i; bool audible = false; long t;
      synth_init(&syn);
      synth_music_play(&syn, SONG_OVER);
      for (t = 0; t < 5L * SYNTH_RATE / 4096; t++) {    /* ~5s > one 2.6s pass */
          synth_render(&syn, buf, 4096);
          for (i = 0; i < 4096; i++) if (buf[i] > 0.02f || buf[i] < -0.02f) audible = true;
      }
      check(audible, "synth: game-over jingle is audible");
      check(!syn.music.on, "synth: game-over jingle plays once and stops itself"); }

    /* the death jingle ducks the music, which then fades back in */
    { Synth syn; float buf[64]; int t;
      synth_init(&syn);
      synth_music_play(&syn, SONG_TITLE);
      synth_trigger(&syn, SFX_DEATH);
      check(syn.music.duckPos > 0, "synth: the death jingle ducks the music");
      for (t = 0; t < SYNTH_RATE / 64 + 1; t++) synth_render(&syn, buf, 64);   /* ~1s */
      check(syn.music.duckPos == 0, "synth: the duck releases after the jingle"); }

    /* the 10-depth descend sweep: arms, stays in range, and ends */
    { Synth syn; float buf[4096]; int i; bool ok = true; long t;
      synth_init(&syn);
      synth_music_play(&syn, SONG_TITLE);
      synth_sweep(&syn);
      check(syn.music.sweepPos > 0, "synth: the descend filter sweep arms");
      for (t = 0; t < 2L * SYNTH_RATE / 4096; t++) {
          synth_render(&syn, buf, 4096);
          for (i = 0; i < 4096; i++) if (buf[i] > 1.0f || buf[i] < -1.0f) ok = false;
      }
      check(ok && syn.music.sweepPos == 0, "synth: the sweep stays clean and ends"); }

    /* the crusher slam thuds */
    { Synth syn; float buf[512]; int i; bool audible = false;
      synth_init(&syn);
      synth_trigger(&syn, SFX_SLAM);
      synth_render(&syn, buf, 512);
      for (i = 0; i < 512; i++) if (buf[i] > 0.01f || buf[i] < -0.01f) audible = true;
      check(audible, "synth: the crusher slam thuds"); }

    scores_selftest();   /* leaderboard ordering/dedupe (defined by the score table) */

    printf(fails ? "\nSELFTEST FAILED (%d)\n" : "\nSELFTEST OK\n", fails);
    return fails ? 1 : 0;
}

/* ============================================================= *
 *  Game shell: title / game-over / name entry + high scores.
 *  DESIGN.md §6.1 state machine and §6.5 high-score table.
 * ============================================================= */
/* The leaderboard ranks finished RUNS by who dove deepest dying least:
 * depth DESC, deaths ASC. 20 places, one row per name (the player is "YOU",
 * bots use their name label) — a returning name must beat its own best.
 * The on-disk format changed with this scheme; an old-format file fails the
 * fread size check below and the board quietly resets to defaults. */
#define NHISCORE 20
typedef struct { char name[12]; int depth; int deaths; } HiScore;
static HiScore g_scores[NHISCORE];
static const char *SCORE_FILE = "undermine_scores.dat";

typedef enum { SCR_TITLE = 0, SCR_PLAY, SCR_OVER, SCR_INFO } Screen;

static void scores_default(void)
{
    static const char *nm[NHISCORE] = {
        "MOLE","DIGGER","COALFACE","PITBOSS","ORESON","TINNIT","GEMMA","MUDLARK",
        "SHAFTY","RUSTPICK","LAMPLIT","CANARY","DUSTUP","HEWER","GRUBBER","SOOTY",
        "PEBBLE","CLOD","SLAGHEAP","NUGGET"};
    int i;
    for (i = 0; i < NHISCORE; i++) {
        snprintf(g_scores[i].name, sizeof g_scores[i].name, "%s", nm[i]);
        g_scores[i].depth  = (NHISCORE - i) / 2 + 1;   /* 11 down to 1 */
        g_scores[i].deaths = i / 3;                    /* 0 up to 6 */
    }
}

/* The score file is untrusted input: a corrupt or hand-edited one can carry
 * unterminated names (DrawText would read past the 12-byte array) or absurd
 * numbers. Terminate, stop names at the first non-printable byte, and clamp
 * the fields to sane play ranges. */
static void scores_sanitize(void)
{
    int i;
    for (i = 0; i < NHISCORE; i++) {
        char *nm = g_scores[i].name; int j;
        nm[sizeof g_scores[i].name - 1] = '\0';
        for (j = 0; nm[j]; j++)
            if (nm[j] < 32 || nm[j] > 126) { nm[j] = '\0'; break; }
        if (!nm[0]) snprintf(nm, sizeof g_scores[i].name, "???");
        if (g_scores[i].depth  < 0)     g_scores[i].depth  = 0;
        if (g_scores[i].depth  > 9999)  g_scores[i].depth  = 9999;
        if (g_scores[i].deaths < 0)     g_scores[i].deaths = 0;
        if (g_scores[i].deaths > 30000) g_scores[i].deaths = 30000;
    }
}

static void scores_load(void)
{
    FILE *f = fopen(SCORE_FILE, "rb");
    if (!f) { scores_default(); return; }
    if (fread(g_scores, sizeof(HiScore), NHISCORE, f) != NHISCORE) scores_default();
    fclose(f);
    scores_sanitize();
}

static void scores_save(void)
{
    FILE *f = fopen(SCORE_FILE, "wb");
    if (f) { fwrite(g_scores, sizeof(HiScore), NHISCORE, f); fclose(f); }
}

/* Is run (d1 deep, k1 deaths) a better dive than (d2, k2)? */
static int scores_better(int d1, int k1, int d2, int k2)
{
    return d1 > d2 || (d1 == d2 && k1 < k2);
}

/* Submit a finished run. Keeps the board sorted (deepest first, fewest
 * deaths breaking ties) and one row per name: a returning name must beat
 * its own best to move. Returns the rank it landed at, or -1. */
static int scores_submit(const char *name, int depth, int deaths)
{
    int i, at = -1, old = -1;
    if (!name || !name[0]) return -1;
    for (i = 0; i < NHISCORE; i++)
        if (strncmp(g_scores[i].name, name, sizeof g_scores[i].name - 1) == 0) { old = i; break; }
    if (old >= 0) {
        if (!scores_better(depth, deaths, g_scores[old].depth, g_scores[old].deaths))
            return -1;                              /* didn't beat its own best */
        for (i = old; i < NHISCORE - 1; i++)        /* drop the old row */
            g_scores[i] = g_scores[i + 1];
        g_scores[NHISCORE - 1].name[0] = '\0';      /* vacated tail slot */
        g_scores[NHISCORE - 1].depth = 0;
        g_scores[NHISCORE - 1].deaths = 30000;
    }
    for (i = 0; i < NHISCORE; i++)
        if (scores_better(depth, deaths, g_scores[i].depth, g_scores[i].deaths)) { at = i; break; }
    if (at < 0) return -1;
    for (i = NHISCORE - 1; i > at; i--) g_scores[i] = g_scores[i - 1];
    snprintf(g_scores[at].name, sizeof g_scores[at].name, "%s", name);
    g_scores[at].depth = depth;
    g_scores[at].deaths = deaths;
    return at;
}

/* leaderboard invariants: deepest-first / fewest-deaths order, one row per
 * name keeping only its best run (called from selftest; touches no file) */
static void scores_selftest(void)
{
    int r1, r2, r3, i, rows = 0, ok = 1;
    scores_default();
    r1 = scores_submit("ALPHA", 30, 5);
    r2 = scores_submit("BRAVO", 30, 2);
    r3 = scores_submit("ALPHA", 29, 0);              /* shallower: must not move */
    check(r1 == 0 && r2 == 0, "scores: deeper dives take the top of the board");
    check(strcmp(g_scores[0].name, "BRAVO") == 0 && strcmp(g_scores[1].name, "ALPHA") == 0,
          "scores: at equal depth, fewer deaths ranks higher");
    check(r3 < 0 && g_scores[1].depth == 30,
          "scores: a shallower run never displaces the same name's best");
    scores_submit("ALPHA", 31, 9);                   /* deeper: replaces its old row */
    for (i = 0; i < NHISCORE; i++) if (strcmp(g_scores[i].name, "ALPHA") == 0) rows++;
    check(rows == 1 && strcmp(g_scores[0].name, "ALPHA") == 0,
          "scores: one row per name, best run kept");
    for (i = 1; i < NHISCORE; i++)
        if (scores_better(g_scores[i].depth, g_scores[i].deaths,
                          g_scores[i - 1].depth, g_scores[i - 1].deaths)) ok = 0;
    check(ok, "scores: the whole board stays sorted");
    /* sanitize — the loader must defuse a hostile file: an unterminated
     * name, embedded control bytes, and out-of-range numbers. */
    memset(g_scores[0].name, 'A', sizeof g_scores[0].name);   /* no NUL at all */
    g_scores[0].depth = -7; g_scores[0].deaths = 1000000;
    snprintf(g_scores[1].name, sizeof g_scores[1].name, "OK");
    g_scores[1].name[1] = (char)0x81;                         /* junk mid-name */
    g_scores[1].depth = 123456; g_scores[1].deaths = -3;
    scores_sanitize();
    check(strlen(g_scores[0].name) == sizeof g_scores[0].name - 1 &&
          g_scores[0].depth == 0 && g_scores[0].deaths == 30000,
          "scores: sanitize terminates names and clamps the numbers");
    check(strcmp(g_scores[1].name, "O") == 0 &&
          g_scores[1].depth == 9999 && g_scores[1].deaths == 0,
          "scores: sanitize cuts a name at the first junk byte");
    scores_default();                                /* leave no test residue */
}

static void ctext(const char *t, int y, int size, Color c)
{
    DrawText(t, (SCREEN_W - MeasureText(t, size)) / 2, y, size, c);
}

/* Post-frame theater for one screen (§6.1 / §8-M5). Render-only — the sim is
 * never paused, so lock-step, determinism and the demo arena are untouched:
 *  - shaft-drop reveal: on a descent the new room is unveiled top-down under
 *    a black curtain, as if dropping in through the entry shaft;
 *  - palette flash on death: the screen strobes white then red for the first
 *    dying ticks (the C64 move), then the sprite's own death anim plays out. */
#define WIPE_FRAMES 40   /* ~0.65 s at 60 fps */
static void draw_screen_fx(const GameState *g, int *wipe, int *lastDepth)
{
    if (g->depth == *lastDepth + 1) *wipe = WIPE_FRAMES;   /* just descended */
    *lastDepth = g->depth;
    if (*wipe > 0) {
        int k = 8 + ((SCREEN_H - 8) * (WIPE_FRAMES - *wipe)) / WIPE_FRAMES;
        DrawRectangle(0, k, SCREEN_W, SCREEN_H - k, C64[BLACK_]);
        if (k <= 112) ctext(TextFormat("DEPTH %d", g->depth), 120, 8, C64[YELLOW_]);
        (*wipe)--;
    }
    if (g->state == PS_DYING) {
        int el = DYING_TICKS - g->dyingTimer;
        if (el < 8 && (el & 2) == 0)
            DrawRectangle(0, 8, SCREEN_W, SCREEN_H - 8, C64[el < 4 ? WHITE_ : RED_]);
    }
}

static void draw_scoreboard(int y)
{
    int i;
    ctext("DEEPEST DIVES      depth/deaths", y, 8, C64[YELLOW_]);
    for (i = 0; i < NHISCORE; i++) {                 /* two columns of ten */
        int x  = (i < 10) ? 10 : 168;
        int ry = y + 12 + (i % 10) * 8;
        if (!g_scores[i].name[0]) continue;
        DrawText(TextFormat("%2d", i + 1), x, ry, 8, C64[GREY_]);
        DrawText(g_scores[i].name, x + 20, ry, 8, C64[LTGREY_]);
        DrawText(TextFormat("%d/%d", g_scores[i].depth, g_scores[i].deaths),
                 x + 110, ry, 8, C64[LTBLUE_]);
    }
}

static void draw_title(long t, int demoSecs)
{
    ClearBackground(C64[BLUE_]);
    DrawRectangle(0, SCREEN_H - 12, SCREEN_W, 12, C64[ORANGE_]);   /* ground strip */
    ctext("U N D E R M I N E", 22, 20, C64[YELLOW_]);
    ctext("a coal-mine descent", 48, 8, C64[LTGREY_]);
    DrawText("V" GAME_VERSION, SCREEN_W - 32, 4, 8, C64[GREY_]);
    draw_scoreboard(60);
    if (g_seedEntry) {
        /* hex seed entry: what's typed so far, with a blinking cursor */
        ctext(TextFormat("SEED? %s%s", g_seedBuf, ((t / 15) % 2 == 0) ? "_" : " "),
              158, 8, C64[WHITE_]);
        ctext("ENTER DIG   ESC CANCEL", 170, 8, C64[LTGREEN_]);
    } else {
        if ((t / 25) % 2 == 0) ctext("PRESS SPACE TO DIG", 158, 8, C64[WHITE_]);
        ctext(TextFormat("D DAILY  S SEED  F1 INFO  M SOUND:%s",
                         g_soundOff ? "OFF" : "ON"), 170, 8, C64[LTGREEN_]);
    }
    /* F2/F3 load a scripted .mole bot into a screen; once loaded, the line names
     * the script and its author. Empty = the built-in bot plays that screen. */
    ctext(g_mole1 ? TextFormat("F2 %s BY %s", g_mole1Name, g_mole1Auth) : "F2 LOAD A BOT", 186, 8, C64[LTBLUE_]);
    ctext(g_mole2 ? TextFormat("F3 %s BY %s", g_mole2Name, g_mole2Auth) : "F3 LOAD A BOT", 196, 8, C64[LTBLUE_]);
    if (t < g_loadMsgUntil) ctext(g_loadMsg, 212, 8, C64[YELLOW_]);
    else if (demoSecs > 0) ctext(TextFormat("or watch the demo  %d", demoSecs), 212, 8, C64[GREY_]);
    else ctext("or watch the demo", 212, 8, C64[GREY_]);
}

/* F1 help / about screen. */
static void draw_info(long t)
{
    int y = 14;
    ClearBackground(C64[BLACK_]);
    ctext("UNDERMINE V" GAME_VERSION, y, 16, C64[YELLOW_]); y += 24;
    ctext("Dig an endless C64-style coal mine:", y, 8, C64[LTGREY_]); y += 10;
    ctext("grab every coal lump, then drop into", y, 8, C64[LTGREY_]); y += 10;
    ctext("the exit shaft to descend a level.", y, 8, C64[LTGREY_]); y += 18;
    ctext("CONTROLS", y, 8, C64[LTGREEN_]); y += 12;
    ctext("ARROWS / WASD   move & climb", y, 8, C64[WHITE_]); y += 10;
    ctext("SPACE / J       jump", y, 8, C64[WHITE_]); y += 10;
    ctext("P               pause", y, 8, C64[WHITE_]); y += 10;
    ctext("M               sound on/off (title)", y, 8, C64[WHITE_]); y += 10;
    ctext("ESC             quit to title / game", y, 8, C64[WHITE_]); y += 18;
    ctext("TWO SCREENS", y, 8, C64[LTGREEN_]); y += 12;
    ctext("You play the left; a bot plays the", y, 8, C64[WHITE_]); y += 10;
    ctext("right on identical rooms. The demo", y, 8, C64[WHITE_]); y += 10;
    ctext("races two bots head to head.", y, 8, C64[WHITE_]); y += 18;
    ctext("SCRIPT YOUR OWN BOT (.mole)", y, 8, C64[LTGREEN_]); y += 12;
    ctext("Load one with F2 / F3 on the title.", y, 8, C64[WHITE_]); y += 10;
    ctext("Reference: MOLESCRIPT.md", y, 8, C64[WHITE_]);
    if ((t / 25) % 2 == 0) ctext("PRESS ANY KEY TO RETURN", SCREEN_H - 14, 8, C64[GREY_]);
}

static void draw_gameover(const GameState *s, long t)
{
    ClearBackground(C64[BLACK_]);
    ctext("GAME OVER", 54, 20, C64[RED_]);
    ctext(TextFormat("SCORE %06ld", s->score), 88, 8, C64[WHITE_]);
    ctext(TextFormat("REACHED DEPTH %d", s->depth), 102, 8, C64[LTBLUE_]);
    ctext(TextFormat("DEATHS %d", s->totalDeaths), 116, 8, C64[LTRED_]);
    if ((t / 25) % 2 == 0) ctext("PRESS SPACE", 144, 8, C64[LTGREY_]);
}

static void start_run(GameState *s, uint64_t seed, double *acc)
{
    sim_init_seed(s, seed);
    *acc = 0;
    synth_set_pitch(&g_synth, 1.0f);
    synth_music_play(&g_synth, SONG_INGAME);
}

int main(int argc, char **argv)
{
    const char *shotPath = NULL;
    GameState s;
    double acc = 0;
    const double DT = 1.0 / TICK_RATE;
    RenderTexture2D targetL, targetR;   /* one per screen; window shows them side by side */
    AudioStream audio;

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0) return selftest();
    if (argc > 1 && strcmp(argv[1], "--botdebug") == 0) {
        int si;
        for (si = 0; si < 60; si++) {           /* find & dump the first failure */
            uint64_t seed = 0x5EEDULL + (uint64_t)si * 0x1234ULL;
            if (!bot_clear_room(seed, 1)) {
                printf("first failing depth-1 seed: index %d\n", si);
                g_botdebug = 1;
                printf("result: %s\n", bot_clear_room(seed, 1) ? "OK" : "FAIL");
                return 0;
            }
        }
        printf("all depth-1 seeds cleared\n");
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--probelad") == 0) {
        /* Bot-author debug tool: run a bot in a crafted chase — the bot
         * starts on a ladder TOP with a foreman climbing up at it, cornered
         * against a platform edge. Takes a .mole path, or "builtin1" /
         * "builtin2" for the built-in bots. Ends with a verdict. */
        char err[160]; MoleVM *vm = NULL;
        Bot1Nav n1; Bot2Nav n2;
        int b1 = strcmp(argv[2], "builtin1") == 0, b2 = strcmp(argv[2], "builtin2") == 0;
        GameState g; int i, c2, r2;
        int edge = (argc > 3 && argv[3][0] == 'e');   /* "edge": ladder at the platform edge */
        int mid  = (argc > 3 && argv[3][0] == 'm');   /* "mid": bot starts halfway DOWN the ladder */
        int lc = edge ? 2 : 6;                        /* ladder column */
        if (!b1 && !b2) {
            vm = mole_load_file(argv[2], 1, err, sizeof err);
            if (!vm) { printf("load error: %s\n", err); return 1; }
        }
        memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
        for (c2 = 2; c2 <= 10; c2++) g.tiles[6][c2] = TILE_SOLID;   /* upper platform */
        g.tiles[6][lc] = TILE_LADDER;                               /* ladder hole in it */
        for (r2 = 7; r2 <= 9; r2++) g.tiles[r2][lc] = TILE_LADDER;  /* run down to floor */
        for (c2 = 2; c2 <= 10; c2++) g.tiles[10][c2] = TILE_SOLID;  /* lower floor */
        g.coal[5][3] = true; g.coalTotal = 1; g.exitCol = 15;
        if (mid) {                                    /* halfway down the run, hanging on */
            g.px = (float)(lc * TILE + 3); g.py = (float)(9 * TILE - HITBOX_H);
            g.onGround = false; g.onLadder = true;
        } else {                                      /* standing on the ladder top */
            g.px = (float)(lc * TILE + 3); g.py = (float)(6 * TILE - HITBOX_H);
            g.onGround = true;
        }
        g.facing = 1;
        g.enemyCount = 1;
        if (edge) {                                   /* foreman patrols the platform at us */
            Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN;
            e->baseX = (float)(8 * TILE); e->baseY = (float)(6 * TILE - HITBOX_H);
            e->x = e->baseX; e->y = e->baseY; e->dir = -1; e->range = 5 * TILE;
        } else {                                      /* foreman climbs the ladder at us */
            Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_FOREMAN;
            e->x = (float)(lc * TILE + 3);
            e->y = (float)((mid ? 10 : 9) * TILE - HITBOX_H);  /* mid: start at the floor */
            e->mode = 1; e->vdir = -1; e->dir = 1;
        }
        g.enemyStart[0] = g.enemies[0];
        if (vm) mole_reset(vm, 1);
        bot1_nav_init(&n1); bot2_nav_init(&n2);
        { int lastHd = 0, reversals = 0;
        for (i = 0; i < 500; i++) {
            Input in = b1 ? bot1_input(&g, &n1)
                     : b2 ? bot2_input(&g, &n2)
                     :      mole_input(vm, &g);
            int hd = in.right ? 1 : (in.left ? -1 : 0);   /* count grounded L/R flips = vibration */
            if (hd && lastHd && hd == -lastHd && g.onGround) reversals++;
            if (hd) lastHd = hd;
            if (i % 10 == 0)
                printf("t%03d p(%.0f,%.0f) grnd=%d lad=%d foe(%.0f,%.0f) in[L%d R%d U%d D%d J%d] say=%.48s\n",
                       i, g.px, g.py, g.onGround, g.onLadder,
                       g.enemies[0].x, g.enemies[0].y,
                       in.left, in.right, in.up, in.down, in.jump,
                       vm ? mole_say(vm) : (b1 ? n1.say : n2.say));
            sim_tick(&g, in);
            if (g.state != PS_ALIVE) { printf("result: DIED at t%03d (reversals=%d)\n", i, reversals); break; }
        }
        if (g.state == PS_ALIVE) printf("result: SURVIVED the chase (500 ticks, reversals=%d)\n", reversals); }
        if (vm) mole_free(vm);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--probegap") == 0) {
        /* Bot-author debug tool: run a .mole in a crafted room where the only
         * coal sits across a one-tile gap — the bot must route to the lip and
         * hop it (go_to alone reports blocked there). Ends with a verdict. */
        char err[160]; MoleVM *vm = mole_load_file(argv[2], 1, err, sizeof err);
        GameState g; int i, c2; bool crossed = false, jumped = false;
        int cru = (argc > 3 && argv[3][0] == 'c');   /* "crusher": one hangs over the landing */
        if (!vm) { printf("load error: %s\n", err); return 1; }
        memset(&g, 0, sizeof g); g.state = PS_ALIVE; g.lives = 9; g.ridingPlat = -1;
        for (c2 = 0; c2 < ROOM_W; c2++) if (c2 != 9) g.tiles[9][c2] = TILE_SOLID;  /* gap at col 9 */
        g.coal[8][12] = true; g.coalTotal = 1; g.exitCol = 15;
        g.px = (float)(5 * TILE); g.py = (float)(9 * TILE - HITBOX_H);
        g.onGround = true; g.facing = 1;
        if (cru) {   /* crusher over the far lip: a correct bot REFUSES the hop */
            Enemy *e = &g.enemies[0]; memset(e, 0, sizeof *e); e->type = EN_CRUSHER;
            e->baseX = (float)(10 * TILE); e->baseY = (float)(4 * TILE);
            e->x = e->baseX; e->y = e->baseY; e->range = 4 * TILE; e->period = 120;
            g.enemyCount = 1; g.enemyStart[0] = g.enemies[0];
        }
        mole_reset(vm, 1);
        for (i = 0; i < 600 && g.state == PS_ALIVE && g.coalGot == 0; i++) {
            Input in = mole_input(vm, &g);
            if (in.jump) jumped = true;
            if (i % 25 == 0)
                printf("t%03d p(%.0f,%.0f) in[L%d R%d J%d] say=%s\n", i, g.px, g.py,
                       in.left, in.right, in.jump, mole_say(vm));
            sim_tick(&g, in);
            if (g.px > 10 * TILE) crossed = true;
        }
        printf("result: jumped=%d crossed=%d coal=%d state=%d\n",
               jumped, crossed, g.coalGot, g.state);
        mole_free(vm);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--probekey") == 0) {
        /* Bot-author debug tool: drop the .mole into a real generated KEY room
         * (depth 10) — the exit stays shut until the key is fetched. Verdict:
         * did it take the key and descend? */
        char err[160]; MoleVM *vm = mole_load_file(argv[2], 1, err, sizeof err);
        GameState g; int i, gotKey = 0; uint64_t seed = 0;
        int skip = (argc > 3) ? atoi(argv[3]) : 0;   /* pick the Nth key room found */
        char lastSay[80] = "";
        if (!vm) { printf("load error: %s\n", err); return 1; }
        for (i = 0; i < 400; i++) {          /* find a seed whose depth 10 is a key room */
            uint64_t sd = 0x1000ULL + (uint64_t)i * 0x9E37ULL;
            memset(&g, 0, sizeof g);
            sim_gen_room(&g, sd, 10, sim_entry_col_for(sd, 10));
            if (g.keyRoom && g.keyCol >= 0 && skip-- == 0) { seed = sd; break; }
        }
        if (!seed) { printf("no key room found\n"); return 1; }
        sim_init_seed(&g, seed); g.lives = 99; sim_sync_room(&g, 10);
        mole_reset(vm, seed);
        printf("key room: seed=%llX key at (%d,%d)\n",
               (unsigned long long)seed, g.keyCol, g.keyRow);
        for (i = 0; i < 6000 && g.depth == 10; i++) {
            sim_tick(&g, mole_input(vm, &g));
            if (g.keyGot) gotKey = 1;
            if (strcmp(mole_say(vm), lastSay) != 0) {
                snprintf(lastSay, sizeof lastSay, "%s", mole_say(vm));
                if (lastSay[0]) printf("  [t%04d] %.60s\n", i, lastSay);
            }
        }
        printf("result: keyGot=%d descended=%d (t%d)\n", gotKey, g.depth > 10, i);
        mole_free(vm);
        return 0;
    }
    if (argc > 1 && strcmp(argv[1], "--demodebug") == 0) {
        /* Simulate a realistic attract-mode session with the watchdog reseed, so
         * the numbers reflect what a viewer actually sees. 60000 ticks = 20 min. */
        GameState g; Bot1Nav n1; Bot2Nav n2; int i, prevState = PS_ALIVE, maxDepth = 1;
        int tEnemy = 0, tFall = 0, tOther = 0, descents = 0, reseeds = 0, hops = 0;
        int lastDepth = 1, lastCoal = 0, lastPick = 0, progressT = 0; float fallStart = 0;
        int b2 = (argc > 2 && argv[2][0] == '2');    /* "--demodebug 2" measures bot2 */
        uint64_t sq = 0;
        sim_init_seed(&g, 0x5EEDULL); g.lives = 3; bot1_nav_init(&n1); bot2_nav_init(&n2);
        for (i = 0; i < 60000; i++) {
            int wasGround = g.onGround; float py0 = g.py;
            Input in = b2 ? bot2_input(&g, &n2) : bot1_input(&g, &n1);
            if (in.jump && g.onGround) hops++;
            sim_tick(&g, in);
            if (wasGround && !g.onGround) fallStart = py0;
            if (g.depth > lastDepth) { descents++; if (g.depth > maxDepth) maxDepth = g.depth; }
            {   /* progress = depth / coal / a pickup (lamp or key fetch counts too) */
                int pick = (g.lampGot ? 1 : 0) | (g.keyGot ? 2 : 0);
                if (g.depth != lastDepth || g.coalGot != lastCoal || pick != lastPick) {
                    lastDepth = g.depth; lastCoal = g.coalGot; lastPick = pick; progressT = i;
                }
            }
            if (g.state == PS_DYING && prevState == PS_ALIVE) {
                if (sim_enemy_threat(&g, g.px, g.py, 1.0f)) tEnemy++;
                else if (g.py - fallStart > 60.0f) tFall++;
                else tOther++;
            }
            prevState = g.state;
            if (g.state == PS_GAMEOVER || i - progressT > 300) {   /* watchdog reseed */
                sim_init_seed(&g, 0x5EEDULL + (sq += 0x9E37)); g.lives = 3;
                bot1_nav_init(&n1); bot2_nav_init(&n2);
                lastDepth = 1; lastCoal = 0; lastPick = 0; progressT = i; prevState = PS_ALIVE;
                reseeds++;
            }
        }
        printf("20-min attract session (%s): descents=%d maxDepth=%d reseeds=%d hops=%d\n",
               b2 ? "bot2" : "bot1", descents, maxDepth, reseeds, hops);
        printf("deaths: enemy=%d fall=%d other=%d\n", tEnemy, tFall, tOther);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--moletest") == 0) {
        /* headless smoke test for a .mole bot: run it across 20 seeds and report
         * how many rooms it clears, so authors can iterate without the window. */
        char err[160];
        MoleVM *vm = mole_load_file(argv[2], 0x1234ULL, err, sizeof err);
        int si, descents = 0, maxDepth = 1;
        if (!vm) { printf("load error: %s\n", err); return 1; }
        for (si = 0; si < 20; si++) {
            GameState g; int i, lastDepth;
            char lastSay[80] = "";
            uint64_t seed = 0x5EEDULL + (uint64_t)si * 0x1234ULL;
            sim_init_seed(&g, seed); g.lives = 200; mole_reset(vm, seed);
            lastDepth = g.depth;
            for (i = 0; i < 6000; i++) {
                sim_tick(&g, mole_input(vm, &g));
                /* introspection: echo the script's say line whenever it changes
                 * (first seed only, so 20 seeds don't flood the console) */
                if (si == 0 && strcmp(mole_say(vm), lastSay) != 0) {
                    snprintf(lastSay, sizeof lastSay, "%s", mole_say(vm));
                    if (lastSay[0]) printf("  [d%d t%04d] %s\n", g.depth, i, lastSay);
                }
                if (g.depth > lastDepth) { descents++; lastDepth = g.depth;
                                           if (g.depth > maxDepth) maxDepth = g.depth; }
            }
        }
        printf("moletest %s: descents=%d maxDepth=%d over 20 seeds\n",
               argv[2], descents, maxDepth);
        mole_free(vm);
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "--shot") == 0) shotPath = argv[2];

    /* --bot1 FILE / --bot2 FILE: load a MoleScript bot for that screen. A load
     * error is reported and the slot falls back to the built-in C bot. */
    {
        int ai;
        for (ai = 1; ai < argc - 1; ai++) {
            char err[160];
            if (strcmp(argv[ai], "--bot1") == 0) {
                g_mole1 = mole_load_file(argv[ai + 1], DEFAULT_SEED, err, sizeof err);
                if (!g_mole1) fprintf(stderr, "bot1 '%s': %s (using default bot)\n", argv[ai + 1], err);
                else { printf("bot1: loaded %s (author: %s)\n", argv[ai + 1], mole_author(g_mole1));
                       set_bot_file_label(g_mole1Name, argv[ai + 1]);
                       set_bot_label(g_mole1Auth, mole_author(g_mole1)); }
            } else if (strcmp(argv[ai], "--bot2") == 0) {
                g_mole2 = mole_load_file(argv[ai + 1], DEFAULT_SEED, err, sizeof err);
                if (!g_mole2) fprintf(stderr, "bot2 '%s': %s (using default bot)\n", argv[ai + 1], err);
                else { printf("bot2: loaded %s (author: %s)\n", argv[ai + 1], mole_author(g_mole2));
                       set_bot_file_label(g_mole2Name, argv[ai + 1]);
                       set_bot_label(g_mole2Auth, mole_author(g_mole2)); }
            }
        }
    }

    sim_init(&s);
    scores_load();
    if (shotPath && argc > 3) {          /* --shot FILE DEPTH|SCREEN [SEED]: QA render */
        int d = atoi(argv[3]);
        uint64_t seed = (argc > 4) ? strtoull(argv[4], NULL, 0) : s.runSeed;
        if (d >= 1) sim_gen_room(&s, seed, d, sim_entry_col_for(seed, d));
    }

    SetConfigFlags(FLAG_VSYNC_HINT);
    InitWindow(2 * SCREEN_W * WINDOW_SCALE, SCREEN_H * WINDOW_SCALE,
               "UNDERMINE v" GAME_VERSION);
    SetExitKey(KEY_NULL);   /* ESC is ours: in-game -> title, title -> quit */
    targetL = LoadRenderTexture(SCREEN_W, SCREEN_H);
    targetR = LoadRenderTexture(SCREEN_W, SCREEN_H);
    SetTextureFilter(targetL.texture, TEXTURE_FILTER_POINT);
    SetTextureFilter(targetR.texture, TEXTURE_FILTER_POINT);

    if (shotPath) {                     /* render one frame, save, exit */
        const char *what = (argc > 3) ? argv[3] : "";
        Image img;
        int i;
        for (i = 0; i < 25; i++) sim_tick(&s, (Input){0});
        BeginTextureMode(targetL);
        if      (strcmp(what, "title") == 0) draw_title(0, -1);
        else if (strcmp(what, "over")  == 0) { s.score = 12345; s.depth = 17;
                                               s.totalDeaths = 3;
                                               draw_gameover(&s, 0); }
        else if (strcmp(what, "info")  == 0) draw_info(0);
        else draw_frame(&s);
        EndTextureMode();
        img = LoadImageFromTexture(targetL.texture);
        ImageFlipVertical(&img);
        ExportImage(img, shotPath);
        CloseWindow();
        return 0;
    }

    synth_init(&g_synth);
    InitAudioDevice();
    audio = LoadAudioStream(SYNTH_RATE, 32, 1);   /* mono 32-bit float */
    SetAudioStreamCallback(audio, audio_cb);
    PlayAudioStream(audio);
    synth_music_play(&g_synth, SONG_TITLE);
    scores_load();

    {
        /* Two games run in lock-step on ONE shared seed, so both screens face
         * identical rooms. Left = bot1 (demo) or the human; right = bot2 always. */
        Screen screen = SCR_TITLE;
        long uiTicks = 0, titleIdle = 0, demoSeq = 0;
        bool paused = false, demo = false, quit = false;
        uint64_t heatSeed = 0;
        int anchLc = 0, anchLr = 0, anchRc = 0, anchRr = 0;  /* confinement anchor cell per bot */
        long moveLT = 0, moveRT = 0;                     /* last tick each bot actually moved */
        int seenDepthL = 1, seenDepthR = 1;
        int wipeL = 0, wipeR = 0, fxDepthL = -1, fxDepthR = -1;  /* shaft-drop reveal state */
        long oxygenTicks = 0;                            /* sim ticks since a bot last reached the exit */
        const long STUCK = 500;                          /* motionless this long (~10s) = stuck */
        const long OXYGEN_MAX = TICK_RATE * 150;         /* 2.5 min of air (sim is a fixed 50 Hz) */
        const long DEMO_IDLE = 600;                      /* title idle before the demo auto-starts */
        GameState gL, gR;
        Bot1Nav nav1; Bot2Nav nav2;
        sim_init(&gL); sim_init(&gR);
        bot1_nav_init(&nav1); bot2_nav_init(&nav2);
        MOLE_RESET1(gL.runSeed); MOLE_RESET2(gR.runSeed);

        while (!quit && !WindowShouldClose()) {
            uiTicks++;

            if (screen == SCR_PLAY) {
                if (demo && GetKeyPressed() != 0) {         /* any key leaves the demo */
                    /* the bots' runs end here: put them on the board */
                    scores_submit(g_mole1Name, gL.depth, gL.totalDeaths);
                    scores_submit(g_mole2Name, gR.depth, gR.totalDeaths);
                    scores_save();
                    screen = SCR_TITLE; titleIdle = 0; demo = false;
                    synth_music_play(&g_synth, SONG_TITLE);
                }
                if (!demo && IsKeyPressed(KEY_P))
                    paused = !paused;
                if (!demo && IsKeyPressed(KEY_ESCAPE)) {    /* abandon the run */
                    screen = SCR_TITLE; titleIdle = 0; paused = false;
                    synth_set_pitch(&g_synth, 1.0f);
                    synth_music_play(&g_synth, SONG_TITLE);
                }
                if (screen == SCR_PLAY && !paused) {
                    double ft = GetFrameTime();
                    Input frameIn = read_input();
                    if (ft > 0.25) ft = 0.25;       /* debugger / hitch guard */
                    acc += ft;
                    while (acc >= DT) {             /* fixed 50 Hz sim, both screens */
                        GameState prevL = gL;
                        Input inL = demo ? (g_mole1 ? mole_input(g_mole1, &gL)
                                                    : bot1_input(&gL, &nav1))
                                         : frameIn;
                        Input inR = g_mole2 ? mole_input(g_mole2, &gR)
                                            : bot2_input(&gR, &nav2);
                        sim_tick(&gL, inL);
                        sim_tick(&gR, inR);
                        if (demo) say_log_tick(0, g_mole1 ? mole_say(g_mole1) : nav1.say, &gL);
                        say_log_tick(1, g_mole2 ? mole_say(g_mole2) : nav2.say, &gR);
                        audio_events(&prevL, &gL);   /* audio follows the left screen */
                        if (gL.depth != prevL.depth) {
                            synth_set_pitch(&g_synth,
                                powf(2.0f, -(float)((gL.depth - 1) / 10) / 12.0f));
                            /* crossing into a new 10-depth band: transpose down
                             * a semitone AND dip the filter — descending dread */
                            if (gL.depth > prevL.depth &&
                                (gL.depth - 1) / 10 != (prevL.depth - 1) / 10)
                                synth_sweep(&g_synth);
                        }
                        /* Human play: if the player clears the room before the bot,
                         * pull the bot forward to the player's new room so both
                         * advance together — the player never waits for a slow bot.
                         * (When the bot clears first, it descends on its own.) */
                        if (!demo && gL.depth > prevL.depth && gR.depth < gL.depth) {
                            sim_sync_room(&gR, gL.depth);
                            bot2_nav_init(&nav2); MOLE_RESET2(heatSeed);
                        }
                        if (!demo) frameIn.jump = false;
                        oxygenTicks++;                  /* air burns down in real time (50 Hz) */
                        if (!demo && prevL.state == PS_ALIVE && gL.state != PS_ALIVE)
                            oxygenTicks = 0;            /* each new life starts with a full tank */
                        acc -= DT;
                    }
                }
                if (screen == SCR_PLAY) {
                    /* a bot counts as "moving" while its position changes — dying and
                     * respawning teleports it, so only a genuine freeze goes stale */
                    { int cc, cr, dc, dr;
                      bot_cell(&gL, &cc, &cr); dc = cc - anchLc; dr = cr - anchLr;
                      if (dc < 0) dc = -dc; if (dr < 0) dr = -dr;
                      if (dc >= 2 || dr >= 2) { anchLc = cc; anchLr = cr; moveLT = uiTicks; }
                      bot_cell(&gR, &cc, &cr); dc = cc - anchRc; dr = cr - anchRr;
                      if (dc < 0) dc = -dc; if (dr < 0) dr = -dr;
                      if (dc >= 2 || dr >= 2) { anchRc = cc; anchRr = cr; moveRT = uiTicks; } }
                    /* reaching an exit refills the air. In the demo either bot counts
                     * (one solving keeps the arena alive); in human play only the
                     * player's own descent tops up their tank. */
                    if (demo ? (gL.depth > seenDepthL || gR.depth > seenDepthR)
                             : (gL.depth > seenDepthL)) oxygenTicks = 0;
                    seenDepthL = gL.depth; seenDepthR = gR.depth;
                }
                if (screen == SCR_PLAY && demo) {
                    /* Let the arena run: the bots have endless lives (deaths just make
                     * them respawn and keep trying). Roll a fresh mine for both when the
                     * air runs out (2.5 min with neither bot reaching an exit), or as a
                     * fast backstop when both bots stay confined to a ~3x3 region for >10s
                     * — which catches a bot vibrating in place, not just a frozen one. */
                    bool bothStuck = (uiTicks - moveLT > STUCK) && (uiTicks - moveRT > STUCK);
                    bool outOfAir  = (oxygenTicks >= OXYGEN_MAX);
                    if (bothStuck || outOfAir) {
                        /* the bots' runs end with the arena: put them on the board */
                        scores_submit(g_mole1Name, gL.depth, gL.totalDeaths);
                        scores_submit(g_mole2Name, gR.depth, gR.totalDeaths);
                        scores_save();
                        heatSeed = seed_clip((uint64_t)time(NULL) * 0x9E3779B1ULL + (uint64_t)(++demoSeq));
                        sim_init_seed(&gL, heatSeed); sim_init_seed(&gR, heatSeed);
                        gL.lives = gR.lives = 99;
                        bot1_nav_init(&nav1); bot2_nav_init(&nav2);
                        MOLE_RESET1(heatSeed); MOLE_RESET2(heatSeed);
                        say_log_open(0);   /* a reseed is a fresh run */
                        say_log_open(1);
                        synth_set_pitch(&g_synth, 1.0f);
                        bot_cell(&gL, &anchLc, &anchLr); bot_cell(&gR, &anchRc, &anchRr);
                        seenDepthL = gL.depth; seenDepthR = gR.depth;
                        moveLT = moveRT = uiTicks; oxygenTicks = 0;
                    }
                } else if (screen == SCR_PLAY) {
                    /* human on the left: run out of air and you suffocate — lose a life
                     * and restart the room (a full tank comes with the new life). */
                    if (oxygenTicks >= OXYGEN_MAX) { sim_kill_player(&gL); oxygenTicks = 0; }
                    /* keep bot2 alive by replaying the same seed if it ever sits
                     * confined to a ~3x3 region for >10s (frozen or vibrating) */
                    if (uiTicks - moveRT > STUCK) {
                        sim_init_seed(&gR, heatSeed); gR.lives = 99; bot2_nav_init(&nav2);
                        MOLE_RESET2(heatSeed);
                        bot_cell(&gR, &anchRc, &anchRr); moveRT = uiTicks;
                    }
                    if (gL.state == PS_GAMEOVER) {   /* the human's run ended */
                        scores_submit("YOU", gL.depth, gL.totalDeaths);
                        scores_submit(g_mole2Name, gR.depth, gR.totalDeaths);
                        scores_save();
                        screen = SCR_OVER;
                        synth_set_pitch(&g_synth, 1.0f);
                        synth_music_play(&g_synth, SONG_OVER);   /* plays once, then quiet */
                    }
                }
            } else if (screen == SCR_TITLE) {
                uint64_t seed = 0; bool startDemo = false;
                titleIdle++;
                if (g_seedEntry) {
                    /* modal hex seed entry (S). Modal because loose hex typing
                     * would collide with the other title hotkeys (D is a hex
                     * digit AND the daily run). BACKSPACE erases (cancels when
                     * already empty), ESC cancels, ENTER digs: with the typed
                     * seed, or the usual datetime seed if nothing was typed. */
                    int ch;
                    titleIdle = 0;                     /* no demo while typing */
                    while ((ch = GetCharPressed()) != 0) {
                        if (ch >= 'a' && ch <= 'f') ch -= 'a' - 'A';
                        if (((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F')) &&
                            g_seedLen < SEED_DIGITS) {
                            g_seedBuf[g_seedLen++] = (char)ch;
                            g_seedBuf[g_seedLen] = '\0';
                        }
                    }
                    if (IsKeyPressed(KEY_ESCAPE)) {
                        g_seedEntry = false; g_seedLen = 0; g_seedBuf[0] = '\0';
                    } else if (IsKeyPressed(KEY_BACKSPACE)) {
                        if (g_seedLen > 0) g_seedBuf[--g_seedLen] = '\0';
                        else g_seedEntry = false;
                    } else if (IsKeyPressed(KEY_ENTER)) {
                        g_seedEntry = false;
                        seed = seed_clip(g_seedLen ? strtoull(g_seedBuf, NULL, 16)
                                                   : (uint64_t)time(NULL) * 0x9E3779B1ULL + (uint64_t)uiTicks);
                        g_seedLen = 0; g_seedBuf[0] = '\0';
                    }
                }
                else if (IsKeyPressed(KEY_F1)) { screen = SCR_INFO; titleIdle = 0; }
                else if (IsKeyPressed(KEY_F2)) { load_bot(1, uiTicks); titleIdle = 0; }
                else if (IsKeyPressed(KEY_F3)) { load_bot(2, uiTicks); titleIdle = 0; }
                else if (IsKeyPressed(KEY_S)) { g_seedEntry = true; g_seedLen = 0;
                                                g_seedBuf[0] = '\0'; titleIdle = 0; }
                else if (IsKeyPressed(KEY_M)) { g_soundOff = !g_soundOff; titleIdle = 0; }
                else if (IsKeyPressed(KEY_ESCAPE)) quit = true;   /* ESC on the title quits */
                else if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_J))
                    seed = seed_clip((uint64_t)time(NULL) * 0x9E3779B1ULL + (uint64_t)uiTicks);
                else if (IsKeyPressed(KEY_D))          /* daily: DAxxxx, xxxx = day count */
                    seed = seed_clip(0xDA0000ULL | ((uint64_t)(time(NULL) / 86400) & 0xFFFFULL));
                else if (titleIdle > 600) {            /* ~10s idle: roll the demo */
                    seed = seed_clip((uint64_t)time(NULL) * 0x9E3779B1ULL + (uint64_t)(++demoSeq));
                    startDemo = true;
                }
                if (seed) {
                    heatSeed = seed;
                    start_run(&gL, heatSeed, &acc); start_run(&gR, heatSeed, &acc);
                    bot1_nav_init(&nav1); bot2_nav_init(&nav2);
                    MOLE_RESET1(heatSeed); MOLE_RESET2(heatSeed);
                    say_log_open(0);   /* fresh bot1.log / bot2.log per run */
                    say_log_open(1);
                    demo = startDemo; paused = false; screen = SCR_PLAY;
                    if (demo) gL.lives = 99;   /* endless demo: only stuck / out-of-air resets it */
                    gR.lives = 99;             /* the right-hand bot always plays endlessly */
                    bot_cell(&gL, &anchLc, &anchLr); bot_cell(&gR, &anchRc, &anchRr);
                    seenDepthL = gL.depth; seenDepthR = gR.depth;
                    moveLT = moveRT = uiTicks; oxygenTicks = 0;
                }
            } else if (screen == SCR_INFO) {
                if (GetKeyPressed() != 0) {
                    screen = SCR_TITLE; titleIdle = 0;
                    /* arriving via game over, the jingle has gone quiet */
                    if (!g_synth.music.on) synth_music_play(&g_synth, SONG_TITLE);
                }
            } else if (screen == SCR_OVER) {
                if (IsKeyPressed(KEY_F1)) { screen = SCR_INFO; }
                else if (IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ENTER)) {
                    screen = SCR_TITLE; titleIdle = 0;
                    synth_music_play(&g_synth, SONG_TITLE);
                }
            }

            /* ---- LEFT screen (bot1 / human) ---- */
            BeginTextureMode(targetL);
            switch (screen) {
            case SCR_TITLE: draw_title(uiTicks, (int)((DEMO_IDLE - titleIdle + 59) / 60)); break;
            case SCR_INFO:  draw_info(uiTicks); break;
            case SCR_OVER:  draw_gameover(&gL, uiTicks); break;
            case SCR_PLAY:
                draw_frame(&gL);
                if (paused) {
                    DrawRectangle(0, 8, SCREEN_W, SCREEN_H - 8, C64[BLACK_]);
                    ctext("PAUSED", 92, 16, C64[WHITE_]);
                    ctext("P: RESUME", 118, 8, C64[LTGREY_]);
                    ctext("ESC: QUIT TO TITLE", 130, 8, C64[GREY_]);
                }
                /* bot name at the left, its say line following on the same row */
                { const char *nm = demo ? g_mole1Name : "YOU";
                  const char *m1 = g_mole1 ? mole_say(g_mole1) : nav1.say;
                  DrawText(nm, 4, 10, 8, C64[LTGREEN_]);
                  if (demo && m1[0])
                      DrawText(m1, 4 + MeasureText(nm, 8) + 8, 10, 8, C64[LTGREY_]); }
                draw_screen_fx(&gL, &wipeL, &fxDepthL);
                if (demo) ctext("PRESS ANY KEY", SCREEN_H - 10, 8, C64[BLACK_]);
                break;
            }
            EndTextureMode();

            /* ---- RIGHT screen (bot2) ---- */
            BeginTextureMode(targetR);
            if (screen == SCR_TITLE) draw_title(uiTicks, (int)((DEMO_IDLE - titleIdle + 59) / 60));
            else if (screen == SCR_INFO) draw_info(uiTicks);
            else { draw_frame(&gR);
                   /* bot name at the left, its say line following on the same row */
                   { const char *m2 = g_mole2 ? mole_say(g_mole2) : nav2.say;
                     DrawText(g_mole2Name, 4, 10, 8, C64[LTBLUE_]);
                     if (m2[0])
                         DrawText(m2, 4 + MeasureText(g_mole2Name, 8) + 8, 10, 8, C64[LTGREY_]); }
                   draw_screen_fx(&gR, &wipeR, &fxDepthR);
                   /* the shared run seed, in hex — read it here to replay a run via S */
                   ctext(TextFormat("SEED %06llX", (unsigned long long)gR.runSeed),
                         SCREEN_H - 10, 8, C64[BLACK_]); }
            EndTextureMode();

            BeginDrawing();
            ClearBackground(C64[BLACK_]);
            DrawTexturePro(targetL.texture,
                (Rectangle){0, 0, (float)SCREEN_W, (float)-SCREEN_H},
                (Rectangle){0, 0, (float)(SCREEN_W * WINDOW_SCALE), (float)(SCREEN_H * WINDOW_SCALE)},
                (Vector2){0, 0}, 0.0f, WHITE);
            DrawTexturePro(targetR.texture,
                (Rectangle){0, 0, (float)SCREEN_W, (float)-SCREEN_H},
                (Rectangle){(float)(SCREEN_W * WINDOW_SCALE), 0,
                            (float)(SCREEN_W * WINDOW_SCALE), (float)(SCREEN_H * WINDOW_SCALE)},
                (Vector2){0, 0}, 0.0f, WHITE);
            DrawRectangle(SCREEN_W * WINDOW_SCALE - 1, 0, 2, SCREEN_H * WINDOW_SCALE, C64[GREY_]);
            /* Oxygen strip: a flat bar across the very bottom that drains over 2.5 min
             * and refills at an exit. In the demo, empty rolls a fresh arena; in human
             * play, empty suffocates the player (a lost life). */
            if (screen == SCR_PLAY) {
                int barW = 2 * SCREEN_W * WINDOW_SCALE;         /* full window width */
                int h    = 3 * WINDOW_SCALE / 2;                /* half the old height */
                int y    = SCREEN_H * WINDOW_SCALE - h;
                long left = OXYGEN_MAX - oxygenTicks; if (left < 0) left = 0;
                int fillW = (int)((long long)barW * left / OXYGEN_MAX);
                int ci = (left * 4 > OXYGEN_MAX) ? CYAN_ : (left * 8 > OXYGEN_MAX) ? YELLOW_ : RED_;
                DrawRectangle(0, y, barW, h, C64[DKGREY_]);     /* spent air */
                DrawRectangle(0, y, fillW, h, C64[ci]);         /* air remaining */
            }
            EndDrawing();
        }
    }

    UnloadAudioStream(audio);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
