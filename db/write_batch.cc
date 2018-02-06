// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record :=
//    kTypeValue varstring varstring         |
//    kTypeDeletion varstring
// varstring :=
//    len: varint32
//    data: uint8[len]

// WriteBatch封装了一批record,
// leveldb为了提高插入和删除的效率，在其插入过程中都采用了批量集合相邻的多个具有相同同步设置的写请求以批量的方式进行写入
// 具体格式如下:
// sequence: 8字节, 是这批record的起始序列号
// count: 4字节, record个数
// count个record, record格式如下:
//                    1字节		     varint	 varint   varint    varint
//                ------------------------------------------------------
//		插入数据时:|type(kTypeValue)|Key_size|  Key  |Value_size| Value |
//				  ------------------------------------------------------
//		删除数据时:|kTypeDeletion   |Key_size|  Key  |
//				  -----------------------------------

#include "leveldb/write_batch.h"

#include "leveldb/db.h"
#include "db/dbformat.h"
#include "db/memtable.h"
#include "db/write_batch_internal.h"
#include "util/coding.h"

namespace leveldb {

// WriteBatch header has an 8-byte sequence number followed by a 4-byte count.
// WriteBatch头部长度, 头部包含8字节sequence number和4字节记录WriteBatch中的kv对
static const size_t kHeader = 12;

// WriteBatch构造函数, 调用Clear
WriteBatch::WriteBatch() {
  Clear();
}

// 析构函数
WriteBatch::~WriteBatch() { }

WriteBatch::Handler::~Handler() { }

// 清空字符串rep_, 然后初始化字符串长度为kHeader=12
void WriteBatch::Clear() {
  rep_.clear();
  rep_.resize(kHeader);
}

// 遍历WriteBatch中变量,添加到handler, Handler是一个纯虚类,具体实现有MemtableInserter和WriteBatchItemPrinter
Status WriteBatch::Iterate(Handler* handler) const {
  Slice input(rep_);
  if (input.size() < kHeader) {
    return Status::Corruption("malformed WriteBatch (too small)");
  }
  // 跳过头部, 剩下的全是record
  input.remove_prefix(kHeader);
  Slice key, value;
  int found = 0;
  while (!input.empty()) {
    found++;
    // record类型
    char tag = input[0];
    input.remove_prefix(1);
    switch (tag) {
      case kTypeValue:
        // 如果是插入数据, 获取key和value,
        // GetLengthPrefixedSlice是字节处理函数, 详见coding.cc
        if (GetLengthPrefixedSlice(&input, &key) &&
            GetLengthPrefixedSlice(&input, &value)) {
          handler->Put(key, value);
        } else {
          return Status::Corruption("bad WriteBatch Put");
        }
        break;
      case kTypeDeletion:
        // 如果是删除数据, 只需获得key
        if (GetLengthPrefixedSlice(&input, &key)) {
          handler->Delete(key);
        } else {
          return Status::Corruption("bad WriteBatch Delete");
        }
        break;
      default:
        return Status::Corruption("unknown WriteBatch tag");
    }
  }
  // 如果解析的record数不等于count, 报错
  if (found != WriteBatchInternal::Count(this)) {
    return Status::Corruption("WriteBatch has wrong count");
  } else {
    return Status::OK();
  }
}

// 解析有多个少个record, 实则就是解析count, DecodeFixed32是解析32位定长整数, 详见coding.h
int WriteBatchInternal::Count(const WriteBatch* b) {
  return DecodeFixed32(b->rep_.data() + 8);
}

// 设置count, EncodeFixed详见coding.h coding.cc
void WriteBatchInternal::SetCount(WriteBatch* b, int n) {
  EncodeFixed32(&b->rep_[8], n);
}

// 解析首部sequence
SequenceNumber WriteBatchInternal::Sequence(const WriteBatch* b) {
  return SequenceNumber(DecodeFixed64(b->rep_.data()));
}

// 设置首部Sequence
void WriteBatchInternal::SetSequence(WriteBatch* b, SequenceNumber seq) {
  EncodeFixed64(&b->rep_[0], seq);
}

// 添加kv
void WriteBatch::Put(const Slice& key, const Slice& value) {
  // 1. count+1
  // 2. 添加type到rep_尾部
  // 3. 添加key到rep_尾部
  // 4. 添加value到rep_尾部
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeValue));
  PutLengthPrefixedSlice(&rep_, key);
  PutLengthPrefixedSlice(&rep_, value);
}

// 删除key, 参考Put的步骤
void WriteBatch::Delete(const Slice& key) {
  WriteBatchInternal::SetCount(this, WriteBatchInternal::Count(this) + 1);
  rep_.push_back(static_cast<char>(kTypeDeletion));
  PutLengthPrefixedSlice(&rep_, key);
}

namespace {
// MemTableInserter包含一个sequence_和一个MemTable对象
// Put函数就是调用MemTable的Add函数, 然后sequence_++
// Delete函数也是调用MemTable的Add函数, 但是标记为kTypeDeletion, 然后sequence_++
// 注意只有public
class MemTableInserter : public WriteBatch::Handler {
 public:
  SequenceNumber sequence_;
  MemTable* mem_;

  virtual void Put(const Slice& key, const Slice& value) {
    mem_->Add(sequence_, kTypeValue, key, value);
    sequence_++;
  }
  virtual void Delete(const Slice& key) {
    mem_->Add(sequence_, kTypeDeletion, key, Slice());
    sequence_++;
  }
};
}  // namespace

// 将b中record插入到memtable中
Status WriteBatchInternal::InsertInto(const WriteBatch* b,
                                      MemTable* memtable) {
  MemTableInserter inserter;
  inserter.sequence_ = WriteBatchInternal::Sequence(b);
  inserter.mem_ = memtable;
  return b->Iterate(&inserter);
}

// 替换b的内容为contents
void WriteBatchInternal::SetContents(WriteBatch* b, const Slice& contents) {
  assert(contents.size() >= kHeader);
  b->rep_.assign(contents.data(), contents.size());
}

// 将src的record内容追加到dst尾部
void WriteBatchInternal::Append(WriteBatch* dst, const WriteBatch* src) {
  SetCount(dst, Count(dst) + Count(src));
  assert(src->rep_.size() >= kHeader);
  dst->rep_.append(src->rep_.data() + kHeader, src->rep_.size() - kHeader);
}

}  // namespace leveldb
