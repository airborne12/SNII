#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <utility>

namespace snii::writer {

// Per-WRITER accurate byte counter for build-time RAM (one per SniiCompoundWriter =
// one per segment's inverted index). Modules report their own resident-byte deltas;
// current_bytes() is that writer's accurate live usage. OBSERVE-ONLY -- SNII never
// makes a flush decision from it (gate 1 belongs to Doris; gate 2 is the internal
// threshold). consume_release mirrors the delta into Doris's LOAD MemTracker so the
// inverted-index RAM is counted by MemTableMemoryLimiter's pressure decision; it is
// null off-Doris (bench / unit tests), where only the local atomic is updated.
class MemoryReporter {
 public:
  using ConsumeReleaseFn = std::function<void(int64_t delta)>;  // null off-Doris
  explicit MemoryReporter(ConsumeReleaseFn consume_release = nullptr)
      : consume_release_(std::move(consume_release)) {}

  MemoryReporter(const MemoryReporter&) = delete;
  MemoryReporter& operator=(const MemoryReporter&) = delete;

  // delta > 0 grows, delta < 0 shrinks/frees. Exactly one report per change site.
  void report(int64_t delta) {
    current_.fetch_add(delta, std::memory_order_relaxed);
    if (consume_release_) consume_release_(delta);  // mirror into Doris load tracker
  }

  int64_t current_bytes() const { return current_.load(std::memory_order_relaxed); }

 private:
  std::atomic<int64_t> current_{0};
  ConsumeReleaseFn consume_release_;
};

}  // namespace snii::writer
