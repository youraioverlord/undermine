# MoleScript for Humans — teach a robot mole to dig

*A friendly guide to writing your own UNDERMINE bot, for people who have never
really programmed before. No experience needed — if you can read this, you can
do this.*

---

## 1. What is this all about?

UNDERMINE is a mining game. You run around a cave, grab every lump of coal,
and then drop down a shaft to the next level. There are ladders, gaps,
crumbling floors — and enemies that will absolutely flatten you.

Here's the fun part: **you don't have to play it yourself.** You can write a
little set of instructions — a *bot* — and the game will follow your
instructions and play for you. Your bot lives in a plain text file ending in
`.mole` (get it? mole script? for a mining game?), and the language it's
written in is called **MoleScript**.

Writing a bot feels a bit like writing instructions for a very obedient, very
literal friend who is playing the game blindfolded while you shout commands:

> "Is there coal left? Yes? Then walk toward it. Enemy nearby?! Step away!
> Okay, coal's gone — head for the exit!"

That's genuinely all a bot is. A loop of "check the situation, do one small
thing" repeated over and over, very fast.

### How do I run my bot?

Save your file (say `mybot.mole`) in the `bots` folder, then either:

- **In the game:** on the title screen, press **F2** to load a bot for the
  left screen or **F3** for the right screen. Your name will show up on the
  title screen as the bot's creator. Fame!
- **From the command line:**

```
undermine --bot1 bots/mybot.mole
```

- **Test without opening the game window at all:**

```
undermine --moletest bots/mybot.mole
```

That last one runs your bot through 20 different randomly-generated cave
layouts and tells you how many levels it cleared. Great for checking whether
your latest "improvement" actually made things worse. (It happens to
everyone. Constantly.)

---

## 2. Your very first bot

Here is a complete, working bot. Four lines:

```
# author: Your Name

loop:
    if coal_remaining() > 0 then go_to(nearest_coal()) else go_to(exit)
loop_end
```

Believe it or not, this little thing will collect coal, climb ladders, drop
off ledges, find the exit, and descend level after level. Let's take it apart
line by line.

**Line 1: `# author: Your Name`**

Every `.mole` file **must** start with this line. It says who wrote the bot —
put your own name there. Lines starting with `#` are *comments*: notes for
humans that the game ignores. This particular comment is special because it's
required and must be first, but you can sprinkle other `#` comments anywhere
to remind yourself what your code does.

**Line 2: `loop:`**

This says "everything until `loop_end` should repeat forever." Round and
round, like a hamster wheel. Games work this way under the hood too: check
the situation, act, repeat.

**Line 3: the actual brain**

```
if coal_remaining() > 0 then go_to(nearest_coal()) else go_to(exit)
```

Read it out loud, it's almost English: *"If the amount of coal remaining is
more than zero, then go toward the nearest coal — otherwise, go to the
exit."* That's the entire strategy. Grab coal until there's none, then leave.

**Line 4: `loop_end`**

Marks where the repeating part ends. Then it jumps back to `loop:` and does
it all again.

Save it, run it, and watch your creation scurry around the mine. It will
eventually die to something — probably an enemy, because we haven't taught
it fear yet. We'll fix that soon.

### The shape of every bot: look, decide, act

Before we go further, let's name the big idea, because *every* bot you will
ever write — including the fancy ones at the end of this guide — has the
same skeleton. People call it an *algorithm*, which is just a fancy word for
"a fixed recipe of steps." The recipe here is:

1. **Look.** Ask sensors what the world looks like *right now*: where's the
   coal, is anything scary nearby, am I on a ladder?
2. **Decide.** Go down a checklist, most urgent thing first: *Am I about to
   die? No? Is something chasing me? No? Okay — back to work.*
3. **Act.** Do exactly ONE small thing — a single step, a single rung, a
   short wait.
4. Go back to 1.

That's it. That's the whole algorithm, repeated many times per second. A bot
never makes a grand plan and follows it blindly; it re-reads the room and
re-decides *every single step*. That's what makes it feel alive — and it's
why a bot can be chasing coal one moment and fleeing the next: the checklist
just came up with a different answer this time around.

Two things make the checklist work:

**Order is everything.** The checks run top to bottom, and the first one
that matches wins. So put survival at the top and chores at the bottom. If
your bot checks "collect coal" *before* "is a foreman about to eat me?", it
will politely mine while being eaten. When you see a bot doing something
dumb, the order of its checks is the first place to look.

