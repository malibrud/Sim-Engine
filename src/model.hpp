// ─────────────────────────────────────────────────────────────────────────────
//  The intermediate model — what stages 3, 4 and 5 build and stage 6 emits.
//
//  Two layers, matching the stage boundaries of SPECIFICATION.md §1.3:
//
//    Program   stage 3.  Definitions, resolved. Still hierarchical, still
//              symbolic: this is the .se files with every name bound.
//    Model     stages 4-5.  The FLAT runtime model. Composites are gone (§15.2),
//              settings are numbers, wires are producer/consumer pairs, states
//              own slots, and the leaves are in topological order.
//
//  Nothing here holds a unit expression: units are evaluated once, at the
//  boundary between the two layers, and every `Unit` below is the evaluated
//  form. What survives into stage 6 is a number plus a spelling (§15.6).
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_MODEL_HPP
#define SE_MODEL_HPP

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "ast.hpp"
#include "diag.hpp"
#include "units.hpp"

namespace se {

// ═════════════════════════════════════════════════════════════════════════════
//  Stage 3 — the resolved program
// ═════════════════════════════════════════════════════════════════════════════

struct RecordInfo;
struct NodeInfo;

// A `.se` file that has been located, read, parsed and had its imports bound.
struct FileInfo {
    std::string path;                  // as opened, for diagnostics and `#line`
    std::string package;               // dotted
    std::unique_ptr<Source> src;
    std::unique_ptr<ast::ModelFile> ast;

    // §2.4. Per-symbol imports bind a bare name to a fully-qualified one;
    // alias imports bind a last segment to a package path.
    std::map<std::string, std::string> symbol_imports;
    std::map<std::string, std::string> alias_imports;

    // The public declaration's name — also the namespace under which this
    // file's private declarations are emitted, which is what keeps two
    // same-named private helpers in one package from colliding in C++.
    std::string public_name;
};

// A resolved scalar-or-record type at a port, field, var or state.
struct Type {
    bool is_record = false;
    const RecordInfo* record = nullptr;   // record
    Unit unit;                            // scalar — evaluated
    std::string scalar = "double";        // scalar — the C++ spelling
    // §5.2 — a parametric record binds its units at the REFERENCE, so the
    // arguments belong to this use of the type, not to the type. They may
    // themselves be parametric (`Vec3(A*B)` inside a node with `units {A; B;}`),
    // in which case they are bound again when that node is instantiated.
    std::vector<Unit> unit_args;
    Loc loc;

    const char* cpp() const { return scalar.c_str(); }
};

struct Field {
    std::string name;
    Type type;
    // §6.4a — `[expr]` on a `var`. Null everywhere else: a port and a
    // record field are both in the signal block, and a run-time-sized member
    // would destroy the `offsetof` property it rests on (§15.5).
    const ast::Expr* extent = nullptr;
    Loc extent_loc;
    Loc loc;
};

struct RecordInfo {
    std::string fq;                    // "drivetrain.WheelState"
    std::string cpp_name;              // "drivetrain::WheelState"
    const ast::TypeDef* def = nullptr;
    const FileInfo* file = nullptr;
    // §5.2 — the record's own unit parameters, in declaration order. A field
    // declared `(U)` carries `param_exp` indexed against this list, exactly as a
    // node's ports do (§6.2a); `unit_bind` substitutes at each reference.
    std::vector<std::string> unit_params;
    std::vector<Field> fields;
    bool resolved = false;             // guards the §5.2 cycle check
    bool resolving = false;
};

// §5.2/§6.2a — a parametric type binds its units at the site that references
// it, so a declared type only becomes concrete once that site's arguments are
// substituted in. A scalar binds its own unit; a record binds the arguments it
// passes on, leaving its fields alone — those live in the record's own scope.
inline Type bind_type(const Type& t, const std::vector<Unit>& binding) {
    if (binding.empty()) return t;
    Type r = t;
    if (t.is_record) {
        for (Unit& u : r.unit_args) u = unit_bind(u, binding);
    } else {
        r.unit = unit_bind(t.unit, binding);
    }
    return r;
}

// The type of field `f` as seen through the reference `rec`. Descending a
// nested record threads that record's already-substituted arguments down, which
// is what makes `type Pose { p: Vec3(U); }` work.
inline Type field_type(const Type& rec, const Field& f) {
    return bind_type(f.type, rec.unit_args);
}

struct NodeInfo {
    std::string fq;                    // "drivetrain.Wheel"
    std::string cpp_name;              // "drivetrain::Wheel"
    const ast::NodeDef* def = nullptr;
    const FileInfo* file = nullptr;
    bool composite = false;

