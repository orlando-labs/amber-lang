#include "frontend/lexer/lexer.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string_view>
#include <unordered_map>

namespace amber::lexer {
namespace {

constexpr std::string_view kLexErrorCode = "L0001";

} // namespace

Lexer::Lexer(std::string source, std::string file)
    : source_(std::move(source)), file_(std::move(file)), indent_stack_{0} {}

LexResult Lexer::lex() {
  while (!at_end()) {
    if (at_line_start_ && bracket_depth_ == 0) {
      lex_line_indent();
      if (at_end()) {
        break;
      }
      if (at_line_start_) {
        continue;
      }
    }

    const char c = current();
    if (c == ' ' || c == '\t') {
      advance();
      continue;
    }
    if (c == '#' && peek() == '{' && macro_splice_active()) {
      // Template splice hole inside a `macro def` body: `#{expr}` /
      // `#{*expr}`. Counts as an open bracket so the hole's `}` pairs up and
      // line breaks inside it stay suppressed.
      const Position start = position();
      advance();
      advance();
      ++bracket_depth_;
      emit(TokenKind::HashLBrace, start, "#{");
      continue;
    }
    if (c == '#' && comment_starts_here()) {
      consume_comment();
      continue;
    }
    if (at_line_break()) {
      const Position start = position();
      advance_line_break();
      one_line_block_active_ = false;
      if (bracket_depth_ == 0) {
        emit(TokenKind::Newline, start, "\n");
        at_line_start_ = true;
      }
      continue;
    }

    if (is_identifier_start_at(index_)) {
      lex_identifier_or_keyword();
      continue;
    }
    if (is_digit(c)) {
      lex_number();
      continue;
    }
    if (c == '"' && starts_with("\"\"\"")) {
      const bool raw = raw_next_string_literal_;
      raw_next_string_literal_ = false;
      lex_text_block(raw);
      continue;
    }
    if (c == '"' || c == '\'') {
      const bool raw = raw_next_string_literal_;
      raw_next_string_literal_ = false;
      lex_string(c, raw);
      continue;
    }

    const Position start = position();
    if (starts_with(".?.")) {
      advance();
      advance();
      advance();
      emit(TokenKind::SafeDot, start, ".?.");
      continue;
    }

    switch (c) {
    case '.': {
      const bool left_space = index_ > 0 && (source_[index_ - 1] == ' ' ||
                                             source_[index_ - 1] == '\t');
      advance();
      if (!at_end() && current() == '.') {
        advance();
        if (!at_end() && current() == '.') {
          advance();
          emit(TokenKind::DotDotDot, start, "...");
        } else {
          emit(TokenKind::DotDot, start, "..");
        }
        break;
      }
      emit(one_line_block_active_ && bracket_depth_ == 0 && left_space
               ? TokenKind::ChainDot
               : TokenKind::Dot,
           start, ".");
      break;
    }
    case ':':
      advance();
      emit(TokenKind::Colon, start, ":");
      if (bracket_depth_ == 0 && has_inline_text_after_current()) {
        one_line_block_active_ = true;
      }
      break;
    case ',':
      advance();
      emit(TokenKind::Comma, start, ",");
      break;
    case '(':
      advance();
      ++bracket_depth_;
      emit(TokenKind::LParen, start, "(");
      break;
    case ')':
      advance();
      if (bracket_depth_ > 0) {
        --bracket_depth_;
      }
      emit(TokenKind::RParen, start, ")");
      break;
    case '[':
      advance();
      ++bracket_depth_;
      emit(TokenKind::LBracket, start, "[");
      break;
    case ']':
      advance();
      if (bracket_depth_ > 0) {
        --bracket_depth_;
      }
      emit(TokenKind::RBracket, start, "]");
      break;
    case '{':
      advance();
      ++bracket_depth_;
      emit(TokenKind::LBrace, start, "{");
      break;
    case '}':
      advance();
      if (bracket_depth_ > 0) {
        --bracket_depth_;
      }
      emit(TokenKind::RBrace, start, "}");
      break;
    case '|':
      advance();
      emit(TokenKind::Pipe, start, "|");
      break;
    case '&':
      advance();
      emit(TokenKind::Ampersand, start, "&");
      break;
    case '^':
      advance();
      emit(TokenKind::Caret, start, "^");
      break;
    case '?':
      advance();
      emit(TokenKind::Question, start, "?");
      break;
    case '+':
      advance();
      emit(TokenKind::Plus, start, "+");
      break;
    case '-':
      advance();
      if (!at_end() && current() == '>') {
        advance();
        emit(TokenKind::Arrow, start, "->");
      } else {
        emit(TokenKind::Minus, start, "-");
      }
      break;
    case '*':
      advance();
      if (!at_end() && current() == '*') {
        advance();
        emit(TokenKind::StarStar, start, "**");
      } else {
        emit(TokenKind::Star, start, "*");
      }
      break;
    case '/':
      advance();
      if (!at_end() && current() == '/') {
        advance();
        emit(TokenKind::SlashSlash, start, "//");
      } else {
        emit(TokenKind::Slash, start, "/");
      }
      break;
    case '%':
      advance();
      emit(TokenKind::Percent, start, "%");
      break;
    case '=':
      advance();
      if (!at_end() && current() == '~') {
        advance();
        emit(TokenKind::EqualTilde, start, "=~");
      } else if (!at_end() && current() == '=' && peek() == '=') {
        advance();
        advance();
        emit(TokenKind::EqualEqualEqual, start, "===");
      } else if (!at_end() && current() == '=') {
        advance();
        emit(TokenKind::EqualEqual, start, "==");
      } else {
        emit(TokenKind::Equal, start, "=");
      }
      break;
    case '!':
      advance();
      if (!at_end() && current() == '~') {
        advance();
        emit(TokenKind::BangTilde, start, "!~");
      } else if (!at_end() && current() == '=') {
        advance();
        emit(TokenKind::BangEqual, start, "!=");
      } else {
        emit(TokenKind::Bang, start, "!");
      }
      break;
    case '<':
      advance();
      if (!at_end() && current() == '<') {
        advance();
        emit(TokenKind::LessLess, start, "<<");
      } else if (!at_end() && current() == '=' && peek() == '>') {
        advance();
        advance();
        emit(TokenKind::LessEqualGreater, start, "<=>");
      } else if (!at_end() && current() == '=') {
        advance();
        emit(TokenKind::LessEqual, start, "<=");
      } else {
        emit(TokenKind::Less, start, "<");
      }
      break;
    case '>':
      advance();
      if (!at_end() && current() == '>') {
        advance();
        emit(TokenKind::GreaterGreater, start, ">>");
      } else if (!at_end() && current() == '=') {
        advance();
        emit(TokenKind::GreaterEqual, start, ">=");
      } else {
        emit(TokenKind::Greater, start, ">");
      }
      break;
    case '@':
      advance();
      if (!at_end() && current() == '@') {
        advance();
        emit(TokenKind::AtAt, start, "@@");
      } else {
        emit(TokenKind::At, start, "@");
      }
      break;
    case '$':
      advance();
      if (!at_end() && current() == '_') {
        advance();
        emit(TokenKind::LastValue, start, "$_");
      } else {
        error(start, "unexpected '$'; only '$_' is reserved in Amber v1");
      }
      break;
    case ';':
      // `;` is an explicit statement separator, lexed as a same-line newline so
      // every existing newline-terminated statement rule applies unchanged. It
      // exists for ad-hoc one-liners (e.g. `iamber --eval 'a = 1; print(a)'`);
      // it is highly discouraged in normal code, where line breaks are the
      // idiomatic separator. It carries no indentation effect (Indent/Dedent
      // are computed only at physical line starts).
      advance();
      emit(TokenKind::Newline, start, ";");
      break;
    default:
      advance();
      error(start, std::string("unexpected character '") + c + "'");
      break;
    }
  }

  const Position eof_start = position();
  if (!result_.tokens.empty() &&
      result_.tokens.back().kind != TokenKind::Newline &&
      result_.tokens.back().kind != TokenKind::Indent &&
      result_.tokens.back().kind != TokenKind::Dedent) {
    emit(TokenKind::Newline, eof_start, "\n");
  }
  while (indent_stack_.size() > 1) {
    indent_stack_.pop_back();
    emit(TokenKind::Dedent, eof_start, "");
  }
  emit(TokenKind::Eof, eof_start, "");
  return std::move(result_);
}

bool Lexer::at_end() const { return index_ >= source_.size(); }

char Lexer::current() const { return at_end() ? '\0' : source_[index_]; }

char Lexer::peek(std::size_t distance) const {
  const std::size_t target = index_ + distance;
  return target >= source_.size() ? '\0' : source_[target];
}

bool Lexer::starts_with(const char *text) const {
  for (std::size_t i = 0; text[i] != '\0'; ++i) {
    if (index_ + i >= source_.size() || source_[index_ + i] != text[i]) {
      return false;
    }
  }
  return true;
}

bool Lexer::at_line_break() const {
  return current() == '\n' || current() == '\r';
}

bool Lexer::comment_starts_here() const {
  if (current() != '#') {
    return false;
  }
  if (index_ == 0) {
    return true;
  }

  std::size_t cursor = index_;
  while (cursor > 0 && source_[cursor - 1] != '\n' &&
         source_[cursor - 1] != '\r') {
    --cursor;
  }

  bool first_non_space = true;
  for (std::size_t i = cursor; i < index_; ++i) {
    if (source_[i] != ' ' && source_[i] != '\t') {
      first_non_space = false;
      break;
    }
  }
  if (first_non_space) {
    return true;
  }

  const char previous = source_[index_ - 1];
  return previous == ' ' || previous == '\t';
}

bool Lexer::next_non_space_is_line_break_or_comment() const {
  std::size_t cursor = index_;
  while (cursor < source_.size() &&
         (source_[cursor] == ' ' || source_[cursor] == '\t')) {
    ++cursor;
  }
  if (cursor >= source_.size() || source_[cursor] == '\n' ||
      source_[cursor] == '\r') {
    return true;
  }
  if (source_[cursor] == '#') {
    // Inside a macro-def body a line may start with a `#{` splice hole; that
    // is code, not a comment.
    return !(macro_splice_active() && cursor + 1 < source_.size() &&
             source_[cursor + 1] == '{');
  }
  return false;
}

bool Lexer::has_inline_text_after_current() const {
  std::size_t cursor = index_;
  while (cursor < source_.size() &&
         (source_[cursor] == ' ' || source_[cursor] == '\t')) {
    ++cursor;
  }
  if (cursor >= source_.size() || source_[cursor] == '\n' ||
      source_[cursor] == '\r') {
    return false;
  }
  if (source_[cursor] == '#') {
    // A `#{` splice hole in a macro-def body is inline text; a comment is not.
    return macro_splice_active() && cursor + 1 < source_.size() &&
           source_[cursor + 1] == '{';
  }
  return true;
}

Position Lexer::position() const { return Position{line_, col_, index_}; }

Span Lexer::span_from(Position start) const {
  return Span{file_, start, position()};
}

char Lexer::advance() {
  const char c = source_[index_];
  std::uint32_t codepoint = 0;
  std::size_t byte_count = 1;
  if (static_cast<unsigned char>(c) >= 0x80U &&
      decode_utf8(source_, index_, &codepoint, &byte_count)) {
    index_ += byte_count;
    ++col_;
    return c;
  }

  ++index_;
  if (c == '\n' || c == '\r') {
    ++line_;
    col_ = 1;
  } else {
    ++col_;
  }
  return c;
}

void Lexer::advance_line_break() {
  if (current() == '\r' && peek() == '\n') {
    ++index_;
    ++index_;
    ++line_;
    col_ = 1;
    return;
  }
  advance();
}

void Lexer::emit(TokenKind kind, Position start, std::string lexeme) {
  update_macro_region(kind, lexeme);
  result_.tokens.push_back(Token{kind, std::move(lexeme), span_from(start)});
}

bool Lexer::macro_splice_active() const {
  return macro_region_state_ == 3 || macro_region_state_ == 4;
}

// Token-driven tracker for `macro def` body regions (see lexer.h). Runs on
// every emitted token; `#{` becomes a splice hole only while a body region is
// open, so ordinary code and comments elsewhere are untouched.
void Lexer::update_macro_region(TokenKind kind, const std::string &lexeme) {
  switch (macro_region_state_) {
  case 0:
    if (kind == TokenKind::Identifier && lexeme == "macro") {
      macro_region_state_ = 1;
    }
    break;
  case 1:
    if (kind == TokenKind::KeywordDef) {
      macro_region_state_ = 2;
    } else if (!(kind == TokenKind::Identifier && lexeme == "macro")) {
      macro_region_state_ = 0;
    }
    break;
  case 2:
    // The header ends at the first `:` outside brackets (signature colons all
    // sit inside parens/brackets). A header line without one is malformed;
    // reset at the line break.
    if (kind == TokenKind::Colon && bracket_depth_ == 0) {
      macro_region_state_ = 3;
      macro_body_inline_tokens_ = 0;
    } else if (kind == TokenKind::Newline || kind == TokenKind::Eof) {
      macro_region_state_ = 0;
    }
    break;
  case 3:
    if (kind == TokenKind::Indent) {
      macro_region_state_ = 4;
      macro_body_indent_size_ = indent_stack_.size();
    } else if (kind == TokenKind::Newline) {
      // A one-line body (tokens between the colon and the newline) ends here;
      // a blank line before an indented body keeps the region pending.
      if (macro_body_inline_tokens_ > 0) {
        macro_region_state_ = 0;
      }
    } else if (kind == TokenKind::Dedent || kind == TokenKind::Eof) {
      macro_region_state_ = 0;
    } else {
      ++macro_body_inline_tokens_;
    }
    break;
  case 4:
    // Blocks nested inside the body push deeper indent levels; the region
    // closes only when a DEDENT pops below the body's own level.
    if (kind == TokenKind::Dedent &&
        indent_stack_.size() < macro_body_indent_size_) {
      macro_region_state_ = 0;
    }
    break;
  default:
    break;
  }
}

void Lexer::error(Position start, const std::string &message) {
  error_code(start, std::string(kLexErrorCode), message);
}

void Lexer::error_code(Position start, const std::string &code,
                       const std::string &message) {
  result_.diagnostics.push_back(
      Diagnostic{code, "error", "lexer", message, span_from(start)});
}

void Lexer::lex_line_indent() {
  int indent = 0;
  const Position indent_start = position();
  while (!at_end() && (current() == ' ' || current() == '\t')) {
    if (current() == '\t') {
      error(position(), "tabs are not allowed in indentation");
    }
    ++indent;
    advance();
  }

  if (next_non_space_is_line_break_or_comment()) {
    if (!at_end() && current() == '#') {
      consume_comment();
    }
    if (!at_end() && at_line_break()) {
      advance_line_break();
    }
    at_line_start_ = true;
    return;
  }

  at_line_start_ = false;
  const int current_indent = indent_stack_.back();
  if (indent > current_indent) {
    indent_stack_.push_back(indent);
    emit(TokenKind::Indent, indent_start, "");
    return;
  }
  while (indent < indent_stack_.back()) {
    indent_stack_.pop_back();
    emit(TokenKind::Dedent, indent_start, "");
  }
  if (indent != indent_stack_.back()) {
    error(indent_start, "indentation does not match any outer block");
  }
}

void Lexer::lex_identifier_or_keyword() {
  const Position start = position();
  while (!at_end() && is_identifier_part_at(index_)) {
    advance();
  }

  if (!at_end() && (current() == '?' || current() == '!')) {
    const char suffix = current();
    advance();
    if (!at_end() && is_identifier_part_at(index_)) {
      error(start,
            "identifier suffix '?' or '!' must terminate the identifier");
      while (!at_end() && is_identifier_part_at(index_)) {
        advance();
      }
    }
    const std::string text =
        source_.substr(start.offset, position().offset - start.offset);
    if (text == "case!") {
      emit(TokenKind::KeywordCaseBang, start, text);
      return;
    }
    if (suffix == '!' && text == "case!") {
      emit(TokenKind::KeywordCaseBang, start, text);
      return;
    }
    emit(TokenKind::Identifier, start, text);
    return;
  }

  const std::string text =
      source_.substr(start.offset, position().offset - start.offset);
  TokenKind keyword = TokenKind::Identifier;
  const bool raw_regexp_tag = text == "r" && !at_end() && current() == '"';
  if (is_keyword_text(text, &keyword)) {
    emit(keyword, start, text);
  } else if (is_placeholder_text(text)) {
    emit(TokenKind::Placeholder, start, text);
  } else {
    emit(TokenKind::Identifier, start, text);
  }
  raw_next_string_literal_ = raw_regexp_tag;
}

void Lexer::lex_number() {
  const Position start = position();
  bool is_float = false;

  auto lower_ascii = [](char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
  };
  auto is_ascii_alpha = [](char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
  };
  auto is_digit_for_base = [&](char c, int base) {
    if (c >= '0' && c <= '9') {
      return (c - '0') < base;
    }
    const char lower = lower_ascii(c);
    return base == 16 && lower >= 'a' && lower <= 'f';
  };
  auto consume_digit_run = [&](int base, bool *saw_digit, bool *underscore_ok) {
    bool previous_underscore = false;
    while (!at_end()) {
      const char c = current();
      if (c == '_') {
        if (!*saw_digit || previous_underscore) {
          *underscore_ok = false;
        }
        previous_underscore = true;
        advance();
        continue;
      }
      if (!is_digit_for_base(c, base)) {
        break;
      }
      *saw_digit = true;
      previous_underscore = false;
      advance();
    }
    if (previous_underscore) {
      *underscore_ok = false;
    }
  };

  bool saw_digit = false;
  bool underscore_ok = true;
  bool base_literal = false;
  if (current() == '0') {
    const char prefix = lower_ascii(peek());
    int base = 10;
    if (prefix == 'x' || prefix == 'b' || prefix == 'o') {
      base_literal = true;
      base = prefix == 'x' ? 16 : (prefix == 'b' ? 2 : 8);
      advance();
      advance();
      consume_digit_run(base, &saw_digit, &underscore_ok);
      if (!saw_digit) {
        error(start, "numeric literal prefix must be followed by digits");
      }
      if (!underscore_ok) {
        error(start, "numeric literal underscores must separate digits");
      }
      if (!at_end() && (is_digit(current()) || is_ascii_alpha(current()) ||
                        current() == '_')) {
        error(position(), "invalid digit in numeric literal");
        while (!at_end() && (is_digit(current()) || is_ascii_alpha(current()) ||
                             current() == '_')) {
          advance();
        }
      }
      const std::string text =
          source_.substr(start.offset, position().offset - start.offset);
      emit(TokenKind::Integer, start, text);
      return;
    }
  }

  consume_digit_run(10, &saw_digit, &underscore_ok);

  if (!base_literal && !at_end() && current() == '.' && peek() != '.' &&
      (is_digit(peek()) || peek() == '_')) {
    is_float = true;
    advance();
    bool fraction_digit = false;
    consume_digit_run(10, &fraction_digit, &underscore_ok);
    if (!fraction_digit) {
      error(start, "float literal fraction must contain digits");
    }
  }

  if (!base_literal && !at_end() && (current() == 'e' || current() == 'E')) {
    is_float = true;
    advance();
    if (!at_end() && (current() == '+' || current() == '-')) {
      advance();
    }
    bool exponent_digit = false;
    consume_digit_run(10, &exponent_digit, &underscore_ok);
    if (!exponent_digit) {
      error(start, "float literal exponent must contain digits");
    }
  }

  if (!underscore_ok) {
    error(start, "numeric literal underscores must separate digits");
  }
  if (!at_end() && is_identifier_start_at(index_)) {
    error(position(), "numeric literal must be separated from identifier text");
    while (!at_end() && is_identifier_part_at(index_)) {
      advance();
    }
  }

  const std::string text =
      source_.substr(start.offset, position().offset - start.offset);
  emit(is_float ? TokenKind::Float : TokenKind::Integer, start, text);
}

void Lexer::lex_string(char quote, bool raw) {
  const Position start = position();
  advance();
  bool closed = false;
  bool emitted_unterminated_interpolation = false;
  while (!at_end()) {
    if (current() == '\\') {
      const Position escape_start = position();
      advance();
      if (raw) {
        if (!at_end() && !at_line_break()) {
          advance();
        }
      } else {
        validate_string_escape(escape_start, quote);
      }
      continue;
    }
    if (!raw && quote == '"' && current() == '#' && peek() == '{') {
      const Position interpolation_start = position();
      advance();
      advance();
      if (!consume_interpolation_in_string(interpolation_start)) {
        emitted_unterminated_interpolation = true;
        break;
      }
      continue;
    }
    if (current() == quote) {
      advance();
      closed = true;
      break;
    }
    if (at_line_break()) {
      break;
    }
    advance();
  }

  if (!closed) {
    if (!emitted_unterminated_interpolation) {
      error_code(start, "AMB_STRING_UNTERMINATED",
                 "unterminated string literal");
    }
  }
  const std::string text =
      source_.substr(start.offset, position().offset - start.offset);
  emit(TokenKind::String, start, text);
}

// Multiline text block `"""…"""` (DESIGN-multiline-string-literals §4/§5/§8).
// The span is layout-suspended (no NEWLINE/INDENT/DEDENT inside); the body is
// dedented by the CLOSING delimiter's column, trailing whitespace per line is
// stripped, a body line ending in `\` joins the next line (suppresses the
// newline; on the last line it suppresses the trailing newline), and blank
// lines contribute just `\n`. Escapes and `#{…}` interpolation are the §7
// double-quote forms; interpolation expressions may span lines. The emitted
// token keeps the `"""` delimiters so the parser can tell the block form
// apart (quote_kind "block"); its content is the dedented text, so the
// existing parts scanning applies unchanged.
void Lexer::lex_text_block(bool raw) {
  const Position start = position();
  advance();
  advance();
  advance();
  // Opener rule (§8): only whitespace may follow the opener on its line.
  while (!at_end() && (current() == ' ' || current() == '\t')) {
    advance();
  }
  if (at_end() || !at_line_break()) {
    error_code(start, "AMB_TEXTBLOCK_OPENER",
               "text block opener `\"\"\"` must be immediately followed by a "
               "newline");
    while (!at_end() && !at_line_break()) {
      advance();
    }
  }
  if (!at_end()) {
    advance_line_break();
  }

  // Collect logical lines. `#{…}` interpolations are copied verbatim
  // (including any newlines inside them) so dedent and the closer scan only
  // see line starts outside interpolation.
  std::vector<std::string> lines;
  std::size_t closer_indent = 0;
  bool closed = false;
  while (!at_end()) {
    std::size_t indent = 0;
    while (!at_end() && current() == ' ') {
      ++indent;
      advance();
    }
    if (!at_end() && current() == '\t') {
      error(position(), "tabs are not allowed in text block indentation");
      while (!at_end() && current() == '\t') {
        advance();
      }
    }
    if (starts_with("\"\"\"")) {
      advance();
      advance();
      advance();
      closer_indent = indent;
      closed = true;
      break;
    }
    std::string line(indent, ' ');
    while (!at_end() && !at_line_break()) {
      if (!raw && current() == '#' && peek() == '{') {
        const std::size_t from = index_;
        const Position interpolation_start = position();
        advance();
        advance();
        if (!consume_interpolation_in_string(interpolation_start)) {
          break;
        }
        line.append(source_, from, index_ - from);
        continue;
      }
      if (current() == '\\') {
        line.push_back(advance());
        if (!at_end() && !at_line_break()) {
          const char escaped = current();
          if (!raw && escaped != 'n' && escaped != 'r' && escaped != 't' &&
              escaped != '\\' && escaped != '"' && escaped != '#' &&
              escaped != '\'' && escaped != 'u') {
            error_code(position(), "AMB_STRING_BAD_ESCAPE",
                       "invalid escape sequence in text block");
          }
          line.push_back(advance());
        }
        continue;
      }
      line.push_back(advance());
    }
    lines.push_back(std::move(line));
    if (!at_end()) {
      advance_line_break();
    }
  }
  if (!closed) {
    error_code(start, "AMB_TEXTBLOCK_UNTERMINATED", "unterminated text block");
  }

  std::string content;
  for (std::string &line : lines) {
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }
    if (line.empty()) {
      content.push_back('\n');
      continue;
    }
    std::size_t leading = 0;
    while (leading < line.size() && line[leading] == ' ') {
      ++leading;
    }
    if (leading < closer_indent) {
      error_code(start, "AMB_TEXTBLOCK_UNDERINDENT",
                 "text block line is indented less than the closing `\"\"\"`");
    }
    line.erase(0, std::min(leading, closer_indent));
    // A trailing run of backslashes with odd length ends in the line
    // continuation marker: drop it and suppress this line's newline.
    std::size_t backslashes = 0;
    while (backslashes < line.size() &&
           line[line.size() - 1U - backslashes] == '\\') {
      ++backslashes;
    }
    const bool continuation = (backslashes % 2U) == 1U;
    if (continuation) {
      line.pop_back();
    }
    content += line;
    if (!continuation) {
      content.push_back('\n');
    }
  }

