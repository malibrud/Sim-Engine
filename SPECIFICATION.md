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
marks rules with the earliest stage that can detect a violation.

| Stage | Name | Detects |
|---|---|---|
| 1 | **Lexical** | Bad characters, unterminated literals/comments, unbalanced verbatim regions. |
| 2 | **Syntactic** | Grammar violations, misplaced sections, duplicate sections. |
| 3 | **Resolution** | Unknown names, file/name mismatch, root collisions, duplicate members. |
| 4 | **Elaboration** | Unit mismatches, required-setting omissions, setting cycles, non-divisor rates, instantiation. |
| 5 | **Scheduling** | Feedthrough sort, algebraic loops, rate/active-set construction. |
| 6 | **Emission** | Code, unit manifest, slot map, build script. A model with array-shaped states (§6.4a) emits its state slot map's *declarations* here and its concrete rows at configuration (§15.6). |

All six stages are implemented in `src/`, one source file per stage from stage 3
on. Where this document says *"is an error"* without qualification, the stage
column in §16.4 gives the detecting stage.

### 1.4 Terminology

- **Definition** — a `node` or `type` declaration. Lowers to a C++ class/struct.
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
<root>/se/mech/Damper.se          ->  package se.mech, node se.mech.Damper
```

The **standard library is one root** shipped with the toolchain, with no special
status: `std` is an ordinary package name, not reserved. Its "version" is
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
advance on a rate:

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
is discrete. A source has no input, so it is never on the receiving end of a
rate transition, and the property that matters at its use site is which waveform
it produces, not how it stores its phase. The cost is real and is recorded here
rather than hidden: a source hands a continuous consumer a zero-order-held
staircase one base step wide, so RK4 is first-order across that path. A
`se.sig.src.ct` sibling — the same waveforms with `der.` on a continuous phase,
exact at every minor step — is a pure extension if that cost ever bites.

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
settings units inputs outputs states vars native structure declarations build
continuous discrete
init output derivative next on_step final
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
*compatible* iff their dimension vectors are equal. `(-)`, `(1)`, and `(%)` are
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

**Why this is nearly free here.** The identities that normally make angle-as-a-
dimension expensive — `v = ωr`, `τ = Iα` — live *only inside bodies*, where
arithmetic is already unchecked (§4.4). The cost is confined to setting flow-down
(§7), where angular products are rare.

**The larger win.** `(rad/s)` is angle·T⁻¹ while `(Hz)` stays T⁻¹, so they are no
longer compatible and the ω-versus-*f* factor-of-2π mix-up becomes a hard error.

**Deliberately not claimed.** This does *not* separate torque from energy. `τ = Iα`
puts angle in the numerator and `W = τθ` in the denominator, so they disagree —
the genuine wart in the angle-as-dimension literature. Force, energy, and inertia
units stay angle-free; torque and energy remain mutually convertible exactly as
before. No regression, no gain, and the conflict never surfaces because it only
appears in bodies.

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
    derivative(…) { … }
    next(…)       { … }
    on_step(…)    { … }
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
    inertia: double(kg*m^2);                           // required
    radius: double(m) = 0.31;                    // default
    corner_mass: double(kg) = param.total_mass / 4.0;  // derived
}
```

- A setting with **no default is required**: every instantiation must bind it,
  checked statically at stage 4 (`SE0420`).
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

**Reserved setting: `rate`.** A node may declare

```
settings { rate: double(Hz) = 200.0; }
```

`rate` is an ordinary setting in **every** respect — declared, defaulted, bound at
instantiation, flowed down, overridable at configuration, and holding a `param`
member like any other. If declared, it must have unit `(Hz)` and type `double`
(`SE0422`). A node that does not declare `rate` runs at the base step.

Its only distinction is that the scheduler *reads* it (§9.2): the node's
decimation is derived from `param.rate` during configuration, in the same pass
and by the same rule as any other derived value. Before §6.2b, `rate` produced
no runtime member because its value folded into a decimation constant; with
values no longer folded there is nothing left to special-case, and `sample_rate`
(§10.4) is simply a reading of it.

`rate` is **not structural**: it sets a decimation counter, not a shape. The
emitted schedule is a fixed sequence of calls in a fixed order (§15.7); rate
supplies only the divisor that guards each one (§9.6). See §9.2 for the one
consequence, which is that `SE0450` moves to configuration time for an override.

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

##### Leaf-only

A **composite may not declare unit parameters** (`SE0418`). Flowing a unit
parameter to a child is a second flow-down mechanism running parallel to §6.9.1's,
and nothing has yet needed it. This is a restriction, not a design position:
lifting it is a pure relaxation and no model written against this rule would
change meaning.

##### Cost

