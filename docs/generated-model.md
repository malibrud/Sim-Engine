# What a generated model does

This document describes the behaviour of a model after `sec --emit` has turned
it into C++: how its settings and initial conditions reach their final values,
what is called on every tick and in what order, what each call can see, and how
a run ends.

It is a description, not the definition. [SPECIFICATION.md](../SPECIFICATION.md)
defines the language and argues for each decision. Where the two disagree, the
specification is right and this document has a bug.

The examples use `examples/drivetrain/`, whose generated files are `Drivetrain.hpp`
(the public interface), `Drivetrain.cpp` (everything else) and `main.cpp` (an
example host).

## Contents

1. [The life of a run](#1-the-life-of-a-run)
2. [How settings are finalized](#2-how-settings-are-finalized)
3. [How initial conditions are finalized](#3-how-initial-conditions-are-finalized)
4. [What is recorded and logged](#4-what-is-recorded-and-logged)
5. [Starting the run](#5-starting-the-run)
6. [The tick](#6-the-tick)
7. [The end of a run](#7-the-end-of-a-run)
8. [Where the data lives](#8-where-the-data-lives)
9. [From C and other languages](#9-from-c-and-other-languages)

## 1. The life of a run

A host makes four kinds of call, in this order.

| Phase | Host calls | What happens |
|---|---|---|
| Configure | `Sim()`, `settings()`, `load_settings()`, `load_settings_file()`, `dump_settings()`, `list_signals()`, `get()` / `set()`, the channel, log and inputs-file calls | Settings, initial-condition overrides, the run's length, the recorded channels, the log and the inputs file are collected. No node code runs. |
| Start | `init()` | Settings are finalized, initial conditions are written, and every node's `init()` runs. |
| Step | `inputs()`, `tick()`, `outputs()`, `done()`, `sync_realtime()`, `read()`, `log_next()` | One `tick()` samples the model at the current time and advances it one step. It never waits; `sync_realtime()` is how a host keeps pace with wall time. |
| Finish | `finish()` | Every node's `final()` runs, the recording and log close, and the exit code is returned. |

```cpp
drivetrain::Sim sim;
sim.settings().wheel_inertia = 1.1;       // configure
if (!sim.init()) return sim.finish();     // start
while (!sim.done()) {                     // step
    sim.inputs().axle_torque = 250.0;
    sim.tick();
    double cut = sim.outputs().torque_cut;
}
return sim.finish();                      // finish
```

`finish()` is owed even when `init()` returns false.

**Pacing is the host's.** `tick()` runs as fast as it can. To keep pace with wall
time, tick as many times as you like and then call `sync_realtime()`:

```cpp
while (!sim.done()) {
    for (int k = 0; k < 10 && !sim.done(); ++k) sim.tick();   // 10 ms of model time
    sim.sync_realtime();                                       // wait for the wall
}
```

It waits until the wall time since the first `tick()` equals `time()`. If the wall
is already past that, it returns at once, and the ticks run flat out until the
model has caught up. Nothing is skipped and there is no limit on how far behind it
may fall. `realtime_stats()` reports how well the run is keeping pace: utilization,
overrun rate, the current lag, and the whole-run frame and overrun counts. The
generated `main.cpp` paces only when given `--realtime SYNC`.

A `Sim` runs once. A second `init()` on it returns false, and `error()` says so.
To run again, construct a new `Sim`. Every piece of run state (the clock, the stop
flag, contract failures, the pacing, the log) belongs to one `Sim`, so
runs one after another start clean, and several `Sim`s in one process are
independent.

## 2. How settings are finalized

### The layers

A setting's value is decided in layers, and the last writer wins. The first four
are seen by `sec` and decide what the generated code starts with. The last two
happen in the compiled program.

| # | Layer | Where it is written | When it applies |
|---|---|---|---|
| 1 | Definition default | `settings { mass: double(kg) = 1500; }` | Compile time |
| 2 | Flow-down from the parent | `node whl : Wheel { inertia = param.wheel_inertia; };` | Compile time, re-evaluated at run time |
| 3 | The sim file's `root` block | `root: drivetrain.Corner { wheel_inertia = 0.9 (kg*m^2); };` | Compile time |
| 4 | A `.settings` file given to `sec` | `whl.radius = 0.315 (m);` | Compile time |
| 5 | `sim.settings()` | `sim.settings().wheel_inertia = 1.1;` | Run time, root settings only |
| 6 | `sim.load_settings()` or `sim.set()`, or `--settings` and `--set` on the generated executable | `front.left.mass = 340 (kg);` | Run time, any depth |

Layers 5 and 6 are a single layer in time: whichever the host does later wins.

### Derived values are recomputed, not folded

An expression that reads `param.*` is emitted as code and runs in
`resolve_settings()`, a generated function with one assignment per setting, in
dependency order, each guarded by a pin:

```cpp
void Sim::Impl::resolve_settings() {
    if (!se_pin_[0]) se_cfg_root.total_mass = 1500.0;
    if (!se_pin_[1]) front.param.mass       = se_cfg_root.total_mass / 2.0;
}
```

So overriding `total_mass` at run time moves everything derived from it.

**A run-time override pins its target.** `resolve_settings()` then leaves that
setting alone, and everything derived from it follows the pinned value. An edit
through `settings()` pins only if it changes the value.

**A deep override detaches from the flow-down.** After `front.mass = 340 (kg);`,
that value no longer follows `total_mass`. The same is true at compile time: if
layer 4 sets a value that a flow-down would have computed, the generated code holds
a constant there, and overriding the parent at run time no longer reaches it.

`sample_rate` and `time_step` are compile-time constants derived from the sim file's
`step`.

### What is fixed, and when

Settings are finalized inside `sim.init()`, before any node's `init()` runs:

1. The host's edits through `settings()` are collected and pinned.
2. `resolve_settings()` runs, so every derived value is current.
3. `settings()` is refreshed, so it shows what the model will run with.

After that the settings are constant. `load_settings()` is refused once `init()`
has been called. A node's `init()` reads final values and cannot change them.

`load_settings()` itself ends with `resolve_settings()`, so `settings()` and
`dump_settings()` are up to date between loads. `dump_settings()` shows exactly
what `init()` would run with, in a form `load_settings()` reads back.

The same paths are reachable one at a time. `settings_list()` returns every
settable path with its kind, canonical unit, type and current value. `get(path)`
reads one in the canonical unit, and `set(path, value)` writes one, either as a
number in the canonical unit or as text a source line would hold
(`"340 (kg)"`, `"CONNECTED"`). Each `set()` goes through the same reader as a
source, so its checks and wording are the same, but a refused `set()` changes
nothing and does not stop the run.

The run's length is one of these paths: `sim.duration`, a whole number of base
steps. `tick_count()` is that length in ticks. The step itself is fixed, because
every rate in the model is a count of it.

### What can go wrong

A load is all or nothing: every binding is parsed and checked against its target's
unit and type before any is applied. Values are converted to the target's unit, so
`0.34 (t)` at a `kg` setting is 340.

A refused load returns false, `error()` says which line and why, and **the refusal
stands**: `init()` will return false without running any node, so a host that
ignores the return value cannot run a configuration it did not ask for.

In a model with array-shaped states, configuration also computes the array sizes
from the settings and then needs the host's state buffers. See
[§6.2b](../SPECIFICATION.md#62b-configuration) and
[§15.5a](../SPECIFICATION.md#155a-the-state-buffers-are-the-hosts).

## 3. How initial conditions are finalized

Initial conditions use the same channel as settings, addressed by path:

```
front.left.velocity = 2 (m/s);     // a state's initial value
bw.w1[3]            = 0 (-);       // one element of an array state
dev.link            = CONNECTED;   // a finite state, by enumerator
```

A state's starting value is decided in this order:

| # | Layer | Written by |
|---|---|---|
| 1 | Declared default | `states { continuous omega: double(rad/s) = 0.0; }` |
| 2 | Override | The sim file, a `.settings` file, or `load_settings()` |
| 3 | The node's `init()` | `state.omega = equilibrium();` |

The consequence of that order: **every override is in place before any `init()`
runs.** An `init()` may do real work, such as opening a socket or solving for an
equilibrium, and it does that work once, against the configuration the run will
actually use. It reads an overridden initial condition as the value it starts
from, and it may refine it.

A state that `init()` writes regardless of its current value is computed rather
than configured. An override of it is accepted and then replaced. A node that wants
that quantity to be tunable exposes the input to the computation as a setting, as
`se.sig.src.Prbs` computes `reg` from `seed`.

`var` members are not initial conditions. They have no override channel, start at
zero, and are normally computed in `init()`.

## 4. What is recorded and logged

The sim file's `record:` and `log:` entries are only where a run starts. Every path
the sim file could record is a channel in the compiled model: each leaf's outputs,
states and vars, each port at every level, and the `sim.*` metrics. A run chooses
among them without regenerating, through the same settings sources:

```
record {
    clear;                     // start from scratch
    file  = "heavy.csv";
    every = 10;
    whl.state.omega;
    tc.out.torque_cut;
}

log { file = "heavy.log"; level = warn; levels { tc = debug; } }
```

Each line is an edit, and sources apply in order, so one file can set a baseline of
channels and a later one can drop a few with `-path;`. The same edits are calls on
`Sim`: `record()`, `unrecord()`, `record_clear()`, `record_file()`,
`record_every()`, `log_file()` and `log_level()`. `channels_list()` returns every
channel and whether it is selected.

Two outputs read back as settings sources:

- `dump_settings()` (`--dump-settings`) is the recipe for the run. Its `record`
  section starts with `clear;`, so loading it reproduces the selection exactly.
- `list_signals()` (`--list-signals`) is the menu: every channel, the unselected
  ones commented out, to be edited and kept as a channel file.

A recorded element of an array state is checked against its extent when the
source loads and when the run starts. The table and the log file open in
`init()`, after configuration, so two copies of one model given a file each write
two of each. A table is written only if at least one channel is selected.

A file name may hold `{time}`, as in `file = "heavy-{time}.csv";`.
- `init()` replaces it with the date and time the run started,
  `heavy-20261112-143005.csv`. The table and the log share that one instant.
- If the name is already taken, `init()` adds `-2`, `-3`, and so on. So two
  copies given the same pattern still write two files.
- The dump keeps the pattern.
- After `init()`, `record_file()` and `log_file()` return the name actually
  opened.

Two calls watch a run without changing it, and work before or after `init()`:
- `channel_handle(path)` resolves any channel once, selected or not.
  `read(handle)` then returns its value as the last `tick()` sampled it: the
  instant `outputs()` describes, and the value that tick's row records.
  `tick()` stores the value where it records, because by the time it returns
  the states have moved on.
- `log_capture(true)` keeps the lines the console would have printed in a
  queue, and `log_next()` takes them one at a time. This is for a host with
  no console, such as a DLL inside another program. The log file is not
  affected.

### Inputs from a file

The root inputs can come from a CSV table instead of from the host
([§13.8](../SPECIFICATION.md#138-driving-the-root-inputs-from-a-file)):

```
inputs { file = "drive.csv"; }
```

```
time (s), axle_torque [N*m], ground_speed [km/h]
0,        0,                 0
2,        250,               36
4,        400,               90
```

- **Columns are matched by name**, records flattened as `ws.speed`. The first
  column is the time. Header units are converted, and a missing one is refused
  for an input with a dimension. A column that is not a root input is ignored,
  and one info line names it, so a table the run recorded drives a run as it
  stands.
- **A double is interpolated**, and an int, a bool or a finite input holds the
  last row. Two rows at one time make a step: the later row applies from that
  time.
- **The table's columns win.** Each `tick()` writes them over whatever the
  host wrote to those inputs. An input the table doesn't name stays the
  host's, and one info line at `init()` says which is which.
- **The table must start by t = 0, and its end ends the run** if it comes
  before `sim.duration`. `tick_count()` reports the shorter length once
  `init()` has read the table, and the run ends with reason `completed`.
- **A relative name** in a file loaded with `load_settings_file()` (as
  `--settings` loads them) is found beside that file. From anywhere else it
  is relative to the working directory. The dump writes the name as joined.

`inputs_file()` sets and reads the name one call at a time. `--inputs FILE` on
the generated executable is shorthand for the section.

## 5. Starting the run

`sim.init()` does the following, in order.

1. Configures logging, as configuration left it ([section 4](#4-what-is-recorded-and-logged)).
2. Finalizes the settings ([section 2](#what-is-fixed-and-when)).
3. Reads the inputs file, if one is named ([section 4](#inputs-from-a-file)).
4. If a configuration failure stands, stops here. No node runs.
5. Writes every state's initial value (layers 1 and 2 above).
6. Opens the recording, if any channel is selected.
7. Calls each node's `init()` in elaboration order, stopping if one halts the run.

`init()` returns false if a load was refused, the inputs file could not drive
the model, or a node halted the run. `error()` says why.

No output has been computed yet. The first values appear during the first
`tick()`.

## 6. The tick

### The sequence

One call to `sim.tick()` at index *k*, with *t*[k] = *k* · `step`:

| # | Step | Called | Runs |
|---|---|---|---|
| 1 | Set the time, and write an inputs file's columns over `inputs()` | | |
| 2 | Propagate outputs | `output()` | Every node, in topological order |
| 3 | Jump map and per-tick work | `next()` | Every node that defines it, in any order, unless the run has already halted |
| 4 | Record | | If this is a recording tick |
| 5 | Exit | | If the run has halted, or *k* = `tick_count()` |
| 6 | Commit | | `dis = nxt`, `fin = fnx`, `x = xnx`, all at once |
| 7 | Flow | `output()` and `derivative()` | Solver stages, below |

The first `tick()` also starts the clock that `sync_realtime()` measures from.
Nothing in the tick waits on wall time.

The host writes `inputs()` before calling `tick()`, and an inputs file's columns
are written over the inputs they name in step 1. `outputs()` afterwards hold
y[k], the values computed in step 2. Steps 2 to 4 all see the state as the tick
found it, and every state moves below the exit. That gives two guarantees:

- **A recorded row is one consistent sample at *t*[k].** A state that jumps on this
  tick still shows its old value on this row. The new value appears on the next row,
  beside the signals it produced.
- **A stopped run is left holding a valid checkpoint**, because it never reaches
  the commit.

The `tick_count()` tick samples the final instant and does not advance, so a full
run is `tick_count()` + 1 calls to `tick()`.

### The solver stages

Step 7 runs the global solver. A stage sets the time to its own instant, writes its
trial state into `x`, re-propagates outputs in topological order, then calls every
`derivative()` in any order.

| Solver | Stages, at |
|---|---|
| `euler` | *t* |
| `rk2` | *t*, *t* + *h*/2 |
| `rk4` | *t*, *t* + *h*/2, *t* + *h*/2, *t* + *h* |

**A node with a `discrete` state is held.** It is left out of the stage output
passes, so its published output stays at y[k] across the whole step. Every other
node is re-evaluated at every stage, so it sees trial states and the stage's time.

### What runs, by kind of node

| In one tick | Algebraic | Continuous | Discrete | Finite |
|---|---|---|---|---|
| `output()` in step 2 | yes | yes | yes | yes |
| `next()` in step 3 | if defined | if defined | yes | yes |
| Committed in step 6 | nothing | `x`, if `next()` wrote it | `dis` | `fin` |
| `output()` at each stage | yes | yes | **no** | yes |
| `derivative()` at each stage | no | yes | no | no |

A node may mix kinds. It is held if it has any discrete state, and `derivative()`
still runs at every stage if it has any continuous state.

With `rk4`, one tick calls an unheld node's `output()` 5 times and a held node's
once. It calls `derivative()` 4 times and `next()` once.

### What each call can see

| | `state` | Inputs | `var` | `sim.time()` |
|---|---|---|---|---|
| `output()`, step 2 | Entry: x[k], dis[k], fin[k] | y[k] from its producers, which have already run | As the last `next()` left it | *t*[k] |
| `next()`, step 3 | Entry: x[k], dis[k], fin[k] | y[k] | Current, including its own writes above | *t*[k] |
| `output()`, stage | Trial x, dis[k+1], fin[k+1] | This stage's values, or y[k] from a held producer | As this tick's `next()` left it | Stage instant |
| `derivative()`, stage | Trial x, dis[k+1], fin[k+1] | The same | As this tick's `next()` left it | Stage instant |

A node sees only the inputs its method lists, as in `output(cmd)`. Reading any
other input is a C++ compile error.

Note the third and fourth rows. Stages run after the commit, so a body running at
a stage reads the **new** discrete and finite values. That is intended for a finite
state, which is in force from just after *t*[k]. It is why a discrete node has to be
held: without the hold, it would publish its next value early.

What each call may change:

| Method | May write | May also |
|---|---|---|
| `init()` | `state`, `var` | Touch natives, log, halt |
| `output()` | `out` | `log.trace`, `sim.check_*` |
| `derivative()` | `der` | `log.trace`, `sim.check_*` |
| `next()` | `next`, `var` | Touch natives, log, halt, `sim.check_*` |
| `final(ctx)` | Nothing the model can see | Touch natives, log |

`output()` and `derivative()` run at trial states the trajectory never visits, which
is why they can have no side effects. `next()` runs once per tick, on the trajectory.

### `next.` and `var` land at different times

A `next.<state>` write goes to a buffer. Every body reads the old value through
`state.`, whatever order the writes are in, and a state the body does not write
keeps its value. All buffers land together at step 6, across the whole model.

A `var` write lands immediately. A later line of the same body reads the new value.

### Ordering and data dependence

- **`output()` order is computed.** A wire from one node's output to an input listed
  in another node's `output(...)` is an edge in a topological sort. With output
  groups, each group is its own vertex. A cycle of these edges is a compile error.
- **A node whose `output()` lists no inputs is a sort root.** It breaks loops.
- **`next()` and `derivative()` lists constrain nothing**, because every output they
  read has already been computed. Their relative order cannot change a result.
- **A wire is the producer's output storage**, not a copy. An output a body does not
  write keeps its value from the last time it was written.

### Latency, worked through

A registered controller publishes its state and updates it from its input:

```
states { discrete cmd: double(-) = 0.0; }
next(err) { next.cmd = param.kp * in.err; }
output()  { out.cmd = state.cmd; }
```

| Tick | Step 2 publishes | Step 3 computes | Step 6 commits |
|---|---|---|---|
| *k* | cmd[k] | K · err[k] | cmd[k+1] = K · err[k] |
| *k* + 1 | K · err[k] | K · err[k+1] | |

That is one tick of latency. An `output(err) { out.cmd = param.kp * in.err; }` has
none, but it joins the sort and can form an algebraic loop.

Anything slower than the tick is written against the tick counter:

```
next() { if (sim.tick() % var.poll_ticks == 0) poll(); }
```

## 7. The end of a run

A run ends in one of these ways, and `tick()` then sets `done()`:

| Cause | `ctx.reason` |
|---|---|
| The `tick_count()` tick: the end of `sim.duration`, or of the inputs file if it ends first | `completed` |
| `sim.stop(...)` | `stopped` |
| `sim.abort(...)` | `aborted` |
| A failed `sim.require(...)`, or a configuration failure | `error` |

A halt in `next()` takes effect on the same tick. Every other node's `next()` still
runs, the row is still recorded, the exit is taken, and nothing commits.

`sim.finish()` then:

1. Calls `final(ctx)` in reverse init order, only on nodes whose `init()` completed.
2. Closes the recording.
3. Prints the real-time summary, if the host called `sync_realtime()`.
4. Prints the contract summary.
5. Writes the closing log line.

It returns 0 for a clean run. It returns 1 if the run was aborted or failed, or if a
`sim.check_*` contract was broken at any point. A broken check does not stop a run.

## 8. Where the data lives

Behind the public header, the model is a small number of flat blocks.

| Block | Holds | Notes |
|---|---|---|
| `x` | Continuous states | The solver's vector |
| `xd` | Their derivatives | Scratch for each stage |
| `sig` | Every wire value and the root inputs | One slot per producer's output |
| `dis` | Discrete states | |
| `fin` | Finite states | |
| `nxt`, `fnx`, `xnx` | Buffers for `dis`, `fin`, `x` | Seeded before `next()` runs, copied back at the commit. `xnx` exists only if some `next()` sits beside a continuous state. |

Settings live in each node's `param` member, and each `var` in its node.

`x` + `sig` + `dis` + `fin` is a complete checkpoint of the model. The buffers are
not part of it, because they are reseeded on every tick.

Each slot's path, offset, type and unit are listed in generated tables, and
`<model>.units.txt` carries the same information for tools. See
[§15.5](../SPECIFICATION.md#155-the-three-blocks) and
[§15.6](../SPECIFICATION.md#156-the-unit-manifest).

## 9. From C and other languages

Everything above is also reachable from C, as plain functions declared in
`se_sim.h` ([§14.2](../SPECIFICATION.md#142-the-c-interface)). Each `se_` call
is the `Sim` member of the same name, and runs at the same point in a run.
`build.ps1 dynamic` builds a DLL that exports these functions and nothing else.

- **The same names in every model.** `se_sim.h` is the same text for every
  model. A wrapper written once, in C#, Python or LabVIEW, can load any model's
  DLL and ask it for its name, its step and its ports.
- **The root's ports are numbered doubles.** The inputs and outputs are listed
  in declaration order, with records flattened (`ws.speed`). `se_set_inputs`
  and `se_get_outputs` move all of them in one call each, so a paced host
  crosses into the DLL three times per tick. Root settings are paths, like
  every other setting, set through `se_set`.
- **Only numbers and strings cross.** A list is a count, then one call per
  field. Every string belongs to the instance. No C++ exception reaches the
  caller.
- **One error for both sides.** `se_error` reports a refusal made by the C
  layer, such as a bad port index, as well as the model's own.
- **Files by name.** `se_load_settings_file` reads a settings file and finds
  the inputs file it names beside it. `se_inputs_file` names the table that
  drives the root inputs, so a host can replay a recorded run without writing
  a single input itself.

## Further reading

| Topic | Specification |
|---|---|
| Configuration | [§6.2b](../SPECIFICATION.md#62b-configuration) |
| The three kinds of state | [§6.4](../SPECIFICATION.md#64-states) |
| The five methods and their rules | [§8.1](../SPECIFICATION.md#81-the-five-methods) |
| The sort | [§8.5](../SPECIFICATION.md#85-the-sort) |
| Holding | [§9.3](../SPECIFICATION.md#93-discrete-outputs) |
| The tick | [§9.6](../SPECIFICATION.md#96-the-tick) |
| Settings layers and initial conditions | [§13.3](../SPECIFICATION.md#133-settings-inflow), [§13.4](../SPECIFICATION.md#134-initial-conditions) |
| Loading settings into a compiled model | [§14.1](../SPECIFICATION.md#141-the-third-carrier-a-compiled-model) |
| Inputs from a file | [§13.8](../SPECIFICATION.md#138-driving-the-root-inputs-from-a-file) |
| The C interface | [§14.2](../SPECIFICATION.md#142-the-c-interface) |
