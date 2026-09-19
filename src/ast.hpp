// ─────────────────────────────────────────────────────────────────────────────
//  AST.  SPECIFICATION.md §17.
//
//  Stage 2 output: a faithful syntactic record with no name resolution, no unit
//  evaluation and no elaboration (§1.3). Unit expressions and setting
//  expressions are kept as trees rather than folded, because folding needs the
//  unit-symbol table and the setting dependency graph — both stage 4.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_AST_HPP
#define SE_AST_HPP

#include <memory>
#include <string>
#include <vector>

#include "diag.hpp"
#include "lexer.hpp"

namespace se {
namespace ast {

// A dotted name: `std.mech.Damper`, or a model path `front.left.mass`.
struct Path {
    std::vector<std::string> segs;
    Loc loc;

    std::string str() const {
        std::string s;
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (i) s += '.';
            s += segs[i];
        }
        return s;
    }
};

// ─── Units (§4.1) ────────────────────────────────────────────────────────────

struct UnitExpr;
using UnitPtr = std::unique_ptr<UnitExpr>;

struct UnitExpr {
    enum class Kind {
        Dimensionless,  // (-)
        One,            // the literal 1, as in (1/(kg*m^2))
        Symbol,         // kg, m, %, degC …
        Mul,
        Div,
        Pow,
    };

    Kind kind = Kind::Dimensionless;
    std::string symbol;          // Symbol
    UnitPtr lhs, rhs;            // Mul / Div
    UnitPtr base;                // Pow
    long exp_num = 1;            // Pow — rational exponent, e.g. m^(1/2)
    long exp_den = 1;
    Loc loc;
};

// ─── Expressions (§7.1) ──────────────────────────────────────────────────────

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
    enum class Kind {
        Number,     // literal, with an optional unit suffix
        Param,      // param.<name>, or param.<name>.<field>… into a record (§6.2c)
        Call,       // f(args…)
        Name,       // a named constant: pi, e, inf
        Unary,      // + -
        Binary,     // + - * / ^
        Aggregate,  // { … } — a record brace list (§6.2c)
    };

    Kind kind = Kind::Number;
    Loc loc;

    double number = 0.0;
    std::string text;            // Number: source spelling; Param/Call/Name: the name
    UnitPtr unit;                // Number: optional unit suffix
    char op = 0;                 // Unary / Binary
    ExprPtr lhs, rhs;            // Binary; Unary uses lhs
    std::vector<ExprPtr> args;   // Call; Aggregate: the field values, in order
    // §6.2c — an Aggregate is positional when this is empty, and named when it
    // is parallel to `args`. The two spellings never mix (SE0243), so one
    // vector being empty-or-parallel is the whole discriminator.
    std::vector<std::string> field_names;
    std::vector<Loc> field_locs;   // Aggregate: per-field, for diagnostics
};

// ─── Types (§5) ──────────────────────────────────────────────────────────────

// A port, field or var type: either `(unit): scalar` or `: RecordName`.
struct TypeRef {
    bool is_scalar = false;
    UnitPtr unit;              // scalar only
    std::string scalar;        // "double" | "float" | "int" | "bool", as written
    Path record;               // record only
    // §5.2 — unit arguments for a parametric record, positional, in the type's
    // `units` order: `force: math.Vec3(N);`. A type has no instantiation site,
    // so unlike a node's (§6.2a) these are bound at every REFERENCE.
    std::vector<UnitPtr> unit_args;
    Loc unit_args_loc;
    Loc loc;
};

// ─── Declarations inside a node ──────────────────────────────────────────────

struct FieldDecl {            // inputs / outputs / vars / record fields
    std::string name;
    TypeRef type;
    // §6.4a — `[expr]`, on a `var` only. A port may not carry one (SE0234) and
    // neither may a record field: both are in the signal block, whose offsets
    // come from `offsetof` (§15.5). Null when the declaration is scalar.
    ExprPtr extent;
    Loc extent_loc;
    Loc loc;
};

struct SettingDecl {          // §6.2
    std::string name;
    TypeRef type;
    ExprPtr default_value;    // null when the setting is required
    Loc loc;
};

struct StateDecl {            // §6.4
    bool is_continuous = false;
    std::string name;
    TypeRef type;
    ExprPtr initial;          // the declared IC
    ExprPtr extent;           // §6.4a — `[expr]`; null when the state is scalar
    Loc extent_loc;
    Loc kind_loc;
    Loc loc;
};

