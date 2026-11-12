/* ----------------------------------------------------------------------------
 *  Two models' C interfaces in one program (SPECIFICATION.md section 14.2).
 *
 *  Every model exports the same names, so two of them can share a program
 *  only if each is built with its own SE_C_PREFIX. Here the drivetrain's
 *  static library was built with `build.ps1 lib -CPrefix dt_`, and Decay's
 *  with `-CPrefix dc_`. c_static_dt.c and c_static_dc.c each include se_sim.h
 *  under one of those prefixes and run their model through it; this file
 *  calls both. If either prefix failed to rename a function, the link would
 *  fail on a duplicate se_ symbol, or on a missing one.
 *
 *  Compiled as C (the .c extension, not /TC, which would take the .libs for
 *  source too) and run by tests/run.ps1. Exit status 0 means every
 *  check passed.
 * ------------------------------------------------------------------------- */
#include <stdio.h>

int run_drivetrain(void);   /* c_static_dt.c: failures */
int run_decay(void);        /* c_static_dc.c: failures */

int main(void) {
    const int failures = run_drivetrain() + run_decay();
    if (failures == 0) printf("c_static: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
