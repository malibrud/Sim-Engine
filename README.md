# sim-engine

A node-based simulation engine. Models are written in a plain-text language and
lowered ahead of time to dependency-free C++17: a node definition becomes a
class, an instance becomes an object, and the wiring diagram becomes the calling
order.

- **[SPECIFICATION.md](SPECIFICATION.md)** — the language, normative. Three file
  kinds (`.se`, `.sim`, `.settings`), units, node anatomy, execution semantics,
  the time model, the lowering, and a diagnostic catalogue.
- **[examples/drivetrain/](examples/drivetrain/)** — a worked example: four `.se`
  sources, a `.sim` experiment, a `.settings` override file, the C++ they lower
  to, and the unit manifest. The generated C++ compiles with stock MSVC and no
  third-party packages.
- **`src/`, `tools/`** — `sec`, the compiler driver.

## What works today

`sec` implements **stages 1–2** of the pipeline in SPECIFICATION.md §1.3:
lexical and syntactic analysis, with real diagnostics and error recovery. It
produces an AST. Name resolution, unit checking, elaboration, scheduling and
code emission are specified but not yet implemented.

```bash
build.bat                                  # -> build\sec.exe  (needs cl on PATH)
build\sec.exe examples\drivetrain\*.se     # parse and report
build\sec.exe --dump-ast Wheel.se          # print the AST
build\sec.exe --dump-tokens Wheel.se       # print the token stream
powershell -File tests\run.ps1             # run the parser test suite
```

## Layout

| Path | |
|---|---|
| `src/diag.*` | Source management, diagnostic rendering, the error cap |
| `src/token.hpp`, `src/lexer.*` | Tokens, the lexer, and the verbatim-region scanners (§11) |
| `src/ast.hpp` | Stage-2 AST |
| `src/parser.*` | Recursive-descent parser for all three file kinds |
| `src/dump.*` | AST rendering |
| `src/unity.cpp` | The single translation unit `build.bat` compiles |
| `tools/sec.cpp` | Driver and command line |
| `tests/ok/` | Files that must parse cleanly; AST compared to `.expected` |
| `tests/err/` | Files that must be diagnosed; output compared to `.expected` |

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

## Stale files

`include/sim.h` and `examples/exponential_decay.c` are left over from an earlier,
unrelated C-callback design. They are not part of this language and are not
referenced by anything above.
