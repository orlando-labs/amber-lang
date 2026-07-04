#pragma once

#include "frontend/lexer/token.h"

#include <cstdint>
#include <string>
#include <vector>

namespace amber::lexer {

struct LexResult {
  std::vector<Token> tokens;
  std::vector<Diagnostic> diagnostics;

  bool ok() const { return diagnostics.empty(); }
};

class Lexer {
public:
  Lexer(std::string source, std::string file);

  LexResult lex();

private:
  bool at_end() const;
  char current() const;
  char peek(std::size_t distance = 1) const;
  bool starts_with(const char *text) const;
  bool at_line_break() const;
  bool comment_starts_here() const;
  bool next_non_space_is_line_break_or_comment() const;
  bool has_inline_text_after_current() const;
  bool macro_splice_active() const;
  void update_macro_region(TokenKind kind, const std::string &lexeme);

  Position position() const;
  Span span_from(Position start) const;

  char advance();
  void advance_line_break();
  void emit(TokenKind kind, Position start, std::string lexeme);
  void error(Position start, const std::string &message);
  void error_code(Position start, const std::string &code,
                  const std::string &message);

  void lex_line_indent();
  void lex_identifier_or_keyword();
  void lex_number();
  void lex_string(char quote);
  void lex_text_block();
  bool consume_interpolation_in_string(Position interpolation_start);
  bool consume_nested_string_in_interpolation(char quote);
  bool validate_string_escape(Position escape_start, char quote);
  void consume_comment();

  bool is_identifier_start_at(std::size_t offset) const;
  bool is_identifier_part_at(std::size_t offset) const;

  static bool decode_utf8(const std::string &source, std::size_t offset,
                          std::uint32_t *codepoint, std::size_t *byte_count);
  static bool is_identifier_start(std::uint32_t codepoint);
  static bool is_identifier_part(std::uint32_t codepoint);
  static bool is_digit(char c);
  static bool is_hex_digit(char c);
  static bool is_keyword_text(const std::string &text, TokenKind *kind);
  static bool is_placeholder_text(const std::string &text);

  std::string source_;
  std::string file_;
  std::size_t index_ = 0;
  std::size_t line_ = 1;
  std::size_t col_ = 1;
  bool at_line_start_ = true;
  int bracket_depth_ = 0;
  bool one_line_block_active_ = false;
  std::vector<int> indent_stack_;
  // Macro template-region tracker (DESIGN-macro-system §6/§17 Q2). Inside a
  // `macro def` body `#{` opens a template splice hole instead of a comment;
  // a bare `#` still starts a comment. States: 0 = outside, 1 = saw `macro`,
  // 2 = inside the `macro def` header, 3 = after the header colon (covers
  // one-line bodies and the gap before the body INDENT), 4 = inside the
  // indented body until its matching DEDENT.
  int macro_region_state_ = 0;
  std::size_t macro_body_indent_size_ = 0;
  int macro_body_inline_tokens_ = 0;
  LexResult result_;
};

} // namespace amber::lexer
