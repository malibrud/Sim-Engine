#include "diag.hpp"

#include <cstdio>
#include <ostream>

namespace se {

// ─── Source ──────────────────────────────────────────────────────────────────

Source::Source(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
    index_lines();
}

void Source::index_lines() {
    line_starts_.clear();
    line_starts_.push_back(0);
    for (std::size_t i = 0; i < text_.size(); ++i) {
        const char c = text_[i];
        if (c == '\n') {
            line_starts_.push_back(i + 1);
        } else if (c == '\r') {
            // A lone CR is a line break; CRLF is one break, counted at the LF.
            if (i + 1 >= text_.size() || text_[i + 1] != '\n') line_starts_.push_back(i + 1);
        }
    }
}

bool Source::load(const std::string& path, Source& out, std::string& error) {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) {
        error = "cannot open file";
        return false;
    }
    std::string text;
    char buf[8192];
    std::size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
    const bool bad = std::ferror(f) != 0;
    std::fclose(f);
    if (bad) {
        error = "read error";
        return false;
    }
    // Strip a UTF-8 BOM so column numbers on line 1 are what the editor shows.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    out = Source(path, std::move(text));
    return true;
}

std::string_view Source::line(std::uint32_t n) const {
    if (n == 0 || n > line_starts_.size()) return {};
    const std::size_t start = line_starts_[n - 1];
    std::size_t end = (n < line_starts_.size()) ? line_starts_[n] : text_.size();
    while (end > start && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) --end;
    return std::string_view(text_.data() + start, end - start);
}

// ─── Diagnostics ─────────────────────────────────────────────────────────────

namespace {

const char* kReset  = "\x1b[0m";
const char* kBold   = "\x1b[1m";
const char* kRed    = "\x1b[1;31m";
const char* kYellow = "\x1b[1;33m";
const char* kCyan   = "\x1b[1;36m";
const char* kBlue   = "\x1b[1;34m";

const char* severity_word(Severity s) {
    switch (s) {
        case Severity::Error:   return "error";
        case Severity::Warning: return "warning";
        case Severity::Note:    return "note";
        case Severity::Help:    return "help";
    }
    return "error";
}

const char* severity_color(Severity s) {
    switch (s) {
        case Severity::Error:   return kRed;
        case Severity::Warning: return kYellow;
        case Severity::Note:    return kCyan;
        case Severity::Help:    return kCyan;
    }
    return kRed;
}

std::string digits_of(std::uint32_t v) {
    std::string s = std::to_string(v);
    return s;
}

// Expand tabs so the caret lands under the right glyph. Tab stops every 4.
std::string expand_tabs(std::string_view text, std::uint32_t& col_in_out,
                        std::uint32_t& span_in_out) {
    std::string out;
    std::uint32_t new_col = col_in_out;
    std::uint32_t new_span = span_in_out;
    std::uint32_t byte = 1;
    for (char c : text) {
        if (c == '\t') {
            const std::size_t pad = 4 - (out.size() % 4);
            if (byte < col_in_out) new_col += static_cast<std::uint32_t>(pad - 1);
            else if (byte < col_in_out + span_in_out) new_span += static_cast<std::uint32_t>(pad - 1);
            out.append(pad, ' ');
        } else {
            out.push_back(c);
        }
        ++byte;
    }
    col_in_out = new_col;
    span_in_out = new_span;
    return out;
}

}  // namespace

Diagnostics::Diagnostics(std::ostream& out, bool color, int limit)
    : out_(out), color_(color), limit_(limit) {}

void Diagnostics::paint(const char* seq) {
    if (color_) out_ << seq;
}

void Diagnostics::report(Severity sev, const char* code, const Source& src, Loc loc,
                         std::string message, std::string caret_label,
                         std::vector<Attachment> attachments) {
    if (sev == Severity::Error) {
        if (errors_ >= limit_) {
            if (!announced_limit_) {
                announced_limit_ = true;
                paint(kRed);
                out_ << "error";
                paint(kReset);
                out_ << ": too many errors; stopping after " << limit_ << "\n";
            }
            ++errors_;
            return;
        }
        ++errors_;
    } else if (sev == Severity::Warning) {
        ++warnings_;
    }

    // Header:  error[SE0230]: message
    paint(severity_color(sev));
    out_ << severity_word(sev);
    if (code && *code) out_ << '[' << code << ']';
    paint(kReset);
    paint(kBold);
    out_ << ": " << message << "\n";
    paint(kReset);

    const std::string gutter(digits_of(loc.line ? loc.line : 1).size(), ' ');

    // Location:   --> path:line:col
    paint(kBlue);
    out_ << gutter << "--> ";
    paint(kReset);
    out_ << src.path();
    if (loc.valid()) out_ << ':' << loc.line << ':' << loc.col;
    out_ << "\n";

    if (loc.valid()) {
        std::string_view raw = src.line(loc.line);
        std::uint32_t col = loc.col ? loc.col : 1;
        std::uint32_t span = loc.length ? loc.length : 1;
        const std::string text = expand_tabs(raw, col, span);

        // Clamp the caret to the line so a bad span never runs off the end.
        if (col > text.size() + 1) col = static_cast<std::uint32_t>(text.size() + 1);
        if (col + span > text.size() + 1) {
            span = static_cast<std::uint32_t>(text.size() + 1 - col);
            if (span == 0) span = 1;
        }

        paint(kBlue);
        out_ << gutter << " |\n";
        out_ << loc.line << " | ";
        paint(kReset);
        out_ << text << "\n";
        paint(kBlue);
        out_ << gutter << " | ";
        paint(kReset);
        out_ << std::string(col - 1, ' ');
        paint(severity_color(sev));
        out_ << std::string(span, '^');
        if (!caret_label.empty()) out_ << ' ' << caret_label;
        paint(kReset);
        out_ << "\n";
    }

    for (const Attachment& a : attachments) {
        paint(kBlue);
        out_ << gutter << " = ";
        paint(kReset);
        paint(kCyan);
        out_ << severity_word(a.severity);
        paint(kReset);
        out_ << ": " << a.text;
        if (!a.where.empty()) out_ << "  [" << a.where << "]";
        out_ << "\n";
    }
    out_ << "\n";
}

void Diagnostics::print_summary() {
    if (errors_ == 0 && warnings_ == 0) return;
    if (errors_ > 0) {
        paint(kRed);
        out_ << errors_ << (errors_ == 1 ? " error" : " errors");
        paint(kReset);
    }
    if (errors_ > 0 && warnings_ > 0) out_ << ", ";
    if (warnings_ > 0) {
        paint(kYellow);
        out_ << warnings_ << (warnings_ == 1 ? " warning" : " warnings");
        paint(kReset);
    }
    out_ << " emitted\n";
}

}  // namespace se
