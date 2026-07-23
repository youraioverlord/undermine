/* MoleScript — a tiny, deterministic, sandboxed bot language for UNDERMINE.
 *
 * A .mole program is compiled to bytecode and stepped once per game tick,
 * yielding exactly one Input per tick (the same contract as the C bots). The
 * script reads like a top-down program with one main loop: control flow and
 * sensors are instantaneous, while ACTIONS (walk/climb/jump/drop/wait/go_to)
 * consume ticks and suspend the program until they finish, resuming in place.
 *
 * The interpreter never touches GameState except to read it; all world change
 * still happens through sim_tick applied to the returned Input. It has no I/O,
 * no wall clock, and its own seeded RNG, so runs stay fully deterministic.
 *
 * See MOLESCRIPT.md for the language reference. */
#ifndef MOLE_H
#define MOLE_H

#include "sim.h"

typedef struct MoleVM MoleVM;

/* Compile a program from source text. Returns NULL on a lex/parse/compile
 * error, writing a human-readable message (with line number) into errbuf.
 * `seed` seeds the script's private random() stream. */
MoleVM *mole_load(const char *src, uint64_t seed, char *errbuf, int errlen);

/* Same, reading the whole file at `path`. NULL on read or compile error. */
MoleVM *mole_load_file(const char *path, uint64_t seed, char *errbuf, int errlen);

/* Rewind to the start of the program for a fresh room/run and reseed RNG.
 * The analog of botN_nav_init. */
void mole_reset(MoleVM *vm, uint64_t seed);

/* One tick of play: advances the program until it produces this tick's input. */
Input mole_input(MoleVM *vm, GameState *s);

/* The script's author, taken from its required first-line comment
 * (e.g. "# author: Claude Opus 4.8"); "unknown" if the line is missing. */
const char *mole_author(const MoleVM *vm);

/* The script's current introspection line — whatever its last `say` built
 * (empty string if it has never said anything). Shown under the bot in-game
 * and printed by --moletest whenever it changes. */
const char *mole_say(const MoleVM *vm);

void mole_free(MoleVM *vm);

#endif
