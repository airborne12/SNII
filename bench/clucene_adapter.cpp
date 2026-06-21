#include "clucene_adapter.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "CLucene.h"
#include "CLucene/search/BooleanQuery.h"
#include "CLucene/store/FSDirectory.h"
#include "CLucene/util/CLStreams.h"
#include "snii/io/local_file.h"

namespace bench {

namespace cl_store = lucene::store;
namespace cl_index = lucene::index;
namespace cl_search = lucene::search;
namespace cl_doc = lucene::document;
namespace cl_analysis = lucene::analysis;

namespace {

// Converts a plain ASCII std::string to a wchar_t (TCHAR) string. The corpus
// vocabulary and field names are pure ASCII, so a 1:1 widening is exact.
std::wstring widen(const std::string& s) {
  std::wstring out;
  out.reserve(s.size());
  for (unsigned char ch : s) out.push_back(static_cast<wchar_t>(ch));
  return out;
}

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error("CLucene adapter: " + what);
}

std::string make_temp_dir() {
  static int counter = 0;
  return "/tmp/snii_bench_cl_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++);
}

void run_shell(const std::string& cmd) {
  if (std::system(cmd.c_str()) != 0) fail("shell command failed: " + cmd);
}

}  // namespace

// FAIRNESS HELPER (lever 4): how many docs CLucene should buffer before flushing
// a segment, so its per-flush RAM matches SNII's spill-at-`ram_buffer_mb_` MiB.
// SNII's spill accounting (SpimiTermBuffer): a doc with L tokens adds about
//   distinct_terms*kBytesPerDocEntry + L*kBytesPerPosition  ~=  L*(8 + 4) = L*12
// bytes (distinct-terms-per-doc ~= L for a high-vocab corpus). So the doc count
// whose buffered postings reach `mb` MiB is mb*MiB / (avg_L * 12). Clamped to >=1.
int32_t CluceneAdapter::flush_doc_count(const Corpus& c) const {
  constexpr double kBytesPerDocEntry = 8.0;   // SNII: one docid + one freq
  constexpr double kBytesPerPosition = 4.0;   // SNII: one position
  uint64_t total_tokens = 0;
  for (const auto& d : c.docs) total_tokens += d.size();
  const double avg_len =
      c.doc_count == 0 ? 1.0
                       : static_cast<double>(total_tokens) / static_cast<double>(c.doc_count);
  const double bytes_per_doc = avg_len * (kBytesPerDocEntry + kBytesPerPosition);
  const double budget = ram_buffer_mb_ * 1024.0 * 1024.0;
  const double n = budget / (bytes_per_doc > 0.0 ? bytes_per_doc : 1.0);
  if (n < 1.0) return 1;
  if (n > 2.0e9) return static_cast<int32_t>(2.0e9);
  return static_cast<int32_t>(n);
}

// A BufferedIndexInput backed by a snii MeteredFileReader. The buffered base
// class issues readInternal() calls at the position established by seekInternal;
// each becomes one MeteredFileReader::read_at -- exactly the object-storage cost
// model SNII is measured against. Clones share the SAME MeteredFileReader so all
// reads of one file aggregate into one metric set.
class MeteredIndexInput : public cl_store::BufferedIndexInput {
 public:
  MeteredIndexInput(snii::io::MeteredFileReader* reader, int64_t length)
      : reader_(reader), length_(length), pos_(0) {}

  MeteredIndexInput(const MeteredIndexInput& other)
      : cl_store::BufferedIndexInput(other),
        reader_(other.reader_),
        length_(other.length_),
        pos_(other.pos_) {}

  cl_store::IndexInput* clone() const override {
    return _CLNEW MeteredIndexInput(*this);
  }

  int64_t length() const override { return length_; }

  const char* getDirectoryType() const override { return "MeteredDirectory"; }
  const char* getObjectName() const override { return getClassName(); }
  static const char* getClassName() { return "MeteredIndexInput"; }

  void close() override {}

