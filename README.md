# sim-engine

A node-based simulation engine. Models are written in a plain-text language and
lowered ahead of time to dependency-free C++17: a node definition becomes a
class, an instance becomes an object, and the wiring diagram becomes the calling
order.

- **[SPECIFICATION.md](SPECIFICATION.md)** — the language, normative. Three file
  kinds (`.se`, `.sim`, `.settings`), units, node anatomy, execution semantics,
  the time model, the lowering, and a diagnostic catalogue.
- **[examples/drivetrain/](examples/drivetrain/)** — a worked example: four `.se`
  sources, a `.sim` experiment, a `.settings` override file, plus a
  hand-written `Sim.generated.cpp` and `Sim.units.txt` kept as *illustration*,
  so the lowering can be read alongside the surface syntax. The real thing is
  what `sec --emit` writes; see below.
- **`src/`, `tools/`** — `sec`, the compiler driver.

## What works today

`sec` implements **all six stages** of the pipeline in SPECIFICATION.md §1.3 —
lexical, syntactic, resolution, elaboration, scheduling and emission. Given a
`.sim` file it writes compilable, runnable C++17.

```bash
build.bat                                  # -> build\sec.exe  (needs cl on PATH)

rem  parse only
build\sec.exe examples\drivetrain\*.se     # parse and report
build\sec.exe --dump-ast Wheel.se          # print the AST
build\sec.exe --dump-tokens Wheel.se       # print the token stream

rem  the whole pipeline
build\sec.exe --emit -I examples -o out examples\drivetrain\Drivetrain.sim
cd out && build.bat && Drivetrain.exe      # -> drivetrain.csv, drivetrain.log

powershell -File tests\run.ps1             # run the test suite
```

`-I` adds a **root** (§2.1) — a directory declared from outside the language,
exactly like a C++ include directory, inside which the package path is the
directory path. So `-I examples` makes `drivetrain.Corner` mean
`examples/drivetrain/Corner.se`. There is deliberately no environment variable
and no search default beyond `.`, so a build is reproducible from its command
line.

`--emit` writes into the output directory:

| File | |
|---|---|
| `Sim.generated.hpp` | The model, the schedule and the host API |
| `Sim.main.cpp` | The batch driver, and nothing else. `--no-main` suppresses it |
| `Sim.units.txt` | The unit manifest (§15.6) — the contract for an external solver or host, not documentation |
| `state_ref.hpp`, `se_runtime.hpp` | Fixed engine runtime headers; identical for every model |
| `build.bat`, `build.sh` | The direct reference build (§12.4): a full unity compile, no external build tool |

The output has no third-party dependencies and no include path outside itself.

The model is a header so a host can embed it and drive the root boundary, which
the batch driver cannot — the sim file has no syntax for supplying a root input:

```cpp
#include "Sim.generated.hpp"

int main() {
    drivetrain::Sim sim;
    sim.init();
    while (!sim.done()) {
        sim.sig.in.axle_torque  = 250.0;   // N*m   [boundary.in]
        sim.sig.in.ground_speed =  22.0;   // m/s
        sim.tick();
    }
    sim.finish();
}
```

`Sim::run_main()` is written in terms of the same six calls, so the driver and a
host cannot drift apart. See [tests/emit/host.cpp](tests/emit/host.cpp), which is
compiled and run by the suite for exactly that reason.

The model's whole data surface is three blocks — `x` (continuous state, the
solver's ABI), `sig` (every wire value and the root boundary) and `dis` (every
discrete state). Together they are a complete checkpoint, and
`Sim::signal_map()` / `Sim::discrete_map()` give each slot's path, offset, type
and unit, so an inspector or a shared-memory host needs no generated accessor
code.

## Layout

| Path | |
|---|---|
| `src/diag.*` | Source management, diagnostic rendering, the error cap |
| `src/token.hpp`, `src/lexer.*` | Tokens, the lexer, and the verbatim-region scanners (§11) |
| `src/ast.hpp` | Stage-2 AST |
| `src/parser.*` | Recursive-descent parser for all three file kinds |
| `src/dump.*` | AST rendering |
| `src/units.*` | Dimensions, the Appendix A symbol table, conversion |
| `src/model.hpp` | The resolved program and the flat runtime model |
| `src/resolve.*` | Stage 3 — the root set, the loader, name binding, §6 checks |
| `src/elaborate.*` | Stage 4 — the §7 evaluator, settings, wires, rates, overrides |
| `src/schedule.*` | Stage 5 — the feedthrough sort and the algebraic-loop diagnostic |
| `src/emit.*`, `src/runtime_src.cpp` | Stage 6 — C++, the manifest, the build script, the embedded runtime headers |
| `src/unity.cpp` | The single translation unit `build.bat` compiles |
| `tools/sec.cpp` | Driver and command line |
| `tests/ok/` | Files that must parse cleanly; AST compared to `.expected` |
| `tests/err/` | Files that must be diagnosed; output compared to `.expected` |
| `tests/emit/` | `.sim` files run through the whole pipeline. A case with a `.expected` must fail and its diagnostics are compared; one without must emit cleanly, and the result is then compiled with `cl`. `Decay` is additionally run and its CSV checked against the closed form. |

To use your own build system, compile the individual `.cpp` files under `src/`
plus `tools/sec.cpp` and ignore `src/unity.cpp`.

## Two design invariants worth knowing before reading the code

- **The compiler never parses C++.** Method bodies, helper functions,
  `declarations` blocks and `native` type text are *verbatim regions*, found by
  bracket matching and copied through byte for byte. `src/lexer.cpp` has the
  scanners; §11.2 has the traps they must survive (digit separators, raw
  strings, braces inside literals and comments).
- **Enforcement is by scoping and typing, not analysis.** Purity, feedthrough
  and I/O confinement are enforced by what the generator puts in scope of a body
  and by `const`, so a violating body is rejected by the C++ compiler with
  `#line` pointing back at the `.se` source. See §15.4.