  emit(TokenKind::String, start, "\"\"\"" + content + "\"\"\"");
}

bool Lexer::consume_interpolation_in_string(Position interpolation_start) {
  int depth = 0;
  while (!at_end()) {
    if (current() == '\\') {
      advance();
      if (!at_end()) {
        advance();
      }
      continue;
    }
    if (current() == '"' || current() == '\'') {
      if (!consume_nested_string_in_interpolation(current())) {
        error_code(interpolation_start, "AMB_STRING_INTERP_UNTERMINATED",
                   "unterminated string interpolation");
        return false;
      }
      continue;
    }
    if (current() == '{') {
      ++depth;
      advance();
      continue;
    }
    if (current() == '}') {
      if (depth == 0) {
        advance();
        return true;
      }
      --depth;
      advance();
      continue;
    }
    advance();
  }

  error_code(interpolation_start, "AMB_STRING_INTERP_UNTERMINATED",
             "unterminated string interpolation");
  return false;
}

bool Lexer::consume_nested_string_in_interpolation(char quote) {
  advance();
  while (!at_end()) {
    if (current() == '\\') {
      advance();
      if (!at_end()) {
        advance();
      }
      continue;
    }
    if (current() == quote) {
      advance();
      return true;
    }
    advance();
  }
  return false;
}

