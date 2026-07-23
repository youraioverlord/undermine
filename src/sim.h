/* UNDERMINE — pure simulation layer.
 * No raylib, no I/O, no entropy: tick(state, input) is fully deterministic.
 * All constants are the DESIGN.md §6.2 values, at a fixed 50 Hz tick. */
#ifndef SIM_H
#define SIM_H

#include <stdbool.h>
#include <stdint.h>

#define TICK_RATE     50
#define TILE          16      /* metatile size in px */
#define ROOM_W        20      /* metatiles */
#define ROOM_H        16
#define HUD_H         0       /* HUD is overlaid on the play field, no reserved band */
#define SCREEN_W      320     /* per screen; the window shows two side by side */
#define SCREEN_H      256

#define WALK_SPEED    1.53f   /* 1.5 + 2% — player/bot walk speed (enemies use their own) */
#define JUMP_VY      -3.5f
#define GRAVITY       0.25f
#define TERMINAL_VY   4.0f
#define CLIMB_SPEED   1.0f
#define AIR_ACCEL     0.125f  /* gentle mid-air steering, capped at WALK_SPEED */
#define HITBOX_W      10
#define HITBOX_H      18
#define FALL_DEATH_PX 64.0f   /* falling farther than this kills */
#define DYING_TICKS   24
#define CRUMBLE_TICKS 60      /* a weak floor holds this many ticks once stood on (~1.2s: time
                               * for the player to step off and a chasing foreman to step on) */
#define BOULDER_SPAWN_DELAY (2 * TICK_RATE)   /* boulder stays away 2s after a (re)spawn */

#define BEDROCK_ROW   (ROOM_H - 1)   /* solid floor at the bottom of every room */
#define DEFAULT_SEED  0xC0FFEEULL
#define MAX_REROLLS   5              /* per DESIGN.md §5.3: reroll then fall back */

#define MAX_ENEMIES   5              /* + player keeps us within the 8-sprite VIC-II budget */
#define FOREMAN_SPEED 0.72765f          /* 0.75 - 2%, then - 1% */
#define FOREMAN_CLIMB (CLIMB_SPEED * 0.95f)  /* foremen climb ladders 5% slower */
#define MAX_PLATS     3
#define PLAT_W        32             /* 2 tiles wide */
#define PLAT_H        6              /* thin ledge */
#define PLAT_SPEED    0.5f
#define SPIDER_SPEED  0.5f
#define BOULDER_SPEED 1.0f
#define BOULDER_FALL  2.5f

typedef enum {
    TILE_EMPTY = 0,
    TILE_SOLID,
    TILE_ONEWAY,   /* platform: solid only from above */
    TILE_LADDER,
    TILE_SPIKE,
    TILE_EXIT,
    TILE_CRUMBLE   /* weak floor: solid until stood on, then collapses */
} TileType;

typedef enum {
    PS_ALIVE = 0,
    PS_DYING,
    PS_GAMEOVER   /* out of lives — the shell (main.c) takes over */
} PlayerState;

typedef enum {
    EN_NONE = 0,
    EN_CRUSHER,   /* ceiling piston, vertical timing hazard */
    EN_FOREMAN,   /* patrols platforms, climbs ladders */
    EN_BAT,       /* flies a diagonal sweep, bounces off terrain */
    EN_SPIDER,    /* hangs from a ceiling, bobs up/down on a thread */
    EN_BOULDER    /* rolls along platforms, falls off edges */
} EnemyType;

/* Moving platform — a dynamic solid the player can stand on and ride.
 * Additive to the static room: never the only way across, so the tile-based
 * solvability checker stays correct. Deterministic fixed-step motion. */
typedef struct {
    float x, y, baseX, baseY;
    int   range;       /* travel extent in px */
    int   axis;        /* 0 = horizontal, 1 = vertical */
    int   dir;         /* +1 / -1 */
    float dx, dy;      /* delta moved this tick (for carrying a rider) */
} MovePlat;

typedef struct {
    EnemyType type;
    float x, y;        /* hitbox/sprite top-left, playfield px (HUD excluded) */
    float baseX, baseY;/* spawn anchor */
    int   range;       /* travel extent in px (crusher drop / patrol / sweep) */
    int   dir;         /* +1 / -1 horizontal travel direction */
    int   vdir;        /* vertical dir: foreman climb / bat bounce (+1 down, -1 up) */
    int   mode;        /* foreman: 0 = walk, 1 = climb a ladder */
    int   phase;       /* crusher state machine */
    int   timer;       /* phase / animation counter */
    int   period;      /* crusher full-cycle length in ticks (depth-scaled) */
    int   carry;       /* foreman coal transport: 0 idle, >0 carrying (drop countdown), <0 cooldown */
    int   falling;     /* ground sprite riding a collapsed crumble floor (0 = no) */
    float fallY0;      /* y where the current fall began, for the drop-death check */
    int   spawnDelay;  /* boulder: ticks parked off-screen before it drops in (0 = active) */
} Enemy;

