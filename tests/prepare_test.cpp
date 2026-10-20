#include "prepare.hpp"

#include "index.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using namespace kretrieve;
namespace fs = std::filesystem;

namespace {

class PrepareTest : public testing::Test {
 protected:
  fs::path root;

  void SetUp() override {
    std::string pattern = (fs::path(KRETRIEVE_TEST_TMP_DIR) / "prepare-test-XXXXXX").string();
    const char* directory = ::mkdtemp(pattern.data());
    ASSERT_NE(directory, nullptr);
    root = directory;
  }

  void TearDown() override {
    std::error_code error;
    if (!root.empty()) fs::remove_all(root, error);
  }

  void write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    ASSERT_TRUE(output) << path;
  }

  std::string read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input) << path;
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }
};

void set_integer(std::string& bytes, size_t offset, uint64_t value, size_t width = 8) {
  for (size_t i = 0; i < width; ++i) {
    bytes.at(offset + i) = static_cast<char>(value >> (8 * i));
  }
}

const std::vector<Document> kDocuments = {
    {10, "cat cat dog"}, {20, "dog bird"}, {30, "cat bird bird"}};

}  // namespace

TEST_F(PrepareTest, StatisticsRoundTrip) {
  const auto expected = compute_global_stats(kDocuments);
  const auto path = root / "stats.bin";
  write_global_stats(path, expected);
  const auto actual = read_global_stats(path);
  EXPECT_EQ(actual.document_count, expected.document_count);
  EXPECT_DOUBLE_EQ(actual.average_document_length, expected.average_document_length);
  EXPECT_EQ(actual.document_frequency, expected.document_frequency);
}

TEST_F(PrepareTest, StatisticsEncodingIsLittleEndianAndStable) {
  const GlobalStats stats{1, 1.0, {{"cat", 1}}};
  const auto path = root / "stats.bin";
  write_global_stats(path, stats);
  const std::vector<uint8_t> expected = {
      'K', 'R', 'S', 'T',
      1, 0, 0, 0,                    // version
      1, 0, 0, 0, 0, 0, 0, 0,        // N
      0, 0, 0, 0, 0, 0, 0xf0, 0x3f,  // IEEE-754 1.0
      1, 0, 0, 0, 0, 0, 0, 0,        // vocabulary size
      3, 0, 0, 0, 'c', 'a', 't',      // term length + bytes
      1, 0, 0, 0, 0, 0, 0, 0};       // DF
  EXPECT_EQ(read(path), std::string(expected.begin(), expected.end()));
}

TEST_F(PrepareTest, StatisticsOutputDoesNotDependOnHashInsertionOrder) {
  auto stats = compute_global_stats(kDocuments);
  write_global_stats(root / "first.bin", stats);
  const auto original = stats.document_frequency;
  stats.document_frequency.clear();
  for (const std::string term : {"dog", "cat", "bird"}) {
    stats.document_frequency.emplace(term, original.at(term));
  }
  write_global_stats(root / "second.bin", stats);
  EXPECT_EQ(read(root / "first.bin"), read(root / "second.bin"));
}

TEST_F(PrepareTest, AllEmptyTokenStatisticsRoundTrip) {
  const auto path = root / "stats.bin";
  write_global_stats(path, GlobalStats{3, 0, {}});
  const auto stats = read_global_stats(path);
  EXPECT_EQ(stats.document_count, 3u);
  EXPECT_DOUBLE_EQ(stats.average_document_length, 0);
  EXPECT_TRUE(stats.document_frequency.empty());
}

TEST_F(PrepareTest, RejectsEveryTruncationAndTrailingBytes) {
  const auto path = root / "stats.bin";
  write_global_stats(path, compute_global_stats(kDocuments));
  const auto valid = read(path);
  for (size_t length = 0; length < valid.size(); ++length) {
    SCOPED_TRACE(length);
    write(path, valid.substr(0, length));
    EXPECT_THROW(read_global_stats(path), std::runtime_error);
  }
  write(path, valid + "extra");
  EXPECT_THROW(read_global_stats(path), std::runtime_error);
}

TEST_F(PrepareTest, RejectsBadMagicVersionAndEncodedLengths) {
  const auto path = root / "stats.bin";
  write_global_stats(path, compute_global_stats(kDocuments));
  const auto valid = read(path);
  auto bytes = valid;
  bytes[0] = 'X';
  write(path, bytes);
  EXPECT_THROW(read_global_stats(path), std::runtime_error);
  bytes = valid;
  set_integer(bytes, 4, 2, 4);
  write(path, bytes);
  EXPECT_THROW(read_global_stats(path), std::runtime_error);
  for (const uint64_t size : {uint64_t{0}, std::numeric_limits<uint64_t>::max()}) {
    bytes = valid;
    set_integer(bytes, 24, size);
    write(path, bytes);
    EXPECT_THROW(read_global_stats(path), std::runtime_error);
  }
  for (const uint64_t length : {uint64_t{0}, uint64_t{0xffffffff}}) {
    bytes = valid;
    set_integer(bytes, 32, length, 4);
    write(path, bytes);
    EXPECT_THROW(read_global_stats(path), std::runtime_error);
  }
}

