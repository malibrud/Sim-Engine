# sim-engine — Design Specification

**A continuous / system-dynamics simulation engine for C and C++.**

Version 0.1.0 · Status: Draft · Normative API: [`include/sim.h`](include/sim.h)

---

## 1. Purpose and scope

`sim-engine` numerically integrates systems of ordinary differential equations
(ODEs) over time. It targets the *continuous / system-dynamics* modeling
paradigm: a model's state evolves according to time-derivative relationships,
integrated forward by a numerical solver. Classic use cases include physical
dynamics (mechanics, circuits, thermal), control systems, and system-dynamics
stock-and-flow models (population, epidemiology, economics).

The engine is written in C (C99) with a stable C ABI, and ships an optional
thin C++ wrapper (§13) for ergonomic model authoring. The numeric core has no
dependencies beyond the C standard library.

### 1.1 Design goals

1. **Correctness and reproducibility.** Bit-identical results for identical
   inputs, integrator, and configuration — across runs, platforms sharing IEEE
   754 double, and thread counts. Stochastic models are reproducible from a seed.
2. **Separation of concerns.** *Models* declare dynamics; *integrators* advance
   state; the *driver* orchestrates time, events, and I/O. These three are
   independently testable and swappable.
3. **Small, predictable hot path.** No allocation, no locking, and no dynamic
   dispatch inside a step beyond the integrator vtable and the model callbacks.
4. **Composability.** Small models compose into larger ones via typed ports and
   connections, with automatic execution-order resolution.
5. **Embeddability.** A flat C ABI with caller-owned memory, suitable for
   linking into larger applications, real-time loops, or language bindings.

### 1.2 Non-goals

- Full differential-algebraic equation (DAE) solving with implicit constraints
  (only ODEs with explicit algebraic *outputs* are in scope; see §4.2).
- Partial differential equations / spatial discretization (out of scope; a PDE
  reduces to an ODE system via the user's own method-of-lines and is then in
  scope as an opaque state vector).
- Symbolic model compilation or a modeling language (à la Modelica). Models are
  authored as C callbacks or via the composition API (§9).
- Automatic differentiation. Jacobians for stiff solvers are supplied by the
  model or approximated by finite differences (§7.5).

---

## 2. Mathematical foundation

### 2.1 State-space form

A model is a first-order ODE system with algebraic outputs and optional event
indicators:

```
    dx/dt = f(t, x, u)      state derivatives   (x ∈ ℝⁿˣ)
    y     = g(t, x, u)      outputs             (y ∈ ℝⁿʸ)
    z     = h(t, x, u)      event indicators    (z ∈ ℝⁿᶻ)
```

- `t` is simulation time, `x` the continuous state vector, `u` the input vector.
- `f` **must** be a pure function of `(t, x, u)`: integrators evaluate it at
  trial points that are *not* on the accepted trajectory, so it may carry no
  hidden per-step state. Parameters and instance data live in the model's
  opaque `ctx` and are constant across a step.
- Higher-order systems are reduced to first order by state augmentation. A
  second-order system `q̈ = a(t, q, q̇)` becomes `x = [q, q̇]`,
  `f = [q̇, a(t, q, q̇)]`.

### 2.2 System-dynamics mapping

Stock-and-flow models map directly onto this form: **stocks** are states `x`,
**flows** are the derivative terms in `f`, and **auxiliaries/converters** are
intermediate quantities computed inside `f` (or exposed as outputs `y`). A stock
with inflow `in` and outflow `out` contributes `dx_i/dt = in − out`.

### 2.3 Hybrid dynamics (state and time events)

Purely continuous integration cannot represent discontinuities (a ball bouncing,
a switch flipping, a valve opening). The engine models these as **events**:

- **State events** occur when an indicator `z_i(t, x, u)` crosses zero. The
  driver detects the sign change over a step and localizes the crossing time
  `t*` (§7.4). At `t*` the model's `handle_event` may apply a discontinuous
  **reset** to `x` (e.g. negate a velocity). Between events the trajectory is
  continuous and the solver's order assumptions hold.
- **Time events** are scheduled instants (e.g. a controller sampling at 100 Hz).
  They are modeled as forced step boundaries (the driver caps `h` so a step
  lands exactly on the event time).