None at run time. Units are erased at the C++ boundary (I3), so a parametric
definition emits **one class regardless of how many ways it is instantiated** —
the parameter exists only to be checked, and is gone by stage 6. This is what
makes unit polymorphism cheap enough to use everywhere in a library, and it is
why the feature needed no code generator changes at all.

#### 6.2b Configuration

Between construction and `init()`, a compiled model passes through
**configuration**:

1. Every setting holds the value elaboration computed for it.
2. Overrides are applied from the §14 settings source, each **pinning** its
   target.
3. Derived defaults (§6.2) and flow-down expressions (§6.9.1) are re-evaluated
   in dependency order, **skipping pinned settings**, and each node's decimation
   is recomputed from its `rate` (§9.2).
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

**A configuration that fails does not start a run.** Five things can be wrong
here — a dimension that disagrees with §15.6, a `rate` that violates §9.2's
divisor rule, an array extent that is not a non-negative integer, an element
path (`bw.w1[3]`) that is out of range for the extent step 4 computed, and a
host buffer whose size disagrees with what step 4 reported. None can be raised
where it is found, because configuration runs before there is a run to fail. The
failure is recorded against the offending path and `init()` refuses to proceed,
leaving the model in the terminated state `sim.abort` produces (§10.2) with no
node having executed.

**Configuration failures are not `SE` diagnostics.** Every `SE` code names a
compile stage (§16.4) and carries a source span; these arise in a shipped binary
with no source in reach, and the last three of the five are not detectable at
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

- **`rate` is scalar.** It must still be `double(Hz)` (`SE0422`).
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
- Port order matters only for readability. Nothing positional exists.
- **A port may not carry an extent** (`SE0234`), unlike a state or a var (§6.4a).
  A port lives in the signal block, whose offsets come from `offsetof` on a
  generated struct (§15.5); a runtime-sized member would destroy that. A bundle
  of N signals crosses a wire as N ports or as a record. See Appendix C.

### 6.4 `states`

**One `states` section holds both kinds, and the kind keyword is mandatory.**

```
states {
    continuous omega: double(rad/s) = 0.0;
    discrete   cut: double(N*m) = 0.0;
    discrete   ticks: int(-)    = 0;
}
```

Omitting `continuous`/`discrete` is an error (`SE0230`), not a default. There is
deliberately **no preferred state kind**: a default would privilege one, and the
two differ genuinely in write mechanism, in schedule, and in their relationship
to the solver.

| | write accessor | written in | in the solver state vector |
|---|---|---|---|
| `continuous` | `der.<name>` | `derivative()` | **yes** |
| `discrete` | `next.<name>` | `next()` | no |

- The **write accessor is symmetric and separately enforced.** The generated
  `Der` struct carries only continuous fields and the `Store` struct only
  discrete ones, so `der.ticks` and `next.omega` are "no such member" compile
  errors with no body parsing whatsoever (I2). (`Store`, not `Next`: the same
  type is both the committed block `dis` and the buffer `nxt`, so naming it for
  the buffer would be wrong half the time it is used — §15.5.)
- **`next.` means simultaneous update.** A direct `state.x = …` mutation would be
  *sequential*, making results depend on statement order inside `next()`. With
  `next.`, every discrete state reads the old value and writes the new one, which
  is what the state-space form x⁺ = h(t,x,u) actually says. The lowering gives
  the whole model one discrete block and one buffer of the same shape (§15.5):
  every `next()` reads `dis` and writes `nxt`, and the tick ends with a single
  `dis = nxt`. Simultaneity is therefore a property of the **model**, not of each
  node in turn, and there is no per-node commit step. A node that did not sample
  this tick left its slice of `nxt` equal to `dis`, so the whole-block assignment
  is a no-op for it.
- Only `double` may be `continuous` (`SE0431`); the state vector is a flat
  `double` array.
- **The declared default is the initial condition.** There is no separate
  `initial` section — ICs ride the one override channel (§13.4). `init()` is the
  escape hatch for a computed or equilibrium IC.
- Discrete states earn their place over `vars` precisely via that IC channel: a
  declarative default plus a sim-file override, which `vars` never had.

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
        rate: double(Hz) = 1000.0;                 // §6.2's reserved setting
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
        const double K  = 2.0 * param.rate;
        const double wa = K * std::tan(pi * param.fc / param.rate);   // pre-warped
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
  call. The split is what makes ZOH, rate transitions and recording well-defined;
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

The rule is **exact-or-reject, checked twice**, and it is the same rule §9.2
already applies to a decimation:

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

### 6.5 `vars`

```
vars { inertia_inv: double(1/(kg*m^2)); }
```

Per-instance persistent storage that is **not state**: private C++ members,
outside the solver vector and outside the override channel.

- **Readable in every method; writable only in `init()` and `on_step()`.**
  `next()` is pure and cannot write one: it carries the transition equation and
  nothing else (§8.1).
- A `var` needs no default; it is zero-initialised and is expected to be computed
  in `init()`.
