#include "veylqora/retrieval/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_set>

namespace veylqora {
namespace {

bool is_alnum(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

bool is_upper(char c) { return c >= 'A' && c <= 'Z'; }

bool is_lower(char c) { return c >= 'a' && c <= 'z'; }

char to_lower(char c) {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Common English and boilerplate programming stopwords that carry near-zero
// discriminative signal in natural-language bug reports or code bodies.
bool is_stopword(std::string_view t) {
  static const std::unordered_set<std::string_view> kStop = {
      "about",   "above",   "after",   "again",   "all",     "also",  "always", "am",
      "an",      "and",     "any",     "are",     "as",      "at",    "be",     "because",
      "been",    "before",  "being",   "below",   "between", "both",  "but",    "by",
      "can",     "cannot",  "could",   "def",     "did",     "do",    "does",   "doing",
      "done",    "down",    "during",  "each",    "else",    "even",  "every",  "false",
      "for",     "from",    "further", "get",     "gets",    "got",   "had",    "has",
      "have",    "having",  "he",      "her",     "here",    "hers",  "him",    "his",
      "how",     "however", "if",      "in",      "into",    "is",    "it",     "its",
      "itself",  "just",    "let",     "like",    "make",    "many",  "may",    "me",
      "might",   "more",    "most",    "much",    "must",    "my",    "need",   "new",
      "no",      "none",    "nor",     "not",     "now",     "null",  "of",     "off",
      "on",      "once",    "one",     "only",    "or",      "other", "our",    "out",
      "over",    "own",     "pass",    "return",  "returns", "same",  "see",    "self",
      "set",     "she",     "should",  "since",   "so",      "some",  "such",   "take",
      "than",    "that",    "the",     "their",   "them",    "then",  "there",  "these",
      "they",    "this",    "those",   "through", "to",      "too",   "true",   "under",
      "until",   "up",      "use",     "used",    "using",   "var",   "very",   "via",
      "want",    "was",     "way",     "we",      "well",    "were",  "what",   "when",
      "where",   "which",   "while",   "who",     "whom",    "why",   "will",   "with",
      "without", "work",    "works",   "would",   "yes",     "yet",   "you",    "your"};
  return kStop.count(t) != 0;
}

// FNV-1a: a fixed hash, so vectors are the same on every platform and run.
std::uint64_t fnv1a(std::string_view s) {
  std::uint64_t h = 1469598103934665603ULL;
  for (const char c : s) {
    h ^= static_cast<unsigned char>(c);
    h *= 1099511628211ULL;
  }
  return h;
}

std::vector<std::string> tokenize_impl(std::string_view text, bool filter_stop) {
  std::vector<std::string> out;
  std::unordered_set<std::string> seen;
  const auto emit = [&](const std::string& tok) {
    if (tok.size() < 2) return;
    if (filter_stop && is_stopword(tok)) return;
    if (seen.insert(tok).second) out.push_back(tok);
  };

  // First pass: split on non-alphanumerics and camelCase boundaries.
  std::string current;
  const auto flush = [&] {
    if (!current.empty()) {
      emit(current);
      current.clear();
    }
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (!is_alnum(c)) {
      flush();
      continue;
    }
    const bool boundary =
        is_upper(c) && !current.empty() &&
        (is_lower(text[i - 1]) || (i + 1 < text.size() && is_lower(text[i + 1])));
    if (boundary) flush();
    current += to_lower(c);
  }
  flush();

  // Second pass: emit compound snake_case and CamelCase identifiers (e.g.
  // "verify_token", "tokenstore") so exact identifier matches get a dedicated token.
  std::string ident;
  bool compound = false;
  const auto flush_ident = [&] {
    if (compound && ident.size() >= 4 && ident.size() <= 64) emit(ident);
    ident.clear();
    compound = false;
  };
  for (std::size_t i = 0; i < text.size(); ++i) {
    const char c = text[i];
    if (is_alnum(c) || c == '_') {
      if (c == '_') {
        if (!ident.empty() && ident.back() != '_') {
          ident += '_';
          compound = true;
        }
      } else {
        if (is_upper(c) && !ident.empty() && ident.back() != '_' &&
            (is_lower(text[i - 1]) || (i + 1 < text.size() && is_lower(text[i + 1])))) {
          compound = true;
        }
        ident += to_lower(c);
      }
    } else {
      while (!ident.empty() && ident.back() == '_') ident.pop_back();
      if (ident.find('_') == std::string::npos && !compound) ident.clear();
      flush_ident();
    }
  }
  while (!ident.empty() && ident.back() == '_') ident.pop_back();
  flush_ident();

  return out;
}

}  // namespace

std::vector<std::string> tokenize(std::string_view text) {
  auto tokens = tokenize_impl(text, true);
  if (!tokens.empty()) return tokens;
  return tokenize_impl(text, false);
}

std::array<float, kVectorDims> embed(const std::vector<std::string>& tokens,
                                     float weight) {
  std::array<float, kVectorDims> v{};
  for (const std::string& t : tokens) {
    const std::uint64_t h = fnv1a(t);
    const std::size_t dim = static_cast<std::size_t>(h % kVectorDims);
    const float sign = ((h >> 32) & 1U) != 0 ? 1.0F : -1.0F;
    v[dim] += sign * weight;
  }
  normalise(v);
  return v;
}

void accumulate(std::array<float, kVectorDims>& into,
                const std::array<float, kVectorDims>& other) {
  for (std::size_t i = 0; i < kVectorDims; ++i) into[i] += other[i];
}

void normalise(std::array<float, kVectorDims>& v) {
  float sum = 0.0F;
  for (const float x : v) sum += x * x;
  if (sum == 0.0F) return;
  const float inv = 1.0F / std::sqrt(sum);
  for (float& x : v) x *= inv;
}

float dot(const std::array<float, kVectorDims>& a,
          const std::array<float, kVectorDims>& b) {
  float sum = 0.0F;
  for (std::size_t i = 0; i < kVectorDims; ++i) sum += a[i] * b[i];
  return sum;
}

}  // namespace veylqora