bool Lexer::validate_string_escape(Position escape_start, char quote) {
  if (at_end()) {
    error_code(escape_start, "AMB_STRING_BAD_ESCAPE",
               "unterminated escape sequence in string literal");
    return false;
  }
  if (at_line_break()) {
    error_code(escape_start, "AMB_STRING_BAD_ESCAPE",
               "unterminated escape sequence in string literal");
    return false;
  }

  const char escaped = current();
  switch (escaped) {
  case 'n':
  case 'r':
  case 't':
  case '\\':
  case '"':
  case '#':
    advance();
    return true;
  case '\'':
    if (quote == '\'') {
      advance();
      return true;
    }
    break;
  case 'u': {
    advance();
    if (at_end() || current() != '{') {
      error_code(escape_start, "AMB_STRING_BAD_ESCAPE",
                 "unicode escape must use \\u{HEX}");
      return false;
    }
    advance();
    std::uint32_t codepoint = 0;
    int digits = 0;
    while (!at_end() && current() != '}') {
      const char c = current();
      if (!is_hex_digit(c)) {
        error_code(position(), "AMB_STRING_BAD_ESCAPE",
                   "unicode escape contains a non-hex digit");
        advance();
        continue;
      }
      std::uint32_t digit = 0;
      if (c >= '0' && c <= '9') {
        digit = static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        digit = static_cast<std::uint32_t>(c - 'a' + 10);
      } else {
        digit = static_cast<std::uint32_t>(c - 'A' + 10);
      }
      codepoint = (codepoint << 4U) | digit;
      ++digits;
      advance();
    }
    if (at_end() || current() != '}') {
      error_code(escape_start, "AMB_STRING_BAD_ESCAPE",
                 "unicode escape must end with '}'");
      return false;
    }
    advance();
    if (digits == 0 || digits > 6) {
      error_code(escape_start, "AMB_STRING_BAD_ESCAPE",
                 "unicode escape must contain 1 to 6 hex digits");
      return false;
    }
    if (codepoint > 0x10FFFFU ||
        (codepoint >= 0xD800U && codepoint <= 0xDFFFU)) {
      error_code(escape_start, "AMB_STRING_BAD_ESCAPE",
                 "unicode escape codepoint is invalid");
      return false;
    }
    return true;
  }
  default:
    break;
  }

  error_code(escape_start, "AMB_STRING_BAD_ESCAPE",
             "invalid escape sequence in string literal");
  advance();
  return false;
}