This "integrate → detect → localize → reset → restart" cycle is the standard
hybrid-system semantics; it keeps each continuous segment smooth so adaptive
error control remains valid.

### 2.4 Stochastic extension (SDEs)

For stochastic models `dx = f(t,x,u) dt + σ(t,x,u) dW`, the engine supplies a
counter-based RNG (§8) that generates Wiener increments deterministically from
`(seed, stream_id, step_index)`. The Euler–Maruyama scheme is the recommended
fixed-step method; adaptive control is *not* meaningful for the stochastic term
and must be disabled (use a fixed-step integrator). SDE support is a convention
layered on the deterministic core, not a distinct solver.

---

## 3. Architecture

Four layers, each depending only on those below it:

```
        ┌──────────────────────────────────────────────┐
        │  Experiment layer                            │   parameter sweeps,
        │  (scenarios, sweeps, batch orchestration)    │   ensembles, sensitivity
        └──────────────────────────────────────────────┘
        ┌──────────────────────────────────────────────┐
        │  Driver / orchestration                      │   sim_run: time loop,
        │  (time stepping, events, observers, inputs)  │   event detection, logging
        └──────────────────────────────────────────────┘
        ┌───────────────────────┬──────────────────────┐
        │  Integrators          │  Composition         │   RK4, RK45(DP), backward
        │  (sim_integrator)     │  (sim_composer)      │   Euler; port wiring + sort
        └───────────────────────┴──────────────────────┘
        ┌──────────────────────────────────────────────┐
        │  Numeric core & model interface              │   sim_model, sim_ode_system,
        │  (vectors, RNG, status, scalar config)       │   sim_rng, sim_real
        └──────────────────────────────────────────────┘
```

- **Numeric core** defines scalar types (`sim_real`, `sim_time`), the model
  vtable, the ODE-system closure integrators consume, status codes, and the RNG.
- **Integrators** implement a single `step()` contract (§6) and know nothing
  about models, events, or I/O.
- **Composition** turns a graph of submodels into one flat `sim_model`.
- **Driver** (`sim_run`) owns the time loop, event handling, input evaluation,
  and observer callbacks.
- **Experiment layer** is thin, built entirely on top of `sim_run` (§10).

The key architectural seam is `sim_ode_system` (§6.1): the driver binds the
current inputs `u` into a flat `rhs(t, x) → dx/dt` closure, so integrators never
see the model, and models never see the integrator.

---

## 4. Model interface

Defined by `sim_model` and `sim_model_vtable` in [`include/sim.h`](include/sim.h).

### 4.1 Structure

```c
typedef struct sim_model {
    const char             *name;
    sim_index               n_x, n_u, n_y, n_z;
    bool                    direct_feedthrough;   /* does y depend on u? */
    void                   *ctx;                   /* model-owned instance data */
    const sim_model_vtable *vt;
} sim_model;
```

Dimensions are fixed for the model's lifetime. `ctx` holds parameters and any
persistent instance data; the engine treats it as opaque.

### 4.2 Callbacks and their contract

| Callback           | Required        | Contract |
|--------------------|-----------------|----------|
| `init`             | optional        | Write `x0[0..n_x)`. Called once at `t_start`. NULL ⇒ state zero-initialized. |
| `derivatives`      | **required**    | Write `dxdt[0..n_x)`. Pure in `(t,x,u)`. Called multiple times per step. |
| `outputs`          | if `n_y>0`      | Write `y[0..n_y)`. If `direct_feedthrough` is false, may ignore `u`. |
| `event_indicators` | if `n_z>0`      | Write `z[0..n_z)`. Continuous in `(t,x,u)`. |
| `handle_event`     | if `n_z>0`      | May overwrite `x` (reset). Return `true` if it did (forces solver restart). |
| `terminate`        | optional        | Cleanup. Called once after the run or on error. |

**Purity rule.** `derivatives`, `outputs`, and `event_indicators` must not
mutate observable state (including `ctx` fields that affect results). The engine
relies on this to call `derivatives` at rejected/trial points. Caching that is
transparent (memoization keyed on inputs) is permitted; anything else is a bug.

**Ownership.** All array arguments are engine-owned scratch, valid only for the
duration of the call. Callbacks must not retain pointers to them.

