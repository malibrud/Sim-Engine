// ─────────────────────────────────────────────────────────────────────────────
//  Stage 4 — elaboration.  SPECIFICATION.md §7, §9.2, §13, §14.
//
//  Folds the hierarchy flat. Settings become numbers, composites evaporate,
//  wires become producer/consumer pairs with their conversion folded to a
//  constant, rates become integer decimations, and states are handed slots in
//  the flat vector.
//
//  Everything here happens once, at compile time, and the language of §7 is
//  small precisely so that it can: no conditionals, no run-time signals, no
//  cross-instance references, and therefore a guaranteed-terminating evaluation
//  whose every result folds into the generated code.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_ELABORATE_HPP
#define SE_ELABORATE_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "diag.hpp"
#include "model.hpp"
#include "resolve.hpp"

namespace se {

// A value in the §7 expression language: a real number with a dimension.
struct Value {
    double v = 0.0;
    Unit unit;
    // §7.2 — "a bare literal takes the unit of the site it initialises". True
    // while every literal that contributed was written without a unit suffix,
    // so `0.31` adopts `(m)` but `130 (km/h)` is converted.
    bool bare = true;

    // §15.5 — the same expression in emittable form, built by the same walk that
    // computes `v`. Keeping them in one traversal is what makes the promise in
    // §7 hold: the emitted and the evaluated reading of a §7 expression cannot
    // drift, because there is only one reading.
    ExprCode code;

    // False once anything that a configuration override could move (§6.2b) has
    // entered the expression — a `param.*`, or `sample_rate`, which is derived
    // from the overridable `rate`. While true the whole expression folds and
    // `cpp` is discarded in favour of the number, which is why the generated
    // code for a model that overrides nothing looks exactly as it did before
    // settings became runtime values.
    bool constant = true;
};

// A setting whose value arrives from outside the node — a flow-down binding
// (§6.9.1) or an override (§13.3). It carries the expression as well as the
// number, because a binding that reads the parent's `param.*` has to keep
// reading it at configuration for an override of the parent to reach the child.
//
// An override is `constant`: §13.3 says a deep override detaches from the
// flow-down, so it emits as a literal and the flow-down expression is not run
// for that setting at all — the same rule at elaboration and at configuration.
struct Pin {
    double value = 0.0;
    ExprCode code;
    bool constant = true;
};

// One path-addressed override from the sim file's root block or a `.settings`
// source (§13.3, §14). Last writer wins, and these are the last writer.
struct OverrideEntry {
    std::string path;              // root-relative, "" for a bare root setting
    const ast::Expr* value = nullptr;
    const Source* src = nullptr;
    Loc loc;
    bool used = false;
};

class Elaborator {
public:
    Elaborator(Diagnostics& diag, Resolver& resolver);

    // `sim_dir` is the directory the sim file lives in; a `settings:` entry is
    // resolved relative to it. Returns false if any error was reported.
    bool run(const ast::SimFile& sim, const Source& sim_src, const std::string& sim_dir,
             Model& out);

private:
    // ─── The sim file ────────────────────────────────────────────────────────
    bool read_sim_entries(const ast::SimFile& sim, const Source& src, Model& m);
    bool load_settings_file(const std::string& path, const Source& from, Loc loc);
    bool time_value(const ast::Expr* e, const Source& src, const char* key, double& out);

    // ─── The §7 evaluator ────────────────────────────────────────────────────
    using Env = std::map<std::string, Value>;
    bool eval(const ast::Expr* e, const Source& src, const Env& env, Value& out);
    bool eval_call(const ast::Expr* e, const Source& src, const Env& env, Value& out);
    // Converts `v` to `target`, reporting SE0410 against `loc` if it cannot.
    // When `out_code` is given it receives the same conversion applied to the
    // expression rather than to the number (§15.5).
    bool coerce(const Value& v, const Type& target, const Source& src, Loc loc,
                const char* what, double& out, ExprCode* out_code = nullptr);

