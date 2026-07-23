#include "sim.h"
#include <math.h>
#include <string.h>

/* ============================================================= *
 *  Deterministic PRNG: splitmix64 seeds xoshiro256**.
 *  The ONLY source of variation in the game — no rand(), no time.
 * ============================================================= */
typedef struct { uint64_t s[4]; } Rng;

static void spawn_player(GameState *s);                     /* defined below */
static void place_enemies(GameState *s, Rng *r, int depth); /* defined below */
static void place_plats(GameState *s, Rng *r, int depth);   /* defined below */

static uint64_t splitmix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

static void rng_seed(Rng *r, uint64_t seed)
{
    int i;
    for (i = 0; i < 4; i++) { seed = splitmix64(seed); r->s[i] = seed; }
}

static uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }

static uint64_t rng_next(Rng *r)
{
    uint64_t *s = r->s;
    uint64_t result = rotl(s[1] * 5, 7) * 9;
    uint64_t t = s[1] << 17;
    s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
    s[2] ^= t; s[3] = rotl(s[3], 45);
    return result;
}

/* uniform in [0, n) */
static int rng_range(Rng *r, int n) { return n <= 0 ? 0 : (int)(rng_next(r) % (uint64_t)n); }

/* Deterministic tick-time randomness: advances the PRNG stored in GameState, so
 * two runs of the same seed still tick identically (replays / daily seed hold). */
static int sim_rand(GameState *s, int n)
{
    s->rng = splitmix64(s->rng);
    return n <= 0 ? 0 : (int)((s->rng >> 33) % (uint64_t)n);
}

static int iclamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int iabs(int v) { return v < 0 ? -v : v; }

/* ============================================================= *
 *  Tile queries
 * ============================================================= */
TileType sim_tile_at(const GameState *s, int tx, int ty)
{
    if (tx < 0 || tx >= ROOM_W) return TILE_SOLID; /* side walls */
    if (ty < 0)                 return TILE_EMPTY; /* open shaft above */
    if (ty >= ROOM_H)           return TILE_SOLID; /* bedrock below */
    /* a weak floor is solid until it has collapsed — then it's just air, so
     * all collision and the checker treat it correctly through this one point */
    if (s->tiles[ty][tx] == TILE_CRUMBLE)
        return (s->crumble[ty][tx] < 0) ? TILE_EMPTY : TILE_SOLID;
    return s->tiles[ty][tx];
}

static bool tile_blocks(const GameState *s, int tx, int ty)
{
    return sim_tile_at(s, tx, ty) == TILE_SOLID;
}

/* ============================================================= *
 *  Room generation — platform-and-ladder chain.
 *
 *  DESIGN.md §5.3 calls for a drunkard's walk carve; we build the
 *  *converted result* directly: horizontal floors joined by ladders
 *  whose column lies inside both the platform above and below. This
 *  is connected by construction, so rooms are solvable without
 *  relying on rerolls; the BFS checker still validates every room.
 * ============================================================= */

/* Exit column depends only on (seed, depth, entryCol) — NOT on the reroll
 * attempt, so the shaft stays aligned even when the layout is rerolled. */
static int pick_exit_col(uint64_t seed, int depth, int entryCol)
{
    Rng r;
    int e;
    rng_seed(&r, splitmix64(seed ^ (0x9E3779B97F4A7C15ULL * (uint64_t)depth)));
    do { e = 2 + rng_range(&r, ROOM_W - 4); }      /* [2, ROOM_W-3] */
    while (iabs(e - entryCol) < 6);                /* §5.3: >=6 offset */
    return e;
}

static int start_col(uint64_t seed) { return 3 + (int)(splitmix64(seed) % 4); } /* 3..6 */

int sim_entry_col_for(uint64_t seed, int depth)
{
    int c = start_col(seed), d;
    for (d = 2; d <= depth; d++) c = pick_exit_col(seed, d - 1, c);
    return c;
}

static void put_floor(GameState *s, int row, int lo, int hi)
{
    int c;
    lo = iclamp(lo, 0, ROOM_W - 1);
    hi = iclamp(hi, 0, ROOM_W - 1);
    for (c = lo; c <= hi; c++) s->tiles[row][c] = TILE_SOLID;
}

/* Carve a ladder column from upper floor `a` down to just above floor `b`. */
static void carve_ladder(GameState *s, int col, int a, int b)
{
    int rr;
    for (rr = a; rr < b; rr++) s->tiles[rr][col] = TILE_LADDER;
}

/* One generation attempt into a cleared state; returns coal count placed. */
static void gen_attempt(GameState *s, Rng *r, int entryCol, int exitCol)
{
    int rows[ROOM_H], K = 0;
    int lcol[ROOM_H];
    int lo[ROOM_H], hi[ROOM_H];
    int i, row, center, coalWant, placed = 0, guard;

    memset(s->tiles, 0, sizeof s->tiles);
    memset(s->coal, 0, sizeof s->coal);
    memset(s->crumble, 0, sizeof s->crumble);

    /* bedrock */
    put_floor(s, BEDROCK_ROW, 0, ROOM_W - 1);

    /* Platform rows descend from the top. Consecutive floors (and the gap to
     * bedrock) must be >= 3 rows apart: Monty is 18px tall, so his body fills
     * the feet cell AND the head cell above it. A floor only 2 rows above a
     * walkway sits at head height and would wall off horizontal movement. */
    row = 2;
    rows[K++] = row;
    while (1) {
        int nr = row + 3 + rng_range(r, 2);          /* +3 or +4 */
        if (nr > BEDROCK_ROW - 3) break;             /* keep headroom over bedrock */
        rows[K++] = nr;
        row = nr;
    }

    /* platform spans + ladder columns, drifting toward the exit */
    center = entryCol;
    for (i = 0; i < K; i++) {
        int w1 = 2 + rng_range(r, 3);            /* left half  2..4 */
        int w2 = 2 + rng_range(r, 3);            /* right half 2..4 */
        int target, step, c;
        lo[i] = iclamp(center - w1, 0, ROOM_W - 1);
        hi[i] = iclamp(center + w2, 0, ROOM_W - 1);
        put_floor(s, rows[i], lo[i], hi[i]);

        /* choose ladder column inside this span, biased toward the exit */
        target = (i == K - 1) ? exitCol : exitCol;
        step = (target > center) ? 1 : -1;
        c = center + step * rng_range(r, (hi[i] - lo[i]) / 2 + 1);
        lcol[i] = iclamp(c, lo[i], hi[i]);

        center = lcol[i];   /* next platform is centered on this ladder */
    }

    /* carve ladders: platform i -> platform i+1 (last -> bedrock) */
    for (i = 0; i < K; i++) {
        int a = rows[i];
        int b = (i + 1 < K) ? rows[i + 1] : BEDROCK_ROW;
        carve_ladder(s, lcol[i], a, b);
    }

    /* record the static floors (+ bedrock) so moving platforms can anchor to them */
    s->fixCount = 0;
    for (i = 0; i < K; i++) {
        s->fixRow[s->fixCount] = rows[i];
        s->fixLo[s->fixCount] = lo[i];
        s->fixHi[s->fixCount] = hi[i];
        s->fixCount++;
    }
    s->mainLedges = K;   /* fixRow[0..K-1] are the main platform rows, top-to-bottom */
    s->fixRow[s->fixCount] = BEDROCK_ROW;
    s->fixLo[s->fixCount] = 0;
    s->fixHi[s->fixCount] = ROOM_W - 1;
    s->fixCount++;

    /* Extra ledges: more than one platform per height. Each is tied by its own
     * ladder to a neighbouring fixed platform (the row above or below), so the room
     * becomes a branching maze — reaching all the coal means winding up and down —
     * while every ledge stays reachable by walk/climb (the checker still verifies,
     * and the bots need no jumps). */
    {
        int want = 5 + rng_range(r, 5), made = 0, tries;   /* 5..9 extra ledges: busy rooms */
        for (tries = 0; tries < 120 && made < want && s->fixCount < 30; tries++) {
            int i = rng_range(r, K);
            int up = (i > 0) && rng_range(r, 2);      /* tie to the row above, else below */
            int j = up ? i - 1 : i + 1;
            int jrow = (j < K) ? rows[j] : BEDROCK_ROW;
            int jlo  = (j < K) ? lo[j]   : 0;
            int jhi  = (j < K) ? hi[j]   : (ROOM_W - 1);
            int lc   = jlo + rng_range(r, jhi - jlo + 1);   /* ladder col within neighbour */
            int elo  = iclamp(lc - (1 + rng_range(r, 3)), 0, ROOM_W - 1);
            int ehi  = iclamp(lc + (1 + rng_range(r, 3)), 0, ROOM_W - 1);
            int top  = rows[i] < jrow ? rows[i] : jrow;
            int bot  = rows[i] < jrow ? jrow : rows[i];
            int cx, clear = 1;
            if (!(elo > hi[i] + 1 || ehi < lo[i] - 1)) continue;   /* keep a gap from the main ledge */
            for (cx = elo; cx <= ehi; cx++)
                if (s->tiles[rows[i]][cx] != TILE_EMPTY) { clear = 0; break; }
            if (!clear) continue;
            put_floor(s, rows[i], elo, ehi);
            carve_ladder(s, lc, top, bot);
            s->fixRow[s->fixCount] = rows[i];
            s->fixLo[s->fixCount]  = elo;
            s->fixHi[s->fixCount]  = ehi;
            s->fixCount++;
            made++;
        }
    }

    /* coal: spread over the standing cells (row above floor) of ANY fixed platform
     * — mains, extra ledges and bedrock — so collecting it all takes a winding path */
    coalWant = 9 + rng_range(r, 7);              /* 9..15 */
    guard = 0;
    while (placed < coalWant && guard++ < 600) {
        int p = rng_range(r, s->fixCount);
        int frow = s->fixRow[p];
        int flo  = s->fixLo[p];
        int fhi  = s->fixHi[p];
        int c = flo + rng_range(r, fhi - flo + 1);
        int stand = frow - 1;
        if (stand < 1) continue;
        if (s->coal[stand][c]) continue;         /* already coal here */
        if (s->tiles[stand][c] != TILE_EMPTY) continue;
        s->coal[stand][c] = true;
        placed++;
    }
    s->coalTotal = placed;
    s->coalGot = 0;

    /* Hide a key (key rooms) and/or a lamp (dark rooms) on reachable platform
     * cells, clear of coal and each other. Degrade the room type if we can't. */
    #define PLACE_PICKUP(OKCOL, OKROW, AVOIDC, AVOIDR)                        \
        do { int g2 = 0; OKCOL = -1;                                         \
            while (g2++ < 300) {                                            \
                int p = rng_range(r, s->fixCount);                          \
                int fr = s->fixRow[p];                                      \
                int fl = s->fixLo[p];                                       \
                int fh = s->fixHi[p];                                       \
                int cc = fl + rng_range(r, fh - fl + 1), st = fr - 1;       \
                if (st < 1 || s->coal[st][cc]) continue;                    \
                if (s->tiles[st][cc] != TILE_EMPTY) continue;              \
                if (cc == (AVOIDC) && st == (AVOIDR)) continue;            \
                OKCOL = cc; OKROW = st; break;                              \
            } } while (0)

    if (s->keyRoom) {
        PLACE_PICKUP(s->keyCol, s->keyRow, -9, -9);
        if (s->keyCol < 0) s->keyRoom = false;
    }
    if (s->darkRoom) {
        PLACE_PICKUP(s->lampCol, s->lampRow, s->keyCol, s->keyRow);
        if (s->lampCol < 0) s->darkRoom = false;
    }
    #undef PLACE_PICKUP

    /* weak floor: turn a short run of one platform's floor into crumbling tiles
     * (an "unstable" section). Keep the ladder attachment solid, and clear any
     * coal that would be stranded above it so nothing becomes uncollectible.
     * At generation these read as solid, so the checker is unaffected. */
    if (s->depth >= 4 && K > 0) {
        int gp = rng_range(r, K);
        int frow = rows[gp];
        int runLen = 2 + rng_range(r, 2);
        int start = lo[gp] + rng_range(r, hi[gp] - lo[gp] + 1);
        int cx;
        for (cx = start; cx < start + runLen && cx <= hi[gp]; cx++) {
            if (cx == lcol[gp]) continue;               /* keep ladder attachment solid */
            if (s->tiles[frow][cx] != TILE_SOLID) continue;
            s->tiles[frow][cx] = TILE_CRUMBLE;
            if (s->coal[frow - 1][cx]) { s->coal[frow - 1][cx] = false; s->coalTotal--; }
        }
    }
}

