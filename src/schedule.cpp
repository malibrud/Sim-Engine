// ─────────────────────────────────────────────────────────────────────────────
//  Stage 5 — scheduling.  SPECIFICATION.md §8.5, §8.6.
// ─────────────────────────────────────────────────────────────────────────────
#include "schedule.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace se {
namespace {

struct Edge {
    std::size_t from;      // producing leaf
    std::size_t to;        // consuming leaf
    std::string src_port;  // the producer's output port
    std::string dst_port;  // the consumer's feedthrough input
};

std::string where(const Leaf& leaf) {
    const ast::Method* m = leaf.node->method(ast::Method::Which::Output);
    const Loc loc = m ? m->loc : leaf.node->def->loc;
    return leaf.node->file->path + ":" + std::to_string(loc.line);
}

}  // namespace

bool schedule(Diagnostics& diag, Model& m) {
    const std::size_t n = m.leaves.size();
    std::vector<Edge> edges;
    std::vector<std::vector<std::size_t>> succ(n);
    std::vector<int> indegree(n, 0);

    for (std::size_t i = 0; i < n; ++i) {
        const Leaf& leaf = m.leaves[i];
        const ast::Method* out = leaf.node->method(ast::Method::Which::Output);
        if (!out) continue;
        // Only `output()`'s list constrains ordering. `derivative()`, `next()`
        // and `on_step()` run after outputs have propagated, so their lists are
        // documentation plus enforcement (§8.3).
        for (const std::string& p : out->params) {
            auto it = leaf.inputs.find(p);
            if (it == leaf.inputs.end()) continue;
            // Anything but a producing leaf is already there when the tick
            // starts: a root input the host wrote, or a setting resolved at
            // configuration (§6.9.2). Neither constrains the order.
            if (it->second.kind != InputSource::Kind::Leaf) continue;
            const std::size_t from = it->second.producer;
            if (from == i) {
                // A node feeding its own feedthrough input is a one-node loop;
                // it falls out of the general check below, but naming it here
                // keeps the cycle rendering honest.
            }
            edges.push_back({from, i, it->second.port, p});
            succ[from].push_back(i);
            ++indegree[i];
        }
    }

    // Kahn, taking the lowest elaboration index among the ready nodes, so the
    // emitted schedule is stable and reads in declaration order wherever the
    // graph does not force otherwise.
    std::vector<std::size_t> ready;
    for (std::size_t i = 0; i < n; ++i)
        if (indegree[i] == 0) ready.push_back(i);
    std::vector<int> remaining = indegree;

    m.order.clear();
    while (!ready.empty()) {
        auto pick = std::min_element(ready.begin(), ready.end());
        const std::size_t i = *pick;
        ready.erase(pick);
        m.order.push_back(i);
        for (std::size_t j : succ[i])
            if (--remaining[j] == 0) ready.push_back(j);
    }

    if (m.order.size() == n) return true;

    // ─── SE0510 ──────────────────────────────────────────────────────────────
    // Everything still holding an in-edge is in or downstream of a cycle. Walk
    // it to find one concrete cycle: §8.6 makes beating "Algebraic loop
    // detected involving block X" an explicit goal, and the budget goes here.
    std::vector<bool> stuck(n, false);
    for (std::size_t i = 0; i < n; ++i) stuck[i] = remaining[i] > 0;

    std::vector<int> colour(n, 0);   // 0 white, 1 grey, 2 black
    std::vector<std::size_t> stack;
    std::vector<std::size_t> cycle;

    std::vector<std::vector<const Edge*>> out_edges(n);
    for (const Edge& e : edges)
        if (stuck[e.from] && stuck[e.to]) out_edges[e.from].push_back(&e);

    struct Dfs {
        const std::vector<std::vector<const Edge*>>& out_edges;
        std::vector<int>& colour;
        std::vector<std::size_t>& stack;
        std::vector<std::size_t>& cycle;

        bool visit(std::size_t i) {
            colour[i] = 1;
            stack.push_back(i);
            for (const Edge* e : out_edges[i]) {
                if (colour[e->to] == 1) {
                    auto it = std::find(stack.begin(), stack.end(), e->to);
                    cycle.assign(it, stack.end());
                    return true;
                }
                if (colour[e->to] == 0 && visit(e->to)) return true;
            }
            stack.pop_back();
            colour[i] = 2;
            return false;
        }
    } dfs{out_edges, colour, stack, cycle};

    for (std::size_t i = 0; i < n && cycle.empty(); ++i)
        if (stuck[i] && colour[i] == 0) dfs.visit(i);

    if (cycle.empty()) {
        for (std::size_t i = 0; i < n; ++i)
            if (stuck[i]) cycle.push_back(i);
    }

    // The signal path around the cycle, in the form a reader can follow.
    auto edge_between = [&](std::size_t a, std::size_t b) -> const Edge* {
        for (const Edge& e : edges)
            if (e.from == a && e.to == b) return &e;
        return nullptr;
    };

    std::string path;
    std::vector<Attachment> att;
    for (std::size_t k = 0; k < cycle.size(); ++k) {
        const std::size_t a = cycle[k];
        const std::size_t b = cycle[(k + 1) % cycle.size()];
        const Edge* e = edge_between(a, b);
        if (!e) continue;
        if (!path.empty()) path += " -> ";
        path += m.leaves[a].path + "." + e->src_port + " -> " + m.leaves[b].path + "." +
                e->dst_port;
    }
    if (!path.empty()) att.push_back(note("cycle: " + path));

    std::string fix_node, fix_port;
    for (std::size_t k = 0; k < cycle.size(); ++k) {
        const std::size_t a = cycle[k];
        const std::size_t b = cycle[(k + 1) % cycle.size()];
        const Edge* e = edge_between(a, b);
        if (!e) continue;
        const Leaf& consumer = m.leaves[b];
        att.push_back(note(consumer.path + ".output(" + e->dst_port +
                               ") declares feedthrough on `" + e->dst_port + "`",
                           where(consumer)));
        if (fix_node.empty()) {
            fix_node = consumer.path;
            fix_port = e->dst_port;
        }
    }
    if (!fix_node.empty())
        att.push_back(help("drop `" + fix_port + "` from `" + fix_node + ".output(" +
                           fix_port + ")` and latch it in `next()`, or insert a unit "
                           "delay / integrator anywhere in the cycle"));
    att.push_back(note("an algebraic loop is an implicit equation z = f(z) with no valid "
                       "ordering; it is rejected rather than solved, because most are "
                       "modelling mistakes and iteration has no statable worst-case "
                       "execution time (§8.6)"));

    const Leaf& first = m.leaves[cycle.front()];
    const ast::Method* out_m = first.node->method(ast::Method::Which::Output);
    diag.error("SE0510", *first.node->file->src,
               out_m ? out_m->loc : first.node->def->loc,
               "algebraic loop through " + std::to_string(cycle.size()) +
                   (cycle.size() == 1 ? " node" : " nodes"),
               "in this feedthrough set", std::move(att));
    return false;
}

}  // namespace se
