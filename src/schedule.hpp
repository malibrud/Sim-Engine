// ─────────────────────────────────────────────────────────────────────────────
//  Stage 5 — scheduling.  SPECIFICATION.md §8.5, §8.6, §9.3.
//
//  One graph, one sort, one diagnostic:
//
//      edge M -> N  iff  M sources one of N's FEEDTHROUGH inputs
//
//  where "feedthrough" means nothing more than "named in N's `output()`
//  parameter list" (§8.3). A node with an empty list is a sort root, which is
//  what makes registered discrete nodes, unit delays and integrators all
//  loop-breakers by one mechanism rather than three special cases.
//
//  The sort is computed once over the whole leaf set rather than per tick. A
//  slow node's edges exist on its sample ticks, and the active set on any tick
//  is a subsequence of the full order, so one static order serves every tick
//  (§9.3).
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_SCHEDULE_HPP
#define SE_SCHEDULE_HPP

#include "diag.hpp"
#include "model.hpp"

namespace se {

// Fills `m.order`. False, having reported SE0510, on an algebraic loop.
bool schedule(Diagnostics& diag, Model& m);

}  // namespace se

#endif  // SE_SCHEDULE_HPP
