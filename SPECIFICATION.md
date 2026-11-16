# The sim-engine Model Language

**Specification, version 0.1 (draft) — 2026-07-27**

A plain-text language for describing hierarchical, node-based simulation models,
lowered ahead of time to dependency-free C++20.

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
| Model file | `.se` | Exactly one public `node`, `type` or `finite` declaration, plus optional file-private helpers. |
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
  global solver, and every node runs every tick. A "continuous" node is a node
  that happens to have integrated states.
- **I5 — Generated identifiers are the generator's business.** Users write only
  model paths. Nothing at the language level constrains emitted names.

### 1.3 Conformance and stage boundaries

A conforming implementation processes a program in these stages. This document
marks rules with the earliest stage that can detect a violation.

| Stage | Name | Detects |
|---|---|---|
| 1 | **Lexical** | Bad characters, unterminated literals/comments, unbalanced verbatim regions. |
| 2 | **Syntactic** | Grammar violations, misplaced sections, duplicate sections. |
| 3 | **Resolution** | Unknown names, file/name mismatch, root collisions, duplicate members. |
| 4 | **Elaboration** | Unit mismatches, required-setting omissions, setting cycles, non-divisor rates, instantiation. |
| 5 | **Scheduling** | Feedthrough sort, algebraic loops. |
| 6 | **Emission** | Code, unit manifest, slot map, build script. A model with array-shaped states (§6.4a) emits its state slot map's *declarations* here and its concrete rows at configuration (§15.6). |

All six stages are implemented in `src/`, one source file per stage from stage 3
on. Where this document says *"is an error"* without qualification, the stage
column in §16.4 gives the detecting stage.

### 1.4 Terminology

- **Definition** — a `node`, `type` or `finite` declaration. Lowers to a C++
  class, struct or scoped enumeration.
- **Instance** — a `node name : Definition { … };` statement inside a
  `structure` block. Lowers to an object (leaf) or to nothing (composite).
- **Leaf** — a node with code. **Composite** — a node with children and wires.
- **Port** — an entry in `inputs` or `outputs`.
- **Elaboration** — the compile-time evaluation that resolves the hierarchy,
  checks every declarative site, and produces the flat runtime model.
  Elaboration fixes **shape**; it does not fix setting **values** (§6.2).
- **Configuration** — the runtime phase between construction and `init()`, in
  which setting overrides are applied and derived settings and flow-down are
  recomputed (§6.2). Elaboration decides what *may* be configured;
  configuration decides what it *is*.
- **Base step** — the global fixed time step. **Tick** — one base step.
- **Frame** — the ticks a host runs between two `sync_realtime()` calls when it
  paces a run against wall time (§9.4). The host chooses it; the model does not
  know it.
- **Sample instant** — a tick on which a given node is active.

---

## 2. Program structure

### 2.1 Files, roots, and packages

A **root** is a directory declared to the compiler from outside the language
(command line or roots file), exactly like a C++ include directory. There is no
in-language root declaration.

