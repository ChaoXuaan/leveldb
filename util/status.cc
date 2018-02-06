// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <stdio.h>
#include "port/port.h"
#include "leveldb/status.h"

namespace leveldb {

// 拷贝state
const char* Status::CopyState(const char* state) {
  uint32_t size;
  // 1. 获取state的长度, 前四个字节记录了长度
  memcpy(&size, state, sizeof(size));
  // 2. size+5因为前5个字节记录了长度和错误码, 可以看出长度不包含固定头部长度，只包含信息长度
  char* result = new char[size + 5];
  // 3. 复制state内容
  memcpy(result, state, size + 5);
  return result;
}

// 私有构造函数
// @code 错误码
// @msg 错误信息的前半部分
// @msg2 错误信息的后半部分, 与前半部分用": "隔开
Status::Status(Code code, const Slice& msg, const Slice& msg2) {
  assert(code != kOk);
  const uint32_t len1 = msg.size();
  const uint32_t len2 = msg2.size();
  // size之所以+2, 是因为msg与msg2用": "隔开
  const uint32_t size = len1 + (len2 ? (2 + len2) : 0);
  char* result = new char[size + 5];
  // 1. 保存长度
  memcpy(result, &size, sizeof(size));
  // 2. 保存错误码
  result[4] = static_cast<char>(code);
  // 3. 保存msg
  memcpy(result + 5, msg.data(), len1);
  // 4. 如果有后半部分, 保存msg2
  if (len2) {
    result[5 + len1] = ':';
    result[6 + len1] = ' ';
    memcpy(result + 7 + len1, msg2.data(), len2);
  }
  state_ = result;
}

// 生成string
std::string Status::ToString() const {
  if (state_ == NULL) {
    return "OK";
  } else {
    char tmp[30];
    const char* type;
    switch (code()) {
      case kOk:
        type = "OK";
        break;
      case kNotFound:
        type = "NotFound: ";
        break;
      case kCorruption:
        type = "Corruption: ";
        break;
      case kNotSupported:
        type = "Not implemented: ";
        break;
      case kInvalidArgument:
        type = "Invalid argument: ";
        break;
      case kIOError:
        type = "IO error: ";
        break;
      default:
        snprintf(tmp, sizeof(tmp), "Unknown code(%d): ",
                 static_cast<int>(code()));
        type = tmp;
        break;
    }
    std::string result(type);
    uint32_t length;
    memcpy(&length, state_, sizeof(length));
    result.append(state_ + 5, length);
    return result;
  }
}

}  // namespace leveldb
