#include "frontend/lexer/token.h"

#include <array>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string_view>

namespace amber::lexer {
namespace {

std::string json_escape(const std::string &value) {
  std::ostringstream out;
  for (unsigned char c : value) {
    switch (c) {
    case '"':
      out << "\\\"";
      break;
    case '\\':
      out << "\\\\";
      break;
    case '\b':
      out << "\\b";
      break;
    case '\f':
      out << "\\f";
      break;
    case '\n':
      out << "\\n";
      break;
    case '\r':
      out << "\\r";
      break;
    case '\t':
      out << "\\t";
      break;
    default:
      if (c < 0x20) {
        out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
            << static_cast<int>(c) << std::dec << std::setfill(' ');
      } else {
        out << static_cast<char>(c);
      }
    }
  }
  return out.str();
}

void append_position_json(std::ostringstream &out, const Position &position) {
  out << "{\"line\":" << position.line << ",\"col\":" << position.col
      << ",\"offset\":" << position.offset << "}";
}

void append_span_json(std::ostringstream &out, const Span &span) {
  out << "{\"file\":\"" << json_escape(span.file) << "\",\"start\":";
  append_position_json(out, span.start);
  out << ",\"end\":";
  append_position_json(out, span.end);
  out << "}";
}

constexpr std::uint32_t rotr(std::uint32_t value, std::uint32_t amount) {
  return (value >> amount) | (value << (32U - amount));
}

} // namespace

const char *token_kind_name(TokenKind kind) {
  switch (kind) {
  case TokenKind::Eof:
    return "EOF";
  case TokenKind::Newline:
    return "NEWLINE";
  case TokenKind::Indent:
    return "INDENT";
  case TokenKind::Dedent:
    return "DEDENT";
  case TokenKind::Identifier:
    return "IDENTIFIER";
  case TokenKind::Placeholder:
    return "PLACEHOLDER";
  case TokenKind::LastValue:
    return "LAST_VALUE";
  case TokenKind::Integer:
    return "INTEGER";
  case TokenKind::Float:
    return "FLOAT";
  case TokenKind::String:
    return "STRING";
  case TokenKind::Dot:
    return "DOT";
  case TokenKind::DotDot:
    return "DOT_DOT";
  case TokenKind::DotDotDot:
    return "DOT_DOT_DOT";
  case TokenKind::ChainDot:
    return "CHAIN_DOT";
  case TokenKind::SafeDot:
    return "SAFE_DOT";
  case TokenKind::Colon:
    return "COLON";
  case TokenKind::Comma:
    return "COMMA";
  case TokenKind::LParen:
    return "LPAREN";
  case TokenKind::RParen:
    return "RPAREN";
  case TokenKind::LBracket:
    return "LBRACKET";
  case TokenKind::RBracket:
    return "RBRACKET";
  case TokenKind::LBrace:
    return "LBRACE";
  case TokenKind::RBrace:
    return "RBRACE";
  case TokenKind::HashLBrace:
    return "HASH_LBRACE";
  case TokenKind::Pipe:
    return "PIPE";
  case TokenKind::Ampersand:
    return "AMPERSAND";
  case TokenKind::Caret:
    return "CARET";
  case TokenKind::Bang:
    return "BANG";
  case TokenKind::Question:
    return "QUESTION";
  case TokenKind::Plus:
    return "PLUS";
  case TokenKind::Minus:
    return "MINUS";
  case TokenKind::Arrow:
    return "ARROW";
  case TokenKind::Star:
    return "STAR";
  case TokenKind::StarStar:
    return "STAR_STAR";
  case TokenKind::Slash:
    return "SLASH";
  case TokenKind::SlashSlash:
    return "SLASH_SLASH";
  case TokenKind::Percent:
    return "PERCENT";
  case TokenKind::Equal:
    return "EQUAL";
  case TokenKind::EqualTilde:
    return "EQUAL_TILDE";
  case TokenKind::EqualEqual:
    return "EQUAL_EQUAL";
  case TokenKind::EqualEqualEqual:
    return "EQUAL_EQUAL_EQUAL";
  case TokenKind::BangTilde:
    return "BANG_TILDE";
  case TokenKind::BangEqual:
    return "BANG_EQUAL";
  case TokenKind::Less:
    return "LESS";
  case TokenKind::LessLess:
    return "LESS_LESS";
  case TokenKind::LessEqualGreater:
    return "LESS_EQUAL_GREATER";
  case TokenKind::LessEqual:
    return "LESS_EQUAL";
  case TokenKind::Greater:
    return "GREATER";
  case TokenKind::GreaterGreater:
    return "GREATER_GREATER";
  case TokenKind::GreaterEqual:
    return "GREATER_EQUAL";
  case TokenKind::At:
    return "AT";
  case TokenKind::AtAt:
    return "AT_AT";
  case TokenKind::KeywordAttr:
    return "KEYWORD_ATTR";
  case TokenKind::KeywordAnd:
    return "KEYWORD_AND";
  case TokenKind::KeywordBreak:
    return "KEYWORD_BREAK";
  case TokenKind::KeywordCatch:
    return "KEYWORD_CATCH";
  case TokenKind::KeywordCase:
    return "KEYWORD_CASE";
  case TokenKind::KeywordCaseBang:
    return "KEYWORD_CASE_BANG";
  case TokenKind::KeywordClass:
    return "KEYWORD_CLASS";
  case TokenKind::KeywordClassMethod:
    return "KEYWORD_CLASS_METHOD";
  case TokenKind::KeywordClassProp:
    return "KEYWORD_CLASS_PROP";
  case TokenKind::KeywordDef:
    return "KEYWORD_DEF";
  case TokenKind::KeywordDo:
    return "KEYWORD_DO";
  case TokenKind::KeywordElif:
    return "KEYWORD_ELIF";
  case TokenKind::KeywordElse:
    return "KEYWORD_ELSE";
  case TokenKind::KeywordElsif:
    return "KEYWORD_ELSIF";
  case TokenKind::KeywordExport:
    return "KEYWORD_EXPORT";
  case TokenKind::KeywordExtend:
    return "KEYWORD_EXTEND";
  case TokenKind::KeywordFalse:
    return "KEYWORD_FALSE";
  case TokenKind::KeywordFrom:
    return "KEYWORD_FROM";
  case TokenKind::KeywordIf:
    return "KEYWORD_IF";
  case TokenKind::KeywordImport:
    return "KEYWORD_IMPORT";
  case TokenKind::KeywordIn:
    return "KEYWORD_IN";
  case TokenKind::KeywordInclude:
    return "KEYWORD_INCLUDE";
  case TokenKind::KeywordLoop:
    return "KEYWORD_LOOP";
  case TokenKind::KeywordMixin:
    return "KEYWORD_MIXIN";
  case TokenKind::KeywordNot:
    return "KEYWORD_NOT";
  case TokenKind::KeywordNull:
    return "KEYWORD_NULL";
  case TokenKind::KeywordOr:
    return "KEYWORD_OR";
  case TokenKind::KeywordPackage:
    return "KEYWORD_PACKAGE";
  case TokenKind::KeywordProp:
    return "KEYWORD_PROP";
  case TokenKind::KeywordRaise:
    return "KEYWORD_RAISE";
  case TokenKind::KeywordRescue:
    return "KEYWORD_RESCUE";
  case TokenKind::KeywordReturn:
    return "KEYWORD_RETURN";
  case TokenKind::KeywordEnsure:
    return "KEYWORD_ENSURE";
  case TokenKind::KeywordThrow:
    return "KEYWORD_THROW";
  case TokenKind::KeywordTry:
    return "KEYWORD_TRY";
  case TokenKind::KeywordTrue:
    return "KEYWORD_TRUE";
  case TokenKind::KeywordUnless:
    return "KEYWORD_UNLESS";
  case TokenKind::KeywordUntil:
    return "KEYWORD_UNTIL";
  case TokenKind::KeywordWhen:
    return "KEYWORD_WHEN";
  case TokenKind::KeywordWhile:
    return "KEYWORD_WHILE";
  }
  return "UNKNOWN";
}

std::string tokens_to_json(const std::vector<Token> &tokens,
                           const std::string &source_hash) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.tokens.v1\",\n";
  out << "  \"source_hash\": \"sha256:" << source_hash << "\",\n";
  out << "  \"tokens\": [\n";
  for (std::size_t i = 0; i < tokens.size(); ++i) {
    const Token &token = tokens[i];
    out << "    {\"kind\":\"" << token_kind_name(token.kind)
        << "\",\"lexeme\":\"" << json_escape(token.lexeme) << "\",\"span\":";
    append_span_json(out, token.span);
    out << "}";
    if (i + 1 < tokens.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

std::string diagnostics_to_json(const std::vector<Diagnostic> &diagnostics) {
  std::ostringstream out;
  out << "{\n";
  out << "  \"format\": \"amber.diag.v1\",\n";
  out << "  \"diagnostics\": [\n";
  for (std::size_t i = 0; i < diagnostics.size(); ++i) {
    const Diagnostic &diagnostic = diagnostics[i];
    out << "    {\"code\":\"" << json_escape(diagnostic.code)
        << "\",\"severity\":\"" << json_escape(diagnostic.severity)
        << "\",\"phase\":\"" << json_escape(diagnostic.phase)
        << "\",\"message\":\"" << json_escape(diagnostic.message)
        << "\",\"primary_span\":";
    append_span_json(out, diagnostic.span);
    out << ",\"related\":[],\"notes\":[]}";
    if (i + 1 < diagnostics.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "  ]\n";
  out << "}\n";
  return out.str();
}

std::string sha256_hex(const std::string &source) {
  static constexpr std::array<std::uint32_t, 64> k = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  std::vector<unsigned char> bytes(source.begin(), source.end());
  const std::uint64_t bit_len = static_cast<std::uint64_t>(bytes.size()) * 8U;
  bytes.push_back(0x80U);
  while ((bytes.size() % 64U) != 56U) {
    bytes.push_back(0U);
  }
  for (int shift = 56; shift >= 0; shift -= 8) {
    bytes.push_back(static_cast<unsigned char>((bit_len >> shift) & 0xffU));
  }

  std::uint32_t h0 = 0x6a09e667U;
  std::uint32_t h1 = 0xbb67ae85U;
  std::uint32_t h2 = 0x3c6ef372U;
  std::uint32_t h3 = 0xa54ff53aU;
  std::uint32_t h4 = 0x510e527fU;
  std::uint32_t h5 = 0x9b05688cU;
  std::uint32_t h6 = 0x1f83d9abU;
  std::uint32_t h7 = 0x5be0cd19U;

  for (std::size_t chunk = 0; chunk < bytes.size(); chunk += 64U) {
    std::array<std::uint32_t, 64> w{};
    for (std::size_t i = 0; i < 16U; ++i) {
      const std::size_t j = chunk + i * 4U;
      w[i] = (static_cast<std::uint32_t>(bytes[j]) << 24U) |
             (static_cast<std::uint32_t>(bytes[j + 1U]) << 16U) |
             (static_cast<std::uint32_t>(bytes[j + 2U]) << 8U) |
             static_cast<std::uint32_t>(bytes[j + 3U]);
    }
    for (std::size_t i = 16U; i < 64U; ++i) {
      const std::uint32_t s0 =
          rotr(w[i - 15U], 7U) ^ rotr(w[i - 15U], 18U) ^ (w[i - 15U] >> 3U);
      const std::uint32_t s1 =
          rotr(w[i - 2U], 17U) ^ rotr(w[i - 2U], 19U) ^ (w[i - 2U] >> 10U);
      w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }

    std::uint32_t a = h0;
    std::uint32_t b = h1;
    std::uint32_t c = h2;
    std::uint32_t d = h3;
    std::uint32_t e = h4;
    std::uint32_t f = h5;
    std::uint32_t g = h6;
    std::uint32_t h = h7;

    for (std::size_t i = 0; i < 64U; ++i) {
      const std::uint32_t s1 = rotr(e, 6U) ^ rotr(e, 11U) ^ rotr(e, 25U);
      const std::uint32_t ch = (e & f) ^ ((~e) & g);
      const std::uint32_t temp1 = h + s1 + ch + k[i] + w[i];
      const std::uint32_t s0 = rotr(a, 2U) ^ rotr(a, 13U) ^ rotr(a, 22U);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t temp2 = s0 + maj;

      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }

    h0 += a;
    h1 += b;
    h2 += c;
    h3 += d;
    h4 += e;
    h5 += f;
    h6 += g;
    h7 += h;
  }

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::uint32_t part : {h0, h1, h2, h3, h4, h5, h6, h7}) {
    out << std::setw(8) << part;
  }
  return out.str();
}

} // namespace amber::lexer
