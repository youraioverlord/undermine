# MoleScript

MoleScript is a tiny, human-readable language for writing UNDERMINE bots. A
`.mole` program is compiled and run inside the game, driving one screen exactly
as a player would — it can go in any direction, jump, wait, and navigate, and it
senses the world through a mix of full map knowledge and short-range perception.

Load a script from the command line:

```
undermine --bot1 bots/greedy.mole                 # script drives the demo/left screen
undermine --bot2 bots/greedy.mole                 # script drives the right screen
undermine --bot1 a.mole --bot2 b.mole             # pit two scripts against each other
```

If a slot has no script, the built-in C bot plays it. Test a script headlessly:

```
undermine --moletest bots/greedy.mole             # runs it over 20 seeds, reports clears
```

---

## How it runs

The game ticks at 50 Hz. Each tick the script must produce one movement. You do
**not** write per-tick code, though — you write a normal top-down program with
one main loop, and two kinds of statement behave differently:

- **Sensors and control flow are instantaneous.** `if`, `while`, `loop`,
  function calls, arithmetic, and every sensor read happen "between frames." You
  can do as many as you like before the program moves.
- **Actions consume ticks.** `walk`, `climb`, `jump`, `drop`, `wait`, `idle`,
  and `go_to` take time. When the program reaches one it suspends there until the
  action finishes, then resumes on the next statement — so the whole thing reads
  as if it ran continuously.

Movement is **tile-granular**: `walk left` moves one tile (over however many
ticks that takes), then control returns to your loop so you can re-check the
world. This keeps a bot reactive without you managing individual frames.

The program has an **implicit outer loop**: when it reaches the end it starts
over. Usually you write an explicit `loop:` for clarity.

Everything is **deterministic**: no clock, no hidden randomness (`random()` uses
a private seeded stream), and the interpreter never modifies the game except
through the move it returns. The same script and seed always play identically.

---

## Language

### Authorship (required first line)

The **very first line** of every `.mole` file must be a comment identifying who
wrote it — the AI model that generated it, or a person's name if a human wrote
it — so authorship is always clear:

```
# author: Claude Opus 4.8          (or e.g. "# author: Jane Q. Coder")
```

### Structure

```
# author: <model or person>        # required first line

func helper(a, b):        # functions are defined at the top level
    ...
    return a + b
func_end

loop:                     # the main loop
    ...
loop_end
```

Each block has its own terminator, so it's always clear what's being closed:
`func_end`, `loop_end`, `while_end`, and `end` (which closes a multi-line `if`).

### Values

Numbers, booleans (`true` / `false`), **cells** (a map position with `.col` and
`.row`), **enemies** (with `.col .row .dir .dist .type`), and `none`.

Truthiness: `none` and `0` are false; everything else (including any cell or
enemy) is true. So `if nearest_coal() ...` is true when some coal exists.

### Control flow

```
if COND then STMT                      # single line — no terminator
if COND then STMT else STMT            # single line with else

if COND then                           # multi-line block — closed by 'end'
    ...
elif COND then                         # zero or more elif arms
    ...
else                                   # optional; the fall-through arm
    ...
end

while COND:                            # closed by 'while_end'
    ...
while_end

loop:                                  # the main loop, closed by 'loop_end'
    ...
loop_end

break        continue
return EXPR
```

A **single-line** `if` (statement written right after `then`, on the same line)
takes no terminator — it ends at the line. A **multi-line** `if` (a newline right
after `then`) runs until `end`, and may chain `elif COND then` arms and one final
`else` before the `end`; the first arm whose condition is true runs and the rest
are skipped. (`elif` is only for the multi-line block form.) Loops and functions
use `loop_end` / `while_end` / `func_end`, so `end` always and only closes an `if`.

```
if e.type == FOREMAN then
    step_away(e.col)
elif e.type == BAT then
    drop
elif e.state == warning then
    wait 2
else
    go_to(nearest_coal())
end
```

Operators: `+ - * /`, `== != < <= > >=`, `and or not`. Parentheses group. A
condition must fit on one line.

### Variables and functions

Assign with `name = expr`. A variable assigned at the top level keeps its value
across ticks (handy for counters and state machines). Function locals do not
persist between calls. Functions take parameters and may `return` a value.

---

## Actions

