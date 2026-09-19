// ─────────────────────────────────────────────────────────────────────────────
//  Node tests.  SPECIFICATION.md §6.13.
//
//  `sec --test` reads a node's `tests` section and writes a program that runs
//  it. Each tested node is elaborated as the root of a synthesised model, so
//  its settings, units and states go through exactly the machinery a real
//  model's do, and the generated class under test is the one a real model
//  would compile. The program then fabricates one (settings, states, inputs)
//  point per row, invokes the node's pure methods on it, and compares what
//  came back. There is no solver, no recorder and no sim file.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_TESTGEN_HPP
#define SE_TESTGEN_HPP

#include <string>
#include <vector>

#include "diag.hpp"

namespace se {

struct TestGenOptions {
    // One subdirectory per tested node is written under here, each holding the
    // generated model, the test program as `Sim.main.cpp`, and the ordinary
    // reference build scripts.
    std::string out_dir = ".";
    bool quiet = false;
};

// Returns 0 when every tested node in `path` was written, 1 if any error was
// reported, 2 on a driver failure.
int generate_tests(const std::string& path, Diagnostics& diag,
                   const std::vector<std::string>& roots, const TestGenOptions& opt);

}  // namespace se

#endif  // SE_TESTGEN_HPP
