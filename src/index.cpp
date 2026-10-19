#include "index.hpp"

#include "tokenizer.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace kretrieve {

InvertedIndex::InvertedIndex(std::span<const Document> documents,
                             GlobalStats global_stats)
    : global_stats_(std::move(global_stats)) {
  const auto& stats = global_stats_;
  if (stats.document_count == 0 || documents.size() > stats.document_count ||
      !std::isfinite(stats.average_document_length) ||
      stats.average_document_length < 0 ||
      ((stats.average_document_length == 0) != stats.document_frequency.empty())) {
    throw std::invalid_argument("invalid global document count or average length");
  }
  for (const auto& [term, df] : stats.document_frequency) {
    if (term.empty() || df == 0 || df > stats.document_count) {
      throw std::invalid_argument("invalid global document frequency");
    }
  }

  document_ids_.reserve(documents.size());
  document_lengths_.reserve(documents.size());
  std::unordered_set<uint64_t> ids;
  for (const auto& document : documents) {
    if (!ids.insert(document.id).second) {
      throw std::invalid_argument("duplicate document ID");
    }
    const auto tokens = tokenize(document.text);
    const size_t document_index = document_ids_.size();
    document_ids_.push_back(document.id);
    document_lengths_.push_back(tokens.size());
    std::unordered_map<std::string, uint64_t> frequencies;
    for (const auto& term : tokens) ++frequencies[term];
    for (const auto& [term, tf] : frequencies) {
      const auto global_df = stats.document_frequency.find(term);
      if (global_df == stats.document_frequency.end()) {
        throw std::invalid_argument("local term missing from global statistics: " + term);
      }
      auto& list = postings_[term];
      list.push_back({document_index, tf});
      if (list.size() > global_df->second) {
        throw std::invalid_argument("local DF exceeds global DF: " + term);
      }
    }
  }
}

std::span<const Posting> InvertedIndex::postings(const std::string& term) const {
  const auto found = postings_.find(term);
  if (found == postings_.end()) return {};
  return found->second;
}

std::vector<SearchHit> InvertedIndex::search(std::string_view query, size_t k) const {
  if (k == 0 || postings_.empty()) return {};
  auto terms = tokenize(query);
  std::sort(terms.begin(), terms.end());
  terms.erase(std::unique(terms.begin(), terms.end()), terms.end());

  constexpr double k1 = 1.2;
  constexpr double b = 0.75;
  std::unordered_map<size_t, double> scores;
  for (const auto& term : terms) {
    const auto list = postings(term);
    if (list.empty()) continue;
    const uint64_t df = global_stats_.document_frequency.at(term);
    const double idf = std::log1p(
        (static_cast<double>(global_stats_.document_count - df) + 0.5) /
        (static_cast<double>(df) + 0.5));
    for (const auto& posting : list) {
      const double tf = static_cast<double>(posting.term_frequency);
      const double length = static_cast<double>(document_lengths_[posting.document_index]);
      const double normalization =
          k1 * (1 - b + b * length / global_stats_.average_document_length);
      scores[posting.document_index] += idf * (tf * (k1 + 1)) / (tf + normalization);
    }
  }

  std::vector<SearchHit> hits;
  hits.reserve(scores.size());
  for (const auto& [index, score] : scores) {
    hits.push_back({document_ids_[index], score});
  }
  const size_t count = std::min(k, hits.size());
  std::partial_sort(hits.begin(), hits.begin() + count, hits.end(),
                    [](const SearchHit& a, const SearchHit& b) {
                      if (a.score != b.score) return a.score > b.score;
                      return a.document_id < b.document_id;
                    });
  hits.resize(count);
  return hits;
}

}  // namespace kretrieve
