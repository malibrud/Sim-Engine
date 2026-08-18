// ─────────────────────────────────────────────────────────────────────────────
//  Stage 3 — resolution.  SPECIFICATION.md §1.3, §2, §5, §6.
//
//  Turns a root node name into a `Program`: every `.se` file it transitively
//  needs, located in the root set, parsed, with every name bound and every
//  per-node consistency rule of §6.11 / §6.12 checked.
//
//  Loading is lazy and driven by need, exactly like a C++ include path
//  (§2.1): a package path is a directory path, so `drivetrain.Wheel` is
//  `<root>/drivetrain/Wheel.se` and nothing has to be listed anywhere.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_RESOLVE_HPP
#define SE_RESOLVE_HPP

#include <map>
#include <string>
#include <vector>

#include "diag.hpp"
#include "model.hpp"

namespace se {

// The unit parameters of the node currently being resolved (§6.2a). A member
// rather than a parameter on `resolve_type`, which is reached from settings,
// states, ports, vars and record fields alike. Records are resolved with it
// cleared: a record type is defined independently of any node and is memoized
// across all of them.
class Resolver {
public:
    Resolver(Diagnostics& diag, std::vector<std::string> roots);

    // Resolves `fq` and everything it reaches. `src`/`loc` locate the request
    // — the sim file's `root:` entry — for the diagnostic. Null on failure.
    const NodeInfo* resolve_root(const std::string& fq, const Source& src, Loc loc);

    Program& program() { return program_; }

private:
    // ─── Loading ─────────────────────────────────────────────────────────────
    // Locates `<root>/<pkg>/<name>.se`, reads, parses, binds imports. Reports
    // SE0305 when more than one root answers. Memoised by "pkg/name".
    FileInfo* load_file(const std::string& pkg, const std::string& name,
                        const Source& from, Loc loc);
    void load_package_manifest(const std::string& pkg);
    bool find_in_roots(const std::string& rel, std::string& path, const Source& from,
                       Loc loc);
    bool bind_imports(FileInfo& file);

    // ─── Name binding (§2.4) ─────────────────────────────────────────────────
    // Splits a reference made inside `from` into a package and a member name.
    bool split_reference(const FileInfo& from, const ast::Path& p,
                         std::string& pkg, std::string& name);

    const NodeInfo* resolve_node_ref(const FileInfo& from, const ast::Path& p,
                                     const Source& src, Loc loc);
    const RecordInfo* resolve_record_ref(const FileInfo& from, const ast::Path& p,
                                         const Source& src, Loc loc);
    // A private declaration in `from`, or null.
    const ast::NodeDef* private_node(const FileInfo& from, const std::string& name);
    const ast::TypeDef* private_type(const FileInfo& from, const std::string& name);

    // ─── Building the resolved forms ─────────────────────────────────────────
    NodeInfo* resolve_node(FileInfo& file, const ast::NodeDef& def,
                           const std::string& fq, const std::string& cpp_name);
    RecordInfo* resolve_record(FileInfo& file, const ast::TypeDef& def,
                               const std::string& fq, const std::string& cpp_name);
    bool resolve_type(const FileInfo& file, const ast::TypeRef& ref, Type& out);
    // Validates `units { … }` and returns the names, in declaration order.
    std::vector<std::string> resolve_unit_params(const FileInfo& file,
                                                 const ast::NodeDef& def);
    std::vector<std::string> unit_params_;

    // ─── Checks ──────────────────────────────────────────────────────────────
    void check_node(const NodeInfo& info);
    void check_members(const NodeInfo& info);
    void check_methods(const NodeInfo& info);
    void check_structure(const NodeInfo& info);

    void collect_build(const std::vector<ast::BuildStmt>& stmts);

    Diagnostics& diag_;
    std::vector<std::string> roots_;
    Program program_;

    std::map<std::string, FileInfo*> by_file_key_;   // "pkg/Name"
    std::map<std::string, bool> manifest_loaded_;
    // Memoised by definition pointer, and inserted BEFORE the members are
    // resolved: a composite that reaches itself finds the partial entry and
    // stops, rather than recursing. The resulting infinite model is caught
    // where it becomes real, at instantiation (stage 4).
    std::map<const ast::NodeDef*, NodeInfo*> node_by_def_;
    std::map<const ast::TypeDef*, RecordInfo*> record_by_def_;
};

}  // namespace se

#endif  // SE_RESOLVE_HPP