- Records are permitted as `vars`.
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
solver. Confining writes to `on_step()` — which runs exactly once per accepted
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
  `output()`, `derivative()` and `next()` the generator simply does not emit the alias, so the
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
instance   := 'node' identifier ':' qualified_name
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

Why declarative and not imperative: under an imperative scheme, every group node
would need hand-written C++ solely to pass a number down, which contradicts §6.1
— a group node does no work. Declarative flow-down also makes required settings
statically checkable, and keeps the flow-down graph *visible to the compiler*,
which is what lets §6.2b re-evaluate it in dependency order at configuration
without the user writing an ordering anywhere.

**A composite's settings therefore exist at run time**, unlike its ports and
wires, which flatten away entirely (§15.2). They are the inputs to the flow-down
expressions, and overriding one at configuration is the whole point of
addressing a path like `front.left.mass` (§13.2). They hold no signal and take
part in no computation beyond §6.2b step 3.

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
| `der.<name>` | — | `derivative()` |
| `next.<name>` | — | `next()` |
| `var.<name>` | every method | `init()`, `on_step()` |
| bare name (`native`) | `init`, `on_step`, `final` | same |

**An array-shaped state or var is indexed through the same accessor** —
`state.w1[i]`, `next.w1[i]`, `der.w[i]`, `var.a1[i]` — with the readability and
writability rules above unchanged. Indexing is ordinary C++ subscripting on the
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

A member may not be named `param`, `in`, `out`, `state`, `der`, `next`, `var`,
`log`, or `sim`, since the generated code puts objects by those names in scope of
bodies (`SE0331`).

### 6.12 Consistency rules

Typo safety comes from these rules rather than from a marker keyword on
functions:

| If the node has… | it must define… | else |
|---|---|---|
| `outputs` | `output()` | `SE0340` |
| any `continuous` state | `derivative()` | `SE0341` |
| any `discrete` state | `next()` | `SE0342` |

The converse is checked for all three, since none could do anything: `output()`
in a node with no outputs (`SE0344`), `derivative()` in a node with no continuous
states (`SE0343`), and `next()` in a node with no discrete states (`SE0345`).

The table is symmetric because each row pairs one state kind with the one pure
method that writes it. `init()`, `on_step()` and `final()` are always optional and
have no converse rule — they carry no equation, so whether a body does anything is
not decidable without parsing it, which I1 forbids.

Note that the rules are per-kind, not blanket: a purely continuous node needs no
`next()`, and a purely algebraic node needs no `derivative()`. Neither needs an
`on_step()`, though either may have one to do per-step `var`, native or logging
work.

---

## 7. The elaboration expression language

A small, total, side-effect-free expression language, evaluated **once per run,
never per tick**. It appears in exactly four places: setting defaults, setting
bindings at instantiation, state defaults, and sim-file / `.settings` values.

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

Every value is a real number with a dimension.

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

### 8.1 The six methods

| Method | Equation | Purity | When it runs |
|---|---|---|---|
| `init()` | — | mutating | Once, at run start, in elaboration order. |
| `output()` | y = g(t, x, u) | **pure** | Whenever signals propagate — many times per step. |
| `derivative()` | ẋ = f(t, x, u) | **pure** | Every minor step (4× per RK4 step). |
| `next()` | x⁺ = h(t, x, u) | **pure** | Once per accepted step, at the node's sample instants. |
| `on_step()` | — | mutating | Once per accepted step, immediately after `next()`. |
| `final(ctx)` | — | mutating | Once at run end, in reverse init order. |

Write permissions, exactly:

- `init()` writes `state` (the IC escape hatch) and `var`; may touch natives; may log.
- `output()` writes `out` only.
- `derivative()` writes `der` only.
- `next()` writes `next` only.
- `on_step()` writes `var`; may touch natives; may log.
- `final(ctx)` writes nothing model-visible; may touch natives; may log.

**The naming rule is the purity column.** The three methods that carry an
equation are pure and are named for the accessor they write — `out.` → `output()`,
`der.` → `derivative()`, `next.` → `next()`. The three that carry none have no
quantity to be named for, so they are named for the moment they fire: `init()`,
`on_step()`, `final()`. Nothing else in the language is prefixed, and the singular
follows the accessor rather than the section, which is why `output()` is singular
though `outputs {}` is plural.

