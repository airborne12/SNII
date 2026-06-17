#pragma once

#include <cstdint>

// SNII 容器与各 section 的 on-disk 契约常量。
// 这些值一旦发布即为格式语义，修改需提升 format_version 并保持兼容策略。
// 所有多字节定长字段小端；变长整数 LEB128（见 snii/encoding/varint.h）。
namespace snii::format {

// ---- 容器级 magic / 版本 ----
// "SNII" 小端读为 0x49494E53。
inline constexpr uint32_t kContainerMagic = 0x49494E53u;  // 'S''N''I''I'
inline constexpr uint32_t kTailMagic = 0x4C494154u;       // 'T''A''I''L'
inline constexpr uint16_t kFormatVersion = 1;
inline constexpr uint16_t kMinReaderVersion = 1;
inline constexpr uint16_t kMetaFormatVersion = 1;

// ---- SectionFramer 的 section 类型 id（per-index meta / tail region 内）----
enum class SectionType : uint8_t {
  kStatsBlock = 1,
  kSampledTermIndex = 2,
  kDictBlockDirectory = 3,
  kXFilter = 4,
  kSectionRefs = 5,
  kPerIndexMetaHeader = 6,
  kLogicalIndexDirectory = 7,
  kTailMetaHeader = 8,
  kFeatureBits = 9,
};

// ---- logical index 倒排存储内容配置（按 logical index 固定，非 per-term）----
// 决定是否写 freq / positions / norms+stats。
enum class IndexConfig : uint8_t {
  kDocsOnly = 0,             // 仅 docid：term/match 过滤
  kDocsPositions = 1,        // docid+freq+positions：MATCH_PHRASE
  kDocsPositionsScoring = 2, // + norms + stats：phrase + BM25
  kPositionsOffsets = 3,     // 预留（高亮/RAG），本期不实现
};

// term stats / posting 能力分层：tier>=kT2 才写 ttf_delta/max_freq 与 .prx。
enum class IndexTier : uint8_t {
  kT1 = 1,  // docs-only
  kT2 = 2,  // docs-positions
  kT3 = 3,  // docs-positions-scoring
};

inline constexpr IndexTier tier_of(IndexConfig cfg) {
  return cfg == IndexConfig::kDocsOnly ? IndexTier::kT1
       : cfg == IndexConfig::kDocsPositions ? IndexTier::kT2
       : IndexTier::kT3;  // scoring / offsets
}
inline constexpr bool has_positions(IndexConfig cfg) { return cfg != IndexConfig::kDocsOnly; }
inline constexpr bool has_scoring(IndexConfig cfg) { return cfg == IndexConfig::kDocsPositionsScoring; }

// ---- DictEntry flags 位定义 ----
namespace dict_flags {
inline constexpr uint8_t kKind = 1u << 0;       // 0=pod_ref / 1=inline
inline constexpr uint8_t kEnc = 1u << 1;        // 0=slim / 1=windowed
inline constexpr uint8_t kHasSb = 1u << 2;      // posting prelude 带子块目录
inline constexpr uint8_t kHasChampion = 1u << 3;  // v1 恒 0
inline constexpr uint8_t kOffsetsRef = 1u << 4;   // v1 恒 0
// bit5-7 reserved
}  // namespace dict_flags

enum class DictEntryKind : uint8_t { kPodRef = 0, kInline = 1 };
enum class DictEntryEnc : uint8_t { kSlim = 0, kWindowed = 1 };

// ---- .frq 窗口压缩模式 ----
enum class FrqWinMode : uint8_t { kRaw = 0, kZstd = 1 };

// ---- .prx 窗口 codec（codec 字节 bit0-5）----
enum class PrxCodec : uint8_t { kRaw = 0, kZstd = 1 /* bit7 cont-reserved */ };

// ---- 构建期参数（非格式语义，可按真实指标校准）----
inline constexpr uint32_t kFrqBaseUnit = 256;             // 窗口基准 unit
inline constexpr uint32_t kSlimDfThreshold = 512;         // df < 此 → slim
inline constexpr uint32_t kDefaultInlineThreshold = 256;  // slim 编码字节 ≤ 此 → inline
inline constexpr uint32_t kDefaultTargetDictBlockBytes = 64 * 1024;
inline constexpr uint32_t kXFilterL0MaxBytes = 256 * 1024;       // ≤ 此入 L0 常驻
inline constexpr uint32_t kXFilterMaxTermCount = 32u * 1024 * 1024;  // > 此可省略 XF

}  // namespace snii::format
