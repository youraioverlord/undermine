/* bot2 "RUSHER" — self-contained play bot (see bot2.h). A complete, private
 * copy of the navigation + behavior stack; nothing here is shared with bot1.
 *
 * Personality: aggressive and greedy. Dives for the DEEPEST reachable coal
 * first (scan bottom-up) and mops upward, cuts closer to enemies (M=4,
 * FLEE=11), and its BFS tie-break prefers descending (climb-down/drop edges
 * first, rightward lean). Contrast: bot1 "STEADY". */
#include "bot2.h"
#include <string.h>
#include <stdbool.h>
#include <stdio.h>

/* Narration, like a mole script's `say`: shown beside the bot's name and
 * streamed into its log, so the built-ins explain themselves too. */
#define BOTSAY(nav, ...) snprintf((nav)->say, sizeof (nav)->say, __VA_ARGS__)

static void bot2_cell(const GameState *s, int *c, int *r)
{
    *c = (int)((s->px + HITBOX_W * 0.5f) / TILE);
    *r = (int)((s->py + HITBOX_H - 1) / TILE);
}

static bool bot2_is_ladder(const GameState *s, int c, int r)
{
    return sim_tile_at(s, c, r) == TILE_LADDER;
}

/* A one-tile gap in direction dir from (c,r) with a same-level platform on the
 * far side and clear headroom — jumpable. Shared by the planner, the decider's
 * take-off, and the cliff guard (so approaching the lip to jump isn't vetoed). */
static bool bot2_gap_jump(const GameState *s, int c, int r, int dir)
{
    int g = c + dir, l = c + 2 * dir;
    if (l < 0 || l >= ROOM_W) return false;
    if (sim_standable(s, g, r)) return false;                 /* not a gap: walk handles it */
    if (sim_tile_at(s, g, r) == TILE_SOLID) return false;     /* a wall, not a gap */
    if (!sim_standable(s, l, r)) return false;                /* need a platform on the far side */
    if (sim_tile_at(s, c, r - 1) == TILE_SOLID) return false; /* head/arc clearance */
    if (sim_tile_at(s, g, r - 1) == TILE_SOLID) return false;
    if (sim_tile_at(s, l, r - 1) == TILE_SOLID) return false;
    return true;
}

/* No spider or crusher endangers the arc/landing of a hop toward `dir` from
 * (cc,cr). A spider blocks with its body; a crusher's hazard is its whole
 * COLUMN — a hop through one needs it guaranteed raised long enough to land
 * and walk clear (a jump is committed mid-air), and its body must be out of
 * the arc rows: touching a crusher kills even while it idles. */
static bool bot2_jump_safe(const GameState *s, int cc, int cr, int dir)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        const Enemy *e = &s->enemies[i];
        int ec, er;
        if (e->type == EN_CRUSHER) {
            ec = (int)((e->baseX + 8.0f) / TILE);
            if (ec != cc && ec != cc + dir && ec != cc + 2 * dir) continue;
            er = (int)((e->y + 8.0f) / TILE);
            if (er >= cr - 1 && er <= cr) return false;          /* body in the arc rows */
            if (sim_crusher_safe_ticks(e) <= 36) return false;   /* could slam onto the arc */
            continue;
        }
        if (e->type != EN_SPIDER) continue;
        ec = (int)((e->x + 8.0f) / TILE);
        er = (int)((e->y + 8.0f) / TILE);
        if (er < cr - 1 || er > cr) continue;                    /* not in the arc/landing rows */
        if (ec == cc || ec == cc + dir || ec == cc + 2 * dir) return false;
    }
    return true;
}

/* BFS over standable cells; returns the first step from (sc,sr) toward the
 * target. Edges: walk (adjacent), climb (ladder up/down), drop (off a ledge). */