`on_step()` earns its name by having no equation. It is the model's only general
per-step mutating hook — `var`, natives, logging — and a node may define it with
no `discrete` state at all. It is *not* the discrete counterpart of
`derivative()`; `next()` is. Keeping the two apart is what lets `next()` be pure
and lets [§6.12](#612-consistency-rules) check it in both directions.

"Mutating" above describes what the method may change in the *model*, not the
constness of the generated C++ member. All five of `output`, `derivative`, `next`,
`on_step` and `final` lower to `const` members, and each receives what it may
write as an out-parameter — `out`, `der`, `next`, `var`. Only `init()` is
non-const, because it writes its own storage directly. See §15.4: that uniformity
is what makes the write permissions above compile-time facts rather than
documented conventions.

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
on_step(ws)           { … }                                         // constrains nothing
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
- Only `output()`'s list constrains ordering. `derivative()`, `next()` and `on_step()` run after
  outputs have propagated, so their lists are documentation plus enforcement.

### 8.4 Why mutation is confined

`output()` and `derivative()` are evaluated **four times per RK4 step, at trial states
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
- `derivative()` and `next()` may run in **any** order once outputs have propagated.
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

### 9.2 Sampling rates and decimation

`rate` is a reserved setting (§6.2) set at instantiation, flowed down through the
expression language, and **constant from configuration onward** (§6.2b). A 100 Hz
PID and a 50 Hz PID are the same definition with different instances.

The decimation is `base_rate / rate`. It is a **counter divisor, not a shape**:
the emitted schedule is a fixed sequence of direct calls in a fixed order
(§15.7), and rate supplies only the modulus that guards each one (§9.6 step 1).
Nothing about which nodes exist, how they are wired, or how they sort depends on
it — §9.3 requires one topological order to be valid whether or not a node
samples, so the order cannot be rate-derived.

A rate that is not an integer divisor of the base rate is an error. `sec` checks
the elaborated default:

```
error[SE0450]: rate 60 Hz is not an integer divisor of the 1 kHz base step
  --> Vehicle.se:31:22
   = note: 1000 / 60 = 16.667; nearest legal rates are 62.5 Hz and 58.82 Hz
```

A `rate` **override** supplied at configuration is checked by the loader instead,
and the same violation fails there — deterministically, before the first tick,
with the offending path named. This is the one check that moves out of `sec` as a
consequence of §6.2b, and it moves only for values `sec` never sees.

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

One base tick at index *k*, normatively:

1. **Set the time** to *t*[k], and determine the active set — base-rate nodes,
   plus nodes whose decimation divides *k*.
2. **Propagate outputs.** `output()` in topological order over the active set,
   at the tick's entry state. Held outputs are constants throughout.
3. **`next()`** on every active node with discrete states, in any order. Each
   reads `dis` and writes `nxt`.
4. **`on_step()`** on every active node that defines it, in any order.
5. **Record** (§13.5) if this tick is a recording tick.
6. **Exit** if the run has been halted (§10.2) or *k* is the last tick.
7. **Advance both state kinds.** `dis = nxt` for the discrete block — and a
   second whole-block copy for `dis_arr` if the model has array-shaped discrete
   states (§15.5) — then `x` by the solver's combination of stage rates, which
   is where `output()` and `derivative()` are evaluated, once per solver stage,
   at trial states the trajectory never visits (§9.5).
8. If `mode: realtime` and this tick closes a frame, spin to the frame boundary
   and update the metrics.

Two properties are load-bearing, and both come from advancing at the end.

**The whole tick observes one state.** Steps 2–5 all see *x*[k] and *dis*[k], so
one recorded row is one consistent sample of the model at *t*[k] — signals,
continuous states and discrete states alike. It is also why `output()` sees
*dis*[k] rather than *dis*[k+1]: y[k] = g(x[k]) precedes x⁺ = h(x[k], u[k]),
which is what discrete-time state space says.

**A stopped run holds a valid checkpoint.** The exit at step 6 is above both
advances, so a run ending on `sim.stop`, on `sim.abort` or on the tick count
leaves `x` + `sig` + `dis` mutually consistent — the completeness §15.5 claims
for that triple.

`next()` precedes `on_step()`, and both read the same `dis`[k], so the order
matters only through `var`: a value `on_step()` learns from a native at tick *k*
is stashed in a `var` and reaches a discrete state through `next()` at tick
*k+1*. For I/O that is an honest one-sample transport delay.

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
- **Escape hatch:** `log.trace` alone is permitted in the three pure methods.
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

### 10.4 `sample_rate` and `time_step`

Two constants in the §7 expression language, giving the node's **effective** rate:

| | | |
|---|---|---|
| `sample_rate` | `(Hz)` | the node's declared `rate` (§6.2), or the base rate if it declares none |
| `time_step` | `(s)` | `1 / sample_rate` |

```
node Biquad {
    settings { wn: double(rad/s);
               fs: double(Hz) = sample_rate; }   // 1 kHz base step -> 1000
}
```

A discrete filter cannot compute its coefficients without its own sample period —
`K = 2·fs` in a bilinear transform — and §6.2 gives the reserved `rate` setting no
runtime member, so it cannot be read directly.

- They are **effective, not global.** A node declaring `rate = 200 (Hz)` inside a
  1 kHz model sees `sample_rate = 200` and `time_step = 0.005`, not the base step.
  That is the point: coefficients designed at the wrong rate are silently wrong.
- **Both spellings ship** because coefficient design wants a rate while anything
  expressed as a period wants the reciprocal. Both derive from one number, so
  `sample_rate * time_step` is exactly 1 and they cannot drift apart.
- They are **derived from the node's own `rate`**, not ambient context. This is what
  keeps §10.2's objection from applying: there is no "which fields are live when"
  question, because these are a function of one of the node's own settings and are
  resolved with them at configuration (§6.2b). Overriding `rate` moves `sample_rate`
  and `time_step` with it, and any coefficient derived from them follows — which is
  the behaviour a filter needs and the reason they are not folded.
- The reserved `rate` setting is resolved **before** any other setting of the same
  node, so a derived setting may read `sample_rate` freely.

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
- **The model is a header; the driver is its own translation unit.** The script
  compiles `<name>.main.cpp`, which contains nothing but `main()` and an
  `#include` of `<name>.generated.hpp`. A host program that wants to drive the
  root boundary itself includes the header and supplies its own `main()`,
  without editing a file stamped DO NOT EDIT; the driver can be suppressed
  entirely. Both the driver and a host use the same `init()` / `done()` /
  `tick()` / `finish()` calls, so neither can drift from the other.
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
| `window:` | time expression, at least `sync`; `realtime` only | `1 (s)` |
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

### 13.4 Initial conditions

There is **no `initial:` section**, deliberately. Initial conditions are
declarative state defaults in the model (§6.4), and overriding one from outside
rides the **same settings channel, addressed by path**:

```
total_mass          = 1800 (kg);    // a setting
front.left.velocity =    2 (m/s);   // a state's initial value
bw.w1[3]            =    0 (-);     // one element of an array state (§6.4a)
```

Layering: declared state default → `init()` code → external override.

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
signals, continuous states and discrete states alike. Recording happens before
either state kind advances (§9.6 steps 5 and 7), so a row never mixes vintages.

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
- **An element of an array state is recorded by index** — `bw.state.w1[3]` — and
  the column is named that way. As with an override (§13.4), an index past the
  configured extent is a configuration failure, not a compile error; and since
  the header is written after configuration, the file's shape is settled by then
  and never changes mid-run.
- Wildcards (`plant.out.*`) are not in this version; see Appendix C. They would
  be worth more now than before: naming every element of an array state
  individually is exactly the case a wildcard removes.

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
- The window is **a whole number of frames**, `round(window / sync)`. Rounding is
  the honest behaviour — unlike `sync / step`, where §9.4 demands an exact
  multiple because sample instants have to land — but rounding to *zero* is not:
  the metrics would then report the latest frame rather than a rolling average,
  which is a different quantity under the same name. A `window` shorter than one
  frame is therefore an error (`SE0264`), not a silent collapse to one frame.
- `window:` only means anything in real time, so giving it under `mode: batch` is
  an error (`SE0263`) rather than a value that is read and ignored. Recording
  `sim.*` in a batch run would otherwise produce a column of zeros with nothing
  said.
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

### 14.1 The third carrier: a compiled model

The same grammar is accepted by a **compiled model** at configuration (§6.2b),
which is the carrier `sec` never sees. A model exposes three operations:

| | |
|---|---|
| `load_settings` | apply a §14 source; each binding pins its target |
| `resolve_settings` | re-evaluate derived defaults and flow-down, skipping pinned |
| `dump_settings` | emit a §14 source describing the model's current settings |

**Record settings cost the runtime nothing** (§6.2c). A leaf is a flat path and
a number, which is what this section already describes, so nothing below changed
when records became settable.

**The runtime accepts a restricted right-hand side.** `sec` evaluates all of §7;
a compiled model accepts a **number with an optional unit suffix** and nothing
else — no arithmetic, no builtins, no `param.*`. Two reasons: the full evaluator
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
The dump emits canonical units and every settable path, so it doubles as a
golden record of what a binary was configured with — and as the readable
inventory of what a model exposes, which no external document has to be kept in
sync with.

Structural settings (§6.2) appear in a dump, marked, and are **rejected on load**:
they are shape, and shape was fixed when the binary was generated.

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

By value for both is what lets **both convert**. A conversion folds to a constant
and lands in the view's initialiser: a scalar as `sig.src.y * 0.001`, a record as
a brace initialiser applying each field's own factor,
`Vec3{(sig.src.v.x * 0.001), (sig.src.v.y * 0.001), (sig.src.v.z * 0.001)}`.
Nothing else in the generated code ever needs conversion machinery.

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
  out-parameter.** `output()`, `derivative()`, `next()`, `on_step()` and `final()` are all const
  members, so no body can write `state`, `var` or `param` through `this`. What a
  method may write, it receives: `Out& out`, `Der& der`, `Store& next`, `Var&
  var`. This is one rule, applied uniformly, and it is why `on_step()` can write
  a `var` (§6.5) while still reaching a state only through `next.` (§6.4).
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
- **I/O confinement is name withholding.** Native aliases and the full `log`
  object are emitted into the preamble of the methods where they are legal, and
  simply are not declared elsewhere. Natives are `mutable`, which gives up
  nothing: §6.7 already establishes that `const` never protected a handle,
  because a `const SOCKET` is still perfectly usable by `recv()`.

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
    State                 state;
    Var                   var{};
    const se_rt::TraceLog log{se_path_};  // log.trace only (§10.1)

    void init();                                              // mutating
    void derivative(const In_derivative& in, Der& der) const; // pure
    void output(const In_output& in, Out& out) const;         // pure
};
```

A consequence worth stating, because it is the property a reader of generated
code most wants: **the three pure methods need no preamble at all.** The
trace-only `log` is a member, and everything else they may touch is already
const through `this`, so between their braces there is nothing but the verbatim
body.

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
of them needs a line of generated accessor code. **`x` + `sig` + `dis` is a
complete checkpoint of the model.**

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
  unchanged; there are now two block copies at the bottom of the tick instead of
  one.
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

**`resolve_settings` also derives the schedule's divisors.** A node that declares
`rate` gets a decimation *member*, and the tick guard reads it — `if (k %
dec_decim == 0)` where the emitted code would otherwise have carried
`if (k % 5 == 0)`. This is what makes §9.2's "counter divisor, not a shape" true
of the generated code and not merely of the language: an override moves the
guard, and nothing about the call sequence or its order moves with it. It is also
where the divisor rule is re-checked (§16.4), the elaborated default no longer
being the value that runs.

#### 15.5a The state buffers are the host's

When a model has array-shaped states its block sizes are not known until §6.2b
step 4, so `Sim` cannot own `std::array` members and will not own `std::vector`
ones. **The host allocates and hands the buffers in.** The engine performs no
allocation for any published block, before a run or during one.

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

[state.discrete]          # Sim::dis; offsets from Sim::discrete_map()
          tc.cut                N*m         double

[state.discrete.array]    # Sim::dis_arr; rows from Sim::discrete_array_map()
dis_arr[?]  bw.w1 [param.nbq]   -           dynamic
dis_arr[?]  bw.w2 [param.nbq]   -           dynamic

[signals]                 # Sim::sig; offsets from Sim::signal_map()
          in.axle_torque        N*m         double
          whl.ws.speed          rad/s       double

[boundary.in]             # root inputs the host must drive, as sig.in.*
          in.axle_torque        N*m

[boundary.out]            # root outputs the host may read
          whl.ws.speed          rad/s

[settings]                # configurable; value is the elaborated default
          whl.inertia           kg*m^2      = 0.9
          tc.rate               Hz          = 200.0
          bw.order              -           = 4
          cue.limit.x           m/s^2       = 5.0
          cue.limit.z           m/s^2       = 4.0

[types]
drivetrain.WheelState.speed     rad/s
se.math.Vec3                   units { U }
se.math.Vec3.x                 U
```

