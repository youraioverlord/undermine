# UNDERMINE — Design Document

*A C64-style flick-screen platformer homage to Wanted: Monty Mole (Gremlin, 1984)*

## 1. Concept & Pillars

You are a mole descending an infinite, unstable coal mine. Each screen is one room; collect the coal quota to open the shaft to the next room down. One touch kills. The mine is procedurally generated and gets meaner the deeper you go. Score = depth + coal.

**Pillars:**

1. **Authentic 8-bit feel** — every visual and audio decision must be achievable on real C64 hardware, even if we don't ship on one.
2. **Instant death, instant restart** — Monty-style brutality, roguelike forgiveness (respawn at top of current room, lose a life).
3. **Readable danger** — one-hit death is only fair if every hazard telegraphs.
4. **Endless but deterministic** — a seed fully defines a run; same seed = same mine (daily-run friendly).

**Target platform (recommendation):** build in a modern engine that *emulates* C64 constraints rather than targeting real hardware. **C + raylib** at a 320×200 internal framebuffer scaled ×3/×4 is the pragmatic choice — it keeps the aesthetic while gaining floats, a debugger, and easy headless testing (which Section 7 depends on). The purist alternative — 6502 assembly via KickAssembler + VICE — is viable but makes procedural generation and automated solvability checks dramatically harder (256-byte pages, no heap, 1 MHz). If the goal is *shipping a game that feels like a C64 game*, the modern engine wins; if the goal is *the hobby of C64 dev itself*, this doc's constraints section becomes the hardware spec instead of a style guide.

## 2. C64 Authenticity Constraints (the style bible)

These rules are what make it look and sound like 1984. Enforce them in code, not by discipline.

- **Resolution:** 320×256 internal per screen, integer-scaled. The HUD is overlaid on the top 8 px of the play field (no reserved band). The window shows **two screens side by side** (see "Two-screen bot arena" below).
- **Palette:** the fixed 16-color C64 palette (Pepto values), no others, ever:
  `#000000 #FFFFFF #68372B #70A4B2 #6F3D86 #588D43 #352879 #B8C76F #6F4F25 #433900 #9A6759 #444444 #6C6C6C #9AD284 #6C5EB5 #959595`
- **Background:** character-based. The playfield is a 40×25 grid of 8×8 tiles. In multicolor char mode a tile may use: background color (shared), two shared auxiliary colors, and one per-tile color — and multicolor pixels are double-wide (effectively 4×8 per char). Build the tile renderer to enforce this.
- **Sprites:** 24×21 px hires, or 12×21 double-wide pixels in multicolor. Multicolor sprites get: transparent, two colors shared by *all* sprites, one individual color. **Hard limit: 8 sprites on screen** (the VIC-II per-scanline limit — honoring it globally keeps rooms honest and readable).
- **Motion:** fixed 50 Hz logic tick (PAL). All speeds in px/frame at 50 fps. No sub-pixel rendering — positions render on integer pixels (keep sub-pixel accumulation internally).
- **No rotation, no alpha, no scaling** of sprites. Animation is frame-flipping only.

## 3. Sprites

### 3.1 Inventory

All sprites are 24×21 multicolor (12×21 fat pixels). Shared sprite colors: **black** (outline) + **white** (highlight). Individual color per sprite as listed.

| Sprite | Frames | Individual color | Notes |
|---|---|---|---|
| Monty walk | 4 (L/R via mirror) | brown `#433900` | classic 4-frame walk cycle, 6 ticks/frame |
| Monty climb | 2 | brown | alternating hands on ladder |
| Monty jump/fall | 1 + 1 | brown | jump = tucked, fall = legs out (telegraphs landing) |
| Monty death | 4, non-looping | white flash → red | plays over 24 ticks, then respawn |
| Crusher | 3 | light grey | ceiling piston; frames: retracted / warning shake / slammed |
| Bat | 2 | dark grey | sine-wave flyer, 8 ticks/frame flap |
| Spider | 2 | green | descends/ascends on thread (thread drawn as char tiles) |
| Foreman | 2 walk | light red | patrols platforms, turns at edges |
| Boulder | 2 (roll) | grey | spawned by deep-level generators, rolls + falls |
| Coal lump | 1 | dark grey w/ white glint | the collectible; glint blinks every 25 ticks |
| Key / Lamp / Pickaxe | 1 each | yellow | room-specific specials (see §5.4) |
| Exit shaft cap | 2 | yellow | closed / open, animates when quota met |

