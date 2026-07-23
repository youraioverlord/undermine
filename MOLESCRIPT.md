# MoleScript — language specification

Reference for writing UNDERMINE bot programs (`.mole` files). Optimized for
automated code generation: every rule is stated explicitly; the CONSTRAINTS and
COMMON ERRORS sections are normative. A human-oriented tutorial is in
`molescript-human-readme.md`; the original prose reference is
`MOLESCRIPT-original.md`.

## 0. Invariants (read first)

- REQUIRED: line 1 of the file is `# author: <name>` (the generating model's
  name, or the human author's name).
- Execution is deterministic: same script + same seed ⇒ identical play. No
  wall clock, no hidden randomness (`random(n)` is seeded and private).
- Sensors, arithmetic, control flow, and `say` are **instantaneous** (cost 0
  ticks). Actions (`walk climb jump drop wait idle go_to`) **consume ticks**;
  the program suspends at the action and resumes on the next statement.
- Movement is **tile-granular**: one action = one tile (or one tick for
  `idle`, N ticks for `wait N`).
- The whole program restarts from the top when it falls off the end (implicit
  outer loop). Convention: wrap main logic in an explicit `loop: … loop_end`.
- A condition must fit on one line.
- The interpreter is sandboxed (no file/network access, bounded work and
  memory per tick). A script that fails to compile is reported on stderr and
  the slot falls back to the built-in bot; the game never crashes on a bad
  script.

## 1. File skeleton (canonical template)

```
# author: <model or person>          ← REQUIRED first line

func helper(a, b):                   ← functions only at top level
    return a + b
func_end

loop:                                ← main loop
    ...
loop_end
```

## 2. Block terminators (exact pairing)

| Opener | Terminator |
|---|---|
| `func name(params):` | `func_end` |
| `loop:` | `loop_end` |
| `while COND:` | `while_end` |
| multi-line `if COND then` | `end` |

`end` closes **only** multi-line `if` blocks — never loops or functions.

## 3. Statements

```
name = expr                          # assignment
if COND then STMT                    # single-line if: NO terminator
if COND then STMT else STMT          # single-line if/else: NO terminator
if COND then                         # multi-line if: terminated by `end`
    ...
elif COND then                       # zero or more elif arms (multi-line form ONLY)
    ...
else                                 # optional final arm
    ...
end
while COND:
    ...
while_end
loop:
    ...
loop_end
break
continue
return EXPR                          # inside func
say EXPR [, EXPR ...]                # introspection line — see §5.1; costs 0 ticks
ACTION                               # see §5
```

Disambiguation rule: a statement on the same line as `then` ⇒ single-line
form (ends at end-of-line, no `end`). A newline right after `then` ⇒
multi-line form (requires `end`). The first arm whose condition is true runs;
the rest are skipped.

Operators: `+ - * /` · comparisons `== != < <= > >=` · logic `and or not` ·
parentheses group.

## 4. Values and truthiness

| Kind | Notes |
|---|---|
| number | integers |
| boolean | `true` / `false` |
| cell | map position; fields `.col` `.row` |
| enemy | fields `.col` `.row` `.dist` `.dir` `.going` `.type` `.state` |
| direction | `left right up down above below any` (returned by `route`, `side_of`, `facing`) |
| string | `"double-quoted"`, max 31 chars, no escapes — used with `say` only |
| `none` | absence of a value |

Truthiness: `none` and `0` are false; **everything else is true** (any cell or
enemy value is true). Idiom: `if nearest_coal() then …` = "some coal is
reachable".

Variable scope: a variable assigned at top level **persists across ticks**
(use for counters / state machines). Function locals do **not** persist
between calls. Functions take parameters and may `return` a value.
**Functions do NOT close over top-level variables**: a function body sees
only its parameters and its own locals — naming a top-level variable inside
one is a compile error (`unknown name`). Pass state in as arguments and
hand results back with `return`; stateful logic belongs in the main loop.

## 5. Actions (tick-consuming)

| Action | Effect |
|---|---|
| `walk left` / `walk right` | move one full tile horizontally, settling centred in the next cell; also steps off a ladder (auto-aligns to the rail). Any other direction value (e.g. `side_of(0)` = `any`) walks NOWHERE — it stands for one tick |
| `climb up` / `climb down` | move one rung on a ladder, settling aligned (see §7 preconditions). Any other direction value stands for one tick |
| `jump` | hop straight up |
| `jump left` / `jump right` | hop over a one-tile gap |
| `drop` / `drop left` / `drop right` | step off the ledge and ride the fall; bare `drop` uses `facing` |
| `wait N` | stand still N ticks |
| `idle` | do nothing for exactly 1 tick |
| `go_to(cell)` | advance ONE tile along a path toward `cell`; evaluates to `arrived` / `moving` / `blocked` |

### 5.1 `say` — introspection (0 ticks)

`say EXPR [, EXPR ...]` builds the bot's status line: shown beside the bot's
name in-game, printed by `--moletest` whenever it changes (first seed only),
and logged to `bot1.log` / `bot2.log` in the working directory (one line per
change, stamped `[d<depth> t<tick>]`; the file is silently overwritten at
every run start).
Instantaneous — it never consumes a tick. Parts are joined with spaces; all
`say`s within one tick append to one line; the first `say` of a new tick
starts a fresh line; the last line persists until the script says again.
Value formatting: string → text · number → `5` / `1.50` · bool →
`true`/`false` · cell → `(c,r)` · enemy → `foreman(c,r)` · `none` → `none`.

```
say "flee", e, "dist", e.dist        # -> flee foreman(7,5) dist 2
```

Use it while developing: narrate the bot's current goal/decision so you can
see WHY it did what it did. It has no gameplay effect.

## 6. Navigation

### 6.1 `go_to(cell [, bias])`

- Uses the walk/climb/drop pathfinder (it never jumps).
- Moves ONE tile per call, so calling it inside `loop:` keeps the bot
  reactive.
- Returns `arrived` / `moving` / `blocked`. Branch on `blocked` to try a
  manual `jump`.
- `go_to(exit)` handles the exit-shaft descent (precise centering) and
  reports `arrived` when the bot actually drops to the next level. ALWAYS use
  `go_to(exit)` for the exit — hand-driven `walk` cannot center precisely
  enough.
- Optional bias `left | right | up | down` breaks ties between equally short
  routes (route stays optimal; use it to differentiate two bots or to prefer
  a side): `go_to(nearest_coal(), up)`.

Canonical minimal bot (note the `key` arm — in a key room, depth >= 10, the
exit stays SHUT until the key is collected; a coal-then-exit bot stalls there
forever):

```
loop:
    if coal_remaining() > 0 then
        go_to(nearest_coal())
    elif key != none then
        go_to(key)
    else
        go_to(exit)
    end
loop_end
```

### 6.2 `route(cell)` — manual steering

Returns the next-step direction toward `cell` (`left right up down`), or
`none` if already there / unreachable. It only reports; you move:

```
d = route(nearest_coal())
if d == up    then climb up
if d == down  then climb down
if d == left  then walk left
if d == right then walk right
if d == none  then wait 1
```

Rules: feed `route`'s answer to `walk`/`climb` exactly as above; for the exit
still use `go_to(exit)` (see 6.1).

### 6.3 `cell(col, row)`

Builds an arbitrary map position — a target or waypoint for
`go_to`/`route`: `go_to(cell(my_col, 2), left)`.

## 7. Ladder rules

`go_to`/`route` climb ladders automatically — the rules below apply only to
manual `climb`:

1. To mount a ladder you must be **centred on its column**: `climb up` beside
   a ladder does nothing; `walk` onto the column first.
2. `climb down` requires a ladder **under** you; off a plain ledge it does
   nothing — use `drop` there.
3. `climb up` ends standing exactly on top of the ladder run; a ladder's top
   edge is `is_standable`.
4. Stepping off sideways is automatic: `walk left`/`walk right` on a ladder
   align to the rail, then step off.

## 8. Sensors

### 8.1 Full map knowledge (static terrain/coal — fair to plan over)

| Sensor | Returns |
|---|---|
| `coal_remaining()` | count of uncollected coal lumps |
| `nearest_coal()` | cell of closest **reachable** coal, else `none` |
| `coal_got`, `coal_total` | quota progress |
| `exit` | cell of the exit shaft |
| `key`, `lamp` | cell of key/lamp if present in this room and uncollected, else `none`. The key is MANDATORY: in a key room the exit stays shut until it is collected (`go_to(key)` when `key != none`). The lamp is optional (score + lights a dark room) |
| `tile(col, row)` | one of `EMPTY WALL PLATFORM LADDER SPIKE EXIT CRUMBLE` |
| `is_solid(c,r)` `is_ladder(c,r)` `is_standable(c,r)` | booleans |
| `depth` | current mine depth (level number) |

### 8.2 Self

`my_col` `my_row` `on_ground` `on_ladder` `facing` (`left`/`right`) `lives`.

### 8.3 Perception conveniences (range-limited to `sight` = 4 tiles)

These three are short-range *reaction* helpers; the FULL enemy roster —
including foes beyond sight — is always available via `enemy(i)` (§8.7),
exactly as the built-in bots see it.

| Sensor | Returns |
|---|---|
| `sight` | the helpers' range in tiles (4) |
| `nearest_enemy()` | closest enemy within sight, else `none` |
| `enemy_within(range, dir)` | true if a **mobile** enemy is within `range` tiles on side `dir` (`range` capped at `sight`) |
| `danger_at(col, row)` | true if a sensed enemy's body covers that cell right now |

`enemy_within` semantics: `left`/`right` = on your row (±1); `above`/`below` =
on your column (±1) — `enemy_within(2, below)` means "a foe on the ladder
below me", not down-and-to-the-side; `any` ignores direction. **CRUSHERs are
excluded** (they threaten only their own column; a ladder beside one is
safe) — test a crusher's slam cell with `danger_at`.

### 8.4 Enemy value fields

| Field | Meaning |
|---|---|
| `.col` `.row` | position |
| `.dist` | distance in tiles |
| `.dir` | which side of you it is on: `left`/`right` |
| `.going` | which way it is **moving**: `left`/`right` (approaching vs retreating) |
| `.type` | `CRUSHER FOREMAN BAT SPIDER BOULDER` |
| `.state` | `idle` / `warning` / `moving` |

`.state` matters chiefly for CRUSHER: `idle` = raised, at rest · `warning` =
flashing, a slam is imminent · `moving` = slamming or retracting.
Always-in-motion enemies (FOREMAN BAT SPIDER BOULDER) report `moving`.

Rule: TOUCHING any enemy's body is lethal — including a crusher's raised
body while `idle`. `idle` describes its timer, not its touch.

### 8.5 Misc

| Sensor | Returns |
|---|---|
| `cell(col, row)` | cell value for any map position |
| `route(cell)` | next-step direction (§6.2) |
| `random(n)` | deterministic integer in `0 .. n-1` |

### 8.6 Number/direction helpers

| Helper | Returns |
|---|---|
| `abs(x)` | magnitude of `x` |
| `distance(a, b)` | `\|a - b\|` in tiles, e.g. `distance(my_col, e.col)` |
| `lower(a, b)` / `higher(a, b)` | min / max |
| `limit(x, lo, hi)` | `x` clamped to `lo..hi` (expects `lo <= hi`) |
| `side_of(delta)` | direction: `left` if `delta < 0`, `right` if `> 0`, `any` if `0` |
| `sign_of(x)` | number: `-1` / `0` / `+1` |

Steering idiom (step away from an enemy):
`if e.dist <= 2 then walk side_of(my_col - e.col)`.

### 8.7 Full-knowledge sensors (parity with the built-in bots)

Everything the built-in C bots can read from the game state, a script can
sense too:

| Sensor | Returns |
|---|---|
| `enemy_count()` | total enemies in the room (no range limit) |
| `enemy(i)` | enemy value for index `0 .. enemy_count()-1`, else `none`. Full fields (§8.4); no range limit. A boulder parked off-screen for its spawn delay reads `.state == idle` (and an off-map position) until it drops in |
| `plat_count()` | number of moving platforms |
| `plat(i)` | cell of moving platform `i`'s left tile right now, else `none` |
| `plat_dir(i)` | direction platform `i` is moving right now (`left right up down`), else `none` |
| `on_platform` | true while standing on (riding) a moving platform |
| `shaking(col, row)` | true if the crumble tile there is armed and about to collapse |
| `score()` | the current score |
| `death` | cell where the player last died in THIS room, else `none`. Cleared on every new room |
| `deaths` | number of deaths in this room so far |
| `safe_ticks(i)` | for enemy `i`: ticks it is still GUARANTEED harmless. Nonzero only for an idle (raised) CRUSHER — its remaining rest time before the next warning; 0 for everything else |

Mechanics fact: a death resets every enemy to its level-start position, and
top-level script variables survive the respawn.

## 9. COMMON ERRORS (checklist — verify each before emitting a script)

1. Missing `# author:` on line 1. (Required.)
2. `end` after a single-line `if` — single-line `if` takes NO terminator.
3. `elif` in single-line form — `elif` exists only in the multi-line block.
4. Closing a loop/function with `end` — use `loop_end` / `while_end` /
   `func_end`.
5. Hand-walking into the exit shaft — use `go_to(exit)`.
6. `climb up` while beside (not on) the ladder column — no-op; `walk` onto
   the column first.
7. `climb down` off a plain ledge — no-op; use `drop`.
8. Condition split across lines — a condition must fit on one line.
9. Expecting function locals to persist — only top-level variables persist
   across ticks.
10. Forgetting that `nearest_coal()` can be `none` (all collected or none
    reachable) — guard before using its fields.
11. Treating `enemy_within` as covering crushers — it excludes them by
    design; use `danger_at` for slam cells.
12. Forgetting the KEY: from depth 10 some rooms hold one, and the exit stays
    shut until it is collected. Every goal chain needs a `key != none →
    go_to(key)` arm, or the bot stalls at a closed exit forever.
13. Passing a computed direction to `walk`/`climb` without handling the zero
    case — `side_of(0)` is `any`, and `walk any` deliberately walks nowhere.
    When the delta can be zero, decide what "already there" means first.
14. Reading a top-level variable inside a `func` — functions see only their
    parameters and locals (no closure over globals; compile error `unknown
    name`). Pass the value in as a parameter, or keep that logic in the
    main loop.

## 10. Complete example

A correct, minimal bot. It survives on reflexes alone — how much better a
bot can do against each enemy type, and when actions like `jump` pay off, is
left for the author to discover through testing (§11).

```
# author: Example Author

loop:
    e = nearest_enemy()
    if e != none and e.type != CRUSHER and e.dist <= 2 then
        say "avoiding", e
        walk side_of(my_col - e.col)    # step away from it
        continue
    end
    if coal_remaining() > 0 then
        if go_to(nearest_coal()) == blocked then jump right
    elif key != none then
        go_to(key)                # a key room's exit stays shut without it
    else
        go_to(exit)
    end
loop_end
```

## 11. Running and testing

```
undermine --bot1 bots/my.mole              # script drives the demo/left screen
undermine --bot2 bots/my.mole              # script drives the right screen
undermine --bot1 a.mole --bot2 b.mole      # two scripts head-to-head
undermine --moletest bots/my.mole          # headless: 20 seeds, reports clears
```

A slot without a script is played by the built-in C bot. In the game, load a
script from the title screen with F2 (left) / F3 (right).
