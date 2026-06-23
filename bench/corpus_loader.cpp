#include "corpus_loader.h"

#include <cstdio>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "doris_english_analyzer.h"

namespace bench {

namespace {

// The streaming loader applies the SAME tokenization rule as the parquet path,
// sourced from the shared Doris-english analyzer so the two stay in lockstep.
constexpr size_t kMaxTokenLen = kDorisEnglishMaxTokenLen;

// RAII wrapper: closes the FILE* on any scope exit (including a thrown
// allocation), so the descriptor never leaks. std::fclose ignores nullptr-free
// inputs because the deleter is only invoked on a non-null pointer.
struct FileCloser {
  void operator()(std::FILE* fp) const noexcept {
    if (fp != nullptr) std::fclose(fp);
  }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

// Narrows a container size to uint32_t, refusing silently-truncating overflow.
// doc/vocab counts that reach UINT32_MAX would corrupt the .idx docid space.
uint32_t checked_u32(size_t n, const char* what) {
  if (n >= std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("corpus_loader: ") + what +
                             " exceeds uint32 range");
  }
  return static_cast<uint32_t>(n);
}

}  // namespace

Corpus load_from_file(const std::string& path, uint32_t max_docs) {
  FilePtr fp(std::fopen(path.c_str(), "rb"));
  if (fp == nullptr) {
    throw std::runtime_error("corpus_loader: cannot open " + path);
  }

  Corpus c;
  std::unordered_map<std::string, uint32_t> vocab_index;
  vocab_index.reserve(1u << 20);

  constexpr size_t kBufSize = 1u << 20;  // 1 MiB read buffer
  std::vector<char> buf(kBufSize);
  std::vector<uint32_t> cur_doc;        // token ids of the current line
  std::string tok;                      // current token being assembled
  bool doc_open = false;                // any byte seen on the current line

  auto flush_token = [&]() {
    if (tok.empty()) return;
    auto it = vocab_index.find(tok);
    uint32_t id;
    if (it == vocab_index.end()) {
      // A new vocab id is the current vocab size; guard the narrowing so the id
      // space can never silently wrap past uint32.
      id = checked_u32(c.vocab.size(), "vocab size");
      c.vocab.push_back(tok);
      vocab_index.emplace(tok, id);
    } else {
      id = it->second;
    }
    cur_doc.push_back(id);
    tok.clear();
  };

  auto flush_doc = [&]() {
    flush_token();
    c.docs.push_back(std::move(cur_doc));
    cur_doc.clear();
    doc_open = false;
  };

  bool stop = false;
  while (!stop) {
    const size_t n = std::fread(buf.data(), 1, kBufSize, fp.get());
    if (n == 0) break;  // 0 => EOF or error; checked after the loop via ferror.
    for (size_t i = 0; i < n; ++i) {
      const char ch = buf[i];
      if (ch == '\n') {
        flush_doc();
        if (max_docs != 0 && c.docs.size() >= max_docs) {
          stop = true;
          break;
        }
        continue;
      }
      doc_open = true;
      const int nc = doris_english_normalize(static_cast<unsigned char>(ch));
      if (nc < 0) {
        flush_token();
      } else if (tok.size() < kMaxTokenLen) {
        tok.push_back(static_cast<char>(nc));
      }
    }
  }

  // A short read can mean EOF *or* a stream error. Distinguish them: a genuine
  // I/O error must surface, not be silently treated as a truncated-but-valid
  // corpus. (Only meaningful when we stopped on fread, not on max_docs.)
  if (!stop && std::ferror(fp.get())) {
    throw std::runtime_error("corpus_loader: read error on " + path);
  }

  // A final line without a trailing newline still becomes a document.
  if (doc_open && (max_docs == 0 || c.docs.size() < max_docs)) flush_doc();

  c.doc_count = checked_u32(c.docs.size(), "doc count");
  // fp is closed by FilePtr's deleter on return.
  return c;
}

}  // namespace bench
