// ─────────────────────────────────────────────────────────────────────────────
//  Documentation.  SPECIFICATION.md §3.2 (doc comments) and §6.14.
//
//  `sec --doc` reads `.se` files to stage 2 only, attaches each `///` block to
//  the declaration it documents, and writes a Markdown reference: an index of
//  packages, and one page per package. Everything a declaration states for
//  itself — types, units, defaults, initial conditions, the inputs `output()`
//  reads, the `tests` tables — is taken from the source as written, so the
//  comment supplies prose only and the two cannot disagree.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_DOC_HPP
#define SE_DOC_HPP

#include <string>
#include <vector>

#include "diag.hpp"

namespace se {

struct DocOptions {
    std::string out_dir = ".";
    // SPECIFICATION.md, against which `§` and `SE` citations are checked and
    // linked. Empty means: look beside the first input, then in the current
    // directory; if there is none, citations are neither checked nor linked.
    std::string spec_path;
    bool quiet = false;
};

// `inputs` are root directories (every `.se` beneath is documented) or single
// `.se` files. Returns 0 when the reference was written, 1 if any error was
// reported (nothing is written), 2 on a driver failure.
int generate_docs(const std::vector<std::string>& inputs, Diagnostics& diag,
                  const DocOptions& opt);

}  // namespace se

#endif  // SE_DOC_HPP