**Direct feedthrough.** `direct_feedthrough == true` declares that `y` depends
on `u` in the same instant (an algebraic path input→output). The composer uses
this to order execution and detect algebraic loops (§9.3). A pure integrator
block (output = a state) has no feedthrough and should set it false.

### 4.3 Minimal model (illustrative)

Exponential decay `ẋ = −k·x`, one state, no inputs/outputs/events:

```c
typedef struct { sim_real k; sim_real x0; } decay_params;

static void decay_init(void *c, sim_time t0, sim_real *x0) {
    (void)t0; x0[0] = ((decay_params*)c)->x0;
}
static void decay_deriv(void *c, sim_time t, const sim_real *x,
                        const sim_real *u, sim_real *dx) {
    (void)t; (void)u; dx[0] = -((decay_params*)c)->k * x[0];
}
static const sim_model_vtable DECAY_VT = { decay_init, decay_deriv, 0,0,0,0 };

sim_model make_decay(decay_params *p) {
    sim_model m = {0};
    m.name = "decay"; m.n_x = 1; m.ctx = p; m.vt = &DECAY_VT;
    return m;
}
```

Full worked examples are in §11 and `examples/`.

---

## 5. Time, state, and the simulation loop

### 5.1 The driver algorithm

`sim_run` implements the following loop. `x` is the accepted state, `t` the
accepted time. `u` is re-evaluated wherever `f`, `g`, or `h` are needed.

```
initialize model → x, at t = t_start
evaluate u(t_start); compute y; notify observer(t_start)
h ← cfg.h
while t < t_stop and no stop requested:
    h ← clamp(h, h_min, h_max)
    h ← min(h, t_stop − t)                      # never overshoot the end
    h ← min(h, next_output_time − t)            # land on output samples
    h ← min(h, next_time_event − t)             # land on scheduled events

    bind u(t) into the ODE closure                                 # (§6.1)
    status ← integrator.step(sys, t, h, x, x_trial, &err, &h_next) # (§6)

    if adaptive and err > 1:                     # local error too large
        h ← h_next                               # shrink and retry (no accept)
        if h < h_min: return SIM_ERR_STEP_TOO_SMALL
        continue

    if cfg.detect_events:                        # (§7.4)
        z0 ← h(t,   x,       u)
        z1 ← h(t+h, x_trial, u)
        if sign_change(z0, z1):
            t*, x* ← localize_crossing(...)      # bracket + refine to event_tol
            fired  ← which indicators crossed
            reset? ← model.handle_event(t*, x*, u, fired)
            t ← t*;  x ← x*
            if reset?: integrator.reset()        # discard multistep history
            notify observer(t*)                  # event sample
            continue                             # restart from the event

    t ← t + h;  x ← x_trial                      # accept the step
    if adaptive: h ← h_next
    if reached an output sample:
        compute y ← g(t, x, u);  notify observer(t)

notify observer(t_stop); call model.terminate; return result
```

Notes:

- **Output decoupling.** The logging cadence (`output_dt`) is independent of the
  integration step. With adaptive stepping the solver may take large or small
  steps internally while the observer still sees a uniform time grid, because the
  loop caps `h` to land exactly on each sample. `output_dt == 0` logs every
  accepted step (useful for debugging step-size behavior).
- **Non-finite guard.** After each step the driver checks `x_trial` for NaN/Inf
  and returns `SIM_ERR_NONFINITE` rather than propagating garbage.
- **Determinism.** The loop contains no wall-clock timing, no unordered
  iteration, and no allocation. Given the same model, integrator, and `cfg`, the
  exact sequence of `f`/`g`/`h` evaluations is fixed (§8.1).

### 5.2 Working memory

`sim_run` allocates one contiguous scratch arena at entry sized from the model
dimensions and integrator requirements: state buffers (`x`, `x_trial`), input
buffer `u`, output buffer `y`, two indicator buffers `z0`/`z1`, and the
integrator's stage scratch. Nothing is allocated inside the loop. The arena is
freed before return (including on error paths, after `terminate`).

---

## 6. Integrators

### 6.1 The ODE-system seam

Integrators operate on a flat closure, never on models:

```c
typedef void (*sim_rhs_fn)(void *ctx, sim_time t, const sim_real *x, sim_real *dxdt);
typedef struct sim_ode_system { sim_index n; sim_rhs_fn rhs; void *ctx; } sim_ode_system;
```

