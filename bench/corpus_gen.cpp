#include "corpus_gen.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace bench {
namespace {

// splitmix64: a deterministic, well-distributed PRNG seeded from a single 64-bit
// state. Used in place of std::random_device so runs are fully reproducible.
class SplitMix64 {
 public:
  explicit SplitMix64(uint64_t seed) : state_(seed) {}

  uint64_t next() {
    uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  // Uniform double in [0, 1).
  double next_unit() {
    return static_cast<double>(next() >> 11) * (1.0 / 9007199254740992.0);
  }

  // Uniform integer in [0, n) (n > 0).
  uint32_t next_below(uint32_t n) {
    return static_cast<uint32_t>(next() % n);
  }

 private:
  uint64_t state_;
};

// Builds a vocabulary of short, unique, lowercase-ALPHABETIC tokens by encoding
// each id in a bijective base-26 alphabet: 0->"wa", 1->"wb", ... (a fixed "w"
// prefix keeps every token >= 2 chars and purely alphabetic). Alphabetic tokens
// survive any CLucene tokenizer (Letter / Simple / Standard) and a space-join
// unchanged, so CLucene tokens equal the corpus tokens exactly. Already
// lowercase, so a lowercasing analyzer is a no-op.
std::string encode_alpha(uint32_t id) {
  std::string s;
  uint32_t n = id + 1;  // bijective base-26 (1-indexed) so 0 maps to "a"
  while (n > 0) {
    uint32_t r = (n - 1) % 26;
    s.push_back(static_cast<char>('a' + r));
    n = (n - 1) / 26;
  }
  return "w" + s;  // fixed alpha prefix; result is always >= 2 letters
}

std::vector<std::string> build_vocab(uint32_t vocab_size) {
  std::vector<std::string> vocab;
  vocab.reserve(vocab_size);
  for (uint32_t i = 0; i < vocab_size; ++i) vocab.push_back(encode_alpha(i));
  return vocab;
}

// Precomputes the cumulative distribution function of a Zipf(s) law over
// `vocab_size` ranks so each token id can be sampled in O(log V) by binary
// search. rank r (1-based) has weight 1 / r^s.
std::vector<double> build_zipf_cdf(uint32_t vocab_size, double zipf_s) {
  std::vector<double> cdf(vocab_size);
  double cumulative = 0.0;
  for (uint32_t r = 0; r < vocab_size; ++r) {
    cumulative += 1.0 / std::pow(static_cast<double>(r + 1), zipf_s);
    cdf[r] = cumulative;
  }
  const double total = cumulative;
  for (double& v : cdf) v /= total;  // normalise to [0, 1]
  return cdf;
}

uint32_t sample_zipf(const std::vector<double>& cdf, SplitMix64* rng) {
  const double u = rng->next_unit();
  const auto it = std::lower_bound(cdf.begin(), cdf.end(), u);
  uint32_t id = static_cast<uint32_t>(it - cdf.begin());
  if (id >= cdf.size()) id = static_cast<uint32_t>(cdf.size()) - 1;
  return id;
}

}  // namespace

uint32_t Corpus::document_frequency(uint32_t id) const {
  uint32_t df = 0;
  for (const auto& doc : docs) {
    bool seen = false;
    for (uint32_t tid : doc) {
      if (tid == id) {
        seen = true;
        break;
      }
    }
    if (seen) ++df;
  }
  return df;
}

Corpus generate(uint32_t doc_count, uint32_t vocab_size, double zipf_s,
                uint32_t avg_doc_len, uint64_t seed) {
  Corpus c;
  c.doc_count = doc_count;
  c.vocab = build_vocab(vocab_size);
  c.docs.resize(doc_count);

  SplitMix64 rng(seed);
  const std::vector<double> cdf = build_zipf_cdf(vocab_size, zipf_s);

  // Each document length is drawn uniformly in [avg/2, avg*3/2] around the mean,
  // guaranteeing at least 1 token, so the corpus has a spread of doc lengths.
  const uint32_t min_len = std::max<uint32_t>(1, avg_doc_len / 2);
  const uint32_t span = std::max<uint32_t>(1, avg_doc_len);  // width of the range

  // Guarantee a very-high-df "anchor" term (vocab id 0) by planting it at the
  // FRONT of ~70% of documents (deterministically). With doc_count >= ~730 this
  // pushes its df past kSlimDfThreshold (512), forcing the SNII windowed posting
  // path and a non-empty .prx POD -- the regime this benchmark targets.
  const uint32_t anchor_id = 0;

  for (uint32_t d = 0; d < doc_count; ++d) {
    const uint32_t len = min_len + rng.next_below(span);
    std::vector<uint32_t>& toks = c.docs[d];
    toks.reserve(len + 1);
    const bool plant_anchor = (rng.next_unit() < 0.70);
    if (plant_anchor) toks.push_back(anchor_id);  // high-df term at position 0
    for (uint32_t k = 0; k < len; ++k) {
      toks.push_back(sample_zipf(cdf, &rng));
    }
  }
  return c;
}

namespace {

// The vocab id whose term string sorts lexicographically LAST. This is excluded
// from query-term selection because it occupies the very tail of the on-disk
// term dictionary; selecting only safely-reachable terms keeps the I/O
// comparison focused on the planning vs cursor behaviour rather than dictionary
// edge handling.
uint32_t lexicographic_max_id(const Corpus& c) {
  uint32_t best = 0;
  for (uint32_t i = 1; i < c.vocab.size(); ++i) {
    if (c.vocab[i] > c.vocab[best]) best = i;
  }
  return best;
}

}  // namespace

uint32_t highest_df_term(const Corpus& c) {
  const std::vector<uint32_t> df = all_dfs(c);
  const uint32_t excluded = lexicographic_max_id(c);
  uint32_t best = (excluded == 0 && df.size() > 1) ? 1u : 0u;
  for (uint32_t i = 0; i < df.size(); ++i) {
    if (i == excluded) continue;
    if (df[i] > df[best]) best = i;
  }
  return best;
}

uint32_t mid_df_term(const Corpus& c) {
  const std::vector<uint32_t> df = all_dfs(c);
  const uint32_t excluded = lexicographic_max_id(c);
  std::vector<std::pair<uint32_t, uint32_t>> nonzero;  // (df, id)
  for (uint32_t i = 0; i < df.size(); ++i) {
    if (i == excluded) continue;
    if (df[i] > 0) nonzero.emplace_back(df[i], i);
  }
  if (nonzero.empty()) return 0;
  std::sort(nonzero.begin(), nonzero.end());
  return nonzero[nonzero.size() / 2].second;
}

uint32_t low_df_term(const Corpus& c) {
  const std::vector<uint32_t> df = all_dfs(c);
  const uint32_t excluded = lexicographic_max_id(c);
  // Smallest non-zero df; ties broken by lexicographically-smallest term so the
  // result is always a safely-reachable, non-tail dictionary entry.
  uint32_t best = 0;
  uint32_t best_df = 0;
  for (uint32_t i = 0; i < df.size(); ++i) {
    if (i == excluded || df[i] == 0) continue;
    const bool better = best_df == 0 || df[i] < best_df ||
                        (df[i] == best_df && c.vocab[i] < c.vocab[best]);
    if (better) {
      best_df = df[i];
      best = i;
    }
  }
  return best;
}

std::vector<std::string> extract_phrase(const Corpus& c, uint32_t length) {
  if (length == 0) return {};
  const uint32_t excluded = lexicographic_max_id(c);
  // Find the first `length`-token window (in any document) that does NOT contain
  // the excluded tail term, and return that window's term strings. This
  // guarantees the phrase occurs in at least one document and that every term is
  // safely reachable in both indexes (so the oracle and both engines agree).
  for (uint32_t d = 0; d < c.doc_count; ++d) {
    const auto& toks = c.docs[d];
    if (toks.size() < length) continue;
    for (uint32_t start = 0; start + length <= toks.size(); ++start) {
      bool ok = true;
      for (uint32_t k = 0; k < length; ++k) {
        if (toks[start + k] == excluded) {
          ok = false;
          break;
        }
      }
      if (!ok) continue;
      std::vector<std::string> phrase;
      phrase.reserve(length);
      for (uint32_t k = 0; k < length; ++k) {
        phrase.push_back(c.vocab[toks[start + k]]);
      }
      return phrase;
    }
  }
  return {};
}

std::vector<uint32_t> all_dfs(const Corpus& c) {
  std::vector<uint32_t> df(c.vocab.size(), 0);
  std::vector<uint8_t> seen(c.vocab.size(), 0);
  for (const auto& doc : c.docs) {
    for (uint32_t tid : doc) {
      if (!seen[tid]) {
        seen[tid] = 1;
        ++df[tid];
      }
    }
    for (uint32_t tid : doc) seen[tid] = 0;  // reset only the touched entries
  }
  return df;
}

uint32_t term_in_df_bucket(const Corpus& c, double lo_frac, double hi_frac) {
  const std::vector<uint32_t> df = all_dfs(c);
  const uint32_t excluded = lexicographic_max_id(c);
  const double n = static_cast<double>(c.doc_count);
  const uint32_t lo = static_cast<uint32_t>(lo_frac * n);
  const uint32_t hi = static_cast<uint32_t>(hi_frac * n);
  uint32_t best = UINT32_MAX;
  for (uint32_t i = 0; i < df.size(); ++i) {
    if (i == excluded || df[i] < lo || df[i] > hi) continue;
    if (best == UINT32_MAX || c.vocab[i] < c.vocab[best]) best = i;
  }
  return best == UINT32_MAX ? 0 : best;
}

uint32_t term_at_df(const Corpus& c, uint32_t target_df) {
  const std::vector<uint32_t> df = all_dfs(c);
  const uint32_t excluded = lexicographic_max_id(c);
  uint32_t best = UINT32_MAX, best_dist = UINT32_MAX;
  for (uint32_t i = 0; i < df.size(); ++i) {
    if (i == excluded || df[i] == 0) continue;
    const uint32_t dist =
        df[i] > target_df ? df[i] - target_df : target_df - df[i];
    if (dist < best_dist ||
        (dist == best_dist && (best == UINT32_MAX || c.vocab[i] < c.vocab[best]))) {
      best_dist = dist;
      best = i;
    }
  }
  return best == UINT32_MAX ? 0 : best;
}

uint32_t df1_term(const Corpus& c) {
  const std::vector<uint32_t> df = all_dfs(c);
  const uint32_t excluded = lexicographic_max_id(c);
  uint32_t best = UINT32_MAX;
  for (uint32_t i = 0; i < df.size(); ++i) {
    if (i == excluded || df[i] != 1) continue;
    if (best == UINT32_MAX || c.vocab[i] < c.vocab[best]) best = i;
  }
  return best == UINT32_MAX ? low_df_term(c) : best;
}

std::string absent_token(const Corpus& c) {
  std::unordered_set<std::string> vocab(c.vocab.begin(), c.vocab.end());
  for (uint32_t n = 0; n < 1000000; ++n) {
    std::string cand = "zzq" + std::to_string(n);  // [a-z0-9] doris-english shape
    if (vocab.find(cand) == vocab.end()) return cand;
  }
  return "zzqabsentfallback";
}

std::pair<uint32_t, uint32_t> cooccurring_pair(const Corpus& c, double a_lo,
                                               double a_hi, double b_lo,
                                               double b_hi) {
  const std::vector<uint32_t> df = all_dfs(c);
  const uint32_t excluded = lexicographic_max_id(c);
  const double n = static_cast<double>(c.doc_count);
  const uint32_t b_lodf = static_cast<uint32_t>(b_lo * n);
  const uint32_t b_hidf = static_cast<uint32_t>(b_hi * n);
  const uint32_t a = term_in_df_bucket(c, a_lo, a_hi);
  for (const auto& doc : c.docs) {
    if (std::find(doc.begin(), doc.end(), a) == doc.end()) continue;
    for (uint32_t tid : doc) {
      if (tid == a || tid == excluded) continue;
      if (df[tid] >= b_lodf && df[tid] <= b_hidf) return {a, tid};
    }
  }
  return {a, a};  // no co-occurring B (caller checks first != second)
}

}  // namespace bench
