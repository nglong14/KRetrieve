#include "index.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace kretrieve;

namespace {

const std::vector<Document> kReferenceCorpus = {
    {10, "cat cat dog"}, {20, "dog bird"}, {30, "cat bird bird"}};

}  // namespace

TEST(InvertedIndex, BuildsPostingsAndDocumentMetadata) {
  const InvertedIndex index(kReferenceCorpus, compute_global_stats(kReferenceCorpus));
  ASSERT_EQ(index.document_count(), 3u);
  EXPECT_EQ(index.document_id(0), 10u);
  EXPECT_EQ(index.document_id(1), 20u);
  EXPECT_EQ(index.document_id(2), 30u);
  EXPECT_EQ(index.document_length(0), 3u);
  EXPECT_EQ(index.document_length(1), 2u);
  EXPECT_EQ(index.document_length(2), 3u);

  const auto cat = index.postings("cat");
  ASSERT_EQ(cat.size(), 2u);
  EXPECT_EQ(cat[0].document_index, 0u);
  EXPECT_EQ(cat[0].term_frequency, 2u);
  EXPECT_EQ(cat[1].document_index, 2u);
  EXPECT_EQ(cat[1].term_frequency, 1u);
  const auto dog = index.postings("dog");
  ASSERT_EQ(dog.size(), 2u);
  EXPECT_EQ(dog[0].document_index, 0u);
  EXPECT_EQ(dog[0].term_frequency, 1u);
  EXPECT_EQ(dog[1].document_index, 1u);
  EXPECT_EQ(dog[1].term_frequency, 1u);
  const auto bird = index.postings("bird");
  ASSERT_EQ(bird.size(), 2u);
  EXPECT_EQ(bird[0].document_index, 1u);
  EXPECT_EQ(bird[0].term_frequency, 1u);
  EXPECT_EQ(bird[1].document_index, 2u);
  EXPECT_EQ(bird[1].term_frequency, 2u);
  EXPECT_TRUE(index.postings("unknown").empty());
}

TEST(InvertedIndex, AcceptsEmptyPartitionWithGlobalStatistics) {
  const InvertedIndex index({}, compute_global_stats(kReferenceCorpus));
  EXPECT_EQ(index.document_count(), 0u);
  EXPECT_TRUE(index.postings("cat").empty());
  EXPECT_TRUE(index.search("cat", 10).empty());
}

TEST(InvertedIndex, RejectsDuplicateIdsWhenBuiltDirectly) {
  const std::vector<Document> documents = {{10, "cat"}, {10, "dog"}};
  EXPECT_THROW((InvertedIndex{documents, compute_global_stats(kReferenceCorpus)}),
               std::invalid_argument);
}

TEST(InvertedIndex, OwnsStatisticsAndIndexedData) {
  auto documents = kReferenceCorpus;
  auto stats = compute_global_stats(documents);
  const InvertedIndex index(documents, stats);
  documents.clear();
  stats.document_frequency.clear();
  stats.average_document_length = 0;
  const auto hits = index.search("cat dog", 1);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].document_id, 10u);
  EXPECT_NEAR(hits[0].score, 1.071445295349, 1e-9);
}

TEST(InvertedIndex, RejectsInvalidGlobalStatistics) {
  const auto valid = compute_global_stats(kReferenceCorpus);
  auto stats = valid;
  stats.document_count = 0;
  EXPECT_THROW((InvertedIndex{{}, stats}), std::invalid_argument);
  stats = valid;
  stats.document_count = 2;
  EXPECT_THROW((InvertedIndex{kReferenceCorpus, stats}), std::invalid_argument);
  for (const double average : {-1.0, 0.0, std::numeric_limits<double>::infinity(),
                               std::numeric_limits<double>::quiet_NaN()}) {
    stats = valid;
    stats.average_document_length = average;
    EXPECT_THROW((InvertedIndex{kReferenceCorpus, stats}), std::invalid_argument);
  }
  stats = valid;
  stats.document_frequency.clear();
  EXPECT_THROW((InvertedIndex{{}, stats}), std::invalid_argument);
  for (const uint64_t df : {uint64_t{0}, uint64_t{4}}) {
    stats = valid;
    stats.document_frequency["cat"] = df;
    EXPECT_THROW((InvertedIndex{kReferenceCorpus, stats}), std::invalid_argument);
  }
  stats = valid;
  stats.document_frequency[""] = 1;
  EXPECT_THROW((InvertedIndex{{}, stats}), std::invalid_argument);
  stats = valid;
  stats.document_frequency.erase("cat");
  EXPECT_THROW((InvertedIndex{kReferenceCorpus, stats}), std::invalid_argument);
  stats = valid;
  stats.document_frequency["cat"] = 1;
  EXPECT_THROW((InvertedIndex{kReferenceCorpus, stats}), std::invalid_argument);
}

TEST(Bm25, MatchesHandComputedTopK) {
  const InvertedIndex index(kReferenceCorpus, compute_global_stats(kReferenceCorpus));
  const auto hits = index.search("cat dog", 3);
  ASSERT_EQ(hits.size(), 3u);
  // N=3, avgdl=8/3, df(cat)=df(dog)=2, IDF=ln(1.6).
  // Independent numeric expectations, not the production scoring formula.
  EXPECT_EQ(hits[0].document_id, 10u);
  EXPECT_NEAR(hits[0].score, 1.071445295349, 1e-9);
  EXPECT_EQ(hits[1].document_id, 20u);
  EXPECT_NEAR(hits[1].score, 0.523548346502, 1e-9);
  EXPECT_EQ(hits[2].document_id, 30u);
  EXPECT_NEAR(hits[2].score, 0.447138587823, 1e-9);
}