/* ============================================================= *
 *  Solvability checker — jump/climb/drop-aware BFS over the grid,
 *  run with the exit CLOSED (bedrock solid). DESIGN.md §5.5.
 * ============================================================= */
static bool is_ladder(const GameState *s, int c, int r) { return sim_tile_at(s, c, r) == TILE_LADDER; }

/* Monty is ~18px tall: his body fills the feet cell AND the head cell above.
 * A cell is occupiable only if BOTH are clear of solids. This is what keeps
 * the checker in step with the physics — a platform at head height blocks
 * passage even though the feet cell itself is empty. */
static bool occupiable(const GameState *s, int c, int r)
{
    if (c < 0 || c >= ROOM_W || r < 0 || r >= ROOM_H) return false;
    if (sim_tile_at(s, c, r) == TILE_SOLID) return false;      /* crumble reads solid */
    if (r - 1 >= 0 && sim_tile_at(s, c, r - 1) == TILE_SOLID) return false;  /* head */
    return true;
}

/* Can the player's feet occupy cell (c,r), standing or hanging there? */
static bool standable(const GameState *s, int c, int r)
{
    TileType below;
    if (!occupiable(s, c, r)) return false;
    if (s->tiles[r][c] == TILE_LADDER) return true;    /* hanging on a ladder */
    below = sim_tile_at(s, c, r + 1);
    return below == TILE_SOLID || below == TILE_ONEWAY || below == TILE_LADDER;
}

bool sim_room_solvable(const GameState *s)
{
    bool seen[ROOM_H][ROOM_W];
    int qx[ROOM_H * ROOM_W], qy[ROOM_H * ROOM_W], head = 0, tail = 0;
    int startC = s->spawnCol, startR = -1, r, c, dd, dx, dy;
    int coalLeft = 0;

    memset(seen, 0, sizeof seen);

    /* entry: first standable cell below the spawn column (fall-in landing) */
    for (r = 0; r < ROOM_H; r++)
        if (standable(s, startC, r)) { startR = r; break; }
    if (startR < 0) return false;

    seen[startR][startC] = true;
    qx[tail] = startC; qy[tail] = startR; tail++;

    while (head < tail) {
        c = qx[head]; r = qy[head]; head++;

        /* walk left/right */
        for (dx = -1; dx <= 1; dx += 2) {
            int nc = c + dx;
            if (standable(s, nc, r) && !seen[r][nc]) {
                seen[r][nc] = true; qx[tail] = nc; qy[tail] = r; tail++;
            }
        }
        /* climb up / down through ladders (and step onto a platform on top) */
        if (is_ladder(s, c, r) || is_ladder(s, c, r - 1)) {
            if (r - 1 >= 0 && standable(s, c, r - 1) && !seen[r - 1][c]) {
                seen[r - 1][c] = true; qx[tail] = c; qy[tail] = r - 1; tail++;
            }
        }
        if (is_ladder(s, c, r) || is_ladder(s, c, r + 1)) {
            if (r + 1 < ROOM_H && standable(s, c, r + 1) && !seen[r + 1][c]) {
                seen[r + 1][c] = true; qx[tail] = c; qy[tail] = r + 1; tail++;
            }
        }
        /* drop off a ledge to the sides (fall up to 4 metatiles) */
        for (dx = -1; dx <= 1; dx += 2) {
            int nc = c + dx;
            if (nc < 0 || nc >= ROOM_W) continue;
            if (standable(s, nc, r)) continue;                       /* handled by walk */
            if (sim_tile_at(s, nc, r) == TILE_SOLID) continue;       /* wall, can't enter */
            if (r - 1 >= 0 && sim_tile_at(s, nc, r - 1) == TILE_SOLID) continue; /* head */
            for (dd = 1; dd <= 4; dd++) {
                int nr = r + dd;
                if (nr >= ROOM_H) break;
                if (standable(s, nc, nr) && !seen[nr][nc]) {
                    seen[nr][nc] = true; qx[tail] = nc; qy[tail] = nr; tail++;
                    break;
                }
                if (sim_tile_at(s, nc, nr) == TILE_SOLID) break;
            }
        }
        /* jump: within the base envelope (<=2 up, <=3 across), approximate */
        for (dy = 0; dy <= 2; dy++) {
            for (dx = -3; dx <= 3; dx++) {
                int nc = c + dx, nr = r - dy;
                if (dx == 0 && dy == 0) continue;
                if (standable(s, nc, nr) && !seen[nr][nc]) {
                    seen[nr][nc] = true; qx[tail] = nc; qy[tail] = nr; tail++;
                }
            }
        }
    }

    /* every coal cell must be reachable */
    for (r = 0; r < ROOM_H; r++)
        for (c = 0; c < ROOM_W; c++)
            if (s->coal[r][c]) { coalLeft++; if (!seen[r][c]) return false; }

    /* the cell above the exit shaft must be reachable */
    if (!seen[BEDROCK_ROW - 1][s->exitCol]) return false;

    /* special-room pickups must be reachable too */
    if (s->keyRoom  && s->keyCol  >= 0 && !seen[s->keyRow][s->keyCol])   return false;
    if (s->darkRoom && s->lampCol >= 0 && !seen[s->lampRow][s->lampCol]) return false;

    return coalLeft > 0;   /* a room with no coal would open its exit instantly */
}

bool sim_standable(const GameState *s, int c, int r) { return standable(s, c, r); }

/* ============================================================= *
 *  Fallback template — a fixed, always-solvable staircase. Stamped
 *  when 5 rerolls all fail, so generation never blocks the game.
 * ============================================================= */
static void stamp_fallback(GameState *s, int entryCol, int exitCol)
{
    int rows[3] = {3, 6, 9};
    int i, c;
    memset(s->tiles, 0, sizeof s->tiles);
    memset(s->coal, 0, sizeof s->coal);
    put_floor(s, BEDROCK_ROW, 0, ROOM_W - 1);
    for (i = 0; i < 3; i++) put_floor(s, rows[i], 1, ROOM_W - 2);
    carve_ladder(s, entryCol, rows[0], rows[1]);
    carve_ladder(s, entryCol, rows[1], rows[2]);
    carve_ladder(s, exitCol,  rows[2], BEDROCK_ROW);
    for (i = 0; i < 3; i++)
        for (c = 3; c < ROOM_W - 3; c += 4) s->coal[rows[i] - 1][c] = true;
    s->coalTotal = 0;
    for (i = 0; i < ROOM_H; i++)
        for (c = 0; c < ROOM_W; c++) if (s->coal[i][c]) s->coalTotal++;
    s->coalGot = 0;
    s->keyRoom = false; s->darkRoom = false;   /* template rooms stay simple */
    s->fixCount = 0;                            /* no moving platforms in templates */
    s->usedFallback = true;
}

void sim_gen_room(GameState *s, uint64_t seed, int depth, int entryCol)
{
    int exitCol = pick_exit_col(seed, depth, entryCol);
    int attempt;

    s->runSeed = seed;
    s->rng = splitmix64(seed ^ (0x2545F4914F6CDD1DULL * (uint64_t)depth) ^ 0xF0E1D2ULL);
    s->depth = depth;
    s->entryCol = entryCol;
    s->exitCol = exitCol;
    s->spawnCol = entryCol;
    s->usedFallback = false;

    /* Deterministic room type (independent of reroll): key rooms from depth 10,
     * dark rooms from depth 20; mutually exclusive, occasional. Depth 1 is
     * always a dark room, so every run opens with the lamp mechanic on show. */
    {
        uint64_t h = splitmix64(seed ^ (0x51ED51EDULL * (uint64_t)depth));
        bool wantKey  = (depth >= 10) && (h % 4 == 0);
        bool wantDark = (depth == 1) ||
                        ((depth >= 20) && !wantKey && (h % 4 == 1));
        bool ok = false;
        s->keyGot = false; s->lampGot = false;
        s->keyCol = s->keyRow = s->lampCol = s->lampRow = -1;
        s->deathCol = s->deathRow = -1; s->deaths = 0;   /* a fresh room holds no grudges */
        for (attempt = 0; attempt < MAX_REROLLS; attempt++) {
            Rng r;
            s->keyRoom = wantKey; s->darkRoom = wantDark;
            rng_seed(&r, splitmix64(seed ^ ((uint64_t)depth << 8) ^ (uint64_t)attempt));
            gen_attempt(s, &r, entryCol, exitCol);
            if (sim_room_solvable(s)) { ok = true; break; }
        }
        if (!ok) stamp_fallback(s, entryCol, exitCol);
    }

    /* Enemies are placed after the room geometry is fixed, from an independent
     * RNG stream so the reroll count can't shift them. The solvability checker
     * (§5.5) ignores enemies — they are timing obstacles, not walls. */
    {
        Rng er;
        rng_seed(&er, splitmix64(seed ^ ((uint64_t)depth << 16) ^ 0xE1E1ULL));
        place_enemies(s, &er, depth);
        memcpy(s->enemyStart, s->enemies, sizeof s->enemyStart);  /* for respawn resets */
    }
    {
        Rng pr;
        rng_seed(&pr, splitmix64(seed ^ ((uint64_t)depth << 24) ^ 0x9AAFULL));
        place_plats(s, &pr, depth);
    }
    spawn_player(s);
}

