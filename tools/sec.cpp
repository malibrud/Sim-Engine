// ─────────────────────────────────────────────────────────────────────────────
//  sec — the sim-engine compiler driver.
//
//  Implements all six stages of SPECIFICATION.md §1.3. `--parse` stops after
//  stage 2 and takes any file kind; `--emit` runs the whole pipeline over one
//  `.sim` file and writes the public header, the implementation, an example
//  host, the unit manifest and the reference build scripts.
//
//  Usage:
//      sec [options] <file>...
//
//      --parse            parse and report diagnostics (default)
//      --emit             compile a .sim file and write the generated model
//      --test             write a test program for each node's `tests` section
//                         (§6.13), one subdirectory of -o per tested node
//      --dump-ast         print the parsed AST
//      --dump-tokens      print the token stream (does not parse)
//      -I <dir>           add a root directory (§2.1); repeatable
//      -o <dir>           output directory for --emit (default `.`), created
//                         if it does not exist
//      --stem=<name>      output base name for --emit (default: the sim name)
//      --no-main          do not write main.cpp (embed in your own host)
//      --runtime=<how>    inline (default): paste the engine runtime into
//                         <stem>.cpp; separate: write it as
//                         se_runtime.hpp and #include it
//      --no-build         do not write build.ps1 / build.sh
//      --no-topology      do not write <stem>.topology.md
//      --kind=<k>         force model | sim | settings, ignoring the extension
//      --max-errors=<n>   diagnostic cap (default 20; 0 means no cap)
//      --no-color         plain output
//      -h, --help
//
//  A root is a directory declared from outside the language, exactly like a
//  C++ include directory, and the package path is the directory path inside
//  it: `-I examples` makes `drivetrain.Corner` mean
//  `examples/drivetrain/Corner.se`. There is deliberately no environment
//  variable and no search-path default beyond `.`, so builds are reproducible.
//
//  Exit status (§16.2): 0 clean, 1 at least one error, 2 driver failure.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "../src/ast.hpp"
#include "../src/diag.hpp"
#include "../src/dump.hpp"
#include "../src/elaborate.hpp"
#include "../src/emit.hpp"
#include "../src/lexer.hpp"
#include "../src/parser.hpp"
#include "../src/resolve.hpp"
#include "../src/schedule.hpp"
#include "../src/testgen.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

enum class Kind { Model, Sim, Settings };
enum class Mode { Parse, Emit, Test, DumpAst, DumpTokens };

void usage(std::ostream& o) {
    o << "sec — sim-engine compiler\n\n"
         "usage: sec [options] <file>...\n\n"
         "  --parse            parse and report diagnostics (default)\n"
         "  --emit             compile a .sim file and write the generated model\n"
         "  --test             write a test program per node `tests` section\n"
         "  --dump-ast         print the parsed AST\n"
         "  --dump-tokens      print the token stream (does not parse)\n"
         "  -I <dir>           add a root directory; repeatable\n"
         "  -o <dir>           output directory for --emit (default `.`),\n"
         "                     created if it does not exist\n"
         "  --stem=<name>      output base name for --emit (default: the sim name)\n"
         "  --no-main          do not write main.cpp\n"
         "  --runtime=<how>    inline (default) | separate: paste the engine\n"
         "                     runtime into <stem>.cpp, or write se_runtime.hpp\n"
         "  --no-build         do not write build.ps1 / build.sh\n"
         "  --no-topology      do not write <stem>.topology.md\n"
         "  --kind=<k>         force model | sim | settings\n"
         "  --max-errors=<n>   diagnostic cap (default 20, 0 = no cap)\n"
         "  --no-color         plain output\n"
         "  -h, --help         this text\n";
}

std::string dir_of(const std::string& path) {
    const std::size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

// Stages 3-6 over one `.sim` file. Each stage runs only if the one before it
// produced no errors: there is no value in scheduling a model whose wires did
// not resolve, and a cascade of derived nonsense buries the real diagnostic.
int compile(const std::string& path, se::Diagnostics& diag,
            const std::vector<std::string>& roots, const se::EmitOptions& opt) {
    se::Source src;
    std::string error;
    if (!se::Source::load(path, src, error)) {
        std::cerr << "sec: " << path << ": " << error << "\n";
        return 2;
    }

    se::Parser parser(src, diag);
    auto sim = parser.parse_sim_file();
    if (diag.error_count() > 0) return 1;

    se::Resolver resolver(diag, roots);
    se::Elaborator elaborator(diag, resolver);
    se::Model model;
    if (!elaborator.run(*sim, src, dir_of(path), model)) return 1;
    if (!se::schedule(diag, model)) return 1;
    if (diag.error_count() > 0) return 1;

    if (!opt.quiet) std::printf("sec: %s -> %s\n", path.c_str(), opt.out_dir.c_str());
    return se::emit(diag, model, opt) ? 0 : 2;
}

bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool kind_from_path(const std::string& path, Kind& out) {
    if (ends_with(path, ".se")) {
        out = Kind::Model;
        return true;
    }
    if (ends_with(path, ".sim")) {
        out = Kind::Sim;
        return true;
    }
    if (ends_with(path, ".settings")) {
        out = Kind::Settings;
        return true;
    }
    return false;
}

bool stdout_is_tty() {
#ifdef _WIN32
    return _isatty(_fileno(stdout)) != 0;
#else
    return isatty(fileno(stdout)) != 0;
#endif
}

void enable_vt_mode() {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (h != INVALID_HANDLE_VALUE && GetConsoleMode(h, &mode))
        SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
}

void dump_tokens(const se::Source& src, se::Diagnostics& diag) {
    se::Lexer lexer(src, diag);
    for (;;) {
        const se::Token t = lexer.next();
        std::printf("%4u:%-4u %-18s", t.loc.line, t.loc.col, se::describe(t.kind));
        if (t.kind == se::Tok::Ident || t.kind == se::Tok::Number ||
            t.kind == se::Tok::String || t.is_keyword()) {
            std::printf("  %s", t.text.c_str());
        }
        std::printf("\n");
        if (t.kind == se::Tok::End) break;
    }
}

}  // namespace

