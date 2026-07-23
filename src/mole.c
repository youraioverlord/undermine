/* MoleScript interpreter — see mole.h and MOLESCRIPT.md.
 *
 * Pipeline: source -> tokens -> bytecode (single pass, functions pre-scanned) ->
 * a small stack VM stepped once per tick. Control flow and sensors are
 * instantaneous; the seven ACTION opcodes (walk/climb/jump/drop/wait/idle/go_to)
 * consume ticks by suspending the VM at the action opcode and resuming there
 * next tick, so the whole program reads like it runs continuously.
 *
 * The go_to pathfinder is a private copy of bot2's drift-safe walk/climb/drop
 * BFS, so a script gets the same navigation the built-in bots use. */
#define _CRT_SECURE_NO_WARNINGS   /* strncpy/fopen are used deliberately here */
#include "mole.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <setjmp.h>

/* ---- value-space constants exposed to scripts (all just numbers) ---- */
enum { DIR_ANY = 0, DIR_LEFT = 1, DIR_RIGHT = 2, DIR_UP = 3, DIR_DOWN = 4,
       DIR_ABOVE = 5, DIR_BELOW = 6 };
enum { TC_EMPTY = 10, TC_WALL, TC_PLATFORM, TC_LADDER, TC_SPIKE, TC_EXIT, TC_CRUMBLE };
enum { EC_CRUSHER = 20, EC_FOREMAN, EC_BAT, EC_SPIDER, EC_BOULDER };
enum { RES_ARRIVED = 30, RES_BLOCKED = 31, RES_MOVING = 32 };
/* enemy .state: idle (a crusher raised & resting — safe to pass under), warning
 * (flashing, about to slam), or moving (RES_MOVING — slamming/retracting, or any
 * other enemy that is always in motion). */
enum { ES_IDLE = 33, ES_WARNING = 34, ES_MOVING = RES_MOVING };
#define SIGHT_TILES 4

/* ---- bytecode ---- */
enum {
    OP_PUSHC, OP_PUSHB, OP_PUSHNONE, OP_POP, OP_DUP,
    OP_LOADL, OP_STOREL,
    OP_NEG, OP_NOT,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV,
    OP_EQ, OP_NE, OP_LT, OP_LE, OP_GT, OP_GE,
    OP_JMP, OP_JMPF, OP_JMPT,
    OP_CALL, OP_RET,
    OP_BUILTIN, OP_MEMBER, OP_ACT, OP_HALT,
    OP_PUSHS, OP_SAY
};

/* action ids for OP_ACT */
enum { ACT_IDLE, ACT_WAIT, ACT_WALK, ACT_CLIMB, ACT_JUMP, ACT_DROP, ACT_GOTO };

/* member field ids */
enum { FLD_COL, FLD_ROW, FLD_DIR, FLD_DIST, FLD_TYPE, FLD_GOING, FLD_STATE };

/* builtin ids (arity in the table below) */
enum {
    B_MY_COL, B_MY_ROW, B_ON_GROUND, B_ON_LADDER, B_FACING,
    B_COAL_GOT, B_COAL_TOTAL, B_COAL_REMAINING, B_LIVES, B_SIGHT, B_DEPTH,
    B_EXIT, B_KEY, B_LAMP, B_NEAREST_COAL, B_NEAREST_ENEMY,
    B_TILE, B_IS_SOLID, B_IS_LADDER, B_IS_STANDABLE,
    B_ENEMY_WITHIN, B_DANGER_AT, B_RANDOM, B_CELL, B_ROUTE,
    B_ABS, B_DISTANCE, B_LOWER, B_HIGHER, B_LIMIT, B_SIDE_OF, B_SIGN_OF,
    B_ENEMY_COUNT, B_ENEMY, B_PLAT_COUNT, B_PLAT, B_PLAT_DIR, B_ON_PLATFORM,
    B_SHAKING, B_SCORE, B_DEATH, B_DEATHS, B_SAFE_TICKS,
    B_COUNT
};
typedef struct { const char *name; int id; int arity; } Builtin;
static const Builtin BUILTINS[] = {
    {"my_col", B_MY_COL, 0}, {"my_row", B_MY_ROW, 0},
    {"on_ground", B_ON_GROUND, 0}, {"on_ladder", B_ON_LADDER, 0},
    {"facing", B_FACING, 0}, {"coal_got", B_COAL_GOT, 0},
    {"coal_total", B_COAL_TOTAL, 0}, {"coal_remaining", B_COAL_REMAINING, 0},
    {"lives", B_LIVES, 0}, {"sight", B_SIGHT, 0}, {"depth", B_DEPTH, 0},
    {"exit", B_EXIT, 0}, {"key", B_KEY, 0}, {"lamp", B_LAMP, 0},
    {"nearest_coal", B_NEAREST_COAL, 0}, {"nearest_enemy", B_NEAREST_ENEMY, 0},
    {"tile", B_TILE, 2}, {"is_solid", B_IS_SOLID, 2},
    {"is_ladder", B_IS_LADDER, 2}, {"is_standable", B_IS_STANDABLE, 2},
    {"enemy_within", B_ENEMY_WITHIN, 2}, {"danger_at", B_DANGER_AT, 2},
    {"random", B_RANDOM, 1}, {"cell", B_CELL, 2}, {"route", B_ROUTE, 1},
    {"abs", B_ABS, 1}, {"distance", B_DISTANCE, 2},
    {"lower", B_LOWER, 2}, {"higher", B_HIGHER, 2}, {"limit", B_LIMIT, 3},
    {"side_of", B_SIDE_OF, 1}, {"sign_of", B_SIGN_OF, 1},
    /* full-knowledge parity with the built-in bots (they read the whole
     * GameState): the complete enemy roster, moving platforms, crumble state */
    {"enemy_count", B_ENEMY_COUNT, 0}, {"enemy", B_ENEMY, 1},
    {"plat_count", B_PLAT_COUNT, 0}, {"plat", B_PLAT, 1}, {"plat_dir", B_PLAT_DIR, 1},
    {"on_platform", B_ON_PLATFORM, 0}, {"shaking", B_SHAKING, 2},
    {"score", B_SCORE, 0},
    /* death memory: where this room last killed us (see MOLESCRIPT.md on
     * SOFT avoidance — never wall the cell off outright) */
    {"death", B_DEATH, 0}, {"deaths", B_DEATHS, 0},
    /* crusher timing: guaranteed-raised ticks left for enemy(i) */
    {"safe_ticks", B_SAFE_TICKS, 1},
};

/* name -> numeric constant */
typedef struct { const char *name; double val; } Konst;
static const Konst KONSTS[] = {
    {"left", DIR_LEFT}, {"right", DIR_RIGHT}, {"up", DIR_UP}, {"down", DIR_DOWN},
    {"above", DIR_ABOVE}, {"below", DIR_BELOW}, {"any", DIR_ANY},
    {"EMPTY", TC_EMPTY}, {"WALL", TC_WALL}, {"PLATFORM", TC_PLATFORM},
    {"LADDER", TC_LADDER}, {"SPIKE", TC_SPIKE}, {"EXIT", TC_EXIT}, {"CRUMBLE", TC_CRUMBLE},
    {"CRUSHER", EC_CRUSHER}, {"FOREMAN", EC_FOREMAN}, {"BAT", EC_BAT},
    {"SPIDER", EC_SPIDER}, {"BOULDER", EC_BOULDER},
    {"arrived", RES_ARRIVED}, {"blocked", RES_BLOCKED}, {"moving", RES_MOVING},
    {"idle", ES_IDLE}, {"warning", ES_WARNING},
};

/* ---- values ---- */
typedef enum { VAL_NONE, VAL_NUM, VAL_BOOL, VAL_CELL, VAL_ENEMY, VAL_STR } ValType;
typedef struct {
    ValType t;
    double num;                 /* NUM / BOOL */
    int col, row;               /* CELL / ENEMY position */
    int edir, etype, edist;     /* ENEMY: side (left/right), type, tile distance */
    int egoing;                 /* ENEMY: heading left/right (which way it's moving) */
    int estate;                 /* ENEMY: idle / warning / moving (crusher slam state) */
    int sidx;                   /* STR: index into the VM's string table */
} Value;

typedef struct { int op, a, b; } Instr;

#define MOLE_MAX_FUNCS   64
#define MOLE_MAX_PARAMS  8
#define MOLE_MAX_LOCALS  48
#define MOLE_MAX_FRAMES  64
#define MOLE_STACK       256
#define MOLE_MAX_LOOP    32
#define MOLE_MAX_BREAKS  64
#define MOLE_MAX_NAMES   MOLE_MAX_LOCALS
#define MOLE_MAX_DEPTH   128              /* compiler recursion cap (paren/block nesting) */
#define MOLE_MAX_ELIF    64              /* elif arms per if */
#define MOLE_MAX_SRC     (1024 * 1024)   /* reject absurdly large .mole files (1 MB) */
#define MOLE_MAX_STRS    64              /* distinct string literals per script */
#define MOLE_SAY_LEN     72              /* the say line shown under the bot */

typedef struct {
    char name[32];
    int  arity, entry;
    char params[MOLE_MAX_PARAMS][32];
    int  headerTok, bodyTok, endTok;   /* token indices, used during compile */
} FuncDef;

typedef struct { int retIp; Value locals[MOLE_MAX_LOCALS]; } Frame;

struct MoleVM {
    /* program */
    Instr  *code;   int codeLen, codeCap;
    double *consts; int nConsts, constsCap;
    FuncDef funcs[MOLE_MAX_FUNCS]; int nFuncs;
    int mainStart;

    /* runtime */
    int ip, ok;
    Value stack[MOLE_STACK]; int sp;
    Frame frames[MOLE_MAX_FRAMES]; int fp;
    uint64_t rng, seed;

    /* action-in-progress state */
    int act;                 /* -1 = none, else ACT_* */
    int actWatch, actPhase;
    int actArg;              /* dir / wait-count */
    int startCol, startRow, startDepth;
    int gotoFinalCol, gotoFinalRow, gotoIsExit, gotoImmediate, gotoResult;
    int gLastDir, gCommitCol, gCommitDir, gSawAir;   /* go_to nav (mirrors bot2) */
    /* per-bot pathfinding freedom: the order in which the BFS tries neighbour
     * moves (a seed-derived permutation of walk/climb/drop). It only breaks ties
     * among equally-short paths, so routes stay optimal but two bots seeded
     * differently pick different ones instead of moving in lockstep. */
    int order[6];
    /* anti-dither: stand still (don't emit) on sustained left/right flip-flop at a
     * platform lip, exactly as bot2 does, so the bot never visibly vibrates. */
    int gDitherCount, gLastHDir;
    /* backstop: if we stay within one cell of the anchor without collecting any
     * coal for a while, we're wedged (vibrating on a lip/ladder rather than
     * making progress) — report blocked so the script can escape. Resetting on
     * coal pickups keeps ordinary tight maze navigation from tripping it. */
    int gAnchorCol, gAnchorRow, gStill, gLastCoal;

    char author[64];   /* from the required first-line comment, for attribution */
    char err[128];

    /* say — the script's introspection line. Says within one tick append to
     * one message; the first say of a NEW tick starts a fresh one. The last
     * message persists (still readable) until the script says again. */
    char strs[MOLE_MAX_STRS][32]; int nStrs;   /* string-literal table */
    char sayBuf[MOLE_SAY_LEN];
    char sayPrev[40];                          /* last appended part (dedupe spam) */
    long tickSerial, sayTick;
};

/* ============================================================= *
 *  Deterministic RNG (splitmix64) — private to the script.
 * ============================================================= */
