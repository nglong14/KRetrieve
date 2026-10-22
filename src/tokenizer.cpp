// Tokenizer implementation and stopword list
#include "tokenizer.hpp"

#include <cctype>
#include <string_view>
#include <unordered_set>

namespace kretrieve {
namespace {

const std::unordered_set<std::string_view>& stopwords() {
  static const std::unordered_set<std::string_view> words = {
      "a",    "an",   "and",   "are",   "as",   "at",   "be",   "but",  "by",  "for",  "from",
      "had",  "has",  "have",  "he",    "her",  "his",  "i",    "if",   "in",  "into", "is",
      "it",   "its",  "me",    "my",    "no",   "not",  "of",   "on",   "or",  "our",  "she",
      "so",   "that", "the",   "their", "them", "they", "this", "to",   "was", "we",   "were",
      "what", "when", "which", "who",   "will", "with", "you",  "your",
  };
  return words;
}

}  // namespace

std::vector<std::string> tokenize(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  for (char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    if (std::isalnum(c)) {
      current.push_back(static_cast<char>(std::tolower(c)));
    } else if (!current.empty()) {
      if (!stopwords().contains(current)) tokens.push_back(std::move(current));
      current.clear();
    }
  }
  if (!current.empty() && !stopwords().contains(current)) {
    tokens.push_back(std::move(current));
  }
  return tokens;
}

}  // namespace kretrieve
