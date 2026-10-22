// Immutable shard-local inverted index, scored with global BM25 statistics.
#pragma once

#include "corpus.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace kretrieve {

struct Posting {
  size_t document_index = 0;
  uint64_t term_frequency = 0;
};

struct SearchHit {
  uint64_t document_id = 0;
  double score = 0;
};

class InvertedIndex {
 public:
  InvertedIndex(std::span<const Document> documents, GlobalStats global_stats);

  // Accessors
  size_t document_count() const { return document_ids_.size(); }
  uint64_t document_id(size_t index) const { return document_ids_.at(index); }
  uint64_t document_length(size_t index) const { return document_lengths_.at(index); }
  std::span<const Posting> postings(const std::string& term) const;

  std::vector<SearchHit> search(std::string_view query, size_t k) const;

 private:
  GlobalStats global_stats_;
  std::vector<uint64_t> document_ids_;
  std::vector<uint64_t> document_lengths_;
  std::unordered_map<std::string, std::vector<Posting>> postings_;
};

}  // namespace kretrieve
