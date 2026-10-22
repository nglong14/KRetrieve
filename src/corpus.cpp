#include "corpus.hpp"

#include "tokenizer.hpp"

#include <charconv>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

namespace kretrieve {

std::vector<Document> load_corpus(std::istream& input, bool allow_empty) {
  std::vector<Document> documents;
  std::unordered_set<uint64_t> ids;
  std::string line;
  size_t line_number = 0;
  while (std::getline(input, line)) {
    ++line_number;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto fail = [line_number](const char* reason) {
      throw std::runtime_error("corpus line " + std::to_string(line_number) + ": " + reason);
    };
    const size_t separator = line.find('\t');
    if (separator == std::string::npos) fail("missing tab separator");

    uint64_t id = 0;
    const char* id_end = line.data() + separator;
    const auto result = std::from_chars(line.data(), id_end, id);
    if (result.ec != std::errc{} || result.ptr != id_end) {
      fail("invalid unsigned 64-bit document ID");
    }
    if (!ids.insert(id).second) fail("duplicate document ID");
    documents.push_back({id, line.substr(separator + 1)});
  }
  if (input.bad() || (input.fail() && !input.eof())) {
    throw std::runtime_error("corpus read failed after line " + std::to_string(line_number));
  }
  if (documents.empty() && !allow_empty) {
    throw std::runtime_error("corpus contains no documents");
  }
  return documents;
}

GlobalStats compute_global_stats(std::span<const Document> documents) {
  if (documents.empty()) throw std::invalid_argument("corpus contains no documents");

  GlobalStats stats;
  stats.document_count = documents.size();
  uint64_t token_count = 0;
  std::unordered_set<uint64_t> ids;
  for (const auto& document : documents) {
    if (!ids.insert(document.id).second) {
      throw std::invalid_argument("duplicate document ID");
    }
    const auto tokens = tokenize(document.text);
    token_count += tokens.size();
    const std::unordered_set<std::string> unique_terms(tokens.begin(), tokens.end());
    for (const auto& term : unique_terms) ++stats.document_frequency[term];
  }
  stats.average_document_length =
      static_cast<double>(token_count) / static_cast<double>(stats.document_count);
  return stats;
}

void validate_global_stats(const GlobalStats& stats) {
  if (stats.document_count == 0 || !std::isfinite(stats.average_document_length) ||
      stats.average_document_length < 0 ||
      ((stats.average_document_length == 0) != stats.document_frequency.empty())) {
    throw std::invalid_argument("invalid global document count or average length");
  }
  for (const auto& [term, df] : stats.document_frequency) {
    if (term.empty() || df == 0 || df > stats.document_count) {
      throw std::invalid_argument("invalid global document frequency");
    }
  }
}

}  // namespace kretrieve