typedef struct {
    bool left, right, up, down;
    bool jump;   /* edge-triggered: true only on the tick it was pressed */
} Input;

typedef struct {
    TileType tiles[ROOM_H][ROOM_W];
    bool     coal[ROOM_H][ROOM_W];   /* collectibles, one per metatile cell */
    signed char crumble[ROOM_H][ROOM_W]; /* for TILE_CRUMBLE: 0 intact, >0 arming, -1 gone */

    /* player — px,py is hitbox top-left in playfield pixels (HUD excluded) */
    float px, py;
    float vx, vy;
    int   facing;        /* -1 left, +1 right */
    bool  onGround;
    bool  onLadder;
    PlayerState state;
    int   dyingTimer;

    /* fall-death tracking */
    bool  fallTracking;
    float fallStartY;

    int   animTimer;

    /* room / run */
    uint64_t runSeed;
    uint64_t rng;              /* deterministic tick-time PRNG (foreman choices) */
    int   entryCol, exitCol;   /* shaft columns; entry matches prev room's exit */
    int   spawnCol;
    int   coalTotal, coalGot;  /* quota = coalTotal; exit opens when got == total */
    bool  usedFallback;        /* this room came from the template, not the generator */

    Enemy enemies[MAX_ENEMIES];
    Enemy enemyStart[MAX_ENEMIES];   /* level-start snapshot, for respawn resets */
    int   enemyCount;

    MovePlat plats[MAX_PLATS];
    int   platCount;
    int   ridingPlat;   /* index of the platform under the player, or -1 */

    /* static floor layout, recorded at generation for anchoring moving platforms */
    int   fixRow[32], fixLo[32], fixHi[32], fixCount;
    int   mainLedges;   /* count of main platform rows (top-to-bottom), fixRow[0..mainLedges-1] */

    /* special rooms */
    bool  keyRoom;  bool keyGot;  int keyCol, keyRow;    /* exit also needs a key (depth >=10) */
    bool  darkRoom; bool lampGot; int lampCol, lampRow;  /* limited sight until lamp (depth >=20) */

    /* death memory — where the player last died in THIS room (deaths reset
     * the enemies, so an unchanged route replays the same death forever; a
     * bot that knows the spot can respect it). Cleared on every new room. */
    int   deathCol, deathRow;   /* feet cell of the last death; -1 = none yet */
    int   deaths;               /* deaths in this room so far */
    int   totalDeaths;          /* deaths across the whole run (for the leaderboard) */

    int   lives;
    int   depth;
    long  score;
    long  ticks;
} GameState;

void     sim_init(GameState *s);              /* new run, DEFAULT_SEED, depth 1 */
void     sim_init_seed(GameState *s, uint64_t seed);
void     sim_tick(GameState *s, Input in);
/* Jump a run to a specific room/depth on its own seed (same room a player would
 * reach descending there). Used to advance the bot to the player's room. */
void     sim_sync_room(GameState *s, int depth);
void     sim_kill_player(GameState *s);   /* lose a life & restart the room (e.g. out of air) */
TileType sim_tile_at(const GameState *s, int tx, int ty);

/* Generation / validation — exposed for property tests (main.c --selftest). */
void     sim_gen_room(GameState *s, uint64_t seed, int depth, int entryCol);
bool     sim_room_solvable(const GameState *s);  /* BFS reachability, exit closed */
void     sim_cull_blocking_crushers(GameState *s); /* §5.5: delete crushers severing the only path */
bool     sim_standable(const GameState *s, int c, int r);  /* body-aware foothold test */
/* Would a player-sized box at (px,py), grown by `margin` px, touch any enemy's
 * lethal zone? Used by the demo bot to steer clear of enemies. */
bool     sim_enemy_threat(const GameState *s, float px, float py, float margin);
bool     sim_enemy_threat_lad(const GameState *s, float px, float py, float margin);  /* ignores crushers + spiders */
bool     sim_enemy_threat_nocr(const GameState *s, float px, float py, float margin); /* ignores crushers only */
int      sim_crusher_safe_ticks(const Enemy *e);  /* remaining guaranteed-raised ticks; 0 if armed */
int      sim_entry_col_for(uint64_t seed, int depth);

#endif