| Action | Meaning |
|---|---|
| `walk left` / `walk right` | move one tile horizontally |
| `climb up` / `climb down` | move one tile on a ladder |
| `jump` / `jump left` / `jump right` | a hop over a one-tile gap; bare `jump` goes straight up |
| `drop` / `drop left` / `drop right` | step off the ledge and ride the fall; bare `drop` uses `facing` |
| `wait N` | stand still for N ticks |
| `idle` | do nothing this one tick |
| `go_to(cell)` | move **one tile** along a path toward `cell`; evaluates to `arrived`, `moving`, or `blocked` |

**Jumping over an enemy is hard** — the jump arc rises slowly at first, so a hop
at an enemy **one tile away collides on the way up and dies**. It only clears a
ground enemy (foreman/boulder) launched when the enemy is about **two tiles
ahead and moving toward you** (check `.going`), landing one tile past it. Do not
hop at closer enemies — sidestep, `wait`, or retreat instead. Bats and spiders
can't be jumped over (the arc passes through them).

`go_to` uses the same walk/climb/drop pathfinder as the built-in bots (it does
not jump). Because it advances a single tile per call, putting it in your loop
keeps you reactive:

```
loop:
    if coal_remaining() > 0 then go_to(nearest_coal()) else go_to(exit)
loop_end
```

`go_to(exit)` knows how to descend the open shaft, so it reports `arrived` when
you actually drop to the next level. You can branch on the result:

```
if go_to(nearest_coal()) == blocked then
    jump right          # try to hop a gap the walker can't cross
end
```

### Choosing your own path

`go_to` finds a *shortest* route, but when several are equally short you decide
which one with an optional **bias**: `go_to(cell, left | right | up | down)`. The
bias only breaks ties, so the route stays optimal — but it's **your logic**, not
chance, that steers it, and two bots that bias differently take different paths
instead of moving in lockstep. Change the bias whenever you like:

```
if coal_remaining() > 0 then go_to(nearest_coal(), up)   # I like the high road
else go_to(exit, down) end
```

You aren't limited to sensor-given targets, either: **`cell(col, row)`** builds
any map position, so you can route through waypoints your own way — e.g.
`go_to(cell(my_col, 2), left)` to work along the top by the left side first.

### Driving movement yourself with `route`

If you'd rather steer every step (dodge, feint, take your own detours) instead of
handing a whole tile-move to `go_to`, ask the pathfinder only for a *direction*:

`route(cell)` → `left` / `right` / `up` / `down` (the next step toward `cell`
along the walk/climb/drop route), or `none` if you're already there or nothing
can reach it. It just tells you the way — **you** do the moving:

```
loop:
    if coal_remaining() == 0 then
        go_to(exit)              # the exit is a hole — let go_to drop into it
        continue
    end
    d = route(nearest_coal())    # which way to the coal?
    if d == up    then climb up
    if d == down  then climb down
    if d == left  then walk left
    if d == right then walk right
    if d == none  then wait 1
loop_end
```

This is the tool for **full hand-driven path control** — it gets a bot down off
the top level and all around a room while you keep control of when and how it
moves. Two things to know:

- **Follow `route` with `walk`/`climb`.** `climb up`/`climb down` handle ladders;
  `walk left`/`walk right` handle floors *and* stepping off a ladder (they align
  to the rail automatically). Feed `route`'s answer straight into them.
- **Use `go_to(exit)` for the exit.** Dropping into the open shaft needs precise
  centering that a hand-driven `walk` can't do, so let `go_to(exit)` finish the
  descent (as above). `route` to ordinary cells is fully hand-drivable.

If you don't need that much control, `go_to(cell(...))` is simpler and reaches
anything — including the exit — on its own.

### Using ladders

Bots are fully ladder-aware. You sense ladders with `on_ladder` (am I on one),
`is_ladder(c, r)` / `tile(c, r) == LADDER` (is a cell a ladder), and a ladder's
top edge reads as `is_standable`. **`go_to` and `route` climb ladders for you** —
they route up and down automatically and manage all the fiddly bits below — so
most bots never touch a ladder explicitly.

If you drive movement yourself with `climb up` / `climb down`, two rules apply
(both of which `go_to`/`route` handle internally):

- **You must be centred on the ladder's column to mount it.** `climb up` while
  standing *beside* a ladder does nothing — `walk` onto that column first. Once
  on it, `climb up` stops exactly standing on top of the run.
- **`climb down` needs a ladder under you.** Off a plain ledge it does nothing;
  use `drop` to leave a ledge without a ladder.

Stepping *off* a ladder sideways is handled for you: `walk left` / `walk right`
align to the rail automatically before stepping off, so `if on_ladder then walk
left` just works.

---

## Sensors

