// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_UTIL_ARENA_H_
#define STORAGE_LEVELDB_UTIL_ARENA_H_

#include <vector>
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include "port/port.h"

namespace leveldb {

// xc 20180708
// Arena 内存管理
class Arena {
 public:
  Arena();
  ~Arena();

  // Return a pointer to a newly allocated memory block of "bytes" bytes.
  char* Allocate(size_t bytes);

  // Allocate memory with the normal alignment guarantees provided by malloc
  char* AllocateAligned(size_t bytes);

  // Returns an estimate of the total memory usage of data allocated
  // by the arena.
  // 返回使用内存大小
  size_t MemoryUsage() const {
    return reinterpret_cast<uintptr_t>(memory_usage_.NoBarrier_Load());
  }

 private:
  char* AllocateFallback(size_t bytes);
  char* AllocateNewBlock(size_t block_bytes);

  // Allocation state
  // 当前内存指针
  char* alloc_ptr_;
  // 当前申请内存的剩余空间
  size_t alloc_bytes_remaining_;

  // Array of new[] allocated memory blocks
  // 保存所有已申请的内存块
  std::vector<char*> blocks_;

  // Total memory usage of the arena.
  // 已经使用的内存大小
  port::AtomicPointer memory_usage_;

  // No copying allowed
  // 禁止拷贝构造
  Arena(const Arena&);
  void operator=(const Arena&);
};

// 申请内存，大小为 bytes 个字节
// 返回首地址
inline char* Arena::Allocate(size_t bytes) {
  // The semantics of what to return are a bit messy if we allow
  // 0-byte allocations, so we disallow them here (we don't need
  // them for our internal use).
  // 申请的内存如果当前内存块剩余空间能够满足则直接分配
  // 否则重新向系统申请一个内存块，这里会存在一些碎片空间
  assert(bytes > 0);
  if (bytes <= alloc_bytes_remaining_) {
    char* result = alloc_ptr_;
    alloc_ptr_ += bytes;
    alloc_bytes_remaining_ -= bytes;
    return result;
  }
  return AllocateFallback(bytes);
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_UTIL_ARENA_H_
