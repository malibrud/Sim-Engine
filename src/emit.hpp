// ─────────────────────────────────────────────────────────────────────────────
//  Stage 6 — emission.  SPECIFICATION.md §11, §12.4, §15.
//
//  Writes, into one output directory, which it creates if it is not there:
//
//      <stem>.hpp             the public interface: class Sim, with the root
//                             node's settings, inputs and outputs, and the
//                             configure / start / step / finish calls
//      <stem>.cpp             everything behind it: the engine runtime, the
//                             node classes, Sim::Impl and the schedule
//      main.cpp               an example host, using <stem>.hpp alone
//      <stem>.units.txt       the unit manifest — load-bearing, not docs (§15.6)
//      <stem>.topology.md     the signal-flow graph, a Mermaid diagram in a
//                             markdown fence
//      se_runtime.hpp         only with `--runtime=separate`: the runtime as
//                             its own header, included instead of pasted
//      build.ps1 / build.sh   the reference build (§12.4): run, exe, lib,
//                             test, clean, all; into build/<config>/
//
//  <stem> is the sim's name unless `--stem` says otherwise. The header holds
//  the interface and nothing else, so a host that includes it — or links the
//  library built from <stem>.cpp — depends on no layout it cannot see.
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
    // Empty means the sim's name: `Drivetrain.hpp`, `Drivetrain.cpp`,
    // `Drivetrain.units.txt`, and the products built from them.
    std::string stem;
    bool write_main = true;
    // `sec --test`: the program in this file #includes <stem>.cpp itself, so
    // the build scripts compile it alone and no main.cpp is written.
    std::string unity_main;
    // Write the runtime to se_runtime.hpp and #include it, rather than paste
    // it into the model header. For a program that ships several models and
    // wants the runtime as one file of its own; the pasted copies would be
    // deduplicated by their include guard either way.
    bool separate_runtime = false;
    bool write_build = true;
    bool write_topology = true;
    bool quiet = false;
};

bool emit(Diagnostics& diag, const Model& m, const EmitOptions& opt);

// The fixed engine runtime, embedded in the compiler, with its version tag
// filled in (see runtime_src.cpp).
const std::string& se_runtime_hpp();

}  // namespace se

#endif  // SE_EMIT_HPP
