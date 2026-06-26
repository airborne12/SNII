#include "snii/query/boolean_query.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "snii/io/local_file.h"
#include "snii/io/metered_file_reader.h"
#include "snii/query/docid_sink.h"
#include "snii/query/term_query.h"
#include "snii/reader/logical_index_reader.h"
#include "snii/reader/snii_segment_reader.h"
#include "snii/writer/snii_compound_writer.h"
#include "snii/writer/spimi_term_buffer.h"

using namespace snii;
using namespace snii::reader;
using namespace snii::writer;

namespace {

std::string TempPath() {
  static int counter = 0;
  return "/tmp/snii_boolean_query_" + std::to_string(getpid()) + "_" +
         std::to_string(counter++) + ".idx";
}

struct Corpus {
  uint32_t doc_count = 900;
  std::vector<std::vector<std::string>> docs;

  std::vector<uint32_t> term_docs(const std::string& term) const {
    std::vector<uint32_t> out;
    for (uint32_t d = 0; d < docs.size(); ++d) {
      if (std::find(docs[d].begin(), docs[d].end(), term) != docs[d].end()) {
        out.push_back(d);
      }
    }
    return out;
  }

  std::vector<uint32_t> any_docs(const std::vector<std::string>& terms) const {
    std::set<uint32_t> ids;
    for (const auto& term : terms) {
      const std::vector<uint32_t> docs_for_term = term_docs(term);
      ids.insert(docs_for_term.begin(), docs_for_term.end());
    }
    return {ids.begin(), ids.end()};
  }
};

Corpus BuildCorpus() {
  Corpus c;
  c.docs.resize(c.doc_count);
  for (uint32_t d = 0; d < c.doc_count; ++d) {
    std::vector<std::string>& toks = c.docs[d];
    if (d < 700) toks.push_back("aa_hot");
    if (d < 700) toks.push_back("aa_warm");
    if (d < 700) toks.push_back("aa_loud");
    if (d % 17 == 0) toks.push_back("aa_rare");
    if (d % 41 == 0) toks.push_back("aa_sparse");
    char filler[16];
    std::snprintf(filler, sizeof(filler), "zz_%03u", d % 200);
    toks.emplace_back(filler);
  }
  return c;
}

void WriteCorpus(const Corpus& c, const std::string& path) {
  SpimiTermBuffer buf(/*has_positions=*/false);
  for (uint32_t d = 0; d < c.docs.size(); ++d) {
    const auto& toks = c.docs[d];
    for (uint32_t pos = 0; pos < toks.size(); ++pos) {
      buf.add_token(toks[pos], d, pos);
    }
  }

  SniiIndexInput in;
  in.index_id = 1;
  in.index_suffix = "body";
  in.config = snii::format::IndexConfig::kDocsOnly;
  in.doc_count = c.doc_count;
  in.terms = buf.finalize_sorted();
  in.target_dict_block_bytes = 256;

  io::LocalFileWriter writer;
  ASSERT_TRUE(writer.open(path).ok());
  SniiCompoundWriter compound(&writer);
  ASSERT_TRUE(compound.add_logical_index(in).ok());
  ASSERT_TRUE(compound.finish().ok());
}

LogicalIndexReader OpenIndex(io::LocalFileReader* file, SniiSegmentReader* segment,
                             const std::string& path) {
  EXPECT_TRUE(file->open(path).ok());
  EXPECT_TRUE(SniiSegmentReader::open(file, segment).ok());
  LogicalIndexReader idx;
  EXPECT_TRUE(segment->open_index(1, "body", &idx).ok());
  return idx;
}

LogicalIndexReader OpenMeteredIndex(io::MeteredFileReader* file,
                                    SniiSegmentReader* segment) {
  EXPECT_TRUE(SniiSegmentReader::open(file, segment).ok());
  LogicalIndexReader idx;
  EXPECT_TRUE(segment->open_index(1, "body", &idx).ok());
  return idx;
}

class RecordingSink final : public query::DocIdSink {
 public:
  Status append_sorted(std::span<const uint32_t> docids) override {
    ++chunks;
    max_chunk = std::max(max_chunk, docids.size());
    out.insert(out.end(), docids.begin(), docids.end());
    return Status::OK();
  }

  std::vector<uint32_t> out;
  size_t chunks = 0;
  size_t max_chunk = 0;
};

class FailingSink final : public query::DocIdSink {
 public:
  Status append_sorted(std::span<const uint32_t> docids) override {
    saw_docids = !docids.empty();
    return Status::IoError("sink append failed");
  }