Field units in `[types]` are shown **as declared**, so a parametric record (§5.2)
reports its parameter rather than any one binding — the type is shared, and no
single binding would be true of it. The substituted units are the per-field rows
in `[signals]` and `[boundary.*]`, which is where a host reads what to send.

Paths are **root-relative** (§13.2): the `root:` entry names a definition, not an
instance, so there is no prefix for the root itself.

**Byte offsets are not in the manifest, deliberately.** Padding is decided by the
C++ compiler, so a table written by `sec` would be a guess, and a wrong offset
table is worse than none. The manifest names, units and types each slot; the
generated header exposes the offsets, built with `offsetof` where they are
actually known:

```cpp
static const se_rt::Slot* signal_map(std::size_t& count);
static const se_rt::Slot* discrete_map(std::size_t& count);
// Non-static, and emitted only for a model with array-shaped states: one
// block each, so one accessor each. Both are built by configure().
const se_rt::Slot* state_map(std::size_t& count) const;            // Sim::x
const se_rt::Slot* discrete_array_map(std::size_t& count) const;   // Sim::dis_arr
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
`SE04xx` elaboration, `SE05xx` scheduling, `SE06xx` emission.

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
| `SE0204` | 2 | Expected a definition (`node` or `type`) |
| `SE0205` | 2 | Wrong number of unit arguments on a scalar type |
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
| `SE0231` | — | *Retired.* A setting may not be a record type — lifted by §6.2c. The number stays burned; codes are stable. |
| `SE0232` | 2 | A state may not be a record type |
| `SE0233` | 2 | A `native` member may not carry a unit |
| `SE0234` | 2 | An extent is not permitted on this declaration (port, setting, record field, `native`) |
| `SE0235` | 2 | Malformed array extent |
| `SE0240` | 2 | Helper function has no body |
| `SE0241` | 2 | `init` takes no parameters |
| `SE0242` | 2 | `final` takes at most one parameter |
| `SE0243` | 2 | A brace list mixes positional and named fields |
| `SE0250` | 2 | Unknown platform atom in `when` |
| `SE0251` | 2 | Unknown build primitive |
| `SE0260` | 2 | Duplicate entry in `sim` |
| `SE0261` | 2 | Unknown entry in `sim` |
| `SE0262` | 2 | Missing required `sim` entry |
| `SE0263` | 2 | `sync` or `window` requires `mode: realtime` |
| `SE0264` | 4 | Bad value for a `sim` entry (solver, mode, log level, `window` shorter than one frame) |
| `SE0301` | 3 | `package` declaration disagrees with the file's location |
| `SE0302` | 3 | No public declaration matching the file name |
| `SE0303` | 3 | Imported name collides |
| `SE0304` | 3 | Unresolved name |
| `SE0305` | 3 | Package path resolves in more than one root |
| `SE0310` | 3 | Cycle in record field types |
| `SE0311` | 3 | Field-level wire endpoints are not supported |
| `SE0312` | 4 | Recursive instantiation: a node transitively contains itself |
| `SE0313` | 3 | Wrong number of unit arguments on a type reference |
| `SE0320` | 3 | Node has both code and a `structure` block |
| `SE0330` | 3 | Duplicate member name in a node |
| `SE0331` | 3 | Member name shadows a generated accessor |
| `SE0340` | 3 | `outputs` declared but no `output()` |
| `SE0341` | 3 | Continuous state declared but no `derivative()` |
| `SE0342` | 3 | Discrete state declared but no `next()` |
| `SE0343` | 3 | `derivative()` in a node with no continuous states |
| `SE0344` | 3 | `output()` in a node with no outputs |
| `SE0345` | 3 | `next()` in a node with no discrete states |
| `SE0350` | 3 | Method parameter is not a declared input |
| `SE0351` | 3 | Duplicate method parameter |
| `SE0402` | 4 | Lossy literal conversion to an integer target |
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
| `SE0418` | 3 | A composite may not declare unit parameters |
| `SE0419` | 4 | Wrong number of unit arguments at an instantiation |
| `SE0422` | 4 | Reserved setting `rate` has the wrong unit or type |
| `SE0423` | 4 | Wrong number of values in a positional brace list |
| `SE0424` | 4 | Unknown field name in a brace list |
| `SE0425` | 4 | A field is given twice in a brace list |
| `SE0426` | 4 | A brace list where a scalar is declared, or a value where a record is |
| `SE0430` | 4 | Unconnected input |
| `SE0431` | 4 | A `continuous` state must be `double` |
| `SE0432` | 4 | An array extent must be a dimensionless, non-negative integer |
| `SE0433` | 4 | An array state or var must be `double` |
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

**Configuration-time failures are not `SE` codes.** A §14 source presented to a
compiled model (§14.1) is diagnosed by that model's loader, not by `sec`, and
reports through `load_settings`'s return rather than through this catalogue. The
three failures are: an unknown path, a right-hand side outside the restricted
grammar, and a dimension that disagrees with §15.6 — including a `rate` override
that violates `SE0450`'s divisor rule, which is the only entry above with a
runtime counterpart. Each names the offending path. `sec`'s own checks are
unchanged: everything it can see, it still rejects at the stage listed.

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

type_def        = "type" IDENT "{" [ units_sec ] { field_decl } "}" ;
field_decl      = IDENT ":" type_ref ";" ;

(* One production for every declaration, scalar or record. A one-segment path
   naming a closed scalar type (5.1) takes its unit as the argument: one for
   `double`/`float`/`int`, none for `bool`. *)
type_ref        = qualified_name [ "(" unit_expr { "," unit_expr } ")" ] ;

(* ─── nodes ──────────────────────────────────────────────────────────── *)

node_def        = "node" IDENT "{" { node_item } "}" ;

node_item       = settings_sec | units_sec | inputs_sec | outputs_sec | states_sec
                | vars_sec | native_sec | declarations_sec | build_sec
                | structure_sec
                | lifecycle_method
                | helper_function ;

settings_sec    = "settings" "{" { setting_decl } "}" ;
setting_decl    = IDENT ":" type_ref [ "=" expression ] ";" ;

units_sec       = "units" "{" { unit_param } "}" ;      (* §6.2a *)
unit_param      = IDENT ";" ;

inputs_sec      = "inputs"  "{" { port_decl } "}" ;
outputs_sec     = "outputs" "{" { port_decl } "}" ;
port_decl       = IDENT ":" type_ref ";" ;

states_sec      = "states" "{" { state_decl } "}" ;
state_decl      = state_kind IDENT [ extent ] ":" type_ref
                  [ "=" expression ] ";" ;
state_kind      = "continuous" | "discrete" ;
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
structure_item  = instance_decl | wire_stmt ;

instance_decl   = "node" IDENT ":" qualified_name
                  [ unit_args ]                          (* §6.2a *)
                  [ "{" { setting_binding } "}" ] ";" ;
unit_args       = "(" unit_expr { "," unit_expr } ")" ;
setting_binding = IDENT "=" expression ";" ;

wire_stmt       = endpoint "-->" endpoint { "," endpoint } ";" ;
endpoint        = endpoint_head "." IDENT { "." IDENT } ;
endpoint_head   = "self" | IDENT ;

(* ─── methods ────────────────────────────────────────────────────────── *)

lifecycle_method= "init"    "(" ")"                VERBATIM_BLOCK
                | "final"   "(" [ IDENT ] ")"      VERBATIM_BLOCK
                | "output"  "(" [ ident_list ] ")" VERBATIM_BLOCK
                | "derivative" "(" [ ident_list ] ")" VERBATIM_BLOCK
                | "next"       "(" [ ident_list ] ")" VERBATIM_BLOCK
                | "on_step"    "(" [ ident_list ] ")" VERBATIM_BLOCK ;
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
                | "(" expression ")" ;

(* §6.2c — a record brace list. Wholly positional or wholly named, never
   mixed (SE0243). It is destructured against the declared field tree at the
   declaration site and never evaluated, so §7 stays scalar. *)
aggregate       = "{" [ agg_positional | agg_named ] "}" ;
agg_positional  = expression { "," expression } ;
agg_named       = IDENT "=" expression { "," IDENT "=" expression } ;

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
build       continuous  declarations  derivative  discrete   final     init
inputs      native      next          node        on_step    output    outputs
package     self        settings      sim         states     structure type
units       use         vars
```