Enemy count per room ≤ 5, so with Monty + pickup twinkle effects we never exceed the 8-sprite budget.

### 3.2 Production pipeline

- Author in **Aseprite** on a 12×21 canvas per multicolor sprite (fat pixels), with a locked 16-color palette file (`c64.pal`). Export sprite sheets as PNG; a build script validates every pixel against the palette and the per-sprite color rules (max 3 colors + transparent, 2 of them the shared pair) and **fails the build** on violation. This validator is cheap and is what actually keeps the art authentic.
- Background tiles: 8×8, same pipeline, validated against the char-mode color rules.
- Naming convention: `monty_walk_0..3.png`, packed by the build script into one atlas + a generated C header of frame rects.

### 3.3 Animation data

Animations are data, not code: a table of `{frame, ticks}` pairs with loop/once flags. Death, crusher-warning, and exit-opening are `once` animations that fire gameplay events on completion (respawn, hitbox-active, room-transition).

## 4. Music & SFX (SID-style)

### 4.1 The instrument model

Emulate the SID's structure: **3 voices**, each one of {pulse (with pulse-width sweep), sawtooth, triangle, noise}, with ADSR envelopes, plus one filter. Implement as a tiny software synth (naive oscillators + ADSR are ~150 lines in C; band-limiting is unnecessary — aliasing is period-correct). Alternative: author in **GoatTracker 2** and play back exported register dumps through the **reSID** library for perfect authenticity — more setup, better sound. Recommendation: start with the tiny synth; swap to reSID later if the music matters enough. The music data format below works for either.

### 4.2 Composition plan

| Track | Voices | Style | Length |
|---|---|---|---|
| Title theme | 3 | jaunty 2-part melody + arpeggiated chords (the classic SID "chord on one voice" trick) + pulse bass, ~140 BPM | 32 bars, loops |
| In-game loop | 2 | bass + sparse lead, deliberately leaves voice 3 free | 16 bars, loops |
| Depth variation | — | every 10 rooms, transpose down a semitone and add filter sweep — cheap "descending dread" | — |
| Death jingle | 3 | descending chromatic run, 1.5 s | once |
| Room-clear jingle | 3 | rising arpeggio, 1 s | once |
| Game over | 3 | minor-key resolution of the title theme, 4 bars | once |

Music data format: a simple tracker pattern (rows of note/instrument/effect per voice), authored in GoatTracker or as text; the game ships with a pattern player, not audio files.

### 4.3 SFX and voice stealing

In-game music uses voices 1–2; **voice 3 is the SFX channel**, exactly like real C64 games. If two SFX overlap, priority wins (death > pickup > jump). SFX list: jump (short pulse blip up), land (noise tick), coal pickup (two-note triangle chirp), quota complete (same chirp ×3 rising), crusher slam (filtered noise burst), death (noise + pitch fall — also duck the music).

## 5. Procedural Endless Levels

### 5.1 Structure

The mine is a vertical chain of **flick-screen rooms**, each one screen: **20×16 metatiles** (each metatile = 2×2 chars = 16×16 px = 320×256 px), with the HUD overlaid on the top row. Monty enters at the top (entry shaft), must collect **all coal in the room** (quota shown in HUD), which opens the **exit shaft** somewhere at the bottom edge. Falling into the open exit = next room, depth +1.

### 5.2 Determinism

`roomSeed = splitmix64(runSeed ^ depth)`. One PRNG (xoshiro256**) seeded per room; **no other entropy source anywhere in game logic** (no `rand()`, no time). This makes daily runs, replays, and — critically — the entire test strategy in §7 possible.

### 5.3 Generation algorithm (per room)

