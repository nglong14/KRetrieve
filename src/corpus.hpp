// Corpus input and collection-wide BM25 statistics.
#pragma once

#include <cstdint>
#include <istream>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace kretrieve {

struct Document {
  uint64_t id = 0;
  std::string text;
};

struct GlobalStats {
  uint64_t document_count = 0;
  double average_document_length = 0;
  std::unordered_map<std::string, uint64_t> document_frequency;
};

// Read unique unsigned IDs and text, separated by the first tab on each line.
// Empty text and CRLF are accepted; malformed/empty input and I/O errors throw
// std::runtime_error.
std::vector<Document> load_corpus(std::istream& input, bool allow_empty = false);

// Lengths count tokens after stopword removal; DF counts each document once.
// Empty corpora and duplicate IDs throw std::invalid_argument. Documents with
// no tokens still count toward N; an all-empty-token corpus has avgdl == 0.
GlobalStats compute_global_stats(std::span<const Document> documents);

// Validate the shared BM25 invariants; throws std::invalid_argument.
void validate_global_stats(const GlobalStats& stats);

}  // namespace kretrieve