The driver constructs this by capturing the current input vector `u`: the
closure's `ctx` bundles `{model, u}` and its `rhs` calls
`model.vt->derivatives(model.ctx, t, x, u, dxdt)`. Because `u` is held fixed for
the duration of a step, evaluating `f` at RK trial stages is well-defined.
(Input *derivatives* within a step are not modeled; inputs are piecewise
constant over a step and updated at step boundaries. Choose `h ≤ output_dt` when
input smoothness matters.)

### 6.2 The step contract

```c
sim_status step(sim_integrator *self, const sim_ode_system *sys,
                sim_time t, sim_real h,
                const sim_real *x_in, sim_real *x_out,
                sim_real *err_out, sim_real *h_next);
```

- Advances `x_in` over `[t, t+h]` → `x_out` (must not alias `x_in`).
- **Fixed-step** methods ignore tolerances; set `*err_out = 0`, `*h_next = h`.
- **Adaptive** methods write to `*err_out` the tolerance-normalized RMS local
  error estimate (see §6.4) and to `*h_next` the suggested next step. The driver
  accepts the step iff `*err_out ≤ 1`.
- Either out-pointer may be NULL (driver not requesting that information).

### 6.3 Built-in methods

| Factory                     | Method                     | Order | Adaptive | Best for |
|-----------------------------|----------------------------|:-----:|:--------:|----------|
| `sim_integrator_euler`      | Explicit (forward) Euler   | 1     | no       | teaching, very cheap, non-stiff |
| `sim_integrator_heun`       | Heun / explicit trapezoid  | 2     | no       | cheap improvement over Euler |
| `sim_integrator_rk4`        | Classic Runge–Kutta        | 4     | no       | smooth non-stiff, fixed grid (default) |
| `sim_integrator_rk45`       | Dormand–Prince 5(4)        | 5(4)  | **yes**  | general non-stiff with error control |
| `sim_integrator_beuler`     | Backward (implicit) Euler  | 1     | no       | stiff systems (Newton inner solve) |

The default recommendation is **RK4** for smooth non-stiff models on a fixed grid
and **RK45** when the dynamics have widely varying time scales but are non-stiff.
For stiff systems (fast decaying modes forcing tiny explicit steps), use
`beuler`, which is L-stable and unconditionally stable for linear decay.

### 6.4 Adaptive step-size control (RK45)

Dormand–Prince produces a 5th-order solution and an embedded 4th-order estimate
from the same 7 stage evaluations (FSAL: the last stage is reused as the first
of the next step, so effective cost is 6 evaluations/step). The local error per
component is scaled by a mixed absolute/relative tolerance:

```
sc_i  = atol + rtol · max(|x_in_i|, |x_out_i|)
err   = sqrt( mean_i ( (x5_i − x4_i) / sc_i )² )        # tolerance-normalized RMS
```

The step is accepted iff `err ≤ 1`. The next step is proposed with the standard
PI-free controller and safety factor:

```
h_next = h · clamp( SAFETY · err^(−1/(p+1)),  MIN_SCALE,  MAX_SCALE )
```

with `SAFETY = 0.9`, `MIN_SCALE = 0.2`, `MAX_SCALE = 5.0`, `p = 4` (embedded
order). Rejected steps shrink `h` and retry without advancing `t`. `h` is always
clamped to `[h_min, h_max]`; hitting `h_min` on a rejection returns
`SIM_ERR_STEP_TOO_SMALL`.

### 6.5 Implicit method (backward Euler)

`x_{n+1} = x_n + h·f(t_{n+1}, x_{n+1})` is solved for `x_{n+1}` by a damped
Newton iteration. The Jacobian `J = ∂f/∂x` is obtained by forward finite
differences (perturbation `√eps · max(|x_i|, 1)`) unless the model supplies an
analytic Jacobian via an extension hook (future: `jacobian` callback). The
iteration stops when `‖Δx‖ ≤ newton_tol` or after a bounded iteration count; a
non-convergence returns `SIM_ERR_EVENT_UNCONVERGED`-class failure via the step's
status. Backward Euler is only order 1; it is intended for stability on stiff
problems, not high accuracy.

### 6.6 Adding a custom integrator