1. **Anchors:** entry position on top edge (must match previous room's exit column — the shaft lines up), exit position on bottom edge (random, ≥6 metatiles horizontal offset from entry so the room can't be trivially fallen through).
2. **Path carve:** a biased drunkard's walk from entry to exit — 60% down, 40% lateral, never up. This guarantees a descending skeleton.
3. **Platform pass:** convert the skeleton into platforms and ladders: horizontal runs become floors, vertical runs become ladders *or* safe drop-shafts (a drop is "safe" if ≤ 4 metatiles — see fall damage rule in §6.2).
4. **Branch pass:** add 2–4 dead-end branches off the skeleton; **coal is placed only on skeleton and branches**, so quota is reachable by construction. 6–10 coal lumps per room.
5. **Hazard pass:** place enemies from a depth-gated budget (below). Placement rules: crushers only above floor tiles on the skeleton (they're the "timing test"); patrollers only on platforms ≥ 4 metatiles wide; bats only in open air ≥ 3 metatiles tall; nothing within 3 metatiles of the entry (spawn safety).
6. **Decoration pass:** rock texture variants, support beams, background gradient darkens with depth (cycle through blue → dark grey → black backgrounds every 10 rooms).
7. **Validation (the important part):** run the **solvability checker** (§5.5). On failure, reroll with `roomSeed+1`, max 5 attempts, then fall back to one of 8 hand-authored template rooms (stamped with random coal/hazard placement). The fallback guarantees generation *never* blocks the game.

> **Implementation note (milestone 2):** the shipped generator builds the *converted result* of steps 2–3 directly — a **platform-and-ladder chain** where each ladder column lies inside both the platform above and below — rather than carving a raw drunkard's walk and post-processing it. This is *connected by construction*, so rooms are solvable without relying on rerolls (measured: 0 fallbacks across 1200 rooms, depths 1–50). The drunkard's walk would produce many unsolvable rooms and lean heavily on the reroll/fallback path. The BFS checker (§5.5) still validates every room and is the thing the property tests exercise, so the safety net is intact if the generator is later made more organic. Enemies/spikes and key/lamp rooms are deferred to milestone 3 per §8; the exit is currently gated purely on the coal quota.

### 5.4 Difficulty ramp

A single `difficulty = min(depth, 50)` drives everything. Threats arrive one at
a time, and **helpers (moving platforms) ramp alongside them** to compensate:

| Depth | Dangers | Helpers |
|---|---|---|
| 1 | none — a completely safe tutorial room (always dark, with the **lamp**) | none |
| 2–3 | crusher (1 enemy) | 1 platform |
| 4–6 | + foremen patrollers; 2 enemies from 4 | 2 platforms from 6 |
| 7–9 | + bats; 3 enemies from 8 | 2 platforms |
| 10–12 | + spiders; occasional **key room** (key opens the exit); 4 enemies from 12 | 3 platforms from 12 |
| 13+ | + boulders; budget reaches 5 at 16 | 3 platforms |
| 15 | the **menagerie**: one of every enemy type in one room | 3 platforms |
| 20+ | occasional darkness rooms return (limited sight until the lamp) | 3 platforms |

Enemy budget: `1 + depth/4`, capped at 5. Crusher cycle speeds up with depth
(period `120 - 2*difficulty` ticks, floor 60). Coal quota: `6 + difficulty/10`.

### 5.5 Solvability checker

A jump-aware reachability BFS over the metatile grid, using the *actual movement constants* from §6.2 compiled into a reachability rule: from any standable tile, the player can reach tiles within the jump envelope (≤ 2 metatiles up, ≤ 3 across), climb connected ladders, and survive falls ≤ 4 metatiles. The room is valid iff: every coal lump is reachable from entry, the exit is reachable from the "all coal collected" state, and (for key rooms) key → exit ordering holds.

> **Body-height rule (milestone 2):** Monty's hitbox is 18px = 2 metatiles tall, so a cell is only *occupiable* if both it and the cell above it are clear of solids. The checker enforces this (a floor at head height blocks passage even when the feet cell is empty). Skipping this was a real bug — a floor 2 rows above the bedrock walkway walled off the bottom of the room while the checker still called it solvable. The generator now also keeps ≥3 rows between any two floors (and above bedrock) so walkways always have head clearance; a property test asserts the bedrock walkway is never obstructed.

Crushers and enemies are ignored by the checker — they are timing obstacles, not walls — *except* a rule that no crusher's kill zone may cover the only standable tile of a mandatory path (checked by re-running BFS with each crusher's floor tile removed; if unreachable, that crusher is deleted).

This checker is pure logic over the grid — no rendering, no engine — which makes it unit-testable and fast (< 1 ms/room).

### 5.6 Moving platforms

