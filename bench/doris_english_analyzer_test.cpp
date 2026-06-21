#include "doris_english_analyzer.h"

#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace {

// Convenience wrapper: tokenize into a fresh vector for terse assertions.
std::vector<std::string> tok(const std::string& s) {
  std::vector<std::string> out;
  bench::doris_english_tokenize(s, &out);
  return out;
}

TEST(DorisEnglishAnalyzer, LowercasesAsciiLetters) {
  EXPECT_EQ(tok("Failed To Place ORDER"),
            (std::vector<std::string>{"failed", "to", "place", "order"}));
}

TEST(DorisEnglishAnalyzer, KeepsDigitsAndAlnumRuns) {
  EXPECT_EQ(tok("code 13 http2 t3st"),
            (std::vector<std::string>{"code", "13", "http2", "t3st"}));
}

TEST(DorisEnglishAnalyzer, SplitsOnJsonPunctuation) {
  // A typical OTel JSON body: every non-[a-z0-9] byte is a separator.
  EXPECT_EQ(tok(R"({"code":13,"details":"rpc error"})"),
            (std::vector<std::string>{"code", "13", "details", "rpc", "error"}));
}

TEST(DorisEnglishAnalyzer, EmptyStringYieldsNoTokens) {
  EXPECT_TRUE(tok("").empty());
}

TEST(DorisEnglishAnalyzer, PureSeparatorStringYieldsNoTokens) {
  EXPECT_TRUE(tok("  ::,.{}[]<>->  ").empty());
}

TEST(DorisEnglishAnalyzer, ConsecutiveSeparatorsProduceNoEmptyTokens) {
  EXPECT_EQ(tok("a,,,b...c"),
            (std::vector<std::string>{"a", "b", "c"}));
}

TEST(DorisEnglishAnalyzer, LongTokenTruncatedToMaxLenRunNotSplit) {
  // A single alnum run longer than the cap keeps only its first kMax bytes and
  // does NOT split into two tokens (the rest of the run is dropped).
  const std::string run(bench::kDorisEnglishMaxTokenLen + 20, 'x');
  const std::string expected(bench::kDorisEnglishMaxTokenLen, 'x');
  EXPECT_EQ(tok(run), (std::vector<std::string>{expected}));
}

TEST(DorisEnglishAnalyzer, TruncationResumesAtNextSeparator) {
  const std::string run(bench::kDorisEnglishMaxTokenLen + 5, 'a');
  const std::string head(bench::kDorisEnglishMaxTokenLen, 'a');
  EXPECT_EQ(tok(run + " tail"),
            (std::vector<std::string>{head, "tail"}));
}

TEST(DorisEnglishAnalyzer, ForEachTokenMatchesTokenize) {
  const std::string s = R"(Visa cache FULL: cannot add item 42)";
  std::vector<std::string> via_callback;
  bench::doris_english_for_each_token(
      s, [&](std::string_view t) { via_callback.emplace_back(t); });
  EXPECT_EQ(via_callback, tok(s));
}

TEST(DorisEnglishAnalyzer, TokenizeClearsOutputFirst) {
  std::vector<std::string> out{"stale"};
  bench::doris_english_tokenize("hello", &out);
  EXPECT_EQ(out, (std::vector<std::string>{"hello"}));
}

}  // namespace