Implement the vtable and expose a factory returning a `sim_integrator*` whose
first member is the vtable pointer. The struct may carry arbitrary scratch (stage
buffers sized to `n`). Custom integrators compose with the driver and events
identically to built-ins; multistep methods should honor `reset()` by discarding
accumulated history (the driver calls it after every state reset).

---

## 7. Events and discontinuities

### 7.1 Why events

Naively integrating across a discontinuity corrupts a solver's error estimate and
order: the Taylor expansions underlying RK methods assume smoothness over the
step. The engine therefore stops integration at each discontinuity, applies the
model's reset, and restarts — so every integrated segment is smooth.

### 7.2 State events

Declared via `n_z > 0` and the `event_indicators` callback. An event of type `i`
fires when `z_i` crosses zero. Indicators must be *continuous* in `(t, x, u)` so
that a sign change reliably brackets a root. (A boolean "is contact" flag is a
poor indicator; `z = height` is a good one.)

### 7.3 Reset semantics

At a localized event the driver calls `handle_event(t*, x*, u, fired)`. The model
inspects `fired[i]` and may overwrite `x*` to encode the discontinuity (e.g.
`v ← −e·v` for a bounce with restitution `e`). Returning `true` signals the state
changed, prompting `integrator.reset()` and a fresh start from `(t*, x*)`. The
event instant is reported to the observer as its own sample.

### 7.4 Zero-crossing localization

When `sign(z_i(t)) ≠ sign(z_i(t+h))` for some `i`, the driver has a bracket
`[t, t+h]`. It refines the crossing time by **Illinois-modified regula falsi** on
the interpolated indicator, re-integrating a trial step from the accepted `x` to
each candidate time (dense output from the integrator is used when available;
otherwise a fresh short step). Refinement stops when the bracket width
`≤ event_tol` or `event_max_iter` is reached (then `SIM_ERR_EVENT_UNCONVERGED`).
When several indicators cross in one step, the **earliest** crossing is handled
first; the loop then restarts and re-detects the rest.

### 7.5 Time events

A model or the experiment layer registers scheduled times (e.g. periodic
controller updates). The driver caps `h` so a step lands exactly on the next
scheduled time, then processes it like a state event with a synthetic indicator.
This gives sample-accurate discrete control interleaved with continuous dynamics.

---

## 8. Determinism and reproducibility

### 8.1 Deterministic execution

Given identical `(model, integrator, cfg, input_fn)`:

- The driver performs no wall-clock-dependent decisions and no unordered
  traversal. Step acceptance depends only on the numeric error estimate.
- Floating-point results are bit-identical on any platform using IEEE-754 double
  with the same rounding mode, provided the compiler does not reassociate or
  contract operations differently. Build with `-ffp-contract=off` (or the MSVC
  `/fp:precise` equivalent) and avoid `-ffast-math` to guarantee cross-platform
  bit-reproducibility. Within a single build, results are always reproducible.
- Parallel parameter sweeps (§10) are embarrassingly parallel and independent;
  each run's result is identical whether executed alone or alongside others.

### 8.2 Stochastic reproducibility

Randomness comes only from `sim_rng`, a **counter-based** generator (Philox-style
mixing of a 64-bit key and counter). A substream is derived by
`sim_rng_make(seed, stream_id)`. Because output is a stateless function of
`(key, ctr)`, a model can reconstruct the exact same random draw for a given
`(seed, stream_id, step_index)` regardless of evaluation order or thread. This is
what makes stochastic runs reproducible *and* safely parallelizable — unlike a
shared mutable PRNG whose sequence depends on interleaving.

Recommended pattern for an SDE model: derive one stream per noise source at
`init` from `cfg.seed`; index the counter by step number to produce the Wiener
increment `ΔW = √h · N(0,1)` deterministically.

---

## 9. Composition

Small models compose into a larger one through `sim_composer`
([`include/sim.h`](include/sim.h)). The result is an ordinary `sim_model` — the
driver and integrators are unaware composition happened.

### 9.1 Ports and connections

Each submodel exposes `n_u` input ports and `n_y` output ports (indexed
`0..n-1`). `sim_composer_connect(c, src, sp, dst, dp)` wires output port `sp` of
subsystem `src` to input port `dp` of subsystem `dst`. Unconnected inputs are
driven by the composite's exported inputs or default to zero. Exported inputs and
outputs (via `sim_composer_export_input/output`) become the composite model's own
`u`/`y`.

