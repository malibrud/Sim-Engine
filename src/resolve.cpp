// ─────────────────────────────────────────────────────────────────────────────
//  Stage 3 — resolution.  SPECIFICATION.md §2, §5, §6.
// ─────────────────────────────────────────────────────────────────────────────
#include "resolve.hpp"

#include <cstdio>
#include <set>

#include "dump.hpp"
#include "parser.hpp"

namespace se {
namespace {

// A member name the generated code already uses for an object in scope of
// every body (§6.11, Appendix B).
bool is_reserved_member(const std::string& n) {
    static const char* const kReserved[] = {"param", "in",  "out", "state", "der",
                                            "next",  "var", "log", "sim"};
    for (const char* r : kReserved)
        if (n == r) return true;
    return false;
}

std::string join_path(const std::string& dir, const std::string& rel) {
    if (dir.empty() || dir == ".") return rel;
    if (dir.back() == '/' || dir.back() == '\\') return dir + rel;
    return dir + "/" + rel;
}

std::string package_dir(const std::string& pkg) {
    std::string s = pkg;
    for (char& c : s)
        if (c == '.') c = '/';
    return s;
}

bool file_exists(const std::string& path) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

// A C++ namespace path for a package: `std.mech` -> `std::mech`.
std::string cpp_namespace(const std::string& pkg) {
    std::string s;
    for (char c : pkg) {
        if (c == '.') s += "::";
        else s += c;
    }
    return s;
}

std::string drop_last(const ast::Path& p) {
    std::string s;
    for (std::size_t i = 0; i + 1 < p.segs.size(); ++i) {
        if (i) s += '.';
        s += p.segs[i];
    }
    return s;
}

}  // namespace

// ─── NodeInfo helpers ────────────────────────────────────────────────────────

const ast::Method* NodeInfo::method(ast::Method::Which w) const {
    if (!def) return nullptr;
    for (const ast::Method& m : def->methods)
        if (m.which == w) return &m;
    return nullptr;
}

const Field* NodeInfo::input(const std::string& n) const {
    for (const Field& f : inputs)
        if (f.name == n) return &f;
    return nullptr;
}

const Field* NodeInfo::output(const std::string& n) const {
    for (const Field& f : outputs)
        if (f.name == n) return &f;
    return nullptr;
}

const NodeInfo::StateInfo* NodeInfo::state(const std::string& n) const {
    for (const StateInfo& s : states)
        if (s.name == n) return &s;
    return nullptr;
}

const NodeInfo::SettingInfo* NodeInfo::setting(const std::string& n) const {
    for (const SettingInfo& s : settings)
        if (s.name == n) return &s;
    return nullptr;
}

bool NodeInfo::has_continuous() const {
    for (const StateInfo& s : states)
        if (s.continuous) return true;
    return false;
}

bool NodeInfo::has_discrete() const {
    for (const StateInfo& s : states)
        if (!s.continuous) return true;
    return false;
}

bool Leaf::has_continuous() const {
    for (const StateSlot& s : states)
        if (s.continuous) return true;
    return false;
}

bool Leaf::has_discrete() const {
    for (const StateSlot& s : states)
        if (!s.continuous) return true;
    return false;
}

// ─── Resolver ────────────────────────────────────────────────────────────────

Resolver::Resolver(Diagnostics& diag, std::vector<std::string> roots)
    : diag_(diag), roots_(std::move(roots)) {
    if (roots_.empty()) roots_.push_back(".");
}

const NodeInfo* Resolver::resolve_root(const std::string& fq, const Source& src, Loc loc) {
    const std::size_t dot = fq.rfind('.');
    if (dot == std::string::npos) {
        diag_.error("SE0304", src, loc,
                    "root node `" + fq + "` is not a qualified name",
                    "expected `package.Node`",
                    {note("the root is named by its package path, which is its "
                          "directory path inside a root (§2.1)")});
        return nullptr;
    }
    const std::string pkg = fq.substr(0, dot);
    const std::string name = fq.substr(dot + 1);

    FileInfo* file = load_file(pkg, name, src, loc);
    if (!file) return nullptr;

    for (const ast::NodeDef& def : file->ast->nodes) {
        if (def.name != name) continue;
        NodeInfo* info = resolve_node(*file, def, fq, cpp_namespace(pkg) + "::" + name);
        return info;
    }
    diag_.error("SE0302", src, loc,
                "`" + fq + "` names no node",
                "no public declaration matching the file name",
                {note("a `.se` file's one public declaration must be named after "
                      "the file (§2.3)", file->path)});
    return nullptr;
}

// ─── Loading ─────────────────────────────────────────────────────────────────

bool Resolver::find_in_roots(const std::string& rel, std::string& path,
                             const Source& from, Loc loc) {
    std::vector<std::string> hits;
    for (const std::string& root : roots_) {
        const std::string p = join_path(root, rel);
        if (file_exists(p)) hits.push_back(p);
    }
    if (hits.empty()) return false;
    if (hits.size() > 1) {
        std::vector<Attachment> att;
        for (const std::string& h : hits) att.push_back(note("found here", h));
        att.push_back(help("remove one root, or mark the shadowing root `override` "
                           "in the manifest so the shadowing is a declared act (§2.1)"));
        diag_.error("SE0305", from, loc,
                    "`" + rel + "` resolves in more than one root",
                    "ambiguous", std::move(att));
    }
    path = hits.front();
    return true;
}

FileInfo* Resolver::load_file(const std::string& pkg, const std::string& name,
                              const Source& from, Loc loc) {
    const std::string key = pkg + "/" + name;
    auto it = by_file_key_.find(key);
    if (it != by_file_key_.end()) return it->second;

    const std::string rel = package_dir(pkg) + "/" + name + ".se";
    std::string path;
    if (!find_in_roots(rel, path, from, loc)) {
        std::vector<Attachment> att;
        att.push_back(note("looked for `" + rel + "` in each root"));
        for (const std::string& root : roots_) att.push_back(note("root", root));
        diag_.error("SE0304", from, loc,
                    "cannot find `" + pkg + "." + name + "`",
                    "no such file in any root", std::move(att));
        by_file_key_[key] = nullptr;
        return nullptr;
    }

    auto file = std::make_unique<FileInfo>();
    file->path = path;
    file->src = std::make_unique<Source>();
    std::string error;
    if (!Source::load(path, *file->src, error)) {
        diag_.error("SE0304", from, loc, "cannot read `" + path + "`: " + error, "");
        by_file_key_[key] = nullptr;
        return nullptr;
    }

    Parser parser(*file->src, diag_);
    file->ast = parser.parse_model_file();
    file->package = file->ast->package.str();
    file->public_name = name;

    // §2.2 — the declaration is redundant with the location and checked
    // against it. Location remains the identity.
    if (file->ast->has_package && file->package != pkg) {
        diag_.error("SE0301", *file->src, file->ast->package.loc,
                    "`package " + file->package + "` disagrees with this file's location",
                    "declared here",
                    {note("the file resolved as package `" + pkg + "`", path),
                     help("the declared path must equal the directory path "
                          "relative to the root that resolved it (§2.2)")});
    }

    FileInfo* raw = file.get();
    program_.files.push_back(std::move(file));
    by_file_key_[key] = raw;

    bind_imports(*raw);
    collect_build(raw->ast->build);
    for (const ast::NodeDef& def : raw->ast->nodes) collect_build(def.build);
    load_package_manifest(pkg);
    return raw;
}

void Resolver::load_package_manifest(const std::string& pkg) {
    if (manifest_loaded_.count(pkg)) return;
    manifest_loaded_[pkg] = true;

    const std::string rel = package_dir(pkg) + "/package.se";
    std::string path;
    for (const std::string& root : roots_) {
        const std::string p = join_path(root, rel);
        if (file_exists(p)) {
            path = p;
            break;
        }
    }
    if (path.empty()) return;

    auto file = std::make_unique<FileInfo>();
    file->path = path;
    file->src = std::make_unique<Source>();
    std::string error;
    if (!Source::load(path, *file->src, error)) return;

    Parser parser(*file->src, diag_);
    file->ast = parser.parse_model_file();
    file->package = file->ast->package.str();
    file->public_name = "package";
    collect_build(file->ast->build);
    program_.files.push_back(std::move(file));
}

bool Resolver::bind_imports(FileInfo& file) {
    bool ok = true;
    std::set<std::string> local;
    for (const ast::NodeDef& d : file.ast->nodes) local.insert(d.name);
    for (const ast::TypeDef& d : file.ast->types) local.insert(d.name);

    for (const ast::UseDecl& u : file.ast->uses) {
        if (u.path.segs.empty()) continue;
        const std::string last = u.path.segs.back();

        std::vector<Attachment> att;
        const bool collides_local = local.count(last) != 0;
        const bool collides_import =
            file.symbol_imports.count(last) || file.alias_imports.count(last);
        if (collides_local || collides_import) {
            att.push_back(note(collides_local
                                   ? "a declaration in this file is already named `" +
                                         last + "`"
                                   : "another `use` already binds `" + last + "`"));
            att.push_back(help("there are no wildcard imports and no renaming, so "
                               "spell this reference fully qualified instead (§2.4)"));
            diag_.error("SE0303", *file.src, u.loc,
                        "imported name `" + last + "` collides", "already bound",
                        std::move(att));
            ok = false;
            continue;
        }

        // A `use` whose last segment starts with an upper-case letter is read
        // as a per-symbol import and anything else as a package alias. That is
        // a convention, not a rule: both forms are recorded, and lookup tries
        // the symbol table first, so a lower-case node name still resolves.
        file.symbol_imports[last] = u.path.str();
        file.alias_imports[last] = u.path.str();
    }
    return ok;
}

void Resolver::collect_build(const std::vector<ast::BuildStmt>& stmts) {
    for (const ast::BuildStmt& s : stmts) {
        bool dup = false;
        for (const ast::BuildStmt& have : program_.build) {
            if (have.primitive == s.primitive && have.argument == s.argument &&
                have.when == s.when) {
                dup = true;
                break;
            }
        }
        if (!dup) program_.build.push_back(s);
    }
}

// ─── Name binding ────────────────────────────────────────────────────────────

const ast::NodeDef* Resolver::private_node(const FileInfo& from, const std::string& name) {
    if (name == from.public_name) return nullptr;
    for (const ast::NodeDef& d : from.ast->nodes)
        if (d.name == name) return &d;
    return nullptr;
}

const ast::TypeDef* Resolver::private_type(const FileInfo& from, const std::string& name) {
    if (name == from.public_name) return nullptr;
    for (const ast::TypeDef& d : from.ast->types)
        if (d.name == name) return &d;
    return nullptr;
}

bool Resolver::split_reference(const FileInfo& from, const ast::Path& p,
                               std::string& pkg, std::string& name) {
    if (p.segs.empty()) return false;
    name = p.segs.back();

    if (p.segs.size() == 1) {
        auto sym = from.symbol_imports.find(name);
        if (sym != from.symbol_imports.end()) {
            const std::string& fq = sym->second;
            const std::size_t dot = fq.rfind('.');
            if (dot == std::string::npos) return false;
            pkg = fq.substr(0, dot);
            name = fq.substr(dot + 1);
            return true;
        }
        pkg = from.package;   // same package (§2.4: no `use` needed)
        return true;
    }

    // `alias.Member` — resolve the head against the alias imports, else read
    // the whole thing as fully qualified.
    auto alias = from.alias_imports.find(p.segs.front());
    if (alias != from.alias_imports.end()) {
        pkg = alias->second;
        for (std::size_t i = 1; i + 1 < p.segs.size(); ++i) pkg += "." + p.segs[i];
        return true;
    }
    pkg = drop_last(p);
    return true;
}

const NodeInfo* Resolver::resolve_node_ref(const FileInfo& from, const ast::Path& p,
                                           const Source& src, Loc loc) {
    if (p.segs.size() == 1) {
        if (const ast::NodeDef* priv = private_node(from, p.segs[0])) {
            return resolve_node(const_cast<FileInfo&>(from), *priv,
                                from.package + "." + from.public_name + "#" + priv->name,
                                cpp_namespace(from.package) +
                                    "::" + from.public_name + "_priv::" + priv->name);
        }
    }
    std::string pkg, name;
    if (!split_reference(from, p, pkg, name)) return nullptr;

    FileInfo* file = load_file(pkg, name, src, loc);
    if (!file) return nullptr;
    for (const ast::NodeDef& def : file->ast->nodes) {
        if (def.name != name) continue;
        return resolve_node(*file, def, pkg + "." + name,
                            cpp_namespace(pkg) + "::" + name);
    }
    diag_.error("SE0304", src, loc, "`" + p.str() + "` names no node",
                "unresolved",
                {note("this file declares no public `node " + name + "`", file->path)});
    return nullptr;
}

const RecordInfo* Resolver::resolve_record_ref(const FileInfo& from, const ast::Path& p,
                                               const Source& src, Loc loc) {
    if (p.segs.size() == 1) {
        if (const ast::TypeDef* priv = private_type(from, p.segs[0])) {
            return resolve_record(const_cast<FileInfo&>(from), *priv,
                                  from.package + "." + from.public_name + "#" + priv->name,
                                  cpp_namespace(from.package) +
                                      "::" + from.public_name + "_priv::" + priv->name);
        }
    }
    std::string pkg, name;
    if (!split_reference(from, p, pkg, name)) return nullptr;

    FileInfo* file = load_file(pkg, name, src, loc);
    if (!file) return nullptr;
    for (const ast::TypeDef& def : file->ast->types) {
        if (def.name != name) continue;
        return resolve_record(*file, def, pkg + "." + name,
                              cpp_namespace(pkg) + "::" + name);
    }
    diag_.error("SE0304", src, loc, "`" + p.str() + "` names no type",
                "unresolved",
                {note("this file declares no public `type " + name + "`", file->path)});
    return nullptr;
}

// ─── Building the resolved forms ─────────────────────────────────────────────

bool Resolver::resolve_type(const FileInfo& file, const ast::TypeRef& ref, Type& out) {
    out.loc = ref.loc;
    if (ref.is_scalar) {
        out.is_record = false;
        out.scalar = ref.scalar.empty() ? "double" : ref.scalar;
        UnitEval eval(*file.src, diag_, unit_params_);
        if (!eval.eval(ref.unit.get(), out.unit)) return false;
        // §4.5 — radians are the only DECLARABLE angle unit. A literal suffix
        // may still be `(deg)`; it converts here and is gone. This is the one
        // place the restriction belongs, because a declared unit is what a body
        // reads through an accessor.
        if (out.unit.nonradian_angle) {
            // Point at the unit expression, not the declaration, so the caret
            // spans the offending symbol the way `SE0414` does.
            diag_.error("SE0415", *file.src, ref.unit ? ref.unit->loc : ref.loc,
                        "`(" + out.unit.str() + ")` may not be a declared unit",
                        "declare this in radians",
                        {note("units are erased in bodies (§4.4), so `sin(state.theta)` "
                              "on a degree-valued accessor is silently wrong, and no "
                              "dimensional system can catch it"),
                         help("declare the radian unit and convert at the literal — "
                              "`theta (rad): double = 90 (deg);`")});
            return false;
        }
        // §5.1 — `bool` needs no rule here any more: it declares zero unit
        // parameters, so the parser's arity check has already rejected any
        // attempt to give it one.
        return true;
    }
    out.is_record = true;
    // §5.2 — evaluate the arguments in the REFERRING scope, before resolving the
    // record. `Vec3(A*B)` inside a node with `units {A; B;}` yields parametric
    // arguments, which are bound again when that node is instantiated; the
    // record's own parameters are a different scope entirely.
    {
        UnitEval eval(*file.src, diag_, unit_params_);
        for (const ast::UnitPtr& u : ref.unit_args) {
            Unit evaluated;
            if (!eval.eval(u.get(), evaluated)) return false;
            out.unit_args.push_back(evaluated);
        }
    }
    out.record = resolve_record_ref(file, ref.record, *file.src, ref.loc);
    if (!out.record) return false;

    const std::size_t want = out.record->unit_params.size();
    const std::size_t got = out.unit_args.size();
    if (want != got) {
        std::vector<Attachment> att;
        if (want == 0) {
            att.push_back(note("`" + out.record->fq + "` declares no `units` section, so "
                               "its field units are fixed at its declaration"));
        } else {
            std::string names;
            for (std::size_t i = 0; i < want; ++i) {
                if (i) names += ", ";
                names += "`" + out.record->unit_params[i] + "`";
            }
            att.push_back(note("`" + out.record->fq + "` declares " + std::to_string(want) +
                               (want == 1 ? " unit parameter: " : " unit parameters: ") +
                               names));
            att.push_back(help("write `" + ref.record.str() + "(" +
                               (want == 1 ? "unit" : "unit, ...") +
                               ")` — a record binds its units at every reference, since "
                               "it is never instantiated (§5.2)"));
        }
        diag_.error("SE0313", *file.src, got ? ref.unit_args_loc : ref.loc,
                    "`" + out.record->fq + "` takes " + std::to_string(want) +
                        " unit argument" + (want == 1 ? "" : "s") + ", " +
                        std::to_string(got) + " given",
                    std::to_string(got) + " given", std::move(att));
        return false;
    }
    return true;
}

RecordInfo* Resolver::resolve_record(FileInfo& file, const ast::TypeDef& def,
                                     const std::string& fq, const std::string& cpp_name) {
    // A record is defined independently of any node and memoized across all of
    // them, so it must not see the unit parameters of whichever node happened
    // to reference it first. It resolves under its OWN parameters instead
    // (§5.2), which `unit_bind` substitutes at each reference.
    std::vector<std::string> saved;
    saved.swap(unit_params_);
    struct Restore {
        std::vector<std::string>& live;
        std::vector<std::string>& saved;
        ~Restore() { live.swap(saved); }
    } restore{unit_params_, saved};

    auto memo = record_by_def_.find(&def);
    if (memo != record_by_def_.end()) {
        RecordInfo* info = memo->second;
        if (info->resolving) {
            diag_.error("SE0310", *file.src, def.loc,
                        "cycle in record field types through `" + fq + "`",
                        "a record cannot contain itself",
                        {help("a record travels a wire as one value, so a cyclic "
                              "field graph has no finite layout (§5.2)")});
            info->resolving = false;
        }
        return info;
    }

    auto owned = std::make_unique<RecordInfo>();
    RecordInfo* info = owned.get();
    info->fq = fq;
    info->cpp_name = cpp_name;
    info->def = &def;
    info->file = &file;
    info->unit_params = resolve_unit_params(file, def.sec_units, def.unit_params);
    unit_params_ = info->unit_params;
    info->resolving = true;
    program_.records[fq] = std::move(owned);
    record_by_def_[&def] = info;

    for (const ast::FieldDecl& f : def.fields) {
        Field field;
        field.name = f.name;
        field.loc = f.loc;
        resolve_type(file, f.type, field.type);
        info->fields.push_back(std::move(field));
    }
    info->resolving = false;
    info->resolved = true;
    program_.record_order.push_back(info);   // post-order: contained first
    return info;
}

// §6.2a — leaf-only for nodes. A parametric composite would need unit
// flow-down alongside setting flow-down; this is a restriction to relax, not a
// rule, so no model valid today would break when it lifts. Records have no such
// restriction: they have no children to flow anything down to.
std::vector<std::string> Resolver::resolve_unit_params(const FileInfo& file,
                                                       const ast::NodeDef& def) {
    if (def.sec_units.present && def.is_composite()) {
        diag_.error("SE0418", *file.src, def.sec_units.loc,
                    "a composite may not declare unit parameters",
                    "only a leaf node may be unit-parametric",
                    {note("binding a composite's parameters would have to flow down "
                          "to its children, which is a separate mechanism (§6.2a)"),
                     help("put the unit parameters on the leaf that declares the ports")});
        return {};
    }
    return resolve_unit_params(file, def.sec_units, def.unit_params);
}

std::vector<std::string> Resolver::resolve_unit_params(
    const FileInfo& file, const ast::SectionMark& sec,
    const std::vector<ast::UnitParamDecl>& params) {
    std::vector<std::string> out;
    if (!sec.present) return out;

    for (const ast::UnitParamDecl& p : params) {
        // A parameter shadows Appendix A inside this node (§6.2a), so letting
        // one be called `m` would silently retype every length in the file.
        Unit clash;
        if (lookup_unit_symbol(p.name, clash)) {
            diag_.error("SE0416", *file.src, p.loc,
                        "unit parameter `" + p.name + "` is already a unit symbol",
                        "shadows Appendix A",
                        {note("a unit parameter shadows real units inside its node, so "
                              "`(" + p.name + ")` would stop meaning what it says")});
            continue;
        }
        bool dup = false;
        for (const std::string& seen : out) dup = dup || seen == p.name;
        if (dup) {
            diag_.error("SE0417", *file.src, p.loc,
                        "duplicate unit parameter `" + p.name + "`", "declared twice");
            continue;
        }
        out.push_back(p.name);
    }
    return out;
}

NodeInfo* Resolver::resolve_node(FileInfo& file, const ast::NodeDef& def,
                                 const std::string& fq, const std::string& cpp_name) {
    auto memo = node_by_def_.find(&def);
    if (memo != node_by_def_.end()) return memo->second;

    auto owned = std::make_unique<NodeInfo>();
    NodeInfo* info = owned.get();
    info->fq = fq;
    info->cpp_name = cpp_name;
    info->def = &def;
    info->file = &file;
    info->composite = def.is_composite();
    program_.nodes[fq] = std::move(owned);
    node_by_def_[&def] = info;

    // §6.2a — the unit parameters must be known before any declaration in this
    // node is resolved, since every port unit may reference them.
    info->unit_params = resolve_unit_params(file, def);
    unit_params_ = info->unit_params;
    struct ClearParams {
        std::vector<std::string>& v;
        ~ClearParams() { v.clear(); }
    } clear_params{unit_params_};

    // §6.1 — one node concept, kind inferred, never both.
    if (def.is_composite() && def.has_code()) {
        diag_.error("SE0320", *file.src, def.loc,
                    "node `" + def.name + "` has both code and a `structure` block",
                    "leaf or composite, never both",
                    {note("a composite just groups and aggregates: its state is the "
                          "union of its descendants' and it leaves no runtime trace "
                          "at all (§6.1, §15.2)"),
                     help("move the code into a child leaf node")});
    }

    for (const ast::SettingDecl& s : def.settings) {
        NodeInfo::SettingInfo si;
        si.name = s.name;
        si.loc = s.loc;
        si.default_value = s.default_value.get();
        resolve_type(file, s.type, si.type);
        // §6.2 — `rate` is an ordinary setting the scheduler happens to read.
        if (si.name == "rate") {
            Unit hz;
            lookup_unit_symbol("Hz", hz);
            if (si.type.is_record || si.type.scalar != "double" ||
                !compatible(si.type.unit, hz)) {
                diag_.error("SE0422", *file.src, s.loc,
                            "the reserved setting `rate` must be `(Hz): double`",
                            "declared otherwise",
                            {note("the scheduler consumes it as an integer decimation "
                                  "of the base step and it produces no runtime member "
                                  "(§6.2, §9.2)")});
            }
        }
        info->settings.push_back(std::move(si));
    }

    Unit seconds;
    lookup_unit_symbol("s", seconds);

    for (const ast::StateDecl& s : def.states) {
        NodeInfo::StateInfo st;
        st.name = s.name;
        st.continuous = s.is_continuous;
        st.initial = s.initial.get();
        st.extent = s.extent.get();
        st.extent_loc = s.extent_loc;
        st.loc = s.loc;
        resolve_type(file, s.type, st.type);
        // §6.4a — an array state is `double`, both kinds. That is what lets
        // the two share one flat-block lowering: a continuous array is a run of
        // slots in `x`, and a discrete one is a run in `dis_arr`, which exists
        // precisely because a run-time-sized member cannot live in `dis`
        // (§15.5). `int`/`bool` states remain available, unextended.
        if (st.extent && (st.type.is_record || st.type.scalar != "double")) {
            diag_.error("SE0433", *file.src, s.extent_loc,
                        "an array-shaped state must be `double`",
                        "declared `" + std::string(st.type.is_record ? "record"
                                                                    : st.type.scalar) +
                            "`",
                        {note("both blocks an extent can land in are flat `double` "
                              "arrays (§15.5)"),
                         help("drop the extent, or make the element type `double`")});
        }
        if (st.continuous && (st.type.is_record || st.type.scalar != "double")) {
            diag_.error("SE0431", *file.src, s.loc,
                        "a `continuous` state must be `double`",
                        "declared `" + std::string(st.type.is_record ? "record"
                                                                     : st.type.scalar) +
                            "`",
                        {note("the solver's ABI is one flat `double` array (§15.5)"),
                         help("make it `discrete` if it is not integrated")});
        }
        // §4.3 — the derivative unit is derived, never declared.
        st.der_unit = unit_div(st.type.unit, seconds);
        info->states.push_back(std::move(st));
    }

    struct {
        const std::vector<ast::FieldDecl>* from;
        std::vector<Field>* to;
    } const groups[] = {{&def.inputs, &info->inputs},
                        {&def.outputs, &info->outputs},
                        {&def.vars, &info->vars}};
    for (const auto& g : groups) {
        for (const ast::FieldDecl& f : *g.from) {
            Field field;
            field.name = f.name;
            field.loc = f.loc;
            field.extent = f.extent.get();
            field.extent_loc = f.extent_loc;
            resolve_type(file, f.type, field.type);
            // §6.5 — a var carries an extent on the same terms as a state,
            // and the same element rule with it. The parser has already refused
            // one on a port or a record field (SE0234), so only `vars` reaches
            // here with `extent` set.
            if (field.extent && (field.type.is_record || field.type.scalar != "double")) {
                diag_.error("SE0433", *file.src, f.extent_loc,
                            "an array-shaped var must be `double`",
                            "declared `" + std::string(field.type.is_record
                                                           ? "record"
                                                           : field.type.scalar) +
                                "`",
                            {note("an array var holds the per-element coefficients an "
                                  "array state is filtered by (§6.5)"),
                             help("drop the extent, or make the element type `double`")});
            }
            g.to->push_back(std::move(field));
        }
    }

    for (const ast::Instance& inst : def.structure.instances) {
        const NodeInfo* child =
            resolve_node_ref(file, inst.definition, *file.src, inst.definition.loc);
        if (child) info->children[inst.name] = child;
    }

    check_node(*info);
    return info;
}

// ─── Checks ──────────────────────────────────────────────────────────────────

void Resolver::check_node(const NodeInfo& info) {
    check_members(info);
    check_methods(info);
    check_structure(info);
}

void Resolver::check_members(const NodeInfo& info) {
    const Source& src = *info.file->src;
    // §6.11 — one namespace for settings, inputs, outputs, vars, natives and
    // instance names. States sit in a separate namespace FROM OUTPUTS ONLY.
    std::map<std::string, Loc> seen;
    auto claim = [&](const std::string& name, Loc loc, const char* what) {
        if (is_reserved_member(name)) {
            diag_.error("SE0331", src, loc,
                        "member `" + name + "` shadows a generated accessor",
                        std::string(what) + " named `" + name + "`",
                        {note("the generated code puts an object called `" + name +
                              "` in scope of every body, because the compiler never "
                              "parses C++ and so cannot rename yours (I1, §15.1)")});
            return;
        }
        auto it = seen.find(name);
        if (it != seen.end()) {
            diag_.error("SE0330", src, loc,
                        "duplicate member name `" + name + "`",
                        std::string(what) + " named `" + name + "`",
                        {note("first declared here",
                              info.file->path + ":" + std::to_string(it->second.line))});
            return;
        }
        seen[name] = loc;
    };

    for (const auto& s : info.settings) claim(s.name, s.loc, "setting");
    for (const Field& f : info.inputs) claim(f.name, f.loc, "input");
    for (const Field& f : info.vars) claim(f.name, f.loc, "var");
    for (const ast::NativeDecl& n : info.def->natives) claim(n.name, n.loc, "native");
    for (const auto& inst : info.def->structure.instances)
        claim(inst.name, inst.loc, "instance");
    // Outputs and states share a name legitimately — `velocity` as both an
    // integrated state and a published output is the commonest node shape in
    // this domain — so each is checked against the shared set but not against
    // the other.
    std::map<std::string, Loc> outs;
    for (const Field& f : info.outputs) {
        claim(f.name, f.loc, "output");
        outs[f.name] = f.loc;
    }
    for (const auto& s : info.states) {
        auto it = outs.find(s.name);
        if (it != outs.end()) seen.erase(s.name);   // legal pairing
        claim(s.name, s.loc, "state");
        if (it != outs.end()) seen[s.name] = it->second;
    }
}

void Resolver::check_methods(const NodeInfo& info) {
    const Source& src = *info.file->src;
    const ast::NodeDef& def = *info.def;

    const ast::Method* out_m = info.method(ast::Method::Which::Output);
    const ast::Method* der_m = info.method(ast::Method::Which::Derivative);
    const ast::Method* next_m = info.method(ast::Method::Which::Next);

    // §6.12 — typo safety, per kind, in both directions.
    if (!info.composite) {
        if (!info.outputs.empty() && !out_m)
            diag_.error("SE0340", src, def.loc,
                        "node `" + def.name + "` declares outputs but no `output()`",
                        "no `output()`",
                        {note("nothing else could compute them (§6.12)")});
        if (info.has_continuous() && !der_m)
            diag_.error("SE0341", src, def.loc,
                        "node `" + def.name +
                            "` has a `continuous` state but no `derivative()`",
                        "no `derivative()`",
                        {note("a continuous state is written through `der.` in "
                              "`derivative()` (§6.4)")});
        if (info.has_discrete() && !next_m)
            diag_.error("SE0342", src, def.loc,
                        "node `" + def.name + "` has a `discrete` state but no `next()`",
                        "no `next()`",
                        {note("a discrete state is written through `next.` in "
                              "`next()` (§6.4)")});
    }
    if (der_m && !info.has_continuous())
        diag_.error("SE0343", src, der_m->loc,
                    "`derivative()` in a node with no `continuous` states",
                    "nothing to write",
                    {note("`derivative()` may write only `der.`, and `Der` is empty here")});
    if (out_m && info.outputs.empty())
        diag_.error("SE0344", src, out_m->loc,
                    "`output()` in a node with no outputs", "nothing to write",
                    {note("`output()` may write only `out.`, and `Out` is empty here")});
    // §6.12 — the converse for `next()`. This check only became possible once
    // `on_step()` stopped carrying the discrete equation: while one method did
    // both jobs, a body with no discrete states could still be doing var work.
    if (next_m && !info.has_discrete())
        diag_.error("SE0345", src, next_m->loc,
                    "`next()` in a node with no `discrete` states", "nothing to write",
                    {note("`next()` may write only `next.`, and `Store` is empty here"),
                     help("per-step work that is not a state transition belongs in "
                          "`on_step()` (§8.1)")});

    // §8.3 — a method's parameter list is exactly the set of inputs it reads.
    // `init()` takes none (SE0241) and `final(ctx)`'s one parameter is the run
    // context, not an input (§10.3), so neither is checked here.
    for (const ast::Method& m : def.methods) {
        if (m.which == ast::Method::Which::Init || m.which == ast::Method::Which::Final)
            continue;
        std::set<std::string> seen;
        for (std::size_t i = 0; i < m.params.size(); ++i) {
            const std::string& p = m.params[i];
            const Loc loc = i < m.param_locs.size() ? m.param_locs[i] : m.loc;
            if (!seen.insert(p).second) {
                diag_.error("SE0351", src, loc,
                            "duplicate parameter `" + p + "` on `" +
                                std::string(m.name()) + "()`",
                            "listed twice");
                continue;
            }
            if (!info.input(p)) {
                std::vector<Attachment> att;
                att.push_back(note("the list names inputs, bare — it is the set of "
                                   "inputs this method reads (§8.3)"));
                if (!info.inputs.empty()) {
                    std::string have = "declared inputs: ";
                    for (std::size_t k = 0; k < info.inputs.size(); ++k) {
                        if (k) have += ", ";
                        have += "`" + info.inputs[k].name + "`";
                    }
                    att.push_back(help(have));
                }
                diag_.error("SE0350", src, loc,
                            "`" + p + "` is not a declared input of `" + def.name + "`",
                            "not an input", std::move(att));
            }
        }
    }
}

void Resolver::check_structure(const NodeInfo& info) {
    if (!info.composite) return;
    const Source& src = *info.file->src;
    for (const ast::Wire& w : info.def->structure.wires) {
        auto check = [&](const ast::Endpoint& e) {
            // §5.3 — the grammar admits a field-level endpoint so the eventual
            // decision is not a grammar change; the semantics reject it today.
            const std::size_t after_instance = e.is_self ? e.segs.size() : e.segs.size() - 1;
            if (after_instance > 1)
                diag_.error("SE0311", src, e.loc,
                            "field-level wire endpoints are not supported",
                            "`" + e.str() + "`",
                            {note("a wire carries the whole record (§5.3)"),
                             help("wire the record and read the field in the body")});
        };
        check(w.source);
        for (const ast::Endpoint& d : w.dests) check(d);
    }
}

}  // namespace se
