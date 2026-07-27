// ─────────────────────────────────────────────────────────────────────────────
//  sec — the sim-engine compiler driver.
//
//  This build implements stages 1–2 of SPECIFICATION.md §1.3: lexical and
//  syntactic analysis over `.se`, `.sim` and `.settings` files. Name
//  resolution, unit checking, elaboration, scheduling and emission are
//  specified but not yet implemented; `--parse` is the whole contract for now.
//
//  Usage:
//      sec [options] <file>...
//
//      --parse            parse and report diagnostics (default)
//      --dump-ast         print the parsed AST
//      --dump-tokens      print the token stream (does not parse)
//      --kind=<k>         force model | sim | settings, ignoring the extension
//      --max-errors=<n>   diagnostic cap (default 20; 0 means no cap)
//      --no-color         plain output
//      -h, --help
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
#include "../src/lexer.hpp"
#include "../src/parser.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

enum class Kind { Model, Sim, Settings };
enum class Mode { Parse, DumpAst, DumpTokens };

void usage(std::ostream& o) {
    o << "sec — sim-engine compiler (stages 1-2: parse only)\n\n"
         "usage: sec [options] <file>...\n\n"
         "  --parse            parse and report diagnostics (default)\n"
         "  --dump-ast         print the parsed AST\n"
         "  --dump-tokens      print the token stream (does not parse)\n"
         "  --kind=<k>         force model | sim | settings\n"
         "  --max-errors=<n>   diagnostic cap (default 20, 0 = no cap)\n"
         "  --no-color         plain output\n"
         "  -h, --help         this text\n";
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

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            usage(std::cout);
            return 0;
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