### 9.2 Flattened state

The composite's state vector is the concatenation of its submodels' states in
registration order: `x = [x⁰ | x¹ | … ]`. `n_x` of the composite is the sum. The
composite `derivatives` callback slices `x`, evaluates each submodel's inputs from
the connection graph, calls each submodel's `derivatives`, and reassembles `dxdt`.
There is no per-step allocation: the composer preallocates the port and slice
buffers at build time.

### 9.3 Execution order and algebraic loops

Within one instant, a submodel with `direct_feedthrough` needs its inputs
computed before its outputs. The composer builds a dependency graph over
feedthrough paths and **topologically sorts** submodels so that every
feedthrough input is available when needed. A cycle of feedthrough connections is
an **algebraic loop** — a system of implicit algebraic equations the ODE engine
does not solve — and `sim_composer_build` returns `SIM_ERR_ALGEBRAIC_LOOP`. The
usual fix is to insert a state (integrator/delay) with no feedthrough to break
the cycle, which is almost always physically correct. Purely
non-feedthrough connections (output = a state) never form loops and impose no
ordering constraint on `derivatives`.

### 9.4 Build

`sim_composer_build` validates the graph, performs the sort, allocates the flat
buffers, and fills an `out_model` whose `ctx`/`vt` are owned by the composer.
The composer must outlive any run of the built model; `sim_composer_free`
releases everything.

---

## 10. Experiment layer (parameter sweeps, ensembles)

The experiment layer is a thin convenience over `sim_run`; it introduces no new
core concepts.

- **Single run.** Configure a `sim_model` + `sim_integrator` + `sim_config`, call
  `sim_run`, collect results through an observer.
- **Parameter sweep.** Vary fields of the model's `ctx` (parameters) across a
  grid; run each combination. Each run is independent — the recommended pattern
  is one `sim_model`/`ctx`/`integrator` triple per worker thread (they hold
  mutable scratch and must not be shared across threads mid-run).
- **Monte-Carlo ensemble.** Fix parameters, vary `cfg.seed` (or the per-run
  `stream_id` offset); aggregate observer output across realizations.
- **Sensitivity.** Finite-difference the quantity of interest with respect to a
  parameter by running the model at `θ` and `θ+Δθ`.

Because runs are deterministic and side-effect-free, the layer parallelizes with
any thread pool; results do not depend on scheduling.

---

## 11. Worked examples

Each example is summarized here; runnable sources are under `examples/`. See
§11.6 for the shared output/observer pattern.

### 11.1 Exponential decay — the minimal model
*(`examples/exponential_decay.c`)*

`ẋ = −k·x`, `x(0) = x₀`. One state, closed-form solution `x(t) = x₀ e^{−kt}` for
validation. Demonstrates the smallest possible model and the fixed-step RK4 path.
Use it as the correctness canary: RK4 at `h = 0.01`, `k = 1` matches the analytic
solution to ~1e-9 over `[0, 10]`.

### 11.2 Lotka–Volterra predator–prey — coupled nonlinear system dynamics
*(`examples/lotka_volterra.c`)*

```
    ẋ = α·x − β·x·y          (prey)
    ẏ = δ·x·y − γ·y          (predator)
```

Two coupled states, no closed form, a conserved quantity
`V = δx − γ ln x + βy − α ln y` that the integrator should keep nearly constant.
Demonstrates: a multi-state system-dynamics model, and using the conserved
quantity to compare integrators (Euler drifts and spirals; RK4/RK45 stay on the
closed orbit). A classic stock-and-flow reading: two stocks (populations) with
flows proportional to encounter rate `x·y`.

### 11.3 SIR epidemic — stock-and-flow with conservation
*(inline below; also `examples/` as a variant)*

```
    Ṡ = −β·S·I / N
    İ =  β·S·I / N − γ·I
    Ṙ =  γ·I
```

Three stocks (Susceptible, Infected, Recovered) with `N = S+I+R` conserved. The
basic reproduction number is `R₀ = β/γ`. This is the archetypal system-dynamics
model: flows between compartments, an invariant (`Ṡ+İ+Ṙ = 0`) usable as a
correctness check, and a parameter (`R₀`) whose value qualitatively changes the
trajectory (epidemic vs. die-out). A natural sweep: vary `β` and observe peak
infected and final epidemic size.