 protected:
  void readInternal(uint8_t* b, const int32_t len) override {
    if (len <= 0) return;
    std::vector<uint8_t> buf;
    const snii::Status s = reader_->read_at(
        static_cast<uint64_t>(pos_), static_cast<size_t>(len), &buf);
    if (!s.ok() || buf.size() != static_cast<size_t>(len)) {
      _CLTHROWA(CL_ERR_IO, "MeteredIndexInput: short read");
    }
    std::memcpy(b, buf.data(), buf.size());
    pos_ += len;
  }

  void seekInternal(const int64_t pos) override { pos_ = pos; }

 private:
  snii::io::MeteredFileReader* reader_;  // not owned (lives in MeteredDirectory)
  int64_t length_;
  int64_t pos_;
};

// A Directory that delegates writes/listing to an internal FSDirectory but
// routes every openInput() through a per-file MeteredFileReader, so all physical
// reads pass through the shared object-storage cost model. The metered readers
// are retained so their metrics can be aggregated and reset between queries.
class MeteredDirectory : public cl_store::Directory {
 public:
  explicit MeteredDirectory(const std::string& path) {
    fs_ = cl_store::FSDirectory::getDirectory(path.c_str());
    if (fs_ == nullptr) fail("FSDirectory::getDirectory returned null");
  }

  ~MeteredDirectory() override {
    if (fs_ != nullptr) fs_->close();  // ref-counted close
  }

  snii::io::IoMetrics aggregate_metrics() const {
    snii::io::IoMetrics total;
    for (const auto& mr : metered_) {
      const snii::io::IoMetrics& m = mr->metrics();
      total.read_at_calls += m.read_at_calls;
      total.serial_rounds += m.serial_rounds;
      total.range_gets += m.range_gets;
      total.remote_bytes += m.remote_bytes;
      total.total_request_bytes += m.total_request_bytes;
    }
    return total;
  }

  // Resets every retained metered reader's counters AND cache, modelling a cold
  // cache for the next query. The reader objects themselves are NOT freed: live
  // IndexInputs opened by the IndexReader (e.g. the term dictionary) hold raw
  // pointers into them and must keep reading across queries.
  void reset_metrics() {
    for (auto& mr : metered_) mr->reset_metrics();
  }

  bool list(std::vector<std::string>* names) const override {
    return fs_->list(names);
  }
  bool fileExists(const char* name) const override {
    return fs_->fileExists(name);
  }
  int64_t fileModified(const char* name) const override {
    return fs_->fileModified(name);
  }
  int64_t fileLength(const char* name) const override {
    return fs_->fileLength(name);
  }
  void touchFile(const char* name) override { fs_->touchFile(name); }
  void renameFile(const char* from, const char* to) override {
    fs_->renameFile(from, to);
  }
  cl_store::IndexOutput* createOutput(const char* name) override {
    return fs_->createOutput(name);
  }
  void close() override { /* directory lifetime managed by adapter */ }
  std::string toString() const override { return "MeteredDirectory"; }
  const char* getObjectName() const override { return getClassName(); }
  static const char* getClassName() { return "MeteredDirectory"; }

  bool openInput(const char* name, cl_store::IndexInput*& ret,
                 CLuceneError& error, int32_t /*bufferSize*/) override {
    const std::string full = std::string(fs_->getDirName()) + "/" + name;
    auto local = std::make_unique<snii::io::LocalFileReader>();
    if (snii::Status s = local->open(full); !s.ok()) {
      error.set(CL_ERR_IO, "MeteredDirectory: cannot open file");
      return false;
    }
    auto metered = std::make_unique<snii::io::MeteredFileReader>(local.get());
    const int64_t len = static_cast<int64_t>(metered->size());
    ret = _CLNEW MeteredIndexInput(metered.get(), len);
    locals_.push_back(std::move(local));
    metered_.push_back(std::move(metered));
    return true;
  }

 protected:
  bool doDeleteFile(const char* name) override {
    return fs_->deleteFile(name, false);
  }

