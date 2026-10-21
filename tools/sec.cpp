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
//      --doc              write a Markdown reference for the given roots or
//                         .se files into -o (§6.14): index.md and one page
//                         per package
//      --spec=<path>      the SPECIFICATION.md that --doc checks and links
//                         `§` and `SE` citations against (default: beside
//                         the first input, then the current directory)
//      --dump-ast         print the parsed AST
//      --dump-tokens      print the token stream (does not parse)
//      -I <dir>           add a root directory (§2.1); repeatable
//      --no-default-roots do not read se.roots or add the toolchain stdlib
//      --show-roots       print each root and where it came from; with no
//                         input files, print them and stop
//      -o <dir>          output directory for --emit (default `.`), created
//                         if it does not exist
//      --stem=<name>      output base name for --emit (default: the sim name)
//      --no-main          do not write main.cpp (embed in your own host)
//      --runtime=<how>    inline (default): paste the engine runtime into
//                         <stem>.cpp; separate: write it as
//                         se_runtime.hpp and #include it
//      --no-build         do not write build.ps1 / build.sh
//      --topology=<list>  which topology diagrams to write, comma-separated:
//                         html (<stem>.topology.html, interactive) and
//                         mermaid (<stem>.topology.md); default both
//      --no-topology      write neither
//      --kind=<k>         force model | sim | settings, ignoring the extension
//      --max-errors=<n>   diagnostic cap (default 20; 0 means no cap)
//      --no-color         plain output
//      -h, --help
//
//  A root is a directory declared from outside the language, exactly like a
//  C++ include directory, and the package path is the directory path inside
//  it: `-I examples` makes `drivetrain.Corner` mean
//  `examples/drivetrain/Corner.se`. After the -I roots come the lines of the
//  nearest `se.roots` at or above the first input's directory, then the
//  toolchain's stdlib at `<dir of sec>/../stdlib`, so `build/sec.exe` finds
//  the repo's `stdlib/` and an installed `bin/sec` its sibling. With no -I and
//  no se.roots, `.` is a root. There is deliberately no environment variable:
//  both sources are checked in or shipped, so builds are reproducible.
//
//  Exit status (§16.2): 0 clean, 1 at least one error, 2 driver failure.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <string>
#include <vector>

#include "../src/ast.hpp"
#include "../src/diag.hpp"
#include "../src/doc.hpp"
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
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

namespace fs = std::filesystem;

enum class Kind { Model, Sim, Settings };
enum class Mode { Parse, Emit, Test, Doc, DumpAst, DumpTokens };

void usage(std::ostream& o) {
    o << "sec — sim-engine compiler\n\n"
         "usage: sec [options] <file>...\n\n"
         "  --parse            parse and report diagnostics (default)\n"
         "  --emit             compile a .sim file and write the generated model\n"
         "  --test             write a test program per node `tests` section\n"
         "  --doc              write a Markdown reference for the given roots\n"
         "                     or .se files into -o: index.md + one page per\n"
         "                     package\n"
         "  --spec=<path>      SPECIFICATION.md for --doc's citation checks\n"
         "  --dump-ast         print the parsed AST\n"
         "  --dump-tokens      print the token stream (does not parse)\n"
         "  -I <dir>           add a root directory; repeatable\n"
         "  --no-default-roots do not read se.roots or add the toolchain stdlib\n"
         "  --show-roots       print each root and its source (no files: then stop)\n"
         "  -o <dir>           output directory for --emit (default `.`),\n"
         "                     created if it does not exist\n"
         "  --stem=<name>      output base name for --emit (default: the sim name)\n"
         "  --no-main          do not write main.cpp\n"
         "  --runtime=<how>    inline (default) | separate: paste the engine\n"
         "                     runtime into <stem>.cpp, or write se_runtime.hpp\n"
         "  --no-build         do not write build.ps1 / build.sh\n"
         "  --topology=<list>  html,mermaid (default both): write\n"
         "                     <stem>.topology.html and/or <stem>.topology.md\n"
         "  --no-topology      write neither\n"
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

// ─── Roots (§2.1) ────────────────────────────────────────────────────────────

struct Root {
    std::string dir;
    std::string from;   // for --show-roots: "-I", "se.roots <file>", ...
};

// The directory holding this executable, from the OS rather than argv[0],
// which is whatever the shell was given and may be a bare name found on PATH.
fs::path exe_dir(const char* argv0) {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
        if (n == 0) break;
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);   // truncated: the path is longer than MAX_PATH
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(&buf[0], &size) == 0) {
        const fs::path p = fs::weakly_canonical(fs::path(buf.c_str()), ec);
        if (!ec) return p.parent_path();
    }
#else
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (!ec) return p.parent_path();
#endif
    const fs::path q = fs::absolute(argv0, ec);
    return ec ? fs::path() : q.parent_path();
}

// The nearest `se.roots` at or above `start`, or empty, found the way an
// editor finds `.clang-format`.
fs::path find_roots_file(const std::string& start) {
    std::error_code ec;
    fs::path dir = fs::absolute(start, ec).lexically_normal();
    if (ec) return {};
    for (;;) {
        const fs::path f = dir / "se.roots";
        if (fs::is_regular_file(f, ec)) return f;
        const fs::path up = dir.parent_path();
        if (up.empty() || up == dir) return {};
        dir = up;
    }
}