Dynamic solids the player can stand on and ride — horizontal or vertical, oscillating between two points at a fixed 0.5 px/tick (deterministic; no entropy). They are modelled as **entities**, not tiles, so they sit outside the static metatile grid the solvability checker and solver-bot reason about. That is deliberate: platforms are **additive** — placed only in open regions (with rider headroom, away from spawn), never removing or replacing the ladder-chain route the checker validates — so a room is always solvable *without* them and the checker/bot stay correct (both simply ignore platforms). They add movement and risk: ride one across a gap as a shortcut, or get carried into a crusher.

Physics: a platform is a one-way surface (land from above). Standing on one, the player is *carried* by the platform's per-tick delta before their own physics runs, and the ledge/support checks treat a platform top as ground. Depth-gated: from depth 2, up to two per room. Unit tests confirm a rider is carried both horizontally and vertically and that runs stay byte-identical.

**Anchoring:** platforms aren't dropped in random gaps — the generator records the static floor layout (rows + spans) and anchors every platform to it. Each is guaranteed **boardable from at least one fixed platform** (its board surface aligns to a floor's standing level, just off that floor's edge), and *most* are **vertical lifts connecting two floors** — board off an upper platform's edge, ride down to a lower one that spans the lift column. A minority are horizontal ledges off an edge (reachable from that one floor, a ride out over the gap). A property test over 600 rooms asserts every platform is anchored; measured split ≈ 96% connecting lifts, 4% ledges.

### 5.7 Crumbling floors

The "unstable mine" made literal (Manic Miner / Monty Mole lineage): a short run of a platform's floor is **weak** — solid until Monty stands on it, then it cracks and collapses ~0.6s (30 ticks) later, dropping whatever's on it. It's a `TILE_CRUMBLE` tile with a per-cell state byte (0 intact, counting-down while armed, −1 collapsed) held in `GameState`, so it's deterministic and memcmp-comparable. The whole thing hangs off one carefully chosen point: `sim_tile_at` reports an intact weak tile as `SOLID` and a collapsed one as `EMPTY`, so *all* collision and the solvability checker treat it correctly with no other changes. It arms from the cell under the player's feet, ages every tick, and **respawn re-solidifies every weak tile** so a death never soft-locks a room. Generation keeps the ladder attachment solid and clears any coal that would be stranded above a weak tile; since weak tiles read solid at generation, the checker's guarantee is untouched (collapse is a timing hazard, like crushers). The solver-bot treats them as permanent floor (it validates geometry, not timing). Depth-gated from depth 4.

## 6. Game Logic

### 6.1 State machine

`BOOT → TITLE → PLAYING ⇄ PAUSED`, `PLAYING → DYING (24 ticks) → PLAYING (respawn, lives−1) | GAME_OVER → HIGH_SCORE → TITLE`. Room transition is a sub-state of PLAYING (1-second shaft-drop animation, next room generated during it — generation is < 1 ms so this is pure theater).

### 6.2 Movement constants (at 50 Hz, in pixels/tick)

- Walk: **1.5 px/t** (75 px/s). Acceleration: instant (8-bit feel — no momentum).
- Jump: initial vy = **−3.5**, gravity **+0.25/t** → apex ≈ 24 px (1.5 metatiles up), airtime ≈ 28 t, max jump span ≈ 42 px (~3 metatiles). No variable jump height. *Playtest outcome (milestone 1):* the fully fixed arc was too punishing — **minimal air control** was added: ±0.125 px/t² steering while airborne, capped at walk speed.
- Fall: same gravity, terminal **4.0 px/t**. Falling **> 4 metatiles (64 px) of drop = death** (measured from walk-off/apex to landing).
- Ladders: up/down to attach (down also works standing on a ladder top); climb **1.0 px/t**; gravity suspended while attached. Ladder tops are walkable ground; falling onto a ladder grabs it (unless steering away). Exit anytime: jump off, or step off sideways where no solid tile blocks.
- Death: any overlap with enemy hitbox, spike tile, crusher in slam frames, or over-fall. One hit. 3 lives, +1 life every 10 rooms.

### 6.3 Update order (per tick, fixed)

