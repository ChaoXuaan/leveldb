// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#ifndef STORAGE_LEVELDB_DB_DBFORMAT_H_
#define STORAGE_LEVELDB_DB_DBFORMAT_H_

#include <stdio.h>
#include "leveldb/comparator.h"
#include "leveldb/db.h"
#include "leveldb/filter_policy.h"
#include "leveldb/slice.h"
#include "leveldb/table_builder.h"
#include "util/coding.h"
#include "util/logging.h"

namespace leveldb {

// Grouping of constants.  We may want to make some of these
// parameters set via options.
namespace config {
/* 磁盘文件的层数 */
static const int kNumLevels = 7;

// Level-0 compaction is started when we hit this many files.
/* Level-0的sstable超过4个就会触发压缩 */
static const int kL0_CompactionTrigger = 4;

// Soft limit on number of level-0 files.  We slow down writes at this point.
/* 当Level-0的sstable的个数超过8就放缓写入速度 */
static const int kL0_SlowdownWritesTrigger = 8;

// Maximum number of level-0 files.  We stop writes at this point.
/* 当Level-0的sstable的个数超过12就停止写服务 */
static const int kL0_StopWritesTrigger = 12;

// Maximum level to which a new compacted memtable is pushed if it
// does not create overlap.  We try to push to level 2 to avoid the
// relatively expensive level 0=>1 compactions and to avoid some
// expensive manifest file operations.  We do not push all the way to
// the largest level since that can generate a lot of wasted disk
// space if the same key space is being repeatedly overwritten.
/* 该常量定义了如果memtable的key没有重叠(overlap)，那么就直接写入到第2层，
 * 这样避免了Level-0到Level-1的压缩，也避免了manifest的写。
 */
static const int kMaxMemCompactLevel = 2;

// Approximate gap in bytes between samples of data read during iteration.
// ？？
static const int kReadBytesPeriod = 1048576;

}  // namespace config

class InternalKey;

// Value types encoded as the last component of internal keys.
// DO NOT CHANGE THESE ENUM VALUES: they are embedded in the on-disk
// data structures.
/* 数据类型，在LevelDB中所有数据都是追加，为了判断数据是否被删除，
 * 需要通过标签识别。不要随意改动下列值，它们会随数据写入磁盘。
 */
enum ValueType {
  kTypeDeletion = 0x0,
  kTypeValue = 0x1
};
// kValueTypeForSeek defines the ValueType that should be passed when
// constructing a ParsedInternalKey object for seeking to a particular
// sequence number (since we sort sequence numbers in decreasing order
// and the value type is embedded as the low 8 bits in the sequence
// number in internal keys, we need to use the highest-numbered
// ValueType, not the lowest).
/* 这个变量的定义和InternalKey的比较有关系，为了不影响比较，所以定为最大，
 * 参见 InternalKey::Compare()
 */
static const ValueType kValueTypeForSeek = kTypeValue;

/* SequenceNUmber对于LevelDB是非常重要的，用于标记最新数据
 * SequnceNumber 在 leveldb 有重要的地位，key 的排序，compact 以及 snapshot 都依赖于它
 */
typedef uint64_t SequenceNumber;

// We leave eight bits empty at the bottom so a type and sequence#
// can be packed together into 64-bits.
/* 低8位存放ValueType，所以SequenceNumber最大为2^56 - 1 */
static const SequenceNumber kMaxSequenceNumber =
    ((0x1ull << 56) - 1);

/* ParsedInternalKey为db内部操作的key，包含三个成员变量：
 * user_key: 用户输入的key
 * sequence: 写入数据时赋予的序列号
 * type: 数据类型，kTypeValue or kTypeDeletion
 */
struct ParsedInternalKey {
  Slice user_key;
  SequenceNumber sequence;
  ValueType type;

  ParsedInternalKey() { }  // Intentionally left uninitialized (for speed)
  ParsedInternalKey(const Slice& u, const SequenceNumber& seq, ValueType t)
      : user_key(u), sequence(seq), type(t) { }
  std::string DebugString() const;
};

// Return the length of the encoding of "key".
/* InternalKey需要的字节数 = user_key字节数 + 8
 * 8个字节是sequence + ValueType, ValueType存放在sequence的低8位
 */
inline size_t InternalKeyEncodingLength(const ParsedInternalKey& key) {
  return key.user_key.size() + 8;
}

// Append the serialization of "key" to *result.
extern void AppendInternalKey(std::string* result,
                              const ParsedInternalKey& key);

// Attempt to parse an internal key from "internal_key".  On success,
// stores the parsed data in "*result", and returns true.
//
// On error, returns false, leaves "*result" in an undefined state.
extern bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result);

// Returns the user key portion of an internal key.
/* 从Internal_key解析user_key */
inline Slice ExtractUserKey(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  return Slice(internal_key.data(), internal_key.size() - 8);
}

/* 解析ValueType */
inline ValueType ExtractValueType(const Slice& internal_key) {
  assert(internal_key.size() >= 8);
  const size_t n = internal_key.size();
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  unsigned char c = num & 0xff;
  return static_cast<ValueType>(c);
}

