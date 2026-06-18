#include "clucene_oss_adapter.h"

#ifdef SNII_WITH_S3

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
#include "CLucene/store/Directory.h"
#include "CLucene/store/FSDirectory.h"
#include "CLucene/util/CLStreams.h"
#include "oss_clucene_directory.h"
#include "snii/common/slice.h"
#include "snii/common/status.h"
#include "snii/io/local_file.h"
#include "snii/io/s3_object_store.h"

namespace bench {

namespace cl_store = lucene::store;
namespace cl_index = lucene::index;
namespace cl_search = lucene::search;
namespace cl_doc = lucene::document;
namespace cl_analysis = lucene::analysis;

namespace {

// ASCII-only widening (the corpus vocabulary and field names are pure ASCII).
std::wstring widen(const std::string& s) {
  std::wstring out;
  out.reserve(s.size());
  for (unsigned char ch : s) out.push_back(static_cast<wchar_t>(ch));
  return out;
}

[[noreturn]] void fail(const std::string& what) {
  throw std::runtime_error("CLucene OSS adapter: " + what);
}

std::string make_temp_dir() {
  static int counter = 0;
  return "/tmp/snii_bench_cl_oss_" + std::to_string(::getpid()) + "_" +
         std::to_string(counter++);
}

void run_shell(const std::string& cmd) {
  if (std::system(cmd.c_str()) != 0) fail("shell command failed: " + cmd);
}

// HitCollector that records every matching docid (sorted afterwards).
class DocidCollector : public cl_search::HitCollector {
 public:
  void collect(const int32_t doc, const float_t /*score*/) override {
    docids.push_back(static_cast<uint32_t>(doc));
  }
  std::vector<uint32_t> docids;
};

// Reads a whole local file into memory (for upload).
snii::Status read_whole_file(const std::string& path, std::vector<uint8_t>* out) {
  snii::io::LocalFileReader r;
  if (snii::Status s = r.open(path); !s.ok()) return s;
  return r.read_at(0, r.size(), out);
}

}  // namespace

struct CluceneOssAdapter::Impl {
  std::string dir_path;  // local temp build dir (removed in dtor)
  std::vector<std::string> file_names;     // segment file names (no prefix)
  std::vector<std::string> uploaded_keys;  // prefix + "/" + name
  std::unique_ptr<OssCluceneDirectory> directory;
  std::unique_ptr<cl_search::IndexSearcher> searcher;
  cl_index::IndexReader* reader = nullptr;

  ~Impl() {
    searcher.reset();  // closes the searcher (isOwner=false, reader untouched)
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

CluceneOssAdapter::CluceneOssAdapter() : impl_(std::make_unique<Impl>()) {}
CluceneOssAdapter::~CluceneOssAdapter() = default;

const std::vector<std::string>& CluceneOssAdapter::uploaded_keys() const {
  return impl_->uploaded_keys;
}

void CluceneOssAdapter::build_upload_and_open(const Corpus& c,
                                              const snii::io::S3Config& cfg) {
  impl_->dir_path = make_temp_dir();
  run_shell("mkdir -p '" + impl_->dir_path + "'");

  // --- Build phase: identical to CluceneAdapter (separate segment files). ---
  {
    cl_store::FSDirectory* build_dir =
        cl_store::FSDirectory::getDirectory(impl_->dir_path.c_str());
    if (build_dir == nullptr) fail("getDirectory (build) returned null");

    auto* analyzer = _CLNEW cl_analysis::SimpleAnalyzer<char>();
    analyzer->set_stopwords(nullptr);
    auto* writer = _CLNEW cl_index::IndexWriter(build_dir, analyzer,
                                                /*create=*/true);
    writer->setUseCompoundFile(false);
    writer->setMaxFieldLength(0x7FFFFFFFL);

    auto* sreader = _CLNEW lucene::util::SStringReader<char>();

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
      sreader->init(joined.data(), static_cast<int32_t>(joined.size()),
                    /*copyData=*/false);
      auto* stream = analyzer->reusableTokenStream(field->name(), sreader);
      field->setValue(stream);
      writer->addDocument(doc);
    }
    writer->optimize();
    writer->close();

    _CLLDELETE(writer);
    _CLLDELETE(doc);
    _CLLDELETE(analyzer);
    _CLLDELETE(sreader);

    // Enumerate the generated segment files before closing the build directory.
    std::vector<std::string> names;
    if (!build_dir->list(&names)) {
      build_dir->close();
      fail("could not list build directory files");
    }
    impl_->file_names = std::move(names);
    build_dir->close();
  }

  if (impl_->file_names.empty()) fail("build produced no index files");

  // --- Upload phase: push every segment file to OSS under cfg.prefix. ---
  for (const std::string& name : impl_->file_names) {
    const std::string local_path = impl_->dir_path + "/" + name;
    std::vector<uint8_t> bytes;
    if (snii::Status s = read_whole_file(local_path, &bytes); !s.ok()) {
      fail("read local index file '" + name + "': " + s.to_string());
    }
    snii::io::S3FileWriter w;
    if (snii::Status s = w.open(cfg, name); !s.ok()) {
      fail("S3 open '" + name + "': " + s.to_string());
    }
    if (snii::Status s = w.append(snii::Slice(bytes)); !s.ok()) {
      fail("S3 append '" + name + "': " + s.to_string());
    }
    if (snii::Status s = w.finalize(); !s.ok()) {
      fail("S3 finalize '" + name + "': " + s.to_string());
    }
    impl_->uploaded_keys.push_back(cfg.prefix + "/" + name);
  }

  // --- Read phase: open through the OSS-backed metered directory. ---
  impl_->directory =
      std::make_unique<OssCluceneDirectory>(cfg, impl_->file_names);
  impl_->reader = cl_index::IndexReader::open(impl_->directory->directory());
  if (impl_->reader == nullptr) fail("IndexReader::open returned null");
  impl_->searcher = std::make_unique<cl_search::IndexSearcher>(
      impl_->reader, /*isOwner=*/false);
}

void CluceneOssAdapter::term_query(const std::string& term,
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

void CluceneOssAdapter::phrase_query(const std::vector<std::string>& words,
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

#endif  // SNII_WITH_S3