TEST(Bm25, HandlesTopKLimits) {
  const InvertedIndex index(kReferenceCorpus, compute_global_stats(kReferenceCorpus));
  EXPECT_TRUE(index.search("cat dog", 0).empty());
  const auto one = index.search("cat dog", 1);
  ASSERT_EQ(one.size(), 1u);
  EXPECT_EQ(one[0].document_id, 10u);
  const auto two = index.search("cat dog", 2);
  ASSERT_EQ(two.size(), 2u);
  EXPECT_EQ(two[0].document_id, 10u);
  EXPECT_EQ(two[1].document_id, 20u);
  EXPECT_EQ(index.search("cat dog", std::numeric_limits<size_t>::max()).size(), 3u);
  EXPECT_EQ(index.search("cat", 10).size(), 2u);
}

TEST(Bm25, IgnoresUnknownTermsAndEmptyQueries) {
  const InvertedIndex index(kReferenceCorpus, compute_global_stats(kReferenceCorpus));
  for (const std::string query : {"", "!?", "the and of", "unknown"}) {
    EXPECT_TRUE(index.search(query, 10).empty()) << query;
  }
  const auto known = index.search("cat", 10);
  const auto mixed = index.search("cat unknown", 10);
  ASSERT_EQ(known.size(), mixed.size());
  for (size_t i = 0; i < known.size(); ++i) {
    EXPECT_EQ(known[i].document_id, mixed[i].document_id);
    EXPECT_DOUBLE_EQ(known[i].score, mixed[i].score);
  }
}

TEST(Bm25, DeduplicatesAndSortsTokenizedQueryTerms) {
  const InvertedIndex index(kReferenceCorpus, compute_global_stats(kReferenceCorpus));
  const auto expected = index.search("cat dog bird", 10);
  const auto actual = index.search("The DOG, cat! CAT bird dog", 10);
  ASSERT_EQ(expected.size(), actual.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(expected[i].document_id, actual[i].document_id);
    EXPECT_DOUBLE_EQ(expected[i].score, actual[i].score);
  }
}

TEST(Bm25, BreaksScoreTiesByAscendingGlobalId) {
  const std::vector<Document> documents = {{30, "cat"}, {10, "cat"}, {20, "cat"}};
  const InvertedIndex index(documents, compute_global_stats(documents));
  const auto hits = index.search("cat", 2);
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_EQ(hits[0].document_id, 10u);
  EXPECT_EQ(hits[1].document_id, 20u);
  EXPECT_DOUBLE_EQ(hits[0].score, hits[1].score);
}

TEST(Bm25, UsesDifferentIdfForRareAndCommonTerms) {
  const std::vector<Document> documents = {
      {1, "rare common"}, {2, "common filler"}, {3, "common filler"}};
  const InvertedIndex index(documents, compute_global_stats(documents));
  const auto hits = index.search("rare common", 3);
  ASSERT_EQ(hits.size(), 3u);
  // Equal document lengths and TF=1 reduce each contribution to its IDF.
  EXPECT_EQ(hits[0].document_id, 1u);
  EXPECT_NEAR(hits[0].score, 1.114360645637, 1e-9);
  EXPECT_EQ(hits[1].document_id, 2u);
  EXPECT_NEAR(hits[1].score, 0.133531392625, 1e-9);
  EXPECT_EQ(hits[2].document_id, 3u);
  EXPECT_NEAR(hits[2].score, 0.133531392625, 1e-9);
}

TEST(Bm25, UsesGlobalStatisticsForLocalPartitions) {
  const auto stats = compute_global_stats(kReferenceCorpus);
  const InvertedIndex full(kReferenceCorpus, stats);
  const auto expected = full.search("cat dog bird", 10);
  for (const auto& document : kReferenceCorpus) {
    const InvertedIndex shard(std::span<const Document>(&document, 1), stats);
    const auto hits = shard.search("cat dog bird", 10);
    ASSERT_EQ(hits.size(), 1u);
    EXPECT_EQ(hits[0].document_id, document.id);
    for (const auto& hit : expected) {
      if (hit.document_id == document.id) {
        EXPECT_DOUBLE_EQ(hits[0].score, hit.score);
      }
    }
  }
}

TEST(Bm25, HandlesEmptyAndStopwordOnlyDocuments) {
  const std::vector<Document> documents = {{1, ""}, {2, "the and"}, {3, "CAT! cat"}};
  const InvertedIndex index(documents, compute_global_stats(documents));
  EXPECT_EQ(index.document_count(), 3u);
  EXPECT_EQ(index.document_length(0), 0u);
  EXPECT_EQ(index.document_length(1), 0u);
  EXPECT_EQ(index.document_length(2), 2u);
  const auto hits = index.search("the CAT", 10);
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].document_id, 3u);
  EXPECT_TRUE(std::isfinite(hits[0].score));
}

TEST(Bm25, AllEmptyTokenCorpusDoesNotDivideByZero) {
  const std::vector<Document> documents = {{1, ""}, {2, "the and"}, {3, "!?"}};
  const InvertedIndex index(documents, compute_global_stats(documents));
  EXPECT_EQ(index.document_count(), 3u);
  EXPECT_TRUE(index.search("cat", 10).empty());
  EXPECT_TRUE(index.search("", 10).empty());
}
