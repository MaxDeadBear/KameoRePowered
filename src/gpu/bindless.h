#pragma once

// Bindless slot allocation and post-fence destruction, ported from re:Blue's gpu/bindless_allocator.h and gpu/deferred_destroy.h.

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace kameo::gpu {

inline constexpr uint32_t kNoSlot = 0xFFFFFFFFu;

// Linear free-slot scan over a used[] bitmap, from a rolling hint since this allocates per streamed texture (re:Blue scans from 0).
class BindlessSlots {
 public:
  void reset(uint32_t capacity, uint32_t first) {
    used_.assign(capacity, false);
    first_ = first;
    hint_ = first;
    live_ = 0;
    for (uint32_t i = 0; i < first && i < capacity; ++i) {
      used_[i] = true;
    }
  }

  uint32_t allocate() {
    const uint32_t n = uint32_t(used_.size());
    if (n == 0) {
      return kNoSlot;
    }
    for (uint32_t step = 0; step < n; ++step) {
      uint32_t i = hint_ + step;
      if (i >= n) {
        i = first_ + (i - n);
        if (i >= n) {
          break;
        }
      }
      if (!used_[i]) {
        used_[i] = true;
        ++live_;
        hint_ = (i + 1 < n) ? i + 1 : first_;
        return i;
      }
    }
    return kNoSlot;
  }

  void free(uint32_t slot) {
    if (slot < first_ || slot >= uint32_t(used_.size()) || !used_[slot]) {
      return;
    }
    used_[slot] = false;
    --live_;
    if (slot < hint_) {
      hint_ = slot;
    }
  }

  uint32_t live() const { return live_; }
  uint32_t capacity() const { return uint32_t(used_.size()); }

 private:
  std::vector<bool> used_;
  uint32_t first_ = 0;
  uint32_t hint_ = 0;
  uint32_t live_ = 0;
};

// Objects a command list in flight may still reference, each stamped with the frame that released it and freed once no list in flight can name it.
template <typename T>
class DeferredDestroyQueue {
 public:
  void queue(T&& object, uint64_t frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.push_back({std::move(object), frame});
  }

  // Frees everything queued more than `age` frames before `frame`; destructors run on the caller's thread with no lock held.
  size_t retire(uint64_t frame, uint64_t age) {
    return retire(frame, age, [](T&) {});
  }

  template <typename Release>
  size_t retire(uint64_t frame, uint64_t age, Release release) {
    std::vector<Entry> dead;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      auto split = std::partition(pending_.begin(), pending_.end(), [&](const Entry& e) {
        return frame < e.frame + age;
      });
      dead.assign(std::make_move_iterator(split), std::make_move_iterator(pending_.end()));
      pending_.erase(split, pending_.end());
    }
    for (Entry& entry : dead) release(entry.object);
    return dead.size();
  }

  // Frees everything regardless of age, for a caller that has just waited on every frame in flight.
  size_t drain() {
    std::vector<Entry> dead;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      dead.swap(pending_);
    }
    return dead.size();
  }

  size_t depth() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.size();
  }

 private:
  struct Entry {
    T object;
    uint64_t frame = 0;
  };
  std::mutex mutex_;
  std::vector<Entry> pending_;
};

}  // namespace kameo::gpu