/* ============================================================= *
 *  Player physics (unchanged from milestone 1)
 * ============================================================= */
/* Before resetting enemies on respawn, put back any coal a foreman was carrying —
 * otherwise the reset would wipe its carry flag and that lump would be lost, making
 * the room unwinnable. Dropped on the foreman's own cell if free, else the first
 * empty standable cell (always reachable — the room is connected by construction). */
static void return_one_coal(GameState *s, const Enemy *e)
{
    int rr, cc;
    int fc = (int)((e->x + 6.0f) / TILE);
    int fr = (int)((e->y + HITBOX_H) / TILE) - 1;
    if (fc >= 0 && fc < ROOM_W && fr >= 0 && fr < ROOM_H &&
        sim_tile_at(s, fc, fr) == TILE_EMPTY && !s->coal[fr][fc] && standable(s, fc, fr)) {
        s->coal[fr][fc] = true; return;
    }
    for (rr = 0; rr < ROOM_H; rr++) for (cc = 0; cc < ROOM_W; cc++)
        if (sim_tile_at(s, cc, rr) == TILE_EMPTY && !s->coal[rr][cc] && standable(s, cc, rr)) {
            s->coal[rr][cc] = true; return;   /* placed; stop scanning */
        }
}

static void return_carried_coal(GameState *s)
{
    int i;
    for (i = 0; i < s->enemyCount; i++)
        if (s->enemies[i].type == EN_FOREMAN && s->enemies[i].carry > 0)
            return_one_coal(s, &s->enemies[i]);
}

static void spawn_player(GameState *s)
{
    s->px = (float)(s->spawnCol * TILE + (TILE - HITBOX_W) / 2);
    s->py = 0.0f;                 /* fall in from the shaft above */
    s->vx = 0; s->vy = 0;
    s->facing = 1;
    s->onGround = false;
    s->onLadder = false;
    s->fallTracking = false;
    s->ridingPlat = -1;
    memset(s->crumble, 0, sizeof s->crumble);   /* restore any collapsed weak floors */

    /* On (re)spawn, restore every enemy to its level-start position — the classic
     * room-restart, and the only reliable way to guarantee the player isn't killed
     * the instant they reappear (the killer is wherever you died, not at spawn). */
    return_carried_coal(s);
    memcpy(s->enemies, s->enemyStart, sizeof s->enemyStart);
    /* Hold each boulder off-screen for 2s after a (re)spawn, then it drops in from
     * the top — so the player/bot isn't greeted by one the instant it appears. */
    {
        int i;
        for (i = 0; i < s->enemyCount; i++)
            if (s->enemies[i].type == EN_BOULDER) {
                s->enemies[i].spawnDelay = BOULDER_SPAWN_DELAY;
                s->enemies[i].x = -1000.0f; s->enemies[i].y = -1000.0f;   /* parked */
            }
    }
    s->state = PS_ALIVE;
}

static void die(GameState *s)
{
    s->state = PS_DYING;
    s->dyingTimer = DYING_TICKS;
    /* remember WHERE: deaths reset the enemies, so an unchanged route replays
     * the identical death — bots that know the spot can respect it */
    s->deathCol = (int)((s->px + HITBOX_W * 0.5f) / TILE);
    s->deathRow = (int)((s->py + HITBOX_H - 1) / TILE);
    s->deaths++;
    s->totalDeaths++;
}

/* Kill the player (loses a life, restarts the room) — e.g. when the air runs out. */
void sim_kill_player(GameState *s) { if (s->state == PS_ALIVE) die(s); }

static void move_x(GameState *s, float dx)
{
    int top, bot, ty, col;
    if (dx == 0) return;
    s->px += dx;
    top = (int)floorf(s->py / TILE);
    bot = (int)floorf((s->py + HITBOX_H - 1) / TILE);
    if (dx > 0) {
        col = (int)floorf((s->px + HITBOX_W - 1) / TILE);
        for (ty = top; ty <= bot; ty++)
            if (tile_blocks(s, col, ty)) { s->px = (float)(col * TILE - HITBOX_W); break; }
    } else {
        col = (int)floorf(s->px / TILE);
        for (ty = top; ty <= bot; ty++)
            if (tile_blocks(s, col, ty)) { s->px = (float)((col + 1) * TILE); break; }
    }
}

static void move_y(GameState *s, float dy)
{
    float prevBottom = s->py + HITBOX_H;
    int lft, rgt, tx, row;
    bool landed = false;
    if (dy == 0) return;
    s->py += dy;
    lft = (int)floorf(s->px / TILE);
    rgt = (int)floorf((s->px + HITBOX_W - 1) / TILE);
    if (dy > 0) {
        row = (int)floorf((s->py + HITBOX_H - 1) / TILE);
        for (tx = lft; tx <= rgt; tx++) {
            TileType t = sim_tile_at(s, tx, row);
            bool landable = (t == TILE_ONEWAY || t == TILE_LADDER);
            bool blocks = (t == TILE_SOLID) ||
                          (landable && !s->onLadder &&
                           prevBottom <= (float)(row * TILE));
            if (blocks) { s->py = (float)(row * TILE - HITBOX_H); landed = true; break; }
        }
        if (landed) {
            if (s->fallTracking && (s->py - s->fallStartY) > FALL_DEATH_PX)
                die(s);
            s->fallTracking = false;
            s->onGround = true;
            s->vy = 0;
        } else {
            s->onGround = false;
        }
    } else {
        row = (int)floorf(s->py / TILE);
        for (tx = lft; tx <= rgt; tx++)
            if (tile_blocks(s, tx, row)) {
                s->py = (float)((row + 1) * TILE);
                s->vy = 0;
                break;
            }
        s->onGround = false;
    }
}

static bool overlaps_tile(const GameState *s, TileType t)
{
    int lft = (int)floorf(s->px / TILE);
    int rgt = (int)floorf((s->px + HITBOX_W - 1) / TILE);
    int top = (int)floorf(s->py / TILE);
    int bot = (int)floorf((s->py + HITBOX_H - 1) / TILE);
    int tx, ty;
    for (ty = top; ty <= bot; ty++)
        for (tx = lft; tx <= rgt; tx++)
            if (sim_tile_at(s, tx, ty) == t) return true;
    return false;
}

static bool center_on_ladder(const GameState *s)
{
    int tx = (int)floorf((s->px + HITBOX_W * 0.5f) / TILE);
    int ty = (int)floorf((s->py + HITBOX_H * 0.5f) / TILE);
    return sim_tile_at(s, tx, ty) == TILE_LADDER;
}

/* True while feet are flush with the grid and something stands below.
 * Ladder tiles count: their top edge is walkable ground. */
static bool standing_supported(const GameState *s)
{
    int row = (int)floorf((s->py + HITBOX_H) / TILE);
    int lft = (int)floorf(s->px / TILE);
    int rgt = (int)floorf((s->px + HITBOX_W - 1) / TILE);
    int tx;
    if (s->py != (float)(row * TILE - HITBOX_H)) return false;
    for (tx = lft; tx <= rgt; tx++) {
        TileType t = sim_tile_at(s, tx, row);
        if (t == TILE_SOLID || t == TILE_ONEWAY || t == TILE_LADDER) return true;
    }
    return false;
}

static bool ladder_below_feet(const GameState *s)
{
    int tx = (int)floorf((s->px + HITBOX_W * 0.5f) / TILE);
    int ty = (int)floorf((s->py + HITBOX_H) / TILE);
    return sim_tile_at(s, tx, ty) == TILE_LADDER;
}

static bool solid_beside(const GameState *s, int dir)
{
    int tx = (int)floorf((s->px + HITBOX_W * 0.5f) / TILE) + dir;
    int top = (int)floorf(s->py / TILE);
    int bot = (int)floorf((s->py + HITBOX_H - 1) / TILE);
    int ty;
    for (ty = top; ty <= bot; ty++)
        if (tile_blocks(s, tx, ty)) return true;
    return false;
}

/* ============================================================= *
 *  Coal pickup + exit
 * ============================================================= */
static void collect_coal(GameState *s)
{
    int lft = (int)floorf(s->px / TILE);
    int rgt = (int)floorf((s->px + HITBOX_W - 1) / TILE);
    int top = (int)floorf(s->py / TILE);
    int bot = (int)floorf((s->py + HITBOX_H - 1) / TILE);
    int tx, ty;
    for (ty = top; ty <= bot; ty++)
        for (tx = lft; tx <= rgt; tx++)
            if (tx >= 0 && tx < ROOM_W && ty >= 0 && ty < ROOM_H && s->coal[ty][tx]) {
                s->coal[ty][tx] = false;
                s->coalGot++;
                s->score += 25;
            }

    /* key / lamp are single-cell pickups collected by standing on them */
    if (s->keyRoom && !s->keyGot &&
        (int)floorf((s->px + HITBOX_W * 0.5f) / TILE) == s->keyCol &&
        (top <= s->keyRow && s->keyRow <= bot)) {
        s->keyGot = true; s->score += 50;
    }
    if (s->darkRoom && !s->lampGot &&
        (int)floorf((s->px + HITBOX_W * 0.5f) / TILE) == s->lampCol &&
        (top <= s->lampRow && s->lampRow <= bot)) {
        s->lampGot = true; s->score += 50;
    }
}

/* exit opens once all coal is collected AND (key rooms) the key is held */
static bool exit_open(const GameState *s)
{
    return s->coalGot >= s->coalTotal && (!s->keyRoom || s->keyGot);
}