  bool saw_docids = false;
};

}  // namespace

TEST(BooleanOr, ReturnsSortedDeduplicatedUnion) {
  const Corpus corpus = BuildCorpus();
  const std::string path = TempPath();
  WriteCorpus(corpus, path);

  io::LocalFileReader file;
  SniiSegmentReader segment;
  LogicalIndexReader idx = OpenIndex(&file, &segment, path);

  const std::vector<std::string> terms = {"aa_hot", "aa_rare", "absent_token", "aa_hot",
                                          "aa_sparse"};
  std::vector<uint32_t> got;
  ASSERT_TRUE(query::boolean_or(idx, terms, &got).ok());

  EXPECT_TRUE(std::is_sorted(got.begin(), got.end()));
  EXPECT_EQ(got, corpus.any_docs(terms));

  std::vector<uint32_t> single;
  ASSERT_TRUE(query::term_query(idx, "aa_hot", &single).ok());
  std::vector<uint32_t> single_or;
  ASSERT_TRUE(query::boolean_or(idx, {"aa_hot"}, &single_or).ok());
  EXPECT_EQ(single_or, single);

  std::vector<uint32_t> absent;
  ASSERT_TRUE(query::boolean_or(idx, {"missing_a", "missing_b"}, &absent).ok());
  EXPECT_TRUE(absent.empty());

  std::remove(path.c_str());
}

TEST(BooleanOr, EmitsHighHitResultsInBulkChunks) {
  const Corpus corpus = BuildCorpus();
  const std::string path = TempPath();
  WriteCorpus(corpus, path);

  io::LocalFileReader file;
  SniiSegmentReader segment;
  LogicalIndexReader idx = OpenIndex(&file, &segment, path);

  RecordingSink sink;
  ASSERT_TRUE(query::boolean_or(idx, {"aa_hot"}, &sink).ok());

  const std::vector<uint32_t> want = corpus.any_docs({"aa_hot"});
  EXPECT_EQ(sink.out, want);
  ASSERT_FALSE(want.empty());
  EXPECT_GT(sink.max_chunk, 1u);
  EXPECT_LT(sink.chunks, want.size() / 100)
      << "high-hit term results must be handed off in bulk, not per doc";

  std::remove(path.c_str());
}

TEST(BooleanOr, PropagatesSinkAppendFailure) {
  const Corpus corpus = BuildCorpus();
  const std::string path = TempPath();
  WriteCorpus(corpus, path);

  io::LocalFileReader file;
  SniiSegmentReader segment;
  LogicalIndexReader idx = OpenIndex(&file, &segment, path);

  FailingSink sink;
  const Status st = query::boolean_or(idx, {"aa_hot"}, &sink);
  EXPECT_EQ(st.code(), StatusCode::kIoError) << st.to_string();
  EXPECT_TRUE(sink.saw_docids);

  std::remove(path.c_str());
}

TEST(BooleanOr, BatchesWindowedPostingReadsByStage) {
  const Corpus corpus = BuildCorpus();
  const std::string path = TempPath();
  WriteCorpus(corpus, path);

  io::LocalFileReader local;
  ASSERT_TRUE(local.open(path).ok());
  io::MeteredFileReader metered(&local, /*block_size=*/128);
  SniiSegmentReader segment;
  LogicalIndexReader idx = OpenMeteredIndex(&metered, &segment);

  const std::vector<std::string> terms = {"aa_hot", "aa_warm", "aa_loud"};
  for (const std::string& term : terms) {
    bool found = false;
    snii::format::DictEntry entry;
    uint64_t frq_base = 0;
    uint64_t prx_base = 0;
    ASSERT_TRUE(idx.lookup(term, &found, &entry, &frq_base, &prx_base).ok());
    ASSERT_TRUE(found) << term;
  }

  const uint64_t serial_before = metered.metrics().serial_rounds;
  std::vector<uint32_t> got;
  ASSERT_TRUE(query::boolean_or(idx, terms, &got).ok());
  const uint64_t serial_delta = metered.metrics().serial_rounds - serial_before;

  EXPECT_EQ(got, corpus.any_docs(terms));
  EXPECT_LE(serial_delta, 2u)
      << "windowed OR should batch posting reads by stage, not by term";

  std::remove(path.c_str());
}