void Lexer::consume_comment() {
  while (!at_end() && !at_line_break()) {
    advance();
  }
}

bool Lexer::is_identifier_start_at(std::size_t offset) const {
  std::uint32_t codepoint = 0;
  std::size_t byte_count = 0;
  if (!decode_utf8(source_, offset, &codepoint, &byte_count)) {
    return false;
  }
  return is_identifier_start(codepoint);
}

bool Lexer::is_identifier_part_at(std::size_t offset) const {
  std::uint32_t codepoint = 0;
  std::size_t byte_count = 0;
  if (!decode_utf8(source_, offset, &codepoint, &byte_count)) {
    return false;
  }
  return is_identifier_part(codepoint);
}

bool Lexer::decode_utf8(const std::string &source, std::size_t offset,
                        std::uint32_t *codepoint, std::size_t *byte_count) {
  if (offset >= source.size()) {
    return false;
  }

  const auto byte0 = static_cast<unsigned char>(source[offset]);
  if (byte0 < 0x80U) {
    *codepoint = byte0;
    *byte_count = 1;
    return true;
  }

  std::uint32_t value = 0;
  std::size_t count = 0;
  if ((byte0 & 0xE0U) == 0xC0U) {
    value = byte0 & 0x1FU;
    count = 2;
  } else if ((byte0 & 0xF0U) == 0xE0U) {
    value = byte0 & 0x0FU;
    count = 3;
  } else if ((byte0 & 0xF8U) == 0xF0U) {
    value = byte0 & 0x07U;
    count = 4;
  } else {
    return false;
  }

  if (offset + count > source.size()) {
    return false;
  }
  for (std::size_t i = 1; i < count; ++i) {
    const auto byte = static_cast<unsigned char>(source[offset + i]);
    if ((byte & 0xC0U) != 0x80U) {
      return false;
    }
    value = (value << 6U) | (byte & 0x3FU);
  }

  if ((count == 2 && value < 0x80U) || (count == 3 && value < 0x800U) ||
      (count == 4 && value < 0x10000U) || value > 0x10FFFFU ||
      (value >= 0xD800U && value <= 0xDFFFU)) {
    return false;
  }

  *codepoint = value;
  *byte_count = count;
  return true;
}

