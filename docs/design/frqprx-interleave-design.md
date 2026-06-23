# Design: Interleave `.frq` POD and `.prx` POD into one per-index posting region

Status: approved (pre-launch format change) — review-resolved, implementation-ready
Owner: SNII writer/reader
Scope: `LogicalIndexWriter`, `SniiCompoundWriter`, `LogicalIndexReader`, `SectionRefs`,
`PerIndexMeta` header flags, container layout

---

## 0. Resolved review (changes folded in)

Four adversarial reviews of the prior draft. All blockers and majors are resolved
below; the relevant sections were rewritten, not merely annotated.

| # | Severity | Finding | Resolution |
|---|----------|---------|------------|
| R1 | **blocker** | `snii_segment_reader.cpp:95-96` infers `has_positions`/tier from `prx_pod.length > 0`. The merge makes `posting_region.length > 0` true for **every** pod_ref index — including a docs-only T1 index (a high-df / windowed term still appends its frq span). A naive repoint to `posting_region.length > 0` mis-classifies docs-only as T2/positional; `DictBlockReader::open`→`check_flags` (`dict_block.cpp:121-124`) then hard-fails with `InvalidArgument`, and `prx_base` is parsed from a header the writer never wrote → **index unreadable**. | **Accepted.** Positions-capability is now **persisted explicitly**: new meta header flag `PerIndexMetaBuilder::kHasPositions = 1u << 0` (bit 0 was free; only `kHasBsbf = 1u << 1` is used today), set from `has_prx_` at `finish_meta` time. `open_index` reads `has_positions` from the flag (`|| has_norms` kept as belt-and-suspenders), **never** from any region length. The `prx_pod.length`-based heuristic is **deleted**, not repointed. See §3.1, §7.4, §8.1. |
| R2 | **major** | §7.3 claimed "most callers use `resolve_*_window` and need no change." False: the hot windowed/phrase/scoring read paths read `frq_pod`/`prx_pod` `.offset` **directly** at 5 sites. Also `stats/snii_stats_provider.cpp` was listed but has **zero** `frq_pod`/`prx_pod` reads (over-count), and `term_query.cpp` only `#include`s the header. | **Accepted.** §7.3 now enumerates the **exact** direct-offset sites (verified by grep) and splits the audit into (a) additive-offset repoints and (b) the one semantic predicate. `stats_provider`/`term_query` removed from the repoint list. |
| R3 | **minor** | `kMetaFormatVersion` is one shared constant consumed by both `per_index_meta.cpp` (header) **and** `tail_meta_region.cpp:50,98` (region). §9 understated the blast radius. | **Accepted.** §9 now treats `kMetaFormatVersion` as the single shared sentinel for **both** layers, staying at **1** (from-scratch, pre-launch — no v1 index to coexist with, so no bump); a reader still fail-fasts with `Corruption` on a mismatch. `kFormatVersion`/`min_reader_version` are untouched (no container-framing change). Do **not** add a second version field. |
| R4 | **minor** | `frq_off_delta == prx_off_delta + prx_len` is a **writer-side consequence** of "nothing is appended to `post_file_` between a term's prx span and frq span", not a reader invariant. For the docs-only tier (`!has_prx`) `prx_off_delta`/`prx_len` are unset, so the equality is undefined there; tests asserting it unconditionally would spuriously fail. | **Accepted.** §4 reframes the equality as a writer-side property; the reader resolves each delta independently. Test §10.2.1/§10.2.7 gate the equality on `has_prx`. |
| R5 | **minor** | Test plan listed files but omitted concrete `refs.frq_pod`/`.prx_pod` member uses that won't compile after the struct change (`snii_compound_writer_test.cpp:210-213,271,466`; `per_index_meta_test.cpp:58-59,73-74`; `phase_a_readback_test.cpp:158`), and a tier-recovery round-trip for docs-only. | **Accepted.** §10.1 adds a per-file checklist of member references to rewrite (with the offset-order flip); §10.2 adds test #9 (docs-only-with-pod_ref tier recovery). |

**Where a reviewer was imprecise:** one review framed the segment_reader fix as
"contradicts the design's no-new-field stance." It does not contradict
*correctness* — but the no-new-field claim in the old §3 was simply wrong for the
*meta header flags* (distinct from `SectionRefs` and `DictEntry`). We add exactly
one header flag **bit** (no new struct field, no new varint), which is free given
the existing `flags` u32. The "no new `SectionRefs`/`DictEntry` field" claim still
holds and is restated precisely in §3.

---

## 1. Motivation

Today, building one logical index keeps **three** on-disk scratch temp files
(`TempSectionFile`): `dict_file_`, `frq_file_`, `prx_file_` (plus the SPIMI spill
temp on the input side). The `.frq` POD and `.prx` POD are two **separate** big
section files that are streamed into the container back-to-back at `finish()`:

```
write_index_sections() per index:  [DICT region][.frq POD][.prx POD]
```

The two posting PODs are the dominant on-disk scratch. They exist only because a
term's `.prx` bytes (built in pass 1 of the windowed builder, streamed straight to
disk) and its `.frq` bytes (built in pass 2) are written to **different** sinks.
Nothing in the format actually requires them to be physically separate: the reader
resolves each window independently via `section.offset + base + off_delta`, and a
term's `.prx` and `.frq` spans are never read with a single contiguous fetch.

**Goal.** Merge the per-index `.frq` POD and `.prx` POD into **one interleaved
"posting region"**. Each term writes its bytes as `[prx...][frq...]` contiguously
into a **single sink** (`post_file_`), in term order. This:

- Eliminates one of the two big section temp files (two sinks → one).
- Makes the posting region a single contiguous byte range whose sink can, on the
  OSS/local-then-upload path, eventually *be the output stream directly* (a single
  forward append with no separate `.prx` temp to splice back in).
- Leaves the DICT region (smaller, produced interleaved with term processing, must
  stay contiguous and is compressed/checksummed block-by-block) on its own buffer.
- Leaves the SPIMI spill temp as the **only** other on-disk scratch.

This is a **breaking on-disk format change**. The project is pre-launch
(no `lifecycle: launched` module), so changing the layout without backward-compat is
acceptable: no index with the old layout exists. The single meta format version stays
**1** (the change folds into it); a reader rejects any version mismatch as a fail-fast
sentinel — there is no old-format dual-read to support.