**`continue` means "stop reading the list, start over."** When a check
matches and the bot handles it (say, it steps away from an enemy), you write
`continue` — that skips everything below and jumps back to the top of the
loop for a fresh look at the world. Without it, the bot would flee AND try
to mine in the same breath. One decision per lap, always based on the
freshest information.

You now know the secret: writing a bot isn't about clever moves — it's
about putting the right checks in the right order. Everything from here on
is just adding better checks.

---

## 3. How time works (the one weird thing)

This is the only genuinely unusual idea in MoleScript, so let's get it out of
the way early.

The game moves in tiny time steps called **ticks** — 50 of them every second.
Think of ticks as frames of a movie.

Your bot's instructions fall into two groups:

**Thinking is free.** Checking sensors, doing math, `if` decisions,
`while` loops — all of that happens *instantly*, between frames. You can
think as much as you want and no game time passes. Your bot is basically The
Flash when it comes to thinking.

**Doing takes time.** The moment your bot actually *moves* — `walk`,
`climb`, `jump`, `drop`, `wait`, `idle`, `go_to` — the script pauses right
there while the little mole animates across the screen. When the move
finishes, the script wakes up at the very next line and carries on.

So when you write:

```
walk left
walk left
climb up
```

...you're saying "walk one tile left (takes a moment), then another tile left
(takes a moment), then climb one rung up (takes a moment)." The script reads
like a story, and the game acts it out in real time.

One more nice thing: movement is **one tile at a time**. `walk left` doesn't
mean "walk left forever" — it means "move exactly one tile left, then come
back to me for the next decision." This keeps your bot alert. Every single
tile, it gets a fresh chance to notice an enemy and change plans.

---

## 4. Making decisions with `if`

You've seen the one-line version:

```
if coal_remaining() > 0 then go_to(nearest_coal()) else go_to(exit)
```

One line, decision made, done. No extra ceremony needed.

When you want a decision to trigger *several* lines of action, use the
multi-line version, which ends with the word `end`:

```
if on_ladder then
    climb up
    climb up
    walk right
end
```

You can chain choices with `elif` ("else if") and finish with a catch-all
`else`:

```
if e.type == FOREMAN then
    walk side_of(my_col - e.col)     # step away from him
elif e.type == BAT then
    drop                             # duck under the flappy menace
else
    go_to(nearest_coal())            # nothing scary — business as usual
end
```

The game checks each condition from the top and runs the **first** one that's
true, skipping the rest. If none are true, the `else` part runs.

**The golden rule:** if your action is on the *same line* as `then`, you do
NOT write `end`. If you press Enter after `then` and put actions on their own
lines, you MUST close with `end`. Mixing these up is the #1 beginner error,
so when something won't compile, check this first.

Conditions can compare things (`==` equals, `!=` not equals, `<`, `<=`, `>`,
`>=`) and combine with `and`, `or`, `not`:

```
if e != none and e.dist <= 2 and e.type != CRUSHER then
    walk side_of(my_col - e.col)
end
```

One small rule: a condition has to fit on one line. If it's getting too long
to read, that's the language politely telling you to simplify.

---

## 5. Remembering things: variables

A variable is a named box you can store a value in:

```
mood = 5
mood = mood + 1        # now it's 6
```

One thing that trips people up: `=` doesn't mean "equals" like in math class
— it means "**put this into the box**." The right side gets worked out
first, THEN the result lands in the box. So `mood = mood + 1` isn't a
riddle; it reads as "take what's in `mood`, add 1, put it back." That's how
you count.

You can name a box almost anything: `counter`, `panic_level`,
`favorite_column`. Then use the name anywhere a value would go.

Here's why this matters so much: without variables, your bot is a goldfish.
Each lap of the loop it looks at the world, acts, and forgets everything —
next lap it starts from zero. Sensors can only tell it about *now*; they
can't tell it "this is the sixth time you've tried this." A variable is the
bot's memory between laps: a variable created in your main loop **keeps its
value from one lap to the next**. So you can count things over time:

```
# author: Patience Tester

boredom = 0

loop:
    if coal_remaining() > 0 then
        result = go_to(nearest_coal())
        if result == blocked then
            boredom = boredom + 1        # can't get there... again
        end
        if boredom > 5 then
            jump right                   # fine, we'll do it the hard way
            boredom = 0
        end
    else
        go_to(exit)
    end
loop_end
```

