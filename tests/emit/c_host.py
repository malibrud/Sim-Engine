"""The C interface (SPECIFICATION.md section 14.2) from Python, through ctypes.

The essentials a Python host needs, against the drivetrain DLL: load it by
path, ask the model what it is, configure it one key at a time, drive the
inputs, tick, read a channel through a handle, and drain the log queue. It
declares only the functions it calls, as a real wrapper would.

    python -I c_host.py path\\to\\Drivetrain.dll

Run by tests/run.ps1 when Python is on PATH. Exit status 0 means every check
passed.
"""
import ctypes
import math
import sys

dll = ctypes.CDLL(sys.argv[1])
P = ctypes.c_void_p
S = ctypes.c_char_p
D = ctypes.c_double
I = ctypes.c_int
U64 = ctypes.c_uint64

for name, res, args in [
    ("se_abi_version", I, []),
    ("se_model_name", S, []),
    ("se_input_count", I, []),
    ("se_output_count", I, []),
    ("se_output_index", I, [S]),
    ("se_create", P, []),
    ("se_destroy", None, [P]),
    ("se_set", I, [P, S, D]),
    ("se_set_text", I, [P, S, S]),
    ("se_get", D, [P, S]),
    ("se_error", S, [P]),
    ("se_log_capture", None, [P, I]),
    ("se_log_file", I, [P, S]),
    ("se_record_clear", None, [P]),
    ("se_init", I, [P]),
    ("se_set_inputs", I, [P, ctypes.POINTER(D), I]),
    ("se_tick", None, [P]),
    ("se_done", I, [P]),
    ("se_get_outputs", I, [P, ctypes.POINTER(D), I]),
    ("se_channel_handle", I, [P, S]),
    ("se_read", D, [P, I]),
    ("se_tick_count", U64, [P]),
    ("se_finish", I, [P]),
    ("se_log_next", S, [P]),
    ("se_log_line_level", I, [P]),
    ("se_log_line_path", S, [P]),
]:
    f = getattr(dll, name)
    f.restype = res
    f.argtypes = args

failures = 0


def check(ok, what):
    global failures
    if not ok:
        print("c_host.py: FAIL " + what, file=sys.stderr)
        failures += 1


check(dll.se_abi_version() == 1, "the ABI version")
check(dll.se_model_name() == b"Drivetrain", "the model's name")
n_in, n_out = dll.se_input_count(), dll.se_output_count()
check((n_in, n_out) == (2, 4), "two inputs, four outputs")

s = dll.se_create()
check(dll.se_set_text(s, b"sim.duration", b"0.5 (s)") == 1, "the run's length, as text")
check(dll.se_tick_count(s) == 500, "moves tick_count")
check(dll.se_set(s, b"whl.radius", 0.3) == 1 and dll.se_get(s, b"whl.radius") == 0.3,
      "a setting, in the canonical unit")
check(dll.se_set(s, b"whl.nope", 1.0) == 0 and b"whl.nope" in dll.se_error(s),
      "a refusal, with its reason")
dll.se_record_clear(s)                      # no table: this host reads live
check(dll.se_log_file(s, b"") == 1, "no log file")
dll.se_log_capture(s, 1)
omega = dll.se_channel_handle(s, b"whl.state.omega")
check(omega >= 0, "a handle on a state")
check(dll.se_init(s) == 1, "the run starts")

inputs = (D * n_in)(300.0, 5.0)
outputs = (D * n_out)()
slip = dll.se_output_index(b"ws.slip")
last = None
ok = True
while not dll.se_done(s):
    ok = ok and dll.se_set_inputs(s, inputs, n_in) == 1
    dll.se_tick(s)
    ok = ok and dll.se_get_outputs(s, outputs, n_out) == 1
    last = dll.se_read(s, omega)
    ok = ok and math.isfinite(last) and math.isfinite(outputs[slip])
check(ok, "every tick: inputs in, outputs and a handle out")
check(last is not None and last > 5.0, "the wheel spun up under 300 N*m")
check(dll.se_finish(s) == 0, "a clean finish")

line = dll.se_log_next(s)
check(line is not None and b"run completed" in line and dll.se_log_line_level(s) == 2
      and dll.se_log_line_path(s) == b"sim", "the closing log line, kept for the host")
check(dll.se_log_next(s) is None, "and nothing else")
dll.se_destroy(s)

if failures == 0:
    print("c_host.py: all checks passed")
sys.exit(0 if failures == 0 else 1)