struct NativeDecl {           // §6.7 — the type text is verbatim C++
    std::string name;
    std::string cpp_type;
    Loc loc;
    Loc type_loc;
};

struct BuildStmt {            // §12.1
    std::string primitive;    // link | include_dir | define | cflag
    std::string argument;
    std::string when;         // platform atom, empty when unconditional
    Loc loc;
    Loc when_loc;
};

struct Method {               // §8.1
    enum class Which { Init, Output, Derivative, Next, Adjust, OnStep, Final };
    Which which = Which::Init;
    std::vector<std::string> params;   // the feedthrough set for output()
    std::vector<Loc> param_locs;
    Verbatim body;
    Loc loc;

    const char* name() const {
        switch (which) {
            case Which::Init:       return "init";
            case Which::Output:     return "output";
            case Which::Derivative: return "derivative";
            case Which::Next:       return "next";
            case Which::Adjust:     return "adjust";
            case Which::OnStep:     return "on_step";
            case Which::Final:      return "final";
        }
        return "?";
    }
};

struct Helper {               // §11.4 — verbatim private member function
    Verbatim signature;
    Verbatim body;
    Loc loc;
};

// ─── Structure (§6.9) ────────────────────────────────────────────────────────

struct Binding {              // setting = expression;
    std::string name;
    ExprPtr value;
    Loc loc;
};

struct Instance {             // node whl : Wheel { … };
    std::string name;
    Path definition;
    // §6.2a — unit arguments, positional, in the definition's `units` order:
    // `node hp : Biquad (m/s^2) { … };`. They sit after the definition path
    // rather than inside the brace list because nothing else may follow that
    // path, so a `(` here needs no lookahead. Inside the braces `U = (m/s^2)`
    // would be indistinguishable from a parenthesised setting expression such
    // as `mass = (total / 4)`.
    std::vector<UnitPtr> unit_args;
    Loc unit_args_loc;
    std::vector<Binding> bindings;
    Loc loc;
};

struct Endpoint {             // self.port | instance.port | instance.rec.field | 0.0 (m)
    bool is_self = false;
    std::vector<std::string> segs;   // excludes the `self` head
    Loc loc;

    // §6.9.2 — a literal as a wire SOURCE. `segs` is empty and `is_self` false,
    // which no other endpoint the parser produces can be.
    //
    // The unit is a `shared_ptr` and not a `UnitPtr` because an `Endpoint` must
    // stay copyable: `parse_wire_stmt` desugars a chain by copying each interior
    // endpoint into two wires. Nothing mutates a unit after parsing, so sharing
    // one is sound.
    bool is_literal = false;
    double value = 0.0;
    std::string text;                       // the source spelling, for diagnostics
    std::shared_ptr<UnitExpr> unit;         // null when the literal is bare (§7.2)

    std::string str() const {
        if (is_literal) return text;
        std::string s = is_self ? "self" : "";
        for (std::size_t i = 0; i < segs.size(); ++i) {
            if (i || is_self) s += '.';
            s += segs[i];
        }
        return s;
    }
};

struct Wire {                 // source --> dest, dest, …;
    Endpoint source;
    std::vector<Endpoint> dests;
    Loc loc;
};

struct Structure {
    std::vector<Instance> instances;
    std::vector<Wire> wires;
};

// Presence + location of an at-most-once section (§6, SE0220).
struct SectionMark {
    bool present = false;
    Loc loc;
};

// ─── Tests (§6.13) ───────────────────────────────────────────────────────────

// One column of a set header: `step`, or an accessor such as `out.q.w`. `ns` is
// the accessor's namespace and `segs` the member path under it, so `out.q.w`
// is ns "out", segs {"q", "w"}. An element index rides on its segment as
// `w1[3]`, exactly as a recorded column's does (§13.5).
struct TestColumn {
    bool is_step = false;
    std::string ns;
    std::vector<std::string> segs;
    Loc loc;

    std::string str() const {
        if (is_step) return "step";
        std::string s = ns;
        for (const std::string& g : segs) s += "." + g;
        return s;
    }
};

struct TestRow {
    std::vector<ExprPtr> cells;
    std::string label;
    Loc label_loc;
    Loc loc;
};

