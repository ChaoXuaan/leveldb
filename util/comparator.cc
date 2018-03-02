// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <algorithm>
#include <stdint.h>
#include "leveldb/comparator.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"

namespace leveldb {

Comparator::~Comparator() { }

namespace {
/* Comparator的子类，实现按字典升序排序 */
class BytewiseComparatorImpl : public Comparator {
 public:
  BytewiseComparatorImpl() { }

  virtual const char* Name() const {
    return "leveldb.BytewiseComparator";
  }

  /* 调用Slice的compare，就是两个字符串比较大小 */
  virtual int Compare(const Slice& a, const Slice& b) const {
    return a.compare(b);
  }

  /* 这个函数的作用就是：如果*start < limit，就在[start,limit)中找到一个
   * 短字符串，并赋给*start返回；简单的comparator实现可能不改变*start，这也是正确的。
   * 这么做的目的是节约存储，加速对比
   */
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const {
    // Find length of common prefix
    /* 找start和limit公共部分的长度 */
    size_t min_length = std::min(start->size(), limit.size());
    /* diff_index记录start和limit第一个不相等的位置 */
    size_t diff_index = 0;
    while ((diff_index < min_length) &&
           ((*start)[diff_index] == limit[diff_index])) {
      diff_index++;
    }

    if (diff_index >= min_length) {
      // Do not shorten if one string is a prefix of the other
    } else {
      /* 如果(*start)[diff_index]的字符小于0xff
       * 并且小于limit[diff_index]
       * 那么将(*start)[diff_index]++，然后重设*start的长度，此时长度一般会变小
       */
      uint8_t diff_byte = static_cast<uint8_t>((*start)[diff_index]);
      if (diff_byte < static_cast<uint8_t>(0xff) &&
          diff_byte + 1 < static_cast<uint8_t>(limit[diff_index])) {
        (*start)[diff_index]++;
        start->resize(diff_index + 1);
        assert(Compare(*start, limit) < 0);
      }
    }
  }

  /* 找一个>= *key的短字符串
   * 简单的comparator实现可能不改变*key，这也是正确的
   */
  virtual void FindShortSuccessor(std::string* key) const {
    // Find first character that can be incremented
    /* 找到第一个可以自增1的字符，然后截断key  */
    size_t n = key->size();
    for (size_t i = 0; i < n; i++) {
      const uint8_t byte = (*key)[i];
      if (byte != static_cast<uint8_t>(0xff)) {
        (*key)[i] = byte + 1;
        key->resize(i+1);
        return;
      }
    }
    // *key is a run of 0xffs.  Leave it alone.
  }
};
}  // namespace

static port::OnceType once = LEVELDB_ONCE_INIT;
static const Comparator* bytewise;

static void InitModule() {
  bytewise = new BytewiseComparatorImpl;
}

const Comparator* BytewiseComparator() {
  port::InitOnce(&once, InitModule);
  return bytewise;
}

}  // namespace leveldb
