/*
 * sim.h — Reference C API for the continuous / system-dynamics simulation engine.
 *
 * This header is normative for the public C ABI described in SPECIFICATION.md.
 * It is designed to be:
 *   - C89/C99-friendly (usable from C and C++),
 *   - stable across minor versions (append-only struct growth via reserved fields),
 *   - free of dynamic dispatch in the numeric hot path except through the
 *     small, cache-resident vtables defined here.
 *
 * All public symbols are prefixed `sim_` / `SIM_`. Types ending in `_t` are
 * value types; opaque handles are typedef'd to forward-declared structs.
 */
#ifndef SIM_H
#define SIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Versioning                                                          */
/* ------------------------------------------------------------------ */
#define SIM_VERSION_MAJOR 0
#define SIM_VERSION_MINOR 1
#define SIM_VERSION_PATCH 0

/* ------------------------------------------------------------------ */
/* Scalar configuration                                               */
/* ------------------------------------------------------------------ */
/* The real type is fixed at build time. Double is the default and the
 * only ABI-stable choice; a float build is a distinct, incompatible ABI. */
#ifndef SIM_REAL_FLOAT
typedef double   sim_real;
#define SIM_REAL_EPS 2.220446049250313e-16
#else
typedef float    sim_real;
#define SIM_REAL_EPS 1.1920929e-07f
#endif

typedef double   sim_time;    /* Simulation time is always double precision. */
typedef size_t   sim_index;   /* Vector index / dimension count.             */

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */
typedef enum sim_status {
    SIM_OK = 0,
    SIM_STOP_REQUESTED,       /* A model asked to terminate early (not an error). */
    SIM_ERR_INVALID_ARG,
    SIM_ERR_ALLOC,
    SIM_ERR_STEP_TOO_SMALL,   /* Adaptive step underflowed h_min.               */
    SIM_ERR_NONFINITE,        /* NaN/Inf detected in a state or derivative.     */
    SIM_ERR_EVENT_UNCONVERGED,/* Zero-crossing localization did not converge.   */
    SIM_ERR_ALGEBRAIC_LOOP,   /* A direct-feedthrough cycle could not be sorted.*/
    SIM_ERR_NOT_SUPPORTED
} sim_status;

const char *sim_status_str(sim_status s);

/* ------------------------------------------------------------------ */
/* Model interface                                                     */
/* ------------------------------------------------------------------ */
/*
 * A model is a first-order ODE system with algebraic outputs and, optionally,
 * event (zero-crossing) indicators:
 *
 *     dx/dt = f(t, x, u)            (derivatives)
 *     y     = g(t, x, u)            (outputs)
 *     z     = h(t, x, u)            (event indicators; events fire at sign changes)
 *
 * `ctx` is an opaque per-instance pointer owned by the model author. The engine
 * never dereferences it. All array arguments are owned by the engine; callbacks
 * must not retain pointers to them beyond the call.
 */
typedef struct sim_model_vtable {
    /* Fill x0[0..n_x) with initial state; may also latch t0 into ctx.
     * Optional (may be NULL -> state initialized to zero). */
    void (*init)(void *ctx, sim_time t0, sim_real *x0);

    /* REQUIRED. Write dxdt[0..n_x). Must be a pure function of its inputs
     * (no hidden state) so integrators may call it at trial points. */
    void (*derivatives)(void *ctx, sim_time t,
                        const sim_real *x, const sim_real *u, sim_real *dxdt);

    /* Write y[0..n_y). Optional if n_y == 0. If the model has no direct
     * feedthrough (y does not depend on u), set `direct_feedthrough` false
     * so the composer can break algebraic loops. */
    void (*outputs)(void *ctx, sim_time t,
                    const sim_real *x, const sim_real *u, sim_real *y);

    /* Write z[0..n_z). Optional if n_z == 0. Continuous in (t,x,u). */
    void (*event_indicators)(void *ctx, sim_time t,
                             const sim_real *x, const sim_real *u, sim_real *z);

    /* Called at a localized event. `fired[i] != 0` marks which indicators
     * crossed zero. May overwrite x to model a state discontinuity (a "reset").
     * Return true if any state was modified (forces integrator restart).
     * Optional if n_z == 0. */
    bool (*handle_event)(void *ctx, sim_time t, sim_real *x,
                         const sim_real *u, const int *fired);

    /* Called once after the run (or on error). Optional. */
    void (*terminate)(void *ctx);
} sim_model_vtable;

typedef struct sim_model {
    const char             *name;
    sim_index               n_x;   /* continuous states */
    sim_index               n_u;   /* inputs            */
    sim_index               n_y;   /* outputs           */
    sim_index               n_z;   /* event indicators  */
    bool                    direct_feedthrough; /* does y depend on u? */
    void                   *ctx;
    const sim_model_vtable *vt;
} sim_model;

/* ------------------------------------------------------------------ */
/* ODE system abstraction consumed by integrators                      */
/* ------------------------------------------------------------------ */
/* The driver wraps a model (with its current inputs bound) into this flat
 * closure so integrators stay model-agnostic. */
typedef void (*sim_rhs_fn)(void *ctx, sim_time t,
                           const sim_real *x, sim_real *dxdt);

typedef struct sim_ode_system {
    sim_index  n;
    sim_rhs_fn rhs;
    void      *ctx;
} sim_ode_system;

/* ------------------------------------------------------------------ */
/* Integrator interface                                                */
/* ------------------------------------------------------------------ */
typedef struct sim_integrator sim_integrator; /* opaque */