    // §6.2a — unit parameters in declaration order. This is the index space
    // `Unit::param_exp` is keyed by, and the order an instantiation's bindings
    // are resolved against. Empty for every non-parametric node.
    std::vector<std::string> unit_params;

    std::vector<Field> inputs;
    std::vector<Field> outputs;
    std::vector<Field> vars;

    struct SettingInfo {
        std::string name;
        Type type;
        const ast::Expr* default_value = nullptr;   // null when required
        Loc loc;
    };
    std::vector<SettingInfo> settings;

    struct StateInfo {
        std::string name;
        bool continuous = false;
        Type type;
        Unit der_unit;                 // §4.3 — derived, never declared
        const ast::Expr* initial = nullptr;
        // §6.4a - the extent, unevaluated. It is a §7 expression that does
        // not settle until configuration, so nothing here is a size.
        const ast::Expr* extent = nullptr;
        Loc extent_loc;
        Loc loc;
    };
    std::vector<StateInfo> states;

    // Instances, by name, resolved to their definitions (composites only).
    std::map<std::string, const NodeInfo*> children;

    const ast::Method* method(ast::Method::Which w) const;
    const Field* input(const std::string& n) const;
    const Field* output(const std::string& n) const;
    const StateInfo* state(const std::string& n) const;
    const SettingInfo* setting(const std::string& n) const;
    bool has_continuous() const;
    bool has_discrete() const;
};

struct Program {
    std::vector<std::unique_ptr<FileInfo>> files;
    std::map<std::string, std::unique_ptr<RecordInfo>> records;   // by fq
    std::map<std::string, std::unique_ptr<NodeInfo>> nodes;       // by fq
    // Records in a valid emission order: a record precedes every record that
    // contains it.
    std::vector<const RecordInfo*> record_order;
    // Build primitives, aggregated and de-duplicated in first-appearance order.
    std::vector<ast::BuildStmt> build;
};

// ═════════════════════════════════════════════════════════════════════════════
//  Stages 4-5 — the flat model
// ═════════════════════════════════════════════════════════════════════════════

// One elaborated setting value. `value` is already in `unit`, post-conversion,
// which is exactly what §15.6 promises the manifest reader.
struct SettingValue {
    std::string name;
    Unit unit;
    std::string scalar;
    double value = 0.0;
};

// One piece of an emitted §7 expression (§15.5). A setting reference cannot be
// rendered when the expression is built: it needs the owning node's C++
// identifier, and those are assigned only after the whole hierarchy is known,
// behind a collision-dedup pass. So a reference stays symbolic — owner path plus
// setting name — and stage 6 renders it once identifiers exist.
//
// Two kinds suffice because §6.2 and §6.9.1 between them guarantee that a
// reference is only ever to a setting of the node being elaborated or of its
// parent: `param.*` reaches the same node, and a flow-down binding is evaluated
// in the parent's scope. There is no third case to represent.
struct ExprTok {
    enum class Kind { Text, Param };
    Kind kind = Kind::Text;
    std::string text;    // Text: literal C++. Param: the setting's name.
    std::string owner;   // Param: model path of the node holding it, "" at root.
};

using ExprCode = std::vector<ExprTok>;

// §6.2b — one assignment in the configuration program. The program is the whole
// settings graph flattened into dependency order at elaboration, so the runtime
// performs no sorting and holds no graph: `resolve_settings()` is a straight
// sequence of these (§15.5).
//
// Composites appear here as well as leaves, which is the one place a composite
// leaves a runtime trace: its settings are the inputs to its own flow-down
// expressions, so overriding `front.left.mass` has something to flow through.
struct SettingSlot {
    std::string path;        // root-relative, §13.2: "fl.whl.inertia"
    std::string owner;       // the node holding it: "fl.whl" ("" at the root)
    std::string name;        // the setting's own name: "inertia"
    std::string pin;         // the pinned-flag member guarding it
    Unit unit;
    std::string scalar;      // "double" / "int"
    double value = 0.0;      // what elaboration computed — the dump's starting point
    ExprCode expr;           // emitted right-hand side, references unresolved
    bool constant = true;    // `expr` is a literal; nothing upstream can move it
    bool rate = false;       // reserved `rate` (§9.2): drives decimation, no param member
    bool structural = false; // fixed at elaboration (§6.2); refused on load
};

// §6.4a — an extent on a state or a var. The expression is not evaluated
// until configuration (§6.2b step 4), so what reaches stage 6 is emittable
// code, not a size: `dflt` is only what the elaborated defaults happen to give
// it, kept for diagnostics and for the manifest's default column.
struct Extent {
    bool present = false;
    ExprCode expr;                     // emitted into Sim::configure()
    std::string text;                  // as written, for the manifest (§15.6)
    double dflt = 0.0;
};

struct StateSlot {
    std::string name;
    bool continuous = false;
    Unit unit;
    Unit der_unit;
    std::string scalar;
    double initial = 0.0;
    std::size_t slot = 0;              // index into Sim::x — continuous only
    // §6.4a — when the model has any array-shaped state, EVERY state's
    // position is settled at configuration rather than here, and `slot` gives
    // way to `off`: an index into the generated offset table, one entry per
    // state declaration, scalar states included with an extent of one.
    Extent extent;
    std::size_t off = 0;
    // §13.4 — per-element initial conditions from a settings source. A
    // scalar state rides `initial`; an array state has no aggregate value to
    // override, so its ICs arrive one index at a time.
    struct ElementIC {
        std::size_t index = 0;
        double value = 0.0;
    };
    std::vector<ElementIC> element_ic;
};

// §6.5 — an array-shaped var. Node-private storage, so the node sizes and
// owns it (Appendix C 8a): it is the one allocation in the lowering, and it
// happens once, at configuration.
struct ArrayVar {
    std::string name;
    Extent extent;
};

// Where a leaf input's value comes from. A wire is not its own variable
// (§15.3): it names the producer's `Out` member, plus any unit conversion the
// two declared units imply, folded to a constant.
// One field's affine conversion. A record wire carries one of these per leaf
// scalar field, flattened depth-first in declaration order, because §5.2 binds
// a parametric record's units at the reference and the two ends of a wire may
// therefore have bound them differently.
struct FieldConv {
    double scale = 1.0;
    double offset = 0.0;
    bool converts() const { return scale != 1.0 || offset != 0.0; }
};

struct InputSource {
    enum class Kind { Leaf, Boundary };
    Kind kind = Kind::Leaf;
    std::size_t producer = 0;          // index into Model::leaves
    std::string port;                  // producer's output port
    std::string boundary;              // Kind::Boundary — the Sim member name
    double scale = 1.0;                // scalar: consumer_value = scale * x + offset
    double offset = 0.0;
    // Record ports only; empty for a scalar. Same flattening order the emitter
    // walks when it writes the brace initialiser.
    std::vector<FieldConv> field_convs;
    bool converts() const {
        if (scale != 1.0 || offset != 0.0) return true;
        for (const FieldConv& f : field_convs)
            if (f.converts()) return true;
        return false;
    }
};

struct Leaf {
    std::string path;                  // model path, root-relative: "fl.whl"
    std::string ident;                 // generated C++ member base: "fl_whl"
    const NodeInfo* node = nullptr;

