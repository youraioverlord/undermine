# UNDERMINE — TODO

## Features & tooling

- [ ] **`--help` for the command line.** There is no usage text; every mode
  (`--selftest`, `--moletest`, `--probelad [edge|mid|builtin1|builtin2]`,
  `--probegap [crusher]`, `--probekey [N]`, `--demodebug [2]`, `--botdebug`,
  `--shot FILE WHAT [SEED]`, `--bot1/--bot2 FILE`) is only discoverable by
  reading `main.c` or the README. Add `--help` (and unknown-flag fallback)
  printing a one-line summary per mode.

- [ ] **Include deaths in the `--moletest` headline.** The headline reads
  `descents=60 maxDepth=7 over 20 seeds` but deaths are half of the game's
  own ranking (deepest dive, *fewest deaths*). Count deaths across the 20
  seeds and report e.g. `descents=60 maxDepth=7 deaths=41 over 20 seeds`, so
  the benchmark number matches what the leaderboard rewards and an AI's
  refinement loop optimizes the right thing.

## Documentation

- [ ] **Training-data contamination caveat.** Once the game is released,
  `MOLESCRIPT.md`, the cheat sheet, the example bots, and write-ups about
  them can be scraped into AI training corpora — so no future model is ever
  truly *virgin* to the system, and the "closed world" claim in the README
  weakens over time. Document this honestly: note the release date as the
  contamination horizon, state that clean-room results are only strictly
  valid for models with a training cutoff before it, and suggest mitigations
  (versioning the spec so post-release changes are detectably unknown to a
  model; comparing models against same-cutoff peers; treating suspiciously
  tactic-aware first drafts as a contamination signal, since the tactics
  deliberately live outside the spec).

## Test & guard infrastructure (flagged in DESIGN.md §7)

- [ ] **Golden seeds** (§7.2.5): 20 pinned seeds with snapshot-tested room
  layouts, so intentional generator changes show up as reviewed diffs.
- [ ] **Performance budget** (§7.2.6): assert full tick + room generation
  stay under 2 ms.
- [ ] **Runtime debug asserts** (§7.1): in-bounds after collision resolve,
  room-passed-validation flag, sprite budget, one-note-per-voice.
- [ ] **Solver-bot as a strict gate** (§7.2.4): waypoint-committed navigator
  to push the clear rate from ~67% toward 100%.