static uint64_t mole_sm64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* ============================================================= *
 *  Navigation: private copy of bot2's drift-safe walk/climb/drop
 *  BFS + step decider, so go_to matches the built-in bots.
 * ============================================================= */
static void mole_cell(const GameState *s, int *c, int *r)
{
    *c = (int)((s->px + HITBOX_W * 0.5f) / TILE);
    *r = (int)((s->py + HITBOX_H - 1) / TILE);
}
static bool mole_is_ladder(const GameState *s, int c, int r)
{
    return sim_tile_at(s, c, r) == TILE_LADDER;
}

/* Neighbour move ids, tried in a bot-specific `order` to break ties among
 * equally-short paths: 0/1 walk L/R, 2/3 climb up/down, 4/5 drop L/R. */
static bool mole_bfs(const GameState *s, int sc, int sr, int tc, int tr,
                     int *fc, int *fr, int maxDrop, const int *order)
{
    static const int def[6] = {0, 1, 2, 3, 4, 5};
    bool seen[ROOM_H][ROOM_W];
    int px[ROOM_H][ROOM_W], py[ROOM_H][ROOM_W];
    int qx[ROOM_H * ROOM_W], qy[ROOM_H * ROOM_W], head = 0, tail = 0;
    int c, r, dd, oi;
    const int *ord = order ? order : def;

    if (sc < 0 || sc >= ROOM_W || sr < 0 || sr >= ROOM_H) return false;
    if (sc == tc && sr == tr) return false;
    memset(seen, 0, sizeof seen);
    seen[sr][sc] = true; px[sr][sc] = -1; py[sr][sc] = -1;
    qx[tail] = sc; qy[tail] = sr; tail++;

    while (head < tail) {
        c = qx[head]; r = qy[head]; head++;
        if (c == tc && r == tr) {
            int cx = c, cy = r;
            while (!(px[cy][cx] == sc && py[cy][cx] == sr)) {
                int nx = px[cy][cx], ny = py[cy][cx];
                if (nx < 0) break;
                cx = nx; cy = ny;
            }
            *fc = cx; *fr = cy;
            return true;
        }
        #define MOLE_PUSH(NC, NR) do { \
            if (!seen[NR][NC]) { seen[NR][NC] = true; px[NR][NC] = c; py[NR][NC] = r; \
                                 qx[tail] = NC; qy[tail] = NR; tail++; } } while (0)
        for (oi = 0; oi < 6; oi++) {
            int m = ord[oi];
            if (m == 0 || m == 1) {                        /* walk left / right */
                int nc = c + (m == 0 ? -1 : 1);
                if (nc >= 0 && nc < ROOM_W && sim_standable(s, nc, r)) MOLE_PUSH(nc, r);
            } else if (m == 2) {                           /* climb up */
                if ((mole_is_ladder(s, c, r) || mole_is_ladder(s, c, r - 1)) &&
                    sim_standable(s, c, r - 1)) MOLE_PUSH(c, r - 1);
            } else if (m == 3) {                           /* climb down */
                if ((mole_is_ladder(s, c, r) || mole_is_ladder(s, c, r + 1)) &&
                    sim_standable(s, c, r + 1)) MOLE_PUSH(c, r + 1);
            } else {                                       /* drop left (4) / right (5) */
                int dc = (m == 4 ? -1 : 1), nc = c + dc;
                if (nc < 0 || nc >= ROOM_W) continue;
                if (sim_standable(s, nc, r)) continue;
                if (sim_tile_at(s, nc, r) == TILE_SOLID) continue;
                if (sim_tile_at(s, nc, r - 1) == TILE_SOLID) continue;
                for (dd = 1; dd <= maxDrop; dd++) {
                    int nr = r + dd, below = nr + 1;
                    if (nr >= ROOM_H) break;
                    if (sim_standable(s, nc, nr)) {
                        int dcol = nc + dc;
                        bool solidFloor = (below >= ROOM_H) ||
                            (sim_tile_at(s, nc, below) == TILE_SOLID &&
                             s->tiles[below][nc] != TILE_CRUMBLE);
                        bool wide = sim_standable(s, nc - 1, nr) || sim_standable(s, nc + 1, nr);
                        bool driftSafe = (dcol < 0 || dcol >= ROOM_W) ||
                                         sim_standable(s, dcol, nr) ||
                                         sim_tile_at(s, dcol, nr) == TILE_SOLID;
                        if (solidFloor && wide && driftSafe) MOLE_PUSH(nc, nr);
                        break;
                    }
                    if (sim_tile_at(s, nc, nr) == TILE_SOLID) break;
                }
            }
        }
        #undef MOLE_PUSH
    }
    return false;
}

static Input mole_decide(const GameState *s, int tc, int tr, MoleVM *vm, int maxDrop)
{
    Input in = {0};
    int cc, cr;
    mole_cell(s, &cc, &cr);

    if (vm->gCommitCol >= 0) {
        if (!s->onGround && !s->onLadder) vm->gSawAir = 1;
        if (vm->gSawAir && (s->onGround || s->onLadder)) { vm->gCommitCol = -1; vm->gSawAir = 0; }
        else {
            if (!vm->gSawAir) { if (vm->gCommitDir > 0) in.right = true; else in.left = true; }
            else if (cc < vm->gCommitCol) in.right = true;
            else if (cc > vm->gCommitCol) in.left = true;
            return in;
        }
    }

    if (!s->onGround && !s->onLadder) {
        if (vm->gLastDir > 0) in.right = true; else if (vm->gLastDir < 0) in.left = true;
    } else if (s->onLadder) {
        int nc, nr;
        if (!mole_bfs(s, cc, cr, tc, tr, &nc, &nr, maxDrop, vm->order)) in.down = true;
        else if (nr < cr) in.up = true;
        else if (nr > cr && nc == cc) in.down = true;
        else {
            float aligned = (float)((cr + 1) * TILE - HITBOX_H);
            if (s->py + 0.01f >= aligned) {
                if (nc > cc) { in.right = true; vm->gLastDir = 1; }
                else         { in.left  = true; vm->gLastDir = -1; }
            } else in.down = true;
        }
    } else {
        int nc, nr;
        if (mole_bfs(s, cc, cr, tc, tr, &nc, &nr, maxDrop, vm->order)) {
            if (nr < cr) in.up = true;
            else if (nr > cr && nc == cc) in.down = true;
            else if (nr > cr && nc != cc) {
                vm->gCommitCol = nc; vm->gCommitDir = (nc > cc) ? 1 : -1; vm->gSawAir = 0;
                if (vm->gCommitDir > 0) { in.right = true; vm->gLastDir = 1; }
                else                    { in.left  = true; vm->gLastDir = -1; }
            }
            else if (nc > cc) { in.right = true; vm->gLastDir = 1; }
            else if (nc < cc) { in.left = true;  vm->gLastDir = -1; }
        }
    }
    return in;
}

static int mole_bedrock_target(const GameState *s, int cc, int cr)
{
    int d, nc, nr;
    for (d = 1; d < ROOM_W; d++) {
        int a = s->exitCol - d, b = s->exitCol + d;
        if (a >= 0     && mole_bfs(s, cc, cr, a, BEDROCK_ROW - 1, &nc, &nr, 4, NULL)) return a;
        if (b < ROOM_W && mole_bfs(s, cc, cr, b, BEDROCK_ROW - 1, &nc, &nr, 4, NULL)) return b;
    }
    return -1;
}

/* Drive toward the (open) exit shaft, mirroring bot2's descent block. */
static Input mole_exit_input(const GameState *s, MoleVM *vm)
{
    Input in = {0};
    int cc, cr;
    mole_cell(s, &cc, &cr);
    if (!s->onLadder && cr >= BEDROCK_ROW - 1) {
        int center = (int)(s->px + HITBOX_W * 0.5f);
        int shaft  = s->exitCol * TILE + TILE / 2;
        if (center < shaft - 1) in.right = true;
        else if (center > shaft + 1) in.left = true;
        if (cc == s->exitCol) in.down = true;
    } else {
        /* Route toward the bedrock walkway with the full decider — INCLUDING
         * on ladders. (Forcing `down` whenever on a ladder, as this used to,
         * made any exit route that climbs UP a ladder impossible: the decider
         * attached going up, this yanked the bot back down, and it bobbed at
         * the ladder foot forever. The decider's own ladder branch still
         * rides DOWN when no route exists, which was the intended case.) */
        int bt = mole_bedrock_target(s, cc, cr);
        int tc = (bt >= 0) ? bt : ((s->exitCol < ROOM_W - 2) ? s->exitCol + 1 : s->exitCol - 1);
        in = mole_decide(s, tc, BEDROCK_ROW - 1, vm, 4);
    }
    return in;
}

/* ============================================================= *
 *  Lexer
 * ============================================================= */
enum {
    TK_EOF, TK_NEWLINE, TK_NUM, TK_ID,
    TK_LP, TK_RP, TK_COMMA, TK_COLON, TK_DOT, TK_ASSIGN,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH,
    TK_EQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE,
    TK_FUNC, TK_END, TK_IF, TK_THEN, TK_ELSE, TK_ELIF, TK_WHILE, TK_LOOP,
    TK_LOOP_END, TK_WHILE_END, TK_FUNC_END,
    TK_RETURN, TK_BREAK, TK_CONTINUE, TK_AND, TK_OR, TK_NOT,
    TK_TRUE, TK_FALSE, TK_NONE, TK_STRING
};
typedef struct { int type; double num; char text[32]; int line; } Tok;

typedef struct {
    Tok *t; int n, cap;
    const char *err;   /* NULL = ok */
    int errLine;
} Lexer;

static void lx_add(Lexer *L, int type, double num, const char *text, int line)
{
    Tok *tk;
    if (L->err) return;
    if (L->n >= L->cap) {
        int ncap = L->cap ? L->cap * 2 : 128;
        Tok *nt = (Tok *)realloc(L->t, (size_t)ncap * sizeof(Tok));
        if (!nt) { L->err = "out of memory"; L->errLine = line; return; }
        L->t = nt; L->cap = ncap;
    }
    tk = &L->t[L->n++];
    tk->type = type; tk->num = num; tk->line = line; tk->text[0] = '\0';
    if (text) { strncpy(tk->text, text, 31); tk->text[31] = '\0'; }
}

static int lx_keyword(const char *s)
{
    if (!strcmp(s, "func")) return TK_FUNC;
    if (!strcmp(s, "end")) return TK_END;              /* closes an if block only */
    if (!strcmp(s, "if")) return TK_IF;
    if (!strcmp(s, "then")) return TK_THEN;
    if (!strcmp(s, "else")) return TK_ELSE;
    if (!strcmp(s, "elif")) return TK_ELIF;
    if (!strcmp(s, "while")) return TK_WHILE;
    if (!strcmp(s, "loop")) return TK_LOOP;
    if (!strcmp(s, "loop_end")) return TK_LOOP_END;
    if (!strcmp(s, "while_end")) return TK_WHILE_END;
    if (!strcmp(s, "func_end")) return TK_FUNC_END;
    if (!strcmp(s, "return")) return TK_RETURN;
    if (!strcmp(s, "break")) return TK_BREAK;
    if (!strcmp(s, "continue")) return TK_CONTINUE;
    if (!strcmp(s, "and")) return TK_AND;
    if (!strcmp(s, "or")) return TK_OR;
    if (!strcmp(s, "not")) return TK_NOT;
    if (!strcmp(s, "true")) return TK_TRUE;
    if (!strcmp(s, "false")) return TK_FALSE;
    if (!strcmp(s, "none")) return TK_NONE;
    return TK_ID;
}