 private:
  cl_store::FSDirectory* fs_ = nullptr;  // ref-counted
  std::vector<std::unique_ptr<snii::io::LocalFileReader>> locals_;
  std::vector<std::unique_ptr<snii::io::MeteredFileReader>> metered_;
};

namespace {

// HitCollector that records every matching docid (sorted afterwards).
class DocidCollector : public cl_search::HitCollector {
 public:
  void collect(const int32_t doc, const float_t /*score*/) override {
    docids.push_back(static_cast<uint32_t>(doc));
  }
  std::vector<uint32_t> docids;
};

}  // namespace

struct CluceneAdapter::Impl {
  std::string dir_path;
  bool keep_dir = false;  // when true, leave the segment files on disk
  std::unique_ptr<MeteredDirectory> directory;
  std::unique_ptr<cl_search::IndexSearcher> searcher;
  cl_index::IndexReader* reader = nullptr;

  ~Impl() {
    searcher.reset();  // closes the searcher (not the reader, isOwner=false)
    if (reader) {
      reader->close();
      _CLLDELETE(reader);
    }
    directory.reset();
    if (!dir_path.empty() && !keep_dir) {
      (void)std::system(("rm -rf '" + dir_path + "'").c_str());
    }
  }
};

CluceneAdapter::CluceneAdapter() : impl_(std::make_unique<Impl>()) {}
CluceneAdapter::~CluceneAdapter() = default;

void CluceneAdapter::build_and_open(const Corpus& c) {
  build_at(make_temp_dir(), c, /*keep_on_disk=*/false);
}

std::vector<std::string> CluceneAdapter::index_files() const {
  if (impl_ == nullptr || impl_->dir_path.empty()) return {};
  std::vector<std::string> files;
  std::error_code ec;
  for (const auto& e :
       std::filesystem::directory_iterator(impl_->dir_path, ec)) {
    if (e.is_regular_file(ec)) files.push_back(e.path().string());
  }
  std::sort(files.begin(), files.end());
  return files;
}

void CluceneAdapter::build_at(const std::string& dir, const Corpus& c,
                              bool keep_on_disk) {
  build_range(dir, c, 0, c.doc_count, keep_on_disk);
}