    std::vector<SettingValue> settings;
    std::vector<StateSlot> states;
    std::vector<ArrayVar> array_vars;            // §6.5, in declaration order
    std::map<std::string, InputSource> inputs;   // by port name

    // §6.2a — the definition's ports with every unit parameter substituted.
    // Identical to `node->inputs`/`node->outputs` for a non-parametric node,
    // which is almost all of them; for a parametric one this is the only place
    // the port's real unit exists, since the definition only knows `(U)`.
    std::vector<Field> ports_in;
    std::vector<Field> ports_out;

    long decimation = 1;               // §9.2 — 1 means base rate
    Loc loc;                           // the instance statement

    bool has_continuous() const;
    bool has_discrete() const;
};

// A root port the host drives or reads (§15.6 [boundary.in] / [boundary.out]).
struct BoundaryIn {
    std::string ident;                 // "src_axle_torque"
    std::string path;                  // "src.axle_torque"
    Type type;
};

struct BoundaryOut {
    std::string path;                  // "fl.whl.ws"
    std::string expr;                  // "fl_whl_out.ws"
    Type type;
};

// A signal named in `record: { signals { … } }`.
struct RecordedSignal {
    std::string path;                  // as written
    std::string expr;                  // a C++ expression in Sim scope
    Unit unit;
};

struct LogConfig {
    bool present = false;
    std::string file;
    std::string level = "info";
    std::vector<std::pair<std::string, std::string>> levels;   // path, level
};

struct RecordConfig {
    bool present = false;
    std::string file;
    long every = 1;
    std::vector<RecordedSignal> signals;
};

struct Model {
    std::string name;                  // the sim declaration's name
    const NodeInfo* root = nullptr;
    std::string root_namespace;        // the C++ namespace `Sim` lands in