static void lex(Lexer *L, const char *src)
{
    int line = 1;
    const char *p = src;
    L->t = NULL; L->n = 0; L->cap = 0; L->err = NULL; L->errLine = 0;
    while (*p) {
        char ch = *p;
        if (L->err) return;   /* allocation failed in lx_add — stop */
        if (ch == '\n') { lx_add(L, TK_NEWLINE, 0, NULL, line); line++; p++; continue; }
        if (ch == ' ' || ch == '\t' || ch == '\r') { p++; continue; }
        if (ch == '#') { while (*p && *p != '\n') p++; continue; }
        if (isdigit((unsigned char)ch) || (ch == '.' && isdigit((unsigned char)p[1]))) {
            char buf[32]; int i = 0;
            while (*p && (isdigit((unsigned char)*p) || *p == '.') && i < 31) buf[i++] = *p++;
            buf[i] = '\0';
            lx_add(L, TK_NUM, atof(buf), NULL, line);
            continue;
        }
        if (isalpha((unsigned char)ch) || ch == '_') {
            char buf[32]; int i = 0, kw;
            while (*p && (isalnum((unsigned char)*p) || *p == '_') && i < 31) buf[i++] = *p++;
            buf[i] = '\0';
            kw = lx_keyword(buf);
            lx_add(L, kw, 0, kw == TK_ID ? buf : buf, line);
            continue;
        }
        if (ch == '"') {                       /* string literal (for say) */
            char buf[32]; int i = 0;
            p++;
            while (*p && *p != '"' && *p != '\n') { if (i < 31) buf[i++] = *p; p++; }
            if (*p != '"') { L->err = "unterminated string"; L->errLine = line; return; }
            p++;
            buf[i] = '\0';
            lx_add(L, TK_STRING, 0, buf, line);
            continue;
        }
        p++;
        switch (ch) {
        case '(': lx_add(L, TK_LP, 0, NULL, line); break;
        case ')': lx_add(L, TK_RP, 0, NULL, line); break;
        case ',': lx_add(L, TK_COMMA, 0, NULL, line); break;
        case ':': lx_add(L, TK_COLON, 0, NULL, line); break;
        case '.': lx_add(L, TK_DOT, 0, NULL, line); break;
        case '+': lx_add(L, TK_PLUS, 0, NULL, line); break;
        case '-': lx_add(L, TK_MINUS, 0, NULL, line); break;
        case '*': lx_add(L, TK_STAR, 0, NULL, line); break;
        case '/': lx_add(L, TK_SLASH, 0, NULL, line); break;
        case '=': if (*p == '=') { p++; lx_add(L, TK_EQ, 0, NULL, line); }
                  else lx_add(L, TK_ASSIGN, 0, NULL, line); break;
        case '!': if (*p == '=') { p++; lx_add(L, TK_NE, 0, NULL, line); }
                  else { L->err = "unexpected '!'"; L->errLine = line; return; } break;
        case '<': if (*p == '=') { p++; lx_add(L, TK_LE, 0, NULL, line); }
                  else lx_add(L, TK_LT, 0, NULL, line); break;
        case '>': if (*p == '=') { p++; lx_add(L, TK_GE, 0, NULL, line); }
                  else lx_add(L, TK_GT, 0, NULL, line); break;
        default: L->err = "unexpected character"; L->errLine = line; return;
        }
    }
    lx_add(L, TK_EOF, 0, NULL, line);
}

/* ============================================================= *
 *  Compiler
 * ============================================================= */
typedef struct {
    MoleVM *vm;
    Tok *toks; int nToks, tp;
    char names[MOLE_MAX_NAMES][32]; int nNames;   /* current scope */
    int loopCont[MOLE_MAX_LOOP];
    int loopBrk[MOLE_MAX_LOOP][MOLE_MAX_BREAKS]; int loopNBrk[MOLE_MAX_LOOP];
    int loopSp;
    int depth;         /* recursion depth, bounded by MOLE_MAX_DEPTH */
    jmp_buf jb;
} Comp;

static void cerr(Comp *c, const char *fmt, ...)
{
    va_list ap; char msg[96];
    int line = (c->tp < c->nToks) ? c->toks[c->tp].line : 0;
    va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    snprintf(c->vm->err, sizeof c->vm->err, "line %d: %s", line, msg);
    longjmp(c->jb, 1);
}

static Tok *cur(Comp *c) { return &c->toks[c->tp]; }
static Tok *peek(Comp *c, int k) { int i = c->tp + k; return &c->toks[i < c->nToks ? i : c->nToks - 1]; }
static void adv(Comp *c) { if (c->tp < c->nToks - 1) c->tp++; }
static void skipnl(Comp *c) { while (cur(c)->type == TK_NEWLINE) adv(c); }
static void expect(Comp *c, int type, const char *what)
{
    if (cur(c)->type != type) cerr(c, "expected %s", what);
    adv(c);
}

static int emit(Comp *c, int op, int a, int b)
{
    MoleVM *vm = c->vm;
    if (vm->codeLen >= vm->codeCap) {
        int ncap = vm->codeCap ? vm->codeCap * 2 : 256;
        Instr *nc = (Instr *)realloc(vm->code, (size_t)ncap * sizeof(Instr));
        if (!nc) cerr(c, "out of memory");
        vm->code = nc; vm->codeCap = ncap;
    }
    vm->code[vm->codeLen].op = op; vm->code[vm->codeLen].a = a; vm->code[vm->codeLen].b = b;
    return vm->codeLen++;
}
static int here(Comp *c) { return c->vm->codeLen; }
static void patch(Comp *c, int at, int target) { c->vm->code[at].a = target; }

static int add_const(Comp *c, double v)
{
    MoleVM *vm = c->vm;
    if (vm->nConsts >= vm->constsCap) {
        int ncap = vm->constsCap ? vm->constsCap * 2 : 64;
        double *ncv = (double *)realloc(vm->consts, (size_t)ncap * sizeof(double));
        if (!ncv) cerr(c, "out of memory");
        vm->consts = ncv; vm->constsCap = ncap;
    }
    vm->consts[vm->nConsts] = v;
    return vm->nConsts++;
}
static void emit_num(Comp *c, double v) { emit(c, OP_PUSHC, add_const(c, v), 0); }

static int konst_val(const char *name, double *out)
{
    size_t i;
    for (i = 0; i < sizeof KONSTS / sizeof KONSTS[0]; i++)
        if (!strcmp(KONSTS[i].name, name)) { *out = KONSTS[i].val; return 1; }
    return 0;
}
static int builtin_of(const char *name, int *id, int *arity)
{
    size_t i;
    for (i = 0; i < sizeof BUILTINS / sizeof BUILTINS[0]; i++)
        if (!strcmp(BUILTINS[i].name, name)) { *id = BUILTINS[i].id; *arity = BUILTINS[i].arity; return 1; }
    return 0;
}
static int is_action_word(const char *n)
{
    return !strcmp(n, "walk") || !strcmp(n, "climb") || !strcmp(n, "jump") ||
           !strcmp(n, "drop") || !strcmp(n, "wait") || !strcmp(n, "idle");
}
static int reserved_name(const char *n)
{
    double d; int id, ar;
    return is_action_word(n) || konst_val(n, &d) || builtin_of(n, &id, &ar) ||
           !strcmp(n, "go_to") || !strcmp(n, "say");
}
static int func_of(Comp *c, const char *name)
{
    int i;
    for (i = 0; i < c->vm->nFuncs; i++)
        if (!strcmp(c->vm->funcs[i].name, name)) return i;
    return -1;
}
static int local_slot(Comp *c, const char *name)
{
    int i;
    for (i = 0; i < c->nNames; i++)
        if (!strcmp(c->names[i], name)) return i;
    return -1;
}
static int ensure_local(Comp *c, const char *name)
{
    int s = local_slot(c, name);
    if (s >= 0) return s;
    if (c->nNames >= MOLE_MAX_NAMES) cerr(c, "too many variables");
    strncpy(c->names[c->nNames], name, 31); c->names[c->nNames][31] = '\0';
    return c->nNames++;
}

static int intern_str(Comp *c, const char *text)
{
    int i;
    MoleVM *vm = c->vm;
    for (i = 0; i < vm->nStrs; i++)
        if (!strcmp(vm->strs[i], text)) return i;
    if (vm->nStrs >= MOLE_MAX_STRS) { cerr(c, "too many string literals"); return 0; }
    strncpy(vm->strs[vm->nStrs], text, 31); vm->strs[vm->nStrs][31] = '\0';
    return vm->nStrs++;
}

static void parse_expr(Comp *c);
static void parse_primary(Comp *c);

/* Bound compiler recursion so a pathologically nested script (deep parens, long
 * 'not' chains, deeply nested if/loop blocks) reports an error instead of
 * overflowing the C stack and crashing the game. */
static void cdepth_enter(Comp *c) { if (++c->depth > MOLE_MAX_DEPTH) cerr(c, "nesting too deep"); }
static void cdepth_leave(Comp *c) { c->depth--; }

static int end_of_stmt(Comp *c)
{
    int t = cur(c)->type;
    return t == TK_NEWLINE || t == TK_EOF || t == TK_END || t == TK_ELSE ||
           t == TK_ELIF || t == TK_LOOP_END || t == TK_WHILE_END || t == TK_FUNC_END;
}

static int parse_args(Comp *c)   /* cur == '(' ; returns argc, consumes ')' */
{
    int argc = 0;
    expect(c, TK_LP, "'('");
    if (cur(c)->type != TK_RP) {
        for (;;) {
            parse_expr(c); argc++;
            if (cur(c)->type == TK_COMMA) { adv(c); continue; }
            break;
        }
    }
    expect(c, TK_RP, "')'");
    return argc;
}

static void parse_primary_inner(Comp *c)
{
    Tok *t = cur(c);
    switch (t->type) {
    case TK_NUM:  emit_num(c, t->num); adv(c); return;
    case TK_TRUE: emit(c, OP_PUSHB, 1, 0); adv(c); return;
    case TK_FALSE:emit(c, OP_PUSHB, 0, 0); adv(c); return;
    case TK_NONE: emit(c, OP_PUSHNONE, 0, 0); adv(c); return;
    case TK_STRING: emit(c, OP_PUSHS, intern_str(c, t->text), 0); adv(c); return;
    case TK_LP:   adv(c); parse_expr(c); expect(c, TK_RP, "')'"); return;
    case TK_MINUS: adv(c); parse_primary(c); emit(c, OP_NEG, 0, 0); return;
    case TK_ID: {
        char name[32]; double kv; int fi, bid, ar;
        strncpy(name, t->text, 31); name[31] = '\0';
        adv(c);
        if (cur(c)->type == TK_LP) {              /* a call */
            if (!strcmp(name, "go_to")) {
                int argc = parse_args(c);
                if (argc != 1 && argc != 2) cerr(c, "go_to takes cell or cell, bias");
                emit(c, OP_ACT, ACT_GOTO, argc);
            } else if ((fi = func_of(c, name)) >= 0) {
                int argc = parse_args(c);
                if (argc != c->vm->funcs[fi].arity)
                    cerr(c, "%s takes %d argument(s)", name, c->vm->funcs[fi].arity);
                emit(c, OP_CALL, fi, argc);
            } else if (builtin_of(name, &bid, &ar)) {
                int argc = parse_args(c);
                if (argc != ar) cerr(c, "%s takes %d argument(s)", name, ar);
                emit(c, OP_BUILTIN, bid, argc);
            } else if (konst_val(name, &kv)) {
                cerr(c, "'%s' is a constant, not a function", name);
            } else {
                cerr(c, "unknown function '%s'", name);
            }
        } else {                                  /* a value */
            int slot;
            if (konst_val(name, &kv)) emit_num(c, kv);
            else if (builtin_of(name, &bid, &ar) && ar == 0) emit(c, OP_BUILTIN, bid, 0);
            else if ((slot = local_slot(c, name)) >= 0) emit(c, OP_LOADL, slot, 0);
            else cerr(c, "unknown name '%s'", name);
        }
        return;
    }
    default: cerr(c, "expected an expression"); return;
    }
}