typedef struct sim_integrator_vtable {
    /* Advance x_in over one step of size h -> x_out.
     * For adaptive methods, *err_out receives the RMS local error estimate
     * (normalized to 1.0 == at tolerance) and *h_next a suggested next step.
     * Either out-pointer may be NULL. Returns SIM_OK on a successful step. */
    sim_status (*step)(sim_integrator *self, const sim_ode_system *sys,
                       sim_time t, sim_real h,
                       const sim_real *x_in, sim_real *x_out,
                       sim_real *err_out, sim_real *h_next);

    sim_index  (*order)(const sim_integrator *self);      /* method order p */
    bool       (*is_adaptive)(const sim_integrator *self);
    void       (*reset)(sim_integrator *self);            /* clear internal history */
    void       (*destroy)(sim_integrator *self);
} sim_integrator_vtable;

struct sim_integrator {
    const sim_integrator_vtable *vt;
    /* subtype-specific fields follow in the concrete allocation */
};

/* Factory functions for the built-in integrators. `rtol`/`atol` are ignored
 * by fixed-step methods. Each allocates scratch sized for `n` states. */
sim_integrator *sim_integrator_euler(sim_index n);        /* explicit Euler, p=1 */
sim_integrator *sim_integrator_heun(sim_index n);         /* Heun / RK2, p=2     */
sim_integrator *sim_integrator_rk4(sim_index n);          /* classic RK4, p=4    */
sim_integrator *sim_integrator_rk45(sim_index n,          /* Dormand-Prince 5(4) */
                                    sim_real rtol, sim_real atol);
sim_integrator *sim_integrator_beuler(sim_index n,        /* backward Euler,     */
                                      sim_real newton_tol);/* implicit / stiff   */

/* ------------------------------------------------------------------ */
/* Run configuration                                                   */
/* ------------------------------------------------------------------ */
typedef struct sim_config {
    sim_time t_start;
    sim_time t_stop;

    sim_real h;            /* fixed step, or initial step for adaptive methods  */
    sim_real h_min;        /* adaptive: floor before SIM_ERR_STEP_TOO_SMALL     */
    sim_real h_max;        /* adaptive: ceiling                                 */
    sim_real rtol, atol;   /* adaptive tolerances (also passed to factory)      */

    sim_real output_dt;    /* logging sample interval; 0 => every accepted step */
    bool     detect_events;
    sim_real event_tol;    /* time tolerance for zero-crossing localization     */
    sim_index event_max_iter;

    uint64_t seed;         /* master RNG seed for stochastic models             */

    uint32_t _reserved[8]; /* zero-initialize; reserved for ABI growth          */
} sim_config;

/* Fills cfg with documented defaults over [t_start, t_stop]. */
void sim_config_defaults(sim_config *cfg, sim_time t_start, sim_time t_stop);

/* ------------------------------------------------------------------ */
/* Observer (logging / streaming) interface                            */
/* ------------------------------------------------------------------ */
/* Invoked at t_start, at each output sample, at localized events, and at
 * t_stop. Return SIM_STOP_REQUESTED to halt the run cleanly. */
typedef sim_status (*sim_observer_fn)(void *ctx, sim_time t,
                                      const sim_real *x, const sim_real *y);

typedef struct sim_observer {
    sim_observer_fn fn;
    void           *ctx;
} sim_observer;

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */
typedef struct sim_result {
    sim_status status;
    sim_time   t_final;
    uint64_t   steps_accepted;
    uint64_t   steps_rejected;
    uint64_t   events_handled;
    uint64_t   rhs_evals;
} sim_result;

/* Runs `model` under `cfg` using `integ`, driving inputs (if any) via
 * `input_fn` (may be NULL when n_u == 0), reporting to `obs` (may be NULL).
 * The engine owns all working memory for the duration of the call. */
sim_result sim_run(const sim_model     *model,
                   sim_integrator      *integ,
                   const sim_config    *cfg,
                   void (*input_fn)(void *ctx, sim_time t, sim_real *u),
                   void                *input_ctx,
                   const sim_observer  *obs);

/* ------------------------------------------------------------------ */
/* Deterministic RNG (counter-based; reproducible & parallel-safe)     */
/* ------------------------------------------------------------------ */
typedef struct sim_rng {
    uint64_t key;
    uint64_t ctr;
} sim_rng;

/* Derives an independent substream from (seed, stream_id). */
sim_rng  sim_rng_make(uint64_t seed, uint64_t stream_id);
uint64_t sim_rng_next_u64(sim_rng *r);
sim_real sim_rng_uniform(sim_rng *r);   /* [0,1)              */
sim_real sim_rng_normal(sim_rng *r);    /* N(0,1), Box-Muller */

/* ------------------------------------------------------------------ */
/* Composition: build a composite model from subsystems + connections  */
/* ------------------------------------------------------------------ */
typedef struct sim_composer sim_composer; /* opaque */

sim_composer *sim_composer_new(void);
/* Returns a subsystem handle (index) for later connection. */
sim_index     sim_composer_add(sim_composer *c, const sim_model *sub);
/* Connect src subsystem output port -> dst subsystem input port. */
sim_status    sim_composer_connect(sim_composer *c,
                                   sim_index src, sim_index src_port,
                                   sim_index dst, sim_index dst_port);
/* Expose a subsystem input/output as a port of the composite. */
sim_status    sim_composer_export_input (sim_composer *c, sim_index sub, sim_index port);
sim_status    sim_composer_export_output(sim_composer *c, sim_index sub, sim_index port);
/* Finalize: topologically sorts feedthrough, detects algebraic loops, and
 * produces a flat sim_model whose ctx/vt are owned by the composer. */
sim_status    sim_composer_build(sim_composer *c, sim_model *out_model);
void          sim_composer_free(sim_composer *c);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SIM_H */
