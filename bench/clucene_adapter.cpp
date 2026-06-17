#include "clucene_adapter.h"

#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "CLucene.h"
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
    if (!dir_path.empty()) {
      (void)std::system(("rm -rf '" + dir_path + "'").c_str());
    }
  }
};

CluceneAdapter::CluceneAdapter() : impl_(std::make_unique<Impl>()) {}
CluceneAdapter::~CluceneAdapter() = default;

void CluceneAdapter::build_and_open(const Corpus& c) {
  impl_->dir_path = make_temp_dir();
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

    auto* reader = _CLNEW lucene::util::SStringReader<char>();

    const std::wstring field_name = widen("body");
    int32_t field_config =
        cl_doc::Field::STORE_NO | cl_doc::Field::INDEX_TOKENIZED;
    auto* doc = _CLNEW cl_doc::Document();
    auto* field = _CLNEW cl_doc::Field(field_name.c_str(), field_config);
    field->setOmitTermFreqAndPositions(false);  // keep positions for PhraseQuery
    doc->add(*field);

    for (uint32_t d = 0; d < c.doc_count; ++d) {
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
    writer->optimize();  // collapse to a single compound segment
    writer->close();

    _CLLDELETE(writer);
    _CLLDELETE(doc);  // owns its fields
    _CLLDELETE(analyzer);
    _CLLDELETE(reader);
    build_dir->close();
  }

  // --- Read phase: open through the metered directory. ---
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

}  // namespace bench