static void parse_primary(Comp *c) { cdepth_enter(c); parse_primary_inner(c); cdepth_leave(c); }

static void parse_postfix(Comp *c)
{
    parse_primary(c);
    while (cur(c)->type == TK_DOT) {
        char *f;
        adv(c);
        if (cur(c)->type != TK_ID) cerr(c, "expected a field name after '.'");
        f = cur(c)->text;
        if      (!strcmp(f, "col"))   emit(c, OP_MEMBER, FLD_COL, 0);
        else if (!strcmp(f, "row"))   emit(c, OP_MEMBER, FLD_ROW, 0);
        else if (!strcmp(f, "dir"))   emit(c, OP_MEMBER, FLD_DIR, 0);
        else if (!strcmp(f, "dist"))  emit(c, OP_MEMBER, FLD_DIST, 0);
        else if (!strcmp(f, "type"))  emit(c, OP_MEMBER, FLD_TYPE, 0);
        else if (!strcmp(f, "going")) emit(c, OP_MEMBER, FLD_GOING, 0);
        else if (!strcmp(f, "state")) emit(c, OP_MEMBER, FLD_STATE, 0);
        else cerr(c, "unknown field '.%s'", f);
        adv(c);
    }
}

static void parse_mul(Comp *c)
{
    parse_postfix(c);
    for (;;) {
        int t = cur(c)->type;
        if (t == TK_STAR)      { adv(c); parse_postfix(c); emit(c, OP_MUL, 0, 0); }
        else if (t == TK_SLASH){ adv(c); parse_postfix(c); emit(c, OP_DIV, 0, 0); }
        else break;
    }
}
static void parse_add(Comp *c)
{
    parse_mul(c);
    for (;;) {
        int t = cur(c)->type;
        if (t == TK_PLUS)      { adv(c); parse_mul(c); emit(c, OP_ADD, 0, 0); }
        else if (t == TK_MINUS){ adv(c); parse_mul(c); emit(c, OP_SUB, 0, 0); }
        else break;
    }
}
static void parse_cmp(Comp *c)
{
    parse_add(c);
    {
        int t = cur(c)->type, op = -1;
        switch (t) {
        case TK_EQ: op = OP_EQ; break; case TK_NE: op = OP_NE; break;
        case TK_LT: op = OP_LT; break; case TK_LE: op = OP_LE; break;
        case TK_GT: op = OP_GT; break; case TK_GE: op = OP_GE; break;
        default: break;
        }
        if (op >= 0) { adv(c); parse_add(c); emit(c, op, 0, 0); }
    }
}
static void parse_not(Comp *c)
{
    cdepth_enter(c);
    if (cur(c)->type == TK_NOT) { adv(c); parse_not(c); emit(c, OP_NOT, 0, 0); }
    else parse_cmp(c);
    cdepth_leave(c);
}
static void parse_and(Comp *c)
{
    parse_not(c);
    while (cur(c)->type == TK_AND) {
        int j;
        adv(c);
        emit(c, OP_DUP, 0, 0);
        j = emit(c, OP_JMPF, 0, 0);
        emit(c, OP_POP, 0, 0);
        parse_not(c);
        patch(c, j, here(c));
    }
}
static void parse_or(Comp *c)
{
    parse_and(c);
    while (cur(c)->type == TK_OR) {
        int j;
        adv(c);
        emit(c, OP_DUP, 0, 0);
        j = emit(c, OP_JMPT, 0, 0);
        emit(c, OP_POP, 0, 0);
        parse_and(c);
        patch(c, j, here(c));
    }
}
static void parse_expr(Comp *c) { parse_or(c); }

static void parse_block(Comp *c);
static void parse_stmt(Comp *c);

static void compile_action(Comp *c)
{
    char w[16];
    strncpy(w, cur(c)->text, 15); w[15] = '\0';
    adv(c);
    if (!strcmp(w, "idle")) { emit(c, OP_ACT, ACT_IDLE, 0); return; }
    if (!strcmp(w, "walk"))  { parse_expr(c); emit(c, OP_ACT, ACT_WALK, 0); return; }
    if (!strcmp(w, "climb")) { parse_expr(c); emit(c, OP_ACT, ACT_CLIMB, 0); return; }
    if (!strcmp(w, "wait"))  { parse_expr(c); emit(c, OP_ACT, ACT_WAIT, 0); return; }
    if (!strcmp(w, "jump")) {
        if (end_of_stmt(c)) emit_num(c, DIR_ANY); else parse_expr(c);
        emit(c, OP_ACT, ACT_JUMP, 0); return;
    }
    if (!strcmp(w, "drop")) {
        if (end_of_stmt(c)) emit_num(c, DIR_ANY); else parse_expr(c);
        emit(c, OP_ACT, ACT_DROP, 0); return;
    }
}

static void parse_stmt_inner(Comp *c)
{
    Tok *t = cur(c);
    switch (t->type) {
    case TK_IF: {
        int jf, jend;
        adv(c); parse_expr(c); expect(c, TK_THEN, "'then'");
        jf = emit(c, OP_JMPF, 0, 0);
        if (cur(c)->type == TK_NEWLINE) {
            /* block form: statements until 'elif'/'else'/'end'. Each 'elif' and an
             * 'else' start a new arm; every arm that runs jumps past the rest to
             * 'end', which closes the whole chain. */
            int endJumps[MOLE_MAX_ELIF]; int nEnd = 0;
            skipnl(c);
            parse_block(c);
            while (cur(c)->type == TK_ELIF) {
                if (nEnd >= MOLE_MAX_ELIF) cerr(c, "too many elif branches");
                endJumps[nEnd++] = emit(c, OP_JMP, 0, 0);   /* end the arm just parsed */
                patch(c, jf, here(c));                       /* previous cond false -> here */
                adv(c); parse_expr(c); expect(c, TK_THEN, "'then'");
                jf = emit(c, OP_JMPF, 0, 0);
                skipnl(c);
                parse_block(c);
            }
            if (cur(c)->type == TK_ELSE) {
                if (nEnd >= MOLE_MAX_ELIF) cerr(c, "too many elif branches");
                endJumps[nEnd++] = emit(c, OP_JMP, 0, 0);
                patch(c, jf, here(c));
                adv(c); skipnl(c);
                parse_block(c);
            } else {
                patch(c, jf, here(c));
            }
            { int k; for (k = 0; k < nEnd; k++) patch(c, endJumps[k], here(c)); }
            expect(c, TK_END, "'end'");
        } else {
            /* single-line form: one statement, optional 'else' + one statement,
             * NO 'end' (it finishes at the end of the line) */
            parse_stmt(c);
            if (cur(c)->type == TK_ELSE) {
                jend = emit(c, OP_JMP, 0, 0);
                patch(c, jf, here(c));
                adv(c);
                parse_stmt(c);
                patch(c, jend, here(c));
            } else {
                patch(c, jf, here(c));
            }
        }
        return;
    }
    case TK_WHILE: {
        int start, jf;
        adv(c);
        start = here(c);
        parse_expr(c); expect(c, TK_COLON, "':'"); skipnl(c);
        jf = emit(c, OP_JMPF, 0, 0);
        if (c->loopSp >= MOLE_MAX_LOOP) cerr(c, "loops nested too deep");
        c->loopCont[c->loopSp] = start; c->loopNBrk[c->loopSp] = 0; c->loopSp++;
        parse_block(c);
        emit(c, OP_JMP, start, 0);
        patch(c, jf, here(c));
        { int i; for (i = 0; i < c->loopNBrk[c->loopSp - 1]; i++)
                     patch(c, c->loopBrk[c->loopSp - 1][i], here(c)); }
        c->loopSp--;
        expect(c, TK_WHILE_END, "'while_end'");
        return;
    }
    case TK_LOOP: {
        int start;
        adv(c); expect(c, TK_COLON, "':'"); skipnl(c);
        start = here(c);
        if (c->loopSp >= MOLE_MAX_LOOP) cerr(c, "loops nested too deep");
        c->loopCont[c->loopSp] = start; c->loopNBrk[c->loopSp] = 0; c->loopSp++;
        parse_block(c);
        emit(c, OP_JMP, start, 0);
        { int i; for (i = 0; i < c->loopNBrk[c->loopSp - 1]; i++)
                     patch(c, c->loopBrk[c->loopSp - 1][i], here(c)); }
        c->loopSp--;
        expect(c, TK_LOOP_END, "'loop_end'");
        return;
    }
    case TK_RETURN:
        adv(c);
        if (end_of_stmt(c)) emit(c, OP_PUSHNONE, 0, 0); else parse_expr(c);
        emit(c, OP_RET, 0, 0);
        return;
    case TK_BREAK: {
        int j;
        adv(c);
        if (c->loopSp <= 0) cerr(c, "break outside a loop");
        j = emit(c, OP_JMP, 0, 0);
        { int L = c->loopSp - 1;
          if (c->loopNBrk[L] >= MOLE_MAX_BREAKS) cerr(c, "too many breaks");
          c->loopBrk[L][c->loopNBrk[L]++] = j; }
        return;
    }
    case TK_CONTINUE:
        adv(c);
        if (c->loopSp <= 0) cerr(c, "continue outside a loop");
        emit(c, OP_JMP, c->loopCont[c->loopSp - 1], 0);
        return;
    case TK_FUNC:
        cerr(c, "functions must be defined at the top level");
        return;
    case TK_ID:
        if (is_action_word(t->text)) { compile_action(c); return; }
        if (!strcmp(t->text, "say")) {
            /* say EXPR [, EXPR ...] — introspection: builds this tick's status
             * line (shown under the bot in-game, printed by --moletest).
             * Instantaneous — consumes no tick. */
            adv(c);
            parse_expr(c);
            emit(c, OP_SAY, 0, 0);
            while (cur(c)->type == TK_COMMA) {
                adv(c);
                parse_expr(c);
                emit(c, OP_SAY, 0, 0);
            }
            return;
        }
        if (peek(c, 1)->type == TK_ASSIGN) {
            char name[32]; int slot;
            strncpy(name, t->text, 31); name[31] = '\0';
            if (reserved_name(name)) cerr(c, "cannot assign to reserved name '%s'", name);
            adv(c); adv(c);           /* name '=' */
            parse_expr(c);
            slot = ensure_local(c, name);
            emit(c, OP_STOREL, slot, 0);
            return;
        }
        /* fall through: expression statement */
    default:
        parse_expr(c);
        emit(c, OP_POP, 0, 0);
        return;
    }
}

static void parse_stmt(Comp *c) { cdepth_enter(c); parse_stmt_inner(c); cdepth_leave(c); }

static int is_block_end(int t)
{
    return t == TK_END || t == TK_ELIF || t == TK_ELSE || t == TK_LOOP_END ||
           t == TK_WHILE_END || t == TK_FUNC_END || t == TK_EOF;
}