struct TestSet {
    std::string claim;
    Loc claim_loc;
    std::vector<TestColumn> given;
    std::vector<TestColumn> expected;
    bool approx = false;       // `~=` rather than `=`
    Loc op_loc;
    ExprPtr within;            // required with `~=`, forbidden with `=` (SE0272)
    std::vector<TestRow> rows;
    Loc loc;
};

struct UnitBinding {           // `U = m/s^2;` in a tests preamble
    std::string name;
    UnitPtr unit;
    Loc loc;
};

struct TestsSection {
    bool has_step = false;
    ExprPtr step;
    Loc step_loc;
    SectionMark sec_units, sec_settings;
    std::vector<UnitBinding> units;
    std::vector<Binding> settings;
    std::vector<TestSet> sets;
};

// ─── Definitions ─────────────────────────────────────────────────────────────

// One unit parameter — a name that stands for a unit inside a node's port,
// state and var declarations (§6.2a), or inside a record's field declarations
// (§5.2). It has no type and no default: a unit variable ranges over units, not
// values. A node binds its parameters once, at instantiation; a record binds
// them at every reference, since a record is never instantiated.
struct UnitParamDecl {
    std::string name;
    Loc loc;
};

struct TypeDef {              // §5.2
    std::string name;
    SectionMark sec_units;
    std::vector<UnitParamDecl> unit_params;
    std::vector<FieldDecl> fields;
    Loc loc;
};

struct NodeDef {              // §6
    std::string name;
    Loc loc;

    SectionMark sec_settings, sec_inputs, sec_outputs, sec_states, sec_vars;
    SectionMark sec_native, sec_declarations, sec_build, sec_structure;
    SectionMark sec_units;
    SectionMark sec_tests;

    std::vector<UnitParamDecl> unit_params;
    std::vector<SettingDecl> settings;
    std::vector<FieldDecl> inputs;
    std::vector<FieldDecl> outputs;
    std::vector<FieldDecl> vars;
    std::vector<StateDecl> states;
    std::vector<NativeDecl> natives;
    std::vector<BuildStmt> build;
    Verbatim declarations;
    Structure structure;
    std::vector<Method> methods;
    std::vector<Helper> helpers;
    // §6.13 — read only by `sec --test`; contributes nothing to a lowered model,
    // and so plays no part in `has_code()` either.
    TestsSection tests;

    // Kind inference (§6.1). Reported, not stored as a decision: stage 3 owns
    // the leaf/composite conflict check.
    bool has_code() const {
        return !methods.empty() || !helpers.empty() || sec_states.present ||
               sec_vars.present || sec_native.present || sec_declarations.present;
    }
    bool is_composite() const { return sec_structure.present; }
};

struct UseDecl {
    Path path;
    Loc loc;
};

// A `.se` file (§2.5).
struct ModelFile {
    Path package;
    bool has_package = false;
    std::vector<UseDecl> uses;
    std::vector<TypeDef> types;
    std::vector<NodeDef> nodes;
    // A package manifest (`package.se`, §2.6) carries only a file-level build block.
    std::vector<BuildStmt> build;
    SectionMark sec_build;
};

// ─── The sim file (§13) ──────────────────────────────────────────────────────

struct RecordBlock {
    std::string file;
    bool has_file = false;
    long every = 0;
    bool has_every = false;
    std::vector<Path> signals;
    Loc loc;
};

struct LogBlock {
    std::string file;
    bool has_file = false;
    std::string level;
    bool has_level = false;
    std::vector<std::pair<Path, std::string>> levels;
    Loc loc;
};

struct SimEntry {
    enum class Kind {
        Root,      // root: Vehicle { … };
        Value,     // step: 1 (ms);   every: 10;
        Word,      // solver: rk4;    mode: realtime;
        Text,      // settings: "file.settings";
        Record,
        Log,
    };

    Kind kind = Kind::Value;
    std::string key;
    Loc key_loc;
    Loc loc;

    Path root;                       // Root
    std::vector<Binding> bindings;   // Root
    ExprPtr value;                   // Value
    std::string word;                // Word
    std::string text;                // Text
    RecordBlock record;              // Record
    LogBlock log;                    // Log
};

struct SimFile {
    std::string name;
    Loc loc;
    std::vector<SimEntry> entries;
};

// ─── The settings source (§14) ───────────────────────────────────────────────

struct Override {
    Path path;
    ExprPtr value;
    Loc loc;
};

struct SettingsFile {
    std::vector<Override> overrides;
};

}  // namespace ast
}  // namespace se

#endif  // SE_AST_HPP
