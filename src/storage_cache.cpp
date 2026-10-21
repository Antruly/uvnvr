#include "storage_cache.h"

#include <algorithm>
#include <fstream>
#include <utility>

namespace uvnvr {

BlockCache::BlockCache(size_t capacity_bytes, size_t block_size)
    : capacity_bytes_(capacity_bytes), block_size_(block_size == 0 ? 64 * 1024 : block_size) {}

BlockCache::Key BlockCache::MakeKey(const std::string& path, uint64_t block) {
  return path + '\x1f' + std::to_string(block);
}

BlockCache::Block BlockCache::LoadBlock(const std::string& path, uint64_t block,
                                        std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error != nullptr)
      *error = "cannot open " + path;
    return nullptr;
  }
  const uint64_t offset = block * block_size_;
  if (offset > static_cast<uint64_t>(1) << 62) {
    if (error != nullptr)
      *error = "cache offset out of range";
    return nullptr;
  }
  file.seekg(static_cast<std::streamoff>(offset));
  auto data = std::make_shared<std::vector<uint8_t>>(block_size_);
  file.read(reinterpret_cast<char*>(data->data()), static_cast<std::streamsize>(data->size()));
  const std::streamsize got = file.gcount();
  if (got <= 0) {
    data->clear();
  } else {
    data->resize(static_cast<size_t>(got));
  }
  if (error != nullptr)
    error->clear();
  return data;
}

BlockCache::Block BlockCache::Find(const Key& key) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(key);
    if (it == entries_.end()) {
      ++misses_;
      return nullptr;
    }
    ++hits_;
    lru_.splice(lru_.begin(), lru_, it->second.position);
    return it->second.block;
  }
}

void BlockCache::Insert(const Key& key, const Block& block) {
  if (!block || block->empty() || capacity_bytes_ == 0)
    return;
  std::lock_guard<std::mutex> lock(mutex_);
  if (entries_.find(key) != entries_.end())
    return;
  const size_t size = block->size();
  while (!lru_.empty() && cached_bytes_ + size > capacity_bytes_) {
    const Key victim = lru_.back();
    lru_.pop_back();
    const auto it = entries_.find(victim);
    if (it != entries_.end()) {
      cached_bytes_ -= it->second.block->size();
      entries_.erase(it);
    }
  }
  if (size > capacity_bytes_)
    return;
  lru_.push_front(key);
  entries_[key] = Entry{block, lru_.begin()};
  cached_bytes_ += size;
}

bool BlockCache::Read(const std::string& path, uint64_t offset, size_t length,
                      std::vector<uint8_t>* out, std::string* error) {
  out->clear();
  if (length == 0)
    return true;
  const uint64_t first_block = offset / block_size_;
  const uint64_t last_block = (offset + length - 1) / block_size_;
  out->reserve(length);
  uint64_t block_start = first_block * block_size_;
  for (uint64_t block = first_block; block <= last_block; ++block) {
    const Key key = MakeKey(path, block);
    Block data = Find(key);
    if (!data) {
      data = LoadBlock(path, block, error);
      if (!data)
        return false;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        bytes_read_ += data->size();
      }
      Insert(key, data);
    }
    if (data->empty())
      break;
    const size_t skip = block == first_block ? static_cast<size_t>(offset - block_start) : 0;
    if (skip >= data->size())
      continue;
    const size_t remaining = length - out->size();
    const size_t take = std::min(remaining, data->size() - skip);
    out->insert(out->end(), data->begin() + static_cast<std::ptrdiff_t>(skip),
                data->begin() + static_cast<std::ptrdiff_t>(skip + take));
    if (out->size() >= length)
      break;
    block_start += block_size_;
  }
  if (error != nullptr)
    error->clear();
  return true;
}

void BlockCache::Preload(const std::string& path, uint64_t offset, size_t length) {
  if (length == 0)
    return;
  const uint64_t first_block = offset / block_size_;
  const uint64_t last_block = (offset + length - 1) / block_size_;
  for (uint64_t block = first_block; block <= last_block; ++block) {
    const Key key = MakeKey(path, block);
    if (Find(key))
      continue;
    std::string error;
    Block data = LoadBlock(path, block, &error);
    if (!data || data->empty())
      break;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      bytes_read_ += data->size();
    }
    Insert(key, data);
  }
}

BlockCache::Stats BlockCache::GetStats() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Stats stats;
  stats.hits = hits_;
  stats.misses = misses_;
  stats.bytes_read = bytes_read_;
  stats.bytes_cached = cached_bytes_;
  stats.entries = entries_.size();
  stats.capacity_bytes = capacity_bytes_;
  stats.block_size = block_size_;
  return stats;
}

void BlockCache::SetCapacity(size_t capacity_bytes) {
  std::lock_guard<std::mutex> lock(mutex_);
  capacity_bytes_ = capacity_bytes;
  while (!lru_.empty() && cached_bytes_ > capacity_bytes_) {
    const Key victim = lru_.back();
    lru_.pop_back();
    const auto it = entries_.find(victim);
    if (it != entries_.end()) {
      cached_bytes_ -= it->second.block->size();
      entries_.erase(it);
    }
  }
}

} // namespace uvnvr
