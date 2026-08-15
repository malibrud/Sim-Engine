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
    bool coerce(const Value& v, const Type& target, const Source& src, Loc loc,
                const char* what, double& out);

    // ─── Instantiation ───────────────────────────────────────────────────────
    void instantiate(const NodeInfo* def, const std::string& path,
                     std::map<std::string, double> pinned, const Source& src, Loc loc,
                     int depth, Model& m);
    // Evaluates a node's settings: pinned values first, then the remaining
    // defaults in dependency order (§6.2), so overriding `total_mass`
    // recomputes `corner_mass` while overriding `corner_mass` pins it.
    bool settings_of(const NodeInfo* def, const std::map<std::string, double>& pinned,
                     const Source& src, Loc loc, std::vector<SettingValue>& out);
    void apply_overrides(const NodeInfo* def, const std::string& path,
                         std::map<std::string, double>& pinned);
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
    void collect_wires(const NodeInfo* def, const std::string& path, Model& m);
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