```c
typedef struct { sim_real beta, gamma, N; sim_real S0, I0, R0; } sir_params;

static void sir_init(void *c, sim_time t0, sim_real *x0) {
    sir_params *p = c; (void)t0;
    x0[0] = p->S0; x0[1] = p->I0; x0[2] = p->R0;
}
static void sir_deriv(void *c, sim_time t, const sim_real *x,
                      const sim_real *u, sim_real *dx) {
    sir_params *p = c; (void)t; (void)u;
    sim_real S = x[0], I = x[1];
    sim_real infect = p->beta * S * I / p->N;
    sim_real recover = p->gamma * I;
    dx[0] = -infect;
    dx[1] =  infect - recover;
    dx[2] =  recover;
}
static const sim_model_vtable SIR_VT = { sir_init, sir_deriv, 0, 0, 0, 0 };
```

### 11.4 Bouncing ball — state events and resets
*(`examples/bouncing_ball.c`)*

```
    ḣ = v,   v̇ = −g          with event  z = h  (ground contact)
    on event:  v ← −e·v,  h ← 0        (restitution e ∈ (0,1))
```

Two states `[h, v]`, one event indicator `z = h`. Demonstrates the full hybrid
cycle: the indicator brackets ground contact, `handle_event` reflects and damps
the velocity, and the solver restarts. Correct localization is visible as clean
bounces whose peak heights decay geometrically by `e²`; without event handling,
fixed-step integration lets the ball sink through the floor. A good stress test
for `event_tol` and for the earliest-crossing rule as bounces bunch up (Zeno
behavior — cap total events or stop when `|v|` at contact falls below a threshold).

### 11.5 Mass–spring–damper + PID — composition and control
*(inline sketch; composition via `sim_composer`)*

A plant `m·q̈ + c·q̇ + k·q = F` (state `[q, q̇]`, output `q`, **no** feedthrough)
in closed loop with a PID controller (input = error `r − q`, output = force `F`,
**with** feedthrough through the proportional term). Wiring:

```
   reference r ──► (+) ──► [ PID ] ──F──► [ Plant ] ──q──┬──► output
                    ▲                                     │
                    └─────────────── −q ◄─────────────────┘
```

The PID's integral term is itself a state, so the closed loop has three states
total. The proportional path is direct feedthrough (controller output depends on
current error which depends on plant output), but the plant has no feedthrough
(its output `q` is a state, independent of `F` in the same instant) — so the loop
is **not** algebraic and the composer sorts it as `Plant.outputs → error →
PID.outputs → derivatives`. This is the canonical demonstration of §9.3: physical
plants break algebraic loops precisely because their measured outputs are states.

### 11.6 Shared observer pattern

Every example logs through a `sim_observer` that writes `t` and selected states
to CSV (or accumulates into arrays for a sweep). Because the driver caps `h` to
land on `output_dt` boundaries, the CSV has a uniform time grid regardless of the
integrator's internal stepping:

```c
static sim_status csv_observer(void *ctx, sim_time t,
                               const sim_real *x, const sim_real *y) {
    FILE *f = ctx; (void)y;
    fprintf(f, "%.9g,%.9g\n", t, x[0]);
    return SIM_OK;
}
```

---

## 12. Error handling and diagnostics

- Every driver-visible failure is a `sim_status`; `sim_run` returns it in
  `sim_result.status` alongside step/event/evaluation counters for diagnosis.
- `sim_status_str` maps codes to human-readable strings.
- The non-finite guard (§5.1) converts silent NaN propagation into an explicit
  `SIM_ERR_NONFINITE`, reported at the first step that produced it — invaluable
  for catching a divide-by-zero in `derivatives`.
- `SIM_STOP_REQUESTED` from an observer or `handle_event` is a *clean* early
  termination, not an error: `sim_result` still reports the state reached.
- Counters (`steps_accepted`, `steps_rejected`, `rhs_evals`) expose solver
  behavior: a high rejection ratio signals tolerances too tight or unhandled
  stiffness; a rejection ratio near zero with visible error signals tolerances
  too loose.

---

## 13. C++ layer (optional)