static void parse_block(Comp *c)
{
    cdepth_enter(c);
    for (;;) {
        skipnl(c);
        if (is_block_end(cur(c)->type)) break;
        parse_stmt(c);
    }
    cdepth_leave(c);
}

/* Scan the token stream, registering every top-level function. */
static void prescan_funcs(Comp *c)
{
    int i = 0;
    while (i < c->nToks) {
        if (c->toks[i].type == TK_FUNC) {
            FuncDef *f;
            int depth, j;
            if (c->vm->nFuncs >= MOLE_MAX_FUNCS) { c->tp = i; cerr(c, "too many functions"); }
            f = &c->vm->funcs[c->vm->nFuncs];
            f->arity = 0; f->headerTok = i;
            if (c->toks[i + 1].type != TK_ID) { c->tp = i + 1; cerr(c, "expected a function name"); }
            strncpy(f->name, c->toks[i + 1].text, 31); f->name[31] = '\0';
            if (reserved_name(f->name)) { c->tp = i + 1; cerr(c, "reserved name '%s'", f->name); }
            j = i + 2;
            if (c->toks[j].type != TK_LP) { c->tp = j; cerr(c, "expected '(' after function name"); }
            j++;
            while (c->toks[j].type != TK_RP && c->toks[j].type != TK_EOF) {
                if (c->toks[j].type == TK_ID) {
                    if (f->arity >= MOLE_MAX_PARAMS) { c->tp = j; cerr(c, "too many parameters"); }
                    strncpy(f->params[f->arity], c->toks[j].text, 31);
                    f->params[f->arity][31] = '\0'; f->arity++;
                } else if (c->toks[j].type != TK_COMMA) {
                    c->tp = j; cerr(c, "bad parameter list");
                }
                j++;
            }
            if (c->toks[j].type != TK_RP) { c->tp = j; cerr(c, "expected ')'"); }
            j++;
            if (c->toks[j].type != TK_COLON) { c->tp = j; cerr(c, "expected ':' after ')'"); }
            j++;
            f->bodyTok = j;
            /* find the closing func_end. Functions don't nest and every other
             * block has its own terminator (end / loop_end / while_end), so the
             * first func_end after the header closes this function. */
            while (j < c->nToks && c->toks[j].type != TK_FUNC_END &&
                   c->toks[j].type != TK_EOF) j++;
            if (c->toks[j].type != TK_FUNC_END) { c->tp = f->bodyTok; cerr(c, "function missing 'func_end'"); }
            f->endTok = j;
            (void)depth;
            c->vm->nFuncs++;
            i = j + 1;
        } else {
            i++;
        }
    }
}

static void compile(Comp *c)
{
    int fi;
    prescan_funcs(c);

    /* compile each function body */
    for (fi = 0; fi < c->vm->nFuncs; fi++) {
        FuncDef *f = &c->vm->funcs[fi];
        int p;
        f->entry = here(c);
        c->nNames = 0;
        for (p = 0; p < f->arity; p++) {
            strncpy(c->names[p], f->params[p], 31); c->names[p][31] = '\0';
        }
        c->nNames = f->arity;
        c->tp = f->bodyTok;
        for (;;) {
            skipnl(c);
            if (cur(c)->type == TK_FUNC_END || cur(c)->type == TK_EOF) break;
            parse_stmt(c);
        }
        emit(c, OP_PUSHNONE, 0, 0);
        emit(c, OP_RET, 0, 0);
    }

    /* compile top-level (main), skipping function regions; then loop forever */
    c->vm->mainStart = here(c);
    c->nNames = 0;
    c->tp = 0;
    for (;;) {
        skipnl(c);
        if (cur(c)->type == TK_EOF) break;
        if (cur(c)->type == TK_FUNC) {
            int k, skipped = 0;
            for (k = 0; k < c->vm->nFuncs; k++)
                if (c->vm->funcs[k].headerTok == c->tp) {
                    c->tp = c->vm->funcs[k].endTok + 1; skipped = 1; break;
                }
            if (!skipped) cerr(c, "internal: unscanned function");
            continue;
        }
        parse_stmt(c);
    }
    emit(c, OP_JMP, c->vm->mainStart, 0);   /* implicit outer loop */
    emit(c, OP_HALT, 0, 0);                  /* unreachable safety net */
}

/* ============================================================= *
 *  Runtime helpers
 * ============================================================= */
static Value v_none(void)          { Value v; v.t = VAL_NONE; v.num = 0; v.col = v.row = 0; v.edir = v.etype = v.edist = v.egoing = v.estate = 0; v.sidx = 0; return v; }
static Value v_num(double n)       { Value v = v_none(); v.t = VAL_NUM; v.num = n; return v; }
static Value v_bool(int b)         { Value v = v_none(); v.t = VAL_BOOL; v.num = b ? 1 : 0; return v; }
static Value v_cell(int col, int row){ Value v = v_none(); v.t = VAL_CELL; v.col = col; v.row = row; return v; }

static int v_truthy(Value v)
{
    switch (v.t) {
    case VAL_NONE: return 0;
    case VAL_NUM:  return v.num != 0.0;
    case VAL_BOOL: return v.num != 0.0;
    default:       return 1;   /* cells and enemies exist -> true */
    }
}
static double v_numify(Value v) { return (v.t == VAL_NUM || v.t == VAL_BOOL) ? v.num : 0.0; }

/* Untrusted script number -> int, clamped. Scripts can reach infinity in a
 * few ticks (x = x * x) and can conjure NaN (inf * 0); casting those to int
 * is UNDEFINED BEHAVIOUR in C, which would break the promise that a
 * malformed script can never crash (or corrupt) the game. NaN and -inf
 * funnel through the first branch (all comparisons with NaN are false). */
static int v_int(Value v)
{
    double d = v_numify(v);
    if (!(d >= -2147483647.0)) return -2147483647;
    if (d > 2147483647.0) return 2147483647;
    return (int)d;
}

static int v_equal(Value a, Value b)
{
    if (a.t == VAL_NONE || b.t == VAL_NONE) return a.t == VAL_NONE && b.t == VAL_NONE;
    if ((a.t == VAL_NUM || a.t == VAL_BOOL) && (b.t == VAL_NUM || b.t == VAL_BOOL))
        return v_numify(a) == v_numify(b);
    if (a.t == VAL_CELL && b.t == VAL_CELL) return a.col == b.col && a.row == b.row;
    if (a.t == VAL_ENEMY && b.t == VAL_ENEMY)
        return a.col == b.col && a.row == b.row && a.etype == b.etype;
    if (a.t == VAL_STR && b.t == VAL_STR) return a.sidx == b.sidx;   /* interned */
    return 0;
}

static void push(MoleVM *vm, Value v)
{
    if (vm->sp >= MOLE_STACK) { vm->ok = 0; return; }
    vm->stack[vm->sp++] = v;
}
static Value pop(MoleVM *vm)
{
    if (vm->sp <= 0) { vm->ok = 0; return v_none(); }
    return vm->stack[--vm->sp];
}

static int mole_tile_const(TileType t)
{
    switch (t) {
    case TILE_SOLID:   return TC_WALL;
    case TILE_ONEWAY:  return TC_PLATFORM;
    case TILE_LADDER:  return TC_LADDER;
    case TILE_SPIKE:   return TC_SPIKE;
    case TILE_EXIT:    return TC_EXIT;
    case TILE_CRUMBLE: return TC_CRUMBLE;
    default:           return TC_EMPTY;
    }
}
static int mole_enemy_const(EnemyType t)
{
    switch (t) {
    case EN_CRUSHER: return EC_CRUSHER;
    case EN_FOREMAN: return EC_FOREMAN;
    case EN_BAT:     return EC_BAT;
    case EN_SPIDER:  return EC_SPIDER;
    case EN_BOULDER: return EC_BOULDER;
    default:         return EC_CRUSHER;
    }
}
static void enemy_cell(const Enemy *e, int *c, int *r)
{
    *c = (int)((e->x + 8.0f) / TILE);
    *r = (int)((e->y + 8.0f) / TILE);
}

/* nearest uncollected coal, reachable first; VAL_CELL or VAL_NONE */
static Value mole_nearest_coal(const GameState *s)
{
    int cc, cr, r, c, nc, nr;
    mole_cell(s, &cc, &cr);
    for (r = 0; r < ROOM_H; r++)
        for (c = 0; c < ROOM_W; c++)
            if (s->coal[r][c] && mole_bfs(s, cc, cr, c, r, &nc, &nr, 4, NULL)) return v_cell(c, r);
    for (r = 0; r < ROOM_H; r++)
        for (c = 0; c < ROOM_W; c++)
            if (s->coal[r][c]) return v_cell(c, r);
    return v_none();
}

/* Build the script-facing enemy value for enemies[i], relative to the bot at
 * (cc,cr). Shared by nearest_enemy (sight-limited) and enemy(i) (full roster). */
static Value mole_enemy_value(const GameState *s, int i, int cc, int cr)
{
    Value v = v_none();
    const Enemy *e = &s->enemies[i];
    int ec, er, dc, dr;
    enemy_cell(e, &ec, &er);
    dc = ec - cc; if (dc < 0) dc = -dc;
    dr = er - cr; if (dr < 0) dr = -dr;
    v.t = VAL_ENEMY; v.col = ec; v.row = er;
    v.edist = dc > dr ? dc : dr;
    v.edir = (ec < cc) ? DIR_LEFT : DIR_RIGHT;
    v.egoing = (e->dir >= 0) ? DIR_RIGHT : DIR_LEFT;   /* which way it's moving */
    v.etype = mole_enemy_const(e->type);
    /* .state: a crusher tells idle / warning / moving from its slam phase
     * (0 rest, 1 warn/flash, 2-3 slam/retract); a boulder parked off-screen
     * for its spawn delay reads idle; anything else is moving. */
    if (e->type == EN_CRUSHER) {
        int ph = e->phase;
        v.estate = (ph == 0) ? ES_IDLE : (ph == 1) ? ES_WARNING : ES_MOVING;
    } else if (e->type == EN_BOULDER && e->spawnDelay > 0) {
        v.estate = ES_IDLE;
    } else {
        v.estate = ES_MOVING;
    }
    return v;
}

static Value mole_nearest_enemy(const GameState *s)
{
    int cc, cr, i, best = -1, bestd = 0;
    mole_cell(s, &cc, &cr);
    for (i = 0; i < s->enemyCount; i++) {
        int ec, er, dc, dr, d;
        enemy_cell(&s->enemies[i], &ec, &er);
        dc = ec - cc; if (dc < 0) dc = -dc;
        dr = er - cr; if (dr < 0) dr = -dr;
        d = dc > dr ? dc : dr;
        if (d > SIGHT_TILES) continue;
        if (best < 0 || d < bestd) { best = i; bestd = d; }
    }
    if (best < 0) return v_none();
    return mole_enemy_value(s, best, cc, cr);
}

