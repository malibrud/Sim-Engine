// ─────────────────────────────────────────────────────────────────────────────
//  Source management and diagnostics.
//
//  SPECIFICATION.md §16. Every diagnostic carries a stable code, a
//  file:line:column, a caret span, and optional notes/helps. The design intent
//  (§8.6) is that the diagnostic is where the budget goes, so this renderer is
//  deliberately a little more capable than "print a message".
// ─────────────────────────────────────────────────────────────────────────────
#ifndef SE_DIAG_HPP
#define SE_DIAG_HPP

#include <cstdint>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>

namespace se {

// A position in one source file. `length` spans the caret; 0 means "one char".
struct Loc {
    std::uint32_t line   = 0;   // 1-based
    std::uint32_t col    = 0;   // 1-based, in bytes
    std::uint32_t offset = 0;   // byte offset from the start of the file
    std::uint32_t length = 0;   // caret span, in bytes

    bool valid() const { return line != 0; }
};

// ─── Source ──────────────────────────────────────────────────────────────────

class Source {
public:
    Source() { line_starts_.push_back(0); }
    Source(std::string path, std::string text);

    // Reads a file. On failure returns false and fills `error`.
    static bool load(const std::string& path, Source& out, std::string& error);

    const std::string& path() const { return path_; }
    const std::string& text() const { return text_; }

    // 1-based; returns the line without its terminator. Out of range -> empty.
    std::string_view line(std::uint32_t n) const;
    std::uint32_t line_count() const { return static_cast<std::uint32_t>(line_starts_.size()); }

private:
    void index_lines();

    std::string path_;
    std::string text_;
    std::vector<std::size_t> line_starts_;
};

// ─── Diagnostics ─────────────────────────────────────────────────────────────

enum class Severity { Error, Warning, Note, Help };

// A note or help line attached under a diagnostic.
struct Attachment {
    Severity severity = Severity::Note;
    std::string text;
    // Optional secondary location, rendered as a trailing `[file:line]`.
    std::string where;
};

inline Attachment note(std::string text, std::string where = {}) {
    return Attachment{Severity::Note, std::move(text), std::move(where)};
}
inline Attachment help(std::string text, std::string where = {}) {
    return Attachment{Severity::Help, std::move(text), std::move(where)};
}

class Diagnostics {
public:
    // `limit` caps errors before the parser is told to give up (§16.3).
    Diagnostics(std::ostream& out, bool color, int limit = 20);

    void report(Severity sev, const char* code, const Source& src, Loc loc,
                std::string message, std::string caret_label = {},
                std::vector<Attachment> attachments = {});

    // Convenience wrappers.
    void error(const char* code, const Source& src, Loc loc, std::string message,
               std::string caret_label = {}, std::vector<Attachment> attachments = {}) {
        report(Severity::Error, code, src, loc, std::move(message),
               std::move(caret_label), std::move(attachments));
    }
    void warning(const char* code, const Source& src, Loc loc, std::string message,
                 std::string caret_label = {}, std::vector<Attachment> attachments = {}) {
        report(Severity::Warning, code, src, loc, std::move(message),
               std::move(caret_label), std::move(attachments));
    }

    int error_count() const { return errors_; }
    int warning_count() const { return warnings_; }

    // True once the error limit is hit; the parser stops rather than cascading.
    bool exhausted() const { return errors_ >= limit_; }

    void print_summary();

private:
    void paint(const char* seq);

    std::ostream& out_;
    bool color_;
    int limit_;
    int errors_ = 0;
    int warnings_ = 0;
    bool announced_limit_ = false;
};

}  // namespace se

#endif  // SE_DIAG_HPP
