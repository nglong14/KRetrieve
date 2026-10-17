#include "tokenizer.hpp"

#include <gtest/gtest.h>

using namespace kretrieve;

TEST(Tokenizer, LowercasesSplitsAndDropsStopwords) {
  EXPECT_EQ(tokenize("The Quick, Brown FOX jumps!"),
            std::vector<std::string>({"quick", "brown", "fox", "jumps"}));
}

TEST(Tokenizer, KeepsDigits) {
  EXPECT_EQ(tokenize("BM25 is a ranking function"),
            std::vector<std::string>({"bm25", "ranking", "function"}));
}

TEST(Tokenizer, EmptyAndPunctuationOnly) {
  EXPECT_TRUE(tokenize("").empty());
  EXPECT_TRUE(tokenize("  -- ... !!  ").empty());
}

TEST(Tokenizer, StopwordsOnly) {
  EXPECT_TRUE(tokenize("a the and of is it to").empty());
}

TEST(Tokenizer, CollapsesSeparatorsAndTrims) {
  EXPECT_EQ(tokenize("  --hello__world--  "),
            std::vector<std::string>({"hello", "world"}));
}

TEST(Tokenizer, ApostropheSplits) {
  EXPECT_EQ(tokenize("don't"), std::vector<std::string>({"don", "t"}));
}
