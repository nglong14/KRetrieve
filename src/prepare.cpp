#include "prepare.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <fstream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace kretrieve {
namespace {

// [u32 term_length][term bytes][u64 document_frequency]
constexpr std::string_view kStatsMagic = "KRST";
constexpr uint32_t kStatsVersion = 1;
static_assert(sizeof(double) == sizeof(uint64_t) && std::numeric_limits<double>::is_iec559);

void append_integer(std::vector<uint8_t>& bytes, uint64_t value, size_t width) {
  for (size_t i = 0; i < width; ++i) {
    bytes.push_back(static_cast<uint8_t>(value >> (8 * i)));
  }
}

uint64_t read_integer(std::span<const uint8_t>& bytes, size_t width) {
  if (bytes.size() < width) throw std::runtime_error("truncated global statistics");
  uint64_t value = 0;
  for (size_t i = 0; i < width; ++i) value |= uint64_t{bytes[i]} << (8 * i);
  bytes = bytes.subspan(width);
  return value;
}

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
  const uintmax_t size = std::filesystem::file_size(path);
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot open statistics file: " + path.string());
  if (size > std::vector<uint8_t>().max_size() ||
      size > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    throw std::runtime_error("invalid statistics file size: " + path.string());
  }
  // Bound allocations by actual file size, never by lengths from its header.
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  if (!bytes.empty()) {
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }
  if (!input) throw std::runtime_error("cannot read statistics file: " + path.string());
  return bytes;
}

}  // namespace

void write_global_stats(const std::filesystem::path& path, const GlobalStats& stats) {
  validate_global_stats(stats);
  std::vector<std::string_view> terms;
  terms.reserve(stats.document_frequency.size());
  for (const auto& [term, df] : stats.document_frequency) {
    if (term.size() > std::numeric_limits<uint32_t>::max()) {
      throw std::invalid_argument("statistics term exceeds u32 length");
    }
    terms.push_back(term);
  }
  std::sort(terms.begin(), terms.end());

  std::vector<uint8_t> bytes(kStatsMagic.begin(), kStatsMagic.end());
  append_integer(bytes, kStatsVersion, 4);
  append_integer(bytes, stats.document_count, 8);
  append_integer(bytes, std::bit_cast<uint64_t>(stats.average_document_length), 8);
  append_integer(bytes, terms.size(), 8);
  for (const auto term : terms) {
    append_integer(bytes, term.size(), 4);
    bytes.insert(bytes.end(), term.begin(), term.end());
    append_integer(bytes, stats.document_frequency.at(std::string(term)), 8);
  }
  if (bytes.size() > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max())) {
    throw std::runtime_error("statistics file exceeds stream size limit");
  }
  std::ofstream output(path, std::ios::binary);
  if (!output) throw std::runtime_error("cannot create statistics file: " + path.string());
  output.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  output.close();
  if (!output) throw std::runtime_error("cannot write statistics file: " + path.string());
}

GlobalStats read_global_stats(const std::filesystem::path& path) {
  const auto buffer = read_file(path);
  std::span<const uint8_t> bytes(buffer);
  if (bytes.size() < kStatsMagic.size() ||
      !std::equal(kStatsMagic.begin(), kStatsMagic.end(), bytes.begin())) {
    throw std::runtime_error("invalid global statistics magic");
  }
  bytes = bytes.subspan(kStatsMagic.size());
  if (read_integer(bytes, 4) != kStatsVersion) {
    throw std::runtime_error("unsupported global statistics version");
  }
  GlobalStats stats;
  stats.document_count = read_integer(bytes, 8);
  stats.average_document_length = std::bit_cast<double>(read_integer(bytes, 8));
  const uint64_t vocabulary_size = read_integer(bytes, 8);
  // Each entry needs a u32 length, at least one term byte, and a u64 DF.
  if (vocabulary_size > bytes.size() / 13) {
    throw std::runtime_error("invalid global statistics vocabulary size");
  }
  for (uint64_t i = 0; i < vocabulary_size; ++i) {
    const uint64_t length = read_integer(bytes, 4);
    if (length == 0 || bytes.size() < 8 || length > bytes.size() - 8) {
      throw std::runtime_error("invalid global statistics term length");
    }
    std::string term(reinterpret_cast<const char*>(bytes.data()), static_cast<size_t>(length));
    bytes = bytes.subspan(static_cast<size_t>(length));
    const uint64_t df = read_integer(bytes, 8);
    if (!stats.document_frequency.emplace(std::move(term), df).second) {
      throw std::runtime_error("duplicate global statistics term");
    }
  }
  if (!bytes.empty()) throw std::runtime_error("trailing bytes in global statistics");
  try {
    validate_global_stats(stats);
  } catch (const std::invalid_argument& error) {
    throw std::runtime_error(std::string("invalid global statistics: ") + error.what());
  }
  return stats;
}

void prepare_corpus(const std::filesystem::path& input, const std::filesystem::path& output,
                    size_t shard_count) {
  if (shard_count == 0) throw std::invalid_argument("shard count must be positive");
  std::ifstream source(input, std::ios::binary);
  if (!source) throw std::runtime_error("cannot open corpus: " + input.string());
  const auto documents = load_corpus(source);
  const auto stats = compute_global_stats(documents);
  std::unordered_map<size_t, std::vector<const Document*>> partitions;
  for (const auto& document : documents) {
    partitions[document.id % shard_count].push_back(&document);
  }

  if (!std::filesystem::create_directory(output)) {
    throw std::runtime_error("output directory already exists: " + output.string());
  }
  // ponytail: keep the corpus in memory; stream partitioning only if it outgrows RAM.
  for (size_t shard = 0; shard < shard_count; ++shard) {
    const auto path = output / ("shard-" + std::to_string(shard) + ".tsv");
    std::ofstream partition(path, std::ios::binary);
    if (!partition) throw std::runtime_error("cannot create partition: " + path.string());
    const auto found = partitions.find(shard);
    if (found != partitions.end()) {
      for (const auto* document : found->second) {
        partition << document->id << '\t' << document->text << '\n';
      }
    }
    partition.close();
    if (!partition) throw std::runtime_error("cannot write partition: " + path.string());
  }
  write_global_stats(output / "global_stats.bin", stats);
}

}  // namespace kretrieve
