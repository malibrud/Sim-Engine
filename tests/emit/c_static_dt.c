/* The drivetrain, through its C interface built with SE_C_PREFIX dt_.
   See c_static.c. Every se_ name below is dt_se_... after the preprocessor. */
#define SE_C_PREFIX dt_
#include "se_sim.h"

#include <stdio.h>
#include <string.h>

int run_drivetrain(void) {
    int failures = 0;
    se_sim* s = se_create();
    const double in[2] = {300.0, 5.0};
    if (strcmp(se_model_name(), "Drivetrain") != 0) {
        fprintf(stderr, "c_static: FAIL dt_se_model_name is %s\n", se_model_name());
        ++failures;
    }
    se_record_clear(s);
    se_set(s, "sim.duration", 0.1);
    if (!se_init(s)) {
        fprintf(stderr, "c_static: FAIL the drivetrain did not start: %s\n", se_error(s));
        ++failures;
    }
    while (!se_done(s)) {
        se_set_inputs(s, in, 2);
        se_tick(s);
    }
    if (se_tick_index(s) != 100 || se_finish(s) != 0) {
        fprintf(stderr, "c_static: FAIL the drivetrain's run\n");
        ++failures;
    }
    se_destroy(s);
    return failures;
}
