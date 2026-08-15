// ─────────────────────────────────────────────────────────────────────────────
//  Unity translation unit.
//
//  Every source file of the compiler, plus the driver, compiled as one TU. This
//  is the same build model SPECIFICATION.md §12.4 prescribes for generated
//  models: one compile, no object files, so "incremental" is not a feature that
//  was skipped but a category that does not exist.
//
//  To use your own build system instead, compile the individual .cpp files —
//  they are self-contained and include what they use.
// ─────────────────────────────────────────────────────────────────────────────

#include "diag.cpp"
#include "dump.cpp"
#include "lexer.cpp"
#include "parser.cpp"
#include "units.cpp"
#include "resolve.cpp"
#include "elaborate.cpp"
#include "schedule.cpp"
#include "emit.cpp"
#include "runtime_src.cpp"

#include "../tools/sec.cpp"
