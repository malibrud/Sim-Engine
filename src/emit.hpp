// ─────────────────────────────────────────────────────────────────────────────
//  Stage 6 — emission.  SPECIFICATION.md §11, §12.4, §15.
//
//  Writes, into one output directory:
//
//      <name>.generated.hpp   the model, the schedule, the host API
//      <name>.main.cpp        the batch driver, and nothing else
//      <name>.units.txt       the unit manifest — load-bearing, not docs (§15.6)
//      state_ref.hpp          fixed engine runtime header (§15.5)
//      se_runtime.hpp         fixed engine runtime header
//      build.bat / build.sh   the direct reference build (§12.4)
//
//  The model is a header so that a host program can include it and drive the
//  root boundary itself; the driver is a separate translation unit so it can
//  be omitted without editing a file stamped DO NOT EDIT.
//
//  The emitter owns every generated identifier (I5) and every line of the
//  class shape; the only text it does not own is the verbatim regions of §11,
//  which are copied through byte for byte with `#line` on both sides.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_EMIT_HPP
#define SE_EMIT_HPP

#include <string>

#include "diag.hpp"
#include "model.hpp"

namespace se {

struct EmitOptions {
    std::string out_dir = ".";
    // Defaults to `Sim`, giving `Sim.generated.cpp` and `Sim.units.txt`.
    std::string stem = "Sim";
    bool write_main = true;
    bool write_runtime = true;
    bool write_build = true;
    bool quiet = false;
};

bool emit(Diagnostics& diag, const Model& m, const EmitOptions& opt);

// The fixed runtime headers, embedded in the compiler (see runtime_src.cpp).
const char* state_ref_hpp();
const char* se_runtime_hpp();

}  // namespace se

#endif  // SE_EMIT_HPP