// A comparator for internal keys that uses a specified comparator for
// the user key portion and breaks ties by decreasing sequence number.
/* InternalKey比较器 */
class InternalKeyComparator : public Comparator {
 private:
  /* 用于比较user key，在具体的实现中用到的是BytewiseComparatorImpl */
  const Comparator* user_comparator_;
 public:
  explicit InternalKeyComparator(const Comparator* c) : user_comparator_(c) { }
  virtual const char* Name() const;
  virtual int Compare(const Slice& a, const Slice& b) const;
  virtual void FindShortestSeparator(
      std::string* start,
      const Slice& limit) const;
  virtual void FindShortSuccessor(std::string* key) const;

  const Comparator* user_comparator() const { return user_comparator_; }

  int Compare(const InternalKey& a, const InternalKey& b) const;
};

// Filter policy wrapper that converts from internal keys to user keys
class InternalFilterPolicy : public FilterPolicy {
 private:
  const FilterPolicy* const user_policy_;
 public:
  explicit InternalFilterPolicy(const FilterPolicy* p) : user_policy_(p) { }
  virtual const char* Name() const;
  virtual void CreateFilter(const Slice* keys, int n, std::string* dst) const;
  virtual bool KeyMayMatch(const Slice& key, const Slice& filter) const;
};

// Modules in this directory should keep internal keys wrapped inside
// the following class instead of plain strings so that we do not
// incorrectly use string comparisons instead of an InternalKeyComparator.
/* InternalKey是对ParsedInternalKey合并后的key，只包含一个成员变量rep_，
 * rep_的格式为:
 *                    7 Bytes        1 Byte
 *   ------------------------------------------
 *   |  user_key | SequenceNumber | ValueType |
 *   --------------------- --------------------
 *
 * 本来SequenceNumber是8个字节，最低1位字节用来存ValueType，
 * 通过dbformat.cc PackSequenceAndType()实现
 */
class InternalKey {
 private:
  std::string rep_;
 public:
  InternalKey() { }   // Leave rep_ as empty to indicate it is invalid
  InternalKey(const Slice& user_key, SequenceNumber s, ValueType t) {
    AppendInternalKey(&rep_, ParsedInternalKey(user_key, s, t));
  }

  /* 将Slice解封成InternalKey的成员变量rep_(string) */
  void DecodeFrom(const Slice& s) { rep_.assign(s.data(), s.size()); }
  /* 将InternalKey的成员变量rep_(string)封装成Slice */
  Slice Encode() const {
    assert(!rep_.empty());
    return rep_;
  }

  Slice user_key() const { return ExtractUserKey(rep_); }

  void SetFrom(const ParsedInternalKey& p) {
    rep_.clear();
    AppendInternalKey(&rep_, p);
  }

  void Clear() { rep_.clear(); }

  std::string DebugString() const;
};

/* 比较InternalKey的成员变量rep_，调用Compare(Slice, Slice) */
inline int InternalKeyComparator::Compare(
    const InternalKey& a, const InternalKey& b) const {
  return Compare(a.Encode(), b.Encode());
}

/* 将InternalKey转换成ParsedInternalKey */
inline bool ParseInternalKey(const Slice& internal_key,
                             ParsedInternalKey* result) {
  // 获取internal_key的size
  const size_t n = internal_key.size();
  // 长度小于8字节返回false
  if (n < 8) return false;
  // 得到internal_key的最后8字节
  uint64_t num = DecodeFixed64(internal_key.data() + n - 8);
  // 解析ValueType
  unsigned char c = num & 0xff;
  // 解析SequenceNumber
  result->sequence = num >> 8;
  result->type = static_cast<ValueType>(c);
  result->user_key = Slice(internal_key.data(), n - 8);
  // 最后检查ValueType
  return (c <= static_cast<unsigned char>(kTypeValue));
}

// A helper class useful for DBImpl::Get()
/* LookupKey在memetable中查找数据使用，格式如下：
 *      Varint32
 * -----------------------------------
 * | Internal Key size| Internal Key |
 * -----------------------------------
 */
class LookupKey {
 public:
  // Initialize *this for looking up user_key at a snapshot with
  // the specified sequence number.
  LookupKey(const Slice& user_key, SequenceNumber sequence);

  ~LookupKey();

  // Return a key suitable for lookup in a MemTable.
  /* memtable key其实就是LookupKey，只不过转换成Slice */
  Slice memtable_key() const { return Slice(start_, end_ - start_); }

  // Return an internal key (suitable for passing to an internal iterator)
  /* 提取internal key */
  Slice internal_key() const { return Slice(kstart_, end_ - kstart_); }

  // Return the user key
  /* 提取user key */
  Slice user_key() const { return Slice(kstart_, end_ - kstart_ - 8); }

 private:
  // We construct a char array of the form:
  //    klength  varint32               <-- start_
  //    userkey  char[klength]          <-- kstart_
  //    tag      uint64
  //                                    <-- end_
  // The array is a suitable MemTable key.
  // The suffix starting with "userkey" can be used as an InternalKey.
  /* 指向LookupKey首地址 */
  const char* start_;
  /* 指向InternalKey首地址 */
  const char* kstart_;
  /* 指向Lookkey尾 */
  const char* end_;
  /* 预留200字节，避免为短字节key分配空间 */
  char space_[200];      // Avoid allocation for short keys

  // No copying allowed
  LookupKey(const LookupKey&);
  void operator=(const LookupKey&);
};

inline LookupKey::~LookupKey() {
  if (start_ != space_) delete[] start_;
}

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_DB_DBFORMAT_H_