int main(int argc, char** argv) {
    Mode mode = Mode::Parse;
    bool color = stdout_is_tty();
    bool forced_kind = false;
    Kind forced = Kind::Model;
    int max_errors = 20;
    std::vector<std::string> files;
    std::vector<std::string> roots;
    se::EmitOptions emit_opt;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(std::cout);
            return 0;
        } else if (a == "-I") {
            if (++i >= argc) {
                std::cerr << "sec: -I needs a directory\n";
                return 2;
            }
            roots.push_back(argv[i]);
        } else if (a.rfind("-I", 0) == 0 && a.size() > 2) {
            roots.push_back(a.substr(2));
        } else if (a == "-o") {
            if (++i >= argc) {
                std::cerr << "sec: -o needs a directory\n";
                return 2;
            }
            emit_opt.out_dir = argv[i];
        } else if (a.rfind("--stem=", 0) == 0) {
            emit_opt.stem = a.substr(7);
        } else if (a == "--no-main") {
            emit_opt.write_main = false;
        } else if (a == "--runtime=inline") {
            emit_opt.separate_runtime = false;
        } else if (a == "--runtime=separate") {
            emit_opt.separate_runtime = true;
        } else if (a == "--no-build") {
            emit_opt.write_build = false;
        } else if (a == "--no-topology") {
            emit_opt.write_topology = false;
        } else if (a == "--quiet") {
            emit_opt.quiet = true;
        } else if (a == "--emit") {
            mode = Mode::Emit;
        } else if (a == "--test") {
            mode = Mode::Test;
        } else if (a == "--parse") {
            mode = Mode::Parse;
        } else if (a == "--dump-ast") {
            mode = Mode::DumpAst;
        } else if (a == "--dump-tokens") {
            mode = Mode::DumpTokens;
        } else if (a == "--no-color") {
            color = false;
        } else if (a.rfind("--kind=", 0) == 0) {
            const std::string k = a.substr(7);
            if (k == "model") forced = Kind::Model;
            else if (k == "sim") forced = Kind::Sim;
            else if (k == "settings") forced = Kind::Settings;
            else {
                std::cerr << "sec: unknown kind `" << k << "`\n";
                return 2;
            }
            forced_kind = true;
        } else if (a.rfind("--max-errors=", 0) == 0) {
            max_errors = std::atoi(a.c_str() + 13);
            if (max_errors <= 0) max_errors = 1000000;
        } else if (!a.empty() && a[0] == '-') {
            std::cerr << "sec: unknown option `" << a << "`\n";
            usage(std::cerr);
            return 2;
        } else {
            files.push_back(a);
        }
    }

    if (files.empty()) {
        std::cerr << "sec: no input files\n";
        usage(std::cerr);
        return 2;
    }
    if (color) enable_vt_mode();

    se::Diagnostics diag(std::cout, color, max_errors);
    int driver_failures = 0;

    if (mode == Mode::Emit) {
        if (files.size() != 1) {
            std::cerr << "sec: --emit takes exactly one .sim file\n";
            return 2;
        }
        const int rc = compile(files.front(), diag, roots, emit_opt);
        diag.print_summary();
        return rc;
    }

    if (mode == Mode::Test) {
        se::TestGenOptions topt;
        topt.out_dir = emit_opt.out_dir;
        topt.quiet = emit_opt.quiet;
        int rc = 0;
        for (const std::string& path : files) {
            const int r = se::generate_tests(path, diag, roots, topt);
            if (r > rc) rc = r;
        }
        diag.print_summary();
        return rc;
    }

    for (const std::string& path : files) {
        Kind kind = Kind::Model;
        if (forced_kind) {
            kind = forced;
        } else if (!kind_from_path(path, kind)) {
            std::cerr << "sec: cannot tell the kind of `" << path
                      << "` from its extension; pass --kind=model|sim|settings\n";
            ++driver_failures;
            continue;
        }

        se::Source src;
        std::string error;
        if (!se::Source::load(path, src, error)) {
            std::cerr << "sec: " << path << ": " << error << "\n";
            ++driver_failures;
            continue;
        }

        if (mode == Mode::DumpTokens) {
            std::cout << "=== " << path << " ===\n";
            dump_tokens(src, diag);
            continue;
        }

        se::Parser parser(src, diag);
        switch (kind) {
            case Kind::Model: {
                auto file = parser.parse_model_file();
                if (mode == Mode::DumpAst) {
                    std::cout << "=== " << path << " ===\n";
                    se::dump(std::cout, *file);
                }
                break;
            }
            case Kind::Sim: {
                auto file = parser.parse_sim_file();
                if (mode == Mode::DumpAst) {
                    std::cout << "=== " << path << " ===\n";
                    se::dump(std::cout, *file);
                }
                break;
            }
            case Kind::Settings: {
                auto file = parser.parse_settings_file();
                if (mode == Mode::DumpAst) {
                    std::cout << "=== " << path << " ===\n";
                    se::dump(std::cout, *file);
                }
                break;
            }
        }
    }

    diag.print_summary();
    if (driver_failures > 0) return 2;
    return diag.error_count() > 0 ? 1 : 0;
}