/* say: append one value, formatted, to this tick's message line. */
static const char *mole_type_name(int etype)
{
    switch (etype) {
    case EC_CRUSHER: return "crusher";
    case EC_FOREMAN: return "foreman";
    case EC_BAT:     return "bat";
    case EC_SPIDER:  return "spider";
    case EC_BOULDER: return "boulder";
    default:         return "enemy";
    }
}
static void mole_do_say(MoleVM *vm, Value v)
{
    char part[40];
    size_t used;
    switch (v.t) {
    case VAL_STR:  snprintf(part, sizeof part, "%s", vm->strs[v.sidx]); break;
    case VAL_BOOL: snprintf(part, sizeof part, "%s", v.num != 0.0 ? "true" : "false"); break;
    case VAL_NUM:
        /* range-check BEFORE the (long) casts — for inf/NaN even the
         * integrality test's cast is undefined behaviour */
        if (v.num >= -9e15 && v.num <= 9e15 && v.num == (double)(long)v.num)
            snprintf(part, sizeof part, "%ld", (long)v.num);
        else snprintf(part, sizeof part, "%g", v.num);
        break;
    case VAL_CELL:  snprintf(part, sizeof part, "(%d,%d)", v.col, v.row); break;
    case VAL_ENEMY: snprintf(part, sizeof part, "%s(%d,%d)", mole_type_name(v.etype), v.col, v.row); break;
    default:        snprintf(part, sizeof part, "none"); break;
    }
    if (vm->sayTick != vm->tickSerial) {   /* first say this tick: fresh line */
        vm->sayBuf[0] = '\0';
        vm->sayPrev[0] = '\0';
        vm->sayTick = vm->tickSerial;
    }
    /* A script loop spinning on an instant action (e.g. a blocked go_to) says
     * the same thing dozens of times per tick — collapse consecutive repeats
     * so the line (and the log) stays readable. */
    if (vm->sayPrev[0] && strcmp(part, vm->sayPrev) == 0) return;
    snprintf(vm->sayPrev, sizeof vm->sayPrev, "%s", part);
    used = strlen(vm->sayBuf);
    if (used > 0 && used + 1 < sizeof vm->sayBuf) { vm->sayBuf[used++] = ' '; vm->sayBuf[used] = '\0'; }
    snprintf(vm->sayBuf + used, sizeof vm->sayBuf - used, "%s", part);
}

static int mole_enemy_within(const GameState *s, int range, int dir)
{
    int cc, cr, i;
    if (range > SIGHT_TILES) range = SIGHT_TILES;
    mole_cell(s, &cc, &cr);
    for (i = 0; i < s->enemyCount; i++) {
        int ec, er, dc, dr, d, ok;
        /* Crushers are stationary and only ever threaten their own column (the
         * generator never puts one over a ladder), so a crusher beside you — e.g.
         * next to the ladder you're on — is harmless. enemy_within reports MOBILE
         * threats closing in; use danger_at for a crusher's slam timing. */
        if (s->enemies[i].type == EN_CRUSHER) continue;
        enemy_cell(&s->enemies[i], &ec, &er);
        dc = ec - cc; dr = er - cr;
        d = (dc < 0 ? -dc : dc) > (dr < 0 ? -dr : dr) ? (dc < 0 ? -dc : dc) : (dr < 0 ? -dr : dr);
        if (d > range) continue;
        /* Horizontal queries mean "on my row" (within a row of it); vertical
         * queries mean "on my column" (within a column of it). So enemy_within
         * (n, below) answers "is a foe on the ladder below me", not "anywhere
         * down-and-to-the-side" — which is what avoidance actually needs. */
        switch (dir) {
        case DIR_LEFT:  ok = dc < 0 && (dr <= 1 && dr >= -1); break;
        case DIR_RIGHT: ok = dc > 0 && (dr <= 1 && dr >= -1); break;
        case DIR_UP: case DIR_ABOVE: ok = dr < 0 && (dc <= 1 && dc >= -1); break;
        case DIR_DOWN: case DIR_BELOW: ok = dr > 0 && (dc <= 1 && dc >= -1); break;
        default: ok = 1; break;   /* any */
        }
        if (ok) return 1;
    }
    return 0;
}

/* would a sensed (in-sight) enemy's body overlap cell (col,row) right now? */
static int mole_danger_at(const GameState *s, int col, int row)
{
    int cc, cr, i;
    float bx, by;
    /* Reject out-of-room cells up front: no enemy body can be outside the room,
     * and it keeps col*TILE from overflowing on a wild cell() coordinate. */
    if (col < 0 || col >= ROOM_W || row < 0 || row >= ROOM_H) return 0;
    bx = (float)(col * TILE); by = (float)(row * TILE);
    mole_cell(s, &cc, &cr);
    for (i = 0; i < s->enemyCount; i++) {
        int ec, er, dc, dr, d;
        const Enemy *e = &s->enemies[i];
        enemy_cell(e, &ec, &er);
        dc = ec - cc; if (dc < 0) dc = -dc;
        dr = er - cr; if (dr < 0) dr = -dr;
        d = dc > dr ? dc : dr;
        if (d > SIGHT_TILES) continue;
        if (e->x + TILE > bx + 1 && e->x < bx + TILE - 1 &&
            e->y + TILE > by + 1 && e->y < by + TILE - 1) return 1;
    }
    return 0;
}

static Value eval_builtin(MoleVM *vm, GameState *s, int id, Value *args, int argc)
{
    int cc, cr;
    mole_cell(s, &cc, &cr);
    (void)argc;
    switch (id) {
    case B_MY_COL: return v_num(cc);
    case B_MY_ROW: return v_num(cr);
    case B_ON_GROUND: return v_bool(s->onGround);
    case B_ON_LADDER: return v_bool(s->onLadder);
    case B_FACING: return v_num(s->facing < 0 ? DIR_LEFT : DIR_RIGHT);
    case B_COAL_GOT: return v_num(s->coalGot);
    case B_COAL_TOTAL: return v_num(s->coalTotal);
    case B_COAL_REMAINING: {
        int r, c, n = 0;
        for (r = 0; r < ROOM_H; r++) for (c = 0; c < ROOM_W; c++) if (s->coal[r][c]) n++;
        return v_num(n);
    }
    case B_LIVES: return v_num(s->lives);
    case B_SIGHT: return v_num(SIGHT_TILES);
    case B_DEPTH: return v_num(s->depth);
    case B_EXIT: return v_cell(s->exitCol, BEDROCK_ROW);
    case B_KEY: return (s->keyRoom && !s->keyGot && s->keyCol >= 0)
                         ? v_cell(s->keyCol, s->keyRow) : v_none();
    case B_LAMP: return (s->darkRoom && !s->lampGot && s->lampCol >= 0)
                          ? v_cell(s->lampCol, s->lampRow) : v_none();
    case B_NEAREST_COAL: return mole_nearest_coal(s);
    case B_NEAREST_ENEMY: return mole_nearest_enemy(s);
    case B_TILE: return v_num(mole_tile_const(sim_tile_at(s, v_int(args[0]), v_int(args[1]))));
    case B_IS_SOLID: return v_bool(sim_tile_at(s, v_int(args[0]), v_int(args[1])) == TILE_SOLID);
    case B_IS_LADDER: return v_bool(sim_tile_at(s, v_int(args[0]), v_int(args[1])) == TILE_LADDER);
    case B_IS_STANDABLE: return v_bool(sim_standable(s, v_int(args[0]), v_int(args[1])));
    case B_ENEMY_WITHIN: return v_bool(mole_enemy_within(s, v_int(args[0]), v_int(args[1])));
    case B_DANGER_AT: return v_bool(mole_danger_at(s, v_int(args[0]), v_int(args[1])));
    case B_RANDOM: {
        int n = v_int(args[0]);
        vm->rng = mole_sm64(vm->rng);
        return v_num(n <= 0 ? 0 : (double)(int)((vm->rng >> 33) % (uint64_t)n));
    }
    case B_CELL: return v_cell(v_int(args[0]), v_int(args[1]));
    case B_ROUTE: {
        /* The next-step direction toward a cell along the walk/climb/drop path
         * (same pathfinder go_to uses), so a bot can steer its own movement:
         * left/right/up/down, or `none` if already there or no route exists. */
        int cc, cr, nc, nr, tcx, tcy;
        if (args[0].t != VAL_CELL) return v_none();
        mole_cell(s, &cc, &cr);
        tcx = args[0].col; tcy = args[0].row;
        if (cc == tcx && cr == tcy) return v_none();
        /* The exit is a hole in the bedrock, not a standable cell, so route to the
         * shaft the way go_to(exit) does: reach the bedrock walkway, walk to the
         * shaft column, then press down to drop in. */
        if (tcx == s->exitCol && tcy == BEDROCK_ROW) {
            if (cr >= BEDROCK_ROW - 1) {
                int center = (int)(s->px + HITBOX_W * 0.5f);
                int shaft  = s->exitCol * TILE + TILE / 2;
                if (cc == s->exitCol) return v_num(DIR_DOWN);
                return v_num(center < shaft ? DIR_RIGHT : DIR_LEFT);
            }
            { int bt = mole_bedrock_target(s, cc, cr);
              tcx = (bt >= 0) ? bt : ((s->exitCol < ROOM_W - 2) ? s->exitCol + 1 : s->exitCol - 1);
              tcy = BEDROCK_ROW - 1; }
            if (cc == tcx && cr == tcy) return v_num(DIR_DOWN);
        }
        if (!mole_bfs(s, cc, cr, tcx, tcy, &nc, &nr, 4, vm->order)) return v_none();
        if (nr < cr) return v_num(DIR_UP);
        if (nr > cr && nc == cc) return v_num(DIR_DOWN);
        if (nc < cc) return v_num(DIR_LEFT);
        if (nc > cc) return v_num(DIR_RIGHT);
        return v_num(DIR_DOWN);
    }
    /* numeric helpers */
    case B_ABS: { double x = v_numify(args[0]); return v_num(x < 0 ? -x : x); }
    case B_DISTANCE: {   /* |a - b| — tiles between two positions, sign-free */
        double d = v_numify(args[0]) - v_numify(args[1]);
        return v_num(d < 0 ? -d : d);
    }
    case B_LOWER:  { double a = v_numify(args[0]), b = v_numify(args[1]); return v_num(a < b ? a : b); }
    case B_HIGHER: { double a = v_numify(args[0]), b = v_numify(args[1]); return v_num(a > b ? a : b); }
    case B_LIMIT: {      /* hold x inside lo..hi (expects lo <= hi) */
        double x = v_numify(args[0]), lo = v_numify(args[1]), hi = v_numify(args[2]);
        if (x < lo) x = lo;
        if (x > hi) x = hi;
        return v_num(x);
    }
    case B_SIDE_OF: {    /* sign as a direction: left / right / any(=0) */
        double x = v_numify(args[0]);
        return v_num(x < 0 ? DIR_LEFT : x > 0 ? DIR_RIGHT : DIR_ANY);
    }
    case B_SIGN_OF: { double x = v_numify(args[0]); return v_num(x < 0 ? -1 : x > 0 ? 1 : 0); }
    /* full-knowledge parity sensors (the built-in bots read all of GameState) */
    case B_ENEMY_COUNT: return v_num(s->enemyCount);
    case B_ENEMY: {
        int i = v_int(args[0]);
        if (i < 0 || i >= s->enemyCount) return v_none();
        return mole_enemy_value(s, i, cc, cr);
    }
    case B_PLAT_COUNT: return v_num(s->platCount);
    case B_PLAT: {       /* a platform's current position, as the cell of its left tile */
        int i = v_int(args[0]);
        if (i < 0 || i >= s->platCount) return v_none();
        return v_cell((int)(s->plats[i].x / TILE), (int)(s->plats[i].y / TILE));
    }
    case B_PLAT_DIR: {   /* which way platform i is moving right now */
        int i = v_int(args[0]);
        const MovePlat *p;
        if (i < 0 || i >= s->platCount) return v_none();
        p = &s->plats[i];
        if (p->axis == 0) return v_num(p->dir > 0 ? DIR_RIGHT : DIR_LEFT);
        return v_num(p->dir > 0 ? DIR_DOWN : DIR_UP);
    }
    case B_ON_PLATFORM: return v_bool(s->ridingPlat >= 0);
    case B_SHAKING: {    /* is the crumble tile at (c,r) armed and about to collapse? */
        int tc2 = v_int(args[0]), tr2 = v_int(args[1]);
        if (tc2 < 0 || tc2 >= ROOM_W || tr2 < 0 || tr2 >= ROOM_H) return v_bool(0);
        return v_bool(s->tiles[tr2][tc2] == TILE_CRUMBLE && s->crumble[tr2][tc2] > 0);
    }
    case B_SCORE: return v_num((double)s->score);
    case B_DEATH: return (s->deaths > 0 && s->deathCol >= 0)
                           ? v_cell(s->deathCol, s->deathRow) : v_none();
    case B_DEATHS: return v_num(s->deaths);
    case B_SAFE_TICKS: {   /* enemy(i)'s guaranteed-raised time; 0 unless an idle crusher */
        int i = v_int(args[0]);
        if (i < 0 || i >= s->enemyCount) return v_num(0);
        return v_num(sim_crusher_safe_ticks(&s->enemies[i]));
    }
    default: return v_none();
    }
}