void CluceneAdapter::build_range(const std::string& dir, const Corpus& c,
                                 uint32_t doc_lo, uint32_t doc_hi,
                                 bool keep_on_disk) {
  if (doc_hi > c.doc_count || doc_lo > doc_hi) {
    fail("build_range bad [lo,hi)");
  }
  impl_->dir_path = dir;
  impl_->keep_dir = keep_on_disk;
  run_shell("mkdir -p '" + impl_->dir_path + "'");

  // --- Build phase: write the compound index with a plain FSDirectory. ---
  // Mirrors the Doris indexing path: SimpleAnalyzer<char> drives the char-based
  // SDocumentsWriter; each doc's space-joined token string is streamed through a
  // reusable token stream into one tokenized, position-indexed field "body".
  {
    cl_store::FSDirectory* build_dir =
        cl_store::FSDirectory::getDirectory(impl_->dir_path.c_str());
    if (build_dir == nullptr) fail("getDirectory (build) returned null");

    auto* analyzer = _CLNEW cl_analysis::SimpleAnalyzer<char>();
    analyzer->set_stopwords(nullptr);
    auto* writer = _CLNEW cl_index::IndexWriter(build_dir, analyzer,
                                                /*create=*/true);
    // The Doris CLucene fork does not implement legacy compound-file creation,
    // so the index is written as separate segment files (.fdx/.tii/.tis/.frq/
    // .prx/...). The MeteredDirectory still routes EVERY physical read of EVERY
    // file through a MeteredFileReader and aggregates the metrics, so CLucene's
    // cursor reads are measured by the same object-storage cost model as SNII.
    writer->setUseCompoundFile(false);
    writer->setMaxFieldLength(0x7FFFFFFFL);  // never truncate a document

    // FAIRNESS (lever 4): match CLucene's flush threshold to SNII's spill.
    // We attempted doc-count flush via setMaxBufferedDocs(flush_doc_count(c)) so a
    // higher spill threshold could be compared fairly (CLucene also flushing
    // larger). BUT at 5M this Doris CLucene fork SEGFAULTS the moment ANY flush
    // setter (setMaxBufferedDocs OR setRAMBufferSizeMB) is called after
    // construction -- the resulting multi-segment merge cascade + optimize() path
    // faults (the UNtouched constructor default is the only stable config, and it
    // is exactly a 16 MiB RAM-buffer flush). So CLucene is PINNED at ~16 MiB.
    // The fair, matched comparison point is therefore SNII --spill-mib 16 (both
    // flush at ~16 MiB). At higher SNII thresholds CLucene still flushes at 16 MiB
    // (i.e. MORE often than SNII), so the memory comparison is, if anything,
    // generous to CLucene there. flush_doc_count() is retained for the model and a
    // future fork that does not crash.
    if (no_merge_) {
      // Keep the stable constructor-default (~16 MiB) periodic flush so each
      // segment stays small; do NOT set a RAM-buffer (setters destabilise this
      // fork) and do NOT optimize() later -- this is the only path that survives
      // corpora exceeding ~2^30 total positions.
    } else if (ram_buffer_mb_ <= 0.0) {
      writer->setRAMBufferSizeMB(1.0e9f);  // no auto flush (in-RAM build)
    } else {
      (void)flush_doc_count(c);  // computed for the fairness model; see note above
    }

    auto* reader = _CLNEW lucene::util::SStringReader<char>();

    const std::wstring field_name = widen("body");
    int32_t field_config =
        cl_doc::Field::STORE_NO | cl_doc::Field::INDEX_TOKENIZED;
    auto* doc = _CLNEW cl_doc::Document();
    auto* field = _CLNEW cl_doc::Field(field_name.c_str(), field_config);
    field->setOmitTermFreqAndPositions(false);  // keep positions for PhraseQuery
    doc->add(*field);

    // Docs in [doc_lo, doc_hi) are added in order, so CLucene assigns this
    // shard LOCAL docids 0..(doc_hi-doc_lo-1); the caller maps back to the global
    // docid by adding doc_lo.
    for (uint32_t d = doc_lo; d < doc_hi; ++d) {
      std::string joined;
      const auto& toks = c.docs[d];
      for (uint32_t k = 0; k < toks.size(); ++k) {
        if (k) joined.push_back(' ');
        joined += c.vocab[toks[k]];
      }
      reader->init(joined.data(), static_cast<int32_t>(joined.size()),
                   /*copyData=*/false);
      auto* stream = analyzer->reusableTokenStream(field->name(), reader);
      field->setValue(stream);
      writer->addDocument(doc);  // doc/field reused across all documents
    }
    if (!no_merge_) {
      writer->optimize();  // collapse to a single compound segment
    }
    writer->close();

    _CLLDELETE(writer);
    _CLLDELETE(doc);  // owns its fields
    _CLLDELETE(analyzer);
    _CLLDELETE(reader);
    build_dir->close();
  }

  // --- Read phase: open through the metered directory. ---
  open_metered_reader();
}

void CluceneAdapter::open_existing(const std::string& dir) {
  impl_->dir_path = dir;
  impl_->keep_dir = true;  // we did not create it; never delete it
  open_metered_reader();
}

void CluceneAdapter::open_metered_reader() {
  impl_->directory = std::make_unique<MeteredDirectory>(impl_->dir_path);
  impl_->reader = cl_index::IndexReader::open(impl_->directory.get());
  if (impl_->reader == nullptr) fail("IndexReader::open returned null");
  impl_->searcher = std::make_unique<cl_search::IndexSearcher>(
      impl_->reader, /*isOwner=*/false);
}

