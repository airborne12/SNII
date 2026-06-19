#include "corpus_loader.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "corpus_gen.h"

// Unit tests for bench::load_from_file. These deliberately avoid CLucene: they
// link only corpus_loader.cpp + corpus_gen.cpp (for the Corpus type) + gtest.
namespace {

// Writes `bytes` to a unique temp file and returns its path. The file is left on
// disk for the duration of the test; tests remove it explicitly.
std::string WriteTemp(const char* name, const std::string& bytes) {
  std::string path = std::string("/tmp/snii_corpus_loader_") + name + ".txt";
  std::FILE* fp = std::fopen(path.c_str(), "wb");
  EXPECT_NE(fp, nullptr);
  if (fp != nullptr) {
    if (!bytes.empty()) {
      const size_t w = std::fwrite(bytes.data(), 1, bytes.size(), fp);
      EXPECT_EQ(w, bytes.size());
    }
    std::fclose(fp);
  }
  return path;
}

// Renders a loaded document back into its token strings, so assertions read in
// terms of text rather than opaque vocab ids.
std::vector<std::string> DocTokens(const bench::Corpus& c, uint32_t doc) {
  std::vector<std::string> out;
  for (uint32_t id : c.docs[doc]) out.push_back(c.vocab[id]);
  return out;
}

}  // namespace

// docid == line number: three lines produce docs 0,1,2 in order.
TEST(CorpusLoader, DocIdEqualsLineNumber) {
  const std::string path =
      WriteTemp("docid", "alpha beta\ngamma\ndelta epsilon\n");
  bench::Corpus c = bench::load_from_file(path, 0);
  std::remove(path.c_str());

  ASSERT_EQ(c.doc_count, 3u);
  ASSERT_EQ(c.docs.size(), 3u);
  EXPECT_EQ(DocTokens(c, 0), (std::vector<std::string>{"alpha", "beta"}));
  EXPECT_EQ(DocTokens(c, 1), (std::vector<std::string>{"gamma"}));
  EXPECT_EQ(DocTokens(c, 2), (std::vector<std::string>{"delta", "epsilon"}));
}

// A final line with no trailing newline still becomes its own document.
TEST(CorpusLoader, FinalLineNoTrailingNewlineBecomesDoc) {
  const std::string path = WriteTemp("notrail", "one\ntwo");  // no final '\n'
  bench::Corpus c = bench::load_from_file(path, 0);
  std::remove(path.c_str());

  ASSERT_EQ(c.doc_count, 2u);
  EXPECT_EQ(DocTokens(c, 0), (std::vector<std::string>{"one"}));
  EXPECT_EQ(DocTokens(c, 1), (std::vector<std::string>{"two"}));
}

// A single logical line whose bytes span the 1 MiB read buffer boundary must
// tokenize as ONE document with all tokens intact (no token split across the
// fread chunk boundary, no spurious extra doc).
TEST(CorpusLoader, LineSpanningReadBufferTokenizesCorrectly) {
  constexpr size_t kBufSize = 1u << 20;  // mirrors loader's read buffer.
  // Build a line of repeated "tok " so it comfortably exceeds 1 MiB, then a
  // trailing token straddling the boundary, then a newline.
  std::string line;
  line.reserve(kBufSize + 4096);
  const std::string unit = "tok ";
  while (line.size() < kBufSize + 2048) line += unit;
  // Ensure the very last token sits across the boundary by appending a marker
  // word with no trailing space before the newline.
  line += "endmarker";
  const size_t expected_tok_count = (line.size() - std::string("endmarker").size()) /
                                        unit.size() +
                                    1;  // all "tok" + the final "endmarker"
  const std::string path = WriteTemp("spanbuf", line + "\n");
  bench::Corpus c = bench::load_from_file(path, 0);
  std::remove(path.c_str());

  ASSERT_EQ(c.doc_count, 1u);  // exactly one document despite spanning buffers.
  const std::vector<std::string> toks = DocTokens(c, 0);
  ASSERT_EQ(toks.size(), expected_tok_count);
  EXPECT_EQ(toks.front(), "tok");
  EXPECT_EQ(toks.back(), "endmarker");  // boundary-straddling token survived.
}

// max_docs caps the corpus even when the cap is reached mid-buffer (before EOF).
TEST(CorpusLoader, MaxDocsCapsMidBuffer) {
  // Five lines in one small buffer; cap at 2 -> only the first two docs kept.
  const std::string path =
      WriteTemp("cap", "a\nb\nc\nd\ne\n");
  bench::Corpus c = bench::load_from_file(path, 2);
  std::remove(path.c_str());

  ASSERT_EQ(c.doc_count, 2u);
  EXPECT_EQ(DocTokens(c, 0), (std::vector<std::string>{"a"}));
  EXPECT_EQ(DocTokens(c, 1), (std::vector<std::string>{"b"}));
}

// An empty line yields an empty document (kept, so docids stay aligned).
TEST(CorpusLoader, EmptyLineIsEmptyDoc) {
  const std::string path = WriteTemp("emptyline", "x\n\ny\n");
  bench::Corpus c = bench::load_from_file(path, 0);
  std::remove(path.c_str());

  ASSERT_EQ(c.doc_count, 3u);
  EXPECT_EQ(DocTokens(c, 0), (std::vector<std::string>{"x"}));
  EXPECT_TRUE(c.docs[1].empty());  // the blank line is an empty doc.
  EXPECT_EQ(DocTokens(c, 2), (std::vector<std::string>{"y"}));
}

// Opening a non-existent file throws (covers the open-failure branch / no fd
// leak path, since the throw happens before any descriptor is held).
TEST(CorpusLoader, MissingFileThrows) {
  EXPECT_THROW(bench::load_from_file("/tmp/snii_corpus_loader_does_not_exist__.txt", 0),
               std::runtime_error);
}

// A read error mid-stream must surface as a throw, NOT a silently truncated
// corpus. We trigger ferror by handing the loader a path that is a directory:
// fopen("rb") on a directory succeeds on Linux, but the first fread sets the
// stream error indicator (EISDIR) and returns 0. This reaches the ferror branch
// distinctly from a clean EOF (which would otherwise return an empty corpus).
TEST(CorpusLoader, ReadErrorOnDirectoryThrows) {
  // /tmp is guaranteed to exist and be a directory.
  EXPECT_THROW(bench::load_from_file("/tmp", 0), std::runtime_error);
}