static void open_exit_tiles(GameState *s)
{
    if (!exit_open(s)) return;
    s->tiles[BEDROCK_ROW][s->exitCol] = TILE_EXIT;   /* single-tile shaft */
}

static void descend(GameState *s)
{
    int nextEntry = s->exitCol;
    s->score += 100 + s->depth * 10;
    s->depth++;
    if (s->depth % 10 == 0) s->lives++;          /* +1 life every 10 rooms */
    sim_gen_room(s, s->runSeed, s->depth, nextEntry);   /* also respawns player */
}

/* Jump straight to `depth` on this run's seed — the identical room a player would
 * reach by descending there (the entry column threads deterministically). No
 * score/life award: this is a synced advance, not an earned clear. */
void sim_sync_room(GameState *s, int depth)
{
    s->depth = depth;
    sim_gen_room(s, s->runSeed, depth, sim_entry_col_for(s->runSeed, depth));
}

/* ============================================================= *
 *  Enemies — deterministic timing hazards. DESIGN.md §3.1, §5.4.
 * ============================================================= */

/* triangle wave: 0 -> amp -> 0, period 2*amp */
static int tri(int t, int amp)
{
    int p;
    if (amp <= 0) return 0;
    p = t % (2 * amp);
    return p <= amp ? p : 2 * amp - p;
}

/* Crusher timing within its cycle (all in ticks). */
#define CR_WARN    12
#define CR_SLAM     6
#define CR_HOLD    14
#define CR_RETRACT 20

/* Ticks a crusher is still GUARANTEED raised (its remaining idle time); 0 once
 * it's warning/slamming/retracting. Lets a bot judge whether there is time to
 * cross the column before the next slam. */
int sim_crusher_safe_ticks(const Enemy *e)
{
    int rest = e->period - (CR_WARN + CR_SLAM + CR_HOLD + CR_RETRACT);
    if (e->type != EN_CRUSHER || e->phase != 0) return 0;
    if (rest < 0) rest = 0;
    return (rest - e->timer < 0) ? 0 : rest - e->timer;
}