### Non-goals

- No change to per-window encoding (`build_dd_region`, `build_freq_region`,
  `build_prx_window_flat`, `build_frq_prelude`) — the *bytes* of every window,
  prelude, dd-block and freq-block are byte-identical to today.
- No change to the two-pass peak-RSS strategy in `BuildWindowedPosting` (prx in
  pass 1, dd/freq in pass 2). Only the **destination sink** of the prx pass and
  the **measurement of offsets** change.
- No change to the slim INLINE branch: tiny postings stay embedded in the
  `DictEntry` and append **nothing** to any posting region.
- No change to norms / bsbf / null_bitmap placement or to the tail/meta region
  *framing* (the meta header gains one flag bit; see §3.1).

---

## 2. Current vs. new on-disk layout

### 2.1 Current per-index layout (three sinks)

```
[DICT region]            (dict_file_  -> SectionRefs.dict_region)
[.frq POD]               (frq_file_   -> SectionRefs.frq_pod)
[.prx POD]               (prx_file_   -> SectionRefs.prx_pod, absent if !has_prx)
```

Per-term within the `.frq` POD (windowed): `[prelude][dd-block][freq-block]`.
Per-term within the `.prx` POD (windowed): `[win0_prx][win1_prx]...`.
The `.frq` and `.prx` bytes of a term live in **different** files.

### 2.2 New per-index layout (one posting sink + dict sink)

```
[posting region]         (post_file_  -> SectionRefs.posting_region)
[DICT region]            (dict_file_  -> SectionRefs.dict_region)
```

> Order within the index changes to **posting region first, then DICT region**.
> Rationale: the posting region is the large, append-only, term-ordered stream that
> we want to be able to alias to the output stream directly; the DICT region is the
> smaller, compressed, contiguous trailer. Putting postings first lets the writer
> stream the (potentially huge) posting bytes straight through, then append the
> compact DICT trailer. (The order is otherwise arbitrary for correctness — all
> offsets are absolute and resolved at `finish()` — but "big stream first, small
> contiguous trailer last" is the layout that the future direct-to-output-stream
> optimization wants.)

Within the **posting region**, bytes are laid out **in term order**, and for each
**pod_ref** term (windowed or slim-pod_ref) the term writes its **prx span first,
then its frq span**, contiguously:

```
posting region = concat over pod_ref terms, in term order, of:
    [ term.prx span ][ term.frq span ]
```

where:

