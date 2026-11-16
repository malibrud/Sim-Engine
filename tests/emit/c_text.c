/* ----------------------------------------------------------------------------
 *  Strings through the C interface (SPECIFICATION.md section 14.2), against
 *  the Text model's DLL, found by name as a foreign language would:
 *
 *    1. A text port's unit is "string"; a numeric port keeps its own.
 *    2. se_get_text reads a string setting, and se_set_text takes its text as
 *       it is; se_get on one is NaN.
 *    3. se_set_input_text writes a string input, refusing text that doesn't
 *       fit; se_set_input refuses a text port, and se_set_inputs ignores its
 *       slot.
 *    4. se_get_output_text reads a string output; se_get_output refuses one,
 *       and se_get_outputs fills its slot with NaN.
 *    5. se_read_text reads a string channel; se_read of one is NaN.
 *
 *  Built as C and run by tests/run.ps1 against build\release\bin\Text.dll.
 *  Exit status 0 means every check passed.
 * ------------------------------------------------------------------------- */
#include <windows.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "se_sim.h"

#define SE_FUNCTIONS(X)                                                        \
    X(int, se_input_count, (void))                                             \
    X(const char*, se_input_unit, (int))                                       \
    X(int, se_input_index, (const char*))                                      \
    X(int, se_output_count, (void))                                            \
    X(const char*, se_output_unit, (int))                                      \
    X(int, se_output_index, (const char*))                                     \
    X(se_sim*, se_create, (void))                                              \
    X(void, se_destroy, (se_sim*))                                             \
    X(double, se_get, (se_sim*, const char*))                                  \
    X(const char*, se_get_text, (se_sim*, const char*))                        \
    X(int, se_set_text, (se_sim*, const char*, const char*))                   \
    X(void, se_record_clear, (se_sim*))                                        \
    X(void, se_log_capture, (se_sim*, int))                                    \
    X(int, se_init, (se_sim*))                                                 \
    X(const char*, se_error, (se_sim*))                                        \
    X(int, se_set_input, (se_sim*, int, double))                               \
    X(int, se_set_inputs, (se_sim*, const double*, int))                       \
    X(int, se_set_input_text, (se_sim*, int, const char*))                     \
    X(void, se_tick, (se_sim*))                                                \
    X(double, se_get_output, (se_sim*, int))                                   \
    X(int, se_get_outputs, (se_sim*, double*, int))                            \
    X(const char*, se_get_output_text, (se_sim*, int))                         \
    X(int, se_channel_handle, (se_sim*, const char*))                          \
    X(double, se_read, (se_sim*, int))                                         \
    X(const char*, se_read_text, (se_sim*, int))                               \
    X(int, se_finish, (se_sim*))

#define DECLARE(ret, name, args) static ret(*p_##name) args;
SE_FUNCTIONS(DECLARE)

static int failures = 0;

static void check(int ok, const char* what) {
    if (ok) return;
    fprintf(stderr, "c_text: FAIL %s\n", what);
    ++failures;
}

static int same(const char* a, const char* b) { return a && b && strcmp(a, b) == 0; }
static int has(const char* text, const char* part) { return text && strstr(text, part) != NULL; }

int main(int argc, char** argv) {
    const char* path = argc > 1 ? argv[1] : "build\\release\\bin\\Text.dll";
    HMODULE dll = LoadLibraryA(path);
    if (!dll) {
        fprintf(stderr, "c_text: cannot load %s\n", path);
        return 1;
    }
    int missing = 0;
#define LOAD(ret, name, args)                                                  \
    p_##name = (ret(*) args)GetProcAddress(dll, #name);                        \
    if (!p_##name) {                                                           \
        fprintf(stderr, "c_text: FAIL %s is not exported\n", #name);           \
        ++missing;                                                             \
    }
    SE_FUNCTIONS(LOAD)
    if (missing) return 1;

    /* ---- 1. the ports ----------------------------------------------------- */
    const int cmd = p_se_input_index("cmd"), gain = p_se_input_index("gain");
    const int echo = p_se_output_index("echo");
    check(cmd >= 0 && gain >= 0 && echo >= 0, "the ports are found by name");
    check(same(p_se_input_unit(cmd), "string"), "a text input's unit is `string`");
    check(same(p_se_input_unit(gain), "-"), "a numeric input keeps its unit");
    check(same(p_se_output_unit(echo), "string"), "a text output's unit is `string`");

    se_sim* s = p_se_create();
    p_se_log_capture(s, 1);

    /* ---- 2. a string setting -------------------------------------------- */
    check(same(p_se_get_text(s, "label"), "from the sim"), "se_get_text");
    check(isnan(p_se_get(s, "label")), "se_get of a string is NaN");
    check(p_se_set_text(s, "label", "x; \"y\""), "se_set_text takes the text as it is");
    check(same(p_se_get_text(s, "label"), "x; \"y\""), "and se_get_text gives it back");
    check(p_se_get_text(s, "sim.duration") == NULL, "se_get_text of a number is NULL");

    /* ---- 3. writing string inputs ----------------------------------------- */
    check(!p_se_set_input(s, cmd, 1.0) && has(p_se_error(s), "se_set_input_text writes"),
          "se_set_input refuses a text port");
    check(!p_se_set_input_text(s, gain, "1"), "se_set_input_text refuses a numeric port");
    check(!p_se_set_input_text(s, cmd, "0123456789abcdefg") &&
              has(p_se_error(s), "17 bytes, and it holds 16"),
          "se_set_input_text refuses text that doesn't fit");
    p_se_record_clear(s);
    check(p_se_init(s), "se_init");
    check(p_se_set_input_text(s, cmd, "go"), "se_set_input_text");
    {
        double in[2];
        in[0] = 123.0;   /* whichever slot is the text port's is ignored */
        in[1] = 123.0;
        in[gain] = 4.0;
        check(p_se_set_inputs(s, in, 2), "se_set_inputs ignores a text port's slot");
    }

    /* ---- 4, 5. reading strings back ------------------------------------- */
    const int seen = p_se_channel_handle(s, "mid.tag.var.seen");
    check(seen >= 0, "a string channel takes a handle");
    p_se_tick(s);
    check(same(p_se_get_output_text(s, echo), "go"), "se_get_output_text: the input came back");
    check(isnan(p_se_get_output(s, echo)) && has(p_se_error(s), "se_get_output_text reads"),
          "se_get_output refuses a text port");
    {
        double out[4] = {0.0, 0.0, 0.0, 0.0};
        const int n = p_se_output_count();
        check(n == 4 && p_se_get_outputs(s, out, n) && isnan(out[echo]),
              "se_get_outputs fills a text port's slot with NaN");
    }
    check(p_se_get_output_text(s, 99) == NULL, "se_get_output_text of a bad index is NULL");
    check(same(p_se_read_text(s, seen), "go"), "se_read_text");
    check(isnan(p_se_read(s, seen)), "se_read of a string is NaN");

    p_se_finish(s);
    p_se_destroy(s);
    if (failures == 0) printf("c_text: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
