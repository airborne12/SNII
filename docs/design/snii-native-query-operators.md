# SNII native query operators

Status: SNII core implemented, Doris adapter next
Scope: `core/include/snii/query/*`, `core/src/query/*`, `core/tests/query/*`

## Goal

SNII query execution is a native read model over SNII dictionary entries,
windowed postings, and `BatchRangeFetcher`. It is not a compatibility layer for
Doris/CLucene `TermQuery`.

The Doris adapter should translate SQL/analyzer output into these SNII-native
operators, then translate the result into Doris storage-layer containers. It
should not implement core query semantics by iterating SNII postings through a
CLucene-shaped query tree.

## Existing operators

- `term_query`: exact term, sorted docid set.
- `boolean_and`: all terms, sorted docid intersection with high-df covering-window
  reads.
- `phrase_query`: positional phrase query with docid-first planning and lazy
  position decoding.
- `scoring_query_*`: BM25 exhaustive/WAND/selective WAND.
- `boolean_or`: any term, sorted deduplicated union. Added as the first TDD slice.
- `prefix_query`: any term matching a dictionary prefix, sorted deduplicated union.
- `wildcard_query`: wildcard term expansion (`*`, `?`) followed by sorted
  deduplicated docid union.
- `regexp_query`: full-term regular-expression expansion followed by sorted
  deduplicated docid union.
- `phrase_prefix_query`: phrase query whose last term is a prefix; exact terms
  are resolved and read once, then tail prefix hits are checked against the
  expected tail positions without second lookups.

## Interface boundary

Public query APIs stay small and behavior-oriented:

```cpp
class DocIdSink {
 public:
  virtual ~DocIdSink() = default;
  virtual Status append_sorted(std::span<const uint32_t> docids) = 0;
};

struct QueryProfile {
  uint64_t elapsed_ns;
  bool has_io_metrics;
  IoMetrics io_before;
  IoMetrics io_after;
  IoMetrics io_delta;
};

Status term_query(const LogicalIndexReader&, std::string_view term,
                  std::vector<uint32_t>* docids);
Status term_query(const LogicalIndexReader&, std::string_view term,
                  std::vector<uint32_t>* docids, QueryProfile* profile);

Status boolean_or(const LogicalIndexReader&, const std::vector<std::string>& terms,
                  std::vector<uint32_t>* docids);
Status boolean_or(const LogicalIndexReader&, const std::vector<std::string>& terms,
                  std::vector<uint32_t>* docids, QueryProfile* profile);
Status boolean_or(const LogicalIndexReader&, const std::vector<std::string>& terms,
                  DocIdSink* sink);

Status boolean_and(const LogicalIndexReader&, const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids);
Status boolean_and(const LogicalIndexReader&, const std::vector<std::string>& terms,
                   std::vector<uint32_t>* docids, QueryProfile* profile);

Status prefix_query(const LogicalIndexReader&, std::string_view prefix,
                    std::vector<uint32_t>* docids);
Status prefix_query(const LogicalIndexReader&, std::string_view prefix,
                    std::vector<uint32_t>* docids, QueryProfile* profile);
Status prefix_query(const LogicalIndexReader&, std::string_view prefix,
                    DocIdSink* sink);

Status wildcard_query(const LogicalIndexReader&, std::string_view pattern,
                      std::vector<uint32_t>* docids);
Status wildcard_query(const LogicalIndexReader&, std::string_view pattern,
                      std::vector<uint32_t>* docids, QueryProfile* profile);
Status wildcard_query(const LogicalIndexReader&, std::string_view pattern,
                      DocIdSink* sink);

Status regexp_query(const LogicalIndexReader&, std::string_view pattern,
                    std::vector<uint32_t>* docids);
Status regexp_query(const LogicalIndexReader&, std::string_view pattern,
                    std::vector<uint32_t>* docids, QueryProfile* profile);
Status regexp_query(const LogicalIndexReader&, std::string_view pattern,
                    DocIdSink* sink);

Status phrase_query(const LogicalIndexReader&, const std::vector<std::string>& terms,
                    std::vector<uint32_t>* docids);
Status phrase_query(const LogicalIndexReader&, const std::vector<std::string>& terms,
                    std::vector<uint32_t>* docids, QueryProfile* profile);
Status phrase_prefix_query(const LogicalIndexReader&,
                           const std::vector<std::string>& terms,
                           std::vector<uint32_t>* docids);
Status phrase_prefix_query(const LogicalIndexReader&,
                           const std::vector<std::string>& terms,
                           std::vector<uint32_t>* docids,
                           QueryProfile* profile);
```