- `term.prx span` (only when `has_prx`): the term's concatenated prx windows,
  exactly the bytes that used to go to `prx_file_`.
  - windowed: `[win0_prx][win1_prx]...[winN-1_prx]`
  - slim pod_ref: `[prx_window]`
  - when `!has_prx` (T1/keyword, docs-only tier): **empty** — the term's posting
    bytes are just its frq span (identical to a today's no-prx index).
- `term.frq span`: exactly the bytes that used to go to `frq_file_`.
  - windowed: `[prelude][dd-block][freq-block]`
  - slim pod_ref: `[dd_region][freq_region]`

INLINE slim terms append **nothing** to the posting region (their `[dd][freq]` and
optional `[prx]` bytes live in the `DictEntry` inside the DICT block, unchanged).

### 2.3 Full container layout (new)

```
[bootstrap_header]
for each logical index, in add order:
    [posting region]      (interleaved prx+frq, in term order)
    [DICT region]         (concatenated DICT blocks)
for each logical index, in add order:
    [norms POD]           (scoring only; else absent)
    [bsbf section]        (one physical section per index; else absent)
[tail_meta_region]        (one per_index_meta block per index + directory)
[tail_pointer]
```

(`norms`, `bsbf`, `null_bitmap`, tail region, tail pointer: **unchanged**.)

### 2.4 Single-pass / append-only property (preserved)

The build remains a single forward pass. Within a term's encode:

1. (pod_ref only) the **prx span** is appended to `post_file_` first — for windowed
   this happens window-by-window during `BuildWindowedPosting` pass 1; for slim it
   is one `append(prx_win)`.
2. then the **frq span** is appended to `post_file_` — for windowed,
   `[prelude][dd-block]`then`[freq-block]`; for slim, `[dd_region][freq_region]`.

`post_file_` is append-only with a monotonic `size()` cursor (`TempSectionFile`),
so offsets are measured by reading `post_file_.size()` before/after appends — no
seek-back. The container `finish()` stays append-only: stream the whole posting
region, then the whole DICT region, per index.

---

## 3. `SectionRefs` change (one region; no new SectionRefs/DictEntry field)

Replace the two posting refs with **one**:

```cpp
struct SectionRefs {
  RegionRef dict_region;
  RegionRef posting_region;   // was: frq_pod; prx_pod removed
  RegionRef norms;
  RegionRef null_bitmap;
  RegionRef bsbf;
};
```

- **Encoding** (`per_index_meta.cpp` `encode_section_refs` / `decode_section_refs`):
  drop the second `encode_region`/`decode_region` call. The field order becomes
  `dict_region, posting_region, norms, null_bitmap, bsbf` (one varint64 pair fewer).
  This is the format-breaking byte change in the meta block; it (together with the
  header flag in §3.1) defines the single from-scratch meta layout (§9; version stays 1).

> **Decision: one ref, not two aliases.** We considered keeping `frq_pod` and
> `prx_pod` as two refs both pointing at the same `posting_region` (minimal reader
> diff). Rejected: it is misleading (the two would be identical), wastes meta bytes,
> and the reader math is cleaner with a single region (the prx and frq deltas are
> already measured against a single per-block base; see §4). We collapse to one
> `posting_region`.

> **`(off, 0)` vs `(0, 0)` convention is unchanged.** `posting_region = (off, 0)`
> means "present but empty" (e.g. an index whose terms are all INLINE); `(0, 0)`
> remains the "absent" convention used for `norms`/`bsbf`. Critically, **no
> capability is inferred from `posting_region.length` in either direction** (see
> §3.1 / §8.1 / §8.3).

### 3.1 `PerIndexMeta` header — new `kHasPositions` flag (the R1 fix)

Positions-capability (tier T1 vs T2/T3) is currently **inferred at open time** from
`prx_pod.length > 0`. That signal is destroyed by the merge: `posting_region.length`
is non-zero for **any** pod_ref index, docs-only included (a high-df / windowed
docs-only term still appends a frq span). Inferring `has_positions` from a region
length is therefore **invalid after this change** and is removed entirely.

Instead, persist positions-capability explicitly in the per-index meta **header
flags** (`PerIndexMetaHeader.flags`, a `u32` that today uses only `kHasBsbf = 1u <<
1`; **bit 0 is free**):

```cpp
// per_index_meta.h  (PerIndexMetaBuilder)
static constexpr uint32_t kHasPositions = 1u << 0;  // NEW: index is positions-capable
static constexpr uint32_t kHasBsbf      = 1u << 1;  // unchanged
```

- **Writer** (`logical_index_writer.cpp` `finish_meta`, today ~line 622): OR in the
  bit from `has_prx_`:

  ```cpp
  uint32_t flags = bsbf_bytes_.empty() ? 0u : PerIndexMetaBuilder::kHasBsbf;
  if (has_prx_) flags |= PerIndexMetaBuilder::kHasPositions;   // NEW
  PerIndexMetaBuilder builder(index_id_, index_suffix_, flags);
  ```

- **Reader** (`PerIndexMetaReader`): expose a convenience accessor mirroring
  `has_bsbf()`:

  ```cpp
  bool has_positions() const { return (flags_ & PerIndexMetaBuilder::kHasPositions) != 0; }
  ```

This is a **header flag bit only** — no new `SectionRefs` field, no new `DictEntry`
field, no new varint, no struct growth (the `u32 flags` already exists on the wire).
It is, however, a **wire-meaningful** change (a previously-zero bit may now be set),
folded into the single from-scratch meta layout (§9; version stays 1).

### 3.2 Reader math is unchanged "in spirit"

Today the reader computes:

```
abs_frq = section_refs.frq_pod.offset + frq_base + frq_off_delta   (+ prelude_len)
abs_prx = section_refs.prx_pod.offset + prx_base + prx_off_delta
```

New: both resolve against the **same** region:

```
abs_frq = section_refs.posting_region.offset + frq_base + frq_off_delta (+ prelude_len)
abs_prx = section_refs.posting_region.offset + prx_base + prx_off_delta
```

The structure `section.offset + base + delta` is identical. Only the section the
deltas index into is now shared. Crucially, **`frq_base`/`prx_base` and the two
`off_delta`s are now offsets *within the single posting region*** (relative to the
same block-open snapshot of `post_file_.size()`), so they already encode the
interleaving — no new offset fields are needed.

---

## 4. `DictEntry` — offset fields (no struct change)

`DictEntry` keeps **exactly** the existing fields:

```
frq_off_delta, frq_len, prelude_len (windowed), frq_docs_len (slim pod_ref),
prx_off_delta, prx_len (tier>=T2)
```

**No new field, no removed field, no wire-encoding change in `dict_entry`.** What
changes is only their **meaning**: both `frq_off_delta` and `prx_off_delta` now
index into the **posting region** instead of into two separate PODs. Because we
write `[prx][frq]` per term and measure each delta against the posting sink's live
size, the existing varints already capture the new contiguous layout. Specifically,
for a windowed term whose prx span precedes its frq span in the region:

- `prx_off_delta` points at the start of the prx span,
- `frq_off_delta` points at the start of the frq span.

### Writer-side property: `frq_off_delta == prx_off_delta + prx_len` (R4)

When `has_prx`, the frq span immediately follows the prx span of the **same** term,
so `frq_off_delta == prx_off_delta + prx_len` holds. **This is a consequence of the
write order — specifically that nothing is appended to `post_file_` between a term's
prx span and its frq span — not a reader invariant.** It is correct today
(`BuildWindowedPosting` appends prx window-by-window in pass 1 and the dd/freq
blocks are built in RAM in pass 2 and appended right after; slim appends `prx_win`
then `frq_win` with nothing between). If a future change ever appended anything to
`post_file_` between the two spans, the equality would break **while the deltas
stayed correct** (each is measured from live `post_file_.size()`).

Therefore:

- The **reader** MUST continue to resolve each delta **independently** from its own
  `off_delta` (it does — §7.1). It never relies on the equality.
- The equality is a **writer-side test assertion only** (§10.2.1/§10.2.7), and it is
  **gated on `has_prx`**: for the docs-only tier (`!has_prx`) `prx_off_delta`/
  `prx_len` are unset, so the equality is undefined there and MUST NOT be asserted.

`frq_docs_len`, `prelude_len`, `dd_meta`/`freq_meta` codecs, the docs-only-prefix
contract (`freq_off_delta = frq_docs_len`), and the inline branch all keep their
current semantics verbatim. Note the docs-only prefix `[frq_off_delta,
frq_off_delta + frq_docs_len)` stays **inside** the frq span (prx is written first),
so no single contiguous fetch ever straddles the prx/frq boundary.

### Per-window prx metadata (`WindowMeta.prx_off`) — unchanged

In `BuildWindowedPosting` pass 1, `m.prx_off = out->prx_total_len` (offset *within
the entry's prx span*, 0 for the first window) and `m.prx_len = sc.prx_sink.size()`.
This stays identical: the per-window `prx_off` is relative to the entry's prx span,
and the entry's prx span start within the region is `prx_off_delta`. The reader
adds them exactly as today; the adjacent frq span does not perturb the per-window
offset (it is bounded by `entry.prx_len`).

---

## 5. Writer changes (`logical_index_writer.{h,cpp}`)

### 5.1 Sinks: three temps → two (one posting, one dict)

```cpp
// logical_index_writer.h  (private members)
TempSectionFile dict_file_;     // unchanged: DICT region trailer
TempSectionFile post_file_;     // NEW: interleaved [prx][frq] posting region
// REMOVE: TempSectionFile frq_file_;
// REMOVE: TempSectionFile prx_file_;
std::vector<uint8_t> norms_section_;  // unchanged
```

Accessors:

```cpp
uint64_t posting_region_size() const { return post_file_.size(); }  // replaces frq/prx sizes
Status stream_posting_region_into(io::FileWriter* out) const { return post_file_.stream_into(out); }
// REMOVE frq_pod_size(), prx_pod_size(), stream_frq_pod_into(), stream_prx_pod_into()
```

`build()` opens `dict` and `post` temps (drop the separate `frq`/`prx` opens):

```cpp
SNII_RETURN_IF_ERROR(dict_file_.open("dict"));
SNII_RETURN_IF_ERROR(post_file_.open("post"));
// (post_file_ is ALWAYS opened, even when !has_prx — it then holds frq spans only)
...
SNII_RETURN_IF_ERROR(dict_file_.seal());
SNII_RETURN_IF_ERROR(post_file_.seal());
```

> Note the prior code opened `prx_file_` only `if (has_prx_)`
> (`logical_index_writer.cpp:559`). `post_file_` is now **always** opened. The
> `has_prx_`-conditional `prx_file_.open`/`.seal` lines are deleted.

### 5.2 Per-block base capture (`process_term`)

Today: `st->frq_base = frq_file_.size(); st->prx_base = prx_file_.size();`
New: both bases come from the **same** posting sink, captured at block open:

```cpp
if (!st->block) {
  const uint64_t base = post_file_.size();   // single snapshot of the posting sink
  st->frq_base = base;
  st->prx_base = base;
  st->block = std::make_unique<DictBlockBuilder>(tier_, has_prx_, st->frq_base, st->prx_base);
  st->block_first_term = tp.term;
}
```

`DictBlockBuilder` keeps both `frq_base` and `prx_base` (block header format
unchanged); they simply hold the same value now. (We keep two fields rather than
collapsing the block header so the dict_block wire format does not change in this
step — see §9 "minimal-touch" note. The block header still writes `prx_base` only
when `has_positions`, which is now driven by the persisted flag, not a region
length.)

### 5.3 `build_windowed_entry` — write `[prx][frq]` into `post_file_`

The two-pass `BuildWindowedPosting` is unchanged except its prx pass streams to
`post_file_` instead of `prx_file_`. Key point: **the prx span must be written
before** we measure `frq_off`, and the frq_off snapshot must be taken **after** the
prx span has been appended.

```cpp
Status LogicalIndexWriter::build_windowed_entry(TermPostings& tp, uint64_t frq_base,
                                                uint64_t prx_base, DictEntry* e) {
  // prx span starts here (pass 1 streams windows straight into post_file_)
  const uint64_t prx_off = post_file_.size();
  WindowedPosting wp;
  SNII_RETURN_IF_ERROR(
      BuildWindowedPosting(tp, has_freq_, has_prx_, encoded_norms_, &post_file_, &wp));
  // wp.prx_total_len bytes were just appended to post_file_ (0 when !has_prx).
  std::vector<uint32_t>().swap(tp.docids);
  std::vector<uint32_t>().swap(tp.freqs);

  std::vector<uint8_t> prelude;
  SNII_RETURN_IF_ERROR(BuildPrelude(wp.windows, has_freq_, has_prx_, &prelude));

  e->kind = DictEntryKind::kPodRef;
  e->enc  = DictEntryEnc::kWindowed;
  e->has_sb = true;
  e->prelude_len  = prelude.size();
  e->frq_docs_len = e->prelude_len + wp.dd_block.size();   // [prelude][dd-block]

  // frq span starts immediately AFTER the prx span, in the SAME sink.
  // frq_off == prx_off + wp.prx_total_len BECAUSE nothing is appended to post_file_
  // between the prx pass and here (see §4). Still measured from live size, not assumed.
  const uint64_t frq_off = post_file_.size();
  SNII_RETURN_IF_ERROR(post_file_.append(prelude));
  SNII_RETURN_IF_ERROR(post_file_.append(wp.dd_block));
  SNII_RETURN_IF_ERROR(post_file_.append(wp.freq_block));
  e->frq_off_delta = frq_off - frq_base;
  e->frq_len       = post_file_.size() - frq_off;
  if (has_prx_) {
    e->prx_off_delta = prx_off - prx_base;     // prx span start
    e->prx_len       = wp.prx_total_len;
  }
  return Status::OK();
}
```

`BuildWindowedPosting`'s signature changes `TempSectionFile* prx_file` →
`TempSectionFile* post_file` (same type; only the pointed-at sink differs). Inside,
`prx_file->append(...)` becomes `post_file->append(...)`. Everything else
(`m.prx_off = out->prx_total_len`, freeing `positions_flat` after pass 1, pass 2
dd/freq blocks held in RAM then appended) is **byte-for-byte unchanged**.

### 5.4 `build_slim_entry` — `[prx][frq]` for pod_ref; INLINE untouched

```cpp
Status LogicalIndexWriter::build_slim_entry(TermPostings& tp, uint64_t frq_base,
                                            uint64_t prx_base, DictEntry* e) {
  // (encode dd/freq/prx exactly as today)
  std::vector<uint8_t> frq_win = dd_bytes; AppendBytes(&frq_win, freq_bytes);
  std::vector<uint8_t> prx_win;
  if (has_prx_) SNII_RETURN_IF_ERROR(MakePrxWindow(tp.positions_flat, tp.freqs, &prx_win));

  e->enc = DictEntryEnc::kSlim;
  e->dd_meta = dd_meta; e->freq_meta = freq_meta;

  if (frq_win.size() <= kDefaultInlineThreshold) {       // INLINE: append NOTHING
    e->kind = DictEntryKind::kInline;
    e->inline_dd_disk_len = dd_meta.disk_len;
    e->frq_bytes = std::move(frq_win);
    if (has_prx_) e->prx_bytes = std::move(prx_win);
    return Status::OK();
  }

  // POD_REF: write [prx][frq] into the single posting sink, prx first.
  e->kind = DictEntryKind::kPodRef;
  e->frq_docs_len = dd_meta.disk_len;                    // docs-only prefix = dd region
  uint64_t prx_off = 0;
  if (has_prx_) {
    prx_off = post_file_.size();
    SNII_RETURN_IF_ERROR(post_file_.append(prx_win));
    e->prx_off_delta = prx_off - prx_base;
    e->prx_len       = post_file_.size() - prx_off;
  }
  const uint64_t frq_off = post_file_.size();            // after the prx span
  SNII_RETURN_IF_ERROR(post_file_.append(frq_win));
  e->frq_off_delta = frq_off - frq_base;
  e->frq_len       = post_file_.size() - frq_off;
  return Status::OK();
}
```

> **Order note (slim):** today slim writes frq **then** prx. We now write prx
> **then** frq to honor the `[prx][frq]` per-term contract. This makes slim and
> windowed consistent (prx span always precedes frq span). The reader does not
> depend on relative order — each delta is resolved independently — so this is safe;
> it only changes the physical byte order inside the region (which is a new-format
> property anyway). For `!has_prx` slim pod_ref, the prx block is skipped entirely
> and the entry has only a frq span (no `prx_off_delta`/`prx_len`).

### 5.5 DICT region: keep one buffer — **RAM-vs-temp decision: keep one temp file**

The DICT region must stay **contiguous** (it is a sequence of independently
zstd-compressed, crc'd blocks, and the directory records each block's offset within
the region). It is produced **interleaved** with term processing (a block is cut and
flushed when it reaches `target_dict_block_bytes`), so it cannot be the output
stream directly (postings for later terms are still streaming).

**Decision: keep the DICT region in its existing single temp file (`dict_file_`),
not RAM.** Justification (peak-RSS bound):

- The DICT region is the *second-largest* section (term keys + entry metadata +
  **inline** postings for all sub-threshold terms). For a large vocabulary it is
  tens-to-hundreds of MiB. Holding it fully in RAM would re-introduce exactly the
  peak-RSS source that `TempSectionFile` was created to remove.
- Per-block buffering is already bounded: only one open `DictBlockBuilder`
  (`target_dict_block_bytes`, default 64 KiB) plus its zstd scratch is in RAM at a
  time; each finished block is compressed and **streamed to `dict_file_`** then
  dropped. So the dict temp adds O(block size) RAM, not O(region size).
- Net peak-RSS after this change is **strictly lower** than today: we still have a
  bounded dict buffer (one temp) and a bounded posting buffer (one temp, plus the
  windowed builder's transient dd/freq blocks for the single widest term), but we
  removed one whole section temp's file handle / fd and the second posting sink.

So: **two temp files total per index** (`dict_file_`, `post_file_`), down from
three. The norms section stays in RAM (1 byte/doc). The SPIMI spill temp on the
input side is unchanged and remains the only other on-disk scratch.

---

## 6. Compound writer / `finish()` (`snii_compound_writer.{cpp,h}`)

### 6.1 `Placement` — collapse frq/prx into posting

```cpp
struct Placement {
  uint64_t dict_off = 0, dict_len = 0;
  uint64_t post_off = 0, post_len = 0;   // was frq_off/frq_len + prx_off/prx_len
  uint64_t norms_off = 0, norms_len = 0;
  uint64_t bsbf_off = 0, bsbf_len = 0;
};
```

### 6.2 `write_index_sections` — posting region first, then DICT

```cpp
for (each index i) {
  Placement& p = (*placements)[i];
  const LogicalIndexWriter& w = *indexes_[i];

  p.post_off = cursor_;
  SNII_RETURN_IF_ERROR(w.stream_posting_region_into(out_));
  cursor_ += w.posting_region_size();
  p.post_len = cursor_ - p.post_off;

  p.dict_off = cursor_;
  SNII_RETURN_IF_ERROR(w.stream_dict_region_into(out_));
  cursor_ += w.dict_region_size();
  p.dict_len = cursor_ - p.dict_off;
}
```

> This replaces the current `cursor_ += w.frq_pod_size()` / `if (has_prx) cursor_ +=
> w.prx_pod_size()` sequence (`snii_compound_writer.cpp:62,68`). The **order flip**
> (posting region streamed *before* the DICT region, per §2.2) changes the expected
> relative positions of dict vs posting in `snii_compound_writer_test.cpp`.
> `finish_meta` still receives `p.dict_off` to rebase the DICT block directory's
> absolute offsets (`BlockRef.offset = dict_region_offset + rel_offset`); that math
> is unchanged, it just uses a `dict_off` that now comes after the posting region.
> The posting-region-relative `frq_base`/`prx_base`/deltas are independent of where
> the posting region lands, so the order flip is transparent to entry resolution.

### 6.3 `write_tail` — single posting ref

```cpp
SectionRefs refs;
refs.dict_region    = {p.dict_off, p.dict_len};
refs.posting_region = {p.post_off, p.post_len};   // was frq_pod + prx_pod
refs.norms          = {p.norms_off, p.norms_len};
refs.null_bitmap    = {0, 0};
refs.bsbf           = {p.bsbf_off, p.bsbf_len};
```

This replaces `refs.frq_pod = {p.frq_off, p.frq_len}; refs.prx_pod = {p.prx_off,
p.prx_len};` (`snii_compound_writer.cpp:106-107`). `write_norms` (norms + bsbf
placement), `write_bootstrap`, tail region / tail pointer, crc: **unchanged**. The
byte-identical-streaming guarantee (`TempSectionFile::stream_into`, 1 MiB chunks) is
preserved — we just stream one posting temp instead of two.

---

## 7. Reader changes

### 7.1 Resolve both windows from `posting_region`
(`logical_index_reader.cpp:249,257`)

```cpp
Status LogicalIndexReader::resolve_frq_window(const DictEntry& entry, uint64_t frq_base,
                                              uint64_t* abs_off, uint64_t* len) const {
  return resolve_window(section_refs().posting_region, frq_base, entry.frq_off_delta,
                        entry.frq_len, entry.prelude_len, abs_off, len);
}
Status LogicalIndexReader::resolve_prx_window(const DictEntry& entry, uint64_t prx_base,
                                              uint64_t* abs_off, uint64_t* len) const {
  return resolve_window(section_refs().posting_region, prx_base, entry.prx_off_delta,
                        entry.prx_len, /*prelude_len=*/0, abs_off, len);
}
```

`resolve_window` itself is **unchanged** (validation against `section.length`,
`abs_off = section.offset + base + off_delta + prelude_len`, overflow rejection).
It now receives `posting_region` for both calls. Because both prx and frq spans of a
term lie inside the same region, the existing bound check `total_len <=
section.length - in_pod` still correctly rejects corrupt/overrunning locators.

`lookup`/`prefix_terms` returning `frq_base`/`prx_base` from `DictBlockReader`:
**unchanged** (the block header still carries both; they now hold the same value).

### 7.2 Doc comments

Update the contract comments in `logical_index_reader.h` (lines ~27-31),
`logical_index_writer.h` (lines ~20-50), and `snii_compound_writer.h` (lines ~16-38)
to describe the single `posting_region` and the `[prx][frq]` per-term order. Replace
`section_refs().frq_pod` / `prx_pod` references with `posting_region`. In
`snii_segment_reader.cpp` specifically, **rewrite** the lines ~85-91 doc comment: the
old rationale ("`.prx` POD is only populated by windowed terms, so `prx_pod` can be
empty while every DICT block still carries the positions flag") is now obsolete and
actively misleading — capability no longer derives from a region length at all.

### 7.3 Downstream callers — exact audit (R2)

Audited by grep over `core/src`. Two categories:

**(a) Additive offset bases — repoint `frq_pod`/`prx_pod` → `posting_region`.**
These do NOT use `resolve_*_window`; they read `.offset` directly and MUST be
repointed, or they fetch from the wrong absolute byte range (and won't compile once
the struct fields are removed):

| Site | Current | After |
|------|---------|-------|
| `reader/windowed_posting.cpp:25` (`PreludeAbs`) | `section_refs().frq_pod.offset + frq_base + frq_off_delta` | `…posting_region.offset…` |
| `reader/windowed_posting.cpp:151` (`FetchWindowGeometry` prx_region_start) | `section_refs().prx_pod.offset + prx_base + prx_off_delta` | `…posting_region.offset…` |
| `reader/windowed_posting.cpp:211` (`FetchBlocks` prx_region_start) | `section_refs().prx_pod.offset + prx_base + prx_off_delta` | `…posting_region.offset…` |
| `query/phrase_query.cpp:117` (windowed `prelude_abs`) | `section_refs().frq_pod.offset + frq_base + frq_off_delta` | `…posting_region.offset…` |
| `query/scoring_query.cpp:82` (`FetchPrelude`) | `section_refs().frq_pod.offset + …` | `…posting_region.offset…` |
| `reader/logical_index_reader.cpp:249,257` (`resolve_*_window`) | `frq_pod` / `prx_pod` | `posting_region` (see §7.1) |

The reader `resolve_*` math after repointing is correct: `prx_base == frq_base` and
the deltas encode the interleave (§3.2).

**(b) Semantic positions predicate — does NOT repoint to a region length.**

| Site | Current | After |
|------|---------|-------|
| `reader/snii_segment_reader.cpp:95-96` (`has_positions`/tier inference) | `has_positions = section_refs().prx_pod.length > 0 \|\| has_norms` | **read the persisted flag** — see §7.4 |

**Not affected (corrections to the prior draft's list):**

- `stats/snii_stats_provider.cpp` — **no** `frq_pod`/`prx_pod` reads (grep: zero
  matches). Removed from the touch list.
- `query/term_query.cpp` — only `#include "snii/format/frq_pod.h"`; uses
  `resolve_frq_window` and has **no** direct field read. No source change beyond the
  include surviving (the codec header `frq_pod.h` is unrelated to `SectionRefs`).

### 7.4 `snii_segment_reader.cpp` `open_index` — read the persisted flag (R1)

Replace the region-length heuristic with the explicit meta flag (§3.1). Keep
`|| has_norms` only as a defensive belt-and-suspenders (a scoring index always has
positions); capability is otherwise read straight from the header bit:

```cpp
PerIndexMetaReader meta;
SNII_RETURN_IF_ERROR(PerIndexMetaReader::open(meta_bytes, &meta));
const bool has_norms = meta.section_refs().norms.length > 0;
const bool has_positions = meta.has_positions() || has_norms;   // NEW: flag, not prx_pod.length
const IndexTier tier = has_norms
                           ? IndexTier::kT3
                           : (has_positions ? IndexTier::kT2 : IndexTier::kT1);
return LogicalIndexReader::open(reader_, tier, has_positions, meta_bytes, out);
```

This makes a docs-only (T1) index with pod_ref terms report `has_positions == false`
(the writer wrote `kHasPositions == 0`), so `DictBlockReader::open`→`check_flags`
(`dict_block.cpp:121-124`) agrees with the per-block header and the index opens and
reads correctly. `posting_region.length` is **never** consulted for capability — in
either direction (non-empty docs-only, or empty all-INLINE positional).

---

## 8. Edge cases

1. **No-positions index (T1 / keyword / docs-only tier, `has_prx_ == false`).**
   No prx span is ever written; each pod_ref term's posting bytes are just its frq
   span. `prx_off_delta`/`prx_len` are not set. The posting region is byte-identical
   to today's `.frq` POD; `SectionRefs.posting_region` equals what `frq_pod` would
   have been. The block header writes no `prx_base` (`has_positions` false). The
   per-index meta header has `kHasPositions == 0`, so `open_index` recovers
   `has_positions == false` / tier T1 **regardless** of `posting_region.length`
   (which is non-zero whenever the index has any pod_ref term). `post_file_` is still
   opened (it holds the frq spans).

2. **Positions-capable index whose `posting_region` may be empty (all-INLINE).**
   A T2/T3 index whose terms are all slim-INLINE appends nothing to `post_file_`, so
   `posting_region.length == 0`. Capability is still recovered correctly because it
   comes from `kHasPositions` (set from `has_prx_`), **not** the region length. (This
   also fixes a pre-existing latent mis-tiering: a no-norms T2 all-INLINE index used
   to be mis-detected as T1 under the old `prx_pod.length`-based heuristic.)

3. **Multiple logical indexes.** Each index has its own `dict_file_`/`post_file_`
   pair, its own `posting_region`/`dict_region` placement, and its own meta header
   flag. `write_index_sections` emits `[posting_i][dict_i]` per index in add order;
   absolute offsets are captured independently via `cursor_`. No cross-index aliasing.

4. **Empty term set (index with zero terms).** No block opens, no posting bytes are
   appended; `post_file_.size() == 0`. `posting_region = {post_off, 0}`. `dict_region
   = {dict_off, 0}`. Reader sees both as zero-length and never resolves a window.
   `(off, 0)` is a present-but-empty region, distinct from the `(0,0)` "absent"
   convention used for norms/bsbf. **Capability is decided solely by `kHasPositions`/
   norms**, so an empty docs-only and an empty docs-positions index are correctly
   distinguished even though both have `posting_region.length == 0`.

5. **High-df term whose big prx span and frq span are far apart.** The widest term
   (df in the millions) writes a tens-of-MiB prx span (streamed window-by-window in
   pass 1) immediately followed by its `[prelude][dd-block][freq-block]` frq span.
   `frq_off_delta = prx_off_delta + prx_len` may therefore be a large value, but it
   is just a varint offset into one region; `resolve_frq_window` validates it
   against `posting_region.length`. No window straddles the prx/frq boundary (each
   dd/freq/prx window is a self-contained sub-range), so the interleaving does not
   affect any single range fetch. Peak RSS is unchanged: pass 1 frees
   `positions_flat` before pass 2 grows the dd/freq blocks; only the destination
   sink changed.

6. **Byte-correctness (reader reads exactly what writer wrote).** For every pod_ref
   term, `writer` appended `[prx][frq]` at `post_file_` offsets
   `prx_off`/`frq_off`; `reader` computes `posting_region.offset + base + delta`
   with the same `base` (block-open snapshot) and the same delta the writer stored.
   Since both deltas are measured against the identical block-open `post_file_.size()`
   snapshot, and the region is streamed verbatim into the container at
   `posting_region.offset`, the reader's absolute offset equals the writer's absolute
   write position byte-for-byte. The per-window prelude/dd/freq/prx *contents* are
   unchanged, so existing per-window decoders see identical bytes.

7. **INLINE terms interleaved with pod_ref terms.** INLINE terms append nothing to
   `post_file_`, so the posting region contains only the pod_ref terms' spans, in
   term order. An INLINE term sitting between two pod_ref terms does not create a gap
   in the region; the next pod_ref term's prx span simply follows the previous one's
   frq span. Deltas remain correct because they are measured from live
   `post_file_.size()`, which only advances on pod_ref appends.

---

## 9. Migration & files to touch (breaking format change)

This changes the on-disk container (meta `SectionRefs` field set, the per-index
section order, and one meta-header flag bit). SNII is **from-scratch and pre-launch**:
no index with an older layout exists, so there is no backward-compat shim, no dual-read
— the change folds into the single, first meta layout.

> **Version constant scope.** `kMetaFormatVersion`
> (`core/include/snii/format/format_constants.h`) is a **single shared constant**
> consumed by **both** `per_index_meta.cpp` (header `u16`) **and** `tail_meta_region.cpp`
> (region `u32`); it stays at **1** — the single from-scratch meta layout. The field is
> a self-describing fail-fast sentinel: a reader returns `Corruption` on any mismatch.
> It is **not** bumped for this change (a pre-launch format has no v1 index to coexist
> with — bumping would be migration ceremony with no deployed reader to gate); bump only
> **after launch**. **Do NOT** add a second/independent version field. The container
> `kFormatVersion` / bootstrap `min_reader_version` are unaffected (no framing change).

**Files to touch:**

| File | Change |
|------|--------|
| `core/include/snii/format/format_constants.h` | `kMetaFormatVersion` stays **1** (single from-scratch meta layout; shared by per_index_meta + tail_meta_region as a fail-fast sentinel). |
| `core/include/snii/format/per_index_meta.h` | `SectionRefs`: replace `frq_pod`+`prx_pod` with `posting_region`. Add `PerIndexMetaBuilder::kHasPositions = 1u << 0`. Add `PerIndexMetaReader::has_positions()` accessor. |
| `core/src/format/per_index_meta.cpp` | `encode_section_refs`/`decode_section_refs`: one region pair (`posting_region`) instead of two. (No header-write change needed beyond the existing `flags` u32, which now may carry bit 0.) |
| `core/include/snii/writer/logical_index_writer.h` | Members: drop `frq_file_`/`prx_file_`, add `post_file_`; replace `frq_pod_size`/`prx_pod_size`/`stream_frq_pod_into`/`stream_prx_pod_into` with `posting_region_size`/`stream_posting_region_into`; update contract comment. |
| `core/src/writer/logical_index_writer.cpp` | `build()` always open/seal `post` temp (drop the `has_prx_`-conditional frq/prx opens at :559,:566); `process_term` capture single `base`; `build_windowed_entry`/`build_slim_entry` write `[prx][frq]` to `post_file_`; `BuildWindowedPosting` param `prx_file`→`post_file`; `finish_meta` OR `kHasPositions` into `flags` from `has_prx_` (~:622). |
| `core/include/snii/writer/snii_compound_writer.h` | `Placement`: `post_off/post_len` instead of frq/prx; update layout doc comment. |
| `core/src/writer/snii_compound_writer.cpp` | `write_index_sections`: stream posting region then dict (replace :62,:68); `write_tail`: set `refs.posting_region` (replace :106-107). |
| `core/include/snii/reader/logical_index_reader.h` | Update contract comment to `posting_region`. |
| `core/src/reader/logical_index_reader.cpp` | `resolve_frq_window`/`resolve_prx_window` (:249,:257): use `section_refs().posting_region`. |
| `core/src/reader/snii_segment_reader.cpp` | **Semantic change (not a repoint):** `has_positions = meta.has_positions() \|\| has_norms` (replace the `prx_pod.length` heuristic at :95-96); rewrite the obsolete :85-91 doc comment. |
| `core/src/reader/windowed_posting.cpp` | Repoint `frq_pod`/`prx_pod` `.offset` reads to `posting_region` at :25, :151, :211. |
| `core/src/query/phrase_query.cpp` | Repoint `frq_pod.offset` to `posting_region.offset` at :117. |
| `core/src/query/scoring_query.cpp` | Repoint `frq_pod.offset` to `posting_region.offset` at :82. |
| `docs/design/SNII-design-spec.source.md` | Fold in the interleaved posting region layout (replace the two-POD section) and the `kHasPositions` meta flag. |

> `dict_block.{h,cpp}` (block header `frq_base`/`prx_base`) is **left unchanged** in
> this step: both bases now hold the same value, but keeping two header fields avoids
> a second wire-format change. `check_flags` (`dict_block.cpp:121-124`) is unchanged
> and now agrees with the writer because `has_positions` is driven by the persisted
> meta flag. A follow-up could collapse `frq_base`/`prx_base` to a single
> `posting_base` (further format bump); out of scope here.

---

## 10. Test plan

### 10.1 Existing tests that must still pass (adjusted for the new refs/order)

Per-file checklist of `frq_pod`/`prx_pod` member references that **will not compile**
after the struct change and must be rewritten to `posting_region` (with the
posting-region-first offset order):

- `core/tests/writer/snii_compound_writer_test.cpp`
  - `:210-213` — `ASSERT_GT(refs.frq_pod.length,0)` / `…prx_pod…` bounds asserts →
    `refs.posting_region` (single region; assert it precedes `dict_region`).
  - `:271` — `frq_abs = refs.frq_pod.offset + …` → `refs.posting_region.offset + …`.
  - `:466` — `idx.section_refs().frq_pod.offset + …` HOT-region check →
    `…posting_region.offset…`.
  - Comment `:207` and the dict-vs-posting position expectations: update for the
    **order flip** (posting region now precedes DICT region).
- `core/tests/format/per_index_meta_test.cpp`
  - `:58-59` — `SampleRefs()` sets `r.frq_pod`/`r.prx_pod` → set a single
    `r.posting_region`.
  - `:73-74` — `ExpectRefsEq()` compares `frq_pod`/`prx_pod` → compare
    `posting_region`. Assert old fields absent. Add a case asserting `kHasPositions`
    flag round-trips through `PerIndexMetaBuilder`/`Reader`.
- `core/tests/writer/phase_a_readback_test.cpp`
  - `:158` — `frq_abs = idx.section_refs().frq_pod.offset + …` →
    `…posting_region.offset…`. Round-trip must still resolve frq/prx windows and
    decode identical postings/positions.
- `core/tests/format/{frq_pod,prx_pod,dict_block}_test.cpp` — per-window/per-block
  encode bytes are unchanged (these don't touch section placement); should pass
  as-is, confirming byte-level codec invariance.
- `core/tests/query/{byte_skip,phrase_skip,posting_grouping,scoring_wand_selective}_test.cpp`
  — end-to-end query paths through `resolve_*_window` and the repointed direct-offset
  sites; must pass once the readers repoint to `posting_region`.

### 10.2 New tests

1. **Round-trip with positions (interleave correctness).** Build an index with
   `has_prx` terms spanning slim-pod_ref and windowed (df above and below
   `kSlimDfThreshold`). After read-back: for each term **(gate on `has_prx`)**,
   `frq_off_delta == prx_off_delta + prx_len`; `resolve_frq_window`/
   `resolve_prx_window` land inside `posting_region`; decoded docids/freqs/positions
   equal the input.
2. **Docids match (no-freq read path).** For a docs-only prefix fetch
   (`frq_docs_len`), confirm the docs decode equals the full decode's docids without
   touching the freq region, in the interleaved layout. Assert the docs-only prefix
   stays inside the frq span (does not read prx bytes).
3. **Byte-correctness of the combined region.** After `finish()`, read the raw
   `posting_region` byte range from the container and assert, term by term, that the
   sub-range `[prx_off_delta, prx_off_delta+prx_len)` equals the independently-built
   prx bytes and `[frq_off_delta, frq_off_delta+frq_len)` equals the independently-
   built frq bytes (prelude+dd-block+freq-block for windowed; dd+freq for slim).
4. **Multi-index.** Two logical indexes (one docs-positions, one docs-only) in one
   container; assert each `posting_region` placement is independent and both resolve
   correctly; no offset bleed across indexes.
5. **No-positions index unaffected (bytes).** A T1/keyword index: `posting_region`
   holds only frq spans, `prx_off_delta`/`prx_len` unset, bytes identical to the
   pre-change `.frq` POD for the same input (golden-bytes compare against a captured
   fixture).
6. **Empty term set.** Index with zero terms → `posting_region.length == 0`,
   `dict_region.length == 0`; reader opens cleanly and all lookups miss. Build it
   both docs-only and docs-positions and assert tier is recovered correctly from the
   flag despite both having `posting_region.length == 0`.
7. **High-df interleave.** A synthetic million-doc term: assert (gate on `has_prx`)
   the big prx span and frq span are contiguous (`frq_off_delta == prx_off_delta +
   prx_len`), each window resolves within bounds, and decoded positions round-trip.
   (Can use the existing `pos_pump` streamed-positions path to keep the test's RSS
   bounded.)
8. **INLINE-between-pod_ref.** Mix tiny (INLINE) and large (pod_ref) terms; assert
   INLINE terms append nothing to the region (region length == sum of pod_ref spans)
   and pod_ref deltas stay contiguous across the interleaving.
9. **Tier recovery for docs-only-with-pod_ref (R1 regression guard).** Build a
   **docs-only (T1)** index containing at least one pod_ref term (df ≥
   `kSlimDfThreshold`, or a windowed term) so `posting_region.length > 0`. Open it
   via `SniiSegmentReader::open_index` and assert `tier == kT1` /
   `has_positions == false`, then perform a term lookup + docid decode and assert it
   succeeds with **no** spurious positions-flag parse (i.e. `DictBlockReader::open`
   does not return `InvalidArgument`). This test fails under the naive
   `posting_region.length > 0` heuristic and passes with the persisted flag — it is
   the dedicated guard for the blocker. The golden-bytes test (#5) does **not** cover
   it (the break is in `open_index` tier selection, not region bytes).

All new tests emit the OpenLogos reporter line per project convention.

### 10.3 Peak-RSS guard

Reuse/extend the existing large-corpus build to confirm: (a) only **two** index temp
files per index exist on disk during build (assert `frq`/`prx` temp tags are gone,
`post` present), and (b) peak RSS is `<=` the pre-change baseline (the dict temp and
the windowed two-pass strategy are retained).
