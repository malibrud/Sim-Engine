/*
 * exponential_decay.c — the minimal sim-engine model.
 *
 *     dx/dt = -k * x,   x(0) = x0
 *
 * Closed form x(t) = x0 * exp(-k t) makes this the correctness canary:
 * with RK4 at h = 0.01, k = 1 the numeric solution matches analytic to ~1e-9.
 *
 * Build (against a conforming implementation of include/sim.h):
 *     cc -I../include exponential_decay.c -lsim -lm -o decay
 */
#include "sim.h"
#include <stdio.h>
#include <math.h>

typedef struct { sim_real k; sim_real x0; } decay_params;

static void decay_init(void *c, sim_time t0, sim_real *x0) {
    (void)t0;
    x0[0] = ((decay_params *)c)->x0;
}

static void decay_deriv(void *c, sim_time t, const sim_real *x,
                        const sim_real *u, sim_real *dx) {
    (void)t; (void)u;
    dx[0] = -((decay_params *)c)->k * x[0];
}

static const sim_model_vtable DECAY_VT = {
    decay_init, decay_deriv, NULL, NULL, NULL, NULL
};

/* Observer: print time, numeric x, analytic x, and absolute error. */
static sim_status report(void *ctx, sim_time t,
                         const sim_real *x, const sim_real *y) {
    decay_params *p = ctx; (void)y;
    double exact = p->x0 * exp(-p->k * t);
    printf("%8.3f  %14.9f  %14.9f  %.3e\n", t, x[0], exact, fabs(x[0] - exact));
    return SIM_OK;
}

int main(void) {
    decay_params p = { .k = 1.0, .x0 = 1.0 };
    sim_model m = { 0 };
    m.name = "exponential_decay";
    m.n_x = 1;
    m.ctx = &p;
    m.vt = &DECAY_VT;

    sim_config cfg;
    sim_config_defaults(&cfg, 0.0, 10.0);
    cfg.h = 0.01;
    cfg.output_dt = 1.0;          /* log once per second on a uniform grid */

    sim_integrator *integ = sim_integrator_rk4(m.n_x);
    sim_observer obs = { report, &p };

    printf("    t             x(num)         x(exact)      err\n");
    sim_result r = sim_run(&m, integ, &cfg, NULL, NULL, &obs);

    printf("status=%s  steps=%llu  rhs_evals=%llu\n",
           sim_status_str(r.status),
           (unsigned long long)r.steps_accepted,
           (unsigned long long)r.rhs_evals);

    integ->vt->destroy(integ);
    return r.status == SIM_OK ? 0 : 1;
}
