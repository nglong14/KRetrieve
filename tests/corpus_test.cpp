#include "corpus.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>

using namespace kretrieve;

TEST(Corpus, ReadsIdsAndPreservesTextAfterFirstTab) {
  std::istringstream input("10\tcat cat dog\n20\tdog\tbird\n30\t");
  const auto documents = load_corpus(input);
  ASSERT_EQ(documents.size(), 3u);
  EXPECT_EQ(documents[0].id, 10u);
  EXPECT_EQ(documents[0].text, "cat cat dog");
  EXPECT_EQ(documents[1].id, 20u);
  EXPECT_EQ(documents[1].text, "dog\tbird");
  EXPECT_EQ(documents[2].id, 30u);
  EXPECT_TRUE(documents[2].text.empty());
}

TEST(Corpus, AcceptsCrLfAndFullUnsignedIdRange) {
  std::istringstream input("0\tcat\r\n18446744073709551615\t\r\n");
  const auto documents = load_corpus(input);
  ASSERT_EQ(documents.size(), 2u);
  EXPECT_EQ(documents[0].id, 0u);
  EXPECT_EQ(documents[0].text, "cat");
  EXPECT_EQ(documents[1].id, std::numeric_limits<uint64_t>::max());
  EXPECT_TRUE(documents[1].text.empty());
}

TEST(Corpus, RejectsMalformedRecordsWithLineNumber) {
  for (const std::string record : {"", "missing tab", "\tcat", "-1\tcat",
                                  "+1\tcat", " 1\tcat", "1 \tcat", "1x\tcat",
                                  "18446744073709551616\tcat"}) {
    SCOPED_TRACE(record);
    std::istringstream input("10\tvalid\n" + record + "\n");
    try {
      (void)load_corpus(input);
      FAIL() << "accepted malformed record";
    } catch (const std::runtime_error& error) {
      EXPECT_NE(std::string(error.what()).find("line 2:"), std::string::npos);
    }
  }
}

TEST(Corpus, RejectsDuplicateIdsIncludingLeadingZeros) {
  std::istringstream input("10\tcat\n010\tdog\n");
  try {
    (void)load_corpus(input);
    FAIL() << "accepted duplicate ID";
  } catch (const std::runtime_error& error) {
    EXPECT_NE(std::string(error.what()).find("line 2: duplicate"), std::string::npos);
  }
}

TEST(Corpus, RejectsEmptyInputAndReadErrors) {
  std::istringstream empty;
  EXPECT_THROW(load_corpus(empty), std::runtime_error);
  std::istringstream failed("10\tcat\n");
  failed.setstate(std::ios::badbit);
  EXPECT_THROW(load_corpus(failed), std::runtime_error);
  std::istringstream failbit("10\tcat\n");
  failbit.setstate(std::ios::failbit);
  EXPECT_THROW(load_corpus(failbit), std::runtime_error);
}

TEST(GlobalStats, MatchesReferenceCorpus) {
  const std::vector<Document> documents = {
      {10, "cat cat dog"}, {20, "dog bird"}, {30, "cat bird bird"}};
  const auto stats = compute_global_stats(documents);
  EXPECT_EQ(stats.document_count, 3u);
  EXPECT_DOUBLE_EQ(stats.average_document_length, 8.0 / 3.0);
  ASSERT_EQ(stats.document_frequency.size(), 3u);
  EXPECT_EQ(stats.document_frequency.at("cat"), 2u);
  EXPECT_EQ(stats.document_frequency.at("dog"), 2u);
  EXPECT_EQ(stats.document_frequency.at("bird"), 2u);
}

TEST(GlobalStats, UsesTokenizerAndCountsEmptyDocuments) {
  const std::vector<Document> documents = {
      {1, "The CAT, cat! BM25"}, {2, ""}, {3, "and the of"}};
  const auto stats = compute_global_stats(documents);
  EXPECT_EQ(stats.document_count, 3u);
  EXPECT_DOUBLE_EQ(stats.average_document_length, 1.0);
  ASSERT_EQ(stats.document_frequency.size(), 2u);
  EXPECT_EQ(stats.document_frequency.at("cat"), 1u);
  EXPECT_EQ(stats.document_frequency.at("bm25"), 1u);
}

TEST(GlobalStats, AllowsAllEmptyTokenDocuments) {
  const std::vector<Document> documents = {{1, ""}, {2, "the and"}, {3, "!?"}};
  const auto stats = compute_global_stats(documents);
  EXPECT_EQ(stats.document_count, 3u);
  EXPECT_DOUBLE_EQ(stats.average_document_length, 0);
  EXPECT_TRUE(stats.document_frequency.empty());
}

TEST(GlobalStats, RejectsEmptyCorpusAndDuplicateIds) {
  EXPECT_THROW(compute_global_stats(std::span<const Document>{}), std::invalid_argument);
  const std::vector<Document> documents = {{1, "cat"}, {1, "dog"}};
  EXPECT_THROW(compute_global_stats(documents), std::invalid_argument);
}