This bot tries the polite route, and if it gets blocked six times, it loses
patience and jumps. Note the little detail at the end: after jumping it sets
`boredom = 0` — it *resets its own memory*, so the next blockage starts a
fresh count instead of triggering an instant jump. Count up on failure,
reset on action (or on success): that pair shows up in almost every bot you
will ever write. Variables are how bots get personality.

---

## 6. Seeing the world: sensors

Your bot is not blind — it has *sensors*, questions it can ask about the
world at any moment, for free. There are three flavors.

### Things the bot always knows (the map)

The cave layout doesn't move, so your bot gets to know it fully — like having
the map open:

| Question | What you get back |
|---|---|
| `coal_remaining()` | how many coal lumps are left |
| `nearest_coal()` | *where* the closest reachable coal is |
| `exit` | where the exit shaft is |
| `key` | where the KEY is, or `none`. From level 10, some rooms have one — and their exit **stays locked until you grab it**. If `key != none`, go get it, or you'll mine every lump and then stand at a shut door forever |
| `lamp` | where the lamp is in a dark room, or `none` — optional, but it lights the room up (+50 points) |
| `tile(col, row)` | what's at a spot: `EMPTY`, `WALL`, `PLATFORM`, `LADDER`, `SPIKE`, `EXIT`, `CRUMBLE` |
| `is_solid(c,r)`, `is_ladder(c,r)`, `is_standable(c,r)` | quick yes/no checks about a spot |
| `depth` | which level you're on |

About positions: the cave is a grid. **Columns** count across (left to
right), **rows** count down (top to bottom). A position like "column 5, row
2" is called a **cell**. When a sensor hands you a cell, you can read its
parts with a dot: if `c = nearest_coal()`, then `c.col` is its column and
`c.row` is its row.

### Things the bot knows about itself

`my_col` and `my_row` (where am I?), `on_ground`, `on_ladder` (true/false),
`facing` (`left` or `right`), and `lives`.

### Things the bot can only see nearby (enemies!)

Here's the twist: enemies are only visible within **4 tiles** (that distance
is available as the sensor `sight`). Beyond that, your bot has no idea
they exist. It's dark in a mine! This is what makes bot-writing exciting —
your bot must react to surprises, not plan around perfect knowledge.

| Question | What you get back |
|---|---|
| `nearest_enemy()` | the closest enemy in sight, or `none` if the coast looks clear |
| `enemy_within(2, left)` | true if some moving enemy is within 2 tiles on your left |
| `danger_at(col, row)` | true if an enemy's body is covering that exact spot right now |

### The `none` thing

Some sensors answer "there isn't one." `nearest_coal()` when all coal is
collected, or `nearest_enemy()` when nothing's in sight, give back the
special value `none`. **Always check for it** before poking at the answer:

```
e = nearest_enemy()
if e != none then
    # only now is it safe to look at e.dist, e.type, etc.
end
```

Handy shortcut: in an `if`, `none` counts as "false" and pretty much
everything real counts as "true." So `if nearest_enemy() then ...` reads as
"if there's an enemy in sight..."

---

## 7. Know your monsters