/* Set the BFS neighbour-try order from a script-chosen bias, so go_to prefers
 * that direction when several shortest paths exist. Deterministic — the script's
 * logic, not chance, steers the route. Moves: 0/1 walk L/R, 2/3 climb U/D, 4/5
 * drop L/R. */
static void mole_set_order(MoleVM *vm, int bias)
{
    static const int L[6] = {0, 4, 2, 3, 1, 5};   /* leftward first  */
    static const int R[6] = {1, 5, 2, 3, 0, 4};   /* rightward first */
    static const int U[6] = {2, 0, 1, 3, 4, 5};   /* climb up first  */
    static const int D[6] = {3, 4, 5, 0, 1, 2};   /* down/drop first */
    static const int A[6] = {0, 1, 2, 3, 4, 5};   /* neutral         */
    const int *o = A;
    if (bias == DIR_LEFT) o = L;
    else if (bias == DIR_RIGHT) o = R;
    else if (bias == DIR_UP || bias == DIR_ABOVE) o = U;
    else if (bias == DIR_DOWN || bias == DIR_BELOW) o = D;
    memcpy(vm->order, o, sizeof vm->order);
}

/* ============================================================= *
 *  Action service — one action, spanning ticks
 * ============================================================= */
static void start_action(MoleVM *vm, GameState *s, int act, int argc)
{
    int cc, cr;
    mole_cell(s, &cc, &cr);
    vm->act = act; vm->actWatch = 0; vm->actPhase = 0;
    vm->startCol = cc; vm->startRow = cr; vm->startDepth = s->depth;
    vm->gLastDir = 0; vm->gCommitCol = -1; vm->gCommitDir = 0; vm->gSawAir = 0;
    switch (act) {
    case ACT_WALK: case ACT_CLIMB: case ACT_JUMP: case ACT_DROP:
        vm->actArg = v_int(pop(vm)); break;
    case ACT_WAIT:
        vm->actArg = v_int(pop(vm)); break;
    case ACT_IDLE:
        break;
    case ACT_GOTO: {
        int bias = DIR_ANY;
        Value tgt;
        if (argc == 2) bias = v_int(pop(vm));   /* optional path bias, pushed last */
        tgt = pop(vm);
        mole_set_order(vm, bias);
        if (tgt.t != VAL_CELL) { vm->gotoImmediate = 1; vm->gotoResult = RES_BLOCKED; }
        else {
            vm->gotoImmediate = 0;
            vm->gotoFinalCol = tgt.col; vm->gotoFinalRow = tgt.row;
            vm->gotoIsExit = (tgt.col == s->exitCol && tgt.row == BEDROCK_ROW);
        }
        break;
    }
    default: break;
    }
}

/* returns 1 when the action has finished (leave *out untouched), else 0 with
 * *out set to this tick's input. */
static int service_action(MoleVM *vm, GameState *s, Input *out)
{
    int cc, cr, dying;
    Input in = {0};
    mole_cell(s, &cc, &cr);
    dying = (s->state != PS_ALIVE);
    vm->actWatch++;
    if (!s->onGround && !s->onLadder) vm->actPhase = 1;   /* airborne seen */

    switch (vm->act) {
    case ACT_IDLE:
        if (vm->actWatch > 1 || dying) return 1;
        *out = in; return 0;
    case ACT_WAIT:
        if (vm->actWatch > vm->actArg || dying) return 1;
        *out = in; return 0;
    case ACT_WALK: {
        int dir = (vm->actArg == DIR_LEFT) ? -1 : (vm->actArg == DIR_RIGHT) ? 1 : 0;
        if (dying) return 1;
        if (dir == 0) {              /* walk any/none (e.g. side_of(0)): stand a tick —
                                      * silently picking a side made bots vibrate at
                                      * their target column forever */
            if (vm->actWatch > 1) return 1;
            *out = in; return 0;
        }
        if (s->onGround || s->onLadder) {
            /* done once SETTLED at the next cell's CENTRE — completing at the
             * first boundary pixel leaves the body straddling two cells, and
             * any decision that flips at that boundary then vibrates in 2px
             * twitches instead of walking honest full-tile steps */
            float centre = s->px + HITBOX_W * 0.5f;
            float goal   = (float)((vm->startCol + dir) * TILE) + (float)TILE * 0.5f;
            if (cc == vm->startCol + dir &&
                (dir > 0 ? centre >= goal - 0.5f : centre <= goal + 0.5f)) return 1;
            if (cc != vm->startCol && cc != vm->startCol + dir) return 1;  /* fell/pushed elsewhere */
        }
        if (vm->actWatch > 45) return 1;
        /* Stepping off a ladder needs the body aligned to the row boundary first
         * (the sim blocks a sideways step whose head/feet would clip a tile), so
         * descend to align before pushing sideways — same as the go_to decider. */
        if (s->onLadder) {
            float aligned = (float)((cr + 1) * TILE - HITBOX_H);
            if (s->py + 0.01f < aligned) { in.down = true; *out = in; return 0; }
        }
        if (dir < 0) in.left = true; else in.right = true;
        *out = in; return 0;
    }
    case ACT_CLIMB: {
        int dir = (vm->actArg == DIR_UP) ? -1 : (vm->actArg == DIR_DOWN) ? 1 : 0;
        if (dying) return 1;
        if (dir == 0) {              /* climb any/none: stand a tick (see ACT_WALK) */
            if (vm->actWatch > 1) return 1;
            *out = in; return 0;
        }
        if (s->onGround && !s->onLadder && cr != vm->startRow) return 1;  /* popped on top / landed */
        if (s->onLadder && cr == vm->startRow + dir) {
            /* settled a full rung away (aligned), not just across the row line */
            float goalY = (float)((vm->startRow + dir + 1) * TILE - HITBOX_H);
            if (dir < 0 ? s->py <= goalY + 0.5f : s->py >= goalY - 0.5f) return 1;
        }
        if (vm->actWatch > 45) return 1;
        if (dir < 0) in.up = true; else in.down = true;
        *out = in; return 0;
    }
    case ACT_JUMP:
        if (dying) return 1;
        if (vm->actPhase == 1 && s->onGround) return 1;   /* landed */
        if (vm->actWatch > 70) return 1;
        if (vm->actWatch == 1) in.jump = true;            /* edge-trigger */
        if (vm->actArg == DIR_LEFT) in.left = true;
        else if (vm->actArg == DIR_RIGHT) in.right = true;
        *out = in; return 0;
    case ACT_DROP: {
        int dir = (vm->actArg == DIR_LEFT) ? -1 : (vm->actArg == DIR_RIGHT) ? 1
                    : (s->facing < 0 ? -1 : 1);
        if (dying) return 1;
        if (vm->actPhase == 1 && (s->onGround || s->onLadder) && cr > vm->startRow) return 1;
        if (vm->actWatch > 70) return 1;
        if (dir < 0) in.left = true; else in.right = true;
        *out = in; return 0;
    }
    case ACT_GOTO:
        if (vm->gotoImmediate) { vm->gotoResult = RES_BLOCKED; return 1; }
        if (dying)            { vm->gotoResult = RES_BLOCKED; return 1; }
        if (s->depth != vm->startDepth) { vm->gotoResult = RES_ARRIVED; return 1; }
        if (cc == vm->gotoFinalCol && cr == vm->gotoFinalRow) { vm->gotoResult = RES_ARRIVED; return 1; }
        if (vm->gotoIsExit) {
            if (vm->actWatch > 240) { vm->gotoResult = RES_BLOCKED; return 1; }
            in = mole_exit_input(s, vm);
            /* No move toward the shaft exists right now: report blocked at once
             * instead of standing mute for the whole watchdog — the script must
             * keep its sensors running (an enemy may be closing in meanwhile). */
            if (s->onGround && !s->onLadder && vm->gCommitCol < 0 &&
                !in.left && !in.right && !in.up && !in.down && !in.jump) {
                vm->gotoResult = RES_BLOCKED; return 1;
            }
            *out = in; return 0;
        }
        /* Anti-oscillation by region: while we stay within one cell of the anchor
         * we accumulate; moving 2+ cells off resets it. Vibrating between a ladder
         * and a platform never resets, so trip -> blocked (the script then hops).
         * Catches walk<->climb flip-flops that per-axis dither counters miss. */
        {
            int da = cc - vm->gAnchorCol, db = cr - vm->gAnchorRow;
            if (da < 0) da = -da; if (db < 0) db = -db;
            if (s->coalGot != vm->gLastCoal) {      /* progress: reset the backstop */
                vm->gLastCoal = s->coalGot; vm->gStill = 0;
                vm->gAnchorCol = cc; vm->gAnchorRow = cr;
            } else if (da <= 1 && db <= 1) {
                if (++vm->gStill > 70) {            /* ~1.4s stuck in one spot, no coal: escape */
                    vm->gStill = 0; vm->gAnchorCol = cc; vm->gAnchorRow = cr;
                    vm->gotoResult = RES_BLOCKED; return 1;
                }
            } else {
                vm->gAnchorCol = cc; vm->gAnchorRow = cr; vm->gStill = 0;
            }
        }
        /* A step finishes once we've settled a cell away — but NOT mid drop-commit:
         * committing to a drop first walks off the lip (still grounded for a tick or
         * two), and completing then would abort the drop and leave us dithering on
         * the edge. Wait for the commit to resolve (gCommitCol back to -1). */
        if (vm->gCommitCol < 0 && (s->onGround || s->onLadder) &&
            (cc != vm->startCol || cr != vm->startRow)) {
            vm->gotoResult = (cc == vm->gotoFinalCol && cr == vm->gotoFinalRow)
                               ? RES_ARRIVED : RES_MOVING;
            return 1;
        }
        if (vm->actWatch > 45) { vm->gotoResult = RES_BLOCKED; return 1; }  /* one tile shouldn't take longer */
        in = mole_decide(s, vm->gotoFinalCol, vm->gotoFinalRow, vm, 4);
        if (s->onGround && !s->onLadder && vm->gCommitCol < 0 &&
            !in.left && !in.right && !in.up && !in.down) { vm->gotoResult = RES_BLOCKED; return 1; }
        /* Anti-dither (bot1/bot2's trick, same semantics): a bot straddling a
         * platform lip gets a first-step that flips left/right/left as its cell
         * flickers on the edge. After a few flips, strip the horizontal flip —
         * ONLY the flip: up/down still pass, and the counter resets on climb
         * input or in free air (never letting it mute the bot permanently; the
         * backstop/watchdog above frees a genuine wedge). */
        {
            int hd = in.right ? 1 : (in.left ? -1 : 0);
            if ((!s->onGround && !s->onLadder) || in.up || in.down) vm->gDitherCount = 0;
            else if (hd && vm->gLastHDir && hd == -vm->gLastHDir) vm->gDitherCount++;
            if (hd) vm->gLastHDir = hd;
            if (vm->gDitherCount > 6) { in.left = in.right = false; }
        }
        *out = in; return 0;
    }
    return 1;
}