`param`, `in`, `out`, `state`, `der`, `next`, `var`, `log`, and `sim` are
additionally unavailable as member names (§6.11), because the generated code puts
objects by those names in scope of bodies.

Everything else — including every sim-file key, every build primitive, `when`, and
every platform atom — is contextual and remains usable as an identifier.

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
   the `init`/`on_step`/`final` confinement excludes it. A real need with no
   mechanism yet.
2a. **Ambient `time` in the pure methods** (§10.2). Time is withheld from bodies as
   part of the broader "`sim.*` as ambient context" design, and §10.4 leans on that
   withholding to justify `sample_rate`/`time_step`. But §10.2's stated objection —
   inheriting the "which fields are live when" question — does not apply to `t`,
   which has a well-defined value at every invocation, unlike `sim.overrun_count`
   or `sim.frame_slip`. Nor does purity: §8.1 writes the equations as
   `y = g(t,x,u)` and `ẋ = f(t,x,u)`, so the language supplies `x` and `u` and
   withholds the `t` its own formulation names. Every source in `se.sig.src`
   therefore carries a redundant clock, and — the sharp part — a **continuous**
   node cannot get the right time at all: a `discrete` `t` is constant across the
   four RK4 substeps, while `Sim::derivatives(double t)` already *receives* the
   correct minor-step time and discards it. If taken up, the open sub-questions
   are the spelling (a bare `time` reads better than `sim.time`, since
   `sim.stop`/`sim.abort` are scoped to the mutating methods and `sim.` would then
   mean two different scopes) and whether `output()` sees the substep time or the
   tick time.
