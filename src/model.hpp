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
    Loc loc;

    const char* cpp() const { return scalar.c_str(); }
};

struct Field {
    std::string name;
    Type type;
    Loc loc;
};

struct RecordInfo {
    std::string fq;                    // "drivetrain.WheelState"
    std::string cpp_name;              // "drivetrain::WheelState"
    const ast::TypeDef* def = nullptr;
    const FileInfo* file = nullptr;
    std::vector<Field> fields;
    bool resolved = false;             // guards the §5.2 cycle check
    bool resolving = false;
};

struct NodeInfo {
    std::string fq;                    // "drivetrain.Wheel"
    std::string cpp_name;              // "drivetrain::Wheel"
    const ast::NodeDef* def = nullptr;
    const FileInfo* file = nullptr;
    bool composite = false;

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

struct StateSlot {
    std::string name;
    bool continuous = false;
    Unit unit;
    Unit der_unit;
    std::string scalar;
    double initial = 0.0;
    std::size_t slot = 0;              // index into Sim::x — continuous only
};

// Where a leaf input's value comes from. A wire is not its own variable
// (§15.3): it names the producer's `Out` member, plus any unit conversion the
// two declared units imply, folded to a constant.
struct InputSource {
    enum class Kind { Leaf, Boundary };
    Kind kind = Kind::Leaf;
    std::size_t producer = 0;          // index into Model::leaves
    std::string port;                  // producer's output port
    std::string boundary;              // Kind::Boundary — the Sim member name
    double scale = 1.0;                // consumer_value = scale * x + offset
    double offset = 0.0;
    bool converts() const { return scale != 1.0 || offset != 0.0; }
};

struct Leaf {
    std::string path;                  // model path, root-relative: "fl.whl"
    std::string ident;                 // generated C++ member base: "fl_whl"
    const NodeInfo* node = nullptr;

    std::vector<SettingValue> settings;
    std::vector<StateSlot> states;
    std::map<std::string, InputSource> inputs;   // by port name

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