static bool bot2_bfs(const GameState *s, int sc, int sr, int tc, int tr,
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
            return true;
        }
        #define BOT2_PUSH(NC, NR) do { \
            if (!seen[NR][NC]) { seen[NR][NC] = true; px[NR][NC] = c; py[NR][NC] = r; \
                                 qx[tail] = NC; qy[tail] = NR; tail++; } } while (0)
        /* RUSHER tie-break: descend first (climb down before up), and lean
         * rightward (dc = +1 first) — among equally short paths bot2 dives for
         * depth where STEADY bot1 walks its level out. Reachability identical. */
        if ((bot2_is_ladder(s, c, r) || bot2_is_ladder(s, c, r + 1)) &&
            sim_standable(s, c, r + 1)) BOT2_PUSH(c, r + 1);   /* climb down */
        for (dc = 1; dc >= -1; dc -= 2)              /* walk (right first) */
            if (sim_standable(s, c + dc, r)) BOT2_PUSH(c + dc, r);
        if ((bot2_is_ladder(s, c, r) || bot2_is_ladder(s, c, r - 1)) &&
            sim_standable(s, c, r - 1)) BOT2_PUSH(c, r - 1);   /* climb up */
        for (dc = 1; dc >= -1; dc -= 2) {            /* drop off a ledge (right first) */
            int nc = c + dc;
            if (nc < 0 || nc >= ROOM_W) continue;
            if (sim_standable(s, nc, r)) continue;
            if (sim_tile_at(s, nc, r) == TILE_SOLID) continue;
            if (sim_tile_at(s, nc, r - 1) == TILE_SOLID) continue;  /* head clearance */
            for (dd = 1; dd <= maxDrop; dd++) {
                int nr = r + dd, below = nr + 1;
                if (nr >= ROOM_H) break;
                if (sim_standable(s, nc, nr)) {
                    int dcol = nc + dc;   /* momentum carries us one more column that way */
                    bool solidFloor = (below >= ROOM_H) ||
                        (sim_tile_at(s, nc, below) == TILE_SOLID &&
                         s->tiles[below][nc] != TILE_CRUMBLE);
                    bool wide = sim_standable(s, nc - 1, nr) || sim_standable(s, nc + 1, nr);
                    /* drift-safe: if we overshoot one column we still land, or a wall
                     * stops us — otherwise the drop pitches us into an adjacent pit, so
                     * skip it and let bfs find a ladder/other route down instead */
                    bool driftSafe = (dcol < 0 || dcol >= ROOM_W) ||
                                     sim_standable(s, dcol, nr) ||
                                     sim_tile_at(s, dcol, nr) == TILE_SOLID;
                    if (solidFloor && wide && driftSafe) BOT2_PUSH(nc, nr);
                    break;                        /* first foothold decides the drop */
                }
                if (sim_tile_at(s, nc, nr) == TILE_SOLID) break;
            }
        }
        for (dc = -1; dc <= 1; dc += 2)              /* jump a one-tile gap to a same-level platform */
            if (bot2_gap_jump(s, c, r, dc)) BOT2_PUSH(c + 2 * dc, r);
        #undef BOT2_PUSH
    }
    return false;
}

void bot2_nav_init(Bot2Nav *n) { n->lastDir = 0; n->commitCol = -1; n->commitDir = 0;
                                 n->sawAir = 0; n->ditherCount = 0; n->lastHDir = 0; n->hopping = 0;
                                 n->stuckCell = -1; n->stuckTicks = 0;
                                 n->escapeDir = 0; n->escapeTicks = 0; n->escapes = 0; n->escCoal = -1;
                                 n->tgtC = -1; n->tgtR = -1; n->say[0] = '\0'; }

/* One tick of the bot brain: the input to move toward cell (tc,tr). */
static Input bot2_decide(GameState *s, int tc, int tr, Bot2Nav *n, int maxDrop)
{
    Input in = {0};
    int cc, cr;
    bot2_cell(s, &cc, &cr);

    if (n->commitCol >= 0) {   /* committing to a drop: walk off, then steer to the landing column */
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
        if (!bot2_bfs(s, cc, cr, tc, tr, &nc, &nr, maxDrop)) in.down = true;
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
        if (bot2_bfs(s, cc, cr, tc, tr, &nc, &nr, maxDrop)) {
            if (nr < cr) in.up = true;
            else if (nr > cr && nc == cc) in.down = true;
            else if (nr > cr && nc != cc) {           /* drop off a side ledge */
                n->commitCol = nc; n->commitDir = (nc > cc) ? 1 : -1; n->sawAir = 0;
                if (n->commitDir > 0) { in.right = true; n->lastDir = 1; }
                else                  { in.left  = true; n->lastDir = -1; }
            }
            else if (nr == cr && (nc == cc + 2 || nc == cc - 2)) {  /* jump a one-tile gap */
                int dir = (nc > cc) ? 1 : -1;
                /* If a spider/crusher blocks the hop, wait — don't even walk to the
                 * lip, or we'd stroll into the gap. Otherwise walk to the lip and
                 * take off from the edge. */
                if (bot2_jump_safe(s, cc, cr, dir)) {
                    float edge = (dir > 0) ? ((float)((cc + 1) * TILE) - (float)HITBOX_W)
                                           : (float)(cc * TILE);
                    int atEdge = (dir > 0) ? (s->px >= edge - 1.0f) : (s->px <= edge + 1.0f);
                    if (dir > 0) { in.right = true; n->lastDir = 1; }
                    else         { in.left  = true; n->lastDir = -1; }
                    if (atEdge) { n->commitCol = nc; n->commitDir = dir; n->sawAir = 0; in.jump = true; }
                }
            }
            else if (nc > cc) { in.right = true; n->lastDir = 1; }
            else if (nc < cc) { in.left = true;  n->lastDir = -1; }
        }
    }
    return in;
}