static bool aabb(float ax, float ay, float aw, float ah,
                 float bx, float by, float bw, float bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/* full sprite rectangle (for enemy-vs-enemy bounce; the lethal rect is 70%) */
static void enemy_sprite_rect(const Enemy *e, float *w, float *h)
{
    switch (e->type) {
    case EN_CRUSHER: *w = TILE; *h = TILE;     break;
    case EN_FOREMAN: *w = 12;   *h = HITBOX_H; break;
    case EN_BAT:     *w = 12;   *h = 10;       break;
    case EN_SPIDER:  *w = 12;   *h = 12;       break;
    case EN_BOULDER: *w = TILE; *h = TILE;     break;
    default:         *w = 0;    *h = 0;        break;
    }
}

/* A bat is blocked by the room edges, solid tiles, or any other enemy sprite. */
static bool bat_blocked(const GameState *s, const Enemy *self, float x, float y)
{
    const float BW = 12, BH = 10;
    int l, r, t, b, tx, ty, i;
    if (x < 0 || y < 0 || x > ROOM_W * TILE - BW || y > ROOM_H * TILE - BH) return true;
    l = (int)(x / TILE); r = (int)((x + BW - 1) / TILE);
    t = (int)(y / TILE); b = (int)((y + BH - 1) / TILE);
    for (ty = t; ty <= b; ty++)
        for (tx = l; tx <= r; tx++)
            if (sim_tile_at(s, tx, ty) == TILE_SOLID) return true;
    for (i = 0; i < s->enemyCount; i++) {
        const Enemy *o = &s->enemies[i];
        float ow, oh;
        if (o == self) continue;
        enemy_sprite_rect(o, &ow, &oh);
        if (aabb(x, y, BW, BH, o->x, o->y, ow, oh)) return true;
    }
    return false;
}

/* A ground sprite (foreman/boulder) whose floor crumbled out from under it rides
 * the fall straight down. On landing, a drop of more than FALL_DEATH_PX (4 tiles)
 * kills it and resets it to its level-start position — returning any coal a
 * foreman was carrying so the room stays solvable; a shorter drop just resumes
 * normal behaviour. Bats fly and never enter this. */
static void enemy_fall(GameState *s, Enemy *e)
{
    float w = (e->type == EN_BOULDER) ? (float)TILE : 12.0f;
    float h = (e->type == EN_BOULDER) ? (float)TILE : (float)HITBOX_H;
    int lft, rgt, footRow;
    e->y += BOULDER_FALL;
    lft = (int)(e->x / TILE);
    rgt = (int)((e->x + w - 1) / TILE);
    footRow = (int)((e->y + h) / TILE);
    if (sim_tile_at(s, lft, footRow) == TILE_SOLID ||
        sim_tile_at(s, rgt, footRow) == TILE_SOLID) {
        e->y = (float)(footRow * TILE - h);             /* land on the floor */
        if (e->y - e->fallY0 > FALL_DEATH_PX) {          /* fell too far: die + reset */
            int i = (int)(e - s->enemies);
            if (e->type == EN_FOREMAN && e->carry > 0) return_one_coal(s, e);
            s->enemies[i] = s->enemyStart[i];
        } else {
            e->falling = 0;
            if (e->type == EN_FOREMAN) { e->mode = 0; e->timer = 0; }
        }
    }
}

/* A cyclic boulder drops in from the top of the screen, above the entry end of
 * its ledge column, and falls onto the platform below to begin its cascade. */
static void boulder_enter(GameState *s, Enemy *e)
{
    int ledge = e->mode, lo, hi, col;
    if (ledge < 0) ledge = 0;
    if (ledge >= s->mainLedges) ledge = (s->mainLedges > 0) ? s->mainLedges - 1 : 0;
    lo = s->fixLo[ledge]; hi = s->fixHi[ledge];
    col = (e->dir > 0) ? lo : hi;
    e->x = (float)(col * TILE);
    e->y = -(float)TILE;   /* start above the top edge; it falls onto the platform */
    e->falling = 0;
}

/* A boulder rolled off a side of the screen at the bottom level: it wraps to the
 * next ledge in the cycle (alternating the 2nd and 3rd platform rows) rolling the
 * other way, and drops back in from the top on the opposite side. */
static void boulder_wrap(GameState *s, Enemy *e)
{
    e->mode = (e->mode == 1) ? 2 : 1;                 /* alternate 2nd <-> 3rd row */
    if (e->mode >= s->mainLedges) e->mode = (s->mainLedges > 1) ? 1 : 0;
    e->dir = -e->dir;
    boulder_enter(s, e);
}

/* Index of a moving platform whose top an enemy (spanning [x, x+w), feet at
 * feetY) is resting on, else -1 — so foremen can board and ride them. */
static int enemy_plat_support(const GameState *s, float x, float w, float feetY)
{
    int i;
    for (i = 0; i < s->platCount; i++) {
        const MovePlat *p = &s->plats[i];
        if (x + w > p->x && x < p->x + PLAT_W && feetY >= p->y - 2.0f && feetY <= p->y + PLAT_H)
            return i;
    }
    return -1;
}

static void update_enemy(GameState *s, Enemy *e)
{
    if (e->falling && e->type == EN_FOREMAN) {   /* rode a collapsing crumble floor */
        enemy_fall(s, e);
        return;
    }
    switch (e->type) {
    case EN_CRUSHER: {
        int P = e->period;
        int rest = P - (CR_WARN + CR_SLAM + CR_HOLD + CR_RETRACT);
        int s0 = rest < 0 ? 0 : rest;   /* warn start  */
        int s1 = s0 + CR_WARN;          /* slam start  */
        int s2 = s1 + CR_SLAM;          /* fully down  */
        int s3 = s2 + CR_HOLD;          /* retract     */
        int t = e->timer;
        if (t < s0)      { e->y = e->baseY;                                    e->phase = 0; }
        else if (t < s1) { e->y = e->baseY;                                    e->phase = 1; }
        else if (t < s2) { e->y = e->baseY + (float)e->range * (t - s1) / CR_SLAM; e->phase = 2; }
        else if (t < s3) { e->y = e->baseY + (float)e->range;                  e->phase = 2; }
        else             { e->y = e->baseY + (float)e->range *
                                  (1.0f - (float)(t - s3) / CR_RETRACT);       e->phase = 3; }
        e->timer = (e->timer + 1) % P;
        break;
    }
    case EN_FOREMAN: {
        /* Roams platforms AND ladders. At a ladder — a junction — it picks a
         * continuation AT RANDOM (keep walking / climb up / climb down, whichever
         * are available), and also picks a random heading when it lands after a
         * climb. It also transports coal: grabs a lump it walks over, carries it a
         * while, then drops it on another platform cell. */
        const int FW = 12, FH = HITBOX_H;
        const int CARRY_HOLD = 180, CARRY_COOL = 90;
        int c = (int)((e->x + FW * 0.5f) / TILE);
        int onPlat = 0;                                       /* riding a moving platform? */
        if (e->mode == 0) {                                   /* --- walking --- */
            int floorRow = (int)((e->y + FH) / TILE);
            int feetCell = floorRow - 1;
            bool ladderDown = sim_tile_at(s, c, floorRow) == TILE_LADDER; /* carved floor */
            bool ladderUp   = sim_tile_at(s, c, feetCell) == TILE_LADDER; /* rail at body */
            float nx = e->x + e->dir * FOREMAN_SPEED;
            int lead = (int)((nx + (e->dir > 0 ? FW - 1 : 0)) / TILE);
            TileType ahead = sim_tile_at(s, lead, floorRow);
            bool wall  = sim_tile_at(s, lead, feetCell) == TILE_SOLID;
            bool inb   = (nx >= 0.0f) && (nx <= (float)(ROOM_W * TILE - FW));  /* stay on-screen */
            /* a moving-platform top ahead at our feet level is walkable too, so a
             * foreman can step onto (and ride) one */
            bool aheadPlat = enemy_plat_support(s, nx, (float)FW, e->y + (float)FH) >= 0;
            bool canWalk = inb && ((((ahead == TILE_SOLID || ahead == TILE_ONEWAY ||
                            ahead == TILE_LADDER) && !wall)) || aheadPlat);
            int climb = 0;                                    /* +1 down, -1 up */
            if ((ladderDown || ladderUp) && e->timer > 50) {  /* junction: choose at random */
                int opt[3], no = 0;                           /* 0 walk, 1 up, 2 down */
                if (canWalk)    opt[no++] = 0;
                if (ladderUp)   opt[no++] = 1;
                if (ladderDown) opt[no++] = 2;
                if (no > 0) { int p = opt[sim_rand(s, no)];
                              if (p == 1) climb = -1; else if (p == 2) climb = 1; }
            }
            if (climb != 0) {
                e->mode = 1; e->vdir = climb;
                e->x = (float)(c * TILE + (TILE - FW) / 2);   /* snap to rail */
                e->timer = 0;
            } else if (canWalk) {
                e->x = nx;
            } else {
                e->dir = -e->dir;                             /* turn at edge/wall */
            }
        } else {                                              /* --- climbing --- */
            int bodyTop = (int)(e->y / TILE);
            int bodyBot = (int)((e->y + FH - 1) / TILE);
            int supp    = (int)((e->y + FH) / TILE);
            int runTop = -1, rr;
            for (rr = bodyBot; rr >= bodyTop; rr--)
                if (sim_tile_at(s, c, rr) == TILE_LADDER) { runTop = rr; break; }
            if (runTop < 0 && sim_tile_at(s, c, supp) == TILE_LADDER) runTop = supp;
            if (runTop >= 0)
                while (runTop - 1 >= 0 && sim_tile_at(s, c, runTop - 1) == TILE_LADDER) runTop--;

            e->y += e->vdir * FOREMAN_CLIMB;                  /* foremen climb 5% slower than the player */
            if (e->vdir > 0) {                                /* down: land on a floor */
                int feetRow = (int)((e->y + FH) / TILE);
                if (sim_tile_at(s, c, feetRow) == TILE_SOLID) {
                    e->y = (float)(feetRow * TILE - FH);
                    e->mode = 0; e->timer = 0;
                    e->dir = (c <= 2) ? 1 : (c >= ROOM_W - 3 ? -1 : (sim_rand(s, 2) ? 1 : -1));
                }
            } else if (runTop >= 0) {                         /* up: stop on top */
                float top = (float)(runTop * TILE - FH);
                if (e->y < top) {
                    e->y = top;
                    e->mode = 0; e->timer = 0;
                    e->dir = (c <= 2) ? 1 : (c >= ROOM_W - 3 ? -1 : (sim_rand(s, 2) ? 1 : -1));
                }
            }
        }
        /* --- ride a moving platform: if our feet are over a gap (no tile floor)
         * and a platform is under them, get carried along and glued to its top. --- */
        {
            int fc = (int)((e->x + FW * 0.5f) / TILE);
            int fRow = (int)((e->y + FH) / TILE);
            TileType ft = sim_tile_at(s, fc, fRow);
            if (ft != TILE_SOLID && ft != TILE_ONEWAY && ft != TILE_LADDER) {
                int pi = enemy_plat_support(s, e->x, (float)FW, e->y + (float)FH);
                if (pi >= 0) {
                    e->x += s->plats[pi].dx;
                    e->y = s->plats[pi].y - (float)FH;
                    onPlat = 1;
                }
            }
        }
        /* --- coal transport (never on a moving platform: can't drop coal there) --- */
        {
            int cc = (int)((e->x + FW * 0.5f) / TILE);        /* recompute after moving */
            int fr = (int)((e->y + FH) / TILE) - 1;           /* body/coal row */
            bool onFloor = (e->mode == 0) && !onPlat;
            if (e->carry > 0) {                               /* carrying a lump */
                if (e->carry > 1) e->carry--;
                else if (onFloor && fr >= 0 && fr < ROOM_H &&  /* hold done: drop where valid */
                         sim_tile_at(s, cc, fr) == TILE_EMPTY && !s->coal[fr][cc] &&
                         standable(s, cc, fr)) {
                    s->coal[fr][cc] = true; e->carry = -CARRY_COOL;
                }
            } else if (e->carry < 0) {                        /* cooldown after a drop */
                e->carry++;
            } else if (onFloor && fr >= 0 && fr < ROOM_H && s->coal[fr][cc]) {
                /* never grab the LAST lump on the ground: the player's quota
                 * needs it, and a carried lump reads as "no coal left" to a
                 * bot's sensors — the room would look done while the exit
                 * stays shut until the foreman deigns to drop it */
                int r2, c2, ground = 0;
                for (r2 = 0; r2 < ROOM_H && ground < 2; r2++)
                    for (c2 = 0; c2 < ROOM_W; c2++)
                        if (s->coal[r2][c2] && ++ground >= 2) break;
                if (ground >= 2) {
                    s->coal[fr][cc] = false; e->carry = CARRY_HOLD;   /* grab it */
                }
            }
        }
        e->timer++;
        break;
    }
    case EN_BAT: {
        /* diagonal sweep, bouncing off room edges, solid tiles and other sprites.
         * Axis-separated so it slides along a surface instead of sticking. */
        float nx = e->x + e->dir * 1.0f;
        float ny = e->y + e->vdir * 1.0f;
        if (bat_blocked(s, e, nx, e->y)) e->dir  = -e->dir;  else e->x = nx;
        if (bat_blocked(s, e, e->x, ny)) e->vdir = -e->vdir; else e->y = ny;
        e->timer++;
        break;
    }
    case EN_SPIDER:
        /* hangs from the ceiling (baseY), bobs down to baseY+range and back */
        e->y += e->vdir * SPIDER_SPEED;
        if (e->y <= e->baseY)            { e->y = e->baseY;            e->vdir =  1; }
        if (e->y >= e->baseY + e->range) { e->y = e->baseY + e->range; e->vdir = -1; }
        e->timer++;
        break;
    case EN_BOULDER: {
        /* Drops in from the top, then cascades: rolls horizontally and falls off
         * ledge ends to the platform below, same direction, dropping into any gap
         * with nothing under its centre. On an upper level it bounces off a side
         * edge; only on the BOTTOM level does it roll out of the screen and wrap to
         * the next ledge (2nd <-> 3rd row). Bounces off interior walls too. */
        const int BW = TILE;
        float nx, lead;
        int midRow, aheadCol, cen, footRow;
        if (e->spawnDelay > 0) {                    /* parked off-screen after a (re)spawn */
            e->spawnDelay--;
            if (e->spawnDelay == 0) boulder_enter(s, e);   /* time's up: drop in from the top */
            e->timer++;
            break;
        }
        nx = e->x + e->dir * BOULDER_SPEED;
        lead = (e->dir > 0) ? (nx + BW) : nx;       /* leading pixel edge (symmetric L/R) */
        midRow = (int)((e->y + BW * 0.5f) / TILE);
        aheadCol = (int)((nx + (e->dir > 0 ? BW - 1 : 0)) / TILE);
        if (lead <= 0.0f || lead >= (float)(ROOM_W * TILE)) {   /* reached a side edge */
            int floorRow = (int)((e->y + BW) / TILE);
            if (floorRow >= BEDROCK_ROW) boulder_wrap(s, e);   /* bottom level: out of screen */
            else e->dir = -e->dir;                             /* upper level: bounce */
            e->timer++; break;
        }
        if (aheadCol >= 0 && aheadCol < ROOM_W &&
            sim_tile_at(s, aheadCol, midRow) == TILE_SOLID) e->dir = -e->dir;  /* interior wall */
        else e->x = nx;
        cen = (int)((e->x + BW * 0.5f) / TILE);     /* support is the tile under the centre */
        footRow = (int)((e->y + BW) / TILE);
        if (cen >= 0 && cen < ROOM_W && sim_tile_at(s, cen, footRow) == TILE_SOLID)
            e->y = (float)(footRow * TILE - BW);    /* rest on this ledge */
        else
            e->y += BOULDER_FALL;                   /* empty underneath: drop toward the next ledge */
        e->timer++;
        break;
    }
    default: break;
    }
}

/* A slamming crusher flattens a foreman or bat caught under its head, knocking it
 * back to its spawn (a carried coal lump is returned so the room stays winnable). */
static void crush_enemies(GameState *s)
{
    int i, j;
    for (i = 0; i < s->enemyCount; i++) {
        const Enemy *cr = &s->enemies[i];
        float cw, ch;
        if (cr->type != EN_CRUSHER || cr->phase != 2) continue;   /* only while slamming/down */
        enemy_sprite_rect(cr, &cw, &ch);
        for (j = 0; j < s->enemyCount; j++) {
            Enemy *e = &s->enemies[j];
            float ew, eh;
            if (j == i || (e->type != EN_FOREMAN && e->type != EN_BAT)) continue;
            enemy_sprite_rect(e, &ew, &eh);
            if (aabb(cr->x, cr->y, cw, ch, e->x, e->y, ew, eh)) {
                if (e->type == EN_FOREMAN && e->carry > 0) return_one_coal(s, e);
                s->enemies[j] = s->enemyStart[j];   /* crushed: back to its spawn */
            }
        }
    }
}

static void update_enemies(GameState *s)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        Enemy *e = &s->enemies[i];
        update_enemy(s, e);
        /* Keep every sprite inside the side walls (a backstop against off-by-one
         * drift past the edges). A boulder parked off-screen for its spawn delay,
         * or dropping in from above, is left alone. */
        if (e->type == EN_BOULDER && e->spawnDelay > 0) continue;
        {
            float w, h, maxx;
            enemy_sprite_rect(e, &w, &h);
            maxx = (float)(ROOM_W * TILE) - w;
            if (e->x < 0.0f)  e->x = 0.0f;
            if (e->x > maxx)  e->x = maxx;
        }
    }
    crush_enemies(s);   /* a slamming crusher flattens foremen/bats under it */
}

/* Lethal rectangle: ~70% of the sprite, biased toward the player (§6.4). */
static void enemy_rect(const Enemy *e, float *rx, float *ry, float *rw, float *rh)
{
    float w, h;
    switch (e->type) {
    case EN_CRUSHER: w = TILE;      h = TILE;      break;
    case EN_FOREMAN: w = 12;        h = HITBOX_H;  break;
    case EN_BAT:     w = 12;        h = 10;        break;
    case EN_SPIDER:  w = 12;        h = 12;        break;
    case EN_BOULDER: w = TILE;      h = TILE;      break;
    default:         w = 0;         h = 0;         break;
    }
    *rw = w * 0.7f; *rh = h * 0.7f;
    *rx = e->x + w * 0.15f; *ry = e->y + h * 0.15f;
}

static bool player_hit_enemy(const GameState *s)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        float rx, ry, rw, rh;
        enemy_rect(&s->enemies[i], &rx, &ry, &rw, &rh);
        if (aabb(s->px, s->py, HITBOX_W, HITBOX_H, rx, ry, rw, rh)) return true;
    }
    return false;
}