When `nearest_enemy()` gives you an enemy, you can ask it questions with the
dot: `.type` (what is it?), `.dist` (how many tiles away?), `.dir` (which
side of me — `left` or `right`?), `.going` (which way is *it* headed?),
`.col` / `.row` (where exactly?), and `.state` (what's it doing?).

Meet the cast:

**FOREMAN** — a grumpy mine boss who patrols platforms and climbs ladders,
lugging coal around. Walks the floors just like you. Your most common
problem.

**BAT** — flies around in diagonal swoops. Ignores floors, ladders, and your
feelings.

**SPIDER** — hangs from the ceiling on a thread and bobs up and down,
guarding its column like a fuzzy security barrier.

**BOULDER** — a big rolling rock. It rolls along platforms, falls off edges,
and reappears to do it all again. Less a creature, more a natural disaster
on a schedule.

**CRUSHER** — a giant piston attached to the ceiling that slams straight
down and pulls back up. It never moves sideways: it only threatens the
column directly beneath it.

### The crusher's tell (and `.state`)

The crusher telegraphs its slam, and `.state` lets you read it:

- `idle` — raised and resting. **Safe to dash under.**
- `warning` — flashing. It's about to slam. **Do not walk under it.**
- `moving` — slamming down or pulling back up. Also no.

```
e = nearest_enemy()
if e != none and e.type == CRUSHER and e.dist <= 1 and e.state != idle then
    wait 2        # it's flashing or slamming — let it finish its tantrum
end
```

(Enemies that are always on the move — foreman, bat, spider, boulder —
simply report `moving` all the time.)

### Can I jump over enemies?

Barely, and only with perfect timing — this is the single most common way
bots die, so read this bit twice:

- Jumping when the enemy is **1 tile away kills you.** The jump rises too
  slowly; you smack into them on the way up. Every time. Never do it.
- You *can* clear a **ground** enemy (foreman or boulder), but only if you
  jump when it's about **2 tiles ahead and coming toward you** — check
  `.going` to make sure it's approaching. You'll land just behind it. Very
  cool when it works.
- **Bats and spiders can never be jumped over.** Your jump arc passes right
  through where they float. Just... don't.

Honestly? Jumping over enemies is a party trick. The reliable moves are:
step away (`walk side_of(my_col - e.col)` — more on `side_of` below), climb
a nearby ladder, or `wait` for the enemy to wander off.

### The ladder trick (free safety tip)

A **ladder is a completely safe spot when a crusher or spider is next to
you.** Both of those only threaten their own column, and neither can ever be
in a ladder's column. So if you're standing at a ladder and one of them is
looming one tile over — don't run. Climb. It cannot touch you. Bots that
know this survive noticeably longer than bots that panic.

**But do not overtrust the ladder.** It only shields you from those two.
A **foreman climbs ladders** and will happily follow you up one — if you
retreat up a ladder, you MUST step off at the top (`walk left` or
`walk right`) and keep moving. A bot that climbs to the top rung and stands
there feeling safe is a bot about to be caught from below. And bats simply
fly wherever they like, ladders included.

---

## 8. Getting around: `go_to`, `route`, and friends

### `go_to(place)` — the easy way

`go_to` is your chauffeur. Give it any cell and it figures out the walking,
climbing, and dropping needed to get there — one tile per call, so your loop
keeps getting a say:

```
go_to(nearest_coal())
```

How does it "figure out" the route? It's worth understanding, because then
`blocked` will never surprise you. Picture pouring water onto your bot's
square on the map: the water spreads outward one tile at a time — along
floors, up and down ladders, off ledges that are safe to drop from. Wherever
the water can flow, the bot can go. When the flood reaches your target,
`go_to` traces the flow backwards to find which single step *you* should
take first — then takes just that one step. Next call, it floods again from
your new spot. (Computer people call this a *search*; the flood picture is
genuinely how it works.)

Three useful truths fall out of that picture:

- If the flood reaches the target at all, `go_to` found a route — it never
  wanders or gets lost.
- The flood only spreads through walk, climb, and drop moves. It does NOT
  jump. A one-tile gap in the floor stops the water dead, even though a
  jump would clear it easily.
- Because it re-floods from scratch every step, it instantly adapts when
  the world changes — but it does *not* see enemies. It will happily route
  you straight through a foreman. Enemy-dodging is YOUR checklist's job.

It answers with one of three words each time you call it:

- `arrived` — you're there!
- `moving` — on the way, took a step.
- `blocked` — the flood never reached the target: every route needs a jump
  (or doesn't exist). This is your cue to take over — usually with a hop.

```
if go_to(nearest_coal()) == blocked then
    jump right        # try hopping the gap ourselves
end
```

**Special case you should memorize:** to leave a level, always use
`go_to(exit)`. Dropping into the exit shaft needs pixel-perfect centering
that manual walking can't manage. `go_to(exit)` nails it and reports
`arrived` once you actually drop through to the next level.

You can also give `go_to` a *preference* for when two routes are equally
good: `go_to(nearest_coal(), up)` says "if it's a tie, I prefer the high
road." Nice for giving two bots different personalities — they'll take
different paths through the same cave.

And you're not stuck going only where sensors point. `cell(col, row)` builds
any position you like, so you can invent your own waypoints:

```
go_to(cell(my_col, 2))      # head for row 2 (near the top), same column
```

### `route(place)` — the hands-on way

Maybe you don't want a chauffeur. Maybe you want to drive. `route` looks at
the same map as `go_to` but only tells you *which direction the next step
is* — `left`, `right`, `up`, `down`, or `none` (already there / can't reach
it). Moving is your job:

```
d = route(nearest_coal())
if d == up    then climb up
if d == down  then climb down
if d == left  then walk left
if d == right then walk right
if d == none  then wait 1
```

Why bother? Because now *you're* in the loop for every single tile — you can
check for danger before each step, take detours, dodge, hesitate
dramatically. It's more code, but it's *your* code. (Exception: even
hand-drivers should still use `go_to(exit)` for the final descent — see
above.)

### Ladders, if you drive manually

`go_to` and `route` handle ladders for you. If you insist on manual
climbing, two rules:

1. **Stand exactly on the ladder's column before `climb up`.** Standing
   *next to* a ladder and climbing does nothing (imagine climbing a ladder
   that's a meter to your left — same result). `walk` onto its column first.
2. **`climb down` needs an actual ladder below you.** At the edge of a plain
   ledge, `climb down` does nothing — that's what `drop` is for.

Getting *off* a ladder sideways is automatic: `walk left` or `walk right`
while on a ladder neatly steps you off. No rule needed.

---

## 9. Little math helpers

A few built-in helpers save you from writing fiddly `if` chains:

| Helper | What it does |
|---|---|
| `distance(a, b)` | how far apart two numbers are — `distance(my_col, e.col)` is "how many columns between me and that enemy" |
| `abs(x)` | drops a minus sign: `abs(-3)` is `3` |
| `lower(a, b)` / `higher(a, b)` | the smaller / larger of two numbers |
| `limit(x, lo, hi)` | keeps `x` inside a range |
| `side_of(delta)` | turns a difference into a **direction**: negative → `left`, positive → `right` |
| `sign_of(x)` | turns a number into `-1`, `0`, or `+1` |

The star of the show is `side_of`. Watch this one-liner:

```
if e.dist <= 2 then walk side_of(my_col - e.col)
```

Think it through: if the enemy is to my *right*, then `my_col - e.col` is
*negative*, and `side_of` says `left` — so I step left, **away** from it.
If the enemy's to my left, the math flips and I step right. One line, and
your bot always retreats in the correct direction. (Flip the subtraction to
`e.col - my_col` and you'd walk *toward* the enemy. Useful for... braver
bots.)

One more tool lives in this drawer: `random(n)` gives you a "random" whole
number from `0` up to `n - 1` — handy for breaking a tie ("left or right?
flip a coin") so your bot doesn't always pick the same side. The quotes
around "random" matter: it's fake randomness, cooked up from the level's
seed. Play the same seed again and you get the *exact same* coin flips in
the exact same order. That's deliberate — it means your bot never becomes
untestable: a bug you saw once will happen again identically, dice and all.
Random enough to feel alive, predictable enough to debug.

---

## 10. Reusable chunks: functions

When you catch yourself writing the same lines twice, wrap them in a
**function** — a named mini-program you can call whenever:

```
func flee(e):
    walk side_of(my_col - e.col)
    walk side_of(my_col - e.col)
    return 0
func_end
```

Define functions at the top of the file (after the author line, before your
main `loop:`), and call them by name: `flee(e)`. They can take inputs (the
names in parentheses) and hand back an answer with `return`.

A word on that `return`: when the function hits it, the function is done,
and whatever comes after `return` is handed back to whoever called it.
That's how you build your own yes/no questions — a function that returns
`true` or `false` can sit right inside an `if`, like
`if can_stand(my_col - 1) then walk left`. You're not just reusing lines;
you're inventing new *words* for your bot's vocabulary, then writing the
rest of the bot in those words. (When a function has nothing meaningful to
answer, returning `0` — like `flee` above — is a fine "I'm done" shrug.)

Two quirks worth knowing. First: variables inside a function are wiped every
time the function ends. If you need a value to survive across ticks, keep it
in the main loop's variables — those persist. Second, and this one surprises
everyone: **a function cannot see your main-loop variables at all.** A
function lives in its own little room — the only things inside are what you
handed it through the parentheses. Write `func check(): return danger + 1`
with `danger` defined up top, and the compiler will stop you with
`unknown name 'danger'`. That's not a bug, it's the design: anything a
function needs, you pass in as a parameter; anything it computes, it hands
back with `return`. It keeps every function honest — you can read its
parentheses and know everything it depends on.

While we're listing loop tools: `while COND:` ... `while_end` repeats while a
condition holds, `break` bails out of a loop early, and `continue` skips
straight to the next lap.

### Cheat sheet: what closes what

| This... | ...is closed by |
|---|---|
| `func name(...):` | `func_end` |
| `loop:` | `loop_end` |
| `while ...:` | `while_end` |
| multi-line `if ... then` | `end` |
| single-line `if ... then ...` | nothing! |

---

## 11. Make your bot talk: `say`

Here's the tool that will save you hours: your bot can **narrate what it's
thinking**, live, right under its feet on the screen.

```
say "going for coal"
```

Whatever your bot last said appears right beside its name in-game, and
`--moletest` prints every message change to the console. On top of that, the
game writes every message to `bot1.log` / `bot2.log` (next to the game, one
file per screen, freshly overwritten each run) — so after a disastrous run
you can read the whole diary back. `say` costs no game time at all — it's
pure commentary.

You can mix text and values with commas, and they get joined with spaces:

```
e = nearest_enemy()
if e != none then
    say "uh oh", e, "dist", e.dist     # shows: uh oh foreman(7,5) dist 2
end
```

Why this matters: when your bot does something dumb (it will), the question
is always *"what was it thinking?"* — and now it can literally tell you.
Sprinkle `say`s at every decision point while developing:

```
loop:
    if coal_remaining() > 0 then
        say "mining, left:", coal_remaining()
        go_to(nearest_coal())
    else
        say "heading home"
        go_to(exit)
    end
loop_end
```

Watch it play, read its little diary, find the moment its words and actions
stop matching — that's your bug. Delete or keep the `say`s when you're done;
they never slow the bot down.

## 12. Advanced senses (seeing everything)

The `nearest_enemy()` family only sees 4 tiles — great for reflexes, but
sometimes you want the whole picture. These sensors see **everything**, at
any distance (exactly what the built-in bots can see):

- `enemy_count()` — how many enemies are in the room, total.
- `enemy(i)` — the i-th enemy (`enemy(0)`, `enemy(1)`, ...), with all the
  usual fields, no matter how far away it is. Loop over them to plan around
  the whole room instead of reacting to surprises:

```
i = 0
while i < enemy_count():
    e = enemy(i)
    if e.type == BOULDER then say "boulder at", e
    i = i + 1
while_end
```

If that little `i` dance is new to you: the enemies are numbered `0, 1, 2,
...`, like a row of lockers. `i` is your finger pointing at one locker.
You start at `0`, look at that enemy, move your finger up by one
(`i = i + 1`), look again — and the `while` keeps you going until your
finger walks past the last locker. This "point, look, move the finger"
pattern works for going through *anything* numbered, and you'll use it
constantly.

- `plat_count()`, `plat(i)`, `plat_dir(i)` — the moving platforms: where
  each one is right now and which way it's heading. `on_platform` is true
  while you're riding one.
- `shaking(col, row)` — true when a crumbling tile has been stepped on and
  is about to give way. If the floor under you is shaking... move.
- `safe_ticks(i)` — for enemy number `i`: how many ticks it is *guaranteed*
  harmless. Only an idle (raised) crusher ever gives a number above zero —
  its remaining rest time before the next warning. Crossing under a crusher
  takes roughly 10–20 ticks, so "is `safe_ticks` comfortably bigger than
  that?" is the grown-up way to time a passage. One warning, learned the
  hard way: its raised body still kills on touch — `idle` means the timer is
  resting, not that the metal is soft.
- `score()` — your current score, if your bot wants to gloat about it.

A fun consequence: with `enemy(i)` your bot can *plan* ("the third enemy is
a boulder patrolling row 12 — I'll clear that row last"), while
`nearest_enemy()` keeps handling the "AAAH IT'S RIGHT THERE" moments.

### The mine remembers where it killed you. Do you?

- `death` — the cell where your bot last died *in this room*, or `none` if
  it hasn't (yet). Wiped when you reach the next room.
- `deaths` — how many times this room has killed you.

Here's why this matters more than it sounds: **dying resets every enemy to
its starting position.** The room rewinds; your bot doesn't. So a bot that
walks the same route at the same pace will meet the same crusher at the same
instant, every single life, until it's out of lives. `death` is how you break
the loop — life two *knows something* life one didn't.

But use it **gently**. The rookie move is "never go near that cell again" —
and then the room's only ladder happens to pass through it, and your bot
stands in a corner sulking until the air runs out. Soft touches work better:

```
nd = 0
if death != none then
    if distance(my_col, death.col) <= 2 and distance(my_row, death.row) <= 2 then nd = 1
end
```

...then use `nd` to be *more careful* near the fatal spot, not absent from
it: start fleeing one tile earlier (`e.dist <= 2 + nd`), or pause a beat at
its doorstep while a *mobile* enemy is near — a two-tick `wait`
desynchronizes you from whatever timing killed you last time. Two hard-won
warnings from our own bots: don't pause because a **crusher** is "near" (it
never leaves — you'll wait until the air runs out; pass crushers by reading
`.state` instead), and give any pause rule a **budget** that resets when you
collect coal, so respect never turns into paralysis. Respect the spot; don't
fear it. (Both example bots do exactly this — watch for their
`death spot - respect` line.)

And here's a thought to grow on: the game tells you *where* you died — but
you can teach your bot to remember **what it was doing** when it died, all
by yourself, with tools you already have. Think it through: your variables
survive death. So a bot could keep a variable that always describes its
current maneuver — "chasing coal", "hopping over a foreman", "crossing a
gap" — updating it just before each risky move. Each lap of the loop it
also compares `deaths` to a count it remembered last lap. The moment those
disagree, the bot just died — and that maneuver variable is still holding
the name of what killed it. Now it *knows*: "the hop-over got me." It can
choose not to try that trick again in this room, or to try it somewhere
else, or more carefully. There's no sensor for this on purpose — no sensor
could know your bot's *intentions*, only you do. A bot that learns from its
own mistakes is the difference between a script and something that starts
to look a little bit clever. We've left it for you to build.

## 13. Putting it all together: a real bot

Here's a bot that uses nearly everything from this guide. Read the comments —
it's less code than it looks:

```
# author: Your Name

# Step away from an enemy, unless a ladder makes us untouchable.
func evade(e):
    if e.type == CRUSHER or e.type == SPIDER then
        if on_ladder then
            idle                  # ladder = safe from these two. Just chill.
            return 0
        end
    end
    walk side_of(my_col - e.col)  # otherwise: step away from it
    return 0
func_end

patience = 0

loop:
    # --- Safety first: anything scary within 2 tiles? ---
    e = nearest_enemy()
    if e != none and e.dist <= 2 and e.type != CRUSHER then
        evade(e)
        continue                  # skip the rest, re-check the world
    end

    # --- A close crusher gets special treatment: pass only while idle ---
    if e != none and e.type == CRUSHER and e.dist <= 1 and e.state != idle then
        wait 2
        continue
    end

    # --- Work: coal, then the key if this room has one, then the exit ---
    if coal_remaining() > 0 then
        if go_to(nearest_coal()) == blocked then
            patience = patience + 1
            if patience > 5 then
                jump right        # stuck too long — try hopping the gap
                patience = 0
            end
        else
            patience = 0
        end
    elif key != none then
        go_to(key)                # locked exit until we hold the key!
    else
        go_to(exit)
    end
loop_end
```

Now let's replay one lap of its loop in slow motion, because *why the pieces
sit where they sit* is the actual lesson:

1. **`e = nearest_enemy()`** — look first. Every lap starts by asking what
   the world looks like right now. Never act on last lap's information.
2. **The danger check comes before everything else** — if something mobile
   is within 2 tiles, evade and `continue`. That `continue` is crucial: a
   fleeing bot must not ALSO try to mine this lap. Handle the emergency,
   then come back with fresh eyes.
3. **The crusher gets its own rule, after the general one.** Why separate?
   Because the right response is different: you don't *run from* a crusher
   (it can't chase you) — you *wait out* its slam and stroll under it while
   it's resting. One enemy, one tailored answer. When two rules could both
   match, the more specific or more urgent one goes higher up.
4. **Work comes last** — and it's a pecking order, not a single goal:
   coal while there is any, then the key if this room has one, then the
   exit. The bot doesn't "switch modes"; the same checklist just naturally
   falls through to the next goal when the earlier one is finished.
5. **`patience` is the bot's memory.** Notice what it really does: `go_to`
   saying `blocked` once might be a fluke, so the bot doesn't panic-jump
   immediately — it counts. Five blocked answers in a row means "this is
   really a gap", THEN it hops, and resets the counter. Counting before
   acting is how you tell a one-off from a pattern, and it's the single most
   reusable trick in bot-writing.

The shape of this bot — *look, handle danger, handle special cases, then do
the job* — is the shape of almost every good bot. Start here and make it
yours: tweak the flee distance, add a taste for the high road with a `go_to`
bias, make it braver, make it cowardlier. Then run:

```
undermine --moletest bots/mybot.mole
```

...and see if your version out-mines the bots in the `bots/` folder. That's
the game behind the game.

---

## 14. When things go wrong (they will)

**"My script won't load / the game says load failed."** There's a typo. The
error message points near the problem. The all-time classics:

1. Forgot `# author: ...` as the very first line.
2. Put `end` after a one-line `if` (one-liners take no `end`).
3. Forgot `end` after a multi-line `if`.
4. Closed a loop with `end` instead of `loop_end` or `while_end`.

Don't worry about breaking anything, by the way — a script that won't compile
just means the built-in bot plays instead. The game never crashes because of
your `.mole` file. Experiment freely.

**"My bot walks into enemies like they're not there."** It probably makes a
plan and never re-checks it. Make sure the danger check runs *every* lap of
the loop, before the work — and remember enemies are invisible beyond 4
tiles, so a bot sprinting at full speed has little time to react. Checking
often is the whole trick.

**"My bot keeps jumping into things and dying."** Re-read the jump-over
rules (§7). Two tiles, coming toward you, ground enemies only. When in
doubt, don't jump — step away or wait.

**"My bot stands at the bottom of a ladder doing nothing."** It's probably
one column off, hammering `climb up` beside the ladder (rule 1 of manual
climbing). Or just let `go_to` handle ladders — that's what it's for.

**"My bot reached the exit but won't go down."** Use `go_to(exit)`, not
manual walking. The shaft needs centering that only `go_to` can do.

**"It works on one level and fails on another."** Welcome to the club — the
caves are randomly generated, so every level is a new pop quiz. This is why
`--moletest` runs 20 different seeds. A good bot isn't one that aces one
cave; it's one that survives twenty.

**Three drill-sergeant tests** for the classic killers, runnable headlessly:

```
undermine --probelad bots/mybot.mole    # a foreman chases you up a ladder
                                        # into a corner - do you survive?
undermine --probegap bots/mybot.mole    # the only coal is across a one-tile
                                        # gap - do you figure out the hop?
undermine --probekey bots/mybot.mole    # a real key room - do you fetch the
                                        # key, or mine coal at a locked door?
```

Each prints a tick-by-tick trace (with your bot's `say` lines) and ends with
a verdict. Variants: `--probelad ... edge` (the ladder sits at a platform
edge), `--probelad ... mid` (you start halfway down with the foreman coming
up), `--probegap ... crusher` (a crusher looms over the landing — the correct
answer is to refuse the jump), and `--probekey ... N` picks the Nth key room.
You can also pit the built-in bots through any of them with `builtin1` /
`builtin2` in place of the file. All of these scenarios have killed every
first draft ever written — including ours.

One last reassuring fact: the game is fully **deterministic** — the same bot
on the same level always does exactly the same thing. If something weird
happens, it will happen again identically, and you can watch it as many
times as you need to figure out why. The bug can run, but it can't hide.

---

## 15. Where to go next

- Read `bots/greedy.mole` and `bots/cautious.mole` — real, complete bots you
  can learn from (and beat).
- Skim `MOLESCRIPT.md` — the terse technical reference, handy once you know
  the basics and just need to check a detail. Note it's deliberately kept
  free of tactical advice: it's also the exam paper we hand to AI models to
  see what they can figure out on their own.
- Open `cheat-sheet.md` — the tactical playbook, every hard-won survival
  trick our own bots died to learn. **Humans only**: AI bot-writers are on
  their honor not to peek, so enjoy your unfair advantage. It's the fastest
  way from "my bot keeps dying" to "my bot embarrasses the machines."
- Load your bot with **F2** on the title screen and watch it play the left
  screen against the built-in bot on the right.

Now go on. Your mole isn't going to program itself.