/* Is stepping into the gap at column c a survivable drop (floor within maxDrop)? */
static bool bot2_safe_landing(const GameState *s, int c, int r, int maxDrop)
{
    int dd;
    if (c < 0 || c >= ROOM_W) return false;
    for (dd = 1; dd <= maxDrop; dd++) {
        int nr = r + dd;
        if (nr >= ROOM_H) return true;
        if (sim_standable(s, c, nr)) return true;
        if (sim_tile_at(s, c, nr) == TILE_SOLID) return true;
    }
    return false;
}

/* A hop in `dir` carries 2-3 tiles: is there anything to land on — footing on
 * the same row at +2/+3, a survivable drop below either, or a MOVING PLATFORM
 * riding under the arc? Without this an escape hop off a ledge is just a long
 * fatal fall wearing a jump's costume. */
static bool bot2_hop_landing(const GameState *s, int c, int r, int dir)
{
    int i, nc = c + 2 * dir;
    if (nc >= 0 && nc < ROOM_W &&
        (sim_standable(s, nc, r) || bot2_safe_landing(s, nc, r, 4))) return true;
    nc = c + 3 * dir;
    if (nc >= 0 && nc < ROOM_W &&
        (sim_standable(s, nc, r) || bot2_safe_landing(s, nc, r, 4))) return true;
    for (i = 0; i < s->platCount; i++) {             /* a platform under the arc catches us */
        const MovePlat *p = &s->plats[i];
        float lx = (float)((c + 2 * dir) * TILE);
        if (p->x + PLAT_W < lx - (float)TILE || p->x > lx + 2.0f * TILE) continue;
        if (p->y >= (float)(r * TILE) && p->y <= (float)((r + 4) * TILE)) return true;
    }
    return false;
}

/* Can the grounded bot clear a ground enemy just ahead with a hop in `dir`? */
static bool bot2_can_jump(const GameState *s, int dir, float margin)
{
    int cc, cr, i;
    const Enemy *tgt = NULL;
    float bx = s->px + HITBOX_W * 0.5f, by = s->py + HITBOX_H * 0.5f;
    bot2_cell(s, &cc, &cr);
    for (i = 0; i < s->enemyCount; i++) {
        const Enemy *e = &s->enemies[i];
        float dx, dy;
        if (e->type != EN_FOREMAN && e->type != EN_BOULDER) continue;
        dx = (e->x + 8.0f) - bx; dy = (e->y + 8.0f) - by;
        if (dy < -14.0f || dy > 14.0f) continue;
        /* the arc rises slowly: it only clears a foe launched ~2 tiles out
         * (20..40px) — anything closer collides on the way up and dies */
        if (dir * dx > 20.0f && dir * dx < 40.0f) { tgt = e; break; }
    }
    if (!tgt) return false;
    if (tgt->dir == dir) return false;
    if (sim_tile_at(s, cc,           cr - 1) == TILE_SOLID) return false;
    if (sim_tile_at(s, cc + dir,     cr - 1) == TILE_SOLID) return false;
    if (sim_tile_at(s, cc + 2 * dir, cr - 1) == TILE_SOLID) return false;
    if (!sim_standable(s, cc + 2 * dir, cr)) return false;
    /* the LANDING (one past the foe, ~3 tiles out) must be clear of OTHER
     * enemies — probing 2 tiles out would hit the very foe we're clearing */
    if (sim_enemy_threat(s, s->px + dir * 48.0f, s->py, margin)) return false;
    return true;
}

/* A non-crusher enemy on column `col`, within `range` tiles on side `side`
 * (+1 = below us, -1 = above us) and coming toward us along the ladder (moving our
 * way, or right beside us). Crushers can't be on a ladder, so they're excluded. */
static bool bot2_ladder_foe(const GameState *s, int col, int cr, int side, int range)
{
    int i;
    for (i = 0; i < s->enemyCount; i++) {
        const Enemy *e = &s->enemies[i];
        int ec, er, dr;
        if (e->type == EN_CRUSHER) continue;
        ec = (int)((e->x + 8.0f) / TILE);
        er = (int)((e->y + 8.0f) / TILE);
        if (ec != col) continue;
        dr = (er - cr) * side;                     /* tiles toward us along `side` */
        if (dr < 1 || dr > range) continue;
        if (dr == 1 || (side > 0 && e->vdir < 0) || (side < 0 && e->vdir > 0)) return true;
    }
    return false;
}