static bool threat_impl(const GameState *s, float px, float py, float margin,
                        bool skipCrusher, bool skipSpider)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        const Enemy *e = &s->enemies[i];
        float rx, ry, rw, rh, hm = margin, vm = margin;
        /* Crushers and spiders are column-bound: neither can occupy a ladder
         * (skip both for a bot on/at one), and a crusher can't chase at all
         * (skip it for flee decisions — the caller times its column instead). */
        if (skipCrusher && e->type == EN_CRUSHER) continue;
        if (skipSpider && e->type == EN_SPIDER) continue;
        if (e->type == EN_CRUSHER) {
            /* A crusher's whole slam column is dangerous once it's about to come
             * down (or already down) — so the bot waits and only darts under it
             * during the safe rest window just after it retracts. */
            int P = e->period;
            int rest = P - (CR_WARN + CR_SLAM + CR_HOLD + CR_RETRACT);
            int slamStart, until;
            if (rest < 0) rest = 0;
            slamStart = rest + CR_WARN;
            until = (e->timer < slamStart) ? (slamStart - e->timer)
                                           : (P - e->timer + slamStart);
            if (until < 16 || e->y > e->baseY + 2.0f) {   /* slamming soon / down */
                rx = e->x; ry = e->baseY; rw = (float)TILE; rh = (float)(e->range + TILE);
                hm = 0.0f;   /* the piston is one tile wide: don't flag the ladder beside it */
            } else {
                enemy_rect(e, &rx, &ry, &rw, &rh);        /* safe: just the raised head */
            }
        } else {
            enemy_rect(e, &rx, &ry, &rw, &rh);
        }
        if (aabb(px, py, HITBOX_W, HITBOX_H,
                 rx - hm, ry - vm, rw + 2 * hm, rh + 2 * vm))
            return true;
    }
    return false;
}

bool sim_enemy_threat(const GameState *s, float px, float py, float margin)
{
    return threat_impl(s, px, py, margin, false, false);
}

/* As sim_enemy_threat but ignores crushers and spiders — for when the bot is on/at
 * a ladder, which neither can ever reach. */
bool sim_enemy_threat_lad(const GameState *s, float px, float py, float margin)
{
    return threat_impl(s, px, py, margin, true, true);
}

/* As sim_enemy_threat but ignores crushers only — for flee decisions: a crusher
 * can't chase, so fleeing sideways from one is pointless and a raised one parked
 * near a route just makes a bot vibrate on the threat boundary. Its column is
 * handled by passage timing instead. */
bool sim_enemy_threat_nocr(const GameState *s, float px, float py, float margin)
{
    return threat_impl(s, px, py, margin, true, false);
}

/* ---- placement ---- */
static bool near_spawn(const GameState *s, int col, int row)
{
    return iabs(col - s->spawnCol) < 3 && row < 5;   /* §5.5 spawn-safe zone */
}

/* An enemy start is spawn-safe if it is >= 3 tiles from the player's entry
 * point (top of the shaft) — keeps randomized starts out of the spawn zone. */
static bool enemy_far_from_spawn(const GameState *s, float x, float y)
{
    /* Match the spawn-safe zone exactly (near_spawn / the gen test): unsafe only
     * when within 3 columns of the entry AND in the top 5 rows. */
    int ecol = (int)((x + 6.0f) / TILE);
    int erow = (int)(y / TILE);
    return iabs(ecol - s->spawnCol) >= 3 || erow >= 5;
}

/* Avoid stacking enemies in the same column (e.g. spider behind a crusher). */
static bool col_occupied(const GameState *s, int col)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        int ec = (int)((s->enemies[i].x + TILE * 0.5f) / TILE);
        if (iabs(ec - col) <= 1) return true;
    }
    return false;
}

static void add_enemy(GameState *s, EnemyType t, float bx, float by,
                      int range, int period)
{
    Enemy *e = &s->enemies[s->enemyCount++];
    e->type = t; e->x = bx; e->y = by; e->baseX = bx; e->baseY = by;
    e->range = range; e->dir = 1; e->vdir = 1; e->mode = 0;
    e->phase = 0; e->timer = 0; e->period = period;
    e->carry = 0; e->falling = 0; e->fallY0 = 0.0f; e->spawnDelay = 0;
}

static bool place_crusher(GameState *s, Rng *r, int period)
{
    int try_;
    for (try_ = 0; try_ < 60; try_++) {
        int c = 2 + rng_range(r, ROOM_W - 4);
        int top, bot, gap, j, taken = 0;
        if (c == s->exitCol) continue;
        /* one crusher per column: never stack two on the same slam tile */
        for (j = 0; j < s->enemyCount; j++)
            if (s->enemies[j].type == EN_CRUSHER &&
                (int)(s->enemies[j].baseX / TILE) == c) { taken = 1; break; }
        if (taken) continue;
        /* find a ceiling (solid with empty just below) — the platform to hang from */
        for (top = 2; top <= BEDROCK_ROW - 4; top++)
            if (s->tiles[top][c] == TILE_SOLID && s->tiles[top + 1][c] == TILE_EMPTY)
                break;
        if (top > BEDROCK_ROW - 4) continue;
        /* find the floor below to slam toward */
        for (bot = top + 1; bot <= BEDROCK_ROW; bot++)
            if (s->tiles[bot][c] == TILE_SOLID) break;
        gap = bot - top - 1;
        if (gap < 3 || gap > 6) continue;
        if (near_spawn(s, c, top + 1)) continue;
        {   /* never slam over a ladder — it would wall off a climb route */
            int rr, hasLadder = 0;
            for (rr = top + 1; rr < bot; rr++)
                if (s->tiles[rr][c] == TILE_LADDER) { hasLadder = 1; break; }
            if (hasLadder) continue;
        }
        {
            float bx = (float)(c * TILE);
            float by = (float)((top + 1) * TILE);            /* retracted: under ceiling */
            int range = (bot - 1 - (top + 1)) * TILE;        /* slam to just above floor */
            if (range < TILE) range = TILE;
            add_enemy(s, EN_CRUSHER, bx, by, range, period);
            s->enemies[s->enemyCount - 1].timer = rng_range(r, period); /* random phase */
            return true;
        }
    }
    return false;
}

static bool place_foreman(GameState *s, Rng *r)
{
    int try_;
    for (try_ = 0; try_ < 40; try_++) {
        int row = 3 + rng_range(r, BEDROCK_ROW - 2);   /* 3..BEDROCK_ROW */
        int c0 = 1 + rng_range(r, ROOM_W - 6);
        int len = 0, c;
        for (c = c0; c < ROOM_W && s->tiles[row][c] == TILE_SOLID &&
                     s->tiles[row - 1][c] == TILE_EMPTY; c++) len++;
        if (len >= 4 && !near_spawn(s, c0 + 1, row - 2)) {  /* body top cell */
            float by = (float)(row * TILE - HITBOX_H);
            int startCol = c0 + 1 + rng_range(r, len - 2);  /* random spot on the run */
            float bx = (float)(startCol * TILE);
            if (!enemy_far_from_spawn(s, bx, by)) bx = (float)((c0 + 1) * TILE);
            add_enemy(s, EN_FOREMAN, bx, by, (len - 2) * TILE, 0);
            s->enemies[s->enemyCount - 1].dir = rng_range(r, 2) ? 1 : -1;
            return true;
        }
    }
    return false;
}

static bool place_bat(GameState *s, Rng *r)
{
    int try_;
    for (try_ = 0; try_ < 40; try_++) {
        int row = 3 + rng_range(r, BEDROCK_ROW - 5);
        int c0  = 2 + rng_range(r, ROOM_W - 4);
        if (s->tiles[row][c0] != TILE_EMPTY) continue;
        if (near_spawn(s, c0, row)) continue;
        add_enemy(s, EN_BAT, (float)(c0 * TILE), (float)(row * TILE), 0, 0);
        /* random diagonal so the whole sweep differs each level */
        s->enemies[s->enemyCount - 1].dir  = rng_range(r, 2) ? 1 : -1;
        s->enemies[s->enemyCount - 1].vdir = rng_range(r, 2) ? 1 : -1;
        return true;
    }
    return false;
}

static bool place_spider(GameState *s, Rng *r)
{
    /* Hang from a ceiling: any solid tile (an overhang) with >= 3 empty rows
     * below it. Systematic scan so a spider reliably finds an anchor. */
    int off = rng_range(r, ROOM_W), i, row;
    for (i = 0; i < ROOM_W; i++) {
        int c = (off + i) % ROOM_W;
        if (col_occupied(s, c)) continue;
        for (row = 1; row <= BEDROCK_ROW - 4; row++) {
            int gap, rr;
            if (s->tiles[row][c] != TILE_SOLID) continue;      /* need a ceiling */
            if (s->tiles[row + 1][c] != TILE_EMPTY ||
                s->tiles[row + 2][c] != TILE_EMPTY ||
                s->tiles[row + 3][c] != TILE_EMPTY) continue;  /* need >=3 open below */
            if (near_spawn(s, c, row + 1)) continue;
            gap = 0;
            for (rr = row + 1; rr < BEDROCK_ROW && s->tiles[rr][c] == TILE_EMPTY; rr++) gap++;
            {
                int range = gap - 1;   /* drop the thread the full shaft, to just above the floor */
                add_enemy(s, EN_SPIDER, (float)(c * TILE),
                          (float)((row + 1) * TILE), range * TILE, 0);
                {   /* start at a random height on the thread, moving either way */
                    Enemy *e = &s->enemies[s->enemyCount - 1];
                    e->y = e->baseY + (float)rng_range(r, range * TILE + 1);
                    e->vdir = rng_range(r, 2) ? 1 : -1;
                }
            }
            return true;
        }
    }
    return false;
}

static bool place_boulder(GameState *s, Rng *r)
{
    /* Cyclic boulder: it begins its cascade on the 2nd platform row, then wraps
     * between the 2nd and 3rd rows as it rolls off the bottom-level edge (see
     * boulder_wrap). Position/timing are set by spawn_player, which parks it and
     * drops it in from the top after a short delay — so here we just register it
     * with its starting row and direction. */
    (void)r;
    if (s->mainLedges < 2) return false;
    add_enemy(s, EN_BOULDER, (float)(s->fixLo[1] * TILE), (float)((s->fixRow[1] - 1) * TILE), 0, 0);
    { Enemy *e = &s->enemies[s->enemyCount - 1]; e->mode = 1; e->dir = 1; }
    return true;
}