input → player physics → player-vs-tile collision resolve → entities update (each type's own logic) → entity-vs-player overlap checks → pickups → room rules (quota/exit) → animation timers → audio triggers.

Deterministic order matters for replay/testing — never iterate entities in hash order.

### 6.4 Collision model

- Player vs tiles: AABB (hitbox 10×18, smaller than sprite — generous, arcade-fair) vs metatile grid, resolved axis-separated (X then Y). Tile classes: solid, one-way platform (Monty-style floors you jump through), ladder, spike, exit.
- Player vs entities: AABB overlap, with enemy hitboxes ~70% of their sprite (fairness bias toward the player, standard for one-hit-kill games).
- Entities vs tiles: patrollers edge-detect (probe tile ahead+below); boulders reflect off walls, fall off edges.

### 6.5 Scoring & HUD

Coal = 25 pts, room clear = 100 + depth×10, key = 50. HUD (top char row): score, lives (mole icons), coal `n/quota`, depth. Leaderboard ("deepest dives") of 20 entries, persisted locally: each row is a name (the human is `YOU`; bots use their name label), the depth reached, and the deaths that run took. Ordered depth-descending, deaths-ascending; one row per name (a returning name must beat its own best). Entries are recorded when a run ends: human game over (records the human and the right-screen bot) and demo arena end (records both bots).

## 7. Game Checks — validation & testing

Two layers: **runtime guards** (ship in debug builds) and an **automated test suite** (CI).

### 7.1 Runtime guards (debug assertions)

- Palette/asset validator at load (every asset obeys §2 color rules).
- Sprite budget: assert ≤ 8 sprites active; generation must never violate it.
- Player position always inside room bounds after collision resolve (catches tunneling).
- Every room entered carries a passed-validation flag; assert it.
- Audio: assert no voice plays two notes in one tick (catches trigger logic bugs).

### 7.2 Automated tests (headless — this is why the engine must separate sim from render)

Structure the code so `tick(state, input) → state` is pure and rendering is a read-only view of state. Then:

1. **Determinism test:** run 10,000 ticks of scripted input twice from the same seed; states must be byte-identical. Guards against hidden entropy and iteration-order bugs — the foundation for everything below.
2. **Generation property tests:** for 100,000 seeds × depths 1–100: room generates without fallback ≥ 99% of the time; solvability checker passes; spawn safety holds; sprite budget holds; entry column matches previous exit. Log the failing seed on any failure — the seed *is* the repro.
3. **Physics unit tests:** jump apex/span match §6.2 exactly; one-way platforms passable from below only; fall-death triggers at 65 px and not at 64; ladder attach/detach edge cases.
4. **Solver-bot integration test:** a bot that follows a BFS path (walk/climb/drop commands derived from the reachability graph) and drives the *actual physics* to clear rooms at depths 1, 5, 10, 25, 50. This tests that the *checker's model of movement* and the *actual physics* agree — the classic procgen failure mode is those two drifting apart. *Milestone-3 status: implemented as a **partial** oracle.* It already earned its keep — driving it exposed the head-clearance-on-drops bug in **both** the checker and the physics (a checker edge that stepped a body-height player through a floor at head level). The bot currently clears ~67% of rooms; the misses are its own navigation-execution gaps (precise drops onto 1-tile platforms, chained ladder→platform→ladder transitions), *not* unsolvable rooms — every failing room passes the checker and has a hand-verifiable path. So the selftest uses the clear **rate** as a regression floor (a generator that began emitting unclearable rooms would collapse it), not a strict per-room proof. **Future work:** a waypoint-committed navigator (precompute the full path, settle at each waypoint) to push the rate toward 100% and make it a strict gate.
5. **Golden seeds:** 20 pinned seeds with snapshot-tested room layouts, so intentional generator changes show up as reviewed diffs, not silent shifts.
6. **Performance budget:** full tick + room generation under 2 ms on target hardware (laughably easy in C, but the assert catches accidental O(n²)).

### 7.3 Manual QA checklist (per milestone)

- Crusher timing feels dodge-able on reaction at depth 2 and learnable at depth 30.
- No unreadable hazard overlaps (enemy paths crossing coal).
- Death always attributable ("I know what killed me").
- Music loop seam inaudible.
- 10-minute play session produces no impossible-feeling room.

## 8. Milestones

1. **Skeleton (week 1):** ✅ *Done.* framebuffer + palette + tile renderer + fixed tick loop + one hand-made room + Monty movement. Feel gate passed (air control added). Sim/render split established; `--selftest`/`--shot` dev modes.
2. **Generator (week 2):** ✅ *Done.* xoshiro256\*\* PRNG, platform generator, BFS solvability checker, coal + quota-gated exit, endless descent, depth-banded background. The generator lays a connected descending **chain** (guaranteed spine) and then adds **extra ledges — multiple platforms per height** — each tied by its own ladder to a neighbouring platform (row above or below), turning each room into a branching maze: collecting all the coal (now spread across every ledge) means winding up and down through ladders and across the floating (moving) platforms. Every ledge stays reachable by walk/climb (no jumps required), so the checker passes and the bots can navigate; 1200-room suite still generates 0 fallbacks and the solver-bot clear-rate rose (more ladders = more routes). Tests: physics units, 1200-room generation property suite (all solvable, 0 fallbacks, shafts aligned), generation + play determinism. Still open before M3: solver-bot test (§7.2.4) and golden seeds (§7.2.5).
3. **Danger (week 3):** 🚧 *Mostly done.* All five enemies implemented, deterministic (integer timers / triangle waves, no float `sin`; any "random" enemy choice draws from a seeded PRNG stored *in* `GameState` and advanced only during `tick`, so replays and the daily seed stay bit-identical): crusher (ceiling piston, depth ≥1), foreman (roams platforms + climbs ladders, **picking a random continuation at each ladder junction** and a random heading when it lands; also **steals coal** — grabs a lump it walks over, hauls it a few seconds, then drops it on another platform cell, so the layout of what you still have to collect keeps shifting; depth ≥5), bat (diagonal sweep bouncing off terrain, depth ≥10), spider (bobs on a ceiling thread, depth ≥20), boulder (rolls along platforms, falls off edges, depth ≥20). Depth-gated placement + difficulty ramp per §5.4, spawn-safe zone, ≤8-sprite budget, one-hit contact death (hitboxes ~70% for fairness). Level 3 spawns one of every type (test aid). Solver-bot integration test (§7.2.4) implemented as a partial oracle — it drove out a head-clearance bug in the checker + physics; clears ~67% of rooms, gated as a regression floor. **Special rooms done:** key rooms (depth ≥10, ~1 in 4) hide a key the exit also requires, with a HUD indicator; dark rooms (depth ≥20) black out everything beyond a square of sight around Monty until he grabs a lamp. Both pickups are placed on reachable platform cells (the checker verifies reachability), and darkness is a pure render effect so determinism holds. **Still to do:** crusher-path-safety rule (§5.5) and a fully robust solver-bot navigator (future work).
4. **Sound (week 4):** ✅ *Done (bar polish).* Tiny SID-style software synth ([synth.c](src/synth.c)): 3 voices, pulse/triangle/saw/noise waveforms, LFSR noise, per-segment attack/release envelopes (no clicks). SFX wired to game events (jump, land, coal pickup, quota-met, death, descend) with priority-based voice stealing. Two-part looping music (melody + bass) via a per-row pattern player — a title theme and an in-game loop — with the §4.2 depth transpose (down a semitone every 10 rooms); music leaves a voice free for SFX. The whole synth lives in the output layer (never touches `GameState`, so determinism holds) and its sample generation is device-free, so it's unit-tested headlessly: silent-when-idle, audible-when-triggered, self-terminating, music-loops-audibly, music-leaves-SFX-voices-free, SFX-over-music. **Optional later:** hand-composed longer tracks / swap to reSID if the music warrants it.
5. **Shell (week 5):** 🚧 *Mostly done.* State machine per §6.1: `TITLE → PLAYING ⇄ PAUSED`, `PLAYING → GAME_OVER → (HIGH_SCORE entry) → TITLE`. Title screen with the high-score table; game-over and 3-initial name-entry screens; an 8-entry high-score table (§6.5) persisted to a local file. Two start modes: a normal run (time-seeded) and a **daily run** (seed derived from the date, so everyone gets the same mine that day). Pause on P/Esc. **Attract/demo mode:** after ~10s idle on the title the solver-bot takes over and plays the live game (a blinking "DEMO" banner; any key returns to the title). The bot's per-tick brain (`bot_decide`) is shared with the headless solver-bot test, so the same navigator drives both; a watchdog reseeds the demo if the bot stalls or dies out, so attract mode never hangs. For the live game the demo bot adds three reactive safety layers on top of the shared navigator:

- **Enemy avoidance** (`sim_enemy_threat`): it won't step into a hazard and backs off one that closes on it — but only onto solid ground, never off a ledge. Crushers are handled by *waiting*: a crusher's whole slam column reads as dangerous while it's about to drop or is down, so the bot holds until the piston retracts and only then darts under it.
- **Cliff guard**: the static pathfinder occasionally mis-plans a drop, so before stepping off any ledge the bot checks there's a survivable landing within reach; if not, it stops at the edge instead of plunging. Ground control is momentum-free, so cancelling the step is enough. Exempt: the exit shaft (an intentional bottomless drop), and — only at a room-edge column, where the side wall zeroes the bot's drift so it falls straight — edging off a platform lip into its own column's shaft (a coal shaft it means to descend; without this it would freeze at the lip).
- **Anti-dither**: if the planner flip-flops the walk direction on the spot with no vertical progress, the bot stands still rather than vibrate.

The enemy-avoidance retreat starts a tile out (not on contact) so a walking foreman never catches the faster bot, and the side probes sit at the bot's own height so bats overhead don't spook it. When a foreman or boulder blocks the way to the target and is coming toward the bot, it **hops over it** instead of retreating — provided the arc is clear and the landing two cells on is solid and enemy-free (it won't try to hop bats, crushers, or a foe moving the way it's jumping). On a ladder the bot keeps climbing past a crusher in the *neighbouring* column (the piston is one tile wide) and only breaks off for a hazard directly on it. It also **retreats to higher ground**: cornered on a platform with no safe sideways escape, or already on a ladder with a hazard closing in, it climbs up rather than holding its level. At the exit shaft it presses *down* once its body is over the shaft column — a ladder in the shaft would otherwise be grabbed-and-hung-on when it tried to fall in, so it climbs the ladder down into the exit instead (harmless on a plain hole). (Crushers are never placed over a ladder column — a slam there would wall off the climb.) The demo watchdog reseeds only when the bot dies out — stall-reseed is disabled so a room it can't solve stays on screen for inspection.

(Known cosmetic quirk: while waiting out a crusher the bot can jitter left/right, since the flee reflex fires before the anti-dither damping — harmless, left in as attract-mode character.)

It's a heuristic, not a perfect player — fast bats and crushers still catch it occasionally and some rooms it simply can't solve (it reseeds past those) — but it descends steadily and almost never falls to its death. The sim now raises `PS_GAMEOVER` when lives run out (instead of silently resetting) and the shell drives the flow from there — the time/date seeding lives in the shell, never in the deterministic sim. **Still to do:** polish pass (palette-flash on hit instead of screen shake), room-transition shaft animation, optional title-theme differentiation.

### Two-screen bot arena

The window shows **two 320×256 screens side by side** (each its own render texture, blitted L/R with a divider). Both run an independent `GameState` on **one shared seed**, so at any depth both face the *identical* room — a fair head-to-head.

- **Left screen:** `bot1` when the demo is running, or the **human player** (arrow keys, the same controls as before) when a run is started from the title.
- **Right screen:** always `bot2`.

`bot1` and `bot2` are **completely independent** — [bot1.c](src/bot1.c) / [bot2.c](src/bot2.c) each hold a full private copy of the whole play stack (cell math, BFS pathfinder, `decide`, safe-landing / jump-over helpers, avoidance + cliff-guard + anti-dither, and their own `Nav` state). They share **no** code, so either bot's algorithm can be rewritten without touching the other — the point is to A/B different behaviors on identical rooms. They start out identical (the tuned navigator described above); diverge as experiments dictate. (The headless SOLVER in [main.c](src/main.c) that validates room reachability for `--selftest` is a separate thing again, unrelated to bot1/bot2.)

In the demo the bots have **endless lives** — a death just respawns them and they keep trying — and the arena rolls a fresh mine for both when **either** both bots have sat motionless for >10 s (genuinely stuck) **or** neither has solved a maze (descended a level) in 60 s (catches a bot that keeps dying without progressing). Until one of those trips, nothing resets: while at least one bot keeps descending, the mine stays. Because both share one seed, they always face identical rooms. During a human run the left screen is the player (normal 3 lives → game-over → high-score entry) and the right-hand `bot2` plays endlessly, replaying the same seed if it ever sits fully stuck for >10 s. Audio follows the left screen.

Both bots now aim for the **nearest reachable** coal (skipping jump-gated lumps so they don't fixate and freeze) and reach the exit by heading to the nearest reachable **bedrock-walkway** cell then strolling to the shaft (the bedrock is continuous), which avoids the old "frozen at a platform lip" bug. Their pathfinder also refuses **drift-unsafe drops** — a drop whose momentum would carry them one column too far into an adjacent pit (the classic "stuck at the platform edge, blind to the ladder that goes down" case): rejecting it forces bfs to route via a nearby ladder instead. This roughly halved how often either bot has to reseed. Where they differ is the escape from a genuinely jump-gated lip: **bot1** is conservative (stands, lets the watchdog reseed), while **bot2** is aggressive — after ~1.7 s frozen it takes a running hop in its last heading to try to clear the gap. In a 20-min measured session both reach ~depth 8; bot2 escapes a few more traps (fewer reseeds) at the cost of more deaths from whiffed jumps. Neither can permanently stick.

### Scriptable bots — MoleScript

Either screen can instead be driven by an external **MoleScript** program (a `.mole` file), so other people or AI models can write and test bots against the same rooms. `undermine --bot1 f.mole` and `--bot2 g.mole` load a script into that slot; an empty slot uses the built-in C bot, so the defaults above are unchanged. A malformed script is reported and falls back to the C bot — the game never crashes on bad input.

MoleScript is a tiny, deterministic, sandboxed language (its own compiler + resumable stack VM in [mole.c](src/mole.c); public API in [mole.h](src/mole.h)). A program reads top-down with one main loop: control flow and sensors are instantaneous, while the seven **actions** (`walk` `climb` `jump` `drop` `wait` `idle` `go_to`) consume ticks by suspending the VM in place and resuming next tick — so it feels continuous while still emitting exactly one `Input` per 50 Hz tick, the same contract as the C bots. Movement is **tile-granular** (`walk left` = one tile, then control returns to the loop). Sensing is split: **full knowledge** of static terrain and all coal (fair to plan over) but **limited perception** of enemies (only within `sight` = 4 tiles). An enemy value carries its side, distance, movement heading (`.going`), type, and `.state` — for a crusher, `idle` / `warning` (flashing, about to slam) / `moving`, so a bot can decide whether to dash under it or wait. `go_to` reuses bot2's drift-safe walk/climb/drop BFS, advancing one tile per call so a script stays reactive. Determinism is preserved: no clock, a private seeded RNG for `random()`, and the interpreter only ever reads `GameState`. `go_to` finds a shortest route but a script keeps control of *which* one: an optional **bias** (`go_to(cell, left|right|up|down)`) breaks ties in the script's chosen direction, and **`cell(col,row)`** lets it route through its own waypoints — so two bots pick their paths by their own logic (deterministically) instead of moving in lockstep on a shared pathfinder. For fully hand-driven movement, **`route(cell)`** returns just the next-step direction (`left/right/up/down`) so a bot steers itself with `walk`/`climb` yet still gets down off the top level and around a room (the `walk` action aligns to a ladder rail before stepping off; the exit's shaft-drop still uses `go_to(exit)`). Every `.mole` file's **first line must be a `# author: …` comment** (AI model or person); the loader surfaces it (`bot1: loaded … (author: …)` / on the title). `--moletest f.mole` runs a script headlessly over 20 seeds; `--selftest` compiles the bundled example, checks it descends and replays identically, verifies a bad script is rejected, and checks the enemy/crusher/jump-over/path-bias behaviours. Reference: [MOLESCRIPT.md](MOLESCRIPT.md); worked examples: [bots/greedy.mole](bots/greedy.mole) (neutral) and [bots/highroad.mole](bots/highroad.mole) (left-biased routing).

## 9. Open design risks

- ~~**Fixed jump arc, no air control**~~ *Resolved in milestone 1 playtesting:* the fixed arc was too punishing; minimal air control (±0.125 px/t² steering) is now in. The solvability checker (milestone 2) must use the base jump envelope only, so air control stays a comfort margin, never a requirement.
- **Sim/render separation** (pure `tick()` function) is load-bearing for all of §7 — it must be established in milestone 1, not retrofitted.
- **Naming/IP:** Monty Mole is a Gremlin Graphics trademark; this is a homage with original name, art, and music. Do not reuse original sprites, level layouts, or the name.