/* Is there a climbable ladder within `range` tiles in direction `dir`, reachable
 * by a clear walk along our row? Used to prefer a ladder escape over hopping. */
static bool bot2_ladder_within(const GameState *s, int cc, int cr, int dir, int range)
{
    int d;
    for (d = 1; d <= range; d++) {
        int lc = cc + dir * d;
        if (lc < 0 || lc >= ROOM_W || !sim_standable(s, lc, cr)) return false;
        if (bot2_is_ladder(s, lc, cr) || bot2_is_ladder(s, lc, cr - 1)) return true;
    }
    return false;
}

/* Nearest bedrock-walkway column reachable from (cc,cr) — the continuous bedrock
 * lets us stroll to the exit once we reach it. -1 if none reachable. */
static int bot2_bedrock_target(const GameState *s, int cc, int cr)
{
    int d, nc, nr;
    for (d = 1; d < ROOM_W; d++) {
        int a = s->exitCol - d, b = s->exitCol + d;
        if (a >= 0     && bot2_bfs(s, cc, cr, a, BEDROCK_ROW - 1, &nc, &nr, 4)) return a;
        if (b < ROOM_W && bot2_bfs(s, cc, cr, b, BEDROCK_ROW - 1, &nc, &nr, 4)) return b;
    }
    return -1;
}

/* Pick a target — remaining coal, then the key, then the exit shaft — and return
 * the input to pursue it, with enemy avoidance / cliff guard / anti-dither.
 * bot2's twist: an aggressive stuck-jump escape (see the tail of this function). */