void CluceneAdapter::term_query(const std::string& term,
                                std::vector<uint32_t>* docids,
                                snii::io::IoMetrics* metrics) {
  impl_->directory->reset_metrics();

  const std::wstring field = widen("body");
  const std::wstring text = widen(term);
  auto* t = _CLNEW cl_index::Term(field.c_str(), text.c_str());
  auto* q = _CLNEW cl_search::TermQuery(t);
  _CLDECDELETE(t);

  DocidCollector collector;
  impl_->searcher->_search(q, /*filter=*/nullptr, &collector);
  _CLDELETE(q);

  std::sort(collector.docids.begin(), collector.docids.end());
  *docids = std::move(collector.docids);
  *metrics = impl_->directory->aggregate_metrics();
}

void CluceneAdapter::boolean_and(const std::vector<std::string>& terms,
                                 std::vector<uint32_t>* docids,
                                 snii::io::IoMetrics* metrics) {
  boolean_query(terms, /*conjunction=*/true, docids, metrics);
}

void CluceneAdapter::boolean_or(const std::vector<std::string>& terms,
                                std::vector<uint32_t>* docids,
                                snii::io::IoMetrics* metrics) {
  boolean_query(terms, /*conjunction=*/false, docids, metrics);
}

void CluceneAdapter::boolean_query(const std::vector<std::string>& terms,
                                   bool conjunction, std::vector<uint32_t>* docids,
                                   snii::io::IoMetrics* metrics) {
  impl_->directory->reset_metrics();
  const std::wstring field = widen("body");
  auto* bq = _CLNEW cl_search::BooleanQuery();
  const cl_search::BooleanClause::Occur occur =
      conjunction ? cl_search::BooleanClause::MUST
                  : cl_search::BooleanClause::SHOULD;
  std::vector<std::wstring> texts;  // keep widened texts alive during add()
  texts.reserve(terms.size());
  for (const std::string& w : terms) {
    texts.push_back(widen(w));
    auto* t = _CLNEW cl_index::Term(field.c_str(), texts.back().c_str());
    auto* tq = _CLNEW cl_search::TermQuery(t);
    _CLDECDELETE(t);
    bq->add(tq, /*deleteQuery=*/true, occur);
  }
  DocidCollector collector;
  impl_->searcher->_search(bq, /*filter=*/nullptr, &collector);
  _CLDELETE(bq);

  std::sort(collector.docids.begin(), collector.docids.end());
  *docids = std::move(collector.docids);
  *metrics = impl_->directory->aggregate_metrics();
}

void CluceneAdapter::match_all(std::vector<uint32_t>* docids,
                               snii::io::IoMetrics* metrics) {
  // Every docid (no deletions) -- no posting I/O, like the SNII path.
  impl_->directory->reset_metrics();
  const int32_t n = impl_->reader->numDocs();
  docids->resize(n);
  for (int32_t i = 0; i < n; ++i) (*docids)[i] = static_cast<uint32_t>(i);
  *metrics = impl_->directory->aggregate_metrics();
}

void CluceneAdapter::phrase_query(const std::vector<std::string>& words,
                                  std::vector<uint32_t>* docids,
                                  snii::io::IoMetrics* metrics) {
  impl_->directory->reset_metrics();

  const std::wstring field = widen("body");
  auto* q = _CLNEW cl_search::PhraseQuery();
  std::vector<std::wstring> texts;  // keep widened texts alive during add()
  texts.reserve(words.size());
  for (const auto& w : words) {
    texts.push_back(widen(w));
    auto* t = _CLNEW cl_index::Term(field.c_str(), texts.back().c_str());
    q->add(t);
    _CLDECDELETE(t);
  }

  DocidCollector collector;
  impl_->searcher->_search(q, /*filter=*/nullptr, &collector);
  _CLDELETE(q);

  std::sort(collector.docids.begin(), collector.docids.end());
  *docids = std::move(collector.docids);
  *metrics = impl_->directory->aggregate_metrics();
}

uint64_t CluceneAdapter::index_bytes() const {
  if (impl_ == nullptr || impl_->dir_path.empty()) return 0;
  std::error_code ec;
  uint64_t total = 0;
  for (const auto& e :
       std::filesystem::directory_iterator(impl_->dir_path, ec)) {
    if (e.is_regular_file(ec)) total += e.file_size(ec);
  }
  return total;
}

}  // namespace bench
