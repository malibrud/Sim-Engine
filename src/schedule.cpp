// ─────────────────────────────────────────────────────────────────────────────
//  Stage 5 — scheduling.  SPECIFICATION.md §8.5, §8.6.
// ─────────────────────────────────────────────────────────────────────────────
#include "schedule.hpp"

#include <algorithm>
#include <string>
#include <vector>

namespace se {
namespace {

using Call = Model::OutputCall;

struct Edge {
    std::size_t from;      // producing vertex
    std::size_t to;        // consuming vertex
    std::string src_port;  // the producer's output port
    std::string dst_port;  // the consumer's feedthrough input
};

const NodeInfo::OutputGroup* group_of(const Model& m, const Call& c) {
    if (c.group == Call::none) return nullptr;
    return &m.leaves[c.leaf].node->out_groups[c.group];
}

std::string where(const Model& m, const Call& c) {
    const Leaf& leaf = m.leaves[c.leaf];
    const NodeInfo::OutputGroup* g = group_of(m, c);
    const Loc loc = g ? g->method->loc : leaf.node->def->loc;
    return leaf.node->file->path + ":" + std::to_string(loc.line);
}

// `output` or `output a, b` — the method's head as its author wrote it.
std::string head(const NodeInfo::OutputGroup* g) {
    std::string h = "output";
    if (!g) return h;
    for (std::size_t i = 0; i < g->method->writes.size(); ++i)
        h += (i ? ", " : " ") + g->method->writes[i];
    return h;
}

}  // namespace

bool schedule(Diagnostics& diag, Model& m) {
    // The vertices are output methods (§8.3a): every one of every leaf, in
    // elaboration order and then declaration order, so a model without groups
    // has exactly the vertices it had when they were leaves. A leaf with no
    // output method is one vertex with no edges.
    std::vector<Call> verts;
    std::vector<std::vector<std::size_t>> vert_of(m.leaves.size());
    for (std::size_t i = 0; i < m.leaves.size(); ++i) {
        const std::size_t ng = m.leaves[i].node->out_groups.size();
        if (ng == 0) {
            vert_of[i].push_back(verts.size());
            verts.push_back({i, Call::none});
        }
        for (std::size_t g = 0; g < ng; ++g) {
            vert_of[i].push_back(verts.size());
            verts.push_back({i, g});
        }
    }
    // The vertex that writes a leaf's output port.
    auto writer = [&](std::size_t leaf, const std::string& port) {
        const auto& groups = m.leaves[leaf].node->out_groups;
        for (std::size_t g = 0; g < groups.size(); ++g)
            if (groups[g].writes_port(port)) return vert_of[leaf][g];
        return vert_of[leaf].front();
    };

    const std::size_t n = verts.size();
    std::vector<Edge> edges;
    std::vector<std::vector<std::size_t>> succ(n);
    std::vector<int> indegree(n, 0);

    for (std::size_t v = 0; v < n; ++v) {
        const Leaf& leaf = m.leaves[verts[v].leaf];
        const NodeInfo::OutputGroup* g = group_of(m, verts[v]);
        if (!g) continue;
        // Only `output()`'s list constrains ordering. `derivative()`, `next()`,
        // `adjust()` and `on_step()` run after outputs have propagated, so their
        // lists are documentation plus enforcement (§8.3).
        for (const std::string& p : g->method->params) {
            auto it = leaf.inputs.find(p);
            if (it == leaf.inputs.end()) continue;
            // Anything but a producing leaf is already there when the tick
            // starts: a root input the host wrote, or a setting resolved at
            // configuration (§6.9.2). Neither constrains the order.
            if (it->second.kind != InputSource::Kind::Leaf) continue;
            // A group fed by another group of its own leaf is an ordinary edge
            // (§8.3a). One fed by itself is a one-vertex loop, and that falls
            // out of the general check below.
            const std::size_t from = writer(it->second.producer, it->second.port);
            edges.push_back({from, v, it->second.port, p});
            succ[from].push_back(v);
            ++indegree[v];
        }
    }

    // Kahn, taking the lowest vertex index among the ready ones, so the emitted
    // schedule is stable and reads in declaration order wherever the graph does
    // not force otherwise.
    std::vector<std::size_t> ready;
    for (std::size_t v = 0; v < n; ++v)
        if (indegree[v] == 0) ready.push_back(v);
    std::vector<int> remaining = indegree;

    m.calls.clear();
    m.order.clear();
    std::vector<bool> placed(m.leaves.size(), false);
    while (!ready.empty()) {
        auto pick = std::min_element(ready.begin(), ready.end());
        const std::size_t v = *pick;
        ready.erase(pick);
        m.calls.push_back(verts[v]);
        if (!placed[verts[v].leaf]) {
            placed[verts[v].leaf] = true;
            m.order.push_back(verts[v].leaf);
        }
        for (std::size_t j : succ[v])
            if (--remaining[j] == 0) ready.push_back(j);
    }

    if (m.calls.size() == n) return true;

    // ─── SE0510 ──────────────────────────────────────────────────────────────
    // Everything still holding an in-edge is in or downstream of a cycle. Walk
    // it to find one concrete cycle: §8.6 makes beating "Algebraic loop
    // detected involving block X" an explicit goal, and the budget goes here.
    std::vector<bool> stuck(n, false);
    for (std::size_t v = 0; v < n; ++v) stuck[v] = remaining[v] > 0;

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

    for (std::size_t v = 0; v < n && cycle.empty(); ++v)
        if (stuck[v] && colour[v] == 0) dfs.visit(v);

    if (cycle.empty()) {
        for (std::size_t v = 0; v < n; ++v)
            if (stuck[v]) cycle.push_back(v);
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
        path += m.leaves[verts[a].leaf].path + "." + e->src_port + " -> " +
                m.leaves[verts[b].leaf].path + "." + e->dst_port;
    }
    if (!path.empty()) att.push_back(note("cycle: " + path));

    std::string fix_node, fix_head, fix_port, group_help;
    for (std::size_t k = 0; k < cycle.size(); ++k) {
        const std::size_t a = cycle[k];
        const std::size_t b = cycle[(k + 1) % cycle.size()];
        const Edge* e = edge_between(a, b);
        if (!e) continue;
        const Leaf& consumer = m.leaves[verts[b].leaf];
        const NodeInfo::OutputGroup* g = group_of(m, verts[b]);
        att.push_back(note(consumer.path + "." + head(g) + "(" + e->dst_port +
                               ") declares feedthrough on `" + e->dst_port + "`",
                           where(m, verts[b])));
        if (fix_node.empty()) {
            fix_node = consumer.path;
            fix_head = head(g);
            fix_port = e->dst_port;
        }
        // §8.6 — the false loop of §8.3a. The cycle enters this method on
        // `dst_port` and leaves it on the port the next edge carries. When the
        // method writes more than that one output, the leaving one may not need
        // the input at all; bodies are verbatim, so the help is conditional.
        if (group_help.empty() && g && g->writes.size() > 1) {
            const Edge* leave = edge_between(b, cycle[(k + 2) % cycle.size()]);
            if (!leave) continue;
            std::string rest;
            for (const std::string& p : g->method->params)
                if (p != e->dst_port) rest += (rest.empty() ? "" : ", ") + p;
            group_help = "if `" + leave->src_port + "` does not need `" + e->dst_port +
                         "`, give it its own group in `" + consumer.node->def->name +
                         "`: `output " + leave->src_port + "(" + rest +
                         ") { ... }` (§8.3a)";
        }
    }
    if (!group_help.empty()) att.push_back(help(group_help));
    if (!fix_node.empty())
        att.push_back(help("drop `" + fix_port + "` from `" + fix_node + "." + fix_head + "(" +
                           fix_port + ")` and latch it in `next()`, or insert a unit "
                           "delay / integrator anywhere in the cycle"));
    att.push_back(note("an algebraic loop is an implicit equation z = f(z) with no valid "
                       "ordering; it is rejected rather than solved, because most are "
                       "modelling mistakes and iteration has no statable worst-case "
                       "execution time (§8.6)"));

    // The count is of nodes, as it always was: a cycle through two groups of
    // one node passes through one node.
    std::size_t nodes = 0;
    {
        std::vector<bool> seen(m.leaves.size(), false);
        for (std::size_t v : cycle)
            if (!seen[verts[v].leaf]) {
                seen[verts[v].leaf] = true;
                ++nodes;
            }
    }
    const Call& first = verts[cycle.front()];
    const Leaf& first_leaf = m.leaves[first.leaf];
    const NodeInfo::OutputGroup* first_g = group_of(m, first);
    diag.error("SE0510", *first_leaf.node->file->src,
               first_g ? first_g->method->loc : first_leaf.node->def->loc,
               "algebraic loop through " + std::to_string(nodes) +
                   (nodes == 1 ? " node" : " nodes"),
               "in this feedthrough set", std::move(att));
    return false;
}

}  // namespace se