TEST_F(PrepareTest, RejectsDuplicateTermsAndInvalidStatistics) {
  const auto path = root / "stats.bin";
  write_global_stats(path, GlobalStats{2, 1, {{"cat", 1}, {"dog", 1}}});
  const auto valid = read(path);
  auto bytes = valid;
  bytes.replace(51, 3, "cat");  // second term duplicates the first term
  write(path, bytes);
  EXPECT_THROW(read_global_stats(path), std::runtime_error);
  bytes = valid;
  set_integer(bytes, 8, 0);  // N = 0
  write(path, bytes);
  EXPECT_THROW(read_global_stats(path), std::runtime_error);
  for (const double average : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    bytes = valid;
    set_integer(bytes, 16, std::bit_cast<uint64_t>(average));
    write(path, bytes);
    EXPECT_THROW(read_global_stats(path), std::runtime_error);
  }
  for (const uint64_t df : {uint64_t{0}, uint64_t{3}}) {
    bytes = valid;
    set_integer(bytes, 39, df);
    write(path, bytes);
    EXPECT_THROW(read_global_stats(path), std::runtime_error);
  }
}

TEST_F(PrepareTest, RejectsInvalidWritesBeforeTouchingTarget) {
  const auto path = root / "stats.bin";
  write(path, "keep this file");
  EXPECT_THROW(write_global_stats(path, GlobalStats{}), std::invalid_argument);
  EXPECT_EQ(read(path), "keep this file");
}

TEST_F(PrepareTest, ReportsStatisticsFileErrors) {
  EXPECT_THROW(read_global_stats(root / "missing.bin"), std::runtime_error);
  EXPECT_THROW(read_global_stats(root), std::runtime_error);
  EXPECT_THROW(write_global_stats(root, compute_global_stats(kDocuments)), std::runtime_error);
}

TEST_F(PrepareTest, ReportsBufferedStatisticsWriteFailures) {
  // Linux's /dev/full accepts opens but fails writes with ENOSPC, including
  // writes deferred until stream close. Other POSIX platforms may not have it.
  if (!fs::is_character_file("/dev/full")) GTEST_SKIP() << "/dev/full is unavailable";
  EXPECT_THROW(write_global_stats("/dev/full", compute_global_stats(kDocuments)),
               std::runtime_error);
}

TEST_F(PrepareTest, PartitionsDeterministicallyAndPreservesDocuments) {
  const auto input = root / "corpus.tsv";
  write(input, "10\tcat cat dog\r\n20\tdog\tbird\r\n30\tcat bird bird\r\n40\t\r\n");
  for (const size_t shards : {1u, 3u, 7u}) {
    const auto output = root / ("prepared-" + std::to_string(shards));
    prepare_corpus(input, output, shards);
    std::map<uint64_t, std::string> seen;
    for (size_t shard = 0; shard < shards; ++shard) {
      std::ifstream partition(output / ("shard-" + std::to_string(shard) + ".tsv"));
      ASSERT_TRUE(partition);
      for (const auto& document : load_corpus(partition, /*allow_empty=*/true)) {
        EXPECT_EQ(document.id % shards, shard);
        EXPECT_TRUE(seen.emplace(document.id, document.text).second);
      }
    }
    const std::map<uint64_t, std::string> expected = {
        {10, "cat cat dog"}, {20, "dog\tbird"}, {30, "cat bird bird"}, {40, ""}};
    EXPECT_EQ(seen, expected);
    const auto stats = read_global_stats(output / "global_stats.bin");
    EXPECT_EQ(stats.document_count, 4u);
    EXPECT_DOUBLE_EQ(stats.average_document_length, 2);
    EXPECT_EQ(stats.document_frequency.at("cat"), 2u);
    EXPECT_EQ(stats.document_frequency.at("dog"), 2u);
    EXPECT_EQ(stats.document_frequency.at("bird"), 2u);
  }
  prepare_corpus(input, root / "repeat", 3);
  for (const std::string name : {"global_stats.bin", "shard-0.tsv", "shard-1.tsv",
                                 "shard-2.tsv"}) {
    EXPECT_EQ(read(root / "prepared-3" / name), read(root / "repeat" / name));
  }
}