// Appends the roots listed in `file`: one path per line, relative to the
// file, `#` to the end of the line a comment. A listed directory that does
// not exist is a driver error, since the file is the model's own checked-in
// claim about where its packages are.
bool read_roots_file(const fs::path& file, std::vector<Root>& out) {
    std::ifstream in(file);
    if (!in) {
        std::cerr << "sec: " << file.generic_string() << ": cannot read\n";
        return false;
    }
    const std::string from = "se.roots " + file.generic_string();
    std::string line;
    for (int n = 1; std::getline(in, line); ++n) {
        const std::size_t hash = line.find('#');
        if (hash != std::string::npos) line.erase(hash);
        const std::size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line = line.substr(b, line.find_last_not_of(" \t\r") - b + 1);
        if (line.rfind("override ", 0) == 0) {
            std::cerr << "sec: " << file.generic_string() << ":" << n
                      << ": `override` is not implemented yet (§2.1)\n";
            return false;
        }
        const fs::path dir = (file.parent_path() / line).lexically_normal();
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) {
            std::cerr << "sec: " << file.generic_string() << ":" << n
                      << ": no such directory `" << line << "`\n";
            return false;
        }
        out.push_back({dir.generic_string(), from});
    }
    return true;
}

// -I first, then se.roots, then `.` if neither gave a root, then the
// toolchain stdlib. A directory reached twice keeps its first place, so
// `-I stdlib` beside the toolchain's own copy is not an SE0305 collision.
bool assemble_roots(const std::vector<std::string>& cli, bool defaults,
                    const std::string& first_input, const char* argv0,
                    std::vector<Root>& out) {
    for (const std::string& r : cli) out.push_back({r, "-I"});
    if (defaults) {
        const fs::path file = find_roots_file(dir_of(first_input));
        if (!file.empty() && !read_roots_file(file, out)) return false;
    }
    if (out.empty()) out.push_back({".", "default"});
    if (defaults) {
        const fs::path bin = exe_dir(argv0);
        std::error_code ec;
        if (!bin.empty()) {
            const fs::path lib = (bin.parent_path() / "stdlib").lexically_normal();
            if (fs::is_directory(lib / "se", ec)) out.push_back({lib.generic_string(), "toolchain"});
        }
    }
    std::set<std::string> seen;
    std::vector<Root> kept;
    for (Root& r : out) {
        std::error_code ec;
        std::string key = fs::weakly_canonical(r.dir, ec).generic_string();
        if (ec) key = r.dir;
        if (seen.insert(key).second) kept.push_back(std::move(r));
    }
    out = std::move(kept);
    return true;
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
    bool default_roots = true;
    bool show_roots = false;
    se::EmitOptions emit_opt;
    se::DocOptions doc_opt;

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
        } else if (a == "--no-default-roots") {
            default_roots = false;
        } else if (a == "--show-roots") {
            show_roots = true;
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
            emit_opt.topology_html = false;
            emit_opt.topology_mermaid = false;
        } else if (a.rfind("--topology=", 0) == 0) {
            // A list, not a single choice: the formats are independent, and
            // naming one turns the others off.
            emit_opt.topology_html = false;
            emit_opt.topology_mermaid = false;
            std::string list = a.substr(11);
            for (std::size_t at = 0; at <= list.size();) {
                std::size_t comma = list.find(',', at);
                if (comma == std::string::npos) comma = list.size();
                const std::string f = list.substr(at, comma - at);
                if (f == "html") {
                    emit_opt.topology_html = true;
                } else if (f == "mermaid") {
                    emit_opt.topology_mermaid = true;
                } else {
                    std::cerr << "sec: --topology: unknown format `" << f
                              << "` (expected html, mermaid)\n";
                    return 2;
                }
                at = comma + 1;
            }
        } else if (a == "--quiet") {
            emit_opt.quiet = true;
        } else if (a == "--emit") {
            mode = Mode::Emit;
        } else if (a == "--test") {
            mode = Mode::Test;
        } else if (a == "--doc") {
            mode = Mode::Doc;
        } else if (a.rfind("--spec=", 0) == 0) {
            doc_opt.spec_path = a.substr(7);
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

    // Only the modes that resolve need roots, so a broken se.roots does not
    // stop a plain parse.
    if (show_roots || mode == Mode::Emit || mode == Mode::Test) {
        std::vector<Root> found;
        if (!assemble_roots(roots, default_roots, files.empty() ? "" : files.front(),
                            argv[0], found))
            return 2;
        if (show_roots)
            for (const Root& r : found) std::printf("root  %-40s  %s\n", r.dir.c_str(), r.from.c_str());
        roots.clear();
        for (const Root& r : found) roots.push_back(r.dir);
        if (show_roots && files.empty()) return 0;
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

    if (mode == Mode::Doc) {
        doc_opt.out_dir = emit_opt.out_dir;
        doc_opt.quiet = emit_opt.quiet;
        const int rc = se::generate_docs(files, diag, doc_opt);
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
