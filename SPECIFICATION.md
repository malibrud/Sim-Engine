# The sim-engine Model Language

**Specification, version 0.1 (draft) — 2026-07-27**

A plain-text language for describing hierarchical, node-based simulation models,
lowered ahead of time to dependency-free C++17.

---

## Table of contents

1. [Scope and conformance](#1-scope-and-conformance)
2. [Program structure](#2-program-structure)
3. [Lexical structure](#3-lexical-structure)
4. [Units](#4-units)
5. [Types](#5-types)
6. [Node definitions](#6-node-definitions)
7. [The elaboration expression language](#7-the-elaboration-expression-language)
8. [Lifecycle methods and execution semantics](#8-lifecycle-methods-and-execution-semantics)
9. [Time model and scheduling](#9-time-model-and-scheduling)
10. [Builtins](#10-builtins)
11. [Verbatim C++ regions](#11-verbatim-c-regions)
12. [Build metadata](#12-build-metadata)
13. [The sim file](#13-the-sim-file)
14. [The settings source](#14-the-settings-source)
15. [Lowering to C++](#15-lowering-to-c)
16. [Diagnostics](#16-diagnostics)
17. [Grammar summary](#17-grammar-summary)
18. [Appendix A — Unit symbols](#appendix-a--unit-symbols)
19. [Appendix B — Reserved words](#appendix-b--reserved-words)
20. [Appendix C — Known gaps and open issues](#appendix-c--known-gaps-and-open-issues)

---

## 1. Scope and conformance

### 1.1 What this document specifies

This document specifies the **surface language** — three file kinds and their
meaning:

| Kind | Extension | Contents |
|---|---|---|
| Model file | `.se` | Exactly one public `node` or `type` declaration, plus optional file-private helpers. |
| Sim file | `.sim` | One `sim` declaration: the run/experiment configuration. |
| Settings source | `.settings` | A flat list of path-addressed setting and initial-condition overrides. |

It also specifies the **shape of the lowering** (§15) to the extent that the
generated code is a public contract — the state-vector layout, the unit manifest,
and the accessor names that verbatim C++ bodies may rely on.

### 1.2 Design invariants

These hold everywhere in this document; a rule that appears to contradict one of
them is a defect in this document.

- **I1 — The compiler never parses C++.** Wherever C++ appears it is a
  *verbatim region*, delimited by bracket matching or by a terminator, copied
  through untouched. There is no C++ front end, no libclang, and no subset
  checker.
- **I2 — Enforcement is by scoping and typing, not by analysis.** Purity,
  feedthrough, and I/O confinement are enforced by *what the generator puts in
  scope of a body* and by `const`, so a violating body is rejected by the C++
  compiler, with `#line` directives pointing back to the `.se` source.
- **I3 — Units are checked in the DSL and erased at the boundary.** Generated
  C++ is plain `double`. Every declarative site (port, wire, setting, state,
  record field, sim-file value) is dimensionally checked and converted at
  elaboration; body arithmetic is not checked. The unit manifest (§15.6) carries
  the erased information out.
- **I4 — Everything is discrete at the base step.** One global fixed step, one
  global solver, per-node rates as integer decimations. A "continuous" node is a
  base-rate node that happens to have integrated states.
- **I5 — Generated identifiers are the generator's business.** Users write only
  model paths. Nothing at the language level constrains emitted names.

### 1.3 Conformance and stage boundaries

A conforming implementation processes a program in these stages. This document
marks rules with the earliest stage that can detect a violation, because the
parser described in §17 and shipped in `src/` implements stages 1–2 only.

| Stage | Name | Detects |
|---|---|---|
| 1 | **Lexical** | Bad characters, unterminated literals/comments, unbalanced verbatim regions. |
| 2 | **Syntactic** | Grammar violations, misplaced sections, duplicate sections. |
| 3 | **Resolution** | Unknown names, file/name mismatch, root collisions, duplicate members. |
| 4 | **Elaboration** | Unit mismatches, required-setting omissions, setting cycles, non-divisor rates, instantiation. |
| 5 | **Scheduling** | Feedthrough sort, algebraic loops, rate/active-set construction. |
| 6 | **Emission** | Code, unit manifest, slot map, build script. |

Stages 3–6 are specified here but are **not** implemented by the current parser.
Where this document says *"is an error"* without qualification, the stage column
in §16.4 gives the detecting stage.

### 1.4 Terminology

- **Definition** — a `node` or `type` declaration. Lowers to a C++ class/struct.
- **Instance** — a `node name : Definition { … };` statement inside a
  `structure` block. Lowers to an object (leaf) or to nothing (composite).
- **Leaf** — a node with code. **Composite** — a node with children and wires.
- **Port** — an entry in `inputs` or `outputs`.
- **Elaboration** — the compile-time evaluation that folds settings, resolves
  the hierarchy, and produces the flat runtime model.
- **Base step** — the global fixed time step. **Tick** — one base step.
- **Frame** — the real-time synchronisation interval (`sync`), an integer
  multiple of the base step.
- **Sample instant** — a tick on which a given node is active.

---

## 2. Program structure

### 2.1 Files, roots, and packages

A **root** is a directory declared to the compiler from outside the language
(command line or project manifest), exactly like a C++ include directory. There
is no in-language root declaration and no environment variable — roots come from
an explicit, checked-in list, so builds are reproducible.

Within a root, **the directory path is the package path**, and the package path
is the C++ namespace path:

```
<root>/drivetrain/Wheel.se        ->  package drivetrain, node drivetrain.Wheel
<root>/std/mech/Damper.se         ->  package std.mech, node std.mech.Damper
```

The **standard library is one root** shipped with the toolchain, with no special
status: `std` is an ordinary package name, not reserved. Its "version" is
whatever is on disk at that path; pinning is delegated to whatever already
manages filesystem contents (vendoring, a submodule), not to a DSL-native
package-version resolver.

**Root collisions.** A package path that resolves in more than one root is an
error (`SE0305`). A root may be marked `override` in the manifest, which permits
it to shadow nodes at the same path in earlier roots. Shadowing is therefore
always a declared, visible act — never silent search-order behaviour.

### 2.2 The `package` declaration

Every `.se` file begins with a `package` declaration:

```
package drivetrain;
```

It is **redundant with the file's location and checked against it**: the declared
path must equal the file's directory path relative to the root that resolved it.
A mismatch is an error (`SE0301`). The declaration exists so that a reader — or a
grep — can see a file's package without reconstructing the root set; location
remains the identity.

`.sim` and `.settings` files have no `package` declaration. They are not library
members and are named by filesystem path only.

### 2.3 One public declaration per file

A `.se` file contains exactly one **public** declaration, whose name must equal
the file's base name. Any further `node` or `type` declarations in the same file
are **file-private**: visible to the rest of that file, invisible to every other
file, and not addressable by any package path.

```
// Wheel.se
package drivetrain;

node Wheel { … }          // public: the name matches the file
node WheelFriction { … }  // file-private helper
```

A file with no declaration whose name matches its base name is an error
(`SE0302`). Two public declarations cannot occur, because only one name can
match.

### 2.4 `use` declarations

`use` declarations follow `package` and precede all definitions.

```
use std.mech.Damper;      // per-symbol: bare `Damper` in this file
use std.mech;             // package alias: `mech.Damper` in this file
```

- **Per-symbol import** binds the last segment as a bare name.
- **Package alias import** binds the last segment as a package alias; members
  are then named `alias.Member`.
- **No wildcard imports.** `use std.mech.*` is rejected (`SE0210`) so that every
  name's origin is traceable from the `use` block, and so a later stdlib
  addition can never silently capture or collide with an existing name.
- A fully-qualified name may always be used without any `use`.
- Importing a name that collides with a file-private declaration, with another
  import, or with a definition in the file is an error (`SE0303`).

### 2.5 Declaration order

```
compilation_unit := package_decl { use_decl } { definition }
```

Within the definitions there is **no ordering requirement** — a node may
reference a definition that appears later in the file, or in a package that
imports it back. Resolution is whole-program (stage 3), not single-pass.

### 2.6 Package manifest files

A file named `package.se` in a package directory is a **package manifest**. It
declares no node or type; it may contain only a `package` declaration, `use`
declarations, and a `build` block (§12). Its purpose is to let a package act as
a **dependency bundle** — a named collection of build primitives that any model
can pull in by depending on the package.

```
// <root>/vendor/openssl/package.se
package vendor.openssl;

build {
    include_dir "third_party/openssl/include";
    link "libssl"    when windows;
    link "libcrypto" when windows;
    link "ssl"       when linux;
    link "crypto"    when linux;
}
```

`package.se` is the only `.se` file exempt from §2.3.

---

## 3. Lexical structure

### 3.1 Source text

A source file is a sequence of bytes interpreted as UTF-8. Outside comments,
string literals, and verbatim regions, only the ASCII subset is significant; a
non-ASCII byte elsewhere is an error (`SE0101`).

Line terminators are LF, CRLF, or CR; all three count as one line break. A file
need not end with a line break.

Horizontal whitespace is space and tab. **The language is not
whitespace-significant**; line breaks are ordinary whitespace and never
terminate a statement. Statements are terminated by `;` or by a closing brace.

### 3.2 Comments

```
// to end of line
/* block, does not nest */
```

An unterminated block comment is an error (`SE0102`). Comments inside verbatim
regions belong to the verbatim text and are copied through, but are still
recognised as comments for the purpose of bracket matching (§11.2).

### 3.3 Identifiers

```
identifier := ( letter | '_' ) { letter | digit | '_' }
letter     := 'A'…'Z' | 'a'…'z'
digit      := '0'…'9'
```

Identifiers are case-sensitive, with no length limit. A leading underscore is
permitted but discouraged in port and setting names, since those names appear in
recorded CSV headers and in the unit manifest.

### 3.4 Keywords

Only these words are **reserved** everywhere:

```
package  use  type  node  self
settings inputs outputs states vars native structure declarations build
continuous discrete
init output rates on_step final
sim
```

Everything else that reads like a keyword in this document is **contextual** —
recognised by position, and usable as an ordinary identifier elsewhere. That
includes:

- the build primitives `link`, `include_dir`, `define`, `cflag`, the tag keyword
  `when`, and the platform atoms — contextual inside `build`;
- every sim-file key (`root`, `step`, `solver`, `duration`, `mode`, `sync`,
  `window`, `record`, `log`, `file`, `every`, `signals`, `level`, `levels`) —
  contextual inside `sim`;
- the accessor prefixes (`param`, `in`, `out`, `state`, `der`, `next`, `var`)
  and the builtin namespaces (`log`, `sim`) — these live in **C++ scope inside
  verbatim bodies**, not in the DSL's token stream, so they are not DSL keywords
  at all. They are, however, unavailable as member names (§6.11).

Keeping this set small is deliberate: adding a sim-file key or a build primitive
must never invalidate an existing model that used that word as a port name.

### 3.5 Number literals

```
number   := integer | decimal
integer  := digit { digit }
decimal  := digit { digit } '.' { digit } [ exponent ]
          | '.' digit { digit } [ exponent ]
          | digit { digit } exponent
exponent := ( 'e' | 'E' ) [ '+' | '-' ] digit { digit }
```

There are no type suffixes, no digit separators, no hexadecimal, and no octal. A
literal has no inherent type: it is converted to the declared type of the site it
initialises, and rejected if that conversion is lossy for an integer target
(`SE0402`).

Numbers inside verbatim C++ regions are C++'s business and are not lexed by these
rules — with one exception noted in §11.2, where digit separators interact with
character-literal scanning.

### 3.6 String literals

```
string := '"' { char | escape } '"'
escape := '\' ( '"' | '\' | 'n' | 'r' | 't' | '0' )
```

A string literal may not span a line break; an unterminated literal is an error
(`SE0103`). An unrecognised escape is an error (`SE0104`) rather than being
passed through, so that adding escapes later is not a silent behaviour change.

### 3.7 Punctuation and operators

```
{  }  (  )  ;  ,  :  .  =  -->  +  -  *  /  ^  %
```

`-->` is a single token. The lexer is maximal-munch, so `-->` always wins over
`-` `-` `>`; a lone `>` is not a token in this language and is an error
(`SE0105`) outside verbatim regions. `%` occurs only as a unit symbol (§4.1).

### 3.8 Verbatim regions

Two constructs introduce text the lexer does not tokenise:

| Construct | Extent | § |
|---|---|---|
| Lifecycle method body, helper function, `declarations { … }` | Balanced braces | §11.2 |
| `native` member type text | To the terminating `;` | §11.3 |

Verbatim regions are scanned, not lexed: the scanner tracks nesting and line
numbers but produces a single token carrying the region's text and its start
position.

---

## 4. Units

Units are **semantic, first-class, and mandatory** on every scalar declaration.
They are checked by full dimensional analysis at every declarative site, with
automatic conversion, and then erased (I3).

### 4.1 Syntax

A unit annotation is a parenthesised unit expression:

```
unit         := '(' unit_expr ')'
unit_expr    := '-'                                  // dimensionless
              | unit_term { ( '*' | '/' ) unit_term }
unit_term    := unit_factor [ '^' unit_exponent ]
unit_factor  := '1' | unit_symbol | '(' unit_expr ')'
unit_exponent:= [ '-' ] integer
              | '(' [ '-' ] integer [ '/' integer ] ')'
unit_symbol  := identifier | '%'
```

`*` and `/` are left-associative and of equal precedence, so `(kg/m/s)` is
kg·m⁻¹·s⁻¹. Use parentheses for anything else: `(1/(kg*m^2))`.

Examples, all from working models:

```
(m)  (kg)  (s)  (rad/s)  (m/s^2)  (N*m)  (kg*m^2)  (1/(kg*m^2))  (-)  (%)
(km/h)  (deg)  (Hz)  (m^(1/2))
```

### 4.2 Dimensions and equivalence

A unit expression evaluates to a **dimension** — a rational exponent vector over
the seven SI base dimensions — together with a **scale factor** and, for a few
units, an **offset**. Two units are *compatible* iff their dimension vectors are
equal. `(-)`, `(1)`, and `(%)` are all dimensionless; `(%)` carries scale 1/100.

Compatible units convert automatically, and the conversion folds to a
compile-time constant. `(m/s)` → `(km/h)` multiplies by 3.6 at elaboration and
costs nothing at run time. Incompatible units are a hard error (`SE0410`) with
both dimensions spelled out.

**Offset units** (`degC`, `degF`) are accepted only where a conversion is
unambiguous — as the declared unit of a setting, state, port, or field, and as
the unit of a literal. They are rejected inside a compound unit expression
(`SE0411`), because `(degC/s)` is ambiguous between a temperature and a
temperature *difference*.

### 4.3 Where units are checked

Every one of these is a declarative site, checked at stage 4:

1. A setting default expression against the setting's declared unit.
2. A setting binding at instantiation against the child setting's unit.
3. A state default against the state's unit.
4. A wire: the producing port's unit against the consuming port's — per field,
   for records.
5. A sim-file or `.settings` value against its target's unit.
6. A recorded signal's unit, for the CSV header and the manifest.

**Derivative units are derived, never declared.** `continuous omega (rad/s)`
implies `der.omega` is `(rad/s^2)`. Declaring a unit for a derivative is a syntax
error — there is no place in the grammar to put one.

### 4.4 What is *not* checked

Arithmetic inside a verbatim C++ body is not dimension-checked. This is a
deliberate, accepted cost:

- In-body checking was the sole job a units library would have had in the
  generated code, and it gets bypassed in practice. Real leaf bodies call
  `<cmath>`, lookup tables, Eigen, vendor SDKs, and pasted legacy code, all of
  which speak `double`. Empirical fits — a Pacejka tyre model, say — have no
  honest dimensions at all. The pattern degenerates to *strip → compute →
  re-attach*, where the only surviving check is whether you spelled the same
  unit symbol twice.
- The value curve is inverted: typing pays most on the one-line physics leaf
  where you were least likely to err, and nothing in the long body where the
  bugs actually live.

The compensations are real generator outputs, not documentation: the **unit
manifest** (§15.6) and a trailing unit comment on every emitted declaration.

The choice is **reversible**. The DSL retains full unit information, so
re-introducing typed bodies later is a codegen change plus a header — not a
redesign.

### 4.5 Known limitation: angle

`(deg)` and `(rad)` are dimensionally identical (both dimensionless), so
dimensional analysis **cannot** catch a radians/degrees mix-up — a common,
expensive real-world bug. Treating angle as a distinct base dimension would catch
it but is physically wrong and breaks identities like (m/s) = (m)·(rad/s). This
is unresolved; see Appendix C.

---

## 5. Types

### 5.1 Scalar types

The scalar type set is closed:

| DSL type | C++ type | Notes |
|---|---|---|
| `double` | `double` | The only type permitted for a `continuous` state. |
| `float` | `float` | |
| `int` | `int` | |
| `bool` | `bool` | Must be declared `(-)`. |

A scalar declaration is always `name (unit): type;` — the unit is not optional,
and `(-)` is how you spell dimensionless. Requiring it makes an omission a syntax
error rather than an unnoticed dimensionless default.

### 5.2 Record types

A `type` declaration defines a record that travels over a wire as one value:

```
// WheelState.se
package drivetrain;

type WheelState {
    speed  (rad/s): double;   // wheel angular velocity
    torque (N*m):   double;   // net torque applied this step
    slip   (-):     double;   // longitudinal slip ratio
}
```

- A record field line is **syntactically identical to a scalar port line**, so a
  port is either `name (unit): ctype;` or `name: TypeName;`. One grammar, two
  shapes.
- Fields may themselves be records; a cycle in the field graph is an error
  (`SE0310`).
- Records lower to a plain C++ struct with one member per field, each annotated
  with its unit as a comment and listed in the manifest's `[types]` section.
- Records may appear as `inputs`, `outputs`, and `vars`. They may **not** be
  `settings` (settings are scalars, because the flow-down expression language is
  scalar) or `states` (the solver ABI is a flat `double` vector).

### 5.3 What a wire carries

A wire carries **the whole record**. Whether a wire may ever carry a single
field —

```
whl.ws.speed --> tc.u;     // reserved, not currently legal
```

— is deliberately still open. The grammar in §17 accepts a multi-segment endpoint
path so that the eventual decision is not a grammar change; stage 3 currently
rejects any endpoint with more than one segment after the instance (`SE0311`).

---

## 6. Node definitions

```
node Wheel {
    settings { … }
    inputs   { … }
    outputs  { … }
    states   { … }
    vars     { … }
    native   { … }
    declarations { … }
    build    { … }
    structure { … }

    init()      { … }
    output(…)   { … }
    rates(…)    { … }
    on_step(…)  { … }
    final(…)    { … }

    double helper(double x) const { … }   // verbatim private member
}
```

Sections may appear in any order. Each may appear **at most once**; a repeat is
an error (`SE0220`) rather than being merged, so that a section's contents are
always contiguous and reviewable in one place.

### 6.1 Leaf or composite, never both

There is exactly one `node` concept, and its kind is **inferred**:

- A node with any lifecycle method, helper function, `states`, `vars`, `native`,
  or `declarations` is a **leaf**.
- A node with a `structure` block is a **composite**.
- A node with both is an error (`SE0320`).
- A node with neither is a **stub**: legal, and useful as a placeholder, but it
  must have no outputs, since nothing could compute them.

A composite **declares no states and no code**. Its state is the union of its
descendants'. That falls directly out of the design intent — a group node "just
groups and aggregates" and should never do real work — and it is what lets the
composite leave no runtime trace at all (§15.2).

There is no separate `model` keyword. A composite *is* a node, which is exactly
what gives arbitrary-depth hierarchy: today's model is tomorrow's subsystem with
no edit.

An explicit kind assertion is permitted as a **checked assertion only**, never as
a declaration that changes meaning.

### 6.2 `settings`

```
settings {
    inertia     (kg*m^2): double;                           // required
    radius      (m):      double = 0.31;                    // default
    corner_mass (kg):     double = param.total_mass / 4.0;  // derived
}
```

- A setting with **no default is required**: every instantiation must bind it,
  checked statically at stage 4 (`SE0420`).
- A default is an expression in the elaboration language (§7). It may reference
  **other settings of the same node** via `param.*`, which makes settings a
  dependency graph requiring a topological sort; a cycle is an error (`SE0421`).
- Derived settings behave the way a reader expects: overriding `total_mass`
  recomputes `corner_mass`; overriding `corner_mass` pins it.
- Settings are **fixed at elaboration**. There is no two-phase init, no runtime
  reparameterisation, and no way for a parent to reach into a child at run time.

**Reserved setting: `rate`.** A node may declare

```
settings { rate (Hz): double = 200.0; }
```

`rate` is an ordinary setting in every respect — declared, defaulted, bound at
instantiation, flowed down — except that the scheduler consumes it (§9.2) and it
produces **no runtime member**. If declared, it must have unit `(Hz)` and type
`double` (`SE0422`). A node that does not declare `rate` runs at the base step.

### 6.3 `inputs` and `outputs`

```
inputs {
    drive_torque (N*m): double;      // scalar port
    ws:                 WheelState;  // record port
}
outputs {
    torque_cut (N*m): double;
}
```

Ports have no default and no initialiser: an input's value comes from its wire,
an output's from `output()`.

- **Fan-out is allowed**; **fan-in is forbidden** (§6.9.2).
- An unconnected input is an error at stage 4 (`SE0430`) — there is no implicit
  zero. An unconnected *output* is fine; it is simply unread.
- Port order matters only for readability. Nothing positional exists.

### 6.4 `states`

**One `states` section holds both kinds, and the kind keyword is mandatory.**

```
states {
    continuous omega (rad/s): double = 0.0;
    discrete   cut   (N*m):   double = 0.0;
    discrete   ticks (-):     int    = 0;
}
```

Omitting `continuous`/`discrete` is an error (`SE0230`), not a default. There is
deliberately **no preferred state kind**: a default would privilege one, and the
two differ genuinely in write mechanism, in schedule, and in their relationship
to the solver.

| | write accessor | written in | in the solver state vector |
|---|---|---|---|
| `continuous` | `der.<name>` | `rates()` | **yes** |
| `discrete` | `next.<name>` | `on_step()` | no |

- The **write accessor is symmetric and separately enforced.** The generated
  `Der` struct carries only continuous fields and the `Next` struct only discrete
  ones, so `der.ticks` and `next.omega` are "no such member" compile errors with
  no body parsing whatsoever (I2).
- **`next.` means simultaneous update.** A direct `state.x = …` mutation would be
  *sequential*, making results depend on statement order inside `on_step()`. With
  `next.`, every discrete state reads the old value and writes the new one, which
  is what the state-space form x⁺ = h(t,x,u) actually says. The lowering uses a
  separate `Next` buffer plus a generated `commit()`.
- Only `double` may be `continuous` (`SE0431`); the state vector is a flat
  `double` array.
- **The declared default is the initial condition.** There is no separate
  `initial` section — ICs ride the one override channel (§13.4). `init()` is the
  escape hatch for a computed or equilibrium IC.
- Discrete states earn their place over `vars` precisely via that IC channel: a
  declarative default plus a sim-file override, which `vars` never had.

### 6.5 `vars`

```
vars { inertia_inv (1/(kg*m^2)): double; }
```

Per-instance persistent storage that is **not state**: private C++ members,
outside the solver vector and outside the override channel.

- **Readable in every method; writable only in `init()` and `on_step()`.**
- A `var` needs no default; it is zero-initialised and is expected to be computed
  in `init()`.
- Records are permitted as `vars`.

The write restriction is the same rule as everywhere else in §8: `output()` and
`rates()` are evaluated four times per RK4 step at trial states the trajectory
never visits. A peak detector written as

```
output() { var.max_v = max(var.max_v, state.velocity); }   // rejected
```

would report a maximum that never physically happened and that changes with the
solver. Confining writes to `on_step()` makes that unrepresentable.

### 6.6 `declarations`

```
declarations {
    #include <cmath>
    #include "vendor/tyre_model.h"
    namespace { constexpr double kEps = 1e-9; }
}
```

Verbatim C++ emitted at **file scope, before the class**, wrapped in a generated
include guard so it lands exactly once regardless of how many times the node is
instantiated. This is the home for `#include`s, free functions, file-local
constants, and helper types.

A node with a `declarations` block is a leaf (§6.1).

### 6.7 `native`

Opaque C++ members the DSL cannot type — sockets, device objects, vendor SDK
state:

```
native {
    sock: SOCKET;
    dev:  std::unique_ptr<Device>;
    port: Serial{"COM3", 115200};    // RAII: ctor at construction
}
```

- **Its own section, not a `vars` escape hatch.** Natives are not quantities:
  they have no unit, are not recordable, never enter the state vector, and are
  outside the override channel. A separate section keeps every rule about `vars`
  exception-free instead of growing an "unless it's opaque" carve-out.
- **The type text is a verbatim region scanned to the terminating `;`** (§11.3),
  so there is no quoting, no `>>` ambiguity, and no C++ parsing (I1).
- An optional C++ initialiser — brace, paren, or `=` form — makes RAII work with
  no `final()` boilerplate.
- **Bodies refer to natives by their bare name** (`sendto(sock, …)`, not
  `native.sock`), because native C++ should feel natural. This does not unwind
  the accessor design of §6.10: the prefixes exist because one name can be both
  a state and an output, and a native has no such twin.
- **Natives are in scope only in `init()`, `on_step()`, and `final()`.** In
  `output()` and `rates()` the generator simply does not emit the alias, so the
  name is undeclared and the C++ compiler says so, with `#line` pointing at the
  `.se` line. The reason is in §8.4. Note that `const` cannot do this job: a
  `const SOCKET` is still perfectly usable by `recv()`, so the purity mechanism
  that works for `double` members leaks entirely for handles, and scoping is what
  plugs it.

A node with a `native` section is a leaf.

### 6.8 `build`

Build metadata is declared first-class; see §12.

### 6.9 `structure`

A composite's children and wiring:

```
structure {
    node whl : Wheel {
        inertia = param.wheel_inertia;   // declarative flow-down
    };
    node tc : TractionController {
        rate       = 200.0;
        slip_limit = 0.10;
    };

    self.axle_torque  --> whl.drive_torque;
    self.ground_speed --> whl.ground_speed;

    whl.ws --> tc.ws, self.ws;           // fan-out; one record on one wire

    tc.torque_cut --> self.torque_cut;
}
```

#### 6.9.1 Instances

```
instance := 'node' identifier ':' qualified_name [ '{' { binding } '}' ] ';'
binding  := identifier '=' expression ';'
```

- The trailing `;` after the closing brace is **required** — it terminates the
  statement the same way a port declaration's does, and it makes recovery after a
  malformed binding list unambiguous.
- Bindings are **declarative** (§7): each right-hand side is a side-effect-free
  expression over literals, the enclosing node's `param.*`, arithmetic, and
  builtin math, evaluated at elaboration and unit-checked.
- **No cross-instance references.** `mass = fl.mass` is illegal (`SE0440`).
  Flow-down goes strictly parent → child.
- Binding an unknown setting, or binding one twice, is an error (`SE0441`).

Why declarative and not imperative: under an imperative scheme, every group node
would need hand-written C++ solely to pass a number down, which contradicts §6.1
— a group node does no work. Declarative flow-down also keeps settings fixed at
elaboration, needs no two-phase init, and makes required settings statically
checkable.

#### 6.9.2 Wires

```
wire     := endpoint '-->' endpoint { ',' endpoint } ';'
endpoint := ( 'self' | identifier ) '.' identifier { '.' identifier }
```

- `-->` is the connection token; `;` terminates.
- **A composite wires its own ports with `self.`** — `self.throttle --> eng.throttle`
  and `drv.speed --> self.speed`. Bare names would collide with child instance
  names.
- **Fan-out** has two equivalent spellings — repeated lines, or a comma list on
  one line. They desugar identically.
- **Fan-in is forbidden.** Two sources into one input is an error (`SE0442`);
  insert an explicit `Sum` node. This is what makes the single-writer wire
  lowering sound (§15.3).
- **Wires are untyped in the syntax.** There is no type annotation on a
  connection; the type is a property of the ports and is checked, with unit
  conversion inserted, at stage 4.
- Direction is always source → destination. An output-to-output or
  input-to-input wire is an error (`SE0443`), except through `self`, where
  `self.<input>` is a source and `self.<output>` is a destination — the boundary's
  polarity is inverted from the inside, as it must be.

### 6.10 Accessors

Inside a verbatim body, node data is reached through **namespaced accessors**:

| Accessor | Readable in | Writable in |
|---|---|---|
| `param.<name>` | every method | never |
| `in.<name>` | methods that declare it | never |
| `out.<name>` | — | `output()` |
| `state.<name>` | every method | `init()` |
| `der.<name>` | — | `rates()` |
| `next.<name>` | — | `on_step()` |
| `var.<name>` | every method | `init()`, `on_step()` |
| bare name (`native`) | `init`, `on_step`, `final` | same |

The prefixes are not decoration. They exist because **one name can legitimately
be both a state and an output** — `velocity` as an integrated state and as a
published output is the single most common node shape in this domain — so bare
names would be ambiguous exactly where clarity matters most.

These tokens **survive verbatim into the generated C++** and are resolved there as
struct member accesses. That is a direct consequence of I1: the compiler cannot
rewrite `in.drive_torque` into a mangled name because it never parses the body,
so objects with exactly these names must be in scope. Which in turn is what rules
out a naive "one giant class, every wire and state a member" flattening — it would
put everything in scope of every body and destroy both enforcement mechanisms.

### 6.11 Name collisions

Within one node, these share a namespace and must be mutually distinct: settings,
inputs, outputs, vars, natives, and instance names. **States are in a separate
namespace from outputs, and only from outputs** — a state and an output may share
a name; everything else collides (`SE0330`).

A member may not be named `param`, `in`, `out`, `state`, `der`, `next`, `var`,
`log`, or `sim`, since the generated code puts objects by those names in scope of
bodies (`SE0331`).

### 6.12 Consistency rules

Typo safety comes from these rules rather than from a marker keyword on
functions:

| If the node has… | it must define… | else |
|---|---|---|
| `outputs` | `output()` | `SE0340` |
| any `continuous` state | `rates()` | `SE0341` |
| any `discrete` state | `on_step()` | `SE0342` |

The converse is also checked: `rates()` in a node with no continuous states
(`SE0343`) and `output()` in a node with no outputs (`SE0344`) are errors, since
neither could do anything.

`init()` and `final()` are always optional. Note that these rules are per-kind,
not blanket: a purely continuous node needs no `on_step()`, and a purely
algebraic node needs no `rates()`.

---

## 7. The elaboration expression language

A small, total, side-effect-free expression language, evaluated once at
elaboration. It appears in exactly four places: setting defaults, setting
bindings at instantiation, state defaults, and sim-file / `.settings` values.

### 7.1 Grammar

```
expression     := additive
additive       := multiplicative { ( '+' | '-' ) multiplicative }
multiplicative := unary { ( '*' | '/' ) unary }
unary          := ( '+' | '-' ) unary | power
power          := primary [ '^' unary ]              // right-associative
primary        := number [ unit ]
                | 'param' '.' identifier
                | identifier '(' [ expression { ',' expression } ] ')'
                | identifier                          // named constant
                | '(' expression ')'
```

Precedence, loosest to tightest: `+ -` · `* /` · unary `+ -` · `^`. So `-2^2` is
−4 and `2^3^2` is 2⁹, matching ordinary mathematical convention.

### 7.2 Values and units

Every value is a real number with a dimension.

- A literal with a unit suffix — `1500 (kg)` — has that unit.
- A **bare literal takes the unit of the site it initialises**. `radius = 0.31`
  where `radius` is `(m)` means 0.31 m. That keeps the common case clean; write
  the unit when you want the conversion, as in `speed_limit = 130 (km/h)` binding
  a `(m/s)` setting.
- Arithmetic combines dimensions: `*` and `/` add and subtract exponent vectors;
  `+` and `-` require compatible dimensions and convert the right operand to the
  left's unit; `^` requires an integer or rational literal exponent and
  multiplies the exponent vector.
- The final value is converted to the target's declared unit, or rejected
  (`SE0410`).

### 7.3 Builtin functions and constants

| | |
|---|---|
| Constants | `pi`, `e`, `inf` |
| Arithmetic | `abs`, `min`, `max`, `sign`, `floor`, `ceil`, `round`, `mod` |
| Powers | `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `hypot` |
| Trigonometry | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2` |

Trigonometric arguments must be dimensionless (radians); `asin`/`acos`/`atan`
return dimensionless values. `sqrt` halves the exponent vector and requires the
result to be rational. `min`, `max`, `hypot`, `atan2`, and `mod` require
compatible arguments. `log*` and `exp` require dimensionless arguments. Anything
else is `SE0412`.

Evaluation is in `double`. Division by zero, a domain error, or a non-finite
result is an elaboration error (`SE0413`) — the language is total by rejection,
not by producing NaN.

### 7.4 What is deliberately absent

No conditionals, no comparisons, no string operations, no user-defined functions,
no references to run-time signals, and no references to other instances. The
language stays small so that (a) elaboration is guaranteed to terminate, (b)
settings can be topologically sorted, and (c) every value is a compile-time
constant that folds into the generated code.

A declarative `stop_when:` condition in the sim file is parked precisely because
it would need run-time signals here, which is a much larger change than it looks
(Appendix C).

---

## 8. Lifecycle methods and execution semantics

This is the load-bearing part of the design.

### 8.1 The five methods

| Method | Equation | Purity | When it runs |
|---|---|---|---|
| `init()` | — | mutating | Once, at run start, in elaboration order. |
| `output()` | y = g(t, x, u) | **pure** | Whenever signals propagate — many times per step. |
| `rates()` | ẋ = f(t, x, u) | **pure** | Every minor step (4× per RK4 step). |
| `on_step()` | x⁺ = h(t, x, u) | mutating | Once per accepted step, at the node's sample instants. |
| `final(ctx)` | — | mutating | Once at run end, in reverse init order. |

Write permissions, exactly:

- `init()` writes `state` (the IC escape hatch) and `var`; may touch natives; may log.
- `output()` writes `out` only.
- `rates()` writes `der` only.
- `on_step()` writes `next` and `var`; may touch natives; may log.
- `final(ctx)` writes nothing model-visible; may touch natives; may log.

`on_step` is named for the `on_` prefix reading as "fires at a specific moment" —
`update()` says nothing about when, and `commit()` describes the mechanism rather
than the event.

### 8.2 Why `output()` and `rates()` are separate

They cannot be merged, and this is not a stylistic point: **the split is the only
reason feedback loops are simulable.** A node with no feedthrough has an
`output()` that needs only `state`, so it can be called *before its inputs are
known* — which is exactly what unwedges a closed loop. If `rates()` also set
outputs, every node would look like a feedthrough node and loop-breaking would be
destroyed everywhere in the model.

### 8.3 Feedthrough is declared by method signature

```
output(cmd)      { out.force = state.force + param.kff * in.cmd; }  // feedthrough on cmd
output()         { out.velocity = state.velocity; }                 // breaks loops
rates(cmd, load) { … }                                              // constrains nothing
on_step(ws)      { … }                                              // constrains nothing
```

- **A method's parameter list is exactly the set of inputs it reads.** The names
  are bare (not `in.cmd`), because the list can only contain inputs.
- Naming an input not declared in `inputs` is an error (`SE0350`); naming one
  twice is `SE0351`.
- This lowers to a signature that **literally omits** the undeclared inputs
  (§15.4), so the C++ compiler enforces it: a stale-value read becomes a compile
  error, not a silently wrong number. No runtime debug proxy is needed, and the
  question "what value should an undeclared input have?" never arises.
- **Granularity is per-port, not per-node.** Node-level marking produces *false*
  algebraic loops whenever a node has one feedthrough input and one rates-only
  input.
- **"Algebraic" and "feedthrough" are orthogonal.** A node can have states *and*
  feedthrough — a PID, or any state-space block with D ≠ 0 — so feedthrough
  cannot be derived from the leaf/composite inference.
- Only `output()`'s list constrains ordering. `rates()` and `on_step()` run after
  outputs have propagated, so their lists are documentation plus enforcement.

### 8.4 Why mutation is confined

`output()` and `rates()` are evaluated **four times per RK4 step, at trial states
the trajectory never visits**. Any side effect there fires four times, at values
that are not on the solution. That single fact is the reason for three separate
restrictions, which are really one restriction wearing three hats:

- `var` writes → `init()`/`on_step()` only (§6.5).
- `log.*` → `init()`/`on_step()`/`final()` only (§10.1).
- `native` handles in scope → `init()`/`on_step()`/`final()` only (§6.7).

A `sendto()` in `output()` would put four packets on the wire per step, at values
the model never actually took, silently. Confining mutation keeps results
solver-independent and reproducible.

### 8.5 The sort

Build a graph over the nodes active on the current tick:

> **edge M → N** iff M sources one of N's **feedthrough** inputs.

- `output()` runs in **topological order** of that graph.
- `rates()` may run in **any** order once outputs have propagated.
- A node whose `In_output` is empty is a **sort root** and needs no predecessor.
  Registered discrete nodes, unit delays, and integrators are all sort roots by
  the same single mechanism, not three special cases.

### 8.6 Algebraic loops: rejected, not solved

A cycle in the §8.5 graph is an implicit equation z = f(z) with no valid
ordering. It is **rejected** (`SE0510`).

Why reject rather than tear-and-Newton:

- Most algebraic loops are **modelling mistakes**. A solver that silently
  converges on a number produces an answer for a model the author did not mean to
  write.
- Iteration to convergence has no statable worst-case execution time, which is
  incompatible with the real-time target (§9.4).

Adding a solver later is a **pure relaxation** — no model valid today would break
— and the strongly-connected-component analysis is shared with the diagnostic, so
nothing is wasted.

**The budget goes into the diagnostic instead.** Because feedthrough lives in a
method signature, the error can name the cycle, name each node's offending
parameter with file and line, and state the exact fix:

```
error[SE0510]: algebraic loop through 3 nodes
  --> Loop.se:14:21
   |
14 |         ctl.cmd --> act.u;
   |                     ^^^^^ act.output(u) reads u
   = cycle: plant.y -> ctl.e -> ctl.cmd -> act.u -> act.f -> plant.f -> plant.y
   = note: ctl.output(e) declares feedthrough on `e`      [Controller.se:22]
   = note: act.output(u) declares feedthrough on `u`      [Actuator.se:18]
   = help: drop `u` from `act.output(u)` and latch it in on_step(), or
           insert a unit delay / integrator anywhere in the cycle
```

Beating "Algebraic loop detected involving block X" is an explicit goal.

Note that a loop solver **cannot** be a library node: it must re-evaluate an
entire SCC iteratively, which requires engine support.

---

## 9. Time model and scheduling

### 9.1 One global fixed base step

```
step: 1 (ms);
```

A single global fixed step, set in the sim file. This is the highest-leverage
constraint in the design:

- Every node's rate is an **integer decimation** of the base step: a 100 Hz
  controller on a 1 kHz base fires every 10th tick. Sample instants land exactly,
  by construction.
- It **eliminates most of Simulink's sample-time machinery**: no step-size
  negotiation, no forcing a variable-step solver to hit sample instants, no
  zero-crossing search for sample hits, and **no fractional resampling ever**.
- It unifies the model: everything is discrete at the base rate, and one
  scheduler serves the whole model.

It also retroactively justifies rejecting algebraic loops (§8.6) — a Newton solve
has no statable worst-case execution time.

### 9.2 Rates

`rate` is a reserved setting (§6.2) set at instantiation, flowed down through the
expression language, and constant from elaboration onward. A 100 Hz PID and a
50 Hz PID are the same definition with different instances.

The decimation is `base_rate / rate`. A rate that is not an integer divisor of
the base rate is an elaboration error:

```
error[SE0450]: rate 60 Hz is not an integer divisor of the 1 kHz base step
  --> Vehicle.se:31:22
   = note: 1000 / 60 = 16.667; nearest legal rates are 62.5 Hz and 58.82 Hz
```

**Rate propagation/inference was dropped**, and that is a real simplification. A
purely algebraic node — `Gain`, `Sum` — has **no rate at all**. It is
re-evaluated in every tick's `output()` pass from whatever its inputs currently
hold. Fed by a 100 Hz held signal, its output effectively changes at 100 Hz; fed
by two rates, it changes whenever either input does. All well-defined, no aliasing
introduced, and no rate to assign — which dissolves the ambiguity that inference
existed to resolve.

### 9.3 Discrete outputs and rate transitions

Discrete outputs are **zero-order held** between sample instants: `output()` is
evaluated at the node's sample ticks and the value persists until the next one.

**No explicit rate-transition blocks exist or are needed.** Simulink's Rate
Transition block exists because *its* multitasking execution order is not
guaranteed; here (1) all rates are harmonic integer decimations and (2) there is a
single deterministic schedule with a topological sort, so transitions are
deterministic by construction.

How discrete interacts with the §8.5 sort:

- The sort runs **per tick over the nodes active on that tick** — base-rate nodes
  always, a slow node only on its sample ticks.
- A slow node **not** sampling this tick contributes its held output as a
  **constant: a sort root with no edge.** So a fast consumer reading a slow signal
  never creates an inter-tick ordering constraint.
- A held output returns the **same value across all RK4 trial substeps**, so the
  multiple-evaluation hazard of §8.4 never touches discrete nodes at all.

Two idioms, both expressed with the existing signature mechanism:

```
// Registered (the default idiom): one sample of latency, sort root, loop-breaker.
// Physically honest — a real digital controller has exactly this latency.
on_step(err) { var.cmd = param.kp * in.err; }
output()     { out.cmd = var.cmd; }

// Direct (Mealy): zero added latency, but a feedthrough node subject to
// ordering, and able to form an algebraic loop.
output(err)  { out.cmd = param.kp * in.err; }
```

### 9.4 The real-time frame

Real-time execution introduces a **third integer quantity** alongside the base
step and the rate decimations:

```
step: 1 (ms);
sync: 10 (ms);     // must be an integer multiple of step
```

The execution model is two-clock: a dedicated thread **free-runs every base step
in the frame as fast as it can, then busy-waits on the high-resolution timer**
until the frame boundary. At 1 ms `step` and 10 ms `sync`, that is ten steps flat
out, then spin.

- `sync` not being an integer multiple of `step` is an elaboration error
  (`SE0451`) — the same error class as a non-divisor rate.
- An **overrun** is a batch that missed its frame boundary. The next batch then
  starts immediately with no spin, so **sim time drifts behind wall time**.
- **Every base step always runs.** Dropping steps to catch up was rejected: it
  would punch a hole in a fixed-step integration, advancing state by one `dt`
  while wall time advanced by several. Drift is the honest failure mode; a
  corrupted trajectory is not.
- **Overrun is instrumented, not policed** — no abort escalation and no policy
  key. See §13.6 for the metrics.

### 9.5 The solver

The solver is **global, not per-node** (`solver: rk4;`). Mixing integrators
across one state vector was deliberately kept closed. Normative values are
`euler`, `rk2`, and `rk4`; the menu beyond that is an open question.

### 9.6 The tick

One base tick, normatively:

1. Determine the active set — base-rate nodes, plus nodes whose decimation
   divides the tick index.
2. **Minor steps.** For each solver stage: propagate `output()` in topological
   order over the active set, then call `rates()` on every node with continuous
   states, in any order. Held outputs are constants throughout.
3. **Accept the step.** Advance `x` by the solver's combination of stage rates,
   and advance time.
4. **Propagate outputs** once more at the accepted state.
5. **`on_step()`** on every active node that has discrete states or writes
   `var`, then `commit()` the `next` buffers.
6. **Record** (§13.5) if this tick is a recording tick.
7. If `mode: realtime` and this tick closes a frame, spin to the frame boundary
   and update the metrics.

---

## 10. Builtins

Two namespaces are injected into bodies. Neither is a DSL keyword (§3.4); both
are names the generator puts in C++ scope, and *withholds* where they are
illegal.

### 10.1 `log.*`

```
log.trace(fmt, args…)   log.debug(…)   log.info(…)   log.warn(…)   log.error(…)
```

- `{}` positional formatting. The engine **auto-tags** every record with sim
  time, node path, and level — those are never arguments.
- **Restricted to `init()`, `on_step()`, and `final()`** for the reason in §8.4.
- **Escape hatch:** `log.trace` alone is permitted in `rates()` and `output()`.
  It is tagged with the substep index and suppressed unless a verbose flag is
  set, so the four-evaluations-per-step hazard is visible in the output rather
  than hidden.
- Logging is **always observational**. It never affects control flow and never
  halts the run.

### 10.2 `sim.stop` and `sim.abort`

```
sim.stop("target speed reached");             // clean end
log.error("CAN bus timeout after {} ms", t);  // then:
sim.abort("CAN bus timeout");                 // fault end
```

- Callable from `init()` and `on_step()` — the mutating, I/O-capable methods.
- **Halting is the `sim.*` family's job.** Overloading `log.fatal` with control
  flow was rejected: a logging call that sometimes ends the program is exactly
  the hidden side effect the purity rules fight everywhere else. A fault site
  writes two honest, greppable lines instead of one overloaded one.
- This mirrors the recording (quantitative) / logging (qualitative) split, and it
  gives `final()` a **single uniform source** of termination reasons with a clean
  severity axis; log level stays orthogonal metadata.

Note that `sim.*` being a *recordable* namespace in the sim file (§13.6) does
**not** make those metrics readable from node code. The broader "`sim.*` as
ambient context" design — `sim.time` readable in `on_step()`, and so on — is
deliberately not opened, to avoid inheriting the "which fields are live when"
question as a side effect.

### 10.3 The run context

`final` may take one parameter, a read-only run-context handle:

```
final(ctx) {
    log.info("run ended: {} at t={} s", ctx.message, ctx.time);
    closesocket(sock);
}
final() { closesocket(sock); }    // a node that does not care
```

| Field | Meaning |
|---|---|
| `ctx.reason` | `completed` (duration expiry) · `stopped` (`sim.stop`) · `aborted` (`sim.abort`) · `error` (C++ exception or unrecoverable fault) |
| `ctx.message` | The string passed to `sim.stop`/`sim.abort`; empty for `completed` |
| `ctx.time` | Final sim time |

It is a **parameter, not a global**, for the same reason feedthrough rides
`output()`'s signature: the outcome is only knowable at teardown, and a parameter
says exactly that.

`final()` runs on normal and error termination alike, in reverse init order, but
**only for nodes whose `init()` completed** — a node that never initialised cannot
be asked to tear down. RAII on C++ members is the quiet default for nodes that
write no `final()` at all.

---

## 11. Verbatim C++ regions

### 11.1 What is verbatim, and what the generator owns

Verbatim: lifecycle method bodies, helper function definitions, `declarations`
blocks, and `native` type text. Everything else — signatures, accessor structs,
class shape, member order, initialisation — is generated.

**All data members go through DSL sections** (`states`, `vars`, `native`). A
verbatim `cpp_members { … }` block was considered and rejected: the compiler must
own the full class layout to check collisions against generated accessors, to
order initialisation, and to make future checkpointing and introspection
possible.

The deeper reason the language is shaped this way — DSL-generates-C++ rather than
decorated C++ with a reflection pass — is **who owns file scope**. Free-form
decorated C++ leaves would force either a libclang front end or the abandonment
of unity builds, because leaf globals, statics, and includes escaping to file
scope are exactly what breaks jumbo builds. Tool-owned file scope avoids both,
for free. A *restricted* decorated-C++ subset was rejected too: it relocates the
parsing problem into subset enforcement rather than removing it.

### 11.2 Brace-matched regions

A brace-matched verbatim region begins at the `{` that opens it and ends at the
matching `}`. The scanner tracks depth while **skipping over** these constructs,
inside which braces are not counted:

| Construct | Ends at |
|---|---|
| `//` comment | End of line |
| `/* … */` comment | `*/` |
| `"…"` string | Unescaped `"`; `\` escapes the next character |
| `'…'` character | Unescaped `'`; `\` escapes the next character |
| `R"delim(…)delim"` raw string | The matching `)delim"` |

Two traps a conforming scanner must handle, both of which produce silently-wrong
region boundaries if missed:

1. **Digit separators.** In `1'000'000` the `'` is not a character literal. A `'`
   opens a character literal only when the preceding non-whitespace character is
   not a digit, letter, or `_`. Getting this wrong swallows the rest of the
   function.
2. **Raw strings.** `R"(unbalanced { brace)"` is legal C++ and must not affect
   depth. The delimiter is the character sequence between `R"` and `(`, up to 16
   characters.

An unterminated region — end of file at depth > 0 — is `SE0110`, reported at the
**opening** brace's position, with a note giving the depth reached.

`#line` directives are emitted so that C++ compiler diagnostics point back into
the `.se` file. Note that the generator injects lines into a body's preamble
(native aliases, §6.7), which **shifts body line numbers**: the `#line` value
must account for the injected count, or every C++ error in that method points a
line or two off.

### 11.3 The `native` type region

A `native` declaration's type text runs from after the `:` to the terminating
`;`. The scanner skips the same constructs as §11.2 and additionally ignores a
`;` that occurs inside balanced `()`, `[]`, or `{}`. Angle brackets are **not**
tracked — `std::map<int, std::vector<double>>` contains no `;`, so template
argument lists never need to be understood, which is what keeps the `>>`
ambiguity out of the language entirely (I1).

Leading and trailing whitespace is trimmed; interior text is copied byte for
byte.

### 11.4 Helper functions

Any node-body item that is not a section keyword and not a lifecycle name is a
**helper function**: verbatim C++ copied into the class as a private member.

The scanner finds its extent without parsing C++: from the item's first token,
scan forward to the first `{` **not** inside a comment, literal, or bracket pair,
then brace-match (§11.2). Consequently:

- A helper must be a **definition**, not a declaration. A `;` encountered at
  bracket depth 0 before any `{` is `SE0240` — "expected a function body";
  forward declarations belong in `declarations`.
- A helper's name is not known to the compiler, which never parses the signature,
  so a helper cannot be checked against §6.11 and cannot be called from another
  node. Collisions with generated names surface as C++ errors, mapped back by
  `#line`.

The text is copied as-is, so a helper's own purity is the author's
responsibility. A non-`const` helper called from `output()` fails to compile,
which is the intended outcome.

---

## 12. Build metadata

Nodes and packages declare their own build requirements first-class; the compiler
**aggregates them at elaboration and emits build commands alongside the code**, so
the output is buildable as emitted. CMake was explicitly rejected as a permanent
public contract; taking the generated sources into your own build system remains a
supported first-class path.

### 12.1 Primitives

```
build {
    link        "ws2_32"  when windows;
    link        "pthread" when linux;
    include_dir "vendor/asio/include";
    define      "ASIO_STANDALONE";
    cflag       "/bigobj" when windows;
}
```

The vocabulary is **concrete build primitives**, not logical dependencies. The
compiler aggregates and forwards them; it never invokes an external resolver, so
there is no mandatory pkg-config or CMake `find_package`. A logical
`depends "openssl"` with compiler-side resolution was rejected precisely because
it would force such a backend.

| Primitive | Argument |
|---|---|
| `link` | Library name or path, as the linker expects it |
| `include_dir` | Path, relative to the declaring file's root |
| `define` | `NAME` or `NAME=value` |
| `cflag` | Verbatim compiler flag |

Duplicate primitives are aggregated and de-duplicated; order of first appearance
is preserved.

### 12.2 Platform conditionals

Any primitive may carry a `when <platform>` tag. Flat, local, and greppable.
Platform *blocks* remain available later as pure sugar. Reusing the §7 expression
language for this was rejected — it over-couples static build metadata to the
elaboration evaluator.

The platform atom vocabulary is **not yet pinned** (Appendix C). An implementation
must accept at least `windows`, `linux`, and `macos`, and must report an unknown
atom rather than silently ignoring the declaration (`SE0250`).

### 12.3 Bundles

A dependency is **just a package that declares primitives** (§2.6), reusing the
"stdlib is only a root" spine. Ergonomic reuse on top of the primitives, not a new
concept.

### 12.4 The emitted build

The emitted build is a **direct reference build script** — no Ninja, no CMake, no
external build tool:

- **Generate-code and build-it are separable steps.** The compiler's contract is
  *generate code*; building it into a running simulation is an optional second
  stage.
- The script is a **full-build reference only**. It never tracks changes and
  never builds incrementally or in parallel.
- That is not an omission. **The unity build is the intended build model** — all
  sources `#include`d into a single translation unit. There are no separate object
  files, so incremental tracking is a category that does not exist rather than a
  feature skipped. It also matches the rest of the design: concrete classes, no
  virtuals, one flat generated call sequence.

---

## 13. The sim file

The experiment configuration lives in **its own file**, keyword `sim`:

```
sim Demo {
    root:     Vehicle { total_mass = 1500 (kg); };
    step:     1 (ms);
    solver:   rk4;
    duration: 10 (s);
    mode:     realtime;
    sync:     10 (ms);
    window:   1 (s);

    settings: "vehicle_heavy.settings";

    record: {
        file:  "run.csv";
        every: 10;
        signals {
            plant.out.velocity;
            plant.state.position;
            front.left.out.force;
            sim.utilization;
            sim.overrun_pct;
        }
    }

    log: {
        file:  "run.log";
        level: info;
        levels { front.left: debug; }
    }
}
```

**Why separate from the model:** one model gets run many ways — 1 ms real-time on
target versus 0.1 ms batch offline, a 10-second manoeuvre versus a 10-minute soak.
If configuration lived in the node, swapping experiments would mean editing the
model, and the model would stop being the thing under test.

### 13.1 Entries

| Entry | Form | Default |
|---|---|---|
| `root:` | `qualified_name [ '{' bindings '}' ] ';'` | required |
| `step:` | time expression | required |
| `solver:` | `euler` · `rk2` · `rk4` | `rk4` |
| `duration:` | time expression | required |
| `mode:` | `batch` · `realtime` | `batch` |
| `sync:` | time expression, an integer multiple of `step` | required iff `realtime` |
| `window:` | time expression | `1 (s)` |
| `settings:` | string — a `.settings` path | none |
| `record:` | block, §13.5 | no recording |
| `log:` | block, §13.7 | console only |

Each entry may appear at most once (`SE0260`). Block-valued entries (`record:`,
`log:`) take **no trailing semicolon**; scalar entries require one.

The `root` entry is what makes a model runnable: it names the single top-level
node to instantiate, and may bind its settings exactly as an instance in a
`structure` block does.

### 13.2 Path addressing

**Path addressing is the keystone** shared by setting overrides, initial
conditions, and recording: a **dotted path from the root**, of
instance-instance-…-name form.

```
front.left.mass              // a setting three levels down
plant.out.velocity           // a qualified signal
sim.overrun_pct              // an engine metric
```

The separator is the same `.` used for port access, and namespaces are
deliberately not visually distinguished — one addressing scheme, learned once.

### 13.3 Settings inflow

Resolution order, **last writer wins**:

1. Definition default (§6.2)
2. Flow-down expression at instantiation (§6.9.1)
3. The sim file's `root` block
4. The external `.settings` source (§14)

Overrides **may reach any depth**, not just the root:

```
front.left.mass = 340 (kg);
```

The cost is explicit and is the user's to own: **a deep override detaches from the
flow-down.** Set `total_mass` and `front.left.mass` and that corner no longer
equals total/4. A possible mitigation — warning when a deep override lands on a
setting that a flow-down expression also drives — is noted but not required.

An override may target a **setting or a state**, never an **output** (outputs are
computed). The resolver therefore only has to disambiguate setting-versus-state
(`SE0460`).

### 13.4 Initial conditions

There is **no `initial:` section**, deliberately. Initial conditions are
declarative state defaults in the model (§6.4), and overriding one from outside
rides the **same settings channel, addressed by path**:

```
total_mass          = 1800 (kg);    // a setting
front.left.velocity =    2 (m/s);   // a state's initial value
```

Layering: declared state default → `init()` code → external override.

### 13.5 Recording

Recording is **quantitative**: an aligned numeric time-series table, distinct from
logging in every respect.

```
record: {
    file:  "run.csv";
    every: 10;                     // decimate: every 10th base tick
    signals { plant.out.velocity; plant.state.position; }
}
```

- **Signals are qualified paths** — `plant.out.velocity`, `plant.state.position` —
  reusing the `out.` / `state.` accessors. A bare `plant.velocity` is legal **when
  unambiguous**, and produces a loud error naming both candidates when it is not
  (`SE0461`), since a node may legitimately have a state and an output with the
  same name (§6.11).
- Recording internals directly was chosen over outputs-only recording, which
  would force boilerplate pass-through outputs just to observe a state.
- **Records at the base step by default**; the first column is `time (s)`.
  `every: N` decimates the file.
- A signal slower than the recording rate is **held (ZOH)** — which is its actual
  physical value, since that is precisely what downstream nodes see.
- The header carries units: `plant.out.velocity [m/s]`.
- Wildcards (`plant.out.*`) are not in this version; see Appendix C.

### 13.6 Real-time instrumentation

Two metrics, both dimensionless percentages reported on 0–100 with a `%` unit —
self-documenting in a CSV header, and `-` in the manifest either way:

| Metric | Meaning |
|---|---|
| `sim.overrun_pct` | Percentage of frames that overran. **A rate, not a count.** |
| `sim.utilization` | Percentage of the frame spent computing rather than spinning. |

- Both are computed over a **rolling window** (`window:`, default 1 s), not
  cumulatively. A since-start percentage is a lagging indicator: a fault starting
  twenty minutes into a run is buried under a long clean history — exactly the
  case a live value must catch. The window sets resolution: 10 ms frames over 1 s
  is 100 frames, so 1% steps.
- **Both are recordable signals**, addressable as `sim.*` paths in `signals`, so
  they land in the CSV beside model signals. That is what identifies *which*
  manoeuvre blows the timing budget, not merely that something did. Per-frame
  values recorded at base rate are held, which §13.5's rule already covers at no
  extra cost.
- A **whole-run cumulative summary** is emitted at teardown, free off the same
  counters.

### 13.7 Logging

Logging is **qualitative**: asynchronous, unaligned text and events.

```
log: {
    file:  "run.log";
    level: info;                     // global threshold
    levels { front.left: debug; }    // per-node-path overrides
}
```

Levels are `trace`, `debug`, `info`, `warn`, `error`. The console always receives
`warn` and above regardless of the configured threshold. The file format is plain
text; a structured alternative is an open question.

---

## 14. The settings source

The external settings source is a flat list of path-addressed bindings:

```
// vehicle_heavy.settings
total_mass          = 1800 (kg);
front.left.mass     =  340 (kg);
front.left.velocity =    2 (m/s);   // a state's initial value
rear.tc.slip_limit  = 0.08;
```

```
settings_source  := { binding }
binding          := path '=' expression ';'
path             := identifier { '.' identifier }
```

**File and string are one grammar, two carriers.** The same text may be passed
inline — as a command-line flag or through an API call — with no separate syntax.
Every value is unit-checked against its target (§4.3).

Comments follow §3.2. There is no `package`, no nesting, and no conditionals:
this is deliberately the flattest possible surface, because it is the layer that
gets generated by scripts and sweeps.

---

## 15. Lowering to C++

Normative to the extent stated. A worked, hand-verified example lives in
[`examples/drivetrain/`](examples/drivetrain/) — four `.se` sources, a `.sim`
file, the generated C++, the unit manifest, and the runtime header. It compiles
with `cl /std:c++17 /EHsc`, with no third-party packages and no include path
outside the repository.

### 15.1 The constraint that decides everything

The compiler never parses C++ (I1) — it bracket-matches. Therefore it **cannot
rewrite** `in.drive_torque` into a mangled name. The tokens `param.`, `in.`,
`out.`, `state.`, `der.`, `next.`, and `var.` survive verbatim into emitted
bodies, which means **objects with exactly those names must be in scope**. Every
other lowering decision follows from that.

### 15.2 Composites flatten; leaves stay classes

One `Sim` class holds leaf *instances* as members. **Composites leave no runtime
trace at all** — their settings fold to literals at elaboration and their
hierarchy becomes name prefixes on generated members. Leaves stay classes so each
compiles once regardless of instance count.

### 15.3 A wire is not its own variable

A wire **coincides with the producer's `Out` storage**, owned by `Sim`. So
`whl.ws --> tc.ws, self.ws` becomes two readers of `fl_whl_out.ws`: fan-out is
free and nothing is copied. Fan-in being forbidden (§6.9.2) is exactly what makes
single-writer storage sound.

**ZOH falls out for free.** A 200 Hz node's `Out` is not written on the other nine
base ticks, so the held value is simply the variable still sitting there. No hold
buffer exists.

### 15.4 Enforcement falls out of the lowering

Not from an analyser:

- **Purity is `const` on the method.** `output()` and `rates()` are const
  members, so a body physically cannot write `state`, `var`, or `param`. The
  writable thing is an out-parameter.
- **Feedthrough is a struct field set.** One `In_<method>` view struct per
  method, holding exactly the inputs that method's DSL signature listed. Reading
  an undeclared input is "no such member". An **empty** `In_output` is the sort
  root and the loop-breaker.
- **I/O confinement is name withholding.** Native aliases and `log.*` are emitted
  into the preamble of the methods where they are legal, and simply are not
  declared elsewhere.

```cpp
struct Wheel {
    struct Param { double inertia;    /* kg*m^2 */  double radius; /* m */ };
    struct Var   { double inertia_inv; };            // 1/(kg*m^2)
    struct State { state_ref omega; };               // rad/s
    struct Der   { state_ref omega; };               // rad/s^2
    struct Out   { WheelState ws; };

    struct In_rates  { double drive_torque; };
    struct In_output { double drive_torque; double ground_speed; };

    Param param; State state; Var var;

    void init();                                              // mutating
    void rates (const In_rates&  in, Der& der) const;         // pure
    void output(const In_output& in, Out& out) const;         // pure
};
```

### 15.5 The state vector

```cpp
static constexpr std::size_t n_states = 1;
std::array<double, n_states> x{};
std::array<double, n_states> xd{};
```

- **One flat `double` array**, so an external solver integrates it directly.
- **Only `continuous` states.** Discrete states live in their node and never reach
  the solver.
- **Slot convention:** `x[i]` holds the value *in that state's declared unit*.
  That is a contract with the external solver, so the generator emits a **slot
  map**.
- Named states are non-template **`state_ref` proxies holding a `double*`**. The
  proxy exists because generated `State` and `Der` structs must be
  default-constructible before `bind_states()` runs, and a `double&` member can be
  neither default-constructed nor rebound. `operator double()` means every
  operator, every `<cmath>` function, and every third-party API just works.
- `state_ref` lives in a fixed engine runtime header — about twenty lines, with no
  includes at all.
- **There is no `gather_x`/`scatter_x`.** The vector *is* the storage, so there is
  no second copy to desynchronise. A solver that owns its own vector (CVODE,
  odeint) does one `memcpy` each way per evaluation: the proxies buy a clean
  *layout*, not zero copying.
- `der` and `next` are **`Sim`-owned scratch, not node data**. `der` is evaluated
  4× per RK4 step at trial states and discarded; `next` is the simultaneous-update
  buffer. `states` and `vars` remain the only DSL storage sections.

### 15.6 The unit manifest

Because units are erased (I3), the manifest is a **load-bearing generator
output**, not documentation. It is the contract for anyone writing an external
solver, feeding the root boundary, or overriding a setting from outside:

```
[state.continuous]        # slot | model path | unit  -- the solver's ABI
x[0]      fl.whl.omega          rad/s
xd[0]     fl.whl.omega'         rad/s^2

[state.discrete]          # not in the state vector; lives in its node
          fl.tc.cut             N*m

[boundary.in]             # root inputs the host must supply
          src.axle_torque       N*m

[boundary.out]            # root outputs the host may read
          fl.whl.ws.speed       rad/s

[settings]                # elaborated values, post-conversion
          fl.whl.inertia        kg*m^2      = 0.9

[types]
WheelState.speed              rad/s
```

### 15.7 Dispatch

**Concrete generated classes: no `INode` base and no virtuals.** The scheduler is
generated code — a flat sequence of direct member calls in elaboration order.
Recorder and inspector machinery is generated per model too, since the compiler
knows every signal's address and unit at codegen time. Everything a vtable would
buy is already guaranteed at generation time.

---

## 16. Diagnostics

### 16.1 Format

```
error[SE0230]: state `omega` must be declared `continuous` or `discrete`
  --> examples/drivetrain/Wheel.se:17:5
   |
17 |     omega (rad/s): double = 0.0;
   |     ^^^^^ expected `continuous` or `discrete` before the state name
   = note: there is deliberately no default state kind — the two differ in
           write accessor (`der.` vs `next.`), in schedule, and in whether
           they enter the solver's state vector
   = help: write `continuous omega (rad/s): double = 0.0;`
```

Required elements: severity and code, a one-line message, a `file:line:column`
location, and a caret span. Notes and helps are optional but strongly encouraged;
the language's whole diagnostic strategy is to spend the budget here (§8.6).

### 16.2 Severity and exit status

`error` (no output produced), `warning` (output still produced), `note`/`help`
(attachments only). Exit status is 0 with no errors, 1 with at least one error,
and 2 for a driver failure such as an unreadable file.

### 16.3 Recovery

The parser must not stop at the first error. Recovery is by **synchronisation on
statement and section boundaries**: on an error inside a declaration, skip to the
next `;` or to the closing `}` of the enclosing block, whichever comes first, then
resume. A verbatim region is skipped whole, never resynchronised into. An
implementation must cap cascading diagnostics rather than emit an unbounded stream
from one bad brace.

### 16.4 Catalogue

Codes are stable. `SE01xx` lexical, `SE02xx` syntactic, `SE03xx` resolution,
`SE04xx` elaboration, `SE05xx` scheduling, `SE06xx` emission.

| Code | Stage | Message |
|---|---|---|
| `SE0101` | 1 | Illegal character outside a verbatim region |
| `SE0102` | 1 | Unterminated block comment |
| `SE0103` | 1 | Unterminated string literal |
| `SE0104` | 1 | Unrecognised escape sequence |
| `SE0105` | 1 | Unexpected character (`>` outside `-->`) |
| `SE0106` | 1 | Malformed number literal |
| `SE0110` | 1 | Unterminated verbatim region (unbalanced braces) |
| `SE0111` | 1 | Unterminated raw string literal in a verbatim region |
| `SE0112` | 1 | Unterminated `native` type text (no `;`) |
| `SE0201` | 2 | Expected one token; found another |
| `SE0202` | 2 | Missing `package` declaration |
| `SE0203` | 2 | `use` declaration after a definition |
| `SE0204` | 2 | Expected a definition (`node` or `type`) |
| `SE0205` | 2 | Expected a unit annotation |
| `SE0206` | 2 | Malformed unit expression |
| `SE0207` | 2 | Unknown scalar type |
| `SE0208` | 2 | Expected `;` |
| `SE0209` | 2 | Unbalanced braces at end of file |
| `SE0210` | 2 | Wildcard import is not supported |
| `SE0211` | 2 | Expected a wire, an instance, or `}` in `structure` |
| `SE0212` | 2 | Wire endpoint must be `self.<port>` or `<instance>.<port>` |
| `SE0213` | 2 | Expected `-->` |
| `SE0220` | 2 | Duplicate section in a node |
| `SE0221` | 2 | Unknown section name |
| `SE0230` | 2 | State kind (`continuous`/`discrete`) omitted |
| `SE0231` | 2 | A setting may not be a record type |
| `SE0232` | 2 | A state may not be a record type |
| `SE0233` | 2 | A `native` member may not carry a unit |
| `SE0240` | 2 | Helper function has no body |
| `SE0241` | 2 | `init` takes no parameters |
| `SE0242` | 2 | `final` takes at most one parameter |
| `SE0250` | 2 | Unknown platform atom in `when` |
| `SE0251` | 2 | Unknown build primitive |
| `SE0260` | 2 | Duplicate entry in `sim` |
| `SE0261` | 2 | Unknown entry in `sim` |
| `SE0262` | 2 | Missing required `sim` entry |
| `SE0263` | 2 | `sync` requires `mode: realtime` |
| `SE0301` | 3 | `package` declaration disagrees with the file's location |
| `SE0302` | 3 | No public declaration matching the file name |
| `SE0303` | 3 | Imported name collides |
| `SE0304` | 3 | Unresolved name |
| `SE0305` | 3 | Package path resolves in more than one root |
| `SE0310` | 3 | Cycle in record field types |
| `SE0311` | 3 | Field-level wire endpoints are not supported |
| `SE0320` | 3 | Node has both code and a `structure` block |
| `SE0330` | 3 | Duplicate member name in a node |
| `SE0331` | 3 | Member name shadows a generated accessor |
| `SE0340` | 3 | `outputs` declared but no `output()` |
| `SE0341` | 3 | Continuous state declared but no `rates()` |
| `SE0342` | 3 | Discrete state declared but no `on_step()` |
| `SE0343` | 3 | `rates()` in a node with no continuous states |
| `SE0344` | 3 | `output()` in a node with no outputs |
| `SE0350` | 3 | Method parameter is not a declared input |
| `SE0351` | 3 | Duplicate method parameter |
| `SE0402` | 4 | Lossy literal conversion to an integer target |
| `SE0410` | 4 | Incompatible units |
| `SE0411` | 4 | Offset unit in a compound unit expression |
| `SE0412` | 4 | Builtin function argument has the wrong dimension |
| `SE0413` | 4 | Non-finite or domain-error result at elaboration |
| `SE0414` | 4 | Unknown unit symbol |
| `SE0420` | 4 | Required setting not bound |
| `SE0421` | 4 | Cycle among derived settings |
| `SE0422` | 4 | Reserved setting `rate` has the wrong unit or type |
| `SE0430` | 4 | Unconnected input |
| `SE0431` | 4 | A `continuous` state must be `double` |
| `SE0440` | 4 | Cross-instance reference in a setting binding |
| `SE0441` | 4 | Unknown or duplicated setting binding |
| `SE0442` | 4 | Fan-in: two sources into one input |
| `SE0443` | 4 | Wire direction is invalid |
| `SE0450` | 4 | Rate is not an integer divisor of the base rate |
| `SE0451` | 4 | `sync` is not an integer multiple of `step` |
| `SE0460` | 4 | Override target is ambiguous, or is an output |
| `SE0461` | 4 | Recorded signal path is ambiguous |
| `SE0510` | 5 | Algebraic loop |
| `SE0511` | 5 | Node is unreachable from the root |

---

## 17. Grammar summary

EBNF for all three file kinds. `{ x }` is zero or more, `[ x ]` optional, `|`
alternation. Terminals are quoted. `VERBATIM_BLOCK`, `VERBATIM_SIGNATURE`, and
`VERBATIM_TO_SEMI` are the scanned regions of §11.

```ebnf
(* ─── model file ─────────────────────────────────────────────────────── *)

model_file      = package_decl { use_decl } { definition } EOF ;

package_decl    = "package" qualified_name ";" ;
use_decl        = "use" qualified_name ";" ;
qualified_name  = IDENT { "." IDENT } ;

definition      = type_def | node_def ;

(* ─── record types ───────────────────────────────────────────────────── *)

type_def        = "type" IDENT "{" { field_decl } "}" ;
field_decl      = IDENT unit ":" scalar_type ";"
                | IDENT ":" qualified_name ";" ;

(* ─── nodes ──────────────────────────────────────────────────────────── *)

node_def        = "node" IDENT "{" { node_item } "}" ;

node_item       = settings_sec | inputs_sec | outputs_sec | states_sec
                | vars_sec | native_sec | declarations_sec | build_sec
                | structure_sec
                | lifecycle_method
                | helper_function ;

settings_sec    = "settings" "{" { setting_decl } "}" ;
setting_decl    = IDENT unit ":" scalar_type [ "=" expression ] ";" ;

inputs_sec      = "inputs"  "{" { port_decl } "}" ;
outputs_sec     = "outputs" "{" { port_decl } "}" ;
port_decl       = IDENT unit ":" scalar_type ";"
                | IDENT ":" qualified_name ";" ;

states_sec      = "states" "{" { state_decl } "}" ;
state_decl      = state_kind IDENT unit ":" scalar_type [ "=" expression ] ";" ;
state_kind      = "continuous" | "discrete" ;

vars_sec        = "vars" "{" { port_decl } "}" ;

native_sec      = "native" "{" { native_decl } "}" ;
native_decl     = IDENT ":" VERBATIM_TO_SEMI ";" ;

declarations_sec= "declarations" VERBATIM_BLOCK ;

build_sec       = "build" "{" { build_stmt } "}" ;
build_stmt      = build_primitive STRING [ "when" IDENT ] ";" ;
build_primitive = "link" | "include_dir" | "define" | "cflag" ;

(* ─── structure ──────────────────────────────────────────────────────── *)

structure_sec   = "structure" "{" { structure_item } "}" ;
structure_item  = instance_decl | wire_stmt ;

instance_decl   = "node" IDENT ":" qualified_name
                  [ "{" { setting_binding } "}" ] ";" ;
setting_binding = IDENT "=" expression ";" ;

wire_stmt       = endpoint "-->" endpoint { "," endpoint } ";" ;
endpoint        = endpoint_head "." IDENT { "." IDENT } ;
endpoint_head   = "self" | IDENT ;

(* ─── methods ────────────────────────────────────────────────────────── *)

lifecycle_method= "init"    "(" ")"                VERBATIM_BLOCK
                | "final"   "(" [ IDENT ] ")"      VERBATIM_BLOCK
                | "output"  "(" [ ident_list ] ")" VERBATIM_BLOCK
                | "rates"   "(" [ ident_list ] ")" VERBATIM_BLOCK
                | "on_step" "(" [ ident_list ] ")" VERBATIM_BLOCK ;
ident_list      = IDENT { "," IDENT } ;

(* A helper is any other item: verbatim C++ up to its opening brace,
   then a brace-matched body. See §11.4. *)
helper_function = VERBATIM_SIGNATURE VERBATIM_BLOCK ;

(* ─── units ──────────────────────────────────────────────────────────── *)

unit            = "(" unit_expr ")" ;
unit_expr       = "-" | unit_term { ( "*" | "/" ) unit_term } ;
unit_term       = unit_factor [ "^" unit_exponent ] ;
unit_factor     = "1" | unit_symbol | "(" unit_expr ")" ;
unit_symbol     = IDENT | "%" ;
unit_exponent   = [ "-" ] INT | "(" [ "-" ] INT [ "/" INT ] ")" ;

scalar_type     = "double" | "float" | "int" | "bool" ;

(* ─── expressions ────────────────────────────────────────────────────── *)

expression      = additive ;
additive        = multiplicative { ( "+" | "-" ) multiplicative } ;
multiplicative  = unary { ( "*" | "/" ) unary } ;
unary           = ( "+" | "-" ) unary | power ;
power           = primary [ "^" unary ] ;
primary         = NUMBER [ unit ]
                | "param" "." IDENT
                | IDENT "(" [ expression { "," expression } ] ")"
                | IDENT
                | "(" expression ")" ;

(* ─── sim file ───────────────────────────────────────────────────────── *)

sim_file        = "sim" IDENT "{" { sim_item } "}" EOF ;

sim_item        = root_entry | scalar_entry | record_entry | log_entry ;

root_entry      = "root" ":" qualified_name
                  [ "{" { setting_binding } "}" ] ";" ;
scalar_entry    = IDENT ":" ( expression | IDENT | STRING ) ";" ;

record_entry    = "record" ":" "{" { record_item } "}" ;
record_item     = "file"  ":" STRING ";"
                | "every" ":" NUMBER ";"
                | "signals" "{" { signal_path ";" } "}" ;
signal_path     = IDENT { "." IDENT } ;

log_entry       = "log" ":" "{" { log_item } "}" ;
log_item        = "file"  ":" STRING ";"
                | "level" ":" IDENT ";"
                | "levels" "{" { signal_path ":" IDENT ";" } "}" ;

(* ─── settings source ────────────────────────────────────────────────── *)

settings_file   = { override_binding } EOF ;
override_binding= signal_path "=" expression ";" ;
```

Two places where the grammar is deliberately looser than the semantics, so that a
future decision is not a grammar change: `endpoint` accepts a multi-segment path
(§5.3), and `scalar_entry` accepts any key, so an unknown sim-file key becomes a
good semantic diagnostic rather than a parse failure.

---

## Appendix A — Unit symbols

An implementation must accept these. Symbol resolution is **exact match first,
then longest prefix plus exact unit**, so `min` is the minute (never
milli-something) and `Pa` is the pascal (never peta-`a`).

**Base:** `m` `kg` `s` `A` `K` `mol` `cd`

**Derived:** `rad` `sr` `Hz` `N` `Pa` `J` `W` `C` `V` `F` `ohm` `S` `Wb` `T` `H`
`lm` `lx` `Bq` `Gy` `Sv` `kat`

**Accepted non-SI:** `g` `deg` `min` `h` `d` `L` `t` `bar` `atm` `rpm` `%`
`degC` `degF` `in` `ft` `mi` `lb` `hp` `psi`

**Prefixes:** `y` `z` `a` `f` `p` `n` `u` `m` `c` `d` `da` `h` `k` `M` `G` `T`
`P` `E` `Z` `Y`

The dimensionless unit is spelled `(-)`; `(1)` is equivalent. `(%)` is
dimensionless with scale 1/100. Note that `kg` is the base unit but the prefix
attaches to `g`, so `(mg)` is a milligram, as expected.

An unknown symbol is `SE0414`, and the diagnostic must suggest near matches —
`(sec)` should point at `(s)`.

## Appendix B — Reserved words

```
build       continuous  declarations  discrete  final   init      inputs
native      node        on_step       output    outputs package   rates
self        settings    sim           states    structure  type   use
vars
```

`param`, `in`, `out`, `state`, `der`, `next`, `var`, `log`, and `sim` are
additionally unavailable as member names (§6.11), because the generated code puts
objects by those names in scope of bodies.

Everything else — including every sim-file key, every build primitive, `when`, and
every platform atom — is contextual and remains usable as an identifier.

## Appendix C — Known gaps and open issues

Recorded so that their absence is visibly deliberate.

**Design questions genuinely open:**

1. **`(deg)` versus `(rad)`** (§4.5). Dimensionally identical, so dimensional
   analysis cannot catch the mix-up, and it is a common expensive bug. Treating
   angle as a distinct dimension would catch it but is unphysical.
2. **Read-only `native` access in `output()`** (§6.7). A preloaded lookup table or
   interpolation map is genuinely pure and genuinely wants to be read there, but
   the `init`/`on_step`/`final` confinement excludes it. A real need with no
   mechanism yet.
3. **Field-level wires** (§5.3) — `whl.ws.speed --> tc.u`. The grammar admits it;
   the semantics currently reject it.
4. **Platform atom vocabulary** (§12.2) — the exact set, and how extensible it is.
5. **The fixed-step solver menu** (§9.5) beyond `euler`/`rk2`/`rk4`.
6. **`name:` display metadata** — an optional human-readable label distinct from
   the type identifier. Never ruled on; currently absent.
7. **Log file format** (§13.7) — plain text versus structured JSONL.

**Deliberately deferred, each a pure extension:**

8. **Algebraic loop solving** (tearing plus Newton, §8.6) — deferred, not
   rejected. Adding it is a relaxation, and the SCC analysis is already needed for
   the diagnostic.
9. **Declarative `stop_when:`** in the sim file — needs run-time signals in the
   §7 expression language, which is a much larger change than it appears. Add it
   only when experiment-level, model-author-oblivious stop conditions are genuinely
   needed.
10. **Chronic-overrun termination** (§9.4) — offered and explicitly not taken;
    overrun is instrumented, not policed. Would route through `sim.abort` →
    `final(ctx)` and would want a new `ctx.reason` value rather than overloading
    `aborted`, which means "a node called `sim.abort`".
11. **Recording wildcards** (§13.5) — `plant.out.*`, or `**` to recurse.
12. **`output(*)`** — "reads every input", for wide `Sum`-style nodes. Tangles with
    variadic ports, which is its own unopened topic.
13. **Self-pulling dependencies** (§12) — a package able to fetch its own source.
    Composes cleanly with the unity build: pull source, `#include` it into the
    single translation unit, no separate link step.
14. **Real constructors and destructors on leaves** instead of `init()`/`final()`.
15. **Downsample aliasing warning** — a fast→slow connection inserts no
    anti-alias filter; the engine could warn.
16. **First-order hold** on upsample instead of ZOH. Rare in this domain.

**Implementation hazards, called out because they fail silently:**

17. **`#line` accounting for injected `native` alias lines** (§11.2). Get the
    offset wrong and every C++ error in that method points a line or two off.
    Two concrete cases already observed in `examples/drivetrain/`:
    - Editing a `.se` body shifts every later body, so hand-maintained `#line`
      values rot silently. Nothing detects it; only a real diagnostic reveals it.
    - A `#line` block set for one body stays in effect over the **generated**
      code that follows it, so a warning about a generated *signature* — an
      unreferenced `in` on an empty `In_output`, say — is attributed to the
      previous method's body. The generator must reset `#line` (to the node's
      declaration line, or to the generated file itself) after each verbatim
      region, not only before one.
18. **Digit separators and raw strings** in verbatim scanning (§11.2).