A header-only C++ wrapper provides RAII and templated model authoring without
changing the ABI:

- `sim::Model` — a CRTP base so a model is a class with `derivatives(...)`,
  `outputs(...)` methods; the wrapper generates the vtable and forwards `this`
  as `ctx`. Eliminates the boilerplate casts seen in §4.3.
- `sim::Integrator` — an RAII owner of `sim_integrator*` (calls `destroy` in its
  destructor).
- `sim::run(model, integrator, config, observer)` — accepts a
  `std::function`/lambda observer.

The wrapper is strictly additive: it emits the same C structs and calls
`sim_run`, so C++-authored models and C-authored models interoperate and compose.

---

## 14. Performance and threading

- **Hot path.** No allocation, no locking, no I/O inside the step. Cost per
  accepted step is `(stages × n)` flops for the RK combination plus `stages`
  model `derivatives` calls; the model dominates for nontrivial `f`.
- **Memory layout.** State, stage, and scratch vectors are contiguous
  `sim_real[n]` arrays for cache efficiency and straightforward SIMD by the
  compiler; the engine imposes no AoS/SoA choice on the model's own data.
- **Threading model.** A `sim_model` (with its mutable `ctx` scratch) and a
  `sim_integrator` (with stage buffers) are **not** thread-safe for concurrent
  use within one run. Parallelism is *across* runs: give each worker its own
  model/integrator instances (§10). The core takes no locks.
- **Real-time use.** Fixed-step integrators (`euler`, `heun`, `rk4`, `beuler`)
  have deterministic per-step cost and no allocation after `sim_run` entry,
  suitable for soft-real-time loops; adaptive methods have variable per-step cost
  and are unsuitable for hard-real-time deadlines.

---

## 15. Conformance and validation

An implementation conforms to this specification if:

1. It exposes the ABI in [`include/sim.h`](include/sim.h) with the documented
   semantics for every callback and driver behavior.
2. **Order verification.** For a smooth linear test problem, each fixed-step
   integrator exhibits its stated order `p`: halving `h` reduces global error by
   ~`2^p` (Euler ≈2×, Heun ≈4×, RK4 ≈16×). RK45 holds global error near the
   requested tolerance across a range of `rtol`.
3. **Conservation.** For Lotka–Volterra (§11.2) and SIR (§11.3), the respective
   invariants are preserved to within the integrator's expected error over the
   run.
4. **Event correctness.** For the bouncing ball (§11.4), successive peak heights
   decay by `e²` and contact times match the analytic sequence to within
   `event_tol`.
5. **Determinism.** Repeated runs are bit-identical; a stochastic run reproduces
   exactly from its seed; a parallel sweep matches a serial sweep element-for-
   element.

A conformance test suite exercising 1–5 is the recommended acceptance gate for
any change to the numeric core.

---

## 16. Glossary

| Term | Meaning |
|------|---------|
| **State** `x` | Continuous variables integrated over time (stocks). |
| **Derivative** `f` | Rate of change of state; the model's core dynamics (flows). |
| **Output** `y` | Algebraic function of state/inputs, for observation or wiring. |
| **Input** `u` | External signal into a model (from a source or a connection). |
| **Event indicator** `z` | Continuous function whose zero-crossing triggers an event. |
| **Reset** | Discontinuous change to state applied at an event. |
| **Direct feedthrough** | Output depends on input in the same instant. |
| **Algebraic loop** | A cycle of feedthrough paths with no intervening state. |
| **Stiff** | Dynamics with fast-decaying modes that force tiny explicit steps. |
| **Substream** | An independent, seed-derived RNG sequence for reproducibility. |
| **FSAL** | "First Same As Last": reuse of the final RK stage as the next first. |

---

## 17. Open questions / future work

- **Analytic Jacobian hook** for `beuler` and future implicit multistep (BDF)
  methods, to replace finite-difference Jacobians on stiff, large-`n` systems.
- **Dense output** (continuous interpolant) as a first-class part of the
  integrator vtable, improving event localization and off-grid sampling.
- **DAE support** (index-1) for models with genuine algebraic constraints, which
  currently must be reformulated as ODEs.
- **Unit / dimension metadata** on ports for connect-time checking.
- **Checkpointing** the driver arena for save/restore and rollback beyond the
  single-step event restart.