static void place_enemies(GameState *s, Rng *r, int depth)
{
    int diff = depth < 50 ? depth : 50;
    int budget = 1 + depth / 4;                        /* §5.4: 1 at depth 2, 5 by 16 */
    int i;

    s->enemyCount = 0;

    /* Depth 1 is completely safe: the tutorial room (dark + lamp) teaches
     * movement, ladders and the lamp with nothing hunting the player. */
    if (depth <= 1) return;

    /* Depth 15 is the menagerie: one of every type, a mid-run gauntlet —
     * every threat has been introduced by 13, now they all share a room. */
    if (depth == 15) {
        int period = 120 - 2 * diff; if (period < 60) period = 60;
        place_crusher(s, r, period);
        place_foreman(s, r);
        place_bat(s, r);
        place_spider(s, r);
        place_boulder(s, r);
        return;
    }

    if (budget > MAX_ENEMIES) budget = MAX_ENEMIES;

    for (i = 0; i < budget && s->enemyCount < MAX_ENEMIES; i++) {
        int period = 120 - 2 * diff;
        int kind, placed = 0, k;
        if (period < 60) period = 60;
        /* one new threat at a time (§5.4): crusher >=2, foreman >=4,
         * bat >=7, spider >=10, boulder >=13 */
        int pool = depth >= 13 ? 5 : (depth >= 10 ? 4 : (depth >= 7 ? 3 : (depth >= 4 ? 2 : 1)));
        kind = rng_range(r, pool);
        for (k = 0; k < 5 && !placed; k++) {           /* try chosen, then fall back */
            switch ((kind + k) % 5) {
            case 0: placed = place_crusher(s, r, period);         break;
            case 1: if (depth >= 4)  placed = place_foreman(s, r); break;
            case 2: if (depth >= 7)  placed = place_bat(s, r);     break;
            case 3: if (depth >= 10) placed = place_spider(s, r);  break;
            case 4: if (depth >= 13) placed = place_boulder(s, r); break;
            }
        }
    }
}

/* ============================================================= *
 *  Moving platforms — dynamic solids the player rides.
 * ============================================================= */
static void update_plats(GameState *s)
{
    int i;
    for (i = 0; i < s->platCount; i++) {
        MovePlat *p = &s->plats[i];
        float *coord = (p->axis == 0) ? &p->x : &p->y;
        float base = (p->axis == 0) ? p->baseX : p->baseY;
        float old = *coord;
        *coord += p->dir * PLAT_SPEED;
        if (*coord <= base)             { *coord = base;             p->dir =  1; }
        if (*coord >= base + p->range)  { *coord = base + p->range;  p->dir = -1; }
        p->dx = (p->axis == 0) ? (*coord - old) : 0.0f;
        p->dy = (p->axis == 1) ? (*coord - old) : 0.0f;
    }
}

static bool plat_hoverlap(const MovePlat *p, float px)
{
    return px + HITBOX_W > p->x && px < p->x + PLAT_W;
}

/* index of the platform the player is resting on (feet at its top), else -1 */
static int plat_support(const GameState *s)
{
    int i;
    float feet = s->py + HITBOX_H;
    for (i = 0; i < s->platCount; i++) {
        const MovePlat *p = &s->plats[i];
        if (plat_hoverlap(p, s->px) && feet >= p->y - 1.0f && feet <= p->y + PLAT_H)
            return i;
    }
    return -1;
}

/* land on a platform top while falling (one-way: only from above) */
static void plat_land(GameState *s, float prevFeet)
{
    int i;
    if (s->vy < 0) return;
    for (i = 0; i < s->platCount; i++) {
        MovePlat *p = &s->plats[i];
        float feet = s->py + HITBOX_H;
        if (plat_hoverlap(p, s->px) && prevFeet <= p->y + 1.0f && feet >= p->y) {
            s->py = p->y - HITBOX_H;
            /* same fall-death bookkeeping as a tile landing — and clear the
             * tracker so being carried down by the platform is never counted
             * as a fall (the bug: stale fallStartY killed on the next step-off). */
            if (s->fallTracking && (s->py - s->fallStartY) > FALL_DEATH_PX) { die(s); return; }
            s->fallTracking = false;
            s->vy = 0; s->onGround = true; s->ridingPlat = i;
            return;
        }
    }
}

static bool plat_area_clear(const GameState *s, int c0, int c1, int r0, int r1)
{
    int c, r;
    for (r = r0; r <= r1; r++)
        for (c = c0; c <= c1; c++) {
            if (c < 0 || c >= ROOM_W || r < 0 || r >= ROOM_H) return false;
            if (s->tiles[r][c] != TILE_EMPTY) return false;
        }
    return true;
}

static void add_plat(GameState *s, float bx, float by, int range, int axis)
{
    MovePlat *p = &s->plats[s->platCount++];
    p->x = bx; p->y = by; p->baseX = bx; p->baseY = by;
    p->range = range; p->axis = axis; p->dir = 1; p->dx = 0; p->dy = 0;
}

/* True if no already-placed moving platform overlaps columns [c0, c1]. */
static bool plat_cols_free(const GameState *s, int c0, int c1)
{
    int i;
    for (i = 0; i < s->platCount; i++) {
        int pc0 = (int)(s->plats[i].x / TILE);
        int pc1 = (int)((s->plats[i].x + PLAT_W - 1) / TILE);
        if (c0 <= pc1 && c1 >= pc0) return false;
    }
    return true;
}

/* Vertical lift beside fixed platform `a`, dropping to a lower platform `b`
 * that spans the lift column — you board off a's edge and ride to b.
 * Connects two fixed platforms. */
static bool try_lift(GameState *s, int a, int b, int side)
{
    int cc = side ? s->fixHi[a] + 1 : s->fixLo[a] - 2;    /* just off a's edge */
    int topR = s->fixRow[a], botR = s->fixRow[b];
    if (botR <= topR + 1 || botR - topR > 8) return false;
    if (cc < 0 || cc + 1 >= ROOM_W || topR - 2 < 0) return false;
    if (!(s->fixLo[b] <= cc && s->fixHi[b] >= cc + 1)) return false;  /* b spans lift */
    /* clear channel + rider headroom, from 2 rows above a's top down to just
     * above b's floor */
    if (!plat_area_clear(s, cc, cc + 1, topR - 2, botR - 1)) return false;
    if (near_spawn(s, cc, topR) || !plat_cols_free(s, cc, cc + 1)) return false;
    add_plat(s, (float)(cc * TILE), (float)(topR * TILE),
             (botR - topR) * TILE, 1);
    return true;
}

/* Horizontal platform off fixed platform `a`'s right edge, floating at a's
 * level — board by walking right off the edge (adjacent at its near extreme),
 * ride out over the gap. Reachable from that one platform. */
static bool try_ledge(GameState *s, int a)
{
    int ry = s->fixRow[a];
    int c0 = s->fixHi[a] + 1;         /* near extreme: adjacent to a's edge */
    int c1 = c0 + 4;                  /* 2 wide + 3 travel */
    if (c1 >= ROOM_W || ry - 2 < 0) return false;
    if (!plat_area_clear(s, c0, c1, ry - 2, ry)) return false;
    if (near_spawn(s, c0, ry) || !plat_cols_free(s, c0, c1)) return false;
    add_plat(s, (float)(c0 * TILE), (float)(ry * TILE), 3 * TILE, 0);
    return true;
}

/* Horizontal shuttle between two fixed platforms on the SAME row (a left of b):
 * a 2-wide platform that travels the gap between their facing edges, so you board
 * off a and ride to b (and back). Connects two fixed platforms. */
static bool try_bridge(GameState *s, int a, int b)
{
    int ry, c0, c1, range;
    if (s->fixRow[a] != s->fixRow[b]) return false;
    if (s->fixHi[a] + 1 >= s->fixLo[b]) return false;   /* need a gap, a strictly left of b */
    ry = s->fixRow[a];
    c0 = s->fixHi[a] + 1;             /* left extreme: adjacent to a */
    c1 = s->fixLo[b] - 1;             /* right extreme: platform's right end adjacent to b */
    if (c1 - c0 < 2 || ry - 2 < 0) return false;         /* room for a 2-wide plat to travel */
    if (!plat_area_clear(s, c0, c1, ry - 2, ry)) return false;
    if (near_spawn(s, c0, ry) || !plat_cols_free(s, c0, c1)) return false;
    range = (c1 - 1 - c0) * TILE;     /* left edge travels c0 -> c1-1 (right end reaches b) */
    if (range < TILE) return false;
    add_plat(s, (float)(c0 * TILE), (float)(ry * TILE), range, 0);
    return true;
}

/* Horizontal shuttle from fixed platform `a` out to a screen edge and back. */
static bool try_wall_shuttle(GameState *s, int a)
{
    int ry = s->fixRow[a], c0, c1;
    if (ry - 2 < 0) return false;
    c0 = s->fixHi[a] + 1; c1 = ROOM_W - 1;               /* out to the right wall */
    if (c1 - c0 >= 2 && plat_area_clear(s, c0, c1, ry - 2, ry) &&
        !near_spawn(s, c0, ry) && plat_cols_free(s, c0, c1)) {
        add_plat(s, (float)(c0 * TILE), (float)(ry * TILE), (c1 - 1 - c0) * TILE, 0);
        return true;
    }
    c0 = 0; c1 = s->fixLo[a] - 1;                         /* out to the left wall */
    if (c1 - c0 >= 2 && plat_area_clear(s, c0, c1, ry - 2, ry) &&
        !near_spawn(s, c1, ry) && plat_cols_free(s, c0, c1)) {
        add_plat(s, (float)(c0 * TILE), (float)(ry * TILE), (c1 - 1 - c0) * TILE, 0);
        return true;
    }
    return false;
}

/* Moving platforms are anchored to the static geometry: every one is boardable
 * from at least one fixed platform. We introduce a horizontal shuttle (platform
 * to platform, or out to a screen edge) first, then vertical lifts. Still
 * additive — the ladder route always remains, so solvability is unaffected. */
