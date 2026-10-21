#pragma once

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace uvnvr {

// A small fixed-block LRU cache shared by every playback session. Playback
// reads 64 KiB at a time, so one request maps to one cache block in the common
// case; multiple clients watching the same recording share the blocks.
class BlockCache {
public:
  struct Stats {
    uint64_t hits = 0;
    uint64_t misses = 0;
    uint64_t bytes_read = 0;
    uint64_t bytes_cached = 0;
    size_t entries = 0;
    size_t capacity_bytes = 0;
    size_t block_size = 0;
  };

  explicit BlockCache(size_t capacity_bytes, size_t block_size = 64 * 1024);

  BlockCache(const BlockCache&) = delete;
  BlockCache& operator=(const BlockCache&) = delete;

  // Reads [offset, offset + length) into `out`. Returns false only on an I/O
  // error; a read that reaches EOF returns true with fewer bytes.
  bool Read(const std::string& path, uint64_t offset, size_t length, std::vector<uint8_t>* out,
            std::string* error);

  // Best-effort fill of the cache for [offset, offset + length). Never fails
  // the caller.
  void Preload(const std::string& path, uint64_t offset, size_t length);

  Stats GetStats() const;
  void SetCapacity(size_t capacity_bytes);

private:
  using Block = std::shared_ptr<std::vector<uint8_t>>;
  using Key = std::string;

  static Key MakeKey(const std::string& path, uint64_t block);

  Block LoadBlock(const std::string& path, uint64_t block, std::string* error);
  Block Find(const Key& key);
  void Insert(const Key& key, const Block& block);

  mutable std::mutex mutex_;
  size_t capacity_bytes_;
  size_t block_size_;
  size_t cached_bytes_ = 0;
  uint64_t hits_ = 0;
  uint64_t misses_ = 0;
  uint64_t bytes_read_ = 0;
  std::list<Key> lru_;
  struct Entry {
    Block block;
    std::list<Key>::iterator position;
  };
  std::unordered_map<Key, Entry> entries_;
};

} // namespace uvnvr
