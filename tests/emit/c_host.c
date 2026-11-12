/* ----------------------------------------------------------------------------
 *  The C interface (SPECIFICATION.md section 14.2), from C, against the
 *  drivetrain built as a DLL.
 *
 *  This file is C, not C++, and it does not link the import library: it finds
 *  every function by name with GetProcAddress, which is what C#, Python and
 *  LabVIEW do underneath. What it checks:
 *
 *    1. Every function se_sim.h declares is exported under its own name.
 *    2. The model and its ports, without an instance.
 *    3. The setting and channel lists, a row at a time; get and set in both
 *       forms, with a unit converted and a wrong one refused.
 *    4. The C layer's own refusals (a bad index, a wrong count) and the
 *       model's, through one se_error.
 *    5. A whole run through se_set_inputs / se_tick / se_get_outputs, with a
 *       channel handle reading what se_get_outputs reads. It writes c_host.csv,
 *       which tests/run.ps1 compares with the C++ exe's table of the same run.
 *    6. The log queue holding the console's closing line.
 *
 *  Built with `cl /TC` and run by tests/run.ps1. Exit status 0 means every
 *  check passed.
 * ------------------------------------------------------------------------- */
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "se_sim.h"

/* Every function, with its type. A pointer per function, found by name. */
#define SE_FUNCTIONS(X)                                                        \
    X(int, se_abi_version, (void))                                             \
    X(const char*, se_model_name, (void))                                      \
    X(double, se_step_seconds, (void))                                         \
    X(int, se_input_count, (void))                                             \
    X(const char*, se_input_name, (int))                                       \
    X(const char*, se_input_unit, (int))                                       \
    X(int, se_input_index, (const char*))                                      \
    X(int, se_output_count, (void))                                            \
    X(const char*, se_output_name, (int))                                      \
    X(const char*, se_output_unit, (int))                                      \
    X(int, se_output_index, (const char*))                                     \
    X(se_sim*, se_create, (void))                                              \
    X(void, se_destroy, (se_sim*))                                             \
    X(int, se_load_settings, (se_sim*, const char*, const char*))              \
    X(const char*, se_dump_settings, (se_sim*))                                \
    X(const char*, se_list_signals, (se_sim*))                                 \
    X(int, se_setting_count, (se_sim*))                                        \
    X(const char*, se_setting_path, (se_sim*, int))                            \
    X(int, se_setting_kind, (se_sim*, int))                                    \
    X(const char*, se_setting_unit, (se_sim*, int))                            \
    X(const char*, se_setting_type, (se_sim*, int))                            \
    X(int, se_setting_structural, (se_sim*, int))                              \
    X(double, se_get, (se_sim*, const char*))                                  \
    X(int, se_set, (se_sim*, const char*, double))                             \
    X(int, se_set_text, (se_sim*, const char*, const char*))                   \
    X(int, se_channel_count, (se_sim*))                                        \
    X(const char*, se_channel_path, (se_sim*, int))                            \
    X(const char*, se_channel_unit, (se_sim*, int))                            \
    X(int, se_channel_selected, (se_sim*, int))                                \
    X(int, se_record, (se_sim*, const char*))                                  \
    X(int, se_unrecord, (se_sim*, const char*))                                \
    X(void, se_record_clear, (se_sim*))                                        \
    X(int, se_record_file, (se_sim*, const char*))                             \
    X(const char*, se_get_record_file, (se_sim*))                              \
    X(int, se_record_every, (se_sim*, uint64_t))                               \
    X(uint64_t, se_get_record_every, (se_sim*))                                \
    X(int, se_log_file, (se_sim*, const char*))                                \
    X(const char*, se_get_log_file, (se_sim*))                                 \
    X(int, se_log_level, (se_sim*, const char*))                               \
    X(int, se_log_path_level, (se_sim*, const char*, const char*))             \
    X(int, se_init, (se_sim*))                                                 \
    X(const char*, se_error, (se_sim*))                                        \
    X(int, se_set_input, (se_sim*, int, double))                               \
    X(int, se_set_inputs, (se_sim*, const double*, int))                       \
    X(void, se_tick, (se_sim*))                                                \
    X(double, se_get_output, (se_sim*, int))                                   \
    X(int, se_get_outputs, (se_sim*, double*, int))                            \
    X(int, se_done, (se_sim*))                                                 \
    X(double, se_time, (se_sim*))                                              \
    X(uint64_t, se_tick_index, (se_sim*))                                      \
    X(uint64_t, se_tick_count, (se_sim*))                                      \
    X(int, se_channel_handle, (se_sim*, const char*))                          \
    X(double, se_read, (se_sim*, int))                                         \
    X(int, se_read_many, (se_sim*, const int*, int, double*))                  \
    X(void, se_sync_realtime, (se_sim*))                                       \
    X(double, se_realtime_utilization, (se_sim*))                              \
    X(double, se_realtime_overrun_pct, (se_sim*))                              \
    X(double, se_realtime_lag, (se_sim*))                                      \
    X(uint64_t, se_realtime_frames, (se_sim*))                                 \
    X(uint64_t, se_realtime_overruns, (se_sim*))                               \
    X(void, se_log_capture, (se_sim*, int))                                    \
    X(const char*, se_log_next, (se_sim*))                                     \
    X(int, se_log_line_level, (se_sim*))                                       \
    X(const char*, se_log_line_path, (se_sim*))                                \
    X(double, se_log_line_time, (se_sim*))                                     \
    X(int, se_finish, (se_sim*))