### Full map knowledge (fair to plan over — terrain and coal are static)

| Sensor | Returns |
|---|---|
| `coal_remaining()` | count of uncollected coal lumps |
| `nearest_coal()` | cell of the closest **reachable** coal, else `none` |
| `coal_got`, `coal_total` | quota progress |
| `exit` | cell of the exit shaft |
| `key`, `lamp` | cell of the key / lamp if this room has one and it's uncollected, else `none` |
| `tile(col, row)` | one of `EMPTY WALL PLATFORM LADDER SPIKE EXIT CRUMBLE` |
| `is_solid(c,r)`, `is_ladder(c,r)`, `is_standable(c,r)` | booleans |
| `depth` | current mine depth (level number) |

### Self

`my_col`, `my_row`, `on_ground`, `on_ladder`, `facing` (`left` or `right`),
`lives`.

### Perception — limited to `sight` (4) tiles; enemies farther away are invisible

| Sensor | Returns |
|---|---|
| `sight` | the perception range, in tiles (4) |
| `nearest_enemy()` | the closest enemy within sight (an enemy value), else `none` |
| `enemy_within(range, dir)` | true if a **mobile** enemy is within `range` tiles on side `dir` (`range` capped at `sight`). `left`/`right` mean on your row (±1); `above`/`below` mean on your column (±1) — so `enemy_within(2, below)` is "a foe on the ladder below me", not merely down-and-to-the-side; `any` ignores direction. **Crushers are ignored** (they only threaten their own column — a ladder beside one is safe); use `danger_at` for a crusher's slam |
| `danger_at(col, row)` | true if a sensed enemy's body covers that cell right now |

`dir` is one of `left right up down above below any`. An enemy value's fields:
`.col`, `.row`, `.dist`, `.dir` (which side of you it's on, `left`/`right`),
`.going` (which way it's **moving**, `left`/`right`), `.type` (`CRUSHER FOREMAN
BAT SPIDER BOULDER`), and `.state`. `.going` lets you tell an approaching foe
from a retreating one — needed to time a jump-over (see below).

`.state` is `idle`, `warning`, or `moving`. It matters most for a **crusher**:
`idle` = raised and resting (safe to dash under), `warning` = flashing, about to
slam (wait!), `moving` = slamming or retracting. So a bot can pass under a
crusher only while it's `idle` and hold otherwise:

```
e = nearest_enemy()
if e != none and e.type == CRUSHER and e.dist <= 1 and e.state != idle then
    wait 2                 # flashing or slamming — let it reset
end
```

Any always-in-motion enemy (foreman, bat, spider, boulder) reports `moving`.

### Misc

| Sensor | Returns |
|---|---|
| `cell(col, row)` | a cell value for any map position (a `go_to`/`route` target or waypoint) |
| `route(cell)` | the next-step direction toward `cell` — `left`/`right`/`up`/`down`, or `none` if you're there or it's unreachable (see [Driving movement yourself](#driving-movement-yourself-with-route)) |
| `random(n)` | a deterministic integer in `0 .. n-1` |

### Numbers and directions

Small helpers so common arithmetic reads cleanly instead of unrolling into
`if`s:

| Helper | Returns |
|---|---|
| `abs(x)` | `x` with its sign dropped |
| `distance(a, b)` | tiles between two positions, `\|a - b\|` — e.g. `distance(my_col, e.col)` |
| `lower(a, b)` / `higher(a, b)` | the smaller / larger of two numbers |
| `limit(x, lo, hi)` | `x` held inside `lo..hi` (expects `lo <= hi`) |
| `side_of(delta)` | a **direction**: `left` if `delta < 0`, `right` if `> 0`, `any` if `0` |
| `sign_of(x)` | a **number**: `-1`, `0`, or `+1` |

`side_of` is the one to reach for when steering: it turns a position difference
straight into a move, so the usual "work out a sign, then map it to left/right"
dance collapses to one line —

```
if e.dist <= 2 then walk side_of(my_col - e.col)   # step away from the enemy
```

Use `sign_of` when you want the raw number (a counter step, say), and `abs` /
`distance` for magnitudes: `if distance(my_col, e.col) == 2 then ...`.

---

## Fallback and errors

- No `--botN` for a slot ⇒ the built-in C bot plays it (the default behavior).
- A script that fails to compile is reported on stderr and that slot falls back
  to the built-in bot — the game never crashes on a bad script.
- The interpreter is sandboxed: no file or network access, bounded work per
  tick, bounded memory.

See `bots/greedy.mole` for a complete worked example.