3. **Field-level wires** (§5.3) — `whl.ws.speed --> tc.u`. The grammar admits it;
   the semantics currently reject it. `se.math.Split3`/`Merge3` cover the need
   meanwhile, at the cost of two nodes per bundle.
4. **Coordinate bases for record types.** A `Vec3(m/s)` in a body frame and one
   in an inertial frame are the same type today, and adding them is a modelling
   error nothing catches. A *checked* tag is the only shape available: unlike a
   unit, a frame transform cannot be inserted automatically, because it needs
   the current attitude — a run-time value — where §4.2 promises conversions
   fold to a compile-time constant. So a mismatch could only ever be an error the
   modeller fixes with an explicit transform node, never a silent conversion.
   Deliberately unresolved; the standard library documents one convention (SAE)
   instead. **If it is ever taken up, do not call it `frame`** — that word is the
   real-time `sync`/`window` machinery of §13.6. `basis` or `axes`.
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

16a. **Array ports** (§6.3). A runtime-sized member in the signal block would
   destroy the `offsetof` property §15.5 depends on, so a bundle of N signals
   crosses a wire only as N ports or as a fixed record. Array *states* and *vars*
   arrived without it (§6.4a) because neither is in the signal block. If taken
   up, the question to answer first is whether `sig` splits the way `dis` did —
   a struct for fixed ports plus a flat block for array ones.

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