    std::vector<Leaf> leaves;          // elaboration order
    std::vector<std::size_t> order;    // §8.5 topological order, into `leaves`
    std::size_t n_states = 0;

    // §6.4a — two lowerings, chosen by whether ANY state carries an extent
    // (§15.5a). False is the original one: `n_states` is a compile-time
    // constant, `x` is an owned `std::array`, and binding happens in the
    // constructor. True makes every block size a configuration result and every
    // published buffer the host's, which is why the choice is model-wide rather
    // than per-state — one block cannot be half constexpr.
    bool dynamic_states = false;
    std::size_t n_cont_slots = 0;      // continuous state DECLARATIONS
    std::size_t n_disc_arr_slots = 0;  // array-shaped discrete state declarations

    // §6.4a — every element index the model names, from an IC override
    // (§13.4) or a recorded column (§13.5). None of them can be checked
    // here: the extent does not settle until configuration, and the same source
    // may be valid against one `order` and invalid against another. So the
    // check is emitted, once per reference, into `Sim::configure()`.
    struct ElementRef {
        std::string path;              // as written, for the message
        bool continuous = false;
        std::size_t off = 0;           // index into the offset/length table
        std::size_t index = 0;
    };
    std::vector<ElementRef> element_refs;

    // §6.2b — every setting in the model, composites included, in the order
    // `resolve_settings()` must assign them. Doubles as the §15.6 configuration
    // schema, so the manifest and the loader cannot disagree about what exists.
    std::vector<SettingSlot> settings;

    std::vector<BoundaryIn> boundary_in;
    std::vector<BoundaryOut> boundary_out;

    double step = 1e-3;                // seconds
    double duration = 0.0;             // seconds
    std::string solver = "rk4";
    bool realtime = false;
    double sync = 0.0;                 // seconds
    long sync_steps = 0;
    double window = 1.0;               // seconds

    RecordConfig record;
    LogConfig log;

    const Program* program = nullptr;
};

}  // namespace se

#endif  // SE_MODEL_HPP
