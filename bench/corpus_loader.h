#pragma once

#include <cstdint>
#include <string>

#include "corpus_gen.h"

// Loads a REAL text corpus (one document per line) into the same bench::Corpus
// the synthetic generator produces, so the entire SNII-vs-CLucene comparison
// (build resources + query I/O metrics) runs unchanged on real data.
namespace bench {

// Reads up to `max_docs` lines from `path` (0 = all). Each line is one document.
// Tokenization: lowercased maximal [a-z0-9] runs (every other byte is a
// separator), mirroring a simple WhitespaceAnalyzer over ASCII -- tokens never
// contain spaces, so they remain space-joinable for the CLucene field value.
// Tokens longer than 64 bytes are truncated (defends against pathological log
// blobs). Empty documents (no tokens) are kept so docids match line numbers.
// Throws std::runtime_error if the file cannot be opened, if a read error is
// observed mid-stream (std::ferror after a short read -- never silently
// truncated), or if the doc/vocab count would overflow the uint32 id space.
// The open descriptor is RAII-closed, so it never leaks even on a throw.
Corpus load_from_file(const std::string& path, uint32_t max_docs);

}  // namespace bench
