#include "snii/query/internal/docid_union.h"

#include <vector>

#include "snii/query/internal/docid_set_ops.h"

namespace snii::query::internal {

Status emit_docid_union(const snii::reader::LogicalIndexReader& idx,
                        const std::vector<ResolvedDocidPosting>& postings,
                        DocIdSink* sink) {
  if (sink == nullptr) return Status::InvalidArgument("docid_union: null sink");
  if (postings.empty()) return Status::OK();

  std::vector<std::vector<uint32_t>> docs_by_posting;
  SNII_RETURN_IF_ERROR(read_docid_postings_batched(idx, postings, &docs_by_posting));

  std::vector<uint32_t> acc = union_sorted_many(docs_by_posting);
  if (acc.empty()) return Status::OK();
  return sink->append_sorted(acc);
}

}  // namespace snii::query::internal
