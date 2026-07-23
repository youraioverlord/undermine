/* bot1 — a self-contained demo/play bot. Fully independent of bot2: it shares
 * no code, so its algorithm can be changed freely without affecting the other.
 * Drives the LEFT screen in the two-screen view. */
#ifndef BOT1_H
#define BOT1_H

#include "sim.h"

/* Persistent per-target navigation state (drop-commit, ladder, facing, dither). */
typedef struct {
    int lastDir, commitCol, commitDir, sawAir, ditherCount, lastHDir, hopping;
    int stuckCell, stuckTicks;                     /* frozen-on-a-lip escape */
    int escapeDir, escapeTicks, escapes, escCoal;  /* commit inward after an escape; give up after a few */
    char say[48];                                  /* narration line, like a mole script's `say` */
} Bot1Nav;

void  bot1_nav_init(Bot1Nav *n);
Input bot1_input(GameState *s, Bot1Nav *n);   /* one tick of play input */

#endif
