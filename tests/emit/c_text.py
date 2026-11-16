"""Strings through the C interface (SPECIFICATION.md section 14.2) from Python.

Against the Text model's DLL: read and set a string setting, write a string
input, tick, and read the string output and a string channel back.

    python -I c_text.py path\\to\\Text.dll

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

for name, res, args in [
    ("se_input_index", I, [S]),
    ("se_output_index", I, [S]),
    ("se_create", P, []),
    ("se_destroy", None, [P]),
    ("se_get", D, [P, S]),
    ("se_get_text", S, [P, S]),
    ("se_set_text", I, [P, S, S]),
    ("se_error", S, [P]),
    ("se_log_capture", None, [P, I]),
    ("se_record_clear", None, [P]),
    ("se_init", I, [P]),
    ("se_set_input_text", I, [P, I, S]),
    ("se_tick", None, [P]),
    ("se_get_output_text", S, [P, I]),
    ("se_channel_handle", I, [P, S]),
    ("se_read_text", S, [P, I]),
    ("se_finish", I, [P]),
]:
    f = getattr(dll, name)
    f.restype = res
    f.argtypes = args

failures = []


def check(ok, what):
    if not ok:
        failures.append(what)
        print("c_text.py: FAIL " + what, file=sys.stderr)


s = dll.se_create()
dll.se_log_capture(s, 1)
check(dll.se_get_text(s, b"label") == b"from the sim", "se_get_text")
check(math.isnan(dll.se_get(s, b"label")), "se_get of a string is NaN")
check(dll.se_set_text(s, b"label", "café, \"ok\"".encode("utf-8")) == 1, "se_set_text, UTF-8")
check(dll.se_get_text(s, b"label").decode("utf-8") == "café, \"ok\"", "the text comes back")
dll.se_record_clear(s)
check(dll.se_init(s) == 1, "se_init: " + str(dll.se_error(s)))
cmd = dll.se_input_index(b"cmd")
echo = dll.se_output_index(b"echo")
check(dll.se_set_input_text(s, cmd, b"go") == 1, "se_set_input_text")
seen = dll.se_channel_handle(s, b"mid.tag.var.seen")
dll.se_tick(s)
check(dll.se_get_output_text(s, echo) == b"go", "se_get_output_text")
check(dll.se_read_text(s, seen) == b"go", "se_read_text")
dll.se_finish(s)
dll.se_destroy(s)

if failures:
    sys.exit(1)
print("c_text.py: all checks passed")