    // ─── Instantiation ───────────────────────────────────────────────────────
    void instantiate(const NodeInfo* def, const std::string& path,
                     std::map<std::string, Pin> pinned, const Source& src, Loc loc,
                     int depth, Model& m, std::vector<Unit> unit_binding = {});
    // §6.2a — evaluates an instance's unit arguments against the definition's
    // `units` declaration, checking arity. Empty (and silent) for a definition
    // with no unit parameters and no arguments.
    std::vector<Unit> unit_args_of(const NodeInfo* child, const ast::Instance& inst,
                                   const Source& src);
    // The unit binding of the node currently being instantiated. A member for
    // the same reason `Resolver::unit_params_` is: every declared unit inside
    // the node has to be substituted, and they are reached from settings,
    // states and ports alike.
    std::vector<Unit> unit_binding_;
    Unit bound(const Unit& u) const { return unit_bind(u, unit_binding_); }

    // §10.4 — `sample_rate` / `time_step`. `base_rate_` is 1/step from the sim
    // file; `effective_rate_` is the node currently being elaborated, which is
    // its declared `rate` when it has one and the base rate otherwise.
    double base_rate_ = 0.0;
    double effective_rate_ = 0.0;
    // The same rate as something emittable (§6.2b). A node that declares `rate`
    // reads its own `param.rate`, so an override of it moves every coefficient
    // derived from `sample_rate`; a node that does not declare one has nothing
    // to override and folds to the base rate.
    ExprCode effective_rate_code_;
    // Model path of the node whose settings are being evaluated — the owner
    // recorded on every `param.*` reference, since §6.2 confines those to the
    // node itself.
    std::string settings_path_;
    // Filled by `settings_of` with the node's slice of the configuration program
    // and consumed immediately by its caller, which is the only place that knows
    // where in the model this node sits.
    std::vector<SettingSlot> slots_;
    // Evaluates a node's settings: pinned values first, then the remaining
    // defaults in dependency order (§6.2), so overriding `total_mass`
    // recomputes `corner_mass` while overriding `corner_mass` pins it.
    bool settings_of(const NodeInfo* def, const std::map<std::string, Pin>& pinned,
                     const Source& src, Loc loc, std::vector<SettingValue>& out);
    void apply_overrides(const NodeInfo* def, const std::string& path,
                         std::map<std::string, Pin>& pinned);
    OverrideEntry* find_override(const std::string& path);

    // ─── Wires ───────────────────────────────────────────────────────────────
    // One hop of a wire, recorded while the composite is walked and replayed
    // when a leaf input is resolved.
    struct Hop {
        std::string source;        // the producing endpoint's path
        double scale = 1.0;
        double offset = 0.0;
        Loc loc;
        const Source* src = nullptr;
    };
    // `child_binding` maps a child instance name to its §6.2a unit arguments.
    // Passed in rather than recomputed so arity errors are reported once.
    void collect_wires(const NodeInfo* def, const std::string& path, Model& m,
                       const std::map<std::string, std::vector<Unit>>& child_binding);
    bool port_conversion(const Field& from, const Field& to, const Source& src, Loc loc,
                         double& scale, double& offset);
    void resolve_inputs(Model& m);
    void resolve_boundary(const NodeInfo* root, Model& m);

    // ─── Recording and logging ───────────────────────────────────────────────
    void resolve_record(const ast::RecordBlock& block, const Source& src, Model& m);
    void resolve_log(const ast::LogBlock& block, const Source& src, Model& m);

    Diagnostics& diag_;
    Resolver& resolver_;

    std::vector<OverrideEntry> overrides_;
    // The settings sources keep their Source and AST alive for diagnostics.
    std::vector<std::unique_ptr<Source>> settings_srcs_;
    std::vector<std::unique_ptr<ast::SettingsFile>> settings_asts_;

    std::map<std::string, Hop> producers_;              // dest path -> hop
    std::map<std::string, std::pair<std::size_t, std::string>> leaf_outputs_;
    std::map<std::string, std::size_t> leaf_by_path_;
    // Every instantiated node, composites included, so that an override that
    // landed on nothing can be told what it nearly hit.
    std::map<std::string, const NodeInfo*> instances_;
    std::vector<std::string> known_setting_paths_;
    bool failed_ = false;
};

}  // namespace se

#endif  // SE_ELABORATE_HPP