static void place_plats(GameState *s, Rng *r, int depth)
{
    int want, made = 0, tries, off;
    s->platCount = 0;
    s->ridingPlat = -1;
    if (depth < 2 || s->fixCount < 2) return;
    /* helpers ramp alongside the dangers (§5.4): one from depth 2, a second
     * from 6, a third from 12 */
    want = 1 + (depth >= 6 ? 1 : 0) + (depth >= 12 ? 1 : 0);
    off = rng_range(r, 97);

    /* a horizontal shuttle: prefer a platform-to-platform bridge, else to a wall */
    for (tries = 0; tries < s->fixCount * s->fixCount && made < 1; tries++) {
        int a = (tries + off) % s->fixCount;
        int b = (tries / s->fixCount + off / 3) % s->fixCount;
        if (a != b && try_bridge(s, a, b)) made++;
    }
    for (tries = 0; tries < s->fixCount && made < 1; tries++)
        if (try_wall_shuttle(s, (tries + off) % s->fixCount)) made++;

    /* fill the rest with vertical lifts, then reachable ledges */
    for (tries = 0; tries < s->fixCount * s->fixCount && made < want; tries++) {
        int a = (tries + off) % s->fixCount;
        int b = (tries / s->fixCount + off / 3) % s->fixCount;
        int side = rng_range(r, 2);
        if (a == b) continue;
        if (try_lift(s, a, b, side) || try_lift(s, a, b, 1 - side)) made++;
    }
    for (tries = 0; tries < s->fixCount * 2 && made < want; tries++)
        if (try_ledge(s, (tries + off) % s->fixCount)) made++;
}

/* ============================================================= *
 *  Crumbling floors — weak tiles that collapse a beat after being
 *  stood on (the "unstable mine"). DESIGN.md §5.6.
 * ============================================================= */
/* A weak floor at (r,c) just collapsed. Sprites over it react:
 *  - foreman / boulder standing on it start falling (see enemy_fall);
 *  - a spider or crusher anchored above it in the same column extends its reach
 *    down to the next solid platform (the floor that limited it is now gone);
 *  - the bat is unaffected (it flies). */
static void crumble_affect(GameState *s, int r, int c)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        Enemy *e = &s->enemies[i];
        if (e->type == EN_FOREMAN) {           /* a walking foreman drops with the floor */
            int lft = (int)(e->x / TILE);
            int rgt = (int)((e->x + 12.0f - 1) / TILE);
            int footRow = (int)((e->y + HITBOX_H) / TILE);
            if (footRow == r && c >= lft && c <= rgt && !e->falling && e->mode == 0) {
                e->falling = 1; e->fallY0 = e->y;
            }
        } else if (e->type == EN_SPIDER || e->type == EN_CRUSHER) {
            int col = (int)(e->baseX / TILE);
            int baseRow = (int)(e->baseY / TILE);        /* first empty row below the ceiling */
            if (col == c && baseRow < r) {               /* the collapsed tile is below it */
                int fr, newRange;
                for (fr = baseRow; fr <= BEDROCK_ROW; fr++)
                    if (sim_tile_at(s, col, fr) == TILE_SOLID) break;
                newRange = (fr - 1 - baseRow) * TILE;     /* reach to just above the new floor */
                if (newRange < TILE) newRange = TILE;
                e->range = newRange;
            }
        }
    }
}

static void update_crumble(GameState *s)
{
    int r, c;
    /* arm the weak tiles currently under the player's feet */
    if (s->onGround) {
        int row = (int)((s->py + HITBOX_H) / TILE);
        int lft = (int)(s->px / TILE);
        int rgt = (int)((s->px + HITBOX_W - 1) / TILE);
        for (c = lft; c <= rgt; c++)
            if (row >= 0 && row < ROOM_H && c >= 0 && c < ROOM_W &&
                s->tiles[row][c] == TILE_CRUMBLE && s->crumble[row][c] == 0)
                s->crumble[row][c] = CRUMBLE_TICKS;
    }
    /* age armed tiles; collapse when the timer runs out (stays gone until respawn) */
    for (r = 0; r < ROOM_H; r++)
        for (c = 0; c < ROOM_W; c++)
            if (s->tiles[r][c] == TILE_CRUMBLE && s->crumble[r][c] > 0)
                if (--s->crumble[r][c] == 0) { s->crumble[r][c] = -1; crumble_affect(s, r, c); }
}

/* ============================================================= *
 *  Init + tick
 * ============================================================= */
void sim_init_seed(GameState *s, uint64_t seed)
{
    memset(s, 0, sizeof *s);       /* no garbage: state must be memcmp-comparable */
    s->lives = 3;
    s->score = 0;
    s->ticks = 0;
    sim_gen_room(s, seed, 1, sim_entry_col_for(seed, 1));   /* also spawns player */
}

void sim_init(GameState *s) { sim_init_seed(s, DEFAULT_SEED); }

void sim_tick(GameState *s, Input in)
{
    s->ticks++;

    if (s->state == PS_GAMEOVER) return;    /* frozen; the shell drives from here */

    if (s->state == PS_DYING) {
        if (--s->dyingTimer <= 0) {
            s->lives--;
            if (s->lives < 0) { s->state = PS_GAMEOVER; return; }  /* out of lives */
            spawn_player(s);
        }
        return;
    }

    update_plats(s);
    /* ride: carry the player along with the platform they stand on */
    if (s->ridingPlat >= 0 && s->ridingPlat < s->platCount && !s->onLadder) {
        MovePlat *p = &s->plats[s->ridingPlat];
        s->px += p->dx;
        s->py += p->dy;
    }

    /* --- ladder attach --- */
    if (!s->onLadder) {
        bool grabUp   = in.up && center_on_ladder(s);
        bool grabDown = in.down && (center_on_ladder(s) || ladder_below_feet(s));
        bool grabFall = !s->onGround && s->vy > 0 && center_on_ladder(s) &&
                        !in.left && !in.right;
        if (grabUp || grabDown || grabFall) {
            int tx = (int)floorf((s->px + HITBOX_W * 0.5f) / TILE);
            s->onLadder = true;
            s->px = (float)(tx * TILE + (TILE - HITBOX_W) / 2); /* snap to rail */
            s->vx = 0; s->vy = 0;
            s->onGround = false;
            s->fallTracking = false;
        }
    }

    if (s->onLadder) {
        if (in.jump) {
            s->onLadder = false;
            s->vy = JUMP_VY;
            s->vx = (in.left ? -WALK_SPEED : 0) + (in.right ? WALK_SPEED : 0);
            if (s->vx != 0) s->facing = (s->vx > 0) ? 1 : -1;
        } else if ((in.left || in.right) && !in.up && !in.down) {
            int dir = in.right ? 1 : -1;
            if (!solid_beside(s, dir)) {
                s->onLadder = false;
                s->vx = WALK_SPEED * dir;
                s->vy = 0;
                s->facing = dir;
            }
        } else {
            float dy = 0;
            int col = (int)floorf((s->px + HITBOX_W * 0.5f) / TILE);
            int bodyTop = (int)floorf(s->py / TILE);
            int bodyBot = (int)floorf((s->py + HITBOX_H - 1) / TILE);
            int supp    = (int)floorf((s->py + HITBOX_H) / TILE);
            int runTop = -1, rr;

            if (in.up)   dy -= CLIMB_SPEED;
            if (in.down) dy += CLIMB_SPEED;

            /* Find the top of the ladder run the player is on — computed BEFORE
             * moving, while still attached, so climbing up can stop exactly on
             * top of the ladder instead of overshooting into the air above it
             * (which used to drop the player back down onto the platform). */
            for (rr = bodyBot; rr >= bodyTop; rr--)
                if (sim_tile_at(s, col, rr) == TILE_LADDER) { runTop = rr; break; }
            if (runTop < 0 && sim_tile_at(s, col, supp) == TILE_LADDER) runTop = supp;
            if (runTop >= 0)
                while (runTop - 1 >= 0 && sim_tile_at(s, col, runTop - 1) == TILE_LADDER)
                    runTop--;

            move_y(s, dy);
            if (dy != 0) s->animTimer++;

            if (dy < 0 && runTop >= 0) {
                float topStand = (float)(runTop * TILE - HITBOX_H);
                if (s->py < topStand) {
                    s->py = topStand;
                    s->vy = 0;
                    s->onGround = true;      /* arrive standing on top */
                    s->onLadder = false;
                }
            }
            if (s->onLadder && (s->onGround ||
                                !(center_on_ladder(s) || ladder_below_feet(s))))
                s->onLadder = false;
        }
        if (s->onLadder) goto post;
    }

    /* walked off a ledge (or ledge vanished): become airborne. A moving
     * platform counts as ground. */
    if (s->onGround && !standing_supported(s) && plat_support(s) < 0) {
        s->onGround = false;
        s->vy = 0;
        s->ridingPlat = -1;
    }

    /* --- ground control: instant, no momentum --- */
    if (s->onGround) {
        s->vx = 0;
        if (in.left)  { s->vx = -WALK_SPEED; s->facing = -1; }
        if (in.right) { s->vx =  WALK_SPEED; s->facing =  1; }
        if (in.jump) {
            s->vy = JUMP_VY;
            s->onGround = false;
        }
    }

    /* --- airborne: gravity + minimal air control --- */
    if (!s->onGround) {
        float steer = 0;
        if (in.left)  steer -= AIR_ACCEL;
        if (in.right) steer += AIR_ACCEL;
        s->vx += steer;
        if (s->vx >  WALK_SPEED) s->vx =  WALK_SPEED;
        if (s->vx < -WALK_SPEED) s->vx = -WALK_SPEED;
        if (steer != 0) s->facing = (steer > 0) ? 1 : -1;

        s->vy += GRAVITY;
        if (s->vy > TERMINAL_VY) s->vy = TERMINAL_VY;
    }

    if (!s->onGround && s->vy > 0 && !s->fallTracking) {
        s->fallTracking = true;
        s->fallStartY = s->py;
    }
    if (s->vy < 0) s->fallTracking = false;

    move_x(s, s->vx);
    {
        float prevFeet = s->py + HITBOX_H;
        if (!s->onGround) move_y(s, s->vy);
        plat_land(s, prevFeet);                 /* catch a platform top while falling */
    }
    /* keep the riding link current: only while standing on a platform top */
    s->ridingPlat = s->onGround ? plat_support(s) : -1;

    if (s->vx != 0 && s->onGround) s->animTimer++;

post:
    update_crumble(s);         /* arm/collapse weak floors under the player */
    update_enemies(s);
    if (player_hit_enemy(s)) { die(s); return; }

    collect_coal(s);
    open_exit_tiles(s);

    if (overlaps_tile(s, TILE_SPIKE)) { die(s); return; }
    if (exit_open(s) && overlaps_tile(s, TILE_EXIT)) descend(s);
}