bool Lexer::is_identifier_start(std::uint32_t codepoint) {
  const bool ascii_alpha = (codepoint >= 'A' && codepoint <= 'Z') ||
                           (codepoint >= 'a' && codepoint <= 'z');
  const bool cyrillic = (codepoint >= 0x0400U && codepoint <= 0x052FU) ||
                        (codepoint >= 0x1C80U && codepoint <= 0x1C8FU) ||
                        (codepoint >= 0x2DE0U && codepoint <= 0x2DFFU) ||
                        (codepoint >= 0xA640U && codepoint <= 0xA69FU);
  const bool greek = (codepoint >= 0x0370U && codepoint <= 0x03FFU) ||
                     (codepoint >= 0x1F00U && codepoint <= 0x1FFFU);
  return ascii_alpha || cyrillic || greek || codepoint == '_';
}

bool Lexer::is_identifier_part(std::uint32_t codepoint) {
  return is_identifier_start(codepoint) ||
         (codepoint >= '0' && codepoint <= '9');
}

bool Lexer::is_digit(char c) { return c >= '0' && c <= '9'; }

bool Lexer::is_hex_digit(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
         (c >= 'A' && c <= 'F');
}

bool Lexer::is_keyword_text(const std::string &text, TokenKind *kind) {
  static const std::unordered_map<std::string_view, TokenKind> keywords = {
      {"and", TokenKind::KeywordAnd},
      {"attr", TokenKind::KeywordAttr},
      {"break", TokenKind::KeywordBreak},
      {"catch", TokenKind::KeywordCatch},
      {"case", TokenKind::KeywordCase},
      {"class", TokenKind::KeywordClass},
      {"class_method", TokenKind::KeywordClassMethod},
      {"class_prop", TokenKind::KeywordClassProp},
      {"def", TokenKind::KeywordDef},
      {"do", TokenKind::KeywordDo},
      {"elif", TokenKind::KeywordElif},
      {"else", TokenKind::KeywordElse},
      {"elsif", TokenKind::KeywordElsif},
      {"export", TokenKind::KeywordExport},
      {"ensure", TokenKind::KeywordEnsure},
      {"extend", TokenKind::KeywordExtend},
      {"false", TokenKind::KeywordFalse},
      {"from", TokenKind::KeywordFrom},
      {"if", TokenKind::KeywordIf},
      {"import", TokenKind::KeywordImport},
      {"in", TokenKind::KeywordIn},
      {"include", TokenKind::KeywordInclude},
      {"loop", TokenKind::KeywordLoop},
      {"mixin", TokenKind::KeywordMixin},
      {"noop", TokenKind::KeywordNoop},
      {"not", TokenKind::KeywordNot},
      {"null", TokenKind::KeywordNull},
      {"or", TokenKind::KeywordOr},
      {"package", TokenKind::KeywordPackage},
      {"pass", TokenKind::KeywordPass},
      {"prop", TokenKind::KeywordProp},
      {"raise", TokenKind::KeywordRaise},
      {"rescue", TokenKind::KeywordRescue},
      {"return", TokenKind::KeywordReturn},
      {"throw", TokenKind::KeywordThrow},
      {"try", TokenKind::KeywordTry},
      {"true", TokenKind::KeywordTrue},
      {"unless", TokenKind::KeywordUnless},
      {"until", TokenKind::KeywordUntil},
      {"when", TokenKind::KeywordWhen},
      {"while", TokenKind::KeywordWhile},
  };
  const auto found = keywords.find(text);
  if (found == keywords.end()) {
    return false;
  }
  *kind = found->second;
  return true;
}

bool Lexer::is_placeholder_text(const std::string &text) {
  if (text.size() < 2 || text[0] != '_') {
    return false;
  }
  for (std::size_t i = 1; i < text.size(); ++i) {
    if (!is_digit(text[i])) {
      return false;
    }
  }
  return true;
}

} // namespace amber::lexer