TEST_F(PrepareTest, RejectsBadInputBeforeCreatingOutput) {
  const auto input = root / "corpus.tsv";
  const auto output = root / "prepared";
  EXPECT_THROW(prepare_corpus(input, output, 2), std::runtime_error);
  EXPECT_FALSE(fs::exists(output));
  for (const std::string corpus : {"", "1\tcat\n1\tdog\n", "bad record\n"}) {
    write(input, corpus);
    EXPECT_THROW(prepare_corpus(input, output, 2), std::runtime_error);
    EXPECT_FALSE(fs::exists(output));
  }
  write(input, "1\tcat\n");
  EXPECT_THROW(prepare_corpus(input, output, 0), std::invalid_argument);
  EXPECT_FALSE(fs::exists(output));
}

TEST_F(PrepareTest, RefusesExistingOutputWithoutOverwriting) {
  const auto input = root / "corpus.tsv";
  const auto output = root / "prepared";
  write(input, "1\tcat\n");
  ASSERT_TRUE(fs::create_directory(output));
  EXPECT_THROW(prepare_corpus(input, output, 2), std::runtime_error);
  EXPECT_TRUE(fs::is_empty(output));
  write(output / "global_stats.bin", "keep stats");
  write(output / "shard-0.tsv", "keep partition");
  EXPECT_THROW(prepare_corpus(input, output, 2), std::runtime_error);
  EXPECT_EQ(read(output / "global_stats.bin"), "keep stats");
  EXPECT_EQ(read(output / "shard-0.tsv"), "keep partition");
  EXPECT_THROW(prepare_corpus(input, input, 2), fs::filesystem_error);
  EXPECT_EQ(read(input), "1\tcat\n");
  EXPECT_THROW(prepare_corpus(input, root / "missing-parent" / "prepared", 2),
               fs::filesystem_error);
}

TEST_F(PrepareTest, DiskToSearchMatchesFullCorpusAcrossShardCounts) {
  const auto input = root / "corpus.tsv";
  write(input, "11\tcat cat dog\n10\tcat cat dog\n20\tdog bird\n"
               "30\tcat bird bird\n40\trare\n50\t\n60\tthe and\n");
  std::ifstream source(input);
  const auto documents = load_corpus(source);
  const InvertedIndex full(documents, compute_global_stats(documents));
  for (const size_t shards : {1u, 2u, 3u, 10u}) {
    SCOPED_TRACE(shards);
    const auto output = root / ("prepared-" + std::to_string(shards));
    prepare_corpus(input, output, shards);
    const auto stats = read_global_stats(output / "global_stats.bin");
    std::vector<InvertedIndex> indexes;
    for (size_t shard = 0; shard < shards; ++shard) {
      std::ifstream partition(output / ("shard-" + std::to_string(shard) + ".tsv"));
      ASSERT_TRUE(partition);
      indexes.emplace_back(load_corpus(partition, /*allow_empty=*/true), stats);
    }
    for (const std::string query : {"cat dog", "rare cat", "bird dog", "cat cat",
                                    "the and", "unknown", ""}) {
      SCOPED_TRACE(query);
      for (const size_t k : {0u, 1u, 2u, 3u, 20u}) {
        SCOPED_TRACE(k);
        std::vector<SearchHit> merged;
        for (const auto& index : indexes) {
          const auto hits = index.search(query, k);
          merged.insert(merged.end(), hits.begin(), hits.end());
        }
        std::sort(merged.begin(), merged.end(), [](const SearchHit& a, const SearchHit& b) {
          if (a.score != b.score) return a.score > b.score;
          return a.document_id < b.document_id;
        });
        merged.resize(std::min(k, merged.size()));
        const auto expected = full.search(query, k);
        ASSERT_EQ(merged.size(), expected.size());
        for (size_t i = 0; i < expected.size(); ++i) {
          EXPECT_EQ(merged[i].document_id, expected[i].document_id);
          EXPECT_NEAR(merged[i].score, expected[i].score, 1e-9);
        }
      }
    }
  }
}

TEST_F(PrepareTest, PreparesAllEmptyTokenCorpusForSearch) {
  const auto input = root / "corpus.tsv";
  const auto output = root / "prepared";
  write(input, "1\t\n2\tthe and\n");
  prepare_corpus(input, output, 3);
  const auto stats = read_global_stats(output / "global_stats.bin");
  EXPECT_EQ(stats.document_count, 2u);
  EXPECT_DOUBLE_EQ(stats.average_document_length, 0);
  for (size_t shard = 0; shard < 3; ++shard) {
    std::ifstream partition(output / ("shard-" + std::to_string(shard) + ".tsv"));
    const InvertedIndex index(load_corpus(partition, /*allow_empty=*/true), stats);
    EXPECT_TRUE(index.search("cat", 10).empty());
  }
}