#define DECLARE(ret, name, args) static ret(*p_##name) args;
SE_FUNCTIONS(DECLARE)

static int failures = 0;

static void check(int ok, const char* what) {
    if (ok) return;
    fprintf(stderr, "c_host: FAIL %s\n", what);
    ++failures;
}

static int same(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }
static int has(const char* text, const char* part) { return text && strstr(text, part) != NULL; }

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "build\\release\\bin\\Drivetrain.dll";
    HMODULE dll = LoadLibraryA(path);
    if (!dll) {
        fprintf(stderr, "c_host: cannot load %s\n", path);
        return 1;
    }

    /* ---- 1. every function, by name ------------------------------------ */
    int missing = 0;
#define LOAD(ret, name, args)                                                  \
    p_##name = (ret(*) args)GetProcAddress(dll, #name);                        \
    if (!p_##name) {                                                           \
        fprintf(stderr, "c_host: FAIL %s is not exported\n", #name);           \
        ++missing;                                                             \
    }
    SE_FUNCTIONS(LOAD)
    if (missing) return 1;

    /* ---- 2. the model, without an instance ----------------------------- */
    check(p_se_abi_version() == SE_ABI_VERSION, "built against this header's ABI");
    check(same(p_se_model_name(), "Drivetrain"), "the model's name");
    check(p_se_step_seconds() == 0.001, "the step");
    check(p_se_input_count() == 2 && same(p_se_input_name(0), "axle_torque") &&
              same(p_se_input_unit(0), "N*m") && same(p_se_input_name(1), "ground_speed"),
          "the inputs, in declaration order, with units");
    check(p_se_output_count() == 4 && same(p_se_output_name(0), "ws.speed") &&
              same(p_se_output_name(2), "ws.slip") && same(p_se_output_unit(2), "-") &&
              same(p_se_output_name(3), "torque_cut") && same(p_se_output_unit(3), "N*m"),
          "the outputs, a record flattened to its fields");
    check(p_se_input_index("ground_speed") == 1 && p_se_output_index("ws.slip") == 2,
          "a port by name");
    check(p_se_input_index("nope") == -1 && p_se_output_index(NULL) == -1, "no such port");
    check(same(p_se_input_name(9), "") && same(p_se_output_unit(-1), ""),
          "an index past the ports names nothing");

    se_sim* s = p_se_create();
    check(s != NULL, "an instance");
    check(isnan(p_se_get_output(s, 99)), "an output index past the ports reads NaN");

    /* ---- 3. the lists, get and set ------------------------------------- */
    {
        int n = p_se_setting_count(s), radius = -1, i;
        for (i = 0; i < n; ++i)
            if (same(p_se_setting_path(s, i), "whl.radius")) radius = i;
        check(n > 0 && radius >= 0, "the settings list names whl.radius");
        check(radius >= 0 && p_se_setting_kind(s, radius) == 0 &&
                  same(p_se_setting_unit(s, radius), "m") &&
                  same(p_se_setting_type(s, radius), "double") &&
                  p_se_setting_structural(s, radius) == 0,
              "and describes it");
        check(same(p_se_setting_path(s, n - 1), "sim.duration") && p_se_setting_kind(s, n - 1) == 2,
              "the run's length comes last");
        check(same(p_se_setting_path(s, n), "") && p_se_setting_kind(s, n) == -1,
              "a row past the list is empty");
    }
    check(p_se_set_text(s, "whl.radius", "31.5 (cm)") && fabs(p_se_get(s, "whl.radius") - 0.315) < 1e-15,
          "set_text converts the unit");
    check(!p_se_set_text(s, "whl.radius", "3 (kg)"), "a wrong unit is refused");
    check(has(p_se_error(s), "whl.radius: incompatible units"), "and se_error says why");
    check(p_se_set(s, "whl.radius", 0.315) && p_se_get(s, "whl.radius") == 0.315,
          "set takes the canonical unit");
    check(same(p_se_error(s), ""), "a call that succeeds clears the error");
    check(isnan(p_se_get(s, "whl.nope")), "get of a path the model lacks is NaN");
    {
        int n = p_se_channel_count(s), omega = -1, i;
        for (i = 0; i < n; ++i)
            if (same(p_se_channel_path(s, i), "whl.state.omega")) omega = i;
        check(omega >= 0 && p_se_channel_selected(s, omega) && same(p_se_channel_unit(s, omega), "rad/s"),
              "the channel list, with the .sim's selection");
    }
    check(has(p_se_dump_settings(s), "whl.radius    = 0.315 (m);"), "the dump");
    check(has(p_se_list_signals(s), "record {"), "the menu");

    /* ---- 4. refusals --------------------------------------------------- */
    {
        double two[2] = {0.0, 0.0};
        check(!p_se_set_input(s, 7, 1.0), "an input index past the ports is refused");
        check(has(p_se_error(s), "se_set_input: 7 is not an input; there are 2"), "and says so");
        check(!p_se_set_inputs(s, two, 3), "a count that is not the ports' is refused");
        check(has(p_se_error(s), "se_set_inputs: 3 values for 2 inputs"), "and says so");
        check(!p_se_record_file(s, "c_{model}.csv"), "a file name with a stray placeholder");
        check(has(p_se_error(s), "`{model}` is not a placeholder"), "is refused by the model");
        check(!p_se_log_level(s, "loud"), "a log level that is not one");
        check(p_se_channel_handle(s, "whl.nope") == -1, "a channel that is not one gets no handle");
        check(has(p_se_error(s), "whl.nope: not a recordable signal"), "and se_error says why");
    }

    /* ---- 5. a whole run ------------------------------------------------ */
    check(p_se_record_file(s, "c_host.csv") && same(p_se_get_record_file(s), "c_host.csv"),
          "the table's file");
    check(p_se_get_record_every(s) == 10, "every 10th tick, as the .sim says");
    check(p_se_log_file(s, "") && same(p_se_get_log_file(s), ""), "no log file: all to the console");
    p_se_log_capture(s, 1);
    {
        const int slip = p_se_channel_handle(s, "ws.slip");
        const int omega = p_se_channel_handle(s, "whl.omega");
        const double in[2] = {0.0, 0.0};   /* as the example host drives it */
        double out[4], read[2];
        const int handles[2] = {0, 1};
        uint64_t ticks = 0;
        int agree = 1;
        check(slip == 0 && omega == 1, "two handles");
        check(p_se_init(s), "the run starts");
        check(same(p_se_error(s), ""), "with nothing to report");
        while (!p_se_done(s)) {
            agree = agree && p_se_set_inputs(s, in, 2);
            p_se_tick(s);
            agree = agree && p_se_get_outputs(s, out, 4);
            agree = agree && p_se_read(s, slip) == out[2] && out[2] == p_se_get_output(s, 2);
            agree = agree && p_se_read_many(s, handles, 2, read) && read[0] == out[2] &&
                    read[1] == p_se_read(s, omega);
            ++ticks;
        }
        check(agree, "every tick: a handle reads what se_get_outputs reads");
        check(ticks == p_se_tick_count(s) + 1 && p_se_tick_count(s) == 5000,
              "tick_count + 1 ticks, the last sampling the final instant");
        check(p_se_time(s) == 5.0 && p_se_tick_index(s) == 5000, "time and tick index at the end");
        check(!p_se_record(s, "whl.omega"), "configuration after se_init is refused");
        check(has(p_se_error(s), "record() comes before init()"), "and se_error says so");
        check(p_se_realtime_frames(s) == 0 && p_se_realtime_utilization(s) == 0.0,
              "a batch run never paced");
        check(p_se_finish(s) == 0, "a clean finish");
    }

    /* ---- 6. the log, kept ---------------------------------------------- */
    {
        const char* line = p_se_log_next(s);
        check(has(line, "run completed") && p_se_log_line_level(s) == 2 &&
                  same(p_se_log_line_path(s), "sim") && p_se_log_line_time(s) == 5.0,
              "the console's closing line, kept");
        check(p_se_log_next(s) == NULL, "and nothing else");
    }

    p_se_destroy(s);
    p_se_destroy(NULL);
    FreeLibrary(dll);
    if (failures == 0) printf("c_host: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
