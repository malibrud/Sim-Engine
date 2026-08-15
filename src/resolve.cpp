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
        UnitEval eval(*file.src, diag_);
        if (!eval.eval(ref.unit.get(), out.unit)) return false;
        // §5.1 — `bool` must be declared `(-)`. A scaled or dimensioned truth
        // value is meaningless, and catching it here beats erasing it silently.
        if (out.scalar == "bool" && !(out.unit.dimensionless() && out.unit.scale == 1.0)) {
            diag_.error("SE0410", *file.src, ref.loc,
                        "a `bool` must be declared `(-)`",
                        "declared `(" + out.unit.str() + ")`");
            return false;
        }
        return true;
    }
    out.is_record = true;
    out.record = resolve_record_ref(file, ref.record, *file.src, ref.loc);
    return out.record != nullptr;
}

RecordInfo* Resolver::resolve_record(FileInfo& file, const ast::TypeDef& def,
                                     const std::string& fq, const std::string& cpp_name) {
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
        st.loc = s.loc;
        resolve_type(file, s.type, st.type);
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
            resolve_type(file, f.type, field.type);
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
    const ast::Method* rates_m = info.method(ast::Method::Which::Rates);
    const ast::Method* step_m = info.method(ast::Method::Which::OnStep);

    // §6.12 — typo safety, per kind, in both directions.
    if (!info.composite) {
        if (!info.outputs.empty() && !out_m)
            diag_.error("SE0340", src, def.loc,
                        "node `" + def.name + "` declares outputs but no `output()`",
                        "no `output()`",
                        {note("nothing else could compute them (§6.12)")});
        if (info.has_continuous() && !rates_m)
            diag_.error("SE0341", src, def.loc,
                        "node `" + def.name +
                            "` has a `continuous` state but no `rates()`",
                        "no `rates()`",
                        {note("a continuous state is written through `der.` in "
                              "`rates()` (§6.4)")});
        if (info.has_discrete() && !step_m)
            diag_.error("SE0342", src, def.loc,
                        "node `" + def.name + "` has a `discrete` state but no `on_step()`",
                        "no `on_step()`",
                        {note("a discrete state is written through `next.` in "
                              "`on_step()` (§6.4)")});
    }
    if (rates_m && !info.has_continuous())
        diag_.error("SE0343", src, rates_m->loc,
                    "`rates()` in a node with no `continuous` states", "nothing to write",
                    {note("`rates()` may write only `der.`, and `Der` is empty here")});
    if (out_m && info.outputs.empty())
        diag_.error("SE0344", src, out_m->loc,
                    "`output()` in a node with no outputs", "nothing to write",
                    {note("`output()` may write only `out.`, and `Out` is empty here")});

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
