#include "corpus_loader.h"

#include <cstdio>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace bench {

namespace {

constexpr size_t kMaxTokenLen = 64;

// Lowercases ASCII letters; leaves digits as-is; everything else is a separator.
inline int normalize(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
  if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) return c;
  return -1;  // separator
}

}  // namespace

Corpus load_from_file(const std::string& path, uint32_t max_docs) {
  std::FILE* fp = std::fopen(path.c_str(), "rb");
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
      id = static_cast<uint32_t>(c.vocab.size());
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
    const size_t n = std::fread(buf.data(), 1, kBufSize, fp);
    if (n == 0) break;
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
      const int nc = normalize(static_cast<unsigned char>(ch));
      if (nc < 0) {
        flush_token();
      } else if (tok.size() < kMaxTokenLen) {
        tok.push_back(static_cast<char>(nc));
      }
    }
  }
  // A final line without a trailing newline still becomes a document.
  if (doc_open && (max_docs == 0 || c.docs.size() < max_docs)) flush_doc();

  std::fclose(fp);
  c.doc_count = static_cast<uint32_t>(c.docs.size());
  return c;
}

}  // namespace bench