Input bot2_input(GameState *s, Bot2Nav *n)
{
    Input in = {0};
    int tc = -1, tr = -1, r, c, cc, cr, nc, nr;
    float M = 4.0f;      /* RUSHER: cuts closer to enemies than bot1 */
    float FLEE = 11.0f;  /* ...and breaks off a flee sooner */
    bot2_cell(s, &cc, &cr);
    /* SOFT respect for the spot that killed us last (deaths reset the enemies,
     * so an unchanged route replays the identical death): near the death cell
     * the caution margins widen — never a hard keep-out, the only route
     * through the room may pass exactly there. */
    if (s->deaths > 0 && s->deathCol >= 0) {
        int ddc = cc - s->deathCol, ddr = cr - s->deathRow;
        if (ddc < 0) ddc = -ddc; if (ddr < 0) ddr = -ddr;
        if (ddc <= 2 && ddr <= 2) { M += 4.0f; FLEE += 8.0f; }
    }
    if (s->coalGot != n->escCoal) { n->escCoal = s->coalGot; n->escapes = 0; }  /* progress: reset give-up */

    /* a dark room: fetch the lamp first — it lights the room (a human would too) */
    if (s->darkRoom && !s->lampGot && s->lampCol >= 0 &&
        bot2_bfs(s, cc, cr, s->lampCol, s->lampRow, &nc, &nr, 4)) {
        tc = s->lampCol; tr = s->lampRow;
        BOTSAY(n, "fetching the lamp");
    }
    if (tc < 0 && s->coalGot < s->coalTotal) {
        BOTSAY(n, "coal left: %d", s->coalTotal - s->coalGot);
        /* RUSHER: prefer coal at or below OUR level (keep diving toward the
         * exit, clear the deep rooms on the way), wrapping to the coal we left
         * above only once the lower half is bare — where STEADY bot1 works the
         * whole room top-down; skip jump-gated lumps so we don't fixate on one.
         * COMMIT to the chosen lump until it's gone: the at-or-below preference
         * re-orders as WE move, so rescanning every tick can flap between two
         * lumps and walk circles (or dither on a ladder top) forever. */
        if (n->tgtC >= 0 && n->tgtR >= 0 && s->coal[n->tgtR][n->tgtC] &&
            bot2_bfs(s, cc, cr, n->tgtC, n->tgtR, &nc, &nr, 4)) {
            tc = n->tgtC; tr = n->tgtR;
        } else {
            int k;
            n->tgtC = n->tgtR = -1;
            for (k = 0; k < ROOM_H && tc < 0; k++) {
                r = (cr + k) % ROOM_H;                /* my row, downward, then wrap to the top */
                for (c = 0; c < ROOM_W; c++)
                    if (s->coal[r][c] && bot2_bfs(s, cc, cr, c, r, &nc, &nr, 4)) { tc = c; tr = r; break; }
            }
            if (tc < 0)
                for (k = 0; k < ROOM_H && tc < 0; k++) {
                    r = (cr + k) % ROOM_H;
                    for (c = 0; c < ROOM_W; c++)
                        if (s->coal[r][c]) { tc = c; tr = r; break; }
                }
            if (tc >= 0) { n->tgtC = tc; n->tgtR = tr; }
        }
    }
    if (tc < 0 && s->keyRoom && !s->keyGot && s->keyCol >= 0) { tc = s->keyCol; tr = s->keyRow; BOTSAY(n, "fetching the key"); }
    if (tc < 0) {                                     /* all collected: into the shaft */
        BOTSAY(n, "to the exit");
        if (cr >= BEDROCK_ROW - 1) {
            int center = (int)(s->px + HITBOX_W * 0.5f);
            int shaft  = s->exitCol * TILE + TILE / 2;
            if (center < shaft - 1) in.right = true;
            else if (center > shaft + 1) in.left = true;
            if (cc == s->exitCol) in.down = true;     /* climb a ladder in the shaft down into the exit */
        } else {
            int bt = bot2_bedrock_target(s, cc, cr);  /* nearest reachable bedrock cell */
            tc = (bt >= 0) ? bt : ((s->exitCol < ROOM_W - 2) ? s->exitCol + 1 : s->exitCol - 1);
            tr = BEDROCK_ROW - 1;
            in = bot2_decide(s, tc, tr, n, 4);
        }
    } else {
        in = bot2_decide(s, tc, tr, n, 4);
    }

    /* After a wedge-escape hop, keep heading inward for a bit rather than letting
     * the planner immediately steer us back to the edge (which made a jump/return
     * loop). Still passes through avoidance and the cliff guard below. */
    if (n->escapeTicks > 0) {
        Input e = {0};
        n->escapeTicks--;
        if (n->escapeDir > 0) e.right = true; else e.left = true;
        n->lastDir = n->escapeDir;
        in = e;
    }

    /* Enemy avoidance (only acts when grounded). Retreat early at our own height;
     * hop over a blocking ground foe; on a ladder keep climbing unless hit. */
    {
        int wantDir = in.right ? 1 : (in.left ? -1 : 0);
        bool grounded = s->onGround && !s->onLadder;
        if (grounded) {
            bool nearR, nearL, onUs;
            n->hopping = 0;
            bot2_cell(s, &cc, &cr);
            /* Standing at a ladder (top or bottom) with an enemy coming up/down it:
             * don't sit there — step off to a safe side and evade. */
            {
                bool ladBelow = bot2_is_ladder(s, cc, cr) || bot2_is_ladder(s, cc, cr + 1);
                bool ladAbove = bot2_is_ladder(s, cc, cr - 1);
                if ((ladBelow && bot2_ladder_foe(s, cc, cr, +1, 3)) ||
                    (ladAbove && bot2_ladder_foe(s, cc, cr, -1, 3))) {
                    BOTSAY(n, "evading off the ladder");
                    Input av = {0};
                    bool okL = sim_standable(s, cc - 1, cr) && !sim_enemy_threat(s, s->px - 16.0f, s->py, M);
                    bool okR = sim_standable(s, cc + 1, cr) && !sim_enemy_threat(s, s->px + 16.0f, s->py, M);
                    if (okL && !okR)      { av.left = true;  n->lastDir = -1; return av; }
                    else if (okR && !okL) { av.right = true; n->lastDir = 1;  return av; }
                    else if (okL && okR)  { if (n->lastDir < 0) { av.left = true; n->lastDir = -1; }
                                            else               { av.right = true; n->lastDir = 1; }
                                            return av; }
                    /* both sides blocked: fall through to the normal avoidance */
                }
            }
            /* Pinned against a wall/cliff with a ground foe closing in on the
             * open side: the hop-over window (~2 tiles) opens BEFORE the flee
             * probes fire, so check it on its own — cornered-ness, not
             * proximity, is the trigger. Fires only in the exact window, so
             * it can't oscillate. */
            {
                bool okL2 = sim_standable(s, cc - 1, cr);
                bool okR2 = sim_standable(s, cc + 1, cr);
                int hd2 = (!okL2 && okR2) ? 1 : (!okR2 && okL2) ? -1 : 0;
                if (hd2 != 0 && bot2_can_jump(s, hd2, M) &&
                    bot2_jump_safe(s, cc, cr, hd2) && bot2_hop_landing(s, cc, cr, hd2)) {
                    Input av2 = {0};
                    BOTSAY(n, "pinned - hop over!");
                    av2.jump = true; n->hopping = 1;
                    if (hd2 > 0) { av2.right = true; n->lastDir = 1; }
                    else         { av2.left = true;  n->lastDir = -1; }
                    return av2;
                }
            }
            /* Flee triggers ignore CRUSHERS: one can't chase, so fleeing
             * sideways from it is pointless — and a raised one parked near a
             * route turns the threat boundary into a permanent vibration spot.
             * Its column is handled by the passage gate further down instead. */
            nearR = sim_enemy_threat_nocr(s, s->px + FLEE, s->py, 2.0f);
            nearL = sim_enemy_threat_nocr(s, s->px - FLEE, s->py, 2.0f);
            onUs  = sim_enemy_threat_nocr(s, s->px, s->py, M);
            {
            if (nearR || nearL || onUs) {
                Input av = {0};
                bool okL, okR, ladderUp, ladderDown, ladL, ladR;
                int enemyDir, away;
                BOTSAY(n, "evading");
                bot2_cell(s, &cc, &cr);
                okL = sim_standable(s, cc - 1, cr) && !sim_enemy_threat(s, s->px - 16.0f, s->py, M);
                okR = sim_standable(s, cc + 1, cr) && !sim_enemy_threat(s, s->px + 16.0f, s->py, M);
                /* Ladder escape at our column, up OR down — probe two tiles away so the
                 * side enemy we're fleeing doesn't veto the climb. */
                ladderUp   = bot2_is_ladder(s, cc, cr - 1) &&
                             !sim_enemy_threat(s, s->px, s->py - 2.0f * (float)TILE, M);
                ladderDown = (bot2_is_ladder(s, cc, cr + 1) || bot2_is_ladder(s, cc, cr)) &&
                             !sim_enemy_threat(s, s->px, s->py + 2.0f * (float)TILE, M);
                ladL = okL && bot2_ladder_within(s, cc, cr, -1, 3);
                ladR = okR && bot2_ladder_within(s, cc, cr, +1, 3);
                enemyDir = nearR ? 1 : (nearL ? -1 : (n->lastDir >= 0 ? 1 : -1));

                /* Only a crusher threatens us and a ladder is right here: a ladder is
                 * a safe haven from a crusher (it can never reach one), so climb onto
                 * it rather than fleeing. */
                if (ladderUp || ladderDown) {
                    bool foeNear = sim_enemy_threat_lad(s, s->px + FLEE, s->py, 2.0f) ||
                                   sim_enemy_threat_lad(s, s->px - FLEE, s->py, 2.0f) ||
                                   sim_enemy_threat_lad(s, s->px, s->py, M);
                    if (!foeNear) {
                        BOTSAY(n, "ladder haven");
                        if (ladderUp) av.up = true; else av.down = true;
                        return av;
                    }
                }

                /* We'd hop the enemy — but if a ladder is close by (up, down, or a
                 * short walk to one), take that instead of hopping. */
                if (wantDir != 0 && bot2_can_jump(s, wantDir, M) &&
                    bot2_hop_landing(s, cc, cr, wantDir)) {
                    if (ladderUp)   { av.up = true; return av; }
                    if (ladderDown) { av.down = true; return av; }
                    if (ladL && !ladR) { av.left = true; return av; }
                    if (ladR && !ladL) { av.right = true; return av; }
                    if (ladL && ladR) { if (nearR) av.left = true; else av.right = true; return av; }
                    BOTSAY(n, "hop over!");
                    av.jump = true; n->hopping = 1;
                    if (wantDir > 0) { av.right = true; n->lastDir = 1; }
                    else             { av.left  = true; n->lastDir = -1; }
                    return av;
                }

                /* Can't hop over it: step away, climb a ladder (up or down), or —
                 * cornered with nowhere to go — jump away IF that jump has a
                 * landing (an escape hop off a cliff is just a slower death),
                 * else take the timed hop OVER the approaching foe. */
                if (nearR && !nearL && okL) av.left = true;
                else if (nearL && !nearR && okR) av.right = true;
                else if (okL && !okR) av.left = true;
                else if (okR && !okL) av.right = true;
                else if (okL && okR) { if (nearR) av.left = true; else av.right = true; }
                else if (ladderUp) av.up = true;
                else if (ladderDown) av.down = true;
                else if (bot2_jump_safe(s, cc, cr, -enemyDir) &&
                         bot2_hop_landing(s, cc, cr, -enemyDir)) {
                    away = -enemyDir;                    /* jump away from the enemy */
                    av.jump = true; n->hopping = 1;
                    if (away > 0) { av.right = true; n->lastDir = 1; }
                    else          { av.left  = true; n->lastDir = -1; }
                }
                else if (bot2_can_jump(s, enemyDir, M) &&
                         bot2_jump_safe(s, cc, cr, enemyDir) &&
                         bot2_hop_landing(s, cc, cr, enemyDir)) {
                    av.jump = true; n->hopping = 1;      /* over the top of it */
                    if (enemyDir > 0) { av.right = true; n->lastDir = 1; }
                    else              { av.left  = true; n->lastDir = -1; }
                }
                else if (onUs || sim_enemy_threat(s, s->px + enemyDir * (float)TILE, s->py, M)) {
                    /* NO escape at all and it's right on us: leap away anyway
                     * and take the chance — a moving platform may drift under
                     * the arc, and standing still is certain death. */
                    BOTSAY(n, "leap of faith!");
                    away = -enemyDir;
                    av.jump = true; n->hopping = 1;
                    if (away > 0) { av.right = true; n->lastDir = 1; }
                    else          { av.left  = true; n->lastDir = -1; }
                }
                return av;
            }
            }
        } else if (s->onLadder) {
            /* enemy on the ladder: climb AWAY from it (down if it's above, up if
             * below); if boxed both ways, hang and wait it out. Crushers are ignored
             * here — a ladder is a safe haven from them. */
            bool above = sim_enemy_threat_lad(s, s->px, s->py - (float)TILE, M);
            bool below = sim_enemy_threat_lad(s, s->px, s->py + (float)TILE, M);
            if (above || below || sim_enemy_threat_lad(s, s->px, s->py, M)) {
                Input mv = {0};
                if (below && !above) {
                    /* a climber at our heels can't be out-climbed (it climbs at
                     * 95% of our speed — we top out with it in contact range):
                     * LEAP off the rail to a survivable side instead */
                    int dd;
                    bot2_cell(s, &cc, &cr);
                    for (dd = -1; dd <= 1; dd += 2) {
                        int nc2 = cc + dd;
                        if (nc2 < 0 || nc2 >= ROOM_W) continue;
                        if (sim_enemy_threat(s, s->px + (float)dd * 16.0f, s->py, M)) continue;
                        if (!bot2_jump_safe(s, cc, cr, dd)) continue;   /* no spider in the arc */
                        {
                            /* NEVER leap through a crusher column, idle or
                             * not — the arc rises into its raised body, and
                             * touching a crusher kills even while it idles */
                            int k2, cru2 = 0;
                            for (k2 = 0; k2 < s->enemyCount; k2++) {
                                const Enemy *e2 = &s->enemies[k2];
                                int ec2;
                                if (e2->type != EN_CRUSHER) continue;
                                ec2 = (int)((e2->baseX + 8.0f) / TILE);
                                if (ec2 == cc + dd || ec2 == cc + 2 * dd) cru2 = 1;
                            }
                            if (cru2) continue;
                        }
                        if (sim_standable(s, nc2, cr) || bot2_safe_landing(s, nc2, cr, 4)) {
                            BOTSAY(n, "leap off the rail!");
                            mv.jump = true;
                            if (dd > 0) mv.right = true; else mv.left = true;
                            n->lastDir = dd;
                            return mv;
                        }
                    }
                    mv.up = true;               /* nowhere to leap: race it anyway */
                }
                else if (above && !below) mv.down = true;
                else if (!above) mv.up = true;
                else if (!below) mv.down = true;
                return mv;
            }
        } else if (!n->hopping && sim_enemy_threat(s, s->px, s->py, M)) {
            Input fall = {0};
            return fall;
        }
    }

    /* Ladder awareness on the ground: don't climb up (or down) into an enemy that
     * is on the ladder just above (or below) us — wait for it to clear instead. */
    if (in.up && sim_enemy_threat(s, s->px, s->py - (float)TILE, M)) in.up = false;
    if (in.down && sim_enemy_threat(s, s->px, s->py + (float)TILE, M)) in.down = false;

    /* Crusher passage gate: its threat is its COLUMN, on a timer — never step
     * into that column unless the crusher is guaranteed raised long enough to
     * cross under (idle with ticks to spare — entering an idle column that
     * arms mid-crossing is how bots get flattened), and if one is armed over
     * OUR column, step out to whichever side stands. */
    {
        int i;
        bot2_cell(s, &cc, &cr);
        for (i = 0; i < s->enemyCount; i++) {
            const Enemy *e = &s->enemies[i];
            int ec;
            if (e->type != EN_CRUSHER) continue;
            if (sim_crusher_safe_ticks(e) > 36) continue;           /* long idle: pass freely */
            ec = (int)((e->baseX + 8.0f) / TILE);
            if (ec == cc) {                                         /* armed over us: get out */
                BOTSAY(n, "crusher! stepping out");
                bool okL2 = sim_standable(s, cc - 1, cr);
                bool okR2 = sim_standable(s, cc + 1, cr);
                in.left = in.right = false;
                if (okL2 && (!okR2 || n->lastDir <= 0)) { in.left = true;  n->lastDir = -1; }
                else if (okR2)                          { in.right = true; n->lastDir = 1; }
            } else if (in.right && ec == cc + 1) {
                BOTSAY(n, "waiting out the crusher");
                in.right = false;                                   /* hold until it resets */
            } else if (in.left && ec == cc - 1) {
                BOTSAY(n, "waiting out the crusher");
                in.left = false;
            }
        }
    }

    /* Cliff guard: never step off a ledge into a fatal fall. Exit shaft exempt;
     * a room-edge lip over our own safe shaft is allowed (wall stops the drift).
     * Skipped mid-drop-commit: that step-off was already validated by the planner,
     * and vetoing it here wedges the bot when the drop target is a border column
     * (its center enters col 0/19 while still supported, and the guard cancels the
     * walk-off and clears the commit, so it oscillates on the edge and never falls). */
    if (s->onGround && !s->onLadder && n->commitCol < 0) {
        bool ownFloor, ownShaftSafe;
        bot2_cell(s, &cc, &cr);
        ownFloor = (cr + 1 >= ROOM_H) ||
                   sim_tile_at(s, cc, cr + 1) == TILE_SOLID ||
                   sim_tile_at(s, cc, cr + 1) == TILE_ONEWAY ||
                   sim_tile_at(s, cc, cr + 1) == TILE_LADDER ||
                   bot2_is_ladder(s, cc, cr);
        ownShaftSafe = !ownFloor && (cc == 0 || cc == ROOM_W - 1) &&
                       bot2_safe_landing(s, cc, cr, 3);
        if (!ownShaftSafe) {
            if (in.right && !in.jump && !bot2_gap_jump(s, cc, cr, 1) && cc + 1 != s->exitCol &&
                !sim_standable(s, cc + 1, cr) &&
                !bot2_safe_landing(s, cc + 1, cr, 4)) { in.right = false; n->commitCol = -1; n->sawAir = 0; }
            if (in.left && !in.jump && !bot2_gap_jump(s, cc, cr, -1) && cc - 1 != s->exitCol &&
                !sim_standable(s, cc - 1, cr) &&
                !bot2_safe_landing(s, cc - 1, cr, 4)) { in.left = false; n->commitCol = -1; n->sawAir = 0; }
        }
    }

    /* Anti-dither: stand still rather than vibrate on the spot. */
    {
        int hd = in.right ? 1 : (in.left ? -1 : 0);
        if (!s->onGround || in.up || in.down) n->ditherCount = 0;
        else if (hd && n->lastHDir && hd == -n->lastHDir) n->ditherCount++;
        if (hd) n->lastHDir = hd;
        if (n->ditherCount > 6) { in.left = in.right = false; }
    }

    /* bot2's escape: if we sit frozen on one ground cell for ~1.7s — a jump-gated
     * lip the walk/climb/drop planner can't cross — take a running hop in our last
     * heading to try to clear it. Aggressive: it may whiff (and the arena watchdog
     * still backstops), but it lets bot2 solve some rooms bot1 just gives up on. */
    {
        int cell = (s->onGround && !s->onLadder) ? (cr * ROOM_W + cc) : -1;
        bool frozen = !in.left && !in.right && !in.up && !in.down && !in.jump;
        if (cell >= 0 && cell == n->stuckCell && frozen) n->stuckTicks++;
        else { n->stuckTicks = 0; n->stuckCell = cell; }
        if (n->stuckTicks > 85 && sim_tile_at(s, cc, cr - 1) != TILE_SOLID && n->escapes < 2) {
            int dir = n->lastDir ? n->lastDir : (cc < s->exitCol ? 1 : -1);
            if (cc <= 1) dir = 1; else if (cc >= ROOM_W - 2) dir = -1;   /* break inward at an edge */
            if (bot2_jump_safe(s, cc, cr, dir) &&                        /* never hop onto a spider/crusher */
                bot2_hop_landing(s, cc, cr, dir)) {                      /* ...or off into a fatal fall */
                BOTSAY(n, "unstick hop!");
                Input j = {0}; j.jump = true;
                if (dir > 0) j.right = true; else j.left = true;
                n->stuckTicks = 0; n->lastDir = dir;
                n->escapeDir = dir; n->escapeTicks = 35; n->escapes++;   /* commit inward, don't snap back */
                return j;
            }
        }
        /* escapes exhausted with no progress -> stop trying; standing put lets the
         * arena's stuck watchdog roll a fresh room instead of looping forever. */
    }
    return in;
}
