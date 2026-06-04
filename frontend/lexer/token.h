#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace amber::lexer {

struct Position {
  std::size_t line = 1;
  std::size_t col = 1;
  std::size_t offset = 0;
};

struct Span {
  std::string file;
  Position start;
  Position end;
};

enum class TokenKind {
  Eof,
  Newline,
  Indent,
  Dedent,

  Identifier,
  Placeholder,
  LastValue,
  Integer,
  Float,
  String,

  Dot,
  DotDot,
  DotDotDot,
  ChainDot,
  SafeDot,
  Colon,
  Comma,
  LParen,
  RParen,
  LBracket,
  RBracket,
  LBrace,
  RBrace,
  Pipe,
  Caret,
  Bang,
  Question,

  Plus,
  Minus,
  Arrow,
  Star,
  Slash,
  SlashSlash,
  Percent,
  Equal,
  EqualEqual,
  EqualEqualEqual,
  BangEqual,
  Less,
  LessEqualGreater,
  LessEqual,
  Greater,
  GreaterEqual,
  At,
  AtAt,

  KeywordAttr,
  KeywordAnd,
  KeywordBreak,
  KeywordCase,
  KeywordCaseBang,
  KeywordClass,
  KeywordClassMethod,
  KeywordClassProp,
  KeywordDef,
  KeywordDo,
  KeywordElif,
  KeywordElse,
  KeywordElsif,
  KeywordExport,
  KeywordExtend,
  KeywordFalse,
  KeywordFrom,
  KeywordIf,
  KeywordImport,
  KeywordIn,
  KeywordInclude,
  KeywordLoop,
  KeywordMixin,
  KeywordNoop,
  KeywordNot,
  KeywordNull,
  KeywordOr,
  KeywordPackage,
  KeywordPass,
  KeywordProp,
  KeywordTrue,
  KeywordUnless,
  KeywordUntil,
  KeywordWhen,
  KeywordWhile
};

struct Token {
  TokenKind kind;
  std::string lexeme;
  Span span;
};

struct Diagnostic {
  std::string code;
  std::string severity;
  std::string phase;
  std::string message;
  Span span;
};

const char *token_kind_name(TokenKind kind);
std::string tokens_to_json(const std::vector<Token> &tokens,
                           const std::string &source_hash);
std::string diagnostics_to_json(const std::vector<Diagnostic> &diagnostics);
std::string sha256_hex(const std::string &source);

} // namespace amber::lexer
