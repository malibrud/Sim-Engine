/* Decay, through its C interface built with SE_C_PREFIX dc_. See c_static.c.
   Every se_ name below is dc_se_... after the preprocessor. Decay's root is a
   leaf with no inputs and two outputs, so its value can be checked against
   the closed form, x0 * exp(-k * t). */
#define SE_C_PREFIX dc_
#include "se_sim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

int run_decay(void) {
    int failures = 0;
    se_sim* s = se_create();
    double value = 0.0, t = 0.0;
    if (strcmp(se_model_name(), "Decay") != 0 || se_input_count() != 0 ||
        se_output_index("value") != 0) {
        fprintf(stderr, "c_static: FAIL Decay's model and ports\n");
        ++failures;
    }
    se_record_clear(s);
    se_log_capture(s, 1);
    se_set(s, "sim.duration", 1.0);
    if (!se_init(s)) {
        fprintf(stderr, "c_static: FAIL Decay did not start: %s\n", se_error(s));
        ++failures;
    }
    while (!se_done(s)) {
        t = se_time(s);
        se_tick(s);
        value = se_get_output(s, 0);
    }
    /* k = 2 /s from Decay.sim; the last tick sampled t = 1 s. RK4 at 1 ms is
       far inside this tolerance. */
    if (t != 1.0 || fabs(value - exp(-2.0)) > 1e-9) {
        fprintf(stderr, "c_static: FAIL Decay at t = %g: %.12g, want %.12g\n", t, value, exp(-2.0));
        ++failures;
    }
    if (se_finish(s) != 0) {
        fprintf(stderr, "c_static: FAIL Decay's finish\n");
        ++failures;
    }
    se_destroy(s);
    return failures;
}
