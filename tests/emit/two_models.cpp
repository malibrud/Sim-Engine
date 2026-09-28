// ─────────────────────────────────────────────────────────────────────────────
//  Two generated models in one program.
//
//  Both public headers are included here, in one translation unit, and both
//  implementations are linked in beside it. Each implementation carries the
//  engine runtime pasted in, under a guard and an inline namespace named for a
//  hash of its text, so the two copies are the same inline definitions and link
//  as one. Each header guards its record types by name, so a record both models
//  use is defined once. If either guard goes, this stops compiling or linking.
//
//  Built by tests/run.ps1 from Drivetrain.cpp, Decay.cpp and this file.
// ─────────────────────────────────────────────────────────────────────────────
#include "Drivetrain.hpp"
#include "Decay.hpp"

int main() {
    drivetrain::Sim a;
    decay::Sim b;
    a.init();
    b.init();
    for (int i = 0; i < 10; ++i) {
        a.tick();
        b.tick();
    }
    return a.tick_index() == 10 && b.tick_index() == 10 ? 0 : 1;
}
