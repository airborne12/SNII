#include "parallel_tokenizer.h"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>

#include "doris_english_analyzer.h"

namespace bench {

namespace {

// One worker's tokenization of a contiguous document range. `local_vocab` lists
// the range's distinct terms in first-occurrence order (the term's local id is
// its index here); `docs` holds each document's local term-id sequence.
struct ShardResult {
  std::vector<std::string> local_vocab;
  std::vector<std::vector<uint32_t>> docs;
};

uint32_t checked_u32(size_t n, const char* what) {
  if (n >= std::numeric_limits<uint32_t>::max()) {
    throw std::runtime_error(std::string("parallel_tokenizer: ") + what +
                             " exceeds uint32 range");
  }
  return static_cast<uint32_t>(n);
}

// Tokenizes bodies[lo, hi) into `out` using a thread-local vocabulary. Pure: it
// touches no shared state, so any number of shards run without synchronization.
void tokenize_shard(const std::vector<std::string>& bodies, size_t lo, size_t hi,
                    ShardResult* out) {
  std::unordered_map<std::string, uint32_t> local_index;
  out->docs.resize(hi - lo);
  for (size_t d = lo; d < hi; ++d) {
    std::vector<uint32_t>& doc = out->docs[d - lo];
    doris_english_for_each_token(bodies[d], [&](std::string_view t) {
      auto it = local_index.find(std::string(t));
      uint32_t id;
      if (it == local_index.end()) {
        id = static_cast<uint32_t>(out->local_vocab.size());
        out->local_vocab.emplace_back(t);
        local_index.emplace(std::string(t), id);
      } else {
        id = it->second;
      }
      doc.push_back(id);
    });
  }
}

}  // namespace

Corpus tokenize_corpus(const std::vector<std::string>& bodies, uint32_t threads) {
  Corpus c;
  const size_t n = bodies.size();
  c.doc_count = checked_u32(n, "doc count");
  if (n == 0) return c;

  // Clamp the worker count to [1, n] so every shard owns at least one document.
  size_t t = threads == 0 ? 1 : threads;
  t = std::min(t, n);

  std::vector<ShardResult> shards(t);
  std::vector<std::thread> workers;
  workers.reserve(t > 0 ? t - 1 : 0);

  // Contiguous, balanced ranges: the first (n % t) shards get one extra doc.
  const size_t base = n / t;
  const size_t rem = n % t;
  auto shard_lo = [&](size_t s) {
    return s * base + std::min(s, rem);
  };

  // Run shards 1..t-1 on worker threads; shard 0 runs inline on this thread.
  for (size_t s = 1; s < t; ++s) {
    workers.emplace_back(tokenize_shard, std::cref(bodies), shard_lo(s),
                         shard_lo(s + 1), &shards[s]);
  }
  tokenize_shard(bodies, shard_lo(0), shard_lo(1), &shards[0]);
  for (auto& w : workers) w.join();

  // Serial merge: walk shards in order, each shard's local vocab in local-id
  // (first-occurrence) order, interning into the global vocab. This reproduces a
  // single-threaded doc-order first-occurrence pass exactly.
  c.docs.resize(n);
  std::unordered_map<std::string, uint32_t> global_index;
  for (size_t s = 0; s < t; ++s) {
    ShardResult& sr = shards[s];
    std::vector<uint32_t> remap(sr.local_vocab.size());
    for (size_t lid = 0; lid < sr.local_vocab.size(); ++lid) {
      const std::string& term = sr.local_vocab[lid];
      auto it = global_index.find(term);
      uint32_t gid;
      if (it == global_index.end()) {
        gid = checked_u32(c.vocab.size(), "vocab size");
        c.vocab.push_back(term);
        global_index.emplace(term, gid);
      } else {
        gid = it->second;
      }
      remap[lid] = gid;
    }
    const size_t lo = shard_lo(s);
    for (size_t i = 0; i < sr.docs.size(); ++i) {
      std::vector<uint32_t>& doc = sr.docs[i];
      for (uint32_t& id : doc) id = remap[id];
      c.docs[lo + i] = std::move(doc);
    }
  }
  return c;
}

Corpus keyword_corpus(const std::vector<std::string>& values) {
  Corpus c;
  c.doc_count = checked_u32(values.size(), "doc count");
  c.docs.resize(values.size());
  std::unordered_map<std::string, uint32_t> index;
  for (size_t d = 0; d < values.size(); ++d) {
    if (values[d].empty()) continue;  // empty value -> empty document
    auto it = index.find(values[d]);
    uint32_t id;
    if (it == index.end()) {
      id = checked_u32(c.vocab.size(), "vocab size");
      c.vocab.push_back(values[d]);
      index.emplace(values[d], id);
    } else {
      id = it->second;
    }
    c.docs[d].push_back(id);
  }
  return c;
}

}  // namespace bench