Implementation details stay behind internal modules:

- `snii/query/internal/docid_posting_reader`: decode one resolved `DictEntry` to
  docids across inline, slim pod_ref, and windowed pod_ref encodings.
- `snii/query/internal/docid_union`: execute sorted docid union over already
  resolved postings; boolean OR and prefix query both reuse it.
- `snii/query/internal/term_expansion`: enumerate dictionary terms by a safe
  prefix, filter in memory, and pass matching `PrefixHit` entries to docid union
  without a second lookup per term.
- phrase-prefix resolved planner path: exact terms are planned once into expected
  tail positions, and expanded tail terms are converted to phrase `TermPlan`s
  without re-entering dictionary lookup.
- `snii/query/docid_sink`: bulk handoff of sorted docid spans to adapters; vector
  APIs are wrappers over this boundary where implemented.
- `snii/query/query_profile`: optional elapsed-time and `FileReader` I/O metric
  snapshots; `QueryProfileScope` finalizes through RAII so early returns still
  report elapsed time and I/O deltas. Query operators do not manually count bytes
  or ranges.
- phrase/conjunction planning: choose low-df drivers and covering windows.
- future chunk streaming: push chunk production deeper into posting decoders so
  high-hit terms avoid unnecessary final materialization.

## Golden metrics

Operator design must preserve the SNII metric model:

- `serial_rounds` are query stages, not term count and not posting-window count.
- docid-only operators must skip freq and prx bytes.
- absent exact terms should be rejected by resident metadata or one dictionary
  probe, with no posting read.
- high-hit terms must support bulk result handoff to the Doris adapter; per-doc
  callback loops are not an acceptable final path.

## TDD task queue

1. Done: `boolean_or` behavior.
   RED: public test builds a real docs-only index and checks mixed high-df,
   low-df, absent, and duplicate terms return a sorted deduplicated union.
   GREEN: `boolean_or` plus shared `docid_posting_reader`; `term_query` reuses the
   same decoder.

2. Done: `boolean_or` I/O metrics.
   RED: through `MeteredFileReader`, prove single-term `boolean_or` has the same
   docid-only request profile as `term_query`, and multi-term first reads are
   bounded by planned stages rather than accidental per-doc iteration.
   GREEN: `read_docid_postings_batched` batches windowed preludes in one round and
   all docid windows in one round; `boolean_or` resolves terms first, then decodes
   postings through that batch helper.

3. Done: bulk output API for high-hit terms.
   RED: public API can stream sorted docid chunks to a sink; a high-df term emits
   chunks without a per-doc callback loop.
   GREEN: add `DocIdSink`/chunk API and keep the vector-returning APIs as wrappers.

4. Done: prefix query.
   RED: `prefix_query(prefix)` equals the union of `prefix_terms(prefix)` followed
   by exact term queries, including empty result and sorted output; metered reads
   reject an implementation that performs one extra dictionary lookup per hit.
   GREEN: reuse the OR planner over enumerated `PrefixHit` entries without extra
   lookups, via `internal::emit_docid_union`.

5. Done: wildcard and regexp docid queries.
   RED: expansion semantics match an oracle over dictionary terms; wide wildcard
   metered reads reject per-expanded-term lookup.
   GREEN: split expansion from execution; expansion produces resolved entries
   through `term_expansion`, then reuses `emit_docid_union`.

6. Done: phrase-prefix.
   RED: expansion semantics match an oracle over terms and positions.
   GREEN: split expansion from phrase execution; expansion produces resolved
   entries and the phrase planner consumes them without per-expanded-term lookup.

7. Done: shared exact-term phrase-prefix planning.
   RED: a wide tail-prefix phrase reuses exact-term posting reads across tail
   expansions, not only exact-term dictionary lookups.
   GREEN: collect expected tail positions from the exact phrase once, then verify
   each expanded tail term against those positions without re-reading exact
   postings.

8. Done: profile counters.
   RED: every native operator reports nonzero query time and stable I/O metrics
   under metered reads.
   GREEN: add optional `QueryProfile*` overloads for native vector-returning
   operators; elapsed time is measured at the query boundary, and I/O deltas are
   derived from `FileReader` metric snapshots. The profile scope is RAII-finalized
   so success and early-return paths share the same accounting boundary.

9. Next: Doris adapter handoff.
   RED: Doris SNII MATCH_ANY/MATCH_ALL/MATCH_PHRASE call native SNII operators and
   produce the same rows as V3 on parity fixtures.
   GREEN: adapter maps analyzer terms to SNII operators and bulk-loads results into
   Doris containers.
