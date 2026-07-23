# UNDERMINE

*A C64-style coal-mining platformer that is secretly an AI benchmark — and openly a very good time.*

**v1.0** · C99 + [raylib](https://www.raylib.com/) · fully deterministic · two screens, one seed, no mercy

---

## What is this?

On the surface: an endless procedurally-generated coal mine, rendered in
sixteen honest Commodore 64 colors. You grab every lump of coal on a level,
find the key if there is one, and drop down the exit shaft — forever deeper,
past crushers, foremen, bats, spiders, and boulders, on a dwindling supply
of air.

Under the surface: a **testbed for AI-written game bots**. The game runs two
screens side by side, playing *identical* mines in lock-step. Bots are
written in **MoleScript**, a small deterministic scripting language made for
exactly this. The two screens can host any combination of:

- **You**, on the left, playing with a keyboard like it's 1984.
- The **built-in bots** — STEADY (methodical, careful) and RUSHER
  (dives first, apologizes never).
- **Your own `.mole` scripts** — or ones written by a large language model
  that has never seen this game before.

That last one is the point.

## Why build this?

Because most "can an AI write code?" tests are contaminated: the model has
read a million React tutorials, so of course it can write React. UNDERMINE
is different by construction:

- **A closed world.** The entire universe a bot can know is one file,
  [`MOLESCRIPT.md`](MOLESCRIPT.md). No Stack Overflow answers exist. No
  training data covers it. What the model produces is what the model can
  actually *derive*.
- **Deterministic to the bit.** Same seed, same mine, same enemy phases,
  same dice (`random()` is a seeded stream). A score is a number, not a
  distribution; a bug replays identically until you understand it.
- **Measurable.** Headless test harnesses score a bot in seconds — rooms
  cleared over 20 fixed seeds, plus a gauntlet of crafted scenarios
  (chased-up-a-ladder, gap-crossing, key rooms) that have killed every
  first draft ever submitted, human and machine alike.
- **Watchable.** Numbers aren't the whole story. Load a bot onto the
  right-hand screen (**F3** on the title, or `--bot2 mybot.mole`), start a
  run — or just wait ten seconds and the demo starts one for you — and watch
  it work. Its `say` line prints its current thinking next to its name at
  the top of the screen, and the same narration streams into `bot2.log` for
  the post-mortem (the built-in bots narrate and log the same way, so
  there's always a diary to read). Debugging by reading a small robot's diary is more fun
  than it has any right to be.

The deliberate twist: the spec tells an AI *what everything does*, but not
*what works*. The hard-won tactical knowledge — jump-over timing, ladder
lore, crusher discipline — lives in [`cheat-sheet.md`](cheat-sheet.md),
which opens with a warning that any AI reading it is cheating. Humans get
the answer key. Machines sit the exam. We think that's only fair; they're
faster than us at everything else.

## Installing

You need **CMake ≥ 3.16**, a C compiler, and **git** (the build fetches
raylib automatically on first configure). Developed and tested on Windows
with MSVC; raylib itself is happily cross-platform.

```
git clone <this repo>
cd undermine
cmake -S . -B build
cmake --build build --config Release
```

The game is then at `build/Release/undermine.exe` (or `build/undermine` on
single-config generators). Run it from the repo root so it can find the
`bots/` folder:

```
./build/Release/undermine.exe
```

Sanity check the build with the 160+ test suite:

```
./build/Release/undermine.exe --selftest
```

## Playing it yourself

- **SPACE** digs a fresh mine · **D** plays the shared daily seed · **S**
  types in any 6-digit hex seed (the current seed is shown in-game — see a
  great mine, share the number).
- **Arrows / WASD** move and climb, **SPACE/J** jumps, **P** pauses,
  **ESC** quits to the title. **M** on the title toggles sound.
- Collect all the coal (and the key, from depth 10) to open the exit. Mind
  the air bar at the bottom. Touch nothing that moves — or hangs from the
  ceiling; a crusher is deadly even while it naps.
- **F2 / F3** load a `.mole` bot onto the left/right screen (the file
  picker is Windows-only — on Linux/macOS pass `--bot1 mybot.mole` /
  `--bot2 mybot.mole` on the command line instead). Left idle for ten
  seconds and the demo runs bot-vs-bot on its own.
- The board ranks **deepest dives, fewest deaths** — 20 places, one row per
  name, humans and bots on equal footing. A human with a keyboard beats a
  fresh bot comfortably — improvisation is still our home turf. But bots
  don't stay fresh: every tweak compounds, they never get tired or greedy at
  the wrong moment, and refined ones start closing the gap. Enjoy the lead
  while it lasts — and read the cheat sheet to keep it.

## Writing your own bot (humans)

Start with [`molescript-human-readme.md`](molescript-human-readme.md) — a
from-zero tutorial that assumes no programming background and explains every
algorithm it uses. Then open [`cheat-sheet.md`](cheat-sheet.md) and steal
shamelessly: it's the tactical playbook our own bots died repeatedly to
write — jump-over timing, ladder escapes, crusher discipline, death-memory
tricks — and it's the single biggest shortcut a human bot-writer has. The
AI models have to rediscover every rule on that page through trial and
burial; you get them for the price of reading. This is your species
advantage — use it. Then study `bots/greedy.mole` and `bots/cautious.mole`,
which implement all of it with running commentary.

Your dev loop:

```
undermine --moletest bots/mybot.mole      # 20 seeds, headless, one score
undermine --probelad bots/mybot.mole      # survive a ladder chase?
undermine --probegap bots/mybot.mole      # figure out the gap hop?
undermine --probekey bots/mybot.mole      # fetch the key, or starve at the door?
undermine --bot2 bots/mybot.mole          # watch it play, right screen
```

Give your bot `say` lines — they show on screen and stream into `bot1.log` /
`bot2.log`, so when it dies you can read exactly what it was thinking. It
was probably thinking "coal left: 1".

## Benchmarking an AI (the actual sport)

The development loop mirrors how a human works, with the model in the
driver's seat and you as the (optional) rally co-driver:

1. **Initialize** the model with the contents of `MOLESCRIPT.md` and a short
   task prompt — *nothing else*. No cheat sheet, no example bots, no this
   README. The spec is the exam paper.
2. The model writes `ai.mole`.
3. **Test it**: `--moletest` for the headline score, the probe scenarios for
   the classic deaths, and a live run with `--bot2 ai.mole` while
   `bot2.log` records every `say`.
4. **Feed the evidence back**: paste the moletest numbers, probe verdicts,
   and the log file into the conversation. The log is the crucial part — a
   bot that narrates its intentions hands the model its own post-mortem.
5. The model refines. Repeat.
6. **Human steering is allowed but optional** — a nudge like "you keep dying
   on ladders; think about who else can climb" is legitimate coaching.
   Pasting the cheat sheet is not. Where exactly coaching becomes cheating
   is your call as the operator; just be consistent between models you
   compare.

A sample initialization prompt (short on purpose — the spec does the heavy
lifting):

> Attached is `MOLESCRIPT.md`, the complete reference for MoleScript — a
> small language for scripting bots in a mining platformer called UNDERMINE.
> Using only this document, write a complete bot that descends as deep as
> possible while dying as little as possible. Use `say` at every decision
> point so your bot's log explains its reasoning. Reply with only the
> contents of the `.mole` file.

Then compare models the honest way: same prompt, same number of refinement
rounds, same seeds. Descents over 20 seeds is the headline number; the probe
matrix is the character test; the leaderboard is the victory lap. Our
reference bots score in the 60s on `--moletest` and survive the full probe
matrix — beat that, silicon.

## The documents

| File | Audience | Contents |
|---|---|---|
| [`MOLESCRIPT.md`](MOLESCRIPT.md) | AI models & terse humans | The complete language spec — rules and facts, deliberately free of tactical advice |
| [`molescript-human-readme.md`](molescript-human-readme.md) | Human beginners | A patient tutorial: the language, the monsters, the algorithms, the debugging |
| [`cheat-sheet.md`](cheat-sheet.md) | **Humans only** | Every hard-won survival tactic. AI models are on their honor to stay out |
| [`DESIGN.md`](DESIGN.md) | The curious | The game's design document — generation, difficulty ramp, the C64 constraints |
| `bots/*.mole` | Everyone (post-exam) | Reference bots, fully commented |

## Fine print

Deterministic 50 Hz simulation; the interpreter is sandboxed (no file or
network access, bounded work per tick) — a broken script falls back to the
built-in bot and can never crash the game. `--selftest` runs the full
regression suite. Seeds are at most 6 hex digits, small enough to memorize
on the walk to tell a friend.

Now go on. The mine isn't going to dig itself, and the machines are already
practicing.
