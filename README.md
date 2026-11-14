# sim-engine

> **🚧 Work in progress.** This is an active, single-author design project, not a
> finished tool. The language, the compiler and the shape of the generated code
> are all still changing. Expect breaking changes between commits, rough edges,
> and gaps that are documented rather than fixed. Nothing here is released or
> versioned yet. See [Status](#status) before depending on anything.

A node-based simulation engine. You describe a model in a small plain-text
language: nodes with typed, unit-checked ports, wired together into a hierarchy.
A compiler, `sec`, lowers it ahead of time to dependency-free C++20. A node
definition becomes a class, an instance becomes an object, and the wiring
diagram becomes a fixed calling order. The result builds into an executable or
a static library and can run in real time.

The inspiration is block-diagram tools such as Simulink, LabVIEW and
SimCreator, but with the model kept as text you can diff, review and generate,
and the node code written directly in C++.

"sim-engine" is a working name. The language does not have a final name yet.

---

## Contents

- [Status](#status)
- [A taste of the language](#a-taste-of-the-language)
- [Getting started](#getting-started)
- [What `sec --emit` writes](#what-sec---emit-writes)
- [Using a generated model from your own program](#using-a-generated-model-from-your-own-program)
- [Building a generated model](#building-a-generated-model)
- [The examples](#the-examples)
- [The standard library](#the-standard-library)
- [Testing](#testing)
- [Repository layout](#repository-layout)
- [Design invariants](#design-invariants)
- [Roadmap](#roadmap)
- [License](#license)

---

## Status

**Working today**

- `sec` implements all six stages of the pipeline in
  [SPECIFICATION.md](SPECIFICATION.md) §1.3: lexical, syntactic, resolution,
  elaboration, scheduling and emission. Given a `.sim` file it writes C++20 that
  compiles warning-free at `/W4` and runs.
- The language has units, records, composites, flow-down settings, continuous
  and discrete states, array states, CSV recording, logging, contracts, and
  node-level unit tests. A generated model keeps pace with wall time when its
  host asks it to.
- A standard library of about 60 nodes: math, kinematics, filters and signal
  sources.
- Two worked examples: a small drivetrain, and a classical motion-cueing
  washout validated against published reference runs.
- The generated code has a small public C++ header, an example host, and a
  build script with `run` / `exe` / `lib` / `test` / `clean` / `all` modes.
- A test suite of 183 cases, run by one script.

**Not there yet, or known to be rough**

- **The specification is a draft** (version 0.1).
  [Appendix C](SPECIFICATION.md#appendix-c--known-gaps-and-open-issues) lists
  the gaps and open questions deliberately left in it.
- **The generated interface is still being built out**, in the phases listed
  under [Roadmap](#roadmap). The next one packages a model as an FMU.
- **Windows and MSVC are the only toolchain actually exercised.** `build.sh` is
  generated for GCC/Clang but has not been run on Linux or macOS.
- **`examples/drivetrain/` holds stale files.** `Sim.generated.cpp`,
  `Sim.units.txt` and `state_ref.hpp` there are hand-written illustrations from
  before the generator existed, and they no longer match its output. Run
  `sec --emit` to see the real thing.

---

## A taste of the language

A **leaf** node carries C++ bodies. Everything around them is declarative:
settings, ports, states and vars, each with a unit.

```
package drivetrain;

node Wheel {
    settings {
        inertia: double(kg*m^2) = 0.9;
        radius:  double(m)      = 0.31;
    }
    inputs {
        drive_torque: double(N*m);
        ground_speed: double(m/s);
    }
    outputs {
        ws: WheelState;                      // a record, on one wire
    }
    states {
        continuous omega: double(rad/s) = 0.0;
    }
    vars {
        inertia_inv: double(1/(kg*m^2));
    }

    init() {
        var.inertia_inv = 1.0 / param.inertia;
    }

    // Feedthrough is declared by the signature: this method reads drive_torque.
    derivative(drive_torque) {
        der.omega = in.drive_torque * var.inertia_inv;   // der unit derived: rad/s^2
    }

    output(drive_torque, ground_speed) {
        out.ws.speed  = state.omega;
        out.ws.torque = in.drive_torque;
        out.ws.slip   = (state.omega * param.radius - in.ground_speed) / in.ground_speed;
    }
}
```

A **composite** node holds no code, only instances and wires:

```
node Corner {
    settings { wheel_inertia: double(kg*m^2) = 0.9; }
    inputs   { axle_torque: double(N*m); ground_speed: double(m/s); }
    outputs  { ws: WheelState; torque_cut: double(N*m); }

    structure {
        node whl : Wheel { inertia = param.wheel_inertia; };   // settings flow down
        node tc  : TractionController {};

        self.axle_torque  --> whl.drive_torque;
        self.ground_speed --> whl.ground_speed;
        whl --> tc, self.ws;                                   // fan-out
        tc  --> self.torque_cut;
    }
}
```

A **sim file** is the experiment: which node is the root, the step and solver,
how long to run, what to record. It lives apart from the model, so one model can
be run many ways without editing it.

```
sim Drivetrain {
    root:     drivetrain.Corner { wheel_inertia = 0.9 (kg*m^2); };
    step:     1 (ms);
    solver:   rk4;
    duration: 5 (s);

    record: {
        file  = "drivetrain.csv";
        every = 10;
        whl.state.omega;
        whl.out.ws.slip;
        tc.out.torque_cut;
    }
}
```

The key ideas, each specified in [SPECIFICATION.md](SPECIFICATION.md):

- **Units are checked, then erased.** Every declarative site — port, wire,
  setting, state, record field — is dimensionally checked, and conversions
  between compatible units are folded at compile time. The generated C++ is
  plain `double`, and a unit manifest records what was erased.
- **One global fixed step and one global solver** (`euler`, `rk2`, `rk4`).
  Every node runs every tick. Discrete outputs are held across the step.
- **Purity is enforced by the C++ compiler.** `output()` and `derivative()` are
  `const` and see only what they may touch. A body that writes a state or
  calls `sim.stop()` where it may not is a compile error, reported at the
  `.se` line.
- **Composites leave no runtime trace.** The hierarchy is flattened. A wire is
  not a variable: the consumer reads the producer's output directly.

---

## Getting started

**Requirements:** Windows with Visual Studio 2022 or later and the *Desktop
development with C++* workload. AddressSanitizer is an optional component,
needed only for sanitized builds. There are no other dependencies.

Commands are for PowerShell, from the repository root.

**1. Build the compiler.** `build.bat` needs `cl` on PATH, so run it from a
*Developer PowerShell for VS*:

```powershell
.\build.bat
```

That produces `build\sec.exe`.

**2. Generate and run the drivetrain example.** The generated `build.ps1` finds
Visual Studio by itself, so this step works from any PowerShell:

```powershell
.\build\sec.exe --emit -I examples -o out\drivetrain examples\drivetrain\Drivetrain.sim
```

```powershell
.\out\drivetrain\build.ps1 run
```

The run is a batch run and finishes at once. To watch the same run keep pace
with the wall clock, waiting every 10 ms of model time, pass `--realtime`
through to it:

```powershell
.\out\drivetrain\build.ps1 run --realtime "10 (ms)"
```

That takes 5 seconds. Either way it writes `drivetrain.csv` and `drivetrain.log`
into `out\drivetrain\`.

The run's whole configuration (settings, initial conditions, its length, the
recorded channels and the log) comes from settings files, applied left to right.
From `out\drivetrain\`, after a build, save the menu of every channel the model
can record, uncomment the ones you want, and run with it:

```powershell
cd out\drivetrain
```

```powershell
.\build\release\bin\Drivetrain.exe --list-signals | Out-File -Encoding utf8 outputs.settings
```

```powershell
.\build\release\bin\Drivetrain.exe --settings outputs.settings --duration 2s
```

`--settings` takes a list, `--settings default.settings, outputs.settings`, and
each file edits what the one before it left. `--dump-settings` prints what a run
would use, in a form `--settings` reads back.

Left alone, the drivetrain's root inputs stay at zero. A CSV table can drive
them instead: a time column, then a column per input, with units that are
converted. A double is interpolated between rows, and the run ends where the
table does:

```powershell
Set-Content drive.csv "time (s),axle_torque [N*m],ground_speed [km/h]", "0,0,0", "2,250,36", "4,400,90"
```

```powershell
.\build\release\bin\Drivetrain.exe --inputs drive.csv
```

A table the run recorded drives a run the same way, so a recorded run can be
replayed. `--inputs` is shorthand for an `inputs { file = "drive.csv"; }`
section, which a settings file can carry too. Read
`out\drivetrain\Drivetrain.hpp` and `out\drivetrain\main.cpp` to see what a
generated model looks like from outside.

The same model builds as a DLL for other languages. Still in `out\drivetrain\`:

```powershell
.\build.ps1 dynamic
```

That gives `build\release\bin\Drivetrain.dll`, which exports the C functions in
`se_sim.h`. From Python, for example:

```python
import ctypes
dt = ctypes.CDLL(r"build\release\bin\Drivetrain.dll")
dt.se_create.restype = ctypes.c_void_p
dt.se_init.argtypes = dt.se_tick.argtypes = dt.se_done.argtypes = [ctypes.c_void_p]
dt.se_finish.argtypes = [ctypes.c_void_p]
s = dt.se_create()
dt.se_init(s)
while not dt.se_done(s):
    dt.se_tick(s)
print(dt.se_finish(s))   # 0: a clean run
```

**3. Try the washout example.** A helper script emits, builds and runs it, then
opens the recorded CSV:

```powershell
powershell -File examples\washout\emit.ps1 -Run
```

**4. Run the test suite:**

```powershell
powershell -File tests\run.ps1
```

**Other `sec` commands:**

```powershell
.\build\sec.exe examples\drivetrain\Wheel.se               # parse and report diagnostics
.\build\sec.exe --dump-ast examples\drivetrain\Wheel.se    # print the AST
.\build\sec.exe --test -o build\test stdlib\se\kin\EulerToQuat.se   # write node tests
.\build\sec.exe --show-roots                                 # where packages are looked up
.\build\sec.exe --help
```

`-I` adds a **root** (§2.1): a directory, like a C++ include directory, in
which the package path is the directory path. So `-I examples` makes
`drivetrain.Corner` mean `examples/drivetrain/Corner.se`.

Two more roots come without `-I`:

- **The standard library**, found beside `sec` itself at `<dir of sec>/../stdlib`:
  `build\sec.exe` finds this checkout's `stdlib\`, and an installed
  `bin\sec.exe` finds its sibling `stdlib\`.
- **An `se.roots` file**, the nearest one at or above the input file. It lists
  your project's roots, one per line, relative to the file, with `#` starting a
  comment:

  ```
  # se.roots, checked in at the top of the project
  model
  ```

There is no environment variable: the roots file is checked in and the stdlib
ships with `sec`, so a checkout plus a pinned `sec` builds the same model on any
machine or CI runner. `--show-roots` prints the list and where each entry came
from; `--no-default-roots` leaves only `-I` (and `.` if there is none).

---

## What `sec --emit` writes

Files are named for the sim (`sim Drivetrain` → `Drivetrain.*`); `--stem`
overrides that.

| File | |
|---|---|
| `Drivetrain.hpp` | **The public interface, and nothing else.** `class Sim`, the root node's settings, inputs and outputs as typed structs, and the configure / start / step / finish calls |
| `Drivetrain.cpp` | Everything behind it: the engine runtime (namespace `se_rt`, the same for every model), the node classes, `Sim::Impl` and the schedule |
| `Drivetrain_c.cpp`, `se_sim.h` | The C interface (§14.2): `se_` functions over `Sim`, for the `dynamic` build and any language that calls C. The header is the same text for every model |
| `main.cpp` | An example host that uses the header alone. `--no-main` suppresses it |
| `Drivetrain.units.txt` | The unit manifest (§15.6): the contract for an external solver or host, not documentation |
| `Drivetrain.topology.html` | The signal-flow graph as it actually runs, interactive: open it in a browser, click any node for its settings, states and wiring, collapse composites and `group`s, which open collapsed where the model says `@diagram(collapsed)`. Laid out by Graphviz, which the page loads from jsDelivr, so it needs a network connection |
| `Drivetrain.topology.md` | The same graph as a Mermaid diagram |
| `se_runtime.hpp` | Only with `--runtime=separate`: the runtime as its own header |
| `build.ps1`, `build.sh` | The reference build, [below](#building-a-generated-model) |

The output has no third-party dependencies and no include path outside itself.
Node method bodies are copied from the `.se` files byte for byte, with `#line`
directives, so C++ compiler errors point at the `.se` source.

---

## Using a generated model from your own program

Include the header and do four things in order. `main.cpp` is a complete
example. This one also drives the root inputs itself, where `main.cpp` leaves
them to an inputs file:

```cpp
#include "Drivetrain.hpp"

int main() {
    drivetrain::Sim sim;
    sim.settings().wheel_inertia = 1.1;       // 1. configure (kg*m^2), optional
    if (!sim.init()) return sim.finish();      // 2. start; sim.error() says why
    while (!sim.done()) {                      // 3. step
        sim.inputs().axle_torque  = 250.0;     //    N*m
        sim.inputs().ground_speed =  22.0;     //    m/s
        sim.tick();
        double cut = sim.outputs().torque_cut; //    N*m, at the instant just sampled
    }
    return sim.finish();                       // 4. finish: 0 clean, 1 failed
}
```

Build the model as a library (`build.ps1 lib`). Then compile this against
`build/release/include` and link `build/release/lib/Drivetrain.lib`.
[tests/emit/host.cpp](tests/emit/host.cpp) is a fuller host that the test
suite compiles and runs.

`settings()` reaches the root node's settings. `load_settings()` takes the same
text as a `.settings` file: any setting or initial condition by path, with units
checked and converted, `sim.duration`, and `record { }`, `log { }` and
`inputs { }` sections that choose the recorded channels, the CSV, the log, and
the table that drives the root inputs. `load_settings_file()` reads a file, and
finds the inputs table it names beside it. `dump_settings()` writes the whole
configuration back out in that form, and `list_signals()` writes every channel
as a `record` section to edit. The same is available one key at a time, with
structured answers: `settings_list()`, `get()` and `set()`; `channels_list()`,
`record()`, `unrecord()` and `record_clear()`; `record_file()`,
`record_every()`, `log_file()`, `log_level()` and `inputs_file()`. The
generated executable takes `--settings FILE...`, `--set PATH=VALUE`,
`--inputs FILE`, `--duration TIME`, `--dump-settings` and `--list-signals`.

A host that watches a run can read any channel live through
`channel_handle()` and `read()`. With `log_capture()` and `log_next()`, the
console's log lines are kept in a queue for it, for a host that has no console.
A file name may contain `{time}`, which gives each run a file of its own.

Everything above is also available from C, as `se_` functions declared in
`se_sim.h` (§14.2). `build.ps1 dynamic` builds a DLL that exports them, and
nothing else. Every model exports the same names, so one wrapper in C#, Python
or LabVIEW can load any model.

**[What a generated model does](docs/generated-model.md)** describes precisely how
settings and initial conditions are finalized, what is called on every tick and in
what order, and what each call can see.

Behind the header, the model's whole data surface is four blocks:
- `x`: continuous state, the solver's interface;
- `sig`: every wire value and the root inputs;
- `dis`: every discrete state;
- `fin`: every finite state.

Together they are a complete checkpoint. Tables built with `offsetof` give each
slot's path, offset, type and unit. These are internals today; a public way to
reach them is on the [Roadmap](#roadmap).

---

## Building a generated model

```
.\build.ps1 [run|exe|lib|dynamic|test|clean|all] [-Config release|debug] [-Sanitize | -NoSanitize] [-StaticCrt] [-CPrefix NAME]
./build.sh  [run|exe|lib|dynamic|test|clean|all] [--config release|debug] [--sanitize | --no-sanitize] [--static-crt] [--c-prefix NAME]
```

| Mode | |
|---|---|
| `run` (default) | Build the exe, then run it as the `.sim` describes |
| `exe` | `build/<config>/bin/Drivetrain.exe` |
| `lib` | `build/<config>/lib/Drivetrain.lib` (`libDrivetrain.a`), the model and its C layer, with `build/<config>/include/Drivetrain.hpp` and `se_sim.h`: all a consumer needs |
| `dynamic` | `build/<config>/bin/Drivetrain.dll` (`lib/libDrivetrain.so`), exporting the `se_` functions of `se_sim.h` and nothing else, with its import library `lib/Drivetrain_dll.lib` and `include/se_sim.h` |
| `test` | Build the exe in debug with AddressSanitizer, run it, and fail unless it exits 0. A contract failure, an abort or a sanitizer report fails it. `-NoSanitize` builds it without the sanitizer; if the optional ASan component is missing, it does so anyway and says so |
| `clean` | Delete `build/` |
| `all` | `exe`, `lib` and `dynamic` |

**Configurations.** `release` is optimised and keeps debug information.
`debug` uses the debug C runtime, so a debug `lib` links only into a debug
program.

**Options.** `-Sanitize` adds AddressSanitizer to any mode. Only `test` has it
by default, because elsewhere it would drop `/RTC1`, force ASan on whatever
links a `lib`, and need its runtime DLL on PATH. `-StaticCrt` links the C
runtime statically. `-CPrefix dt_` renames the C functions (`dt_se_create`), so
two models' static libraries can be linked into one program. The host defines
`SE_C_PREFIX` the same way before including `se_sim.h`.

`build.ps1` finds Visual Studio itself when `cl` is not on PATH. Every build is
a full one: nothing is tracked between runs.

---

## The examples

- **[examples/drivetrain/](examples/drivetrain/)** is the small one: a wheel
  (continuous), a traction controller (discrete) and a composite that
  wires them. It exercises records on wires, flow-down settings, real-time
  pacing and a `.settings` override file. It is the
  example used throughout the spec.
- **[examples/washout/](examples/washout/)** is a classical motion-cueing
  washout filter for a motion platform: translational, tilt and rotational
  channels built almost entirely from the standard library. It was ported block
  for block from an existing C++ implementation, and its `.sim` files reproduce
  Reid & Nahon's standard test manoeuvres (surge, heave, yaw, cornering,
  slalom) for comparison against that implementation's reference runs.
  `Washout.sim` roots the filter with nothing driving its inputs, for a host
  program to drive.

---

## The standard library

`stdlib/` is an ordinary root whose package is `se`. `sec` finds it beside
itself, so no `-I` is needed to use it.

| Package | Contents |
|---|---|
| `se.math` | `Vec2`, `Vec3`, `Poly2`, `Poly3`; sums, scaling, norms, cross products; split/merge between records and scalars |
| `se.kin` | Quaternions and rotation matrices: `Quat`, `Rot`, Euler ↔ quaternion, quaternion products, exp/log, integrators (plain and leaky), frame rotations |
| `se.sig` | Gain-and-limit blocks and `WnZetaToPoly` |
| `se.sig.ct` | Continuous-time blocks: integrators, first- and second-order systems and low-passes, a sine |
| `se.sig.dt` | Discrete-time blocks: biquad, Butterworth, integrator, differentiator, second-order low-pass |
| `se.sig.src` | Sources: constant, step, ramp, sine, square, triangle, sawtooth, rectangular pulse, chirp, PRBS, and uniform, white and pink noise |

Several nodes carry a `tests { }` section (§6.13): tables of inputs and expected
outputs that `sec --test` turns into a test program.

---

## Testing

```powershell
powershell -File tests\run.ps1              # everything
powershell -File tests\run.ps1 -Update      # rewrite the expected-output files
powershell -File tools\nodetest.ps1         # just the node `tests { }` sections
```

`tests\run.ps1` finds Visual Studio itself. It checks:
- parsing, both clean files and every diagnostic, against golden output;
- emission, including the unit manifests and topology diagrams;
- that every generated model compiles warning-free;
- that some constructs must fail to compile, at the `.se` line;
- the numerical results of about 20 models against closed-form solutions:
  decay, integrators, filters, quaternion kinematics, sources, array states;
- a static-library consumer, the sanitized `test` mode, a hand-written host,
  and two models linked into one program.

---

## Repository layout

| Path | |
|---|---|
| [`SPECIFICATION.md`](SPECIFICATION.md) | The language, normative: three file kinds (`.se`, `.sim`, `.settings`), units, node anatomy, execution semantics, the time model, the lowering to C++, and a diagnostic catalogue |
| `src/diag.*` | Source management, diagnostic rendering, the error cap |
| `src/token.hpp`, `src/lexer.*` | Tokens, the lexer, and the scanners for verbatim C++ regions (§11) |
| `src/ast.hpp`, `src/parser.*`, `src/dump.*` | Stage 2: the AST, a recursive-descent parser for all three file kinds, and AST rendering |
| `src/units.*` | Dimensions, the unit symbol table (Appendix A), conversion |
| `src/model.hpp` | The resolved program and the flat runtime model |
| `src/resolve.*` | Stage 3: the root set, the loader, name binding, §6 checks |
| `src/elaborate.*` | Stage 4: the §7 evaluator, settings, wires, overrides |
| `src/schedule.*` | Stage 5: the feedthrough sort and the algebraic-loop diagnostic |
| `src/emit.*`, `src/runtime_src.cpp` | Stage 6: the C++, the manifest, the build scripts, and the embedded engine runtime |
| `src/testgen.*` | `sec --test`: node test programs from `tests { }` sections |
| `src/unity.cpp` | The single translation unit `build.bat` compiles |
| `tools/sec.cpp` | The driver and command line |
| `tools/nodetest.ps1` | Emit, build and run node tests |
| `stdlib/` | The standard library (`se.*`) |
| `examples/` | The drivetrain and washout examples |
| `tests/ok/`, `tests/err/` | Files that must parse cleanly, and files that must be diagnosed, each against `.expected` |
| `tests/emit/` | `.sim` files run through the whole pipeline, with manifest and topology goldens, hand-written hosts, and the numerical checks |
| `tests/cppfail/` | Models whose generated C++ must NOT compile |
| `tests/nodetest/` | Cases for the node-test runner itself |

To build `sec` with your own build system, compile the `.cpp` files under
`src/` plus `tools/sec.cpp`, and leave out `src/unity.cpp`. `sec` itself is
C++17; the code it generates is C++20.

---

## Design invariants

Two rules explain most of the code. They are the first things worth knowing
before reading it (§1.2 has all five):

- **The compiler never parses C++.** Method bodies, helper functions,
  `declarations` blocks and `native` type text are *verbatim regions*. They are
  found by bracket matching and copied through byte for byte.
  `src/lexer.cpp` has the scanners; §11.2 lists the traps they must survive:
  digit separators, raw strings, and braces inside literals and comments.
- **Enforcement is by scoping and typing, not analysis.** Purity, feedthrough
  and I/O confinement come from what the generator puts in scope of a body and
  from `const`. A violating body is rejected by the C++ compiler, with `#line`
  pointing back at the `.se` source (§15.4).

---

## Roadmap

In rough order. Each step is decided on its own, so this list changes.

1. ~~**The public interface.** A header that is only the interface, an example
   host, and build modes.~~ Done.
2. ~~**Per-instance run control.** Each model gets its own stop flag, contract
   checks and clocks, so a program can run a model more than once, or run
   several models.~~ Done.
3. ~~**Settings at run time.** `load_settings` / `dump_settings` in the compiled
   model (§14.1), and an application with `--settings`, `--set` and
   `--dump-settings`.~~ Done.
4. ~~**Recording at run time.** One settings format for a whole run, layered
   left to right: values, initial conditions, `sim.duration`, the recorded
   channels and the log, with a key-value API beside it.~~ Done.
5. ~~**A C interface and a `dynamic` build.** A DLL / shared object callable by
   name from C, C#, Python, LabVIEW and others.~~ Done.
6. ~~**Inputs from a file.** A run's root inputs driven from a CSV table, an
   `inputs` section in the settings format, and `--inputs` (§13.8), so a
   recorded run replays.~~ Done.
7. **An FMU.** Packaging a model as an FMI co-simulation FMU.
8. **Later:** clean-ups to the generated code's readability, and the open items
   in the spec's Appendix C, such as display units for recording and
   field-level wires.

---

## License

MIT — see [LICENSE](LICENSE).
