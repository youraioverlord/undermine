/* bot2 — a self-contained demo/play bot. Fully independent of bot1: it shares
 * no code, so its algorithm can be changed freely without affecting the other.
 * Drives the RIGHT screen in the two-screen view. */
#ifndef BOT2_H
#define BOT2_H

#include "sim.h"

/* Persistent per-target navigation state (drop-commit, ladder, facing, dither). */
typedef struct {
    int lastDir, commitCol, commitDir, sawAir, ditherCount, lastHDir, hopping;
    int stuckCell, stuckTicks;                     /* stuck-jump escape */
    int escapeDir, escapeTicks, escapes, escCoal;  /* commit inward after an escape; give up after a few */
    int tgtC, tgtR;                                /* committed coal target (RUSHER's scan is
                                                    * position-relative — recomputing it every
                                                    * tick can flap between lumps and walk circles) */
    char say[48];                                  /* narration line, like a mole script's `say` */
} Bot2Nav;

void  bot2_nav_init(Bot2Nav *n);
Input bot2_input(GameState *s, Bot2Nav *n);   /* one tick of play input */

#endif