Roots come from three places, in this order: `-I` on the command line; a **roots
file**, `se.roots`, found by walking up from the input file's directory, whose
lines are root paths relative to the file (`#` starts a comment); and the
**toolchain's standard library**, `<dir>/../stdlib` where `<dir>` holds the `sec`
executable, used when it exists. There is no environment variable: the roots
file is checked in beside the model and the standard library travels with the
toolchain, so a checkout plus a pinned `sec` builds the same model anywhere, CI
included. `--no-default-roots` turns off both the roots file and the standard
library. A root reached two ways (say `-I stdlib` and the toolchain's copy)
counts once. With no `-I` and no roots file, the current directory is a root.

Within a root, **the directory path is the package path**, and the package path
is the C++ namespace path:

```
<root>/drivetrain/Wheel.se        ->  package drivetrain, node drivetrain.Wheel
<root>/se/mech/Damper.se          ->  package se.mech, node se.mech.Damper
```

The **standard library is one root** shipped with the toolchain, found beside the
toolchain rather than declared, but resolved exactly as any other root, with no
other special status: `std` is an ordinary package name, not reserved. Its "version" is
whatever is on disk at that path; pinning is delegated to whatever already
manages filesystem contents (vendoring, a submodule), not to a DSL-native
package-version resolver.

**Standard-library naming.** Within the shipped library, **package segments are
abbreviated and declaration names are spelled out** — `se.sig.dt.Biquad`, not
`se.signal.discrete.Biquad`. A package path is paid at every use site and it
stacks, so the segments stay short; a declaration is read far more often than it
is typed, so it stays whole. This is a convention for the library's own contents,
not a rule the compiler enforces on anyone's packages.

The library uses the second level to split blocks by **time character**, which is
the property that decides whether a node's states join the solver's vector or
advance once per tick:

```
<root>/se/sig/ScaleLimit.se       ->  se.sig.ScaleLimit      memoryless
<root>/se/sig/ct/Biquad.se        ->  se.sig.ct.Biquad       continuous
<root>/se/sig/dt/Biquad.se        ->  se.sig.dt.Biquad       discrete
<root>/se/sig/src/Sine.se         ->  se.sig.src.Sine        source
```

Two realizations of one transfer function therefore share a name and differ by
one segment. Note also that `continuous` and `discrete` are reserved words
(Appendix B) and a package path takes identifiers only, so those two segments
could not be spelled out even if the convention allowed it.

`se.sig.src` is a **deliberate exception**, and the only one: it groups signal
*sources* by what they are rather than by time character, and every block in it
is discrete. A source has no input, and the property that matters at its use site is which
waveform it produces, not how it stores its phase. The cost is real and is recorded here
rather than hidden: a source hands a continuous consumer a zero-order-held
staircase one base step wide, so RK4 is first-order across that path. A
`se.sig.src.ct` sibling — the same waveforms with `der.` on a continuous phase,
exact at every minor step — is a pure extension if that cost ever bites.

**Root collisions.** A package path that resolves in more than one root is an
error (`SE0305`). A root may be marked `override` in the roots file, which permits
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
the file's base name. Any further `node`, `type` or `finite` declarations in the same file
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
use se.mech.Damper;      // per-symbol: bare `Damper` in this file
use se.mech;             // package alias: `mech.Damper` in this file
```

- **Per-symbol import** binds the last segment as a bare name.
- **Package alias import** binds the last segment as a package alias; members
  are then named `alias.Member`.
- **No wildcard imports.** `use se.mech.*` is rejected (`SE0210`) so that every
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
declares no node, type or finite type; it may contain only a `package` declaration, `use`
declarations, a `build` block (§12), and a `declarations` block (§6.6). Its
purpose is to let a package act as a **dependency bundle** — a named collection
of build primitives, and of C++ shared by the package's nodes, that any model
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

**A manifest's `declarations` block is the package's shared C++.** A helper class
two nodes of one package both need — a socket wrapper, a protocol codec — cannot
live in either node's own `declarations` (§6.6): each block is guarded by its
node, so two copies of one class would both land in the model's single
translation unit and collide. The manifest's block is guarded by the *package*
instead, and is emitted once, ahead of every node's `declarations`, in any model
that loads the package. That is the same trigger as the manifest's build
primitives — the manifest is read when any file of its package is — so a
package's link flags and its shared C++ always arrive together. A file-level
`declarations` block anywhere but a `package.se` is `SE0322`.

```
// <root>/se/net/package.se
package se.net;

build { link "ws2_32" when windows; }

declarations {
    #include <cstdint>
    namespace se_net_detail { class UdpSocket { /* … */ }; }
}
```

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

**Doc comments.** A line comment that begins with exactly three slashes, `///`,
is a *doc comment*; `////` and longer are ordinary comments. Doc comments have no
effect on a model — every stage but `sec --doc` (§6.14) skips them exactly as it
skips `//` — and they attach to declarations by position:

```
/// Follows its input through a first-order lag.     <- documents `node Lag`
node Lag {
    settings {
        tau: double(s) = 0.5;   /// time constant      <- documents `tau`
    }
}
```

- A run of `///` lines on consecutive lines documents the declaration that
  begins on the line immediately after the run.
- A `///` that follows other text on its line documents the declaration that
  begins on that line.
- A declaration that carries annotations (§6.15) **begins at its first
  annotation**, so its doc block goes above the `@`. A `///` between an
  annotation and the keyword it leads documents nothing (`SE0701`).
- The declarations that may be documented are `node`, `type`, unit parameters,
  settings, inputs, outputs, states, `vars`, `native` members, record fields,
  and a composite's `group`s (§6.9.3). A doc comment that documents none of
  them is `SE0701`; one whose line begins more than one is `SE0703`.

A `///` inside a verbatim region is C++ text and is never a doc comment.

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
settings units inputs outputs states vars native structure declarations build
continuous discrete
init output derivative next final
sim
```

Everything else that reads like a keyword in this document is **contextual** —
recognised by position, and usable as an ordinary identifier elsewhere. That
includes:

- the build primitives `link`, `include_dir`, `define`, `cflag`, the tag keyword
  `when`, and the platform atoms — contextual inside `build`;
- every sim-file key (`root`, `step`, `solver`, `duration`, `settings`,
  `record`, `log`, `file`, `every`, `level`, `levels`, and `clear` inside a
  record body) — contextual inside `sim`, and `record`, `log` and their bodies'
  keys likewise inside a settings source (§14);
- annotation names and their keys (`diagram`, `collapsed`, `expanded`) —
  recognised only after an `@` (§6.15);
- the type name `buffer` — recognised as a one-segment type, as the scalar
  type names are (§5.1a);
- the accessor prefixes (`param`, `in`, `out`, `state`, `der`, `next`,
  `var`) and the builtin namespaces (`log`, `sim`) — these live in
  **C++ scope inside verbatim bodies**, not in the DSL's token stream, so they
  are not DSL keywords at all. They are, however, unavailable as member names (§6.11).

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

The same literal, with exactly these escapes, is how a string value (§5.1b) is
written in a settings source (§14) and in a dump (§14.1). A compiled model
reads and writes the same set, so text with a quote, a backslash or a line
break round-trips through a dump.

### 3.7 Punctuation and operators

```
{  }  (  )  ;  ,  :  .  =  -->  +  -  *  /  ^  %  @
```

`-->` is a single token. The lexer is maximal-munch, so `-->` always wins over
`-` `-` `>`; a lone `>` is not a token in this language and is an error
(`SE0105`) outside verbatim regions. `%` occurs only as a unit symbol (§4.1),
and `@` only to open an annotation (§6.15).

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

`(deg)` and `(rpm)` are well-formed unit expressions, but they may appear only as
a literal suffix or a display unit — never as a *declared* unit. See §4.5.

**Where a unit may appear.** Two roles, and one shape for each:

| Role | Sites (§17) | Form | Accepts |
|---|---|---|---|
| **Argument** binding a `units { }` parameter | `type_ref` on a `setting`, port, state, var or record field; `unit_args` on an instance | `Path(u1, u2, …)` | zero, one or N, per the target's `units` section |
| **Suffix** on a numeric literal | `primary` | `315 (mm)` | the full unit language, `deg` and `rpm` included |

`units { U; }` is the third site and the only one containing no unit expression:
it *declares* the parameters the argument role binds.

**A scalar type is unit-parametric like any other.** `double`, `float` and `int`
each take exactly one unit argument; `bool` takes none, having no dimension to
carry. So `limit: double(m/s)` and `force: math.Vec3(N)` are the same
construct — a type reference with its units bound at the reference — and the
declaration grammar has one production rather than a scalar shape and a record
shape (§5.1).

That uniformity is not only cosmetic. **Compatible units convert at every wire**,
whatever the type: `(mm)` into `(m)` folds a factor of 1000 at elaboration
(§4.2), and `Vec3(mm)` into `Vec3(m)` folds the same factor into each field. The
one thing that never converts is a *dimension* — `(m)` into `(m/s)` is `SE0410`,
and so is `Vec3(m)` into `Vec3(m/s)`, reported once against the offending
argument rather than once per field it reaches.

The two roles still differ in what they accept, which is the last column: a
declared unit may not name `deg`, `rev` or `rpm` (§4.5), while a literal suffix
may.

### 4.2 Dimensions and equivalence

A unit expression evaluates to a **dimension** — a rational exponent vector over
**eight** base dimensions: the seven SI bases plus **angle** (§4.5) — together
with a **scale factor** and, for a few units, an **offset**. Two units are
*compatible* iff their dimension vectors are equal, except that the angle
exponent is ignored where length is a positive power (§4.5, *absorption into
length*): `(rad*m)` is compatible with `(m)`. `(-)`, `(1)`, and `(%)` are
all dimensionless; `(%)` carries scale 1/100.

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

1. A setting default expression against the setting's declared unit — per
   leaf, for a record setting (§6.2c).
2. A setting binding at instantiation against the child setting's unit, per leaf
   in the same way.
3. A state default against the state's unit.
4. A wire: the producing port's unit against the consuming port's — per field,
   for records, and per unit argument for a parametric record (§5.2).
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

### 4.5 Angle

**Angle is a distinct base dimension** — the eighth — and **radians are the only
declarable angle unit**. These are two separate rules fixing two separate
confusions, and both are needed.

**The dimension.** Without it, `(deg)` and `(-)` have equal dimension vectors, so
wiring a heading into a slip-ratio port is legal and silently scales by π/180.
With it:

| Unit | Dimension |
|---|---|
| `rad` | 1·angle |
| `deg` | (π/180)·angle |
| `rev` | 2π·angle |
| `rpm` | (2π/60)·angle·T⁻¹ |
| `sr` | angle² |

**Canonical radian storage.** Units are erased in bodies (§4.4), so `sin(state.theta)`
on a `(deg)` accessor is silently wrong and *no* dimensional system catches it.
Therefore **a declared unit may not contain a non-radian angle unit**: `(deg)`,
`(rpm)`, and `(deg/s)` are legal as literal suffixes and as display units, and
illegal as the declared unit of a setting, state, port, or field (`SE0415`).
Writing `theta: double(rad) = 90 (deg);` is correct and converts at elaboration.
In §4.1's terms the restriction lands on the **argument** role alone; the literal
suffix and display roles keep the full unit language, which is exactly what makes
that line spellable.

**The larger win.** `(rad/s)` is angle·T⁻¹ while `(Hz)` stays T⁻¹, so they are no
longer compatible and the ω-versus-*f* factor-of-2π mix-up becomes a hard error.

**Absorption into length.** Where a unit's **length exponent is positive**, its
angle exponent — of either sign — is ignored when deciding compatibility:

| Unit | Compatible with | Why it arises |
|---|---|---|
| `rad*m/s` | `m/s` | `v = ω × r` |
| `rad*m/s^2` | `m/s^2` | `a = α × r` |
| `rad^2*m/s^2` | `m/s^2` | `a = ω × (ω × r)` |
| `kg*m^2*rad/s^2` | `N*m` | `τ = Iα` |

The justification is the radian's own definition — arc length over radius — so an
angle times a length *is* a length. Without the rule the dimension check gets
lever arms exactly backwards: the correct `ω·r` arrives as `rad·m/s` and is
refused by an `m/s` port, while the wrong `f·r` arrives as `m/s` and is accepted.
It is what lets `math.Cross3(rad/s, m)` feed a velocity port, so lever-arm and
rolling-contact identities can be wired from generic blocks instead of being
written as dedicated leaves.

The rule is deliberately narrow, and everything §4.5 exists to catch survives it:

- **No length, no absorption.** `(rad/s)` against `(Hz)` is still `SE0410`.
- **Negative length, no absorption.** `(rad/m)` against `(1/m)` is still `SE0410`
  — a wavenumber or a curvature has the same 2π hazard as a frequency.
- **One-way.** An angle is absorbed into a length, never into `(-)`: `(rad)`
  against `(-)` is still `SE0410`, so a heading wired into a slip-ratio port is
  still caught. The price is that `θ = s/r` from two `(m)` quantities is `(-)`
  and must be computed in a body or carried as `(rad*m)/(m)`.

**What it gives up.** A negative angle exponent is absorbed too, so a torsional
stiffness `(N*m/rad)` is compatible with a torque `(N*m)`, and a per-radian
quantity with length in it no longer stands apart from its per-unit form. That is
the same trade SI makes everywhere by treating the radian as 1; here it is made
only where a length is present.

It applies **at the compatibility check only**, after unit arguments are bound
(§6.2a) — the dimension vector itself keeps the angle. `(rad*m)/(m)` is therefore
still `(rad)`, the manifest (§15.6) still prints `rad·m/s^2` for a port declared
that way, and `rad` being scale 1 means the conversion across such a wire is the
identity. An `SE0410` whose two dimensions differ in angle alone carries a note
saying where angle does and does not cancel.

**Torque and energy.** With absorption, `τ = Iα` gives `kg·m²·rad/s²` and `W = τθ`
gives `N·m·rad`, and both are compatible with `(N*m)` — the disagreement that is
the genuine wart in the angle-as-dimension literature never reaches a wire.
Force, energy, and inertia units stay angle-free, and torque and energy remain
mutually convertible exactly as before. The rule does *not* separate torque from
energy; nothing here claims to.

---

## 5. Types

### 5.1 Scalar types

The scalar type set is closed:

| DSL type | C++ type | Notes |
|---|---|---|
| `double` | `double` | The only type permitted for a `continuous` state. |
| `float` | `float` | |
| `int` | `int` | |
| `bool` | `bool` | Takes no unit argument: a truth value has no dimension. |

**Every declaration has one shape**, `name: Type(units);`, whether `Type` is a
scalar or a record (§5.2):

```
gain:   double(-)     = 1.0;
limit:  double(m/s^2);
ticks:  int(-)        = 0;
ok:     bool;
force:  math.Vec3(N);
```

A scalar type is unit-parametric like any other type. `double`, `float` and `int`
declare exactly one unit parameter, so the argument is not optional and `(-)` is
how you spell dimensionless; omitting it is `SE0205` at parse time, not an
unnoticed dimensionless default. `bool` declares none, so `ok: bool(-)` is the
same arity error from the other side. Nothing about `bool` needs a rule of its
own — the arity check it shares with every other type is the whole of it.

The payoff is in §5.2 and §4.1: because a scalar and a record are the same
construct, **compatible units convert at every wire** rather than at scalar wires
only, and the language has one rule to state instead of a rule and an exception.

**A `state` or a `var` may carry an extent** — `name [n]: type(unit);` — making
it an array of that scalar type (§6.4, §6.5). The scalar type set is unchanged;
an extent is a property of the declaration, not a new type. Ports, settings,
record fields and natives may not carry one (`SE0234`).

Two builtin types are not on this list because neither is a scalar: `buffer`,
a run of raw bytes with a length (§5.1a), and `string`, bounded text (§5.1b).
A buffer is not an array either, and that is the point of making it its own
type.

### 5.1a Buffers

A **buffer** is opaque bytes plus a size. It exists for the values a model
carries without understanding them — a received datagram, a frame on its way to
a device — where the bytes mean something only to the C++ that encodes or
decodes them.

```
outputs { data:   buffer(1500); }   // storage: up to 1500 bytes
inputs  { packet: buffer; }         // a reader: any capacity
vars    { rx:     buffer(1500); }
```

**It is a buffer, not an array, and the language keeps the two apart.** An array
state (§6.4a) has elements the DSL knows about: each has the declared unit, an
index path (`bw.w1[3]`), an initial condition. A buffer has none of that. There
is no element type, no unit, no index, no element-wise conversion and no literal;
only C++ looks inside. Its argument is therefore not an extent in brackets but
the type's own argument in the slot every type has (§5.1), and it is a capacity
rather than a unit.

- **The capacity is an integer literal, and only storage states one.** A
  buffer that *holds* bytes — a leaf's output, a var — declares how many it can
  hold: `buffer(1500)`. A buffer that only *reads* or *passes through* — any
  input, either port of a composite — is written bare, `buffer`, exactly as
  `bool` is written with no unit. A missing capacity on storage is `SE0323`; a
  capacity on a reader is `SE0324`; a capacity that is not a positive integer
  literal is `SE0236`. There is no capacity setting: a capacity is part of the
  signal block's layout, which is fixed when the code is generated (§15.5), and
  a configuration-time override (§6.2b) could otherwise resize a struct that
  has already been compiled.
- **The value carries its size.** `0 <= size <= capacity`, and the size travels
  with the bytes on one wire, so the two cannot be wired apart. A zero-size
  buffer is a value like any other.
- **A reader accepts any capacity.** A `buffer` input wired from a
  `buffer(1500)` and from a `buffer(64)` is the same input. Nothing is ever
  truncated by a wire: a reader sees exactly the bytes the producer holds, and
  checks `size()` itself if it needs a minimum.
- **Where it may appear:** inputs, outputs and vars. Not a setting — a
  setting's value is a number with a dimension, an enumerator, or text
  (§5.1b), and a buffer is none of them — not a state, and not a record field
  (`SE0325`). A var buffer may not carry an extent
  (`SE0433`).
- **A wire carries a buffer only into a buffer.** A buffer into a scalar or a
  record port, or either into a buffer, is the port-type mismatch `SE0410`. A
  literal or a setting is a number, so neither may drive a buffer input
  (`SE0410`).
- **Not on the root.** The root's ports are the host's boundary (§15.6), and a
  buffer there has no settled shape for a host to read or write yet. A buffer
  port on the root node is `SE0445`; see Appendix C.
- **`buffer` is contextual**, recognised as a one-segment type name exactly as
  `double` is, and usable as an ordinary identifier everywhere else.

**In C++** a storage buffer is `se_rt::buffer<N>` and a reader is
`se_rt::buffer_view`. Both have the same three members, so a body reads
`in.packet.data()`, `in.packet.size()`, `in.packet.capacity()` and
`var.rx.size()` the same way:

```cpp
template <std::size_t N> struct buffer {
    std::int32_t size_;          // first, so a reader of the signal block can
    std::uint8_t data_[N];       // find the size without knowing N
    const std::uint8_t* data() const;  std::uint8_t* data();
    std::size_t size() const;          static constexpr std::size_t capacity();
    void resize(std::size_t n);        // clamped to N
    bool assign(const void* p, std::size_t n);   // false if it had to truncate
    bool assign(const buffer_view& v);
};
struct buffer_view {             // pointer, size, capacity — never owns
    const std::uint8_t* data() const;  std::size_t size() const;
    std::size_t capacity() const;
};
```

Copying one `buffer<N>` to another copies `size()` bytes, not `N`. A
`buffer<N>` converts to a `buffer_view` implicitly, which is the whole of how a
wire reaches a reader (§15.3). `assign` is the only way to put a view's bytes
into storage, and it says when it truncates rather than doing so quietly.

### 5.1b Strings

A **string** is bounded text: up to a fixed number of bytes, plus a size. It
exists for the text a model carries or is configured with — a host to send to,
a label, a status line, a command name — where a number would be a code a
person has to look up.

```
settings { host:   string(64) = "127.0.0.1"; }   // storage: up to 64 bytes
inputs   { cmd:    string; }                    // a reader: any capacity
outputs  { status: string(32); }
vars     { line:   string(80); }
```

**It follows the buffer (§5.1a) wherever the two can agree.**
- **Only storage states a capacity.** A string that *holds* text — a setting,
  a leaf's output, a var — declares how many bytes it can hold:
  `string(64)`. A string that only *reads* or *passes through* — an input,
  either port of a composite — is written bare, `string`, and accepts any
  capacity. A missing capacity on storage is `SE0323`, a capacity on a reader
  `SE0324`, and a capacity that is not a positive integer literal `SE0236`.
  The capacity is part of the signal block's layout, fixed when the code is
  generated (§15.5), so no setting can change it.
- **The capacity counts bytes.** Text is UTF-8, so a character outside ASCII
  takes more than one. The value carries its size, `0 <= size <= capacity`,
  and an empty string is a value like any other.
- **Nothing allocates.** Storage is a fixed array, so writing a string during
  a run never touches the heap.
- **A wire carries a string only into a string**, of any capacity. A string
  into a number, a record or a buffer, or any of those into a string, is
  `SE0410`.
- **`string` is contextual**, recognised as a one-segment type name as
  `buffer` is, and usable as an ordinary identifier everywhere else.

**Where it differs from a buffer.**
- **It may be a setting.** A string setting's value is a literal or a
  `param.` reference to a parent's string setting, and nothing else (§7.2b).
  `sec` checks that it fits: a literal longer than the capacity, or a
  `string(M)` setting flowing into a `string(N)` one with M > N, is `SE0427`.
  At configuration an over-long value is refused as the source is loaded
  (§14.1).
- **A literal or a string setting may drive a string input**, as a number
  drives a numeric input: `"stop" --> ctl.cmd;` or `param.host --> tx.host;`.
- **It may be on the root.** On the root an input is the host's storage, not
  a reader, so a root string input states its capacity, `cmd: string(16)`. A
  bare one is `SE0326`, and a capacity on any input that is not the root's is
  still `SE0324`. The host reaches root strings through the root structs
  (§15.5) and the C interface's text calls (§14.2).
- **It is recorded as its text** (§13.5), not as its size.

**Not a state and not a record field** (`SE0325`). A string is configuration
or a message, not the evolving condition of a system; text a node keeps from
one step to the next lives in a var. Keeping it out of records keeps a record
a bundle of numbers that converts field by field (§4.1).

**Text too long for its destination is cut, and the run is told once.** `sec`
checks every capacity it can see, but C++ in a body may write anything. A
write longer than the capacity keeps the longest prefix that ends on a whole
UTF-8 character, so a cut never leaves half a character behind. After
`init()` and after each tick, the engine looks at every string output and
var, and the first time one has been cut it logs one `warn` line naming it,
stamped with that tick's time:
`tx.out.status: 61 bytes into string(32), truncated`. Later cuts of the same
destination are silent, the run carries on, and logging never halts a run (§10.1).
Text from the host is never cut: a host value that doesn't fit is refused
(§13.8, §14.1, §14.2).

**In C++** a storage string is `se_rt::string<N>` and a reader is
`std::string_view`:

```cpp
template <std::size_t N> struct string {
    std::int32_t size_;          // first, as in buffer<N>
    char         data_[N + 1];   // always NUL-terminated, for C APIs
    std::int32_t wanted_;        // the length last asked for; over N after a cut
    const char* c_str() const;   std::size_t size() const;   bool empty() const;
    static constexpr std::size_t capacity();
    operator std::string_view() const;
    bool assign(std::string_view s);   // false if it had to cut
    string& operator=(std::string_view s);
};   // == and != compare text
```

A body therefore writes `sock.open(param.host.c_str())`, `out.status = "no
data";` and `if (in.cmd == "stop")` with the standard library alone. A
`string<N>` converts to a `std::string_view` implicitly, which is how a wire
reaches a reader, and a view is valid for the method call that receives it.

### 5.2 Record types

A `type` declaration defines a record that travels over a wire as one value:

```
// WheelState.se
package drivetrain;

type WheelState {
    speed:  double(rad/s);   // wheel angular velocity
    torque: double(N*m);     // net torque applied this step
    slip:   double(-);       // longitudinal slip ratio
}
```

- A record field line is **identical to a scalar port line**, because a scalar
  type is itself unit-parametric (§5.1): every declaration is
  `name: Type(units);` and only the name of the type differs.
- **A record may be parametric over its field units**, with the same `units { U; }`
  section a node uses (§6.2a):

  ```
  type Vec3 {
      units { U; }
      x: double(U);  y: double(U);  z: double(U);
  }
  ```

  One declaration then serves `Vec3(m)`, `Vec3(m/s)`, `Vec3(m/s^2)` and
  `Vec3(rad/s)`. **The binding site is every reference, not an instantiation** —
  a type is never instantiated — so the arguments ride the port declaration:
  `force: math.Vec3(N);`. Arity is checked there (`SE0313`).
- **A record converts field by field, exactly as a scalar port does.** Wiring
  `Vec3(mm)` into `Vec3(m)` is legal: the arguments are dimensionally compatible,
  so each field picks up the same factor of 1/1000, folded at elaboration and
  applied where the value is read into the consumer's view (§15.3). There is no
  scalar-only conversion rule to remember.
- **Dimensions still never convert.** `Vec3(m)` into `Vec3(m/s)` is `SE0410`,
  matching §6.2a's rule for differently-bound node instances and the plain `(m)`
  into `(m/s)` on a scalar port. One wrong argument is one mistake, so it is
  reported against the argument rather than against each field it reaches.
- **Arguments may be unit expressions over the referring node's own parameters.**
  Inside `units { P; Q; }` a port may read `c: math.Vec3(P*Q)`, which is what
  makes one `Cross3` cover every pair of dimensions instead of one per pair. Such
  an argument is itself parametric and is bound again when that node is
  instantiated.
- **A parametric field threads down through nesting.** In
  `type Pose { units { U; } p: Vec3(U); heading: double(rad); }` a reference to
  `Pose(m)` substitutes into `Vec3` as well, so `p.x` reports `(m)`.
- Fields may themselves be records; a cycle in the field graph is an error
  (`SE0310`).
- Records lower to a plain C++ struct with one member per field, each annotated
  with its unit as a comment and listed in the manifest's `[types]` section.
- Records may appear as `inputs`, `outputs`, `vars`, and `settings` (§6.2c).
  They may **not** be `states`: the solver ABI is a flat `double` vector.
  *This bullet used to bar settings as well, on the ground that the flow-down
  expression language is scalar. That ground still holds and §7 is unchanged —
  §6.2c does not make §7 record-valued, it splits a record setting into the
  scalar leaves it carries before §7 ever sees it.*

### 5.2a Finite types

A `finite` declaration at file level defines a **finite type**: a type whose
values are a fixed list of names, its **enumerators**.

```
// LinkState.se
package device;

finite LinkState {
    IDLE, CONNECTING, CONNECTED,
    FAULT = 15                    // the remote device's own code
}
```

It exists for the states a model keeps **about** something that is itself a
state machine — a remote device, a protocol, a controller's AUTO/MANUAL — where
an `int` with a comment beside each value is the incumbent, and where the
values are what §6.4's `finite` states range over.

**One keyword, two places, one idea.** At file level `finite` declares a finite
set of values; inside `states` it declares a state that ranges over one and
jumps between them (§6.4c). Position alone tells the parser which is meant, so
neither needs a second keyword. The word was chosen over `enum`, which would
have meant something narrower than the C++ `enum` written a few lines away in
the same file's verbatim bodies.

- **It is its own declaration**, a sibling of `node` and `type`, and obeys §2.3
  the same way: one public finite type per file, named for the file, with
  file-private ones beside it.
- **Values follow C.** An enumerator without `= n` takes one more than the one
  before it, and the first takes 0. `n` is an integer literal, optionally
  negative. Two enumerators may not share a name (`SE0317`) or a value
  (`SE0318`): a recorded column (§13.5) is decoded by value, and two names for
  one value would make the decoding ambiguous. A finite type with no
  enumerators is `SE0319`.
- **A finite type takes no unit argument**, for `bool`'s reason (§5.1): a name
  has no dimension. `link: LinkState;` is the declaration, and `LinkState(-)` is
  the arity error from the other side.
- **It goes wherever a scalar goes** — a setting, a port, a var, a record field, a
  `discrete` or a `finite` state (§6.4) — except a `continuous` state, which
  stays `double` (`SE0431`), and an array, whose element stays `double`
  (`SE0433`).
- **A wire carries a finite type only into the same type.** There is no unit, so
  there is nothing to convert, and there is no conversion to or from `int`: a
  `LinkState` into an `int` input is the same port-type mismatch as a record into
  a scalar.
- **In C++ it is a scoped enumeration**, `enum class LinkState : int`, in the
  package's namespace, and a body names its values the C++ way:
  `state.link == LinkState::CONNECTED`. A file-private finite type is named bare
  in every body of its file, as §2.3 says the rest of the file sees it; another
  package's is qualified as any C++ name is. Where the DSL names an enumerator
  itself — a default, a binding, an override, a test cell — the name is bare and
  its type comes from the site (§7.2a).

### 5.3 What a wire carries

A wire carries **the whole record**. Whether a wire may ever carry a single
field —

```
whl.ws.speed --> tc.u;     // reserved, not currently legal
```

— is deliberately still open. The grammar in §17 accepts a multi-segment endpoint
path so that the eventual decision is not a grammar change; stage 3 currently
rejects any endpoint with more than one segment after the instance (`SE0311`).

The endpoint grammar is loose in the other direction too, and for the same
reason: it accepts *fewer* segments than a wire needs, because §6.9.2's bare
endpoint is resolved by the named node's port arity and that is a question stage
3 answers (`SE0314`), not one the parser can.

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
    derivative(…) { … }
    next(…)       { … }
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
descendants'. That falls directly out of the design intent — a composite "just
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
    inertia: double(kg*m^2);                           // required
    radius: double(m) = 0.31;                    // default
    corner_mass: double(kg) = param.total_mass / 4.0;  // derived
}
```

- A setting with **no default is required**: every instantiation must bind it,
  checked statically at stage 4 (`SE0420`).
- A **string setting** (§5.1b) takes a string literal or a `param.` reference
  to another string setting, in its default and wherever it is bound (§7.2b).
- A default is an expression in the elaboration language (§7). It may reference
  **other settings of the same node** via `param.*`, which makes settings a
  dependency graph requiring a topological sort; a cycle is an error (`SE0421`).
- Derived settings behave the way a reader expects: overriding `total_mass`
  recomputes `corner_mass`; overriding `corner_mass` **pins** it — the derived
  expression no longer runs for that setting. This is the same rule at
  elaboration and at configuration.
- Settings are **values, not shape**. Their *names, units, types and dependency
  graph* are fixed at elaboration; their *values* are not. A setting may be
  overridden at **configuration** (§6.2b) in a binary that has already been
  compiled.

**What settings may not decide.** A setting may not select **wiring** or
**instance count**. Those two decide the emitted schedule — a flat sequence of
named calls in a fixed order (§15.7) — and varying them at run time would require
a container of nodes behind a uniform interface, which is the design §15.7
exists to avoid. §7.4's absence of conditionals and §7.2's requirement of a
literal `^` exponent already prevent most of this.

**State count is different, and a setting may decide it.** An **array extent**
(`states { discrete w [param.order] … }`) is evaluated at *configuration*
(§6.2b step 4), not at elaboration, so an extent-bearing setting is an
**ordinary setting** in every respect — including overridable in a compiled
binary. This is affordable precisely because it is *not* shape in the sense
above: a node with a dynamic state count is still one instance called from one
call site, and only the size of its slice of the state blocks moves. The
schedule, the signal block, and every wire are untouched.

**`structural` is reserved, not inferred.** Earlier drafts marked
extent-bearing settings structural — fixed at elaboration and refused on load.
Nothing in this version produces such a setting, because array *ports* do not
exist (§6.3) and nothing else lets a setting reach shape. The concept and the
manifest marker (§15.6) are kept for a feature that genuinely drives instance
count or wiring, should one ever be added; they are currently unreachable.

#### 6.2a `units` — unit parameters

A node may declare **unit variables**, and write its port, state and setting
units as expressions over them:

```
node Integrator {
    units    { U; }
    inputs   { x: double(U); }
    outputs  { y: double(U*s); }
}
```

`U` ranges over a **unit**, not a dimension. `(km/h)` and `(m/s)` are different
bindings that happen to share a dimension vector, and a node bound to one is not
interchangeable with a node bound to the other — the conversion between them is
exactly what §4.3 exists to insert.

Each unit variable is a bare identifier, `;`-terminated, in a `units` section.
Declaring one that shadows an Appendix A symbol is an error (`SE0416`); so is
declaring the same name twice (`SE0417`).

**The same section serves a `type` declaration** (§5.2), with one difference
that follows from what a type is. A node is *instantiated*, so it binds once, in
`structure`. A type is only ever *referenced*, so it binds at every reference —
on the port declaration, `v: math.Vec3(m/s)`. Everything else carries over
unchanged: the same expressions, the same per-binding identity, the same
`SE0416`/`SE0417`. Only the arity error differs, `SE0313` against `SE0419`,
because the two name different things in their message.

**Why the library needs this.** Without unit parameters a filter is written once
per dimension it might carry — five definitions for two concepts in the motion-
cueing algorithm alone — and remains closed to any dimension its author did not
anticipate. Declaring the ports dimensionless instead is worse, and *virally* so:
a chain of scale → high-pass → high-pass → integrate → integrate loses every
downstream check the moment its first output goes dimensionless.

**A unit parameter is an asserted invariant**, which is what makes it worth more
than a hole in the type. `U → U` asserts that a block preserves its input's
dimension; `U → U*s` asserts that it integrates; `U → V` asserts nothing between
them, but still pins both at instantiation and checks everything downstream.

##### Binding

Unit arguments are **positional**, in the order the `units` section declares
them, and are written **after the definition path**:

```
node acc : dt.Integrator (m/s^2) { };   // y is (m/s^2 * s) = (m/s)
```

Supplying the wrong number of arguments is an error (`SE0419`).

*Why not inside the brace list*, which is the more natural first instinct:
`U = (m/s^2)` is **indistinguishable from a setting binding**. `(m/s^2)` is a
valid §7 expression — `m` and `s` parse as named constants, `^2` as a power — so
it has the identical token shape to `mass = (total / 4.0)`. Telling them apart
requires knowing which names are unit parameters, which is resolution's job, not
the parser's. Moving the argument list left of the braces dissolves the ambiguity
completely: only `{`, `;` or `(` may follow a definition path, and there is no
expression context there, so a `(` in that position can only introduce a unit.
No lookahead, no backtracking, no trial parse.

##### Substitution

Binding happens at instantiation, and substitution reaches **every declarative
site the definition owns** — input and output units, state and derivative units,
and setting units. A parametric definition only ever knows `(U)`, so the
substituted unit is the only place a port's real unit exists; the wire check
(§15.3), the root boundary, and the manifest all read the substituted form.

Substitution is per instance. Two instances of one definition bound to different
units are **not interchangeable**, and wiring one into the other is an ordinary
`SE0410`:

```
node a : Integrator (m/s^2);
node b : Integrator (N);
a.y --> b.x;              // error: (m/s) where (N) is declared
```

A setting declared `(U)` converts per instance like any other declarative site: a
node bound `(m/s^2)` and given `36000 (km/h^2)` receives `2.7778`. A **record**
setting (§6.2c) does the same one field at a time, exactly as a record port
does: `limit: Vec3(U)` on a node bound `(m/s^2)`, given
`{ 5000.0 (mm/s^2), … }`, receives `5.0` in each field, each factor folded at
elaboration.

##### Flow-down to children

**A composite declares unit parameters exactly as a leaf does**, and for the
reason §6.1 gives: a node's kind is *inferred*, today's model is tomorrow's
subsystem with no edit, and nothing about a node's external surface may depend
on which kind it happens to be. A `units` section is external surface.

Inside a composite, a **child's unit argument may be an expression over the
composite's own parameters** — the same rule §5.2 already gives a record
reference:

```
node Biquad3 {
    units   { U; }
    inputs  { vin:  math.Vec3(U); }
    outputs { vout: math.Vec3(U); }
    structure {
        node sp: math.Split3(U) { };
        node x:  ct.Biquad(U)   { A = param.A; B = param.B; };
        …
    }
}
```

Such an argument is **bound again when the composite is instantiated**, and
composes through nesting to any depth: a composite bound `(m/s^2)` that passes
`U` to a child which passes it on in turn binds the innermost port to
`(m/s^2)`. Nothing else changes — an argument naming no parameter of the
enclosing node is an ordinary Appendix A symbol, so a typo is still `SE0414`,
and arity is still `SE0419`.

This is a second flow-down mechanism running parallel to §6.9.1's, and the two
are deliberately **not** unified: a setting flows a *value*, which §7 may
compute and §6.2b may override in a shipped binary; a unit parameter flows a
*unit*, which is checked and then erased before any value exists.

##### Checking a parametric composite

A leaf's `U → U` is an assertion the compiler **cannot** verify, because the
body is verbatim C++. A composite's interior is wires, so it can be — and a
contract that costs the same to declare should not be worth less on one kind of
node than the other.

**Today that interior is checked per instantiation**: `U` is substituted and the
wires are checked exactly as a non-parametric composite's are. The intended
strengthening is to check it **once at the definition**, with `U` left symbolic:
an unbound parameter compares only against itself, so `(U) --> (U)` passes,
`(U) --> (V)` and `(U)` into a child bound `(m)` do not, and no instance need
exist for either verdict. Until that lands, an interior that is correct for only
some bindings is diagnosed at the call sites that happen to use them rather than
where the mistake is.

##### Cost

None at run time. Units are erased at the C++ boundary (I3), so a parametric
leaf emits **one class regardless of how many ways it is instantiated** — the
parameter exists only to be checked, and is gone by stage 6. A parametric
composite emits nothing at all, having no runtime form to be parametric in
(§15.2). This is what makes unit polymorphism cheap enough to use everywhere in
a library, and it is why the feature needed no code generator changes at all.

#### 6.2b Configuration

Between construction and `init()`, a compiled model passes through
**configuration**:

1. Every setting holds the value elaboration computed for it.
2. Overrides are applied from the §14 settings source, each **pinning** its
   target.
3. Derived defaults (§6.2) and flow-down expressions (§6.9.1) are re-evaluated
   in dependency order, **skipping pinned settings**.
4. **Array extents (§6.4, §6.5) are evaluated**, each node reports its slot
   counts, and `Sim` sums them into the block sizes.
5. **The host queries those sizes, provides the state buffers, and `Sim` binds
   every state proxy to its slot** (§15.5). Initial conditions are applied.
6. `init()` runs.

Step 3 is what makes overriding `total_mass` recompute `corner_mass` in a
shipped binary, exactly as it does at elaboration. The §7 expressions are not
folded away; they are **emitted** (§15.5) and run here.

Steps 4 and 5 exist only because a state may be array-shaped. **A model with no
array states resolves both to constants at elaboration** and binds in its
constructor exactly as before — the dynamic path is opt-in per model, not a
blanket change to the ABI. Step 4 is also why an extent may read `param.*`
freely: it runs after step 3, so an extent sees overridden settings, and an
extent is therefore an ordinary consumer of the flow-down rather than a
privileged one.

**Sizes are settled before any state exists, and never move again.** Step 4
cannot read state, step 5 cannot run before step 4, and `init()` cannot resize
anything — by step 6 every buffer is bound and constant in extent for the
lifetime of the model. This is what keeps §9's timing story intact: no
allocation, no reshaping, and no branch on size inside the tick.

**Configuration happens once, before the first tick.** There is no two-phase
init and no way for a parent to reach into a child *during* a run: once `init()`
has run, settings are constant for the lifetime of the model. Nothing in this
document requires `init()` to be re-entrant.

Unit checking is **not** deferred to configuration. Every declarative site is
checked at elaboration (§4.3); an override supplied at configuration is checked
against the target's recorded dimension (§15.6) by the loader, which is the same
check applied to a value that arrives later.

**A configuration that fails does not start a run.** Four things can be wrong
here — a settings source that cannot be applied (§14.1: a path the model does
not have, a dimension that disagrees with §15.6, a right-hand side the loader
does not accept), an array extent that is not a non-negative integer, an element
path (`bw.w1[3]`) that is out of range for the extent step 4 computed, and a
host buffer whose size disagrees with what step 4 reported. None can be raised
where it is found, because configuration runs before there is a run to fail. The
failure is recorded against the offending path and `init()` refuses to proceed,
leaving the model in the terminated state `sim.abort` produces (§10.2) with no
node having executed.

**Configuration failures are not `SE` diagnostics.** Every `SE` code names a
compile stage (§16.4) and carries a source span; these arise in a shipped binary
with no source in reach, and the last three of the four are not detectable at
compile time at all. They are reported by the model against a *path*, in the
mechanism §14 defines for a rejected settings source, and the catalogue in §16.4
deliberately does not cover them.

#### 6.2c Record settings

A setting is declared exactly the way a port is — the same `type_ref`, records
included, nesting to any depth, with a concrete scalar at every leaf:

```
settings {
    gain:   math.Vec3(-)     = { 1.0, 1.0, 1.0 };
    limit:  math.Vec3(U);                            // every leaf required
    offset: math.Vec3(m)     = { z = 0.0 };          // x and y still required
    seat:   Pose(m)          = { p = { 0.5, 0.0, 1.2 },
                                 q = { w = 1.0, x = 0, y = 0, z = 0 } };
}
```

**A record setting is a tree of scalar leaf settings, and only the leaves are
real.** This is the whole of the design. §6.2's dependency graph, `SE0421` cycle
detection, required-ness, pinning, §6.2b step 3, the §13.2 override path and the
§15.6 manifest row all keep operating on scalars, exactly as before; a record
setting simply contributes more than one of them. Nothing in §7 became
record-valued, which is why §7.2's "every value is a real number with a
dimension" still holds without qualification.

A leaf is named by a dotted path, and that one spelling is right in all three
places it is used — as the tail of an override path, as a `param.*` reference,
and as C++ member access in the generated `resolve_settings` — because a record
lowers to a struct carrying the field names verbatim (§5.2).

**Required-ness, defaults and pinning are per leaf.** `limit` above has no
default, so all three of `limit.x`, `limit.y` and `limit.z` are required and
`SE0420` names the leaf. `offset` defaults only `z`, so `offset.x` and
`offset.y` remain required — which a positional list cannot express, and is the
reason both spellings exist.

##### The brace list

A brace list is **not a value**. It is destructured against the declared field
tree at the declaration site, and never evaluated: no aggregate ever becomes a
§7 value. What survives destructuring is one ordinary scalar expression per
leaf, checked and converted by the rules §7.2 already states.

It has **two spellings, and a list is wholly one or the other** (`SE0243`) —
the same "two equivalent spellings that desugar identically" as fan-out
(§6.9.2):

- **Positional** — matched to the order the `type` declares its fields, and
  **must be complete** (`SE0423`). Terse, and the right choice for a `Vec3`.
- **Named** — order-free and **may be partial**; an unnamed field falls through
  to its own default, or stays required. An unknown field name is `SE0424`, and
  a field given twice in one list is `SE0425`. Last-writer-wins is the rule for
  *overrides* (§13.3), which arrive from different sources; inside one brace
  list a repeat can only be a mistake.

The two nest independently, so each level picks whichever reads better. A brace
list where a scalar is declared — or a scalar expression where a record is —
is `SE0426`.

**Each leaf converts on its own**, exactly as a record wire's field does (§5.2):

```
limit = { 5000.0 (mm/s^2), 5000.0 (mm/s^2), 4000.0 (mm/s^2) };
```

##### Reaching a leaf, and copying a record

`param.` reaches a leaf by the same dotted path, and the result is an ordinary
scalar in an ordinary §7 expression:

```
settings { span: double(m) = param.limit.x - param.offset.x; }
```

A flow-down binding (§6.9.1) takes either brace spelling, and one form more:
naming a setting of the same record type **copies it, leaf by leaf**.

```
node cue : sig.ScaleLimit3 (m/s^2) {
    gain  = { 0.6, 0.6, 0.4 };
    limit = param.accel_limit;      // one spelling, every leaf
};
```

The copy is emitted per leaf rather than folded, so overriding the parent's
`accel_limit.z` at configuration still reaches the child (§6.2b step 3). This is
a copy and not arithmetic: **there is no record algebra**. `param.a + param.b`
over two `Vec3`s is `SE0426`, and deliberately — a component-wise `+` would have
to answer for `*` as well, and a language with a dot product in its *setting*
expressions has stopped being the small total language §7.4 argues for.

##### What did not change

- **An override addresses a leaf, never a record** (§13.4).
- **The compiled model's loader is untouched** (§14.1). It still reads a flat
  path and a number with an optional unit suffix, because that is all a leaf
  ever is.

### 6.3 `inputs` and `outputs`

```
inputs {
    drive_torque: double(N*m);      // scalar port
    ws:                 WheelState;  // record port
}
outputs {
    torque_cut: double(N*m);
}
```

Ports have no default and no initialiser: an input's value comes from its wire,
an output's from `output()`.

- **Fan-out is allowed**; **fan-in is forbidden** (§6.9.2).
- An unconnected input is an error at stage 4 (`SE0430`) — there is no implicit
  zero. An unconnected *output* is fine; it is simply unread.
- **An unread output is how a leaf exposes a value only to record it** (§13.5)
  — a stroke command, a motor speed, an intermediate the outputs compute on
  the way. There is no separate section and no qualifier: wiring it later is
  the only change, and its recording path, `node.out.x`, stays the same either
  way. The topology page hides outputs nothing reads until **unread outputs**
  is ticked, and the details panel lists them regardless. What having no
  syntax costs is that an output left unwired by mistake looks exactly like
  one kept on purpose.

  What satisfies it need not be a producing node. §6.9.2 admits two constant
  sources — a literal, `0.0 --> ain.y`, and a setting, `param.grav --> …` — and
  either one *connects* the input. So the rule costs nothing at a channel a
  model means to hold still, and the port itself never acquires a default: a
  forgotten wire stays an error at every node, which is the property the absent
  implicit zero exists to protect.
- Port order matters only for readability. Nothing positional exists.
- **A port may not carry an extent** (`SE0234`), unlike a state or a var (§6.4a).
  A port lives in the signal block, whose offsets come from `offsetof` on a
  generated struct (§15.5); a runtime-sized member would destroy that. A bundle
  of N signals crosses a wire as N ports or as a record. See Appendix C.
- **A port may be a buffer** (§5.1a). An output declares its capacity,
  `data: buffer(1500);`, because the producer's `Out` member is the wire's
  storage (§15.3); an input does not, `packet: buffer;`, because it only reads.
  The capacity is a literal, so the member has a fixed size and the signal
  block keeps its `offsetof` layout — which is why a buffer is allowed here
  when an extent is not.

### 6.4 `states`

**One `states` section holds all three kinds, and the kind keyword is mandatory.**

```
states {
    continuous omega: double(rad/s) = 0.0;
    discrete   cut:   double(N*m)   = 0.0;
    discrete   ticks: int(-)        = 0;
    finite     link:  LinkState     = IDLE;
}
```

Omitting `continuous`/`discrete`/`finite` is an error (`SE0230`), not a default.
There is deliberately **no preferred state kind**: a default would privilege one,
and the three differ genuinely in write mechanism, in schedule, and in their
relationship to the solver.

| | moved by | written in | in the solver state vector | holds its node (§9.3) |
|---|---|---|---|---|
| `continuous` | flow (`der.<name>`) and jump (`next.<name>`) | `derivative()`, `next()` | **yes** | no |
| `discrete` | jump (`next.<name>`) | `next()` | no | **yes** |
| `finite` | jump (`next.<name>`) | `next()` | no | no |

The kinds answer **how a state changes**, not what values it takes. A
`continuous` state flows and is integrated, and may also jump between steps
(§6.4b); a `discrete` one is a sampled-data state, x⁺ = h(t,x,u); a `finite` one
is the state of a state machine, which jumps between steps and holds still within
one (§6.4c). A `discrete int` and a `finite int` can hold the same numbers; what
differs is what that does to the schedule.

- **The write accessors are enforced by shape.** The generated `Der` struct
  carries only continuous fields, so `der.ticks` is a "no such member" compile
  error with no body parsing whatsoever (I2). `Next` carries one field for each
  state of any kind, so `next.<name>` exists for exactly the names `state.<name>`
  does.
- **`next.` means simultaneous update.** A direct `state.x = …` mutation would be
  *sequential*, making results depend on statement order inside `next()`. With
  `next.`, every state reads the old value and writes the new one, which is what
  the state-space form x⁺ = h(t,x,u) actually says. The lowering gives each block
  a buffer of the same shape (§15.5): every `next()` reads the committed blocks
  and writes the buffers, and the tick commits them once. Simultaneity is
  therefore a property of the **model**, not of each node in turn.
- **A state the body does not write keeps its value.** The buffers are seeded
  from the committed blocks before the first `next()` runs.
- Only `double` may be `continuous` (`SE0431`); the state vector is a flat
  `double` array.
- Only a type with finitely many values may be `finite` — `bool`, `int` or an
  finite type (§5.2a) — and `double`/`float` is `SE0434`. It is the counterpart of
  `SE0431`, from the other side: a state machine's state is a name, and a real
  number that only ever jumps is either a counter (`int`) or a `discrete`
  state.
- **The declared default is the initial condition.** There is no separate
  `initial` section — ICs ride the one override channel (§13.4). `init()` is the
  escape hatch for a computed or equilibrium IC. It runs after every override
  has been applied, so it starts from the IC the run was configured with.
- Discrete and finite states earn their place over `vars` precisely via that IC
  channel: a declarative default plus a sim-file override, which `vars` never
  had.

#### 6.4a Array-shaped states

A state may carry an **extent**, making it an array of `double`:

```
settings { order: int(-) = 4; }
vars     { nbq: int(-); }

states {
    discrete w1 [param.nbq]: double(-) = 0.0;
    discrete w2 [param.nbq]: double(-) = 0.0;
}
```

- **The extent is a §7 expression evaluated at configuration** (§6.2b step 4),
  so it may read `param.*` and it moves when an override moves. It must be
  dimensionless and a non-negative integer; a non-integer or negative value is a
  configuration failure, and an extent that cannot be dimensionally valid at all
  is `SE0432` at elaboration. Zero is legal and yields no slots.
- **An array state must be `double`** (`SE0433`), both kinds. `SE0431` already
  says this for `continuous`; the extension to `discrete` is what lets both kinds
  share one flat-block lowering (§15.5), and `int`/`bool` states remain available
  unextended.
- **The declared default initialises every element.** There is no per-element
  default syntax. Non-uniform initialisation is `init()`'s job — which is where
  a computed IC already belonged (above), and which is where the coefficients
  such a state is usually paired with get computed anyway.
- **Elements are addressed by index** — `bw.w1[3]` — as a recorded signal
  (§13.5) and as an override target (§13.4). The whole array is not addressable
  as one path: there is no aggregate value in §7 to set it to, and the declared
  default already covers the uniform case.
- **Extent is not a type.** `w1 [3]` and `w1 [4]` are the same declaration with
  different configurations, not different types. Nothing about an extent
  participates in unit checking, wire compatibility, or instance identity,
  because a state is never a port and never crosses a wire.

**What this deliberately does not buy.** An array state lets *one node* hold a
parameterised amount of state. It does not let a *model* hold a parameterised
number of nodes: instance count and wiring remain elaboration-fixed (§6.2), the
schedule remains a flat sequence of named calls (§15.7), and a cascade of N
sections is therefore one leaf that loops internally, not N instances. The loop
lives in a verbatim body, where loops have always been legal, so nothing in §7
grows to support this.

**Name the extent with a derived setting.** An extent is a §7 expression and a
body is C++, so the two cannot share a subexpression; writing
`[ceil(param.order/2)]` on the declaration and `(param.order+1)/2` in the loop
bound states the same fact twice in two languages, and nothing checks that they
agree. Declaring the count once as a derived setting (§6.2) and referring to
`param.nbq` from both sides removes the duplication entirely, and is the
idiomatic form. It also means an override may pin the section count directly,
detaching it from `order` exactly as §13.3 describes for any derived setting.

##### A worked example

The case this feature was added for: an Nth-order Butterworth as a cascade of
second-order sections, in one leaf.

```
package se.sig.dt;

node Butterworth {
    units { U; }

    settings {
        fc: double(Hz) = 2.0;                    // cutoff
        order: int(-)    = 4;                      // filter order
        nbq: int(-)    = ceil(param.order / 2);  // sections, named once
        fs: double(Hz) = sample_rate;
    }

    inputs  { x: double(U); }
    outputs { y: double(U); }

    states {                                         // Direct Form II, per section
        discrete w1 [param.nbq]: double(-) = 0.0;
        discrete w2 [param.nbq]: double(-) = 0.0;
    }

    vars {
        a1 [param.nbq]: double(-);   a2 [param.nbq]: double(-);
        b0 [param.nbq]: double(-);   b1 [param.nbq]: double(-);
        b2 [param.nbq]: double(-);
    }

    // Pole placement and the bilinear transform, in C++ because that is where
    // loops and <cmath> live (§4.4). Runs at §6.2b step 6, so `param.fc` is
    // already whatever a §14 source overrode it to.
    init() {
        const double pi = std::acos(-1.0);
        const double K  = 2.0 * param.fs;
        const double wa = K * std::tan(pi * param.fc / param.fs);     // pre-warped
        const int    N  = param.order;
        const bool   odd = (N % 2) == 1;

        for (int i = 0; i < param.nbq; ++i) {
            if (odd && i == param.nbq - 1) {
                // The lone real pole at s = -wa, as a FIRST-order section with
                // its second-order terms left at zero. See the fourth note
                // below for why it is not folded into the biquad formula.
                const double d0 = K + wa;
                var.a1[i] = (wa - K) / d0;   var.a2[i] = 0.0;
                var.b0[i] = wa / d0;         var.b1[i] = wa / d0;
                var.b2[i] = 0.0;
                continue;
            }
            // A conjugate pole pair on the Butterworth circle:
            //     s = wa * exp(j*th),  th = pi/2 + pi*(2i + 1)/(2N)
            // giving s^2 - 2*wa*cos(th)*s + wa^2, over a constant numerator
            // wa^2 so the section is unity-gain at DC.
            const double th  = pi/2.0 + pi*(2*i + 1) / (2.0*N);
            const double ac1 = -2.0*wa*std::cos(th);
            const double ac2 = wa*wa;
            const double d0  = K*K + K*ac1 + ac2;

            var.a1[i] = (-2.0*K*K + 2.0*ac2) / d0;
            var.a2[i] = ( K*K - K*ac1 + ac2) / d0;
            var.b0[i] = ac2 / d0;
            var.b1[i] = 2.0*ac2 / d0;
            var.b2[i] = ac2 / d0;
        }
    }

    output(x) {
        double u = in.x;
        for (int i = 0; i < param.nbq; ++i) {
            const double w0 = u - var.a1[i]*state.w1[i] - var.a2[i]*state.w2[i];
            u = var.b0[i]*w0 + var.b1[i]*state.w1[i] + var.b2[i]*state.w2[i];
        }
        out.y = u;
    }

    next(x) {
        double u = in.x;
        for (int i = 0; i < param.nbq; ++i) {
            const double w0 = u - var.a1[i]*state.w1[i] - var.a2[i]*state.w2[i];
            u = var.b0[i]*w0 + var.b1[i]*state.w1[i] + var.b2[i]*state.w2[i];
            next.w1[i] = w0;
            next.w2[i] = state.w1[i];
        }
    }
}
```

Four things are worth reading off this listing. The first three were design
constraints rather than coincidences; the fourth is the one place the arithmetic
had to be written a particular way to stay honest.

- **`output()` and `next()` recompute `w0` rather than sharing it.** §8.2 splits
  the two, and the predecessor C++ this replaces fused them into one
  `double filter(double x)` that advanced state and returned a value in the same
  call. The split is what makes ZOH and recording well-defined;
  the cost is a handful of multiply-adds and it is the same cost a scalar
  `dt.Biquad` already pays.
- **Nothing here is variadic to the compiler.** One instance, one call site per
  method, one entry in the schedule, whatever `order` is. §15.7 never learns that
  this node is a cascade.
- **`order` and `fc` are both ordinary settings**, so both are overridable in a
  compiled binary — `order` resizing the blocks at §6.2b step 4 and `fc` reaching
  the coefficients through `init()` at step 6.
- **An odd order's real pole is a first-order section, not a degenerate biquad.**
  Setting `ac0 = 0` and running the pair formula anyway is tempting — it keeps
  one code path — and it is algebraically correct: the extra factor appears in
  numerator and denominator alike, since
  `(K + wa) + 2·wa·z⁻¹ + (wa − K)·z⁻²` is exactly
  `(1 + z⁻¹)·[(K + wa) + (wa − K)·z⁻¹]` and the numerator carries the matching
  `wa·(1 + z⁻¹)²`. But the **recursion** runs on the unfactored denominator, so
  what actually gets iterated has a pole at `z = -1` — undamped, on the unit
  circle — held down only by a zero that cancels it on paper. Rounding does not
  respect a cancellation, and neither does a non-zero initial condition arriving
  through §13.4. Released from one, the folded form decays normally until it
  reaches the rounding floor and then **stops decaying**, holding the same
  ~1e-17 alternating residue for the rest of the run; the first-order form keeps
  going geometrically. Writing the section as first order costs one branch and
  removes the mode entirely. This is the shape `se.sig.dt.Butterworth` ships.

**Narrowing to an `int` setting.** `nbq: int(-) = ceil(param.order / 2);` binds
a §7 expression, evaluated in `double` (§7.2), to a setting declared `int`. The
question predates array extents — it applies to any `int` setting with a
computed default — but it becomes load-bearing here, because an extent must
resolve to an exact non-negative integer and a value that is `2.9999999` is not
one.

The rule is **exact-or-reject, checked twice**:

- At **elaboration**, the value must be within `1e-9` of an integer. It is
  rounded to that integer if it is, and is `SE0402` (a lossy conversion to an
  `int` target) if it is not. A tolerance rather than equality, because the
  value arrives through decimal literals and possibly a unit conversion, so an
  intended integer need not be exact.
- At **configuration**, the same expression is re-evaluated against whatever a
  §14 source supplied, so a non-constant one emits as `std::floor(x + 0.5)`
  rather than letting C++ truncate toward zero. §7 does not grow an
  integer-valued subset; it stays one arithmetic over `double`, and the
  narrowing is a property of the SITE, which is where the declared type is.

An **extent** carries the same rule one step further, because it must also be
non-negative: an extent that is not an exact non-negative integer is `SE0432` at
elaboration when nothing overridable feeds it, and a configuration failure
(§6.2b, §16.4) when something does. Zero is legal and yields no slots.

#### 6.4b Jumping a continuous state

A continuous state normally moves only by being integrated. Writing `next.<name>`
for one moves it **between** steps instead: the *jump map* of a hybrid system,
the counterpart of `derivative()`'s flow map. Four ordinary things need it:

- normalise a quaternion or a direction-cosine matrix;
- wrap a heading into (−π, π];
- clamp an integrator when the actuator it drives has saturated (anti-windup);
- reset a state on a mode change, or flip a velocity at a contact.

```
states { continuous qw: double(-) = 1.0;   continuous qx: double(-) = 0.0; … }

next() {
    const double n = std::sqrt(state.qw*state.qw + state.qx*state.qx + …);
    next.qw = state.qw / n;
    next.qx = state.qx / n;
    …
}
```

A jump is opt-in: a continuous state has `derivative()` to move it, and most never
jump. It lands at the commit, below the recording and the exit and above the
solver (§9.6), so the row that tick still reports the state before the jump, and
the integration starts from the state after it.

It lands **before** the flow rather than after, for the same reason every jump
does. `next()` reads the inputs the tick's outputs produced at *t*[k]; run after
the solver, it would pair *x*[k+1] with inputs left over from the last trial
stage.

**It does not replace normalising in `output()`.** The advance re-propagates
`output()` at each solver stage's trial state (§9.6), which is off the manifold
again. A jump keeps the *state* on the manifold across a long run; it never makes
a consumer's precondition (§10.6) hold. A node publishing a quaternion that others
rely on being unit still normalises what it publishes.

#### 6.4c Finite states

A `finite` state is the state of a **state machine**: a gear, a clutch that is
locked or slipping, a controller in MANUAL or AUTO, a local mirror of a remote
device's fifteen states. It jumps between steps and holds still within one, and
it reads, like any state, as `state.<name>` in every method.

It is a kind of its own because neither other kind fits. A `discrete` state
makes its node zero-order held (§9.3), so a leaf that keeps a gear beside a
torque computed from a continuous speed would publish that torque as a
staircase, and RK4 would collapse to first order across it. A `continuous`
state with a derivative of zero gives the right answer — but it is a `double` in
the solver's vector standing in for a name, and every `derivative()` has to
remember to write its zero.

```
states {
    finite link: LinkState = IDLE;
    finite retries: int(-) = 0;
}

next(ack, err) {
    if (in.err)                                      next.link = LinkState::FAULT;
    else if (state.link == LinkState::CONNECTING && in.ack)
                                                     next.link = LinkState::CONNECTED;
    if (in.err) next.retries = state.retries + 1;
}
```

**Coupled updates are ordinary**, and `next.` being buffered is what makes them
safe. Every `state.` read sees the value the tick began with:

```
states { finite range: Range = DRIVE;   finite gear: int(-) = 1; }

next(sel, w_out) {
    next.range = in.sel;
    if (state.range == Range::DRIVE) next.gear = schedule(in.w_out);   // the OLD range
    else                             next.gear = 0;
}
```

Moving the selector to NEUTRAL drops the gear on the **following** tick,
whichever order those two statements are in. Wanting it on this tick is a
different function, and it says so: `in.sel == Range::NEUTRAL` rather than
`state.range == Range::NEUTRAL`.

**A mode and the jump it causes are one body.** A clutch that locks snaps its
speed to the engine's in the same tick:

```
next(w_eng) {
    const bool lock = state.locked || slip_small(in.w_eng);
    next.locked = lock;
    if (lock) next.w_clutch = in.w_eng;
}
```

A reset that happens only on **entry** compares the new mode with `state.`, which
is still the old one. Bumpless transfer is the standard case: the integrator is
preset once, when the controller switches to AUTO, and left to integrate
afterwards.

```
next(want_auto, u_manual, err) {
    const Ctl c = in.want_auto ? Ctl::AUTO : Ctl::MANUAL;
    next.ctl = c;
    if (c == Ctl::AUTO && state.ctl != Ctl::AUTO)
        next.integ = (in.u_manual - param.kp * in.err) / param.ki;
}
```

**The new value is in force from t_k⁺.** The commit happens before the advance,
so every stage of the solver and the next tick's `output()` all see it, and that
is why a finite state does **not** hold its node. A `discrete` node is held
because its stages would otherwise read *dis*[k+1] while publishing the step that
began at *dis*[k] (§9.3); a finite state has no such pair to confuse. It holds one
value across the whole step, the one the step was decided under, and a node that
reads it is re-evaluated at the stages like any other.

### 6.5 `vars`

```
vars { inertia_inv: double(1/(kg*m^2)); }
```

Per-instance persistent storage that is **not state**: private C++ members,
outside the solver vector and outside the override channel.

- **Readable in every method; writable only in `init()` and `next()`.** A write
  in `next()` lands at once, unlike a `next.` write (§6.4): it is the node's own
  bookkeeping, not part of the jump the model commits together.
- A `var` needs no default; it is zero-initialised and is expected to be computed
  in `init()`.
- Records are permitted as `vars`.
- **A `var` may be a buffer**, with a capacity (§5.1a). It is the usual staging
  area for one: a body that reads a device in `next()` cannot write an
  output there, so it fills `var.rx` and `output()` copies it to `out`.
- **A `var` may carry an extent**, on the same terms as a state (§6.4a): a §7
  expression evaluated at configuration, dimensionless, a non-negative integer.
  This is what holds the per-element coefficients an array state is filtered by,
  and it is why the two features arrive together — an array state with scalar
  coefficients would describe nothing worth describing.
- **An array `var` is sized and owned by the node**, not by the host. §6.5's
  first sentence is the reason: a `var` is private storage outside the override
  channel and outside every block the model publishes, so it carries no external
  contract and there is nothing for a host to bind. It is the one allocation in
  the lowering, it happens once at configuration, and it never occurs inside a
  tick (§15.5).

The write restriction is the same rule as everywhere else in §8: `output()` and
`derivative()` are evaluated four times per RK4 step at trial states the trajectory
never visits. A peak detector written as

```
output() { var.max_v = max(var.max_v, state.velocity); }   // rejected
```

would report a maximum that never physically happened and that changes with the
solver. Confining writes to `next()` — which runs exactly once per accepted
step — makes that unrepresentable.

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

C++ that several nodes of one package share belongs in the package manifest's
`declarations` block instead (§2.6), which is guarded once per package and
emitted ahead of every node's block.

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
- **Natives are in scope only in `init()`, `next()`, and `final()`.** In
  `output()` and `derivative()` the generator simply does not emit the alias, so the
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
instance   := { annotation } 'node' identifier ':' qualified_name
              [ unit_args ] [ '{' { binding } '}' ] ';'
unit_args  := '(' unit_expr { ',' unit_expr } ')'        // §6.2a
binding    := identifier '=' expression ';'
```

- The trailing `;` after the closing brace is **required** — it terminates the
  statement the same way a port declaration's does, and it makes recovery after a
  malformed binding list unambiguous.
- Bindings are **declarative** (§7): each right-hand side is a side-effect-free
  expression over literals, the enclosing node's `param.*`, arithmetic, and
  builtin math, **unit-checked at elaboration and evaluated at configuration**
  (§6.2b). A binding whose operands are all literal folds at elaboration and is
  emitted as a constant; one that reads `param.*` is emitted as an expression, so
  overriding the parent's setting flows down in a compiled binary.
- **No cross-instance references.** `mass = fl.mass` is illegal (`SE0440`).
  Flow-down goes strictly parent → child.
- Binding an unknown setting, or binding one twice, is an error (`SE0441`).

Why declarative and not imperative: under an imperative scheme, every composite
would need hand-written C++ solely to pass a number down, which contradicts §6.1
— a composite does no work. Declarative flow-down also makes required settings
statically checkable, and keeps the flow-down graph *visible to the compiler*,
which is what lets §6.2b re-evaluate it in dependency order at configuration
without the user writing an ordering anywhere.

**A composite's settings therefore exist at run time**, unlike its ports and
wires, which flatten away entirely (§15.2). They are the inputs to the flow-down
expressions, and overriding one at configuration is the whole point of
addressing a path like `front.left.mass` (§13.2). They compute nothing beyond
§6.2b step 3 — but because they are real members, one may be *read* as a wire
source (§6.9.2), which is the cheapest **live** constant the language offers.

#### 6.9.2 Wires

```
wire     := endpoint '-->' endpoint { '-->' endpoint } { ',' endpoint } ';'
endpoint := ( 'self' | identifier ) { '.' identifier }
          | [ '+' | '-' ] number [ unit ]      // a literal: a SOURCE only
          | string                             // text: a SOURCE only (§5.1b)
```

- `-->` is the connection token; `;` terminates.
- **A composite wires its own ports with `self.`** — `self.throttle --> eng.throttle`
  and `drv.speed --> self.speed`. Bare names would collide with child instance
  names. `self` alone is still the keyword and never an identifier, so the same
  argument holds for the bare form below.
- **Fan-out** has two equivalent spellings — repeated lines, or a comma list on
  one line. They desugar identically.
- **An endpoint may name no port**, in which case it means the sole port on the
  side the arrow needs: `gain --> filter` is `gain.y --> filter.x` when `gain`
  has one output and `filter` has one input. If that side does not have exactly
  one port the wire is an error (`SE0314`) — there is no defaulting rule, no
  declaration order to remember, and nothing to mark on the definition.
  Whether a bare endpoint is legal is a property of the node it names, so the
  same node reads the same way at every call site.

  The rule applies **per endpoint, not per wire**. `sp.x --> gain` names the
  source because `sp` has three outputs and *which axis* is the thing worth
  reading, while the destination stays bare because naming it would say
  nothing. An all-or-nothing form would force the noise back on.

  `self` obeys the same inversion as everywhere else in this section: bare
  `self` as a **source** means this node's sole *input*, and as a
  **destination** its sole *output*.
- **Wires chain.** `a --> b --> c` is exactly `a --> b; b --> c;`, and desugars
  in the parser, so a chain is not a distinct kind of thing that later stages
  can see. Any length is allowed.

  **An interior endpoint must be bare** (`SE0214`). It is a destination and a
  source at once, so a port name there would have to mean `b`'s input `y` and
  `b`'s output `y` — two different ports that merely share a spelling — and an
  interior `self` would have to mean this node's output and its input in one
  place. The ends are unrestricted, which is what makes the form useful: the
  interesting names stay, and only the ones that carry no information go.

  A chain reads as a signal path, which repeated lines do not. Where the
  alternative is a grid of per-axis wires that the reader has to transpose, this
  is the difference the feature exists for:

  ```
  sp.x --> sx --> hx --> mg.x;
  sp.y --> sy --> hy --> mg.y;
  sp.z --> sz --> hz --> mg.z;
  ```
- **A comma list terminates the statement**, so a fan-out attaches to the last
  hop: `a --> b --> c, d;` is `a --> b; b --> c; b --> d;`. This is not a
  precedence rule to remember but the only thing the grammar admits — once a
  comma is seen no further `-->` may follow, and `a --> b, c --> d` is a syntax
  error.
- **A setting may be a wire source**, spelled `param.<setting>` — the enclosing
  composite's own setting, driving an input directly:

  ```
  settings { grav: math.Vec3(m/s^2) = { 0.0, 0.0, -9.80665 }; }
  structure {
      self --> linScale --> sfSum.a;
      param.grav        --> sfSum.b;
  }
  ```

  This is the cheapest **live** constant the language has. A composite's
  settings already exist at run time (§6.9.1), so the wire reads a member that
  is already there: no node, no `Out` storage, and no edge in the §8.5 sort. It
  is *live* — the reference is emitted, not folded, so overriding `grav.z` at
  configuration reaches the wire in a compiled binary, exactly as a flow-down
  binding does (§6.2b step 3). That is the whole of what it buys over a literal
  (below), which is cheaper still and folds. `se.sig.src.Constant` remains the
  right answer when the constant wants a name in the schedule, or a
  recordable signal path.

  `param` is available as an endpoint head because it can never be anything
  else: §6.11 already forbids it as a member name, instance names included, so
  the spelling is unambiguous without reserving a word.

  Four rules, and they divide by stage. **Shape** is settled at stage 3, before
  any setting is looked up, because it is a property of the wire:

  - **Source only.** A wire may not drive a setting (`SE0315`). A setting is
    configuration, not signal; it moves at §6.2b and nowhere else.
  - **No bare form.** `param` alone is an error (`SE0315`). Bare defaulting is
    an arity rule over *ports*, and a node's settings present no side for the
    arrow to pick from.

  **Which setting** is stage 4, where the settings exist:

  - **A record leaf may be named**: `param.grav.z` wires that scalar, and
    `param.grav` wires the whole `Vec3`. §6.2c holds that only the leaves of a
    record setting are real, so naming one here is naming a scalar — not taking
    a field of a wire, which is the separate and still-unsupported thing
    `SE0311` rejects. Unknown setting, or unknown field, is `SE0304`.

  Everything else about the endpoint is unchanged: units are checked and
  converted per §5.2, scalar and record alike, and fan-out is free — a setting is
  read-only, which makes it a legitimate single writer for any number of
  destinations.
- **A literal may be a wire source**, spelled `[+|-] number [unit]` — a constant
  driving an input directly:

  ```
  structure {
      node surge : sig.src.Step (m/s^2);
      node ain   : math.Merge3 (m/s^2);

      surge --> ain.x;
      0.0   --> ain.y, ain.z;
  }
  ```

  This is the answer to the question `SE0430` raises. An input held at a
  constant is still **connected** — connected to a constant rather than to a
  producer — so §6.3 needs no exception and no port ever acquires a default.
  The alternative spellings are one `se.sig.src.Constant` per channel, or a
  setting declared solely to be zero.

  It shares the setting source's shape exactly: no node, no `Out` storage, and
  no edge in the §8.5 sort. **It differs in exactly one way, and the difference
  is the point:** a literal is *folded* at elaboration, where a setting is
  *emitted as a reference*. A setting has to stay live because §6.2b can
  override it in a compiled binary; a literal has nothing to override. That
  asymmetry is what keeps ports out of the configuration schema (§15.6) — a
  literal appears in no `[settings]` row, so there is no way for a wire and an
  override to disagree about one input.

  **Units are §7.2's rule, unchanged.** A bare literal takes the unit of the
  site it initialises, and the site is the *destination*: `0.0 --> ain.y` is
  `(m/s^2)` because `ain.y` is. This is applied per destination, so a fan-out
  onto ports declaring different units gives each the value in its own unit. A
  suffix converts instead, exactly as a setting source's does — `9810 (mm/s^2)`
  into an `(m/s^2)` port folds to `9.81` — and every hop of a chain folds into
  the same single number.

  Three rules. The first is settled at **stage 2**, unlike the `param` rules
  above, because a literal is a number and the parser can see what it is looking
  at without resolving anything:

  - **Source only.** A wire may not drive a literal (`SE0215`), in a destination
    or at an interior position of a chain. There is nothing to write to.

  The rest are the ordinary stage-4 questions any endpoint answers:

  - **Scalar only.** A literal is a scalar, so a literal into a record port is
    the usual `SE0410`. A constant *record* is `param.<setting>`, which is the
    better spelling regardless: it is named, overridable, and in the manifest.
    A brace list as a wire source is a pure extension; see Appendix C.
  - **The suffix is checked like any other source's unit** (`SE0410`).

  There is deliberately **no arithmetic**: the endpoint is a constant, not an
  expression. `2.0 * param.k --> x` is not a wire. A *computed* constant is a
  setting with a §7 default wired as `param.<name>`, which is again the better
  answer, being named and overridable. Fan-out is free, and a literal cannot
  create fan-in because it can never be a destination.
- **Fan-in is forbidden.** Two sources into one input is an error (`SE0442`);
  insert an explicit `Sum` node. This is what makes the single-writer wire
  lowering sound (§15.3). A setting source cannot create fan-in, because it can
  never be the destination end.
- **Wires are untyped in the syntax.** There is no type annotation on a
  connection; the type is a property of the ports and is checked, with unit
  conversion inserted, at stage 4.
- Direction is always source → destination. An output-to-output or
  input-to-input wire is an error (`SE0443`), except through `self`, where
  `self.<input>` is a source and `self.<output>` is a destination — the boundary's
  polarity is inverted from the inside, as it must be.

#### 6.9.3 Groups

A large composite has sections — a filter's translational, rotational and tilt
channels, say — and a reader of its topology diagram wants to see them. A
**group** draws a labelled box around some of a composite's children:

```
structure {
    /// The X and Y accelerations, low passed, become the steady lean that
    /// stands in for them.
    group "Tilt Channel" {
        node xyAccel:  math.Vec3GetXY (m/s^2) {};
        node tiltCalc: TiltCalc { G = param.grav.z; };
        node tiltQuat: kin.EulerToQuat {};
    }

    group "Head Offset, Out" { ... }

    toHead --> xyAccel;
    tiltCalc.tilt --> tiltQuat;
}
```

```
group := { annotation } 'group' string '{' { instance | wire | group } '}'
```

- **A group is diagram-only.** It changes no path, no generated identifier, no
  setting, no `.sim` address and no §8.5 order: `xyAccel` above is still
  `xyAccel`, never `tilt.xyAccel`. Deleting every group from a model changes
  the topology outputs (§15) and nothing else. That is the whole point of the
  construct and its whole limit. A section that should have its own settings,
  ports or path is a child composite, which already exists; a group is for
  the case where the sections are too cross-wired to be worth the ports.
- **`group` is a contextual keyword.** Inside `structure`, an identifier
  followed by a string literal is never a wire or an instance, so `group` is
  recognised only there and stays legal as an instance name: `node group : Gain;`
  and `group --> b;` mean what they always did.
- **The label is a string literal** (§3.6), because it is a title for a person
  to read, and a title has spaces and commas in it. Nothing refers to a group,
  so it needs no identifier.
- **Membership is by declaration.** An instance belongs to the innermost group
  its `node` statement is written in, so it is in at most one, which is also
  all a Graphviz cluster allows. Wires may be written inside a group, beside the
  nodes they connect, but a wire belongs to no group: it is drawn between its
  endpoints, wherever they are.
- **Groups nest to any depth**, and a child composite declared in a group is
  drawn inside it. A group's box holds its members together on the page, so one
  group around two sections that sit at opposite ends of the signal flow folds
  the layout back on itself. Two sibling groups do not.
- **A group documents.** A `///` block directly above one (§3.2) is shown with
  it on the topology page. It is never required (`SE0702` is about public
  interfaces), and `sec --doc` does not publish it: a reference page documents
  what a node offers, and a group is how its inside is laid out.

Three mistakes are caught at stage 2, because each would otherwise surface as a
puzzling picture rather than a message:

- A label used twice by **sibling** groups is an error (`SE0216`). Two boxes
  with one title, and one box merged from two places, would each surprise the
  reader. The same label under different parents is fine.
- An empty label, or one of only blanks, is an error (`SE0217`).
- A group that declares no instances is a **warning** (`SE0218`). It draws
  nothing, and if it holds wires, its author probably expected them to move.

### 6.10 Accessors

Inside a verbatim body, node data is reached through **namespaced accessors**:

| Accessor | Readable in | Writable in |
|---|---|---|
| `param.<name>` | every method | never |
| `in.<name>` | methods that declare it | never |
| `out.<name>` | — | `output()` |
| `state.<name>` | every method | `init()` |
| `der.<name>` | — | `derivative()` |
| `next.<name>` | — | `next()` |
| `var.<name>` | every method | `init()`, `next()` |
| bare name (`native`) | `init`, `next`, `final` | same |

**An array-shaped state or var is indexed through the same accessor** —
`state.w1[i]`, `next.w1[i]`, `der.w[i]`, `var.a1[i]` — with the
readability and writability rules above unchanged. Indexing is ordinary C++ subscripting on the
proxy §15.5 defines, so a body loops over stages the way any C++ does. Bounds
are not checked: a body's arithmetic is already unchecked (§4.4), and an extent
is available to the body as the same `param.*` or `var.*` the declaration read.

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

A member may not be named `param`, `in`, `out`, `state`, `der`, `next`,
`var`, `log`, or `sim`, since the generated
code puts objects by those names in scope of bodies (`SE0331`).

**No name the generated C++ spells verbatim may be a C++ keyword** (`SE0332`):
a setting, port, state, var or native; a record field; an enumerator; a node,
`type` or `finite` name; a package segment, which becomes a namespace. Appendix
B reserves only the DSL's own words, so without this rule `settings { new: … }`
would be accepted here and rejected by the C++ compiler, at a line in generated
code. The list is C++20's keywords and alternative tokens — `class`, `int`,
`delete`, `enum`, `and`, `or` and the rest.

**An instance name is exempt.** It is a path segment, and nothing in a body
names it; it reaches C++ only through an identifier the generator makes from the
path, which steps around a keyword itself (`delete` becomes `delete_`), as I5
says generated identifiers are its business. `node delete : Valve {};` is
legal, and `delete.y` records under that name.

### 6.12 Consistency rules

Typo safety comes from these rules rather than from a marker keyword on
functions:

| If the node has… | it must define… | else |
|---|---|---|
| `outputs` | an output method for each output (§8.3a) | `SE0340` |
| any `continuous` state | `derivative()` | `SE0341` |
| any `discrete` or `finite` state | `next()` | `SE0342` |

The converse is checked for the first two, since neither could do anything:
`output()` in a node with no outputs (`SE0344`), and `derivative()` in a node with
no continuous states (`SE0343`). An
`output default()` whose every output a group has already claimed is the same
mistake one level down (`SE0355`).

Every lifecycle method is defined at most once (`SE0352`). An output group is
not a second `output()` but a different method, named by what it writes, so
§8.3a's rules — each output written exactly once — are the ones that apply to
it.

The table pairs each state kind with the method that must write it. A continuous
state gets no row for `next()`, because a jump is optional (§6.4b). `next()` has no
converse rule: it is also the node's per-tick hook (§8.1), so a node with no state
may define one. `init()` and `final()` are always optional too — whether a body
does anything is not decidable without parsing it, which I1 forbids.

Note that the rules are per-kind, not blanket: a purely continuous node needs no
`next()`, and a purely algebraic node needs no `derivative()`.

### 6.13 `tests`

A node's worked examples live in header comments today, where nothing checks
them. `Cross3` says it is right-handed, `dt.Integrator` says its gain is half
the sample period, and both claims are prose: they were true when they were
written, they are load-bearing for anyone reading the block, and no run goes
red when one stops being true.

A `tests` section turns those examples into the thing that checks them. It
holds one or more named **sets**. A set is a claim; its rows are the worked
examples that demonstrate it.

```
node Cross3 {
    units   { U; }
    inputs  { a: Vec3(U); b: Vec3(U); }
    outputs { c: Vec3(U^2); }

    output(a, b) { … }

    tests {
        units { U = m; }

        set "the cross product is right-handed", in.a, in.b = out.c {
            {1,0,0}, {0,1,0}, {0,0,1}, "X x Y = Z";
            {0,1,0}, {0,0,1}, {1,0,0}, "Y x Z = X";
            {0,0,1}, {1,0,0}, {0,1,0}, "Z x X = Y";
            {1,0,0}, {1,0,0}, {0,0,0}, "X x X = 0";
        }
    }
}
```

**A test is not an experiment.** It fabricates one (settings, states, inputs)
point, invokes the node, and checks what came back. There is no sim file, no
solver, no recorder, no CSV and no golden file, and that is what keeps every
cell an ordinary §7 expression rather than a second language. A claim about
*accumulation* — that a filter is stable, that an integrator does not drift —
is a claim about a trajectory and is not what this section is for.

#### The set header

```
set "<claim>", <given> { "," <given> } ( "=" | "~=" ) <expected> { "," <expected> }
    [ "within" <expression> ] "{" <row>… "}"
```

Columns are named with §6.10's accessors — the same names the body uses, so
there is no second vocabulary — and the operator divides what is supplied from
what is checked:

| Side | Columns | Meaning |
|---|---|---|
| given | `param.<name>`, `in.<name>`, `state.<name>`, `step` | supplied before the invocation |
| expected | `out.<name>`, `der.<name>`, `next.<name>`, `var.<name>` | read after it and compared |

A column on the wrong side is `SE0361`. The split is not arbitrary: it is
§6.10's writability column read from the outside. What a body may only write is
what a test may only check.

**The expected side is the five pure methods, plus `var`.** §8.1 makes the five
methods that carry an equation pure and names each for the accessor it writes,
and purity is exactly the property that makes a method callable in isolation on
a fabricated (x, u) and repeatable. Nothing else in the language is callable
that way, and nothing else needs to be: `se.sig.ct.Integrator`'s entire
behaviour is `der.y = in.x` and `out.y = state.y`, and verifying it needs no
solver. The solver is verified once, not once per node.

**Which methods run is inferred from the expected columns**, and they run in
one fixed order:

```
init()  →  output()  →  derivative()  →  next()
```

`init()` always runs — a node whose `output()` reads a `var` its `init()`
computed is not callable before it, which is most discrete filters — and the
rest run only if some column names what they write. In a node with output
groups (§8.3a), `output()` in that line means the groups that write an
expected `out.` column, in declaration order; they write disjoint outputs and
read only the row's inputs, so the order among them cannot matter. So a set expecting only
`var.g` supplies no inputs and calls nothing else. The order is fixed rather
than derived so that a set expecting several fields means one thing.

A `next.` column reads what the body wrote, without the commit, so a given
`state.` column stays the value the body read as the old one.

#### Rows

A row is one §7 expression per column, in header order, then a label, then `;`.
Every row supplies every column; there are no holes, and a case that wants a
different shape wants a different set.

Cells are §7 expressions at §7 sites, so a cell is checked against its column's
declared unit by the same evaluator that checks a setting default, and
`{1,0,0}` destructures against a record port exactly as §6.2c destructures a
binding. A brace list's commas are already brace-delimited, so they are never
confused with the commas between columns.

**The label is required.** It is the worked example's name and it is what a
failure report leads with; `X x X = 0` locates a case in a way a row index
never will.

#### `=` and `~=`

`=` compares bitwise. `~=` requires a `within` clause closing the header, which
governs every expected column in that set. There is no default tolerance
anywhere, and `~=` without `within` is `SE0272`.

The choice of operator is part of what the set documents. `out.c = {0,0,1}`
says a cross product of unit axes is exact, and `next.w1 = 1.0` says the node
does no arithmetic it did not need to — claims a tolerance would quietly
weaken. A set whose columns differ in this respect is two sets, and that is the
right grain: *these are exact identities* and *these are computed to 1e-15* are
different things to claim about a block.

A bare tolerance literal takes the unit of the site it qualifies (§7.2), so
`within 1e-15` against `out.y : double(U*s)` is 1e-15 in that port's unit and
is dimension-checked like any other value.

#### The preamble

`step:`, `units {}` and `settings {}` may precede the sets and are shared by
all of them. A column of the same name overrides for its row.

```
tests {
    step:  10 (ms);
    units { U = m/s^2; }

    // `fs` is absent, so it takes its declared default of `sample_rate`.
    // That is what this set is for.
    set "the gain is half the sample period", step ~= var.g within 1e-15 {
        10 (ms), 0.005,  "100 Hz";
         5 (ms), 0.0025, "200 Hz";
    }

    set "the accumulator advances exactly", state.w1, in.x = next.w1 {
        0.0, 1.0, 1.0, "1 + 0 needs no tolerance";
        1.0, 1.0, 2.0, "and neither does 1 + 1";
    }
}
```

**`step` is a column because the default-binding path is worth testing.** A
node test has no sim file, so nothing else establishes the clock, and a node
whose setting reads `sample_rate` (§10.4) cannot be configured without one.
Supplying `param.fs` in every row would work and would never once exercise
`fs = sample_rate` — which §10.4 exists to warn about, since coefficients
designed at the wrong rate are silently wrong. Reaching a rate-derived setting
with neither a `step` column nor a `step:` entry is `SE0470`; the node is
unconfigurable, and saying so beats defaulting to a number nobody chose.

§10.5's `sim.step()` reads what `step` supplied.

#### Tests are not part of the model

A `tests` section contributes nothing to a lowered model: no member, no
schedule entry, no manifest line, no topology node. It is read only by
`sec --test`. Adding tests to a block cannot change what that block compiles
to, and the unit manifest (§15.6) is what holds that to account.

#### Running tests

`sec --test` **writes** a test program; it does not compile or run one. For
each tested node it emits the node exactly as `--emit` would — as the root of
a model whose class under test is the one any other model compiles — plus a
`main` that fabricates each row, invokes the methods, and compares. The
program exits non-zero if any row fails, and prints one line per set.

The split is §12.4's, applied to tests: generation and building are separable,
and `sec` never chooses, finds or invokes a compiler. Building the program is
the same job as building any emitted model, done by the same reference build
scripts, and running it needs nothing but the executable. A tool that does
both in one step — a script, a build system, a future driver — sits above
`sec`, not inside it.

#### Diagnostics

| Code | Stage | Meaning |
|---|---|---|
| `SE0270` | 2 | Duplicate entry in `tests` |
| `SE0271` | 2 | Unknown entry in `tests` |
| `SE0272` | 2 | `~=` with no `within`, or `=` with one |
| `SE0273` | 2 | Row arity does not match the set header |
| `SE0274` | 2 | Row has no label |
| `SE0360` | 3 | A column names no member of the node |
| `SE0361` | 3 | A column is on the wrong side of the operator |
| `SE0362` | 3 | An expected column names a method the node does not define |
| `SE0363` | 3 | A node with a buffer or string port has a `tests` section, or a column names a string setting |
| `SE0470` | 4 | A set reaches a rate-derived setting with no `step` |

A dimension error in a cell or a tolerance is the ordinary `SE0410`.

**A node with a buffer port cannot carry `tests` yet** (`SE0363`). A test
program runs the node as the root of a model (below), and a buffer may not be on
the root (§5.1a, Appendix C 16c); nor could a cell supply one, a cell being a §7
expression with no way to spell bytes. Both lift together with 16c.

**Nor can a node with a string port, or a column that names a string
setting** (`SE0363`). A string input on the root must state a capacity, which
a reader port does not (§5.1b), and a test row's cells are compared as
numbers. A string setting the rows don't name keeps its default and is no
obstacle.

### 6.14 Documentation

`sec --doc` writes a Markdown reference for a library: `index.md`, listing every
package with a one-line summary of each definition, and one page per package. It
reads `.se` files to stage 2 only, so a library can be documented without being
instantiated.

**The comment supplies prose; the source supplies everything else.** For each
file's public definition (§2.3) the page shows its unit parameters; its settings
with their types and defaults as written; its inputs, outputs and states with
their types and initial conditions; which inputs each output method reads (§8.3);
and each set of its `tests` section (§6.13) as a table of worked examples. A doc
comment therefore never restates a type, unit, default or initial condition: the
page already shows it, and a restatement is a second copy that can disagree.

**The shape of a definition's doc comment.** The first paragraph is the
*summary*: one sentence, ending in a period, which the index shows (`SE0704`).
Body paragraphs follow, and may contain an indented block for an equation. Then,
optionally, any of these sections, each opened by a line `# Name`, in this order
(`SE0705`, `SE0706`):

| Section | Holds |
|---|---|
| `Conventions` | frames, signs, handedness, orderings |
| `Initial conditions` | what the states start at, and why it matters |
| `Accuracy` | numerical behaviour, error bounds, drift |
| `Limitations` | inputs or settings outside which the block is wrong |
| `Design notes` | why the block is shaped as it is |
| `See also` | related definitions |

On the page the summary and body open the entry, and the sections follow the
signature tables. A member's doc comment is a fragment — the table cell beside
its name.

**Citations are checked.** A `§n.n` in a doc comment must name a heading of this
specification (`SE0707`), and an `SEnnnn` must name an entry of §16.4 (`SE0708`);
on the page a `§` becomes a link. `--spec=<path>` names the specification;
without it `sec` looks beside the first input, then in the current directory,
and with none found, citations are neither checked nor linked.

**Completeness is a warning.** A public definition, or a setting, input, output,
state or record field of one, with no doc comment is `SE0702`. It is a warning
so that a partly documented library still produces a reference; a project that
wants a complete one treats the warning as a failure. Unit parameters, `vars`,
`native` members and private definitions may be documented but need not be.

An error suppresses the output, as everywhere in `sec` (§16.2).

### 6.15 Annotations

A model says what it computes; sometimes it also wants to say how it should be
**drawn**. A per-axis wrapper such as `se.sig.ct.Integrator3` — a split, three
scalar integrators, a merge — is mechanics, and a reader of the topology
diagram wants it as one box until they ask for more. An **annotation** carries
that kind of hint:

```
/// Integrates each axis of a `Vec3`.
@diagram(collapsed)
node Integrator3 { ... }

structure {
    @diagram(expanded) node vel : sig.ct.Integrator3 (m/s^2);

    @diagram(collapsed)
    group "Head Offset, Out" { ... }
}
```

```
annotation := '@' identifier [ '(' arg { ',' arg } ')' ]
arg        := identifier [ '=' ( string | number | identifier ) ]
```

- **An annotation leads the declaration it annotates**: a `node` definition, a
  `group` (§6.9.3), or an instance in `structure`. It is made of tokens, not
  comments, so line breaks and blank lines between it and its declaration mean
  nothing. The declaration **begins at its first annotation**, which is where a
  doc comment attaches (§3.2): `///`, then `@`, then the keyword. An annotation
  followed by anything else — a wire, a `use`, a section, the end of a block —
  annotates nothing (`SE0287`).
- **It is metadata.** An annotation never changes a path, a generated
  identifier, a setting, a `.sim` address, the §8.5 order, the unit manifest or
  a `sec --doc` page. Deleting every annotation from a model changes the
  topology page (§15) and nothing else.
- **Why `@` and not a contextual word.** The `@` gives every annotation, at
  every site, one rule in the grammar, so a new annotation never has to be
  checked for ambiguity against what each site already accepts. Inside a node
  body, `name(` already means a lifecycle method or a verbatim member (§6),
  so a bare `diagram(...)` could not mean anything else there. And the `@` lets
  a reader skip a line, knowing it does not change what the model computes.
- **Arguments are literals.** A bare key is a flag; `key = value` takes a
  string, a number or a name, never an expression. A new key is a row in the
  compiler's key table, not a change to this grammar.

**`@diagram`** is the only annotation. Its keys are:

| Key | On | Meaning |
|---|---|---|
| `collapsed` | node definition, group, instance | The box starts collapsed on the topology page |
| `expanded` | instance | The box starts open, overriding its definition's `collapsed` |

An instance's own key wins over its definition's. `expanded` on an instance
whose definition does not say `collapsed` is allowed: it says nothing today, and
stays correct if the definition gains `collapsed` later. A definition's
`collapsed` is ignored when that node is the root, since a definition cannot
know where it will be used, and the root has no box to close.

The page opens in the declared state. **Expand all** and **Collapse all**
override it, **Reset** returns to it, and finding or selecting a node inside a
collapsed box opens the box. It also opens with outputs nothing reads hidden
(§6.3), until **unread outputs** is ticked. The Mermaid file (`.topology.md`)
ignores `@diagram`, and hides nothing: it is a static, complete record, and a
box drawn closed there could never be opened.

Checking is **strict**: every mistake is an error, because a mistyped key that
drew nothing would look exactly like a key that worked. At stage 2:

- an unknown annotation name (`SE0280`) or key (`SE0281`), with a
  did-you-mean;
- a key on a declaration it does not apply to (`SE0282`) — `expanded` on a
  definition or a group, anything on a `type`;
- a value given to a flag, or a `@diagram` with no keys (`SE0283`);
- a key given twice (`SE0284`), or `collapsed` with `expanded` (`SE0285`);
- two `@diagram`s on one declaration (`SE0286`), for the reason a section is
  written once (§6, `SE0220`): one declaration's hints are read in one place;
- an annotation that leads nothing (`SE0287`).

At stage 3, where an instance's definition is known, `collapsed` or `expanded`
on a leaf, or on an instance of one, is `SE0321`: a leaf is drawn as one box
already.

---

## 7. The elaboration expression language

A small, total, side-effect-free expression language, evaluated **once per run,
never per tick**. It appears in exactly four places: setting defaults, setting
bindings at instantiation, state defaults, and sim-file / `.settings` values.
(`tests {}` cells are §7 expressions too, at §7 sites — §6.13.)

`sec` evaluates it at elaboration to produce the values a model starts with, and
**emits** it — one C++ expression per §7 expression — so that configuration
(§6.2b) can re-evaluate it against overrides. Both readings are the same
language and cannot disagree, because the emitter walks the same tree the
evaluator does. A compiled model does **not** accept §7 text; §14.1 restricts the
right-hand side a running binary will parse.

### 7.1 Grammar

```
expression     := additive
additive       := multiplicative { ( '+' | '-' ) multiplicative }
multiplicative := unary { ( '*' | '/' ) unary }
unary          := ( '+' | '-' ) unary | power
power          := primary [ '^' unary ]              // right-associative
primary        := number [ unit ]
                | 'param' '.' identifier { '.' identifier }   // §6.2c
                | aggregate                                   // §6.2c
                | identifier '(' [ expression { ',' expression } ] ')'
                | identifier                          // named constant
                | string                              // §7.2b
                | '(' expression ')'

aggregate      := '{' [ positional | named ] '}'
positional     := expression { ',' expression }
named          := identifier '=' expression { ',' identifier '=' expression }
```

The trailing segments on `param.` reach a leaf of a record setting (§6.2c);
`param.offset.x` is a scalar and behaves as any other `param.*` does.

Precedence, loosest to tightest: `+ -` · `* /` · unary `+ -` · `^`. So `-2^2` is
−4 and `2^3^2` is 2⁹, matching ordinary mathematical convention.

### 7.2 Values and units

Every value is a real number with a dimension — or an enumerator, which has
neither and appears only where a finite type is expected (§7.2a), or text,
which appears only where a string is expected (§7.2b).

**An `aggregate` is not a value, which is why that sentence needs no
qualification.** A brace list never reaches the evaluator: it is destructured
against the declared field tree at the declaration site (§6.2c), and what the
evaluator sees is one ordinary scalar expression per leaf. So §7 has no
aggregate type, no record arithmetic, and nothing that could fail to have a
dimension. It appears in the grammar above rather than beside it because it may
only occur where a §7 expression may, and the parser has no way to know a
target's shape.

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

### 7.2a Enumerators

A site of finite type (§5.2a) — a setting, a state default, a binding, an
override, a test cell — takes an **enumerator, written bare**:

```
finite link: LinkState = IDLE;          // a state default
node dev : Device { policy = RETRY; };  // a binding
dev.link = CONNECTED;                   // a sim-file or .settings override
```

The site's type says which finite type the name belongs to, so `IDLE` can mean only
`LinkState::IDLE` and there is nothing to qualify. Settings are read through
`param.`, so a bare name never collides with one; and a site of finite type looks
an identifier up among its enumerators **before** §7.3's constants, so an
enumerator named `e` is still that enumerator. A name the type does not have is
`SE0403`, and the message lists the ones it does.

That is the whole of a finite type's life in §7. A site of finite type takes a
bare enumerator or a `param.` reference to a setting of the **same** type, which
copies it; no arithmetic, no builtin and no unit applies to either, and an
enumerator or such a setting anywhere else — an operand of `+`, an argument to
`max`, the value of an `int` site — is `SE0404`. §7.4's absence of comparisons
and conditionals is why there is nothing more: `param.mode == FAST ? HI : LO`
is not §7, for finite types or for anything else (Appendix C).

### 7.2b Text

A site of string type (§5.1b) — a setting's default, a binding, an override —
takes a **string literal** (§3.6) or a `param.` reference to a string setting,
which copies it:

```
host: string(64) = "127.0.0.1";          // a setting's default
node tx : Sender { host = param.host; };  // a binding that flows down
tx.host = "rig-pc";                       // a sim-file or .settings override
```

That is the whole of text in §7, as it is for an enumerator: no operation
joins, cuts or formats it, because §7.4's promise of one straight-line C++
expression per setting holds only for numbers. A file name built from parts
is put together by the node's C++ in `init()`.
- **It must fit.** A literal longer than the site's capacity, or a
  `string(M)` setting copied into a `string(N)` site with M > N, is `SE0427`.
  The capacity counts bytes.
- **Text and numbers don't mix.** A literal or a string setting at a numeric
  or finite site, or anything but those two at a string site, is `SE0428`.

### 7.3 Builtin functions and constants

| | |
|---|---|
| Constants | `pi`, `e`, `inf`, `sample_rate` (§10.4), `time_step` (§10.4) |
| Arithmetic | `abs`, `min`, `max`, `sign`, `floor`, `ceil`, `round`, `mod` |
| Powers | `sqrt`, `cbrt`, `pow`, `exp`, `log`, `log2`, `log10`, `hypot` |
| Trigonometry | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2` |

`sin`, `cos`, and `tan` take an **angle** (§4.5) and return a dimensionless value;
`asin`, `acos`, `atan`, and `atan2` return an **angle**. `sqrt` halves the
exponent vector and requires the result to be rational. `min`, `max`, `hypot`,
`atan2`, and `mod` require compatible arguments. `log*` and `exp` require
dimensionless arguments. Anything else is `SE0412`.

Evaluation is in `double`. Division by zero, a domain error, or a non-finite
result is an elaboration error (`SE0413`) — the language is total by rejection,
not by producing NaN.

### 7.4 What is deliberately absent

No conditionals, no comparisons, no string operations, no user-defined functions,
no references to run-time signals, and no references to other instances. The
language stays small so that (a) elaboration is guaranteed to terminate, (b)
settings can be topologically sorted, and (c) every expression emits as **one
C++ expression over `double` plus `<cmath>`**, with no temporaries, no control
flow, and no runtime evaluator.

(c) used to read "folds into the generated code," which §6.2b changed. What the
restriction actually buys is unchanged and is now doing more work: an expression
language with conditionals or loops could not be emitted as straight-line code,
so `resolve_settings` would need an interpreter and a dependency graph at run
time instead of a flat sequence of assignments (§15.5).

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
| `derivative()` | ẋ = f(t, x, u) | **pure** | Every minor step (4× per RK4 step). |
| `next()` | x⁺ = h(t, x, u) | mutating | Once per accepted step, every tick. |
| `final(ctx)` | — | mutating | Once at run end, in reverse init order. |

Write permissions, exactly:

- `init()` writes `state` (the IC escape hatch) and `var`; may touch natives; may
  log; may halt.
- `output()` writes `out` only — and an output group (§8.3a) only the outputs it
  names.
- `derivative()` writes `der` only.
- `next()` writes `next` and `var`; may touch natives; may log; may halt.
- `final(ctx)` writes nothing model-visible; may touch natives; may log.

**`next()` is the node's one per-tick hook, and it does two jobs in one body.** It
is the jump map, x⁺ = h(t, x, u), for a state of any kind (§6.4). It is also
where a node does per-tick work that is no equation at all: read a socket, keep
a count in a `var`, log, stop the run. A node with no state may define it for the
second job alone. It runs exactly once per accepted step and at no trial state,
which is what lets it have side effects where `output()` and `derivative()` may
not (§8.4).

Its two kinds of write land at different moments, and this is deliberate. A
`next.<state>` write goes into a buffer and lands at the commit, together with
every other node's (§9.6). A `var` write lands at once, and a statement below it
in the same body reads the new value: it is the node's private bookkeeping.

Slower work is written in the body, against the tick counter:

```
next() { if (sim.tick() % var.poll_ticks == 0) poll(); }
```

Methods that write model values are named for what they write — `out.` →
`output()`, `der.` → `derivative()`, `next.` → `next()`. `init()` and `final()`
carry no equation and are named for the moment they fire. The singular follows
the accessor rather than the section, which is why `output()` is singular though
`outputs {}` is plural.

"Mutating" above describes what the method may change in the *model*, not the
constness of the generated C++ member. `output`, `derivative`, `next` and `final`
all lower to `const` members, and each receives what it may write as an
out-parameter — `out`, `der`, `next`, `var`. Only `init()` is non-const, because
it writes its own storage directly. See §15.4: that uniformity is what makes the
write permissions above compile-time facts rather than documented conventions.

`next()`'s out-parameter is named `next`, shadowing the member function inside its
own body. That is deliberate and harmless: §11 copies bodies verbatim, so a body
writes `next.<state>` and it resolves to the parameter. The accessor token cannot
be spelled anything else (§6.10).

### 8.2 Why `output()` and `derivative()` are separate

They cannot be merged, and this is not a stylistic point: **the split is the only
reason feedback loops are simulable.** A node with no feedthrough has an
`output()` that needs only `state`, so it can be called *before its inputs are
known* — which is exactly what unwedges a closed loop. If `derivative()` also set
outputs, every node would look like a feedthrough node and loop-breaking would be
destroyed everywhere in the model.

### 8.3 Feedthrough is declared by method signature

```
output(cmd)      { out.force = state.force + param.kff * in.cmd; }  // feedthrough on cmd
output()         { out.velocity = state.velocity; }                 // breaks loops
derivative(cmd, load) { … }                                         // constrains nothing
next()                { … }                                         // constrains nothing
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
  algebraic loops whenever a node has one feedthrough input and one derivative-only
  input.
- **"Algebraic" and "feedthrough" are orthogonal.** A node can have states *and*
  feedthrough — a PID, or any state-space block with D ≠ 0 — so feedthrough
  cannot be derived from the leaf/composite inference.
- Only `output()`'s list constrains ordering. `derivative()` and `next()` run
  after outputs have propagated, so their lists are documentation plus
  enforcement.
- Granularity is per-port on the output side too, where a node asks for it:
  §8.3a splits `output()` into groups, each with its own list.

### 8.3a Output groups

One `output()` gives a node one feedthrough set, and that set applies to every
output. A node whose outputs depend on different inputs therefore looks, to the
sort, like a loop that is not there. The standard case is a reduction between
two bodies: the speed goes one way through it and the torque comes back.

```
node FinalDrive {
    settings { ratio: double(-); }
    inputs   { w_s: double(rad/s);   tau_out: double(N*m); }
    outputs  { w_out: double(rad/s); tau_s:   double(N*m); }

    output w_out(w_s)     { out.w_out = param.ratio * in.w_s; }
    output tau_s(tau_out) { out.tau_s = param.ratio * in.tau_out; }
}
```

Written as one `output(w_s, tau_out)`, `w_out` would feed through `tau_out`,
and the transmission that turns `w_out` into `tau_out` would close a cycle:
`SE0510`. The only way out used to be splitting the node in two — a composite,
two leaves, and every setting declared twice — for no reason the physics gave.

- **An output method may name the outputs it writes**, between `output` and
  its parameter list. That is an *output group*. Its parameter list is the
  feedthrough set of exactly those outputs; several are written
  `output a, b (u, v) { … }`.
- **`output default(...)` writes every output no group names**, the way
  `switch`'s `default:` takes every case no label names. It appears exactly
  when groups do: beside groups, the catch-all says so, and a bare `output()`
  there is `SE0356`; without groups, the one output method writes every output
  and is the plain `output()` of §8.3, and an `output default()` there is
  `SE0357`. So a node that never names a group is unaffected by any of this,
  and in one that does, every output method's head says what it writes.
  `default` is not reserved: it is a C++ keyword, so `SE0332` already keeps it
  from naming an output, and after `output` it can mean nothing else. It
  stands alone — `output default, a(...)` is `SE0201`.
- **Each output is written by exactly one method.** Named by two groups, or
  twice by one: `SE0354`. A group naming something that is not an output of
  the node: `SE0353`. An `output default()` left with nothing to write:
  `SE0355`. An output that no method computes: `SE0340`.
- **Only `output` takes a name list.** Every other method writes a whole
  accessor, so a list there would select nothing (`SE0201`).
- **The sort's vertices are groups** (§8.5). A group with an empty list is a
  sort root, and a group may read an input wired from another group of the
  same node, as long as no cycle runs through them.
- **Holding is per node.** A held node (§9.3) holds all of its groups: each is
  evaluated at the tick and none at the solver's stages, so a
  hold never splits a node's outputs across two instants.

**Sharing goes through helpers.** Outputs that need a common intermediate
either stay in one group or each call a helper (§11.4) that computes it. A
helper receives what its caller passes, and a group can pass only the inputs
its own list declares — the others do not exist in its `In_` struct (§15.4) —
so a helper cannot carry a dependency past the sort. The price is
recomputation, which is also what `output()` and `derivative()` pay today when
both need the same intermediate at one stage, and it buys something: each call
computes from the values of that moment, so nothing it returns can be stale.

A hand-made cache does not have that property, and `const` does not stop one.
A `mutable` member or a `static` in a helper survives from one pass to the
next, so a value computed at an RK4 stage, at a trial state the trajectory
never visits, can reach the next tick's `output()`. The generator cannot see it, because bodies are verbatim. A compute-once intermediate that the
language manages is Appendix C 8e.

### 8.4 Why mutation is confined

`output()` and `derivative()` are evaluated **four times per RK4 step, at trial states
the trajectory never visits**. Any side effect there fires four times, at values
that are not on the solution. That single fact is the reason for three separate
restrictions, which are really one restriction wearing three hats:

- `var` writes → `init()`/`next()` only (§6.5).
- `log.*` → `init()`/`next()`/`final()` only (§10.1).
- `native` handles in scope → `init()`/`next()`/`final()` only (§6.7).
- `sim.stop`/`sim.abort` → `init()`/`next()` only (§10.2).
- `sim.require` → `init()`/`next()` only (§10.6): it halts, so it is
  confined where every other way of halting is.

`next()` is on the permitted side because the hazard does not reach it: it runs
once per accepted step, at the tick's entry state, which is on the trajectory.
Its body runs above the recording and the exit (§9.6), so a halt takes effect that
tick, and its `next.` writes are buffered until below the exit, so a run that halts
never commits them.

A `sendto()` in `output()` would put four packets on the wire per step, at values
the model never actually took, silently. Confining mutation keeps results
solver-independent and reproducible.

**Reading is not on that list, and `sim.*` reflection is the case that shows
why.** `sim.time()` is legal in every method (§10.5): the hazard here is a side
effect repeated at states the trajectory never visits, and a read has no side
effect to repeat. Evaluating y = g(t,x,u) four times is not a problem — it is
what a Runge-Kutta stage IS, and the language already hands those methods `x`
and `u` at exactly those trial states — and the helpers they call `x` too
(§11.4).

`sim.check_*` (§10.6) is legal in every method for the same reason `log.trace`
is: it is observational. It fires four times per step when a contract is
broken, and that is *correct* — the four are counted, five are reported and
the rest are a total. What it never does is change the model or end the run;
the form that ends a run is `sim.require`, on the line above.

### 8.5 The sort

Build a graph over the nodes active on the current tick:

> **edge M → N** iff M sources one of N's **feedthrough** inputs.

Strictly, the vertices are output methods rather than nodes: in a node with
output groups (§8.3a) the edge runs from the group that writes the source port
to the group whose list names the input, and a node without groups is one
vertex, as before.

- `output()` runs in **topological order** of that graph.
- `derivative()` and `next()` may run in **any** order once outputs have
  propagated.
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
   = help: drop `u` from `act.output(u)` and latch it in next(), or
           insert a unit delay / integrator anywhere in the cycle
```

Beating "Algebraic loop detected involving block X" is an explicit goal.

When the method through which the cycle passes a node writes more than one
output, the loop may be §8.3a's false one: the method lists the input the
cycle enters on for the sake of some of its outputs, and the cycle leaves on
another. The help names that fix first:

```
   = help: if `w_out` does not need `tau_out`, give it its own group in
           `FinalDrive`: `output w_out(w_s) { ... }` (§8.3a)
```

The generator cannot tell whether `tau_s` needs `w_s`, since bodies are
verbatim, so the help is conditional; it is offered whenever the method writes
more than one output.

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

- Every node runs **every tick**, at the base rate.
- It **eliminates most of Simulink's sample-time machinery**: no step-size
  negotiation, no forcing a variable-step solver to hit sample instants, no
  zero-crossing search for sample hits, and **no fractional resampling ever**.
- It unifies the model: everything is discrete at the base rate, and one
  scheduler serves the whole model.

It also retroactively justifies rejecting algebraic loops (§8.6) — a Newton solve
has no statable worst-case execution time.

### 9.2 One rate

There is one rate, the base rate, and every node runs at it. The signal
processing a model does happens every tick. Work that is genuinely slower — how
often to poll a device, say — is written in `next()` against `sim.tick()` (§8.1),
as the special case it is; it does not need a schedule.

A purely algebraic node — `Gain`, `Sum` — is re-evaluated in every `output()` pass
from whatever its inputs currently hold.

### 9.3 Discrete outputs

Discrete outputs are **zero-order held** across the step: `output()` is evaluated
once, at the tick, and the value stands until the next tick.

**A node is held when it has a `discrete` state**, and in no other case. A `finite`
state (§6.4c) does not hold its node, although it too moves only between steps:
its new value is committed before the advance, so the stages see one value across
the whole step, and nothing a held output would protect is at risk. A leaf may
therefore keep a gear beside a torque computed from a continuous speed, and the
torque is re-evaluated at every stage.

The commit is above the solver, so the stages run with *dis*[k+1] already in place.
An unheld discrete node would publish its next value inside the step. Holding is
what keeps its published output at y[k] for the whole of it, and that includes a
direct-feedthrough node, whose output would otherwise follow its inputs through
the trial stages.

A held output returns the **same value across all RK4 trial substeps**, so the
multiple-evaluation hazard of §8.4 never touches discrete nodes at all. This is
guaranteed by *omission*: a held node's `output()` is not re-evaluated at the
solver's minor steps (§9.6 step 7), so nothing that moves within a step —
`sim.time()` above all — can reach it.

A node with no state of its own to hold still can be made held by giving it a
`discrete` state that nothing writes, which is what `se.sig.src.Step` and
`se.sig.src.Ramp` do.

Two idioms, both expressed with the existing signature mechanism:

```
// Registered (the default idiom): one sample of latency, sort root, loop-breaker.
// Physically honest — a real digital controller has exactly this latency.
states { discrete cmd: double(-) = 0.0; }
next(err) { next.cmd = param.kp * in.err; }
output()  { out.cmd = state.cmd; }

// Direct (Mealy): zero added latency, but a feedthrough node subject to
// ordering, and able to form an algebraic loop.
output(err)  { out.cmd = param.kp * in.err; }
```

### 9.4 Real-time pacing

**Pacing belongs to the host, not to the model.** `tick()` computes one base step
and returns; it never waits. A host that wants the model to keep pace with wall
time ticks it some number of times and then calls `sync_realtime()`:

```cpp
while (!sim.done()) {
    for (int k = 0; k < 10 && !sim.done(); ++k) sim.tick();   // 10 ms of model time
    sim.sync_realtime();                                       // wait for the wall
}
```

`sync_realtime()` busy-waits on the high-resolution timer until the wall time
since the **first `tick()`** equals `time()`, the instant the next tick will
sample. How many ticks go between calls is the host's choice, and need not be
the same every time. At 1 ms `step` and ten ticks a call, that is ten steps flat
out, then spin.

- **The anchor is the first tick, not `init()`.** A host that does slow work
  between `init()` and its loop does not start the run behind.
- An **overrun** is a call that finds wall time already past `time()`: the ticks
  since the last call took longer than the model time they covered. It returns
  without waiting, and so do the calls after it until the model is level with
  the wall again. **The model catches up**; it does not drift.
- **Catching up has no limit.** A stall of any length — a breakpoint, a host that
  stopped ticking — is followed by ticks run flat out until the lost time is
  made good. There is no call to move the anchor; one is additive if a host
  that pauses on purpose ever needs it.
- **Every base step always runs.** Dropping steps to catch up was rejected: it
  would punch a hole in a fixed-step integration, advancing state by one `dt`
  while wall time advanced by several. Catching up is late; a corrupted
  trajectory is wrong.
- **Overrun is instrumented, not policed** — no abort escalation and no policy.
  See §13.6 for the metrics.

**Why not in the sim file.** Earlier versions put `mode: realtime`, `sync` and
`window` in the `.sim`, and the generated `tick()` waited at every frame
boundary. A host could then not run a real-time model flat out — in a test, say
— without editing the experiment, nor pace a batch one, nor choose its own
frame. Whether a run keeps pace with the wall is a property of how it is run,
not of the model or the experiment, so it moved to the host. The three keys
are now an error (`SE0265`) rather than unknown entries, so an old `.sim`
says what happened to them. The emitted example host takes the frame from its
command line as `--realtime SYNC` (§12.4).

### 9.5 The solver

The solver is **global, not per-node** (`solver: rk4;`). Mixing integrators
across one state vector was deliberately kept closed. Normative values are
`euler`, `rk2`, and `rk4`; the menu beyond that is an open question.

**A jump of a continuous state (§6.4b) constrains what may join that menu**, and
the constraint is recorded here rather than discovered later. All three normative
solvers are **one-step and fixed-step**, so there is no history for a jump to
invalidate: step 7 begins from whatever `x` step 6 left, and nothing older is
consulted. A multistep method carries past states and past rates, and an
adaptive one carries an accepted step size; a jump between steps makes both
stale. Either would have to restart its history whenever a `next()` changed a
state — which is implementable, and is a cost the solver must pay
rather than a reason to forbid the method.

### 9.6 The tick

One base tick at index *k*, normatively:

1. **Set the time** to *t*[k] — which is what `sim.time()` returns for the rest
   of this step, except where step 7 moves it. When an inputs file drives the
   run (§13.8), write the root inputs it names, at *t*[k], over whatever the
   host wrote.
2. **Propagate outputs.** `output()` in topological order, at the tick's entry
   state.
3. **`next()`** on every node that defines it, in any order, unless the run has
   already been halted. Each reads `state` and `var` and this tick's inputs, and
   writes the buffers through `next.` (§6.4).
4. **Record** (§13.5) if this tick is a recording tick.
5. **Exit** if the run has been halted (§10.2) or *k* is the last tick.
6. **Commit.** The buffers land together: `dis = nxt` and `fin = fnx`, and `x = xnx`
   in a model where a `next()` can reach a continuous state. A model with
   array-shaped discrete states has a second whole-block copy for `dis_arr` (§15.5).
   This is the tick's only write to any state outside the solver.
7. **Flow.** `x` by the solver's combination of stage rates. Each stage sets the
   time to that stage's instant, re-propagates outputs, and evaluates
   `derivative()`, at trial states the trajectory never visits (§9.5). **Only nodes
   that are not zero-order held are re-propagated**: a held node keeps the output
   it published in step 2, which is what makes the hold a hold, and `sim.time()`
   moves for the others alone (§10.5). `derivative()` runs at every stage on every
   node that has one, held or not.

A tick never waits on wall time. Keeping pace is a separate call the host makes
between ticks (§9.4).

Two properties are load-bearing, and both come from moving state last.

**The whole tick observes one state.** Steps 2–4 all see *x*[k], *dis*[k] and
the finite block as the tick found it, so one recorded row is one consistent
sample of the model at *t*[k] — signals and states of every kind alike. The same
reasoning is why `output()` sees *dis*[k] rather than *dis*[k+1]: y[k] = g(x[k])
precedes x⁺ = h(x[k], u[k]), which is what discrete-time state space says.

**A stopped run holds a valid checkpoint.** The exit at step 5 is above the
commit and the flow, so a run ending on `sim.stop`, on `sim.abort` or on the tick
count leaves `x` + `sig` + `dis` + the finite block mutually consistent — the
completeness §15.5 claims for that set.

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
- **Restricted to `init()`, `next()`, and `final()`** for the reason in §8.4.
- **Escape hatch:** `log.trace` alone is permitted in the two pure methods.
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

- Callable from `init()` and `next()` — the mutating, I/O-capable methods.
- **Halting is the `sim.*` family's job.** Overloading `log.fatal` with control
  flow was rejected: a logging call that sometimes ends the program is exactly
  the hidden side effect the purity rules fight everywhere else. A fault site
  writes two honest, greppable lines instead of one overloaded one.
- This mirrors the recording (quantitative) / logging (qualitative) split, and it
  gives `final()` a **single uniform source** of termination reasons with a clean
  severity axis; log level stays orthogonal metadata.

**These two are the only members of `sim` confined to the mutating methods.**
The rest of the namespace — `sim.time()` and the other reflection members — is
readable from every body; §10.5 sets out the surface and the rule that admits a
member to it. The split is carried by the type, not by a runtime check: a leaf
holds a reflection-only handle, and `init()`/`next()` shadow it with a wider
one, so `sim.stop` inside `output()` is *no such member* — a compile error at
the `.se` line, like every other confinement in §8.4.

`final()` deliberately gets the narrow handle too. It already learns the outcome
from `ctx` (§10.3), and halting a run that is already ending is not something
this section offers.

Note that `sim.*` being a *recordable* namespace in the sim file (§13.6) still
does **not** make those metrics readable from node code. `sim.overrun_count` and
`sim.frame_slip` fail §10.5's admission rule, and that is the whole of the
objection this section used to state as a blanket refusal.

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

A `sim.require` failure (§10.6) arrives here as `ctx.reason == "error"`, with the
contract named in `ctx.message`. A non-halting `sim.check_*` violation does not
touch `ctx` at all — the run completed — but it does still make the process exit
code nonzero, which is the distinction §10.6 draws between reporting and halting.

`final()` runs on normal and error termination alike, in reverse init order, but
**only for nodes whose `init()` completed** — a node that never initialised cannot
be asked to tear down. RAII on C++ members is the quiet default for nodes that
write no `final()` at all.

### 10.4 `sample_rate` and `time_step`

Two constants in the §7 expression language, giving the base rate:

| | | |
|---|---|---|
| `sample_rate` | `(Hz)` | 1 / `step`, from the sim file |
| `time_step` | `(s)` | `step`, from the sim file |

```
node Biquad {
    settings { wn: double(rad/s);
               fs: double(Hz) = sample_rate; }   // 1 kHz base step -> 1000
}
```

A discrete filter cannot compute its coefficients without its own sample period —
`K = 2·fs` in a bilinear transform.

- **Both spellings ship** because coefficient design wants a rate while anything
  expressed as a period wants the reciprocal. Both derive from one number, so
  `sample_rate * time_step` is exactly 1 and they cannot drift apart.

These are **configuration-time constants in the §7 expression language**, and
that is what keeps them distinct from §10.5's `sim.step()`. `sample_rate` is
fixed before the run starts and legal in a setting default; `sim.step()` is the
same base step read at run time from a body. Neither substitutes for the other.

### 10.5 `sim.*` reflection

Every body — pure and mutating alike — may read the run's current instant and
the node's own identity:

| | | |
|---|---|---|
| `sim.time()` | `(s)` | the instant at which this method is being evaluated |
| `sim.tick()` | integer | base tick index *k* |
| `sim.step()` | `(s)` | the model's global base step |
| `sim.path()` | text | this node's model path, e.g. `whl.tc` |
| `sim.type()` | text | this node's fully qualified type, e.g. `se.sig.src.Ramp` |

**The admission rule.** A member belongs here iff its value is **well defined at
every invocation** and is a function of the current instant or of the node
itself — never of run history. That rule is doing real work at the boundary:
`sim.overrun_count` and `sim.frame_slip` (§13.6) are excluded by it, because
what they read depends on when you look, and a body that samples one is
recording the schedule rather than the model.

Time was withheld from bodies for a long time as a side effect of not opening
this namespace. It never met that objection: §8.1 writes the equations as
y = g(t,x,u), ẋ = f(t,x,u) and x⁺ = h(t,x,u), so the language supplied `x` and
`u` and withheld the `t` its own formulation names, and every source in
`se.sig.src` carried a redundant `discrete t` clock to put it back.

**Which instant `sim.time()` returns.** The one this evaluation is at — nothing
finer to state, because the *schedule* rather than the value carries the
distinction:

- A node with a `discrete` state is zero-order held across the step (§9.3); a
  `finite` state does not make it one. Its `output()` is evaluated once, at the
  tick, and is **not** re-evaluated at the solver's minor steps; `sim.time()`
  there is the tick instant, identical for the whole step, and its held output
  stays held.
- Every other node — continuous or purely algebraic — **is** re-evaluated at
  each minor step, and `sim.time()` returns *t* + *c·h*, the instant the solver
  is actually at.

The second bullet is the point. Without it a **continuous source cannot exist**:
its output would be a staircase, and a fourth-order solver integrating it would
collapse to first order across that path no matter how good the integrator is.
`se.sig.ct.Sine` is the first block to depend on this, and `se.sig.src.Sine` is
the held twin that deliberately does not.

**A body's use of `sim.*` is invisible to the compiler**, because bodies are
verbatim and are never parsed (§11.1). The schedule therefore assumes every body
reads time, which is why holding is decided by the node's states and never by
what a body happens to contain.

`sim.stop` and `sim.abort` are **not** part of this surface; they stay confined
to `init()` and `next()` by §10.2, and the confinement is carried by the
handle's type rather than by a rule an implementation has to remember.

`sim.check_*` (§10.6) **is** part of it, and meets the admission rule the same
way `sim.time()` does: what it reports is a function of the values in front of
it at this instant. `sim.require` is not, for `sim.stop`'s reason — it halts —
and rides the same widened handle.

### 10.6 Contract checks

A **contract** is a claim a body makes about its own inputs, states or outputs —
this quaternion is unit-norm, this gain is finite, this duty cycle is in [0,1].
Today those claims live in header comments, and when one is broken the model
produces plausible numbers and the first sign of trouble is a recorded column
that is wrong by a factor nobody notices.

Observational, legal in **every** method:

| | On failure it reports |
|---|---|
| `sim.check(ok, "contract")` | the contract |
| `sim.check(ok, "contract", v)` | …and the value that decided it |
| `sim.check_finite(v, "contract")` | the non-finite value |
| `sim.check_range(v, lo, hi, "contract")` | got, the interval, and the overshoot |
| `sim.check_near(a, b, tol, "contract")` | got, want, \|err\|, tol |

Halting, and confined to `init()`/`next()` by §8.4:

| | |
|---|---|
| `sim.require(ok, "contract")` | reports, then ends the run with reason `error` |
| `sim.require(ok, "contract", v)` | …carrying the value into the report |

Each returns the condition, so a body that cannot proceed on bad input can say
`if (!sim.check_finite(in.a, "…")) return;`.

**The overloads that take values are the point of the family.** A body can always
write `check(fabs(n - 1.0) < tol, "unit norm")`, but the report can then only say
the condition was false. `check_near(n, 1.0, tol, "unit norm")` says what `n` was
and by how much it missed, and that is the difference between a diagnostic and a
notification.

**Reporting is observational, on §10.1's rule and for §10.1's reason.**
`output()` and `derivative()` are evaluated at every minor step of every tick, so
a check that halted there would both break §10.2's confinement and end runs over
trial states the trajectory never visits. The halting form is `sim.require`, and
it lives on the same handle `sim.stop` does — in `output()` it is *no member
named require*, a compile error at the `.se` line, while `sim.check_*` on the
next line still compiles.

**A violated contract still fails the run.** Teardown prints one line per
violated site — the contract, the node path, how many times it fired, when it
first fired and with what values — and the process exit code is nonzero even
when nothing halted. That is what makes a contract usable as an oracle rather
than as something a harness has to go grepping a log for.

**Reports are throttled per site, and that is not a nicety.** A contract broken
for a whole run at 1 kHz reports tens of thousands of times, and the first
report — the only one that locates the cause — is the one that scrolls away.
Five reports per (node, contract) pair, then silence and a count in the summary.

A contract is a claim about the **model**, not about the compiler. That is why it
is checked at run time, in a named block instance, at a sim instant, rather than
at generation: `sec` can tell you that a unit does not balance (§5), but only the
run can tell you that the quaternion arriving on this wire drifted off the unit
sphere at t = 4.31 s.

```
node QuatToRot {
    output(q) {
        sim.check_near(norm4(in.q), 1.0, 1e-9, "input quaternion is unit-norm");
        ...
    }
}
```

```
[    0.010500] error drive.q2r: contract failed: input quaternion is unit-norm;
               got 1.0349, want 1, |err| 0.0349 > tol 1e-09  (se.kin.QuatToRot, tick 10)
...
[    2.000000] error sim: 1 contract(s) violated
[    2.000000] error sim:   drive.q2r: input quaternion is unit-norm -- 8004 time(s),
               first at t = 0.0105 s (tick 10); got 1.0349, want 1, |err| 0.0349 > tol 1e-09
```

The fractional instant in that first report is not a typo: a contract in a pure
method sees the solver's clock (§10.5), so it names the minor step the violation
actually occurred at rather than the tick that contains it.

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

**A helper sees the node as its caller does, at that moment.** It is a member,
so what it reads through `this` is what the calling method reads, and this is
guaranteed rather than incidental:

- `state` is the **trial state** when the caller runs at a solver stage —
  `derivative()` always, `output()` in a node that is not held — and the
  step's state otherwise. Only continuous states have a trial value; discrete
  and finite ones read the tick's, as they do for the caller. `state` is a
  view into `x`, not a parameter, and every solver puts each stage's trial
  state into `x` (§15.5).
- `sim.time()` is the stage's instant, as it is for the caller (§10.5).
- `param` and `var` are what they always are: a stage moves neither.
- `in`, `out`, `der`, `next` and the other out-parameters are the caller's
  arguments, not members, so a helper sees them only when they are passed.
  That is what keeps §8.3a sound: a helper can read no input its caller's
  list did not declare.

So a helper that computes from `state` needs no argument for it, and gives the
same answer at a stage as the same expression written inline in the caller.

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
- **The header is the public interface, and nothing else.** `sec` writes five
  sources, named for the sim unless `--stem` says otherwise:
  - `<name>.hpp` declares `class Sim`: the ROOT node's settings, inputs and
    outputs as typed structs (with the record types they use), and the calls a
    host makes in order — configuration (`settings()`, `load_settings()` /
    `dump_settings()` / `list_signals()`, and the key-value, channel and log
    calls of §14.1), `init()`, `inputs()` / `tick()` / `outputs()` /
    `done()`, `finish()` — and, for a host that paces its run against wall
    time, `sync_realtime()` and `realtime_stats()` (§9.4, §13.6); and, for a
    host that watches a run, channel handles and the log queue (§14.1).
    `tick()` never waits. The model itself is `Sim::Impl`,
    incomplete in the header, so a host depends on no layout it cannot see.
    A `Sim` runs once: a second `init()` is refused, and a fresh `Sim` is how
    a host runs again. Nothing outlives a `Sim`, so a program may run a model
    any number of times, or several models side by side (§15.4).
  - `<name>.cpp` holds everything behind it: the engine runtime, the node
    classes, `Sim::Impl` and the schedule. It is ONE translation unit — the
    whole model — which keeps the unity-build property that matters: the
    model's flat call sequence is compiled in one piece.
  - `se_sim.h` and `<name>_c.cpp` are the C interface (§14.2). The header is
    the same text for every model. The source is a thin layer that uses
    `<name>.hpp` alone, with each `se_` call forwarding to the `Sim`.
  - `main.cpp` is an example host that uses the header alone. It is what the
    `run` and `exe` modes build, and `--no-main` suppresses it. It takes its
    whole configuration — settings, initial conditions, run length, channels
    and log — as a list of settings sources on its command line (§14.1). The
    run's length is `tick_count()`, `sim.duration` in steps; `step_seconds`
    stays a constant. It runs flat out unless
    given `--realtime SYNC`, a time that is a whole number of base steps, and
    then calls `sync_realtime()` after every `SYNC` of ticks. `run` and `test`
    pass any arguments after the script's own through to it.
- **Two scripts, one set of modes.** `build.ps1` for Windows and `build.sh` for
  a POSIX shell. They differ in quoting and in how an exit code is read, and
  take the same modes: `run` (build the exe and run it as the `.sim`
  describes; the default), `exe`, `lib` (a static library of the model and its
  C layer, plus both headers), `dynamic` (a DLL or shared object that exports
  the C interface of §14.2 and nothing else, plus `se_sim.h`), `test` (build
  in debug, run, fail unless the exit status is 0), `clean`, and `all` (exe,
  lib and dynamic). They take the same options:
  - a `release` or `debug` configuration;
  - a sanitizer build;
  - a statically linked C runtime;
  - a prefix for the C names (`-CPrefix dt_`, `--c-prefix dt_`, giving
    `SE_C_PREFIX`), for linking two models' static libraries into one program.

  On Windows the DLL's import library is `<name>_dll.lib`, so it can sit
  beside the static `<name>.lib` that `all` also builds. `test` alone sanitizes
  by default — finding bugs is its whole job, and it runs its own exe — and
  falls back to a plain build, saying so, where the compiler has no sanitizer.
  Elsewhere it is opt-in: it displaces other debug checks (`/RTC1` under MSVC),
  a sanitized `lib` imposes the sanitizer on its consumer, and a sanitized exe
  needs its runtime to be findable when it starts.
- **The compiler output and the C++ compiler's output stay apart.** Each script
  builds into `build/<config>/`, as `bin/`, `lib/` and `include/`, and creates
  it if it is not there. The generated sources can then be re-emitted, diffed
  or deleted without object files and executables mixed in among them, and a
  consumer of `lib` points its include and library paths at the last two
  unchanged. The output directory itself is likewise created by `sec`, so
  `--emit -o <dir>` does not require the caller to have made `<dir>` first.
- The scripts are **full builds only**. They never track changes and never
  build incrementally or in parallel. That is not an omission: the model is one
  translation unit and the example host is another, so there is nothing for
  incremental tracking to save.

---

## 13. The sim file

The experiment configuration lives in **its own file**, keyword `sim`:

```
sim Demo {
    root:     Vehicle { total_mass = 1500 (kg); };
    step:     1 (ms);
    solver:   rk4;
    duration: 10 (s);

    settings: "vehicle_heavy.settings";

    record: {
        file  = "run.csv";
        every = 10;
        plant.out.velocity;
        plant.state.position;
        front.left.out.force;
        sim.utilization;
        sim.overrun_pct;
    }

    log: {
        file  = "run.log";
        level = info;
        levels { front.left = debug; }
    }
}
```

**Why separate from the model:** one model gets run many ways — 1 ms on target
versus 0.1 ms offline, a 10-second manoeuvre versus a 10-minute soak.
If configuration lived in the node, swapping experiments would mean editing the
model, and the model would stop being the thing under test.

### 13.1 Entries

| Entry | Form | Default |
|---|---|---|
| `root:` | `qualified_name [ '{' bindings '}' ] ';'` | required |
| `step:` | time expression | required |
| `solver:` | `euler` · `rk2` · `rk4` | `rk4` |
| `duration:` | time expression | required |
| `settings:` | string — a `.settings` path | none |
| `record:` | a record body, §13.5 | no recording |
| `log:` | a log body, §13.7 | console only |
| `inputs:` | an inputs body, §13.8 | the host drives every root input |

Each entry may appear at most once (`SE0260`). Whether a run keeps pace with wall
time is not an entry: it is the host's choice (§9.4), and the `mode`, `sync` and
`window` entries of earlier versions are rejected as removed (`SE0265`).
Block-valued entries (`record:`, `log:`, `inputs:`) take **no trailing
semicolon**; scalar entries require one.

**`record:`, `log:` and `inputs:` take exactly the body a settings source's
`record`, `log` and `inputs` sections take** (§14), so a block copies between a
`.sim` and a `.settings` file unchanged. The body of earlier versions — `file:` and
`every:` with colons, the channels inside `signals { }`, `levels { p: debug; }`
— is `SE0266`, and the help shows the new form. Each entry here is the bottom
layer of its kind: `duration:` is what `sim.duration` overrides, `record:` is the
selection a settings source's `record` section edits, `log:` is what a `log`
section edits, and `inputs:` is the file an `inputs` section replaces (§13.3).

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
wash.aout.x                  // a port of a composite, and a field of it
sim.overrun_pct              // an engine metric
```

The separator is the same `.` used for port access, and namespaces are
deliberately not visually distinguished — one addressing scheme, learned once.

### 13.3 Settings inflow

Resolution order, **last writer wins**:

1. Definition default (§6.2)
2. Flow-down expression at instantiation (§6.9.1)
3. The sim file's `root` block
4. The external `.settings` source (§14) presented to `sec`
5. The external `.settings` source presented to the **compiled model** at
   configuration (§6.2b)

Sources 1–4 are seen by `sec` and decide the values the generated code starts
with. Source 5 is the same grammar arriving later, at a carrier `sec` never sees
(§14). It is **opt-in**: a configuration source binds only the paths it names,
and every path it does not name keeps whatever 1–4 produced.

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

**The run itself layers the same way.** Four things about a run that are not
model values ride the same sources, in the same order:

| | bottom layer | edited by |
|---|---|---|
| the run's length | `duration:` | `sim.duration = 20 (s);` |
| the recorded channels, file and rate | `record:` | a `record { }` section (§13.5) |
| the log file and levels | `log:` | a `log { }` section (§13.7) |
| the inputs file | `inputs:` | an `inputs { }` section (§13.8) |

`sim` is reserved (§3.4), so `sim.duration` can collide with no model path. It
is a whole number of base steps, or `SE0267`; `step` itself is not settable,
because every rate in the model is an integer count of it (§9.2). Each source
applies its lines in order, and sources apply in the order given — the `.sim`'s
entries, its `settings:` file, then each source the compiled model is handed,
left to right. A dump (§14.1) writes all four, so loading it reproduces the
run.

### 13.4 Initial conditions

There is **no `initial:` section**, deliberately. Initial conditions are
declarative state defaults in the model (§6.4), and overriding one from outside
rides the **same settings channel, addressed by path**:

```
total_mass          = 1800 (kg);    // a setting
front.left.velocity =    2 (m/s);   // a state's initial value
bw.w1[3]            =    0 (-);     // one element of an array state (§6.4a)
dev.link            = CONNECTED;    // a finite state's, by enumerator (§7.2a)
```

Layering: declared state default → external override → `init()` code. **Every
override is in place before any `init()` runs** — settings and initial
conditions alike — because configuration is over by then (§6.2b). `init()` may
do real work: allocate, open a socket, size a buffer, solve for an equilibrium.
It does that work once, against the configuration the run will actually use, and
so it reads an overridden IC as the value it starts from and may refine it. A
state that `init()` writes regardless of its value is computed, not configured:
an override of it is accepted and then replaced, and a node that wants that
quantity to be tunable exposes the input to the computation as a setting
(`se.sig.src.Prbs` computes `reg` from `seed`). A `finite` state's initial value
rides the channel exactly as a `discrete` one's does, and one of finite type is
written as a bare enumerator.

**A record setting is addressed one leaf at a time** (§6.2c), and never as a
whole (`SE0460`):

```
cue.limit.z = 4.0 (m/s^2);       // ok
cue.limit   = { … };             // SE0460 — a record has no single value
```

This is the same rule as the one below it, for the same reason: §7 has no
aggregate value a running model could be handed, and §14.1's loader reads a
number. The leaf paths are exactly the ones §15.6 lists, so a `.settings` file
is checkable against the manifest without knowing that a record was involved.

**An array state is addressed one element at a time.** There is no path naming
the whole array, because §7 has no aggregate value to assign to it and the
declared default already initialises every element uniformly. An index past the
extent configuration computed is a configuration failure (§6.2b) rather than a
load-time parse error — the extent is not known until step 4, and the same
source may be valid against one `order` and invalid against another.

### 13.5 Recording

A recorded row is the model at *t*[k] for **every** kind of value it names —
signals and continuous, discrete and finite states alike. Recording happens
before any state moves (§9.6 step 4, above steps 6 and 7), so a row never mixes
vintages. On the tick a state jumps, the row still shows the old value;
the new one is in the next row, beside the signals it produced.

Recording is **quantitative**: an aligned numeric time-series table, distinct from
logging in every respect.

```
record: {
    file  = "run.csv";
    every = 10;                    // decimate: every 10th base tick
    plant.out.velocity;
    plant.state.position;
}
```

**The body is a list of edits to a selection**, applied in order:

| line | effect |
|---|---|
| `path;` | add the channel; one already selected keeps its place |
| `-path;` | remove it; one not selected is left alone |
| `clear;` | remove every channel |
| `file = "…";` | where the table is written; `run.csv` if never set; may hold `{time}` |
| `every = N;` | write every Nth base tick; 1 if never set |

The same body is a `record` section in a settings source (§14), and the
sections layer (§13.3): one file builds a baseline of channels, a later one
removes a few, and one that starts with `clear;` starts from scratch. Removing
an unselected channel is not an error, so a "turn these off" file layers on any
baseline. Columns are in the order their channels were added; the header names
each by the path as written where it was added. A table is written only when at
least one channel is selected, so `clear;` with nothing after it turns
recording off. `clear` is the one word the body reserves: a root port that is
literally named `clear` is not reachable by its one-segment path here.

**A file name may hold `{time}`.** When the run starts, `{time}` is replaced by
the local date and time it started, written `YYYYMMDD-HHMMSS`. There is one
instant per run, so a run's table and its log (§13.7) carry the same one.
- **A name that is taken is not overwritten.** If a file of that name already
  exists, `-2`, `-3`, … is added before the extension until one does not. The
  file is created exclusively, so two copies of one model started in the same
  second write two files, not one.
- **A name without `{time}` is opened as it always was**, replacing any file
  already there.
- **A dump writes the pattern**, not the name it produced, so loading a recipe
  gives the next run a file of its own. Once the run has started,
  `record_file()` and `log_file()` report the name actually used (§14.1).
- **`{time}` is the only placeholder.** Any other `{…}`, or a brace standing
  alone, is `SE0268`, and a compiled model refuses it with the same wording.

- **Signals are qualified paths** — `plant.out.velocity`, `plant.state.position` —
  reusing the `out.` / `state.` accessors. A bare `plant.velocity` is legal **when
  unambiguous**, and produces a loud error naming both candidates when it is not
  (`SE0461`), since a node may legitimately have a state and an output with the
  same name (§6.11).
- Recording internals directly was chosen over outputs-only recording, which
  would force boilerplate pass-through outputs just to observe a state.
- **A computed intermediate is recorded as an output nothing reads** (§6.3).
  States have storage and outputs have storage; a local in `output()` has
  none, so it gains a column by becoming an output. It belongs to its row:
  §9.6 step 2 writes outputs at *t*[k], before step 4 records, and the solver
  stages that re-run `output()` at trial states come after the record and are
  overwritten by the next tick's step 2. It costs a store when the outputs
  compute the value anyway; work done only for the column is repeated at every
  solver stage, as all of `output()` is.
- **A port is a signal at every level of nesting**, input or output, on a
  composite as readily as on a leaf — `wash.aout.x` names the composite's own
  output where `wash.sfSum.y.x` names the leaf that happens to drive it. Both
  are legal and both are the same column; the first is the one the author of
  `wash` wrote down, and requiring the second would make every recording depend
  on `wash`'s internals. The bare form needs no accessor and admits no ambiguity:
  §6.11 puts instance names and port names in **one** namespace, so a port path
  can never also be a leaf-member path.
- A port is **not storage**. A composite evaporates at elaboration and a wire
  coincides with the producer's member (§15.3), so a port path is resolved by
  chasing the producer chain — exactly the walk that builds a leaf's `in` view —
  and the column reports **what the node at that port actually reads**: the
  producer's storage with every hop's conversion applied, in the unit the *port*
  declares. Recording `wash.aout.x` where the wire into it converts therefore
  reads metres because `aout` says metres, not the producer's millimetres.
- The chain may end somewhere other than a signal, and all of them record. A port
  driven by a **setting** (§6.9.2) yields the setting, which is a live member: an
  override moves the column, so the file shows the value the run actually used. A
  port driven by a **literal** yields the folded number. A root input yields the
  boundary the host writes.
- An **undriven** port is `SE0304` when recorded. §15.6 may quietly omit a root
  output nothing drives — nobody has to read a manifest row — but a column has to
  hold something, so here silence is not available.
- **Records at the base step by default**; the first column is `time (s)`.
  `every = N;` decimates the file.
- A signal slower than the recording rate is **held (ZOH)** — which is its actual
  physical value, since that is precisely what downstream nodes see.
- The header carries units: `plant.out.velocity [m/s]`.
- **A finite type is recorded as its value**, and the header names the type,
  fully qualified, where a unit would go: `dev.state.link [device.LinkState]`, then
  `0`, `1`, `15` in the rows. The table stays numeric — every tool that reads
  the rest of it reads this column too — and the manifest's `[types]` row of
  the same name (§15.6) maps each value back to its name. A protocol code
  therefore appears in the file exactly as the device reports it. A `bool` records as 0 or 1.
- **A buffer is recorded as its size.** Its bytes are opaque (§5.1a) and a
  table of numbers has nowhere honest to put them, so the column is the one
  thing about a buffer that *is* a number, headed by the type where a unit
  would go: `rx.data [buffer(1500)]`, or `[buffer]` for a reader's port.
- **A string is recorded as its text** (§5.1b), headed `[string]` where a
  unit would go, whatever its capacity. Every cell of a string column is
  quoted the RFC 4180 way: the text between double quotes, a quote inside it
  doubled, and a comma or a line break kept as it is. Excel, pandas and every
  other CSV reader read that as it stands. Quoting every cell keeps the empty
  string, `""`, apart from an empty cell, and keeps a text column that happens
  to hold digits a text column.
- **An element of an array state is recorded by index** — `bw.state.w1[3]` — and
  the column is named that way. As with an override (§13.4), an index past the
  configured extent is a configuration failure, not a compile error; and since
  the header is written after configuration, the file's shape is settled by then
  and never changes mid-run.
- Wildcards (`plant.out.*`) are not in this version; see Appendix C. They would
  be worth more now than before: naming every element of an array state
  individually is exactly the case a wildcard removes.

**Every path the `.sim` could record can be chosen at run time** (§14.1). The
compiled model carries a table of every recordable channel — each leaf's
outputs, states and vars, each port at every level of nesting, the
`sim.*` metrics — and resolves a path against it exactly as `sec` resolves one
here: the same qualified and bare forms, the same ambiguity, and the same
conversions on a port's chain. An undriven port is not in the table. An array
state is one entry, its elements named by index at the extent configuration
settles. The `.sim`'s `record:` is therefore only the selection a run starts
with, and choosing another one needs no regeneration.

### 13.6 Real-time instrumentation

Two metrics, both dimensionless percentages reported on 0–100 with a `%` unit —
self-documenting in a CSV header, and `-` in the manifest either way:

| Metric | Meaning |
|---|---|
| `sim.overrun_pct` | Percentage of `sync_realtime()` calls that overran (§9.4). **A rate, not a count.** |
| `sim.utilization` | Time spent computing, as a percentage of the model time those ticks covered. |

- **`sync_realtime()` measures them**, because it is the only place the engine
  meets the wall clock. A run whose host never calls it — any batch run — reads
  zero in both. That is said here rather than diagnosed, because whether a host
  paces its run is no longer anything the sim file knows (§9.4).
- Both are computed over a **rolling window of the last second of model time**,
  not cumulatively. A since-start percentage is a lagging indicator: a fault
  starting twenty minutes into a run is buried under a long clean history —
  exactly the case a live value must catch. The window is counted in model time,
  not in calls, so it means the same thing whatever number of ticks the host puts
  between calls. At 10 ticks of 1 ms a call that is 100 calls, so 1% steps.
- Utilization can exceed 100: a call that overran spent longer computing than
  the model time it covered. Catching up does not inflate it, since a call that
  returns without waiting has still only computed for as long as it computed.
- **Both are recordable signals**, addressable as `sim.*` paths in a record body, so
  they land in the CSV beside model signals. That is what identifies *which*
  manoeuvre blows the timing budget, not merely that something did. Values
  between calls are held, which §13.5's rule already covers at no extra cost.
- **The host can read them too**, through `Sim::realtime_stats()`: the two
  metrics, the whole-run frame and overrun counts, and the **lag** — how far wall
  time was ahead of `time()` at the last call, zero when the model is keeping
  pace. A host driving hardware watches its margin live without recording it.
- A **whole-run cumulative summary** is emitted at teardown, free off the same
  counters, when the host called `sync_realtime()` at all.

### 13.7 Logging

Logging is **qualitative**: asynchronous, unaligned text and events.

```
log: {
    file  = "run.log";
    level = info;                    // global threshold
    levels { front.left = debug; }   // per-node-path overrides
}
```

Levels are `trace`, `debug`, `info`, `warn`, `error`. The console always receives
`warn` and above regardless of the configured threshold. The file format is plain
text; a structured alternative is an open question.

The same body is a `log` section in a settings source (§14), and sections layer
(§13.3): `file` and `level` are single values and the last writer wins; each
`levels` line sets one path's threshold, and a later line for the same path
replaces it. Per-node levels keep their own `levels { }` so that a node path
can never be mistaken for `file` or `level`. A log file is opened when the run
starts, after configuration. A host that gives each of two copies of one model
its own `file` therefore gets two logs, and so does one that gives both the
same `{time}` pattern: `file` takes `{time}` exactly as `record`'s does
(§13.5).

A host with no console has nowhere for the console's lines to go. It can ask
for them to be kept in a queue instead (§14.1), and the file is not affected.

### 13.8 Driving the root inputs from a file

A run's root inputs can come from a CSV table instead of from the host:

```
inputs: {
    file = "drive.csv";
}
```

The body takes one line, `file = "…";`. A later section replaces the file, and
`file = "";` means none: the host drives every root input, as it does with no
section at all. The same body is an `inputs` section in a settings source
(§14), and a generated application takes `--inputs FILE` as shorthand for one
(§14.1). The file is read, not written, so its name may not hold `{time}`
(`SE0269`); any other brace is `SE0268`, as in every file name (§13.5).

**The table is read whole when the run starts**, at `init()`, after
configuration. Anything wrong with it is a configuration failure (§6.2b):
`init()` refuses, and `error()` names the file, the row and the column.

```
time (s), axle_torque [N*m], ground_speed [km/h], gear [drive.Gear]
0,        0,                 0,                   1
2,        250,               36,                  2
2,        250,               36,                  3
12,       400,               90,                  3
```

**Columns are matched by name.**
- The first column is the time. Its header is `time` with a time unit in
  brackets or parentheses: `time (s)`, as the recorder writes it, or
  `time [ms]`.
- Each other column whose name is a root input drives that input. A record
  input is flattened to its fields, each named by its path, `ws.speed`, as the
  C interface names them (§14.2).
- **Any other column is ignored**, and one `info` line from `sim` names every
  ignored column. A table the recorder wrote (§13.5) therefore drives a run as
  it stands: its root input columns drive the inputs, and its other columns
  are passed over. A misspelt input name shows up in that line.
- A root input with no column is still the host's to write.

**A header's unit is converted.** A column's bracket is checked and
converted, as a unit suffix in a settings source is (§14.1), so a
`ground_speed [km/h]` column drives an `m/s` input. An incompatible unit is
refused. So is a missing one where the input has a dimension, because a bare
number there could mean anything. A dimensionless input takes `[-]`, `[]` or
no bracket. A finite input's bracket may be its type's name, as the recorder
writes it, or absent, and a string input's may be `string` or absent.

**The values are checked as the C interface checks them** (§14.2).
- An `int` input takes a whole number, and a `bool` input 0 or 1.
- A finite input takes an enumerator's value, as the recorder writes it, or
  its name.
- A string input takes the cell's text, which must fit the input's capacity.
  A quoted empty cell, `""`, is the empty string.
- An empty cell is refused.

**Between rows, a value is interpolated or held, by its type.**
- A `double` input is interpolated linearly between the last row at or before
  *t*[k] and the next row.
- An `int`, `bool`, finite or string input holds the last row at or before
  *t*[k], because halfway between two gears is not a gear.
- **Two rows with the same time make a step**: up to that time the value comes
  from the earlier row, and from that time on from the later one.
- Times may repeat but never go backwards.

**The file has to cover the start, and its end ends the run.**
- The first row must be at or before *t* = 0. A table that starts later is
  refused, since the values before its first row would be invented.
- The run ends at whichever comes first: `sim.duration`, or the last tick at
  or before the last row. Either way the reason is `completed` (§10.3). When it
  is the file that ends the run, one `info` line says so: `the run ended at 12
  s, where drive.csv ends`.
- `tick_count()` reports the shorter length once `init()` has read the table.
  Before that it reports `sim.duration`.
- A dump keeps both `sim.duration` and the file, so it reproduces the same
  length.

**A row meets a tick within a thousandth of a step.** A row time written as
text does not always read back as exactly *k* times the step, so a row within
a thousandth of a step of *t*[k] counts as at *t*[k]. The same tolerance
applies to the first row and to the last. It is deliberately small, so a table
sampled faster than the model is interpolated, never snapped to a nearby row.

**The file's columns win.** At step 1 of each tick (§9.6) the file's values are
written over whatever the host wrote to those inputs. One `info` line at
`init()` names the inputs the file drives and those it leaves to the host.

**A relative name is relative to the settings file that gave it.**
- A settings file loaded at run time through `load_settings_file` (§14.1),
  which is how the generated application loads `--settings`, resolves a
  relative name in its `inputs` section beside itself. A recipe directory can
  then carry its own data.
- Everything else resolves against the working directory: `--inputs`, text
  passed to `load_settings`, the key-value call, and the `.sim`. The `.sim`
  and the settings file it names are read when the model is generated, and
  the application runs somewhere else, so their names are kept as written.
- An absolute name is never joined.
- The dump writes the name as joined, so it reproduces the run from the same
  working directory. `inputs_file()` returns it the same way.

**The table's dialect** is RFC 4180's, as the recorder writes it (§13.5).
Fields are separated by commas. A field wrapped in double quotes may hold
commas and line breaks, and a doubled quote inside it is one quote. An
optional UTF-8 BOM is skipped, lines may end in LF or CRLF, and blank lines
are skipped. A row with the wrong number of fields is refused.

**A recorded run replays to the recorder's precision.** The recorder writes 9
significant digits, so a replay drives the model with each input as recorded.
That is the same value whenever the input was a number with 9 or fewer
significant digits, and otherwise its nearest 9-digit neighbour.

---

## 14. The settings source

The external settings source is a flat list of path-addressed bindings:

```
// vehicle_heavy.settings
total_mass          = 1800 (kg);
front.left.mass     =  340 (kg);
front.left.velocity =    2 (m/s);   // a state's initial value
rear.tc.slip_limit  = 0.08;
tx.host             = "rig-pc";     // a string setting (§5.1b)
```

```
settings_source  := { binding | record_section | log_section | inputs_section }
binding          := path '=' expression ';'
path             := identifier { '.' identifier } [ '[' integer ']' ]
record_section   := 'record' record_body          // §13.5
log_section      := 'log' log_body                // §13.7
inputs_section   := 'inputs' inputs_body          // §13.8
```

**File and string are one grammar, two carriers.** The same text may be passed
inline — as a command-line flag or through an API call — with no separate syntax.
Every value is unit-checked against its target (§4.3), and a string is written
as a literal with §3.6's escapes.

A source holds model values, run configuration, or both, in any mix and any
order: settings and initial conditions as bindings, the run's length as the
binding `sim.duration`, and the recording, the log and the inputs file as
sections whose bodies are the `.sim`'s own (§13.1). One file may carry all of them, or a run may be
assembled from several, each applied in turn (§13.3):

```
// outputs.settings — the channels for this study, from scratch
record {
    clear;
    file = "heavy.csv";
    plant.out.velocity;
    front.left.out.force;
}

// quiet.settings — layered after it: one channel fewer, less log
record { -front.left.out.force; }
log    { level = warn; }
```

Comments follow §3.2. There is no `package` and no conditionals, and the
three sections are the only nesting: this is deliberately the flattest surface that
can configure a whole run, because it is the layer that gets generated by
scripts and sweeps.

### 14.1 The third carrier: a compiled model

The same grammar is accepted by a **compiled model** at configuration (§6.2b),
which is the carrier `sec` never sees. A model exposes three operations:

| | |
|---|---|
| `load_settings` | apply a §14 source; each binding pins its target |
| `resolve_settings` | re-evaluate derived defaults and flow-down, skipping pinned |
| `dump_settings` | emit a §14 source describing how the run is configured |
| `list_signals` | emit a `record` section offering every channel the model has |

**The whole grammar, sections included.** A compiled model accepts the
`record`, `log` and `inputs` sections and the `sim.duration` binding as `sec`
does. A
section's channel paths resolve against the model's channel table (§13.5), with
`sec`'s qualified and bare forms and its wording for an ambiguous one; a
channel's array index, like an override's (§13.4), is checked when
configuration settles the extent.

**Record settings cost the runtime nothing** (§6.2c). A leaf is a flat path and
a number, which is what this section already describes, so nothing below changed
when records became settable.

**The runtime accepts a restricted right-hand side.** `sec` evaluates all of §7;
a compiled model accepts a **number with an optional unit suffix**, at a
path of finite type a **bare enumerator** (§7.2a), and at a path of string
type a **string literal** (§3.6, §7.2b), and nothing else — no arithmetic, no builtins, no `param.*`. Two reasons: the full evaluator
would have to ship inside every generated artifact, and `param.*` at this layer
would name a value that a later line in the same source may itself rebind,
requiring an evaluation order that the elaboration-time version gets for free
from its topological sort. A source containing an expression is rejected with the
offending path named; it is not silently truncated.

The unit suffix is **checked, not decorative**:

```
front.left.mass = 340 (kg);   // ok
front.left.mass = 340;        // ok — canonical unit, per the manifest (§15.6)
front.left.mass = 340 (m);    // rejected: L where M is declared
```

This is the check that `sec` applies at every other site (§4.3), applied at the
one site where the value arrives after units have been erased. It requires the
generated artifact to carry each setting's dimension and scale — which §15.6
already records — and it is why erasure at the C++ boundary (I3) does not leave
a hole here.

**`dump_settings` and `load_settings` round-trip.** Dumping a model, loading the
result into a freshly constructed one, and dumping again produces identical text.
The dump emits canonical units, enumerators by name, strings as literals,
and every settable path,
so it doubles as a
golden record of what a binary was configured with — and as the readable
inventory of what a model exposes, which no external document has to be kept in
sync with.

Structural settings (§6.2) appear in a dump, marked, and are **rejected on load**:
they are shape, and shape was fixed when the binary was generated. The mark is a
comment — the row is written commented out, with the reason beside it — so a
dump stays loadable and the round trip above holds for a model that has one.

#### The operations on a generated `Sim`

The operations are members of the generated `Sim` (§15.5), beside
`settings()`, and like it they belong to configuration — after construction,
before `init()`:

```cpp
bool        load_settings(const char* text, const char* origin = "settings");
bool        load_settings_file(const char* path);
const char* dump_settings();
const char* list_signals();
// resolve_settings is not a separate call: load_settings ends with it.
```

`load_settings_file` reads the file and loads it as `load_settings` would,
with the path as its origin. It differs in one way: a relative name in its
`inputs` section resolves beside the file (§13.8). A file that can't be read
is refused, with `cannot read PATH`, and like any refused source the refusal
stands.

A host that would rather not write text has the same operations one key at a
time, with structured answers. Each call is exactly one line of a source, takes
the same checks, fails with the same wording through `error()`, and layers as
that line would:

```cpp
// Settings, initial conditions and sim.duration: one row per settable path.
enum class SettingKind { Setting, Initial, Run };
struct SettingInfo {
    std::string path;          // "front.left.mass", "bw.w1[3]", "sim.duration"
    SettingKind kind;
    std::string unit;          // canonical (§15.6); "-" for none
    std::string type;          // "double", "int", "bool", "string", or a finite type
    double      value;         // current, in `unit`; an enumerator's value; NaN for text
    std::string text;          // a string setting's value; "" otherwise
    bool        structural;    // listed, never settable (§6.2)
};
std::vector<SettingInfo> settings_list() const;
double      get(const char* path) const;          // canonical unit; NaN if unknown
const char* get_text(const char* path) const;     // a string setting; nullptr if not one
bool        set(const char* path, double value);  // canonical unit
bool        set(const char* path, const char* value);  // "340 (kg)", "CONNECTED", rig-pc

// Channels: every recordable path (§13.5), and whether it is selected.
struct ChannelInfo {
    std::string path;          // "plant.out.velocity", "bw.state.w1[3]"
    std::string unit;          // the header's bracket: a unit, a finite type, buffer(N), string
    bool        selected;
};
std::vector<ChannelInfo> channels_list() const;
bool record(const char* path);                    // `path;`
bool unrecord(const char* path);                  // `-path;`
void record_clear();                              // `clear;`
bool record_file(const char* file);   const char*   record_file() const;
bool record_every(std::uint64_t n);   std::uint64_t record_every() const;

// The log (§13.7).
bool log_file(const char* file);   const char*   log_file() const;   // "" for console only
bool log_level(const char* level);                // the global threshold
bool log_level(const char* path, const char* level);

// The inputs file (§13.8).
bool inputs_file(const char* file);   const char*   inputs_file() const;   // "" for none
```

`get` and `set(path, double)` speak the canonical unit, so a sweep or an
optimiser never formats or parses; `set(path, text)` takes what a source line
would, unit suffix and all. The lists are taken after configuration has
settled what it can, so array rows appear at the extent the current settings
give. `record_file()` and `log_file()` return the name as configured, a
`{time}` pattern included (§13.5). Once `init()` has opened the file, they
return the name it actually opened. `inputs_file()` returns the name as it
will be opened, joined to a settings file's directory where §13.8 says so.

Two more pairs serve a host that watches a run rather than configures one.
Unlike everything above, they are **not configuration**: they work before and
after `init()`, and nothing they do appears in a dump.

```cpp
// A channel read live (§13.5): resolve it once, then read it each tick.
int         channel_handle(const char* path);     // -1 if refused; error() says why
double      read(int handle) const;               // NaN for a bad handle, or for text
const char* read_text(int handle) const;          // a string channel; nullptr if not one

// The console's lines, kept for a host that has none (§13.7).
enum class LogLevel { Trace, Debug, Info, Warn, Error };
struct LogLine {
    LogLevel    level;
    std::string path;          // the node, or "sim"
    double      time;          // s, model time
    std::string text;
};
void log_capture(bool on);
bool log_next(LogLine& line);                     // false when none is waiting
```

- **A handle names a channel, not a column.**
  - `channel_handle` resolves a path exactly as `record` does: qualified or
    bare, with the same wording for an ambiguous one.
  - The channel doesn't have to be selected, and a handle costs nothing on a
    tick that doesn't read it.
  - `read` returns the model at the instant the last `tick()` sampled. That is
    the same instant as `outputs()`, and the same value that tick's row
    records.
  - The value is kept, not looked up. By the time `tick()` returns, the
    states have moved on and the solver has overwritten the signals, so
    `tick()` stores each handle's value at the point where it records a row.
    A handle taken partway through the run reads NaN until the next tick has
    sampled it.
  - Before the first tick, `read` returns what `init()` left. Before `init()`
    the value is not meaningful.
  - An array element is checked against the extent when it is read. An
    element past the extent, like a handle the model never issued, reads NaN.
  - A string channel is read with `read_text`, which returns the text as the
    last tick sampled it. Its storage is set aside when the handle is taken,
    so reading never allocates during a run.
- **Captured lines are the console's.**
  - While capture is on, every line the console would have received goes to
    the queue instead. That means `warn` and above always, and, when there is
    no log file, everything that passes the threshold.
  - The whole-run summary of §13.6 is queued too, as an `info` line from
    `sim`.
  - The log file is not affected.
  - The queue holds 10,000 lines. Past that the oldest are dropped, and the
    next `log_next` first returns one `warn` line from `sim` saying how many
    were dropped.
  - Capture may be turned on and off at any time.

- **A load is all or nothing.** Every binding is parsed, resolved to a path and
  checked against its target before any is applied; if one fails, none is.
  `load_settings` returns false and `error()` names the origin, the line, the
  path and the reason, worded as `sec` words the same fault:
  `heavy.settings:3: front.left.mass: incompatible units: (m) where (kg) is
  declared`.
- **A failed load stops the run.** The failure is a configuration failure
  (§6.2b): it stands until the model is destroyed, and `init()` refuses to
  proceed while it does. A host that ignores the return value therefore cannot
  run a configuration it did not ask for.
- **A load after `init()` is refused**, for the reason §6.2b gives: settings are
  constant once a run has begun.
- **Loads layer.** Each binding pins its target, and a later load — or a later
  edit through `settings()` — is a later writer (§13.3). A load ends by
  re-resolving (§6.2b step 3), so what `settings()` and `dump_settings` show
  afterwards includes every derived value.
- **What a load may bind** is what §13.3 and §13.4 say an override may target —
  a setting at any depth, one leaf of a record setting, a scalar state's initial
  value, or one element of an array state — plus `sim.duration` and the three
  sections (§13.3), and nothing else. An output, a whole record, a whole array,
  and a path the model does not have are each rejected by name.
- **A bound value is converted, not only checked.** A suffix compatible with
  the target (§4.2, with §4.5's angle rule) is converted to the target's
  canonical unit, so `0.34 (t)` at a `kg` setting is 340, and `20 (degC)` at a
  `K` one is 293.15. The compiled model knows every unit symbol `sec` knows
  (Appendix A): the table it carries is generated from the same one.
- **`int` and `bool` targets take a number**, under elaboration's rules: a
  value that is not an integer is rejected at an `int` target, as `SE0402`
  rejects it (§3.5), and §7 has no boolean literal, so a `bool` setting is
  written `0` or `1` here as it is everywhere else.
- **A string target takes a quoted literal that fits.** Text longer than the
  setting's capacity is refused, naming both lengths, and never cut: only
  node code cuts (§5.1b). A number at a string path, or a literal at any
  other path, is refused.
  - `set(path, text)` at a string path takes the characters **as they are**,
    without quotes, since the call already carries exactly one value. A
    quote, a semicolon or a line break in it is just a character.
  - `get_text` reads a string setting. `get`, `set(path, double)` and `read`
    on text return NaN or refuse, and the error names the call to use.

`dump_settings` returns text owned by the model, valid until the next call. It
is **the recipe for the run**: the settings, in configuration order; the initial
conditions — every scalar state's, and each array-state element that a source
bound; `sim.duration`; a `record` section that starts with `clear;` and then
names the file, the rate and the selected channels in column order; a `log`
section; and an `inputs` section, `file = "";` when there is none. Numbers are written to round-trip exactly; units are the canonical ones
(§15.6); enumerators are written by name; strings are written as literals,
escaped as §3.6 says. Because the record section clears
first, loading a dump on top of any configuration reproduces the selection
exactly, column order included.

`list_signals` is **the menu**: a `record` section that starts with `clear;` and
lists every channel in the table, in tree order, each with its unit as a
comment — the selected ones as lines, the rest commented out. It is meant to be
saved, edited by uncommenting or deleting lines, and loaded back as a run's
channel file. It names channels only — no file and no rate — so it layers
beside a file that sets those.

A generated `main.cpp` (§12.4) is a host of this interface, and takes the same
text from its command line:

| | |
|---|---|
| `--settings FILE [FILE …]` | load §14 files in order, each through `load_settings_file`; the list may be comma-separated, and the flag repeated |
| `--set PATH=VALUE` | load one binding, `PATH = VALUE;`; repeatable, in order |
| `--duration TIME` | shorthand for `--set sim.duration=TIME` |
| `--inputs FILE` | shorthand for an `inputs` section naming `FILE` (§13.8), relative to the working directory |
| `--dump-settings` | after any loads, print `dump_settings()` and exit without running |
| `--list-signals` | after any loads, print `list_signals()` and exit without running |

Everything a run can be configured with therefore arrives through one list,
applied left to right:

```
mysim.exe --settings default.settings, thisrun.settings, initialcond.settings, outputs.settings
```

### 14.2 The C interface

A generated model is also callable from C, and through C from anything that
can load a shared library and call a function by name: C#, Python, LabVIEW,
MATLAB. A C++ class can't be found with `GetProcAddress` or `dlsym`, so `sec`
writes a C layer over the `Sim` of §14.1. The `dynamic` build (§12.4) exports
it from a DLL or shared object.

**Every model exports the same names.** This works as FMI's `fmi2…` functions
do. One wrapper, written once against `se_sim.h`, loads any model's library.
`se_sim.h` is the same text for every model. What differs between models is
asked for at run time: the model's name, its step, and its ports.

```c
#include <stdint.h>

#define SE_ABI_VERSION 1
typedef struct se_sim se_sim;   /* one instance of the model; opaque */

/* ---- the model: no instance needed ---------------------------------- */
SE_API int         se_abi_version(void);          /* SE_ABI_VERSION it was built with */
SE_API const char* se_model_name(void);           /* "Drivetrain" */
SE_API double      se_step_seconds(void);
SE_API int         se_input_count(void);
SE_API const char* se_input_name(int i);          /* "axle_torque", "cmd.gain" */
SE_API const char* se_input_unit(int i);          /* as a CSV header's bracket */
SE_API int         se_input_index(const char* name);   /* -1 if none */
SE_API int         se_output_count(void);
SE_API const char* se_output_name(int i);         /* "ws.speed" */
SE_API const char* se_output_unit(int i);
SE_API int         se_output_index(const char* name);

/* ---- life ----------------------------------------------------------- */
SE_API se_sim*     se_create(void);               /* NULL only if out of memory */
SE_API void        se_destroy(se_sim* s);         /* NULL is allowed */

/* ---- 1. configure: before se_init() --------------------------------- */
SE_API int         se_load_settings(se_sim* s, const char* text, const char* origin);
SE_API int         se_load_settings_file(se_sim* s, const char* path);
SE_API const char* se_dump_settings(se_sim* s);
SE_API const char* se_list_signals(se_sim* s);

SE_API int         se_setting_count(se_sim* s);   /* takes the list */
SE_API const char* se_setting_path(se_sim* s, int i);
SE_API int         se_setting_kind(se_sim* s, int i);   /* 0 setting, 1 initial, 2 run */
SE_API const char* se_setting_unit(se_sim* s, int i);
SE_API const char* se_setting_type(se_sim* s, int i);
SE_API int         se_setting_structural(se_sim* s, int i);
SE_API double      se_get(se_sim* s, const char* path);
SE_API const char* se_get_text(se_sim* s, const char* path);   /* NULL if not a string */
SE_API int         se_set(se_sim* s, const char* path, double value);
SE_API int         se_set_text(se_sim* s, const char* path, const char* value);

SE_API int         se_channel_count(se_sim* s);   /* takes the list */
SE_API const char* se_channel_path(se_sim* s, int i);
SE_API const char* se_channel_unit(se_sim* s, int i);
SE_API int         se_channel_selected(se_sim* s, int i);
SE_API int         se_record(se_sim* s, const char* path);
SE_API int         se_unrecord(se_sim* s, const char* path);
SE_API void        se_record_clear(se_sim* s);
SE_API int         se_record_file(se_sim* s, const char* file);
SE_API const char* se_get_record_file(se_sim* s);
SE_API int         se_record_every(se_sim* s, uint64_t ticks);
SE_API uint64_t    se_get_record_every(se_sim* s);
SE_API int         se_log_file(se_sim* s, const char* file);
SE_API const char* se_get_log_file(se_sim* s);
SE_API int         se_log_level(se_sim* s, const char* level);
SE_API int         se_log_path_level(se_sim* s, const char* path, const char* level);
SE_API int         se_inputs_file(se_sim* s, const char* file);
SE_API const char* se_get_inputs_file(se_sim* s);

/* ---- 2. start ------------------------------------------------------- */
SE_API int         se_init(se_sim* s);
SE_API const char* se_error(se_sim* s);           /* "" when there is none */

/* ---- 3. step -------------------------------------------------------- */
SE_API int         se_set_input(se_sim* s, int i, double value);
SE_API int         se_set_inputs(se_sim* s, const double* values, int n);
SE_API int         se_set_input_text(se_sim* s, int i, const char* value);
SE_API void        se_tick(se_sim* s);
SE_API double      se_get_output(se_sim* s, int i);    /* NaN for a bad index */
SE_API int         se_get_outputs(se_sim* s, double* values, int n);
SE_API const char* se_get_output_text(se_sim* s, int i);   /* NULL if not text */
SE_API int         se_done(se_sim* s);
SE_API double      se_time(se_sim* s);
SE_API uint64_t    se_tick_index(se_sim* s);
SE_API uint64_t    se_tick_count(se_sim* s);

SE_API int         se_channel_handle(se_sim* s, const char* path);   /* -1 if refused */
SE_API double      se_read(se_sim* s, int handle);
SE_API int         se_read_many(se_sim* s, const int* handles, int n, double* out);
SE_API const char* se_read_text(se_sim* s, int handle);    /* NULL if not text */

SE_API void        se_sync_realtime(se_sim* s);
SE_API double      se_realtime_utilization(se_sim* s);
SE_API double      se_realtime_overrun_pct(se_sim* s);
SE_API double      se_realtime_lag(se_sim* s);
SE_API uint64_t    se_realtime_frames(se_sim* s);
SE_API uint64_t    se_realtime_overruns(se_sim* s);

SE_API void        se_log_capture(se_sim* s, int on);
SE_API const char* se_log_next(se_sim* s);        /* the next line's text, or NULL */
SE_API int         se_log_line_level(se_sim* s);  /* 0 trace ... 4 error */
SE_API const char* se_log_line_path(se_sim* s);
SE_API double      se_log_line_time(se_sim* s);

/* ---- 4. finish ------------------------------------------------------ */
SE_API int         se_finish(se_sim* s);          /* 0 for a clean run */
```

Each call is the §14.1 member of the same name and behaves as that member
does. The rules below are what C adds.

- **Plain values only.**
  - A call returns a number or a string, and takes only numbers, strings and
    arrays of `double` or `int`. There are no structs, callbacks or enums to
    declare, so every language's foreign-function layer can use it as it
    stands.
  - A truth value is an `int`, 1 or 0.
  - A count, an index or a handle is an `int`.
  - A tick count is a `uint64_t`.
- **C has no overloads.** So `set(path, text)` is `se_set_text`, and
  `log_level(path, level)` is `se_log_path_level`. A getter that shares a
  setter's name in C++ is `se_get_…`.
- **A list is a count, then one call per field.** `se_setting_count` and
  `se_channel_count` take the list, and the per-row calls read it, until the
  next count. A setting's value is `se_get` of its path, so it is always
  current.
- **Every string belongs to the instance.** It stays valid until the next call
  on that instance that returns a string of the same kind; a list's strings
  stay valid until the next count. The model's own strings
  (`se_model_name`, the port names) stay valid for as long as the library is
  loaded. Nothing is ever freed by the caller.
- **The root's ports are numbers, by index.**
  - Inputs and outputs are listed in declaration order. A record port is
    flattened to its fields, depth first, and each field is named by its path
    (`ws.speed`).
  - Every value crosses as a `double`: an `int` as its value, a `bool` as 0 or
    1, and a finite type as its enumerator's value.
  - Writing an `int`, `bool` or finite input needs a whole number. A finite
    input needs one of its enumerators, and a `bool` input 0 or 1.
  - `se_set_inputs` and `se_get_outputs` move every port in one call, in index
    order. `n` must equal the count.
  - The root's settings have no struct here. They are paths like any other,
    reached through `se_get` and `se_set`.
- **A string port is text, in the same numbering.**
  - A text port's unit is `string`, which is how a host tells it from a
    number.
  - `se_set_input_text` writes one, and `se_get_output_text` reads one. Text
    longer than the input's capacity is refused, never cut.
  - `se_set_input` and `se_get_output` refuse a text port, and the error names
    the call to use. `se_set_inputs` ignores a text port's slot, and
    `se_get_outputs` fills it with NaN, so a model with one text port can
    still move all its numbers in one call.
  - `se_get_text` and `se_read_text` are `get_text` and `read_text`. A string
    setting is written with `se_set_text`, which takes the characters as they
    are.
- **`se_error` reports the last refusal.**
  - That is either the C layer's own (a bad index, a value the input cannot
    hold, a count that doesn't match), or the model's `error()`.
  - It is cleared by the next call that can be refused, except that a refused
    source stands, as §14.1 says.
- **Nothing is thrown.** No C++ exception crosses the boundary. The one a
  model can raise, running out of memory, is caught and reported through
  `se_error`.
- **Threads.** An instance is used by one thread at a time. Instances share
  nothing (§15.4), so separate threads may each run their own.
- **Exports.**
  - `SE_API` marks every function, and only these functions are exported.
    It is `__declspec(dllexport)` when building the DLL (`SE_BUILD_DLL`),
    `__declspec(dllimport)` for a host that links the import library
    (`SE_USE_DLL`), and nothing otherwise. On POSIX it is default visibility,
    with everything else hidden.
  - The calling convention is the platform's default, which is what ctypes'
    `CDLL`, P/Invoke and LabVIEW's "C" setting all mean.
- **Two models in one program.** Each model's library exports the same names,
  so two models fit in one process when each is loaded as its own DLL. Linking
  them statically needs distinct names:
  - defining `SE_C_PREFIX` (for example `dt_`) both where a model's C layer is
    compiled and where the host includes `se_sim.h` renames every function
    (`dt_se_create`), as FMI's prefix macro does;
  - a host that uses two models this way includes `se_sim.h` once in each of
    two source files, one per prefix.

---

## 15. Lowering to C++

Normative to the extent stated. A worked, hand-verified example lives in
[`examples/drivetrain/`](examples/drivetrain/) — four `.se` sources, a `.sim`
file, the generated C++, the unit manifest, and the runtime header. It compiles
with `cl /std:c++20 /EHsc`, with no third-party packages and no include path
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
`whl.ws --> tc.ws, self.ws` becomes two readers of `sig.whl.ws`: fan-out is
free and nothing is copied. Fan-in being forbidden (§6.9.2) is exactly what makes
single-writer storage sound.

That storage lives in the **signal block** (§15.5), not as a loose member, so the
whole signal set has one address, one size, and one offset table.

**ZOH falls out for free.** A 200 Hz node's `Out` is not written on the other nine
base ticks, so the held value is simply the variable still sitting there. No hold
buffer exists.

**Every input is materialised in the callee's `In_` view, by value.** A record
port holds a `WheelState`, a scalar port a `double`. The view is not wire
storage — the wire is still the producer's `Out` member, and fan-out still costs
nothing — it is the parameter block one call reads through.

Which is why a **setting source** (§6.9.2) needs no lowering of its own. There
are three things an input can name — a producer's `Out`, a root input in
`sig.in`, and a setting's member — and materialising by value makes the third
indistinguishable from the other two at the call site:
`sfSum.output({sig.linScale.vout, se_cfg_root.grav}, sig.sfSum)`. No `Out` is
invented, no schedule edge appears, and because the member is read rather than
folded, a configuration-time override reaches the wire.

By value for both is what lets **both convert**. A conversion folds to a constant
and lands in the view's initialiser: a scalar as `sig.src.y * 0.001`, a record as
a brace initialiser applying each field's own factor,
`Vec3{(sig.src.v.x * 0.001), (sig.src.v.y * 0.001), (sig.src.v.z * 0.001)}`.
Nothing else in the generated code ever needs conversion machinery.

**A buffer is the one input that is not copied.** Its view is
`se_rt::buffer_view` (§5.1a) — a pointer into the producer's `Out` member, plus
the size and capacity — built from `sig.rx.data` by an implicit conversion, so
the call site reads `dec.output({sig.rx.data}, sig.dec)` like any other. There
is nothing to convert, a buffer having no unit, and copying up to a datagram per
reader per call would buy nothing. The reference is sound for the same reason
the signal block is: every reader runs after its producer in the output pass
(§8.5), and the storage outlives the call.

The cost is a struct copy per record input per call — three doubles for a `Vec3`
— where an earlier lowering passed `const WheelState&`. That reference is what
made `Vec3(mm)` into `Vec3(m)` an error rather than a conversion: aliasing the
producer's storage left nowhere to put the factor. Paying the copy buys the
uniform rule of §5.2, and it is stated here rather than left to be discovered.

### 15.4 Enforcement falls out of the lowering

Not from an analyser:

There are exactly three mechanisms, and **none of them is a name that shadows
another name**. That was tried and is not sound: a `const State&` alias declared
in a method's preamble hides the member for unqualified lookup, so `state.x = …`
fails, but `this->state.x = …` still compiles. A guarantee with a one-token
escape hatch is not a guarantee, so the storage itself carries the constness.

- **Purity is `const` on the method, and the writable thing is an
  out-parameter.** `output()`, `derivative()`, `next()` and `final()` are all
  const members, so no body can write `state`, `var` or `param` through `this`.
  What a method may write, it receives: `Out& out`, `Der& der`, `Next& next`,
  `Var& var`. This is one rule, applied uniformly, and it is why `next()` can
  write a `var` (§6.5) while reaching a state only through `next.` (§6.4).
- **Settings are `const` by construction.** `param` is a `const Param` member,
  initialised when the leaf is constructed, because settings are fixed at
  elaboration (§6.2) and there is no moment at which writing one would be
  meaningful. No spelling of it is assignable, in any method.
- **`init()` is the one non-const method**, since §6.4 makes it the escape hatch
  for a computed initial condition and §6.5 lets it compute a `var`. It needs no
  protection for `param`, which is const anyway.
- **Feedthrough is a struct field set.** One `In_<method>` view struct per
  method, holding exactly the inputs that method's DSL signature listed. Reading
  an undeclared input is "no such member". An **empty** `In_output` is the sort
  root and the loop-breaker.
- **An output group's write set is a struct of references.** A group (§8.3a)
  lowers to its own const member, `output_<first>(const In_output_<first>&,
  Out_<first>)`, named for the first output it lists. `Out_<first>` holds a
  reference to each output the group names and nothing else, bound at the call
  site to the node's slice of `sig` — so it is passed by value, and the copy
  writes through — and writing another group's output is "no such member"
  too. `output default()` lowers the same way, to `output_default` with an
  `Out_default` view of the outputs left to it — a name no group can take,
  since no output can be called `default`. The plain `output()` of a node
  without groups keeps `output(const In_output&, Out&)`.
- **I/O confinement is name withholding.** Native aliases and the full `log`
  object are emitted into the preamble of the methods where they are legal, and
  simply are not declared elsewhere. Natives are `mutable`, which gives up
  nothing: §6.7 already establishes that `const` never protected a handle,
  because a `const SOCKET` is still perfectly usable by `recv()`.
- **A narrow member, shadowed by a wider local, is the same mechanism a second
  time.** `log` is a trace-only member that the mutating methods shadow with the
  full object, and `sim` is a reflection-only member (§10.5) that `init()` and
  `next()` shadow with one that also carries `stop`/`abort` (§10.2). Both
  make an illegal call *no such member* rather than a rule to enforce, and both
  keep the pure methods free of preamble.

```cpp
struct Wheel {
    struct Param     { double inertia;    /* kg*m^2 */  double radius; /* m */ };
    struct Var       { double inertia_inv{}; };          // 1/(kg*m^2)
    struct State     { state_ref omega; };               // a view into Sim::x
    struct Der       { state_ref omega; };               // a view into Sim::xd
    struct Out       { WheelState ws{}; };
    struct In_derivative { double drive_torque{}; };
    struct In_output { double drive_torque{}; double ground_speed{}; };

    const Param           param;          // set at construction; never writable
    const char* const     se_path_;
    const char* const     se_type_;
    se_rt::Run* const     se_run_;        // the enclosing Sim's run (§10.5)
    State                 state;
    Var                   var{};
    const se_rt::RunView  sim{se_run_, se_path_, se_type_};   // reflection only
    const se_rt::TraceLog log{se_run_, se_path_};  // log.trace only (§10.1)

    void init();                                              // mutating
    void derivative(const In_derivative& in, Der& der) const; // pure
    void output(const In_output& in, Out& out) const;         // pure
};
```

A consequence worth stating, because it is the property a reader of generated
code most wants: **the five pure methods need no preamble at all.** The
trace-only `log` and the reflection-only `sim` are members, and everything else
they may touch is already const through `this`, so between their braces there is
nothing but the verbatim body.

`se_run_` points at a member of the enclosing `Sim`, not at a global. Everything
a run accumulates lives there, in one `se_rt::Run` per model: the clock that
`sim.time()` reads, the stop flag (§10.2), the contract registry (§10.6), the
real-time pacing (§9.4) and the log sink with its levels and timestamp (§13.7).
The engine keeps no process-wide state at all. So two models in one process
share none of it, a finished run leaves nothing behind for the next `Sim`, and
the clock can be moved to a solver stage's instant without disturbing anything
else (§10.5). The pointer is not to const because a contract check in a pure
method records into the registry. What a body may *call* is still decided by
the handle it is given, as above.

### 15.5 The three blocks

A leaf owns almost nothing. Its state and its outputs live in model-wide blocks
that `Sim` owns, and the leaf holds views into them:

```cpp
static constexpr std::size_t n_states = 1;
std::array<double, n_states> x{};    // continuous state — the solver's ABI
std::array<double, n_states> xd{};

struct Signals {                     // every wire value + the root boundary
    struct In { double axle_torque{}; double ground_speed{}; } in;
    Wheel::Out              whl;
    TractionController::Out tc;
};
Signals sig{};

struct Discrete { TractionController::Store tc; };
Discrete dis{};                      // every discrete state, as committed
Discrete nxt{};                      // the x⁺ = h(t,x,u) buffer

struct Finite { Clutch::Fin cl; };   // only in a model with a `finite` state
Finite fin{};                        // every finite state, as committed
Finite fnx{};                        // the q⁺ buffer
```

A model containing an **array-shaped state** (§6.4a) differs in two ways, and
only two:

```cpp
std::size_t n_states = 0;            // settled at configuration, not compile time
double*     x  = nullptr;            // host-provided (§15.5a)
double*     xd = nullptr;

std::size_t n_dstates = 0;           // discrete slots that are array-shaped
double*     dis_arr = nullptr;       // the fourth block, and its commit twin
double*     nxt_arr = nullptr;
```

Everything else — `sig`, `dis`/`nxt`, the schedule, every wire — is byte-for-byte
what it would have been. **A model with no array state emits the first form**,
`constexpr` size and all, and binds in its constructor exactly as it always did.

**Why blocks rather than loose members.** `x` was always a block, for a stated
reason — an external solver integrates it directly. The same argument reaches
further than the solver: an inspector, a shared-memory host, a replay recorder
or a co-simulation peer wants an address, a size, and a table. Given those, none
of them needs a line of generated accessor code. **`x` + `sig` + `dis` (+ `fin`,
when the model has finite states) is a complete checkpoint of the model.**

- **`x` is one flat `double` array** holding only `continuous` states, so an
  external solver integrates it directly. `x[i]` holds the value *in that
  state's declared unit* — a contract, hence the slot map. An array-shaped
  continuous state occupies a contiguous run of slots in the same block; there
  is no second continuous vector, because the solver's ABI is the whole point of
  the first one.
- **`sig` and `dis` are generated structs, not arrays**, because a port may be
  `int`, `bool` or a record, and the block has to hold each faithfully. Their
  offsets therefore come from `offsetof` in the generated header, not from the
  DSL compiler: padding is the C++ compiler's business and a guessed table would
  be worse than none (§15.6).
- **`dis_arr`/`nxt_arr` is a fourth block, and exists for exactly that reason.**
  An array-shaped *discrete* state cannot live in `dis`, because a runtime-sized
  member would destroy the `offsetof` property the previous bullet depends on.
  It gets a flat `double` block of its own instead — array states being
  `double`-only (§6.4a) is what makes that possible — so `dis` stays a plain
  struct and the new block gets the same address-size-table treatment as `x`.
  The commit is still one whole-block copy per side, so §6.4's claim that
  simultaneity is a property of the *model* rather than of each node survives
  unchanged; there are now two block copies at the commit instead of
  one.
- **`fin`/`fnx` is the finite block and its buffer**, a generated struct for
  `dis`'s reasons: a finite state may be `bool`, `int` or a finite type.
- **`xnx` is the continuous buffer, and only a model that needs one has it.** It
  exists when some node defines `next()` beside a continuous state, and it is a
  flat `double` block like `x`.
- **`Next` is a view, not a block.** Each node that defines `next()` has one
  `Next`, holding a proxy per state, each bound into the buffer of that state's
  kind. Like `Der` it is a `Sim` member rather than a leaf member, because the
  out-parameter has to be non-const at the call site while the method is const.
- **Seed, write, commit.** Before the first `next()` the buffers are seeded from
  the committed blocks (`nxt = dis`, `fnx = fin`, `xnx = x`), so a state no body
  writes keeps its value. Every `next()` writes the buffers. The commit copies them
  back below the exit (§9.6 step 6). Values move, not addresses, so every proxy
  stays bound.
- **A named state is a `value_ref<T>` proxy holding a `T*`.** The proxy exists
  because generated `State` and `Der` structs must be default-constructible
  before `bind()` runs, and a `T&` member can be neither default-constructed nor
  rebound. `operator T()` means every operator, every `<cmath>` function and
  every third-party API just works. It is a template only because a continuous
  state is always `double` while a discrete one may not be; that does not
  disturb the default-constructibility argument, which holds per instantiation.
  `using state_ref = value_ref<double>;` is the continuous case.
- **An array-shaped state is an `array_ref<T>` proxy** — a `T*` plus a length,
  with `operator[]`. It is the same argument one step further: `State`, `Der` and
  `Store` must still be default-constructible before `bind()` runs, and now the
  *length* is not known at compile time either, so a `std::array` member is not
  available even in principle. Rebindability, which §15.5 already required of
  `value_ref`, is what the whole configuration-time binding step (§6.2b step 5)
  rests on.
- **Constness propagates because `State` is held by value.** In a const method
  `state` is a `const State`, so `state.omega` is a `const value_ref` and
  `operator=` is not callable. A `T&` or `T*` member would leak, which is the
  whole reason the proxy exists rather than a raw reference. `array_ref` must
  carry the property through its subscript: a `const array_ref`'s `operator[]`
  yields `const T&`, so `state.w1[i] = …` fails to compile in `output()` for the
  same reason `state.omega = …` does, and by the same mechanism rather than a
  parallel one.
- **There is no `gather_x`/`scatter_x`.** The vector *is* the storage, so there
  is no second copy to desynchronise. A solver that owns its own vector (CVODE,
  odeint) does one `memcpy` each way per evaluation: the proxies buy a clean
  *layout*, not zero copying.
- **A stage's trial state is in `x`, and that is an obligation.** Every solver
  evaluates a stage by writing its trial state into `x` — the built-in ones
  assign `x[i] = x0[i] + …` before each stage, an owning solver copies in — so
  that `state` is never a snapshot. `derivative()` and `output()` could be
  handed the state as an argument, but a helper they call cannot: it is a
  member that reads `state` through `this`, its body is verbatim, and the
  generator could never redirect it (§11.4). A lowering that passed `x` as a
  parameter and left the member at the step's state would leave every helper
  reading the wrong instant, silently.
- `der` is **`Sim`-owned scratch, not node data** — evaluated once per solver
  stage at trial states the trajectory never visits, then discarded. `states`
  and `vars` remain the only DSL storage sections; `vars` stay inside the node,
  because §6.5 makes them private per-instance storage outside the override
  channel and therefore outside the model's addressable surface.

**Settings are a fourth block, and they are addressable.** Each leaf owns its
`Param`; each composite owns one too, holding only the settings its flow-down
expressions read (§6.9.1). `resolve_settings` is a generated method containing a
flat sequence of assignments in dependency order — one per setting, guarded by
its pinned flag:

```cpp
void resolve_settings() {
    if (!pin_.fl_wheel_inertia) fl.param.wheel_inertia = 0.9;
    if (!pin_.fl_whl_inertia)   fl_whl.param.inertia   = fl.param.wheel_inertia;
    if (!pin_.fl_whl_radius)    fl_whl.param.radius    = 0.31;
}
```

The order is the topological sort §6.2 already requires; emitting it as straight
line code means the runtime performs no sorting and holds no dependency graph.
A binding over literals emits as a constant, exactly as before §6.2b existed; one
that reads `param.*` emits as the expression, which is what makes a parent's
override reach its children. **§7 is emitted, not interpreted** — it is total,
side-effect-free arithmetic over `double` plus `<cmath>`, so each expression
becomes one C++ expression and I1 is untouched: the generator writes C++, it
still never parses it.

#### 15.5a The state buffers are the host's

When a model has array-shaped states its block sizes are not known until §6.2b
step 4, so `Sim::Impl` cannot own `std::array` members and will not own
`std::vector` ones. **The host allocates and hands the buffers in.** The engine
performs no allocation for any published block, before a run or during one.

The handshake below is `Sim::Impl`'s. The public `Sim` of §12.4 is itself such a
host: its `init()` sizes the buffers once configuration has settled and binds
them, so a program written against the public header never sees the handshake.

```cpp
void        configure(const SettingsSource&);       // §6.2b steps 1–4
std::size_t state_size() const;                     // continuous slots
std::size_t discrete_array_size() const;
void        bind_state(double* x, double* xd, std::size_t n);
void        bind_discrete_array(double* dis, double* nxt, std::size_t n);
void        init();                                 // §6.2b step 6
```

```cpp
Sim sim;
sim.configure(src);
std::vector<double> x(sim.state_size()), xd(sim.state_size());
sim.bind_state(x.data(), xd.data(), x.size());
// … same for the discrete array block …
sim.init();
```

- **This is the shape the `x` ABI already had.** §15.5's opening argument is that
  an external solver, an inspector, a shared-memory host or a co-simulation peer
  wants an address, a size and a table. Handing the address *in* rather than
  reading it out is the same contract from the other end, and it is what CVODE,
  odeint and FMI ask for anyway. A host that wants `x` in shared memory, in a
  ring buffer, or inside its own arena no longer has to copy to get it.
- **A size mismatch at `bind_state` is a configuration failure** (§6.2b), not
  undefined behaviour. It is the one error the handshake makes possible and the
  cheapest possible check.
- **Binding is not re-entrant and not repeatable.** Rebinding after `init()` is
  not defined; §6.2b's "configuration happens once, before the first tick" covers
  the buffers as much as the settings.
- **A model with no array state keeps the old shape** — `constexpr n_states`,
  owned `std::array`, binding in the constructor — and `bind_state` is not
  emitted for it. Two lowerings, chosen by whether any state carries an extent.
  The cost of the dynamic form is one indirection per state access, paid only by
  models that asked for it.

### 15.6 The unit manifest

Because units are erased (I3), the manifest is a **load-bearing generator
output**, not documentation. It is the contract for anyone writing an external
solver, feeding the root boundary, or overriding a setting from outside:

```
[state.continuous]        # slot | model path | unit  -- the solver's ABI
x[0]      whl.omega             rad/s
xd[0]     whl.omega'            rad/s^2

[state.discrete]          # Sim::Impl::dis; offsets from Sim::Impl::discrete_map()
          tc.cut                N*m         double

[state.finite]            # Sim::Impl::fin; offsets from Sim::Impl::finite_map()
          cl.locked             -           bool
          dev.link              -           device.LinkState

[state.discrete.array]    # Sim::Impl::dis_arr; rows from Sim::Impl::discrete_array_map()
dis_arr[?]  bw.w1 [param.nbq]   -           dynamic
dis_arr[?]  bw.w2 [param.nbq]   -           dynamic

[signals]                 # Sim::Impl::sig; offsets from Sim::Impl::signal_map()
          in.axle_torque        N*m         double
          whl.ws.speed          rad/s       double

[boundary.in]             # root inputs the host must drive, through Sim::inputs()
          in.axle_torque        N*m

[boundary.out]            # root outputs the host may read
          whl.ws.speed          rad/s

[settings]                # configurable; value is the elaborated default
          whl.inertia           kg*m^2      = 0.9
          bw.order              -           = 4
          cue.limit.x           m/s^2       = 5.0
          cue.limit.z           m/s^2       = 4.0

[types]
drivetrain.WheelState.speed     rad/s
se.math.Vec3                   units { U }
se.math.Vec3.x                 U
device.LinkState               finite { IDLE = 0, CONNECTING = 1, CONNECTED = 2, FAULT = 15 }
```

Field units in `[types]` are shown **as declared**, so a parametric record (§5.2)
reports its parameter rather than any one binding — the type is shared, and no
single binding would be true of it. The substituted units are the per-field rows
in `[signals]` and `[boundary.*]`, which is where a host reads what to send.

**A finite type's `[types]` row is its decoding table**: every enumerator with
its value, in declaration order. Wherever the type appears in another section
its type column carries the type's qualified name and its unit column `-`, and a
recorded column of it (§13.5) is decoded through this row. Its `[settings]`
value is the enumerator's name, which is also what `dump_settings` writes.

**A buffer's `[signals]` row is one row**, unit `-` and type `buffer(N)`; its
`signal_map()` entry is one slot whose size is the whole `se_rt::buffer<N>`,
size field first (§5.1a), so a host reading the signal block finds the length
at the slot's offset without knowing N.

```
[signals]
          rx.data               -           buffer(1500)
```

Paths are **root-relative** (§13.2): the `root:` entry names a definition, not an
instance, so there is no prefix for the root itself.

**Byte offsets are not in the manifest, deliberately.** Padding is decided by the
C++ compiler, so a table written by `sec` would be a guess, and a wrong offset
table is worse than none. The manifest names, units and types each slot; the
implementation exposes the offsets on `Sim::Impl`, built with `offsetof` where they are
actually known:

```cpp
static const se_rt::Slot* signal_map(std::size_t& count);
static const se_rt::Slot* discrete_map(std::size_t& count);
static const se_rt::Slot* finite_map(std::size_t& count);   // a model with finite states
// Non-static, and emitted only for a model with array-shaped states: one
// block each, so one accessor each. Both are built by configure().
const se_rt::Slot* state_map(std::size_t& count) const;            // Sim::Impl::x
const se_rt::Slot* discrete_array_map(std::size_t& count) const;   // Sim::Impl::dis_arr
// struct Slot { const char* path; std::size_t offset, size;
//               const char* type; const char* unit; };
```

Both the manifest section and the table are generated from one list, so they
cannot drift apart.

**A dynamic model's `[state.continuous]` rows carry no slot number.** Once any
state in the model has an extent, every state's POSITION is a configuration
result — a scalar one included, since it sits after whatever run precedes it
— so those rows print `x[?]` and `xd[?]` and are marked `dynamic` too. The
extent is printed as written beside the path. A model with no array state prints
the numbered form exactly as before.

**An array state's slot map is a run-time artifact, and `state_map` is therefore
not static.** `sec` cannot print `x[0] bw.w1[0]` for a state whose extent is not
known until §6.2b step 4, so the manifest prints the *declaration* — the extent
expression as written, marked `dynamic` — and the concrete rows are built by the
model once configuration has settled. A host reads the manifest to learn what
paths exist and what units they carry, and calls `state_map` to learn where they
landed. This is the same division the previous paragraph already makes for
offsets, extended from "the C++ compiler knows this and `sec` does not" to "the
configured model knows this and `sec` does not."

A model with no array state has a fully static map, and may emit `state_map` as
a static table exactly like the other two.

**A record setting appears as one row per leaf** (§6.2c), addressed by the
dotted path — which is what `[signals]` above already does for a record port,
so this block gains rows and not a new shape. There is no row naming the record
itself, because there is no value a §14 source could bind to one (§13.4).

**`[settings]` is the configuration schema** (§14.1), not a record of what was
baked. It names every path a §14 source may bind, the unit that source is checked
against, and the value elaboration produced — which is the value the binary
starts with, and which `dump_settings` reproduces. `bw.order` above carries no
marker because an extent-bearing setting is an ordinary one (§6.2): it is bound
by a §14 source like any other, and step 4 sizes the blocks around whatever it
resolves to. The `structural` marker — a setting that appears in the inventory
for completeness and is *refused* on load — remains defined for a setting that
reaches instance count or wiring, and no construct in this version produces one.

The dimension behind each unit string ships in the generated header alongside the
slot maps, for the same reason the offsets do: the loader has to compare
dimensions, and a string comparison — `"m/s^2"` against `"m/s2"` — is not that
comparison.

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
17 |     omega: double(rad/s) = 0.0;
   |     ^^^^^ expected `continuous` or `discrete` before the state name
   = note: there is deliberately no default state kind — the two differ in
           write accessor (`der.` vs `next.`), in schedule, and in whether
           they enter the solver's state vector
   = help: write `continuous omega: double(rad/s) = 0.0;`
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
`SE04xx` elaboration, `SE05xx` scheduling, `SE06xx` emission, `SE07xx`
documentation (§6.14).

**Every code names a compile stage, and configuration failures have none.** The
five things §6.2b can reject happen in a shipped binary with no source in reach,
and three of them — a non-integer extent, an out-of-range element path, a
mis-sized host buffer — are not detectable at compile time in principle. They
are reported against a *path* by the mechanism §14 defines for a rejected
settings source. Extending the `SE` range to cover them was considered and
rejected: a code whose diagnostic cannot carry a source span is a different kind
of thing wearing the same name.

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
| `SE0204` | 2 | Expected a definition (`node`, `type` or `finite`) |
| `SE0205` | 2 | Wrong number of unit arguments on a scalar type |
| `SE0206` | 2 | Malformed unit expression |
| `SE0207` | 2 | Unknown scalar type |
| `SE0208` | 2 | Expected `;` |
| `SE0209` | 2 | Unbalanced braces at end of file |
| `SE0210` | 2 | Wildcard import is not supported |
| `SE0211` | 2 | Expected a wire, an instance, a `group`, or `}` in `structure` |
| `SE0212` | 2 | Malformed wire endpoint |
| `SE0213` | 2 | Expected `-->` |
| `SE0214` | 2 | An interior endpoint of a wire chain is not a bare instance name |
| `SE0215` | 2 | A literal wire endpoint is a destination |
| `SE0216` | 2 | Two sibling groups share a label |
| `SE0217` | 2 | A group's label is empty |
| `SE0218` | 2 | A group declares no instances (warning) |
| `SE0220` | 2 | Duplicate section in a node |
| `SE0221` | 2 | Unknown section name |
| `SE0230` | 2 | State kind (`continuous`/`discrete`/`finite`) omitted |
| `SE0231` | — | *Retired.* A setting may not be a record type — lifted by §6.2c. The number stays burned; codes are stable. |
| `SE0232` | 3 | A state may not be a record type |
| `SE0233` | 2 | A `native` member may not carry a unit |
| `SE0234` | 2 | An extent is not permitted on this declaration (port, setting, record field, `native`) |
| `SE0235` | 2 | Malformed array extent |
| `SE0236` | 2 | A buffer or string capacity that is not a positive integer literal |
| `SE0240` | 2 | Helper function has no body |
| `SE0241` | 2 | `init` takes no parameters |
| `SE0242` | 2 | `final` takes at most one parameter |
| `SE0243` | 2 | A brace list mixes positional and named fields |
| `SE0250` | 2 | Unknown platform atom in `when` |
| `SE0251` | 2 | Unknown build primitive |
| `SE0260` | 2 | Duplicate entry in `sim` |
| `SE0261` | 2 | Unknown entry in `sim` |
| `SE0262` | 2 | Missing required `sim` entry |
| `SE0263` | — | *Retired.* `sync` or `window` requires `mode: realtime` — the entries were removed with §9.4's host pacing. The number stays burned; codes are stable. |
| `SE0264` | 4 | Bad value for a `sim` entry (solver, log level) |
| `SE0265` | 2 | A removed `sim` entry (`mode`, `sync`, `window`): pacing is the host's (§9.4) |
| `SE0266` | 2 | An old-style `record:` or `log:` body (`file:`, `signals { }`, `levels { p: l; }`): the body is now a settings section's (§13.1) |
| `SE0267` | 4 | `sim.duration` that is not a whole number of base steps, or an unknown `sim.` binding (§13.3) |
| `SE0268` | 2 | A file name with a placeholder other than `{time}`, or a brace standing alone (§13.5) |
| `SE0269` | 2 | `{time}` in an inputs file's name: the file is read, not written (§13.8) |
| `SE0270` | 2 | Duplicate entry in `tests` |
| `SE0271` | 2 | Unknown entry in `tests` |
| `SE0272` | 2 | `~=` with no `within`, or `=` with one |
| `SE0273` | 2 | Row arity does not match the set header |
| `SE0274` | 2 | Row has no label |
| `SE0280` | 2 | Unknown annotation name |
| `SE0281` | 2 | Unknown `@diagram` key |
| `SE0282` | 2 | An annotation key is not allowed on this declaration |
| `SE0283` | 2 | A value on a flag key, or a `@diagram` with no keys |
| `SE0284` | 2 | Duplicate annotation key |
| `SE0285` | 2 | `collapsed` together with `expanded` |
| `SE0286` | 2 | Two `@diagram` annotations on one declaration |
| `SE0287` | 2 | An annotation that annotates nothing |
| `SE0301` | 3 | `package` declaration disagrees with the file's location |
| `SE0302` | 3 | No public declaration matching the file name |
| `SE0303` | 3 | Imported name collides |
| `SE0304` | 3 | Unresolved name |
| `SE0305` | 3 | Package path resolves in more than one root |
| `SE0310` | 3 | Cycle in record field types |
| `SE0311` | 3 | Field-level wire endpoints are not supported |
| `SE0312` | 4 | Recursive instantiation: a node transitively contains itself |
| `SE0313` | 3 | Wrong number of unit arguments on a type reference |
| `SE0314` | 3 | A bare wire endpoint needs exactly one port on that side |
| `SE0315` | 3 | A `param` wire endpoint is a destination, or names no setting |
| `SE0317` | 3 | Two enumerators of one finite type share a name |
| `SE0318` | 3 | Two enumerators of one finite type share a value |
| `SE0319` | 3 | A finite type declares no enumerators |
| `SE0320` | 3 | Node has both code and a `structure` block |
| `SE0321` | 3 | `collapsed` or `expanded` on a leaf, or on an instance of one |
| `SE0322` | 3 | A file-level `declarations` block outside `package.se` |
| `SE0323` | 3 | A buffer or string with no capacity where it is storage (a setting, a leaf's output, a var) |
| `SE0324` | 3 | A buffer or string capacity on a reader (an input that is not the root's, a composite's port) |
| `SE0325` | 3 | A buffer setting, or a buffer or string state or record field |
| `SE0326` | 4 | A string input on the root with no capacity |
| `SE0330` | 3 | Duplicate member name in a node |
| `SE0331` | 3 | Member name shadows a generated accessor |
| `SE0332` | 3 | A name the generated C++ spells verbatim is a C++ keyword |
| `SE0340` | 3 | An output that no output method computes |
| `SE0341` | 3 | Continuous state declared but no `derivative()` |
| `SE0342` | 3 | Discrete or finite state declared but no `next()` |
| `SE0343` | 3 | `derivative()` in a node with no continuous states |
| `SE0344` | 3 | `output()` in a node with no outputs |
| `SE0350` | 3 | Method parameter is not a declared input |
| `SE0351` | 3 | Duplicate method parameter |
| `SE0352` | 3 | A lifecycle method defined twice (two `output()`s or two `output default()`s included) |
| `SE0353` | 3 | An output group names something that is not an output |
| `SE0354` | 3 | An output named by two output groups, or twice by one |
| `SE0355` | 3 | An `output default()` beside groups that already name every output |
| `SE0356` | 3 | A bare `output()` beside output groups: the catch-all is `output default()` |
| `SE0357` | 3 | `output default()` in a node with no output groups |
| `SE0360` | 3 | A test column names no member of the node |
| `SE0361` | 3 | A test column is on the wrong side of the operator |
| `SE0362` | 3 | An expected column names a method the node does not define |
| `SE0363` | 3 | A node with a buffer port has a `tests` section |
| `SE0402` | 4 | Lossy literal conversion to an integer target |
| `SE0403` | 4 | Not an enumerator of the finite type the site expects |
| `SE0404` | 4 | An enumerator where a number is expected, or a number where an enumerator is |
| `SE0410` | 4 | Incompatible units |
| `SE0411` | 4 | Offset unit in a compound unit expression |
| `SE0412` | 4 | Builtin function argument has the wrong dimension |
| `SE0413` | 4 | Non-finite or domain-error result at elaboration |
| `SE0414` | 4 | Unknown unit symbol |
| `SE0415` | 4 | Non-radian angle unit in a declared unit |
| `SE0420` | 4 | Required setting not bound |
| `SE0421` | 4 | Cycle among derived settings |
| `SE0416` | 3 | Unit parameter shadows an Appendix A unit symbol |
| `SE0417` | 3 | Duplicate unit parameter |
| `SE0418` | — | *Retired.* A composite may not declare unit parameters — lifted by §6.2a. The number stays burned; codes are stable. |
| `SE0419` | 4 | Wrong number of unit arguments at an instantiation |
| `SE0423` | 4 | Wrong number of values in a positional brace list |
| `SE0424` | 4 | Unknown field name in a brace list |
| `SE0425` | 4 | A field is given twice in a brace list |
| `SE0426` | 4 | A brace list where a scalar is declared, or a value where a record is |
| `SE0427` | 4 | Text longer than the string it is copied into |
| `SE0428` | 4 | Text where a number or an enumerator is expected, or anything else where text is |
| `SE0430` | 4 | Unconnected input |
| `SE0431` | 4 | A `continuous` state must be `double` |
| `SE0432` | 4 | An array extent must be a dimensionless, non-negative integer |
| `SE0433` | 4 | An array state or var must be `double` |
| `SE0434` | 3 | A `finite` state must be `bool`, `int` or a finite type |
| `SE0440` | 4 | Cross-instance reference in a setting binding |
| `SE0441` | 4 | Unknown or duplicated setting binding |
| `SE0442` | 4 | Fan-in: two sources into one input |
| `SE0443` | 4 | Wire direction is invalid |
| `SE0445` | 4 | A buffer port on the root node |
| `SE0451` | — | *Retired.* `sync` is not an integer multiple of `step` — the frame is the host's now (§9.4). The number stays burned; codes are stable. |
| `SE0460` | 4 | Override target is ambiguous, or is an output |
| `SE0461` | 4 | Recorded signal path is ambiguous |
| `SE0470` | 4 | A set reaches a rate-derived setting with no `step` |
| `SE0510` | 5 | Algebraic loop |
| `SE0511` | 5 | Node is unreachable from the root |
| `SE0701` | doc | A doc comment documents no declaration |
| `SE0702` | doc | A public declaration has no doc comment (warning) |
| `SE0703` | doc | A doc comment's line begins more than one declaration |
| `SE0704` | doc | A definition's doc comment has no summary ending in a period |
| `SE0705` | doc | Unknown doc section |
| `SE0706` | doc | Doc section out of order or repeated |
| `SE0707` | doc | A `§` citation names no section of the specification |
| `SE0708` | doc | An `SE` citation names no catalogue entry |

**Configuration-time failures are not `SE` codes.** A §14 source presented to a
compiled model (§14.1) is diagnosed by that model's loader, not by `sec`, and
reports through `load_settings`'s return rather than through this catalogue. The
three failures are: an unknown path, a right-hand side outside the restricted
grammar, and a dimension that disagrees with §15.6. Each names the offending path. `sec`'s own checks are
unchanged: everything it can see, it still rejects at the stage listed.

---

## 17. Grammar summary

EBNF for all three file kinds. `{ x }` is zero or more, `[ x ]` optional, `|`
alternation. Terminals are quoted. `VERBATIM_BLOCK`, `VERBATIM_SIGNATURE`, and
`VERBATIM_TO_SEMI` are the scanned regions of §11.

```ebnf
(* ─── model file ─────────────────────────────────────────────────────── *)

model_file      = package_decl { use_decl } { definition } EOF ;
(* §2.6 — a package manifest, `package.se`, holds no definition. *)
manifest_file   = package_decl { use_decl } { build_sec | declarations_sec } EOF ;

package_decl    = "package" qualified_name ";" ;
use_decl        = "use" qualified_name ";" ;
qualified_name  = IDENT { "." IDENT } ;

definition      = { annotation } ( type_def | finite_def | node_def ) ;

(* §6.15 — a value is a literal, never an expression. *)
annotation      = "@" IDENT [ "(" annotation_arg { "," annotation_arg } ")" ] ;
annotation_arg  = IDENT [ "=" ( STRING | [ "-" ] NUMBER | IDENT ) ] ;

(* ─── record types ───────────────────────────────────────────────────── *)

type_def        = "type" IDENT "{" [ units_sec ] { field_decl } "}" ;
field_decl      = IDENT ":" type_ref ";" ;

(* §5.2a *)
finite_def      = "finite" IDENT "{" enumerator { "," enumerator } [ "," ] "}" ;
enumerator      = IDENT [ "=" [ "-" ] INT ] ;

(* One production for every declaration, scalar or record. A one-segment path
   naming a closed scalar type (5.1) takes its unit as the argument: one for
   `double`/`float`/`int`, none for `bool`. The one-segment paths `buffer`
   and `string` take no unit; their optional argument is a capacity (5.1a,
   5.1b). *)
type_ref        = qualified_name [ "(" unit_expr { "," unit_expr } ")" ]
                | "buffer" [ "(" INT ")" ]
                | "string" [ "(" INT ")" ] ;

(* ─── nodes ──────────────────────────────────────────────────────────── *)

node_def        = "node" IDENT "{" { node_item } "}" ;

node_item       = settings_sec | units_sec | inputs_sec | outputs_sec | states_sec
                | vars_sec | native_sec | declarations_sec | build_sec
                | structure_sec | tests_sec
                | lifecycle_method
                | helper_function ;

settings_sec    = "settings" "{" { setting_decl } "}" ;
setting_decl    = IDENT ":" type_ref [ "=" expression ] ";" ;

units_sec       = "units" "{" { unit_param } "}" ;      (* §6.2a *)
unit_param      = IDENT ";" ;

(* §6.13. `tests`, `set` and `within` are all contextual, not reserved. *)
tests_sec       = "tests" "{" { tests_item } "}" ;
tests_item      = step_entry | units_bind_sec | settings_bind_sec | test_set ;
step_entry      = "step" ":" expression ";" ;
units_bind_sec  = "units"    "{" { IDENT "=" unit_expr ";" } "}" ;
settings_bind_sec = "settings" "{" { IDENT "=" expression ";" } "}" ;

test_set        = "set" STRING "," given_list ( "=" | "~=" ) accessor_list
                  [ "within" expression ] "{" { test_row } "}" ;
given_list      = given { "," given } ;
given           = "step" | accessor ;
accessor_list   = accessor { "," accessor } ;
accessor        = accessor_ns "." IDENT { "." IDENT | "[" INT "]" } ;
accessor_ns     = "param" | "in" | "state" | "out" | "der" | "next" | "var" ;
test_row        = expression { "," expression } "," STRING ";" ;

inputs_sec      = "inputs"  "{" { port_decl } "}" ;
outputs_sec     = "outputs" "{" { port_decl } "}" ;
port_decl       = IDENT ":" type_ref ";" ;

states_sec      = "states" "{" { state_decl } "}" ;
state_decl      = state_kind IDENT [ extent ] ":" type_ref
                  [ "=" expression ] ";" ;
state_kind      = "continuous" | "discrete" | "finite" ;
extent          = "[" expression "]" ;                  (* §6.4a *)

vars_sec        = "vars" "{" { var_decl } "}" ;
var_decl        = IDENT [ extent ] ":" type_ref ";" ;

native_sec      = "native" "{" { native_decl } "}" ;
native_decl     = IDENT ":" VERBATIM_TO_SEMI ";" ;

declarations_sec= "declarations" VERBATIM_BLOCK ;

build_sec       = "build" "{" { build_stmt } "}" ;
build_stmt      = build_primitive STRING [ "when" IDENT ] ";" ;
build_primitive = "link" | "include_dir" | "define" | "cflag" ;

(* ─── structure ──────────────────────────────────────────────────────── *)

structure_sec   = "structure" "{" { structure_item } "}" ;
structure_item  = { annotation } ( instance_decl | group_decl ) | wire_stmt ;

(* §6.9.3 — `group` is contextual: an IDENT spelled `group` followed by a
   STRING. Diagram-only; it changes no path. *)
group_decl      = "group" STRING "{" { structure_item } "}" ;

instance_decl   = "node" IDENT ":" qualified_name
                  [ unit_args ]                          (* §6.2a *)
                  [ "{" { setting_binding } "}" ] ";" ;
unit_args       = "(" unit_expr { "," unit_expr } ")" ;
setting_binding = IDENT "=" expression ";" ;

(* A chain desugars to one wire per hop; the comma list can only TERMINATE the
   statement, so a fan-out attaches to the last hop and `a --> b, c --> d` is a
   syntax error rather than an ambiguity (§6.9.2). *)
wire_stmt       = endpoint "-->" endpoint { "-->" endpoint }
                                          { "," endpoint } ";" ;
endpoint        = endpoint_head { "." IDENT }
                | literal_endpoint ;
(* `param` is an ordinary IDENT here and is separated at stage 3: it names the
   enclosing composite's setting rather than a child instance, and cannot be a
   destination (§6.9.2). No token is reserved for it, because §6.11 already
   forbids `param` as a member name. *)
endpoint_head   = "self" | IDENT ;
(* A literal is a SOURCE only, and unlike `param` that is settled at stage 2:
   nothing else in a `structure` block begins with a number or a sign, so the
   parser tells the two apart with no lookahead and rejects a literal
   destination outright (SE0215). It is a constant, not an expression — there is
   no arithmetic on a wire (§6.9.2). *)
literal_endpoint= [ "+" | "-" ] NUMBER [ unit ]
                | STRING ;                             (* §5.1b *)

(* ─── methods ────────────────────────────────────────────────────────── *)

lifecycle_method= "init"    "(" ")"                VERBATIM_BLOCK
                | "final"   "(" [ IDENT ] ")"      VERBATIM_BLOCK
                | "output"  [ ident_list | "default" ] "(" [ ident_list ] ")" VERBATIM_BLOCK
                    (* the first list, when present, names the outputs this
                       group writes; `default`, contextual, marks the method
                       that writes the rest; see §8.3a *)
                | "derivative" "(" [ ident_list ] ")" VERBATIM_BLOCK
                | "next"       "(" [ ident_list ] ")" VERBATIM_BLOCK ;
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
                | "param" "." IDENT { "." IDENT }      (* §6.2c *)
                | aggregate                            (* §6.2c *)
                | IDENT "(" [ expression { "," expression } ] ")"
                | IDENT
                | STRING                               (* §7.2b *)
                | "(" expression ")" ;

(* §6.2c — a record brace list. Wholly positional or wholly named, never
   mixed (SE0243). It is destructured against the declared field tree at the
   declaration site and never evaluated, so §7 stays scalar. *)
aggregate       = "{" [ agg_positional | agg_named ] "}" ;
agg_positional  = expression { "," expression } ;
agg_named       = IDENT "=" expression { "," IDENT "=" expression } ;

(* ─── sim file ───────────────────────────────────────────────────────── *)

sim_file        = "sim" IDENT "{" { sim_item } "}" EOF ;

sim_item        = root_entry | scalar_entry | record_entry | log_entry | inputs_entry ;

root_entry      = "root" ":" qualified_name
                  [ "{" { setting_binding } "}" ] ";" ;
scalar_entry    = IDENT ":" ( expression | IDENT | STRING ) ";" ;

record_entry    = "record" ":" record_body ;
log_entry       = "log" ":" log_body ;
inputs_entry    = "inputs" ":" inputs_body ;

(* §13.5, §13.7, §13.8 — one body, shared with the settings source's sections. *)
record_body     = "{" { record_item } "}" ;
record_item     = "clear" ";"
                | [ "-" ] signal_path ";"
                | "file"  "=" STRING ";"
                | "every" "=" NUMBER ";" ;
log_body        = "{" { log_item } "}" ;
log_item        = "file"  "=" STRING ";"
                | "level" "=" IDENT ";"
                | "levels" "{" { signal_path "=" IDENT ";" } "}" ;
inputs_body     = "{" { "file" "=" STRING ";" } "}" ;
signal_path     = IDENT { "." IDENT } [ "[" NUMBER "]" ] ;

(* ─── settings source ────────────────────────────────────────────────── *)

settings_file   = { override_binding | record_section | log_section | inputs_section } EOF ;
override_binding= signal_path "=" expression ";" ;
record_section  = "record" record_body ;
log_section     = "log" log_body ;
inputs_section  = "inputs" inputs_body ;
```

Two places where the grammar is deliberately looser than the semantics, so that a
future decision is not a grammar change: `endpoint` accepts a multi-segment path
(§5.3), and `scalar_entry` accepts any key, so an unknown sim-file key becomes a
good semantic diagnostic rather than a parse failure.

`endpoint` is also looser in the other direction, though not for that reason:
it accepts no segments at all because §6.9.2's bare endpoint resolves against
the named node's port arity, which the parser cannot know (`SE0314`).

---

## Appendix A — Unit symbols

An implementation must accept these. Symbol resolution is **exact match first,
then longest prefix plus exact unit**, so `min` is the minute (never
milli-something) and `Pa` is the pascal (never peta-`a`).

**Base:** `m` `kg` `s` `A` `K` `mol` `cd`

**Derived:** `rad` `sr` `Hz` `N` `Pa` `J` `W` `C` `V` `F` `ohm` `S` `Wb` `T` `H`
`lm` `lx` `Bq` `Gy` `Sv` `kat`

**Accepted non-SI:** `g` `deg` `rev` `min` `h` `d` `L` `t` `bar` `atm` `rpm` `%`
`degC` `degF` `in` `ft` `mi` `lb` `hp` `psi`

**Angle symbols** — `rad` and `sr` carry the angle dimension (§4.5); `deg`, `rev`,
and `rpm` carry it with a non-unity scale and are therefore accepted only as
literal suffixes and display units, never as a declared unit (`SE0415`).

**Prefixes:** `y` `z` `a` `f` `p` `n` `u` `m` `c` `d` `da` `h` `k` `M` `G` `T`
`P` `E` `Z` `Y`

The dimensionless unit is spelled `(-)`; `(1)` is equivalent. `(%)` is
dimensionless with scale 1/100. Note that `kg` is the base unit but the prefix
attaches to `g`, so `(mg)` is a milligram, as expected.

An unknown symbol is `SE0414`, and the diagnostic must suggest near matches —
`(sec)` should point at `(s)`.

## Appendix B — Reserved words

```
build       continuous    declarations  derivative  discrete    final
finite      init          inputs        native        next        node
output      outputs       package       self          settings    sim
states      structure     type          units         use         vars
```

`param`, `in`, `out`, `state`, `der`, `next`, `var`, `log`, and `sim` are
additionally unavailable as member names (§6.11), because the generated code
puts objects by those names in scope of bodies.

C++'s keywords are not reserved here but are refused as names where the generated
C++ spells a name verbatim (§6.11, `SE0332`).

Everything else — including every sim-file key, every build primitive, `when`,
every platform atom, and `tests`, `set` and `within` (§6.13) — is contextual and
remains usable as an identifier. `default` after `output` (§8.3a) is contextual
too, and needs no reservation: as a C++ keyword it can never name an output in
the first place. `tests` in particular *must* stay contextual:
`package tests;` is a real package path in this repository's own suite.

## Appendix C — Known gaps and open issues

Recorded so that their absence is visibly deliberate.

**Design questions genuinely open:**

1. **Display units for recording** (§13.5) — a gap *created* by resolving the angle
   question. §4.5 makes radians the only declarable angle unit, so a recorded CSV
   can no longer report degrees or rpm; recording emits the declared unit. A
   display-unit annotation on recorded signals is needed before any model wants
   degrees in its output.
2. **Read-only `native` access in `output()`** (§6.7). A preloaded lookup table or
   interpolation map is genuinely pure and genuinely wants to be read there, but
   the `init`/`next`/`final` confinement excludes it. A real need with no
   mechanism yet.
2a. ~~**Ambient `time` in the pure methods**~~ — **RESOLVED.** See §10.5. Time
   is readable from every body as `sim.time()`, and it is the first member of a
   named reflection surface rather than a one-off: the namespace is admitted by
   a stated rule — well defined at every invocation, a function of the current
   instant or of the node itself, never of run history — which lets `time`,
   `tick`, `step`, `path` and `type` in and still keeps `sim.overrun_count` out.

   `sim.stop`/`sim.abort` did not move. They stay confined to the mutating
   methods, and the confinement is now carried by the handle's TYPE — a leaf
   holds a reflection-only view, the mutating methods shadow it with a wider one
   — so nothing that was illegal became legal.

   The sharp half of the original item is closed too: a continuous node now sees
   the solver's minor-step time, because the minor-step output pass omits
   zero-order-held nodes rather than re-running everything. `se.sig.ct.Sine` is
   the first block that could not have been written before, and `se.sig.src.Ramp`
   and `se.sig.src.Step` have dropped the redundant `discrete t` clocks they
   carried only because time was withheld.

3. **Field-level wires** (§5.3) — `whl.ws.speed --> tc.u`. The grammar admits it;
   the semantics currently reject it. `se.math.Split3`/`Merge3` cover the need
   meanwhile, at the cost of two nodes per bundle.

   Note the deliberate asymmetry with `param.grav.z`, which §6.9.2 *does* admit.
   A record setting is a tree of scalar leaves and only the leaves are real
   (§6.2c), so naming one addresses a member that exists on its own. A record
   port is one value in one place, and taking a field of it is the harder
   question this item is about.
4. **Coordinate bases for record types.** A `Vec3(m/s)` in a body frame and one
   in an inertial frame are the same type today, and adding them is a modelling
   error nothing catches. A *checked* tag is the only shape available: unlike a
   unit, a frame transform cannot be inserted automatically, because it needs
   the current attitude — a run-time value — where §4.2 promises conversions
   fold to a compile-time constant. So a mismatch could only ever be an error the
   modeller fixes with an explicit transform node, never a silent conversion.
   Deliberately unresolved; the standard library documents one convention (SAE)
   instead. **If it is ever taken up, do not call it `frame`** — that word is the
   host's real-time pacing of §9.4. `basis` or `axes`.
5. **Platform atom vocabulary** (§12.2) — the exact set, and how extensible it is.
6. **The fixed-step solver menu** (§9.5) beyond `euler`/`rk2`/`rk4`.
7. **`name:` display metadata** — an optional human-readable label distinct from
   the type identifier. Never ruled on; currently absent.
8. **Log file format** (§13.7) — plain text versus structured JSONL.

8a. **Array `vars` are the one allocation in the lowering** (§6.5). Every
   published block is host-provided (§15.5a), but an array `var` is node-private
   with no external contract, so the node sizes its own at configuration. It
   never allocates inside a tick, so the real-time story holds; but "the engine
   allocates nothing" is now "the engine allocates nothing the host can see,"
   which is a weaker sentence. A fixed-capacity or host-provided scratch arena
   would restore the stronger one, at the cost of a bound the modeller has to
   pick. Not taken; recorded because the weakening was deliberate.

8b. **Record `states`** (§5.2). §6.2c opened records as settings by splitting
   them into scalar leaves before §7 sees them; the same trick does not open
   them as states, because the obstacle there is not the expression language but
   the solver ABI, which is a flat `double` vector the host owns (§15.5). A
   record state would have to be either scattered across that vector — which
   makes `state.pose.p.x` an offset computation rather than a member access — or
   held beside it, which is a second state block and a second ABI. Neither is
   obviously right. `se.math.Split3`/`Merge3` cover the need meanwhile, and
   `se.math.Vec3`'s fields are `double` deliberately so that nothing forecloses
   it.

8c. **A parametric composite's interior is not yet checked at its definition**
   (§6.2a). Lifting `SE0418` made composites unit-parametric; substitution
   happens per instantiation, so an interior that holds for only some bindings
   is reported at the call sites using them rather than where it was written.
   Checking it once with `U` left symbolic is the decided semantics and the
   algebra already supports it — an unbound parameter compares only against
   itself. Recorded here rather than in the list below because it is a
   *tightening*: it would reject interiors that are accidentally monomorphic,
   which the ones written so far are not, but which is not nothing.
8d. **Comparisons and conditionals in §7** (§7.4, §7.2a). Finite types made the
   absence visible: a flow-down that picks an enumerator from another setting —
   `param.mode == FAST ? HI : LO` — has no spelling, so for now it is a setting
   of its own. Adding `==`/`!=`/`?:` would still emit as one C++ expression, so
   §7.4's (c) survives; what it costs is a boolean value in an evaluator whose
   every value has so far been a number with a dimension.
8e. **Compute-once intermediates** (§8.3a). Output groups share work through
   helpers that recompute it, and so do `output()` and `derivative()` at one
   stage — a node computing an expensive intermediate pays for it once per
   reader. A declared intermediate, computed once per pass by its own method
   with its own input list and sorted like an output, would remove that. It
   is safe only under one rule, which comes from §9.6: it is recomputed at
   every stage even in a held node, whose `derivative()` runs at trial states while its
   outputs hold. Recording does not need it — a recorded intermediate is an
   output nothing reads (§6.3, §13.5) — so this is about sharing alone, which
   wants opaque C++ types where a recorded column wants DSL ones and units.

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

8c. **A brace list as a wire source** (§6.9.2). A literal endpoint is a scalar,
   so `{0, 0, 0} --> ain` does not exist and a scalar into a record port is
   `SE0410`. Nothing forbids it: §7.2 already destructures an aggregate against
   a declared field tree, and a wire destination has one, so the obstacle is
   scope rather than semantics. It is not taken because `param.<setting>`
   already spells a constant record and spells it *better* — named, overridable
   at configuration, and visible in the manifest — which leaves anonymity as the
   only thing the brace form would buy. Revisit if a model appears that wants a
   throwaway constant record often enough for the setting to feel like
   ceremony.

16a. **Array ports** (§6.3). A runtime-sized member in the signal block would
   destroy the `offsetof` property §15.5 depends on, so a bundle of N signals
   crosses a wire only as N ports or as a fixed record. Array *states* and *vars*
   arrived without it (§6.4a) because neither is in the signal block. If taken
   up, the question to answer first is whether `sig` splits the way `dis` did —
   a struct for fixed ports plus a flat block for array ones. The buffer
   (§5.1a) does not answer it: a buffer's capacity is a literal, so its member
   has a fixed size and needs no such split, and a buffer is opaque bytes, not
   an array of signals the DSL can describe.

16c. **A buffer on the root** (§5.1a, `SE0445`). The root's ports are the
   host's typed `Sim::Inputs` / `Sim::Outputs` and the manifest's boundary rows.
   A root buffer output could publish a copy; a root buffer input has no
   capacity to size `Sim::Inputs` with, since a reader declares none. Both wait
   for the C API roadmap item, which will settle how a host passes bytes in.
   Node tests (§6.13) wait with them (`SE0363`): a test program runs its node
   as the root.

16b. **Instance arrays and generated wiring** — `node stage[N] : Biquad` plus a
   rule for chaining them. This is the one that genuinely costs what §6.4a's
   preamble says: instance count decides the emitted schedule (§15.7), so a
   run-time count means a container of nodes behind a uniform interface, and
   every argument for concrete classes and no virtuals is an argument against
   it. An *elaboration-fixed* instance array is a much smaller feature and is
   the one to consider if the need returns; a cascade of N second-order sections
   does not need it, being one leaf that loops internally.


**Implementation hazards, called out because they fail silently:**

17. **`#line` accounting for injected `native` alias lines** (§11.2). Get the
    offset wrong and every C++ error in that method points a line or two off.
    Two concrete cases were observed in `examples/drivetrain/`. Both are now
    handled by the emitter, and both stay recorded here because they fail
    *silently* — no test goes red for either, so a future rewrite would
    reintroduce them unnoticed.
    - Editing a `.se` body shifts every later body, so hand-maintained `#line`
      values rot silently. Nothing detects it; only a real diagnostic reveals it.
      The emitter sidesteps the arithmetic entirely: it writes the whole
      preamble first and the directive last, immediately before the body text,
      so the injected line count never enters the calculation.
    - A `#line` block set for one body stays in effect over the **generated**
      code that follows it, so a warning about a generated *signature* — an
      unreferenced `in` on an empty `In_output`, say — is attributed to the
      previous method's body. The generator must reset `#line` (to the node's
      declaration line, or to the generated file itself) after each verbatim
      region, not only before one — which is why the emitter's output buffer
      tracks its own line number.
18. **Digit separators and raw strings** in verbatim scanning (§11.2).
