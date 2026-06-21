#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "corpus_gen.h"

// Multi-threaded Doris-english tokenization of a raw-text corpus into the same
// bench::Corpus the rest of the harness consumes. ONLY the tokenization is
// parallel (the agreed scope): each worker tokenizes a contiguous document range
// with a thread-local vocabulary, then a deterministic serial merge assigns the
// final vocab ids. The result is byte-for-byte identical to a single-threaded
// first-occurrence interning pass regardless of thread count, so the same Corpus
// feeds both engines and the cross-engine consistency check stays meaningful.
namespace bench {

// Tokenizes `bodies` (bodies[d] is document d's raw text, d == docid) using
// `threads` workers (0 is treated as 1; clamped to the document count). Vocab
// ids are assigned in order of each term's first appearance scanning documents
// 0,1,2,..., independent of `threads`.
Corpus tokenize_corpus(const std::vector<std::string>& bodies, uint32_t threads);

}  // namespace bench