static void finish_action(MoleVM *vm)
{
    if (vm->act == ACT_GOTO) push(vm, v_num(vm->gotoResult));
    vm->act = -1;
    vm->ip++;
}

/* ============================================================= *
 *  Bytecode dispatch — run until an action starts or we must yield
 * ============================================================= */
enum { MR_STARTED, MR_HALT, MR_ERR };

static int run_until_action(MoleVM *vm, GameState *s)
{
    long ops = 0;
    while (vm->ok) {
        Instr *i;
        if (vm->ip < 0 || vm->ip >= vm->codeLen) return MR_HALT;
        if (++ops > 20000L) return MR_HALT;    /* runaway guard: idle this tick */
        i = &vm->code[vm->ip];
        switch (i->op) {
        case OP_PUSHC:   push(vm, v_num(vm->consts[i->a])); vm->ip++; break;
        case OP_PUSHB:   push(vm, v_bool(i->a)); vm->ip++; break;
        case OP_PUSHNONE:push(vm, v_none()); vm->ip++; break;
        case OP_POP:     pop(vm); vm->ip++; break;
        case OP_DUP: {
            Value v = pop(vm); push(vm, v); push(vm, v); vm->ip++; break;
        }
        case OP_LOADL:   push(vm, vm->frames[vm->fp].locals[i->a]); vm->ip++; break;
        case OP_STOREL:  vm->frames[vm->fp].locals[i->a] = pop(vm); vm->ip++; break;
        case OP_NEG:     push(vm, v_num(-v_numify(pop(vm)))); vm->ip++; break;
        case OP_NOT:     push(vm, v_bool(!v_truthy(pop(vm)))); vm->ip++; break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: {
            double b = v_numify(pop(vm)), a = v_numify(pop(vm)), r = 0;
            if (i->op == OP_ADD) r = a + b;
            else if (i->op == OP_SUB) r = a - b;
            else if (i->op == OP_MUL) r = a * b;
            else r = (b != 0.0) ? a / b : 0.0;
            push(vm, v_num(r)); vm->ip++; break;
        }
        case OP_EQ: case OP_NE: {
            Value b = pop(vm), a = pop(vm);
            int e = v_equal(a, b);
            push(vm, v_bool(i->op == OP_EQ ? e : !e)); vm->ip++; break;
        }
        case OP_LT: case OP_LE: case OP_GT: case OP_GE: {
            double b = v_numify(pop(vm)), a = v_numify(pop(vm)); int r = 0;
            if (i->op == OP_LT) r = a < b;
            else if (i->op == OP_LE) r = a <= b;
            else if (i->op == OP_GT) r = a > b;
            else r = a >= b;
            push(vm, v_bool(r)); vm->ip++; break;
        }
        case OP_JMP:  vm->ip = i->a; break;
        case OP_JMPF: { Value v = pop(vm); if (!v_truthy(v)) vm->ip = i->a; else vm->ip++; break; }
        case OP_JMPT: { Value v = pop(vm); if (v_truthy(v)) vm->ip = i->a; else vm->ip++; break; }
        case OP_CALL: {
            int argc = i->b, k;
            Frame *nf;
            if (vm->fp + 1 >= MOLE_MAX_FRAMES) { vm->ok = 0; return MR_ERR; }
            nf = &vm->frames[vm->fp + 1];
            nf->retIp = vm->ip + 1;
            for (k = argc - 1; k >= 0; k--) nf->locals[k] = pop(vm);
            for (k = argc; k < MOLE_MAX_LOCALS; k++) nf->locals[k] = v_none();
            vm->fp++;
            vm->ip = vm->funcs[i->a].entry;
            break;
        }
        case OP_RET: {
            Value rv = pop(vm);
            if (vm->fp <= 0) return MR_HALT;   /* return from main: fall to loop-back */
            vm->ip = vm->frames[vm->fp].retIp;
            vm->fp--;
            push(vm, rv);
            break;
        }
        case OP_BUILTIN: {
            Value args[4], r;
            int k, argc = i->b;
            if (argc > 4) argc = 4;
            for (k = argc - 1; k >= 0; k--) args[k] = pop(vm);
            r = eval_builtin(vm, s, i->a, args, argc);
            push(vm, r); vm->ip++; break;
        }
        case OP_MEMBER: {
            Value v = pop(vm), r = v_none();
            if (i->a == FLD_COL && (v.t == VAL_CELL || v.t == VAL_ENEMY)) r = v_num(v.col);
            else if (i->a == FLD_ROW && (v.t == VAL_CELL || v.t == VAL_ENEMY)) r = v_num(v.row);
            else if (i->a == FLD_DIR && v.t == VAL_ENEMY) r = v_num(v.edir);
            else if (i->a == FLD_DIST && v.t == VAL_ENEMY) r = v_num(v.edist);
            else if (i->a == FLD_TYPE && v.t == VAL_ENEMY) r = v_num(v.etype);
            else if (i->a == FLD_GOING && v.t == VAL_ENEMY) r = v_num(v.egoing);
            else if (i->a == FLD_STATE && v.t == VAL_ENEMY) r = v_num(v.estate);
            push(vm, r); vm->ip++; break;
        }
        case OP_PUSHS: {
            Value v = v_none(); v.t = VAL_STR; v.sidx = i->a;
            push(vm, v); vm->ip++; break;
        }
        case OP_SAY:
            mole_do_say(vm, pop(vm)); vm->ip++; break;
        case OP_ACT:
            start_action(vm, s, i->a, i->b);
            return MR_STARTED;             /* ip stays on the action op */
        case OP_HALT:
            return MR_HALT;
        default:
            vm->ok = 0; return MR_ERR;
        }
    }
    return MR_ERR;
}

/* ============================================================= *
 *  Public API
 * ============================================================= */
Input mole_input(MoleVM *vm, GameState *s)
{
    Input idle = {0};
    long guard = 0;
    if (!vm || !vm->ok) return idle;
    vm->tickSerial++;   /* says within one tick share a line; a new tick starts fresh */

    for (;;) {
        if (vm->act >= 0) {
            Input in;
            if (!service_action(vm, s, &in)) return in;   /* still running */
            finish_action(vm);                            /* advance past the op */
        }
        {
            int r = run_until_action(vm, s);
            /* Bound instant-completing actions per tick: a script that loops on
             * go_to()==blocked (or arrived) would otherwise re-run its sensors
             * thousands of times a tick. After a small budget, idle this tick and
             * let the game advance; the live arena's watchdog handles a truly
             * stuck bot. */
            if (r == MR_STARTED) { if (++guard > 64L) return idle; continue; }
            return idle;   /* HALT or ERR: idle this tick */
        }
    }
}

void mole_reset(MoleVM *vm, uint64_t seed)
{
    int k;
    if (!vm) return;
    vm->ip = vm->mainStart;
    vm->sp = 0;
    vm->fp = 0;
    vm->frames[0].retIp = -1;
    for (k = 0; k < MOLE_MAX_LOCALS; k++) vm->frames[0].locals[k] = v_none();
    vm->act = -1; vm->actWatch = 0; vm->actPhase = 0;
    vm->gCommitCol = -1;
    vm->gDitherCount = 0; vm->gLastHDir = 0;
    vm->gAnchorCol = -100; vm->gAnchorRow = -100; vm->gStill = 0; vm->gLastCoal = -1;
    { int o; for (o = 0; o < 6; o++) vm->order[o] = o; }   /* neutral tie-break until a go_to bias sets it */
    vm->seed = seed;
    vm->rng = mole_sm64(seed ^ 0x5D1E7A9CB3F02461ULL);
    vm->sayBuf[0] = '\0'; vm->sayPrev[0] = '\0'; vm->tickSerial = 0; vm->sayTick = -1;
    vm->ok = 1;
}

const char *mole_say(const MoleVM *vm)
{
    return vm ? vm->sayBuf : "";
}

static MoleVM *mole_alloc(void)
{
    MoleVM *vm = (MoleVM *)calloc(1, sizeof(MoleVM));
    if (!vm) return NULL;
    vm->act = -1;
    vm->err[0] = '\0';
    return vm;
}

MoleVM *mole_load(const char *src, uint64_t seed, char *errbuf, int errlen)
{
    MoleVM *vm;
    Lexer L;
    Comp c;

    if (!src) { if (errbuf && errlen) snprintf(errbuf, errlen, "no source"); return NULL; }
    vm = mole_alloc();
    if (!vm) { if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory"); return NULL; }

    /* Attribution: capture the required first-line comment (# author: ...). */
    {
        const char *p = src, *q;
        int n = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '#') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            /* drop an "author:" label if present, keeping just the name */
            if ((p[0] == 'a' || p[0] == 'A') && (p[1] == 'u' || p[1] == 'U') &&
                (p[2] == 't' || p[2] == 'T') && (p[3] == 'h' || p[3] == 'H') &&
                (p[4] == 'o' || p[4] == 'O') && (p[5] == 'r' || p[5] == 'R') && p[6] == ':') {
                p += 7;
                while (*p == ' ' || *p == '\t') p++;
            }
            q = p;
            while (*q && *q != '\n' && *q != '\r') q++;
            while (q > p && (q[-1] == ' ' || q[-1] == '\t')) q--;
            n = (int)(q - p); if (n > 63) n = 63;
            memcpy(vm->author, p, (size_t)n);
        }
        vm->author[n] = '\0';
    }

    lex(&L, src);
    if (L.err) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "line %d: %s", L.errLine, L.err);
        free(L.t); free(vm); return NULL;
    }

    memset(&c, 0, sizeof c);
    c.vm = vm; c.toks = L.t; c.nToks = L.n; c.tp = 0; c.loopSp = 0;
    if (setjmp(c.jb)) {
        if (errbuf && errlen) snprintf(errbuf, errlen, "%s", vm->err);
        free(L.t); mole_free(vm); return NULL;
    }
    compile(&c);
    free(L.t);

    mole_reset(vm, seed);
    if (errbuf && errlen) errbuf[0] = '\0';
    return vm;
}

MoleVM *mole_load_file(const char *path, uint64_t seed, char *errbuf, int errlen)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf;
    MoleVM *vm;
    if (!f) { if (errbuf && errlen) snprintf(errbuf, errlen, "cannot open '%s'", path); return NULL; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); if (errbuf && errlen) snprintf(errbuf, errlen, "cannot read '%s'", path); return NULL; }
    if (n > MOLE_MAX_SRC) { fclose(f); if (errbuf && errlen) snprintf(errbuf, errlen, "'%s' too large (%ld bytes, max %d)", path, n, MOLE_MAX_SRC); return NULL; }
    buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); if (errbuf && errlen) snprintf(errbuf, errlen, "out of memory"); return NULL; }
    n = (long)fread(buf, 1, (size_t)n, f);
    buf[n] = '\0';
    fclose(f);
    vm = mole_load(buf, seed, errbuf, errlen);
    free(buf);
    return vm;
}

const char *mole_author(const MoleVM *vm)
{
    return (vm && vm->author[0]) ? vm->author : "unknown";
}

void mole_free(MoleVM *vm)
{
    if (!vm) return;
    free(vm->code);
    free(vm->consts);
    free(vm);
}
