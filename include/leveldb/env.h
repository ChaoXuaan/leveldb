// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// An Env is an interface used by the leveldb implementation to access
// operating system functionality like the filesystem etc.  Callers
// may wish to provide a custom Env object when opening a database to
// get fine gain control; e.g., to rate limit file system operations.
//
// All Env implementations are safe for concurrent access from
// multiple threads without any external synchronization.

#ifndef STORAGE_LEVELDB_INCLUDE_ENV_H_
#define STORAGE_LEVELDB_INCLUDE_ENV_H_

#include <string>
#include <vector>
#include <stdarg.h>
#include <stdint.h>
#include "leveldb/status.h"

namespace leveldb {

class FileLock;
class Logger;
class RandomAccessFile;
class SequentialFile;
class Slice;
class WritableFile;

/* 环境配置基类, 当前有以下具体实现:
 * PosixEnv, InMemoryEnv, SpecialEnv
 */
class Env {
 public:
  Env() { }
  virtual ~Env();

  // Return a default environment suitable for the current operating
  // system.  Sophisticated users may wish to provide their own Env
  // implementation instead of relying on this default environment.
  //
  // The result of Default() belongs to leveldb and must never be deleted.
  /* 默认环境配置适配当前系统, 具体实现在env_posix.cc文件中 */
  static Env* Default();

  // Create a brand new sequentially-readable file with the specified name.
  // On success, stores a pointer to the new file in *result and returns OK.
  // On failure stores NULL in *result and returns non-OK.  If the file does
  // not exist, returns a non-OK status.
  //
  // The returned file will only be accessed by one thread at a time.
  /* 用指定的名字创建一个全新的顺序可读的文件,
   * 成功: 文件指针放在*result, 返回OK
   * 失败: 置*result为NULL, 返回non-OK
   */
  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) = 0;

  // Create a brand new random access read-only file with the
  // specified name.  On success, stores a pointer to the new file in
  // *result and returns OK.  On failure stores NULL in *result and
  // returns non-OK.  If the file does not exist, returns a non-OK
  // status.
  //
  // The returned file may be concurrently accessed by multiple threads.
  /* 用指定的名称创建一个全新的随机访问只读文件
   * 成功: 同上
   * 失败: 同上
   */
  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) = 0;

  // Create an object that writes to a new file with the specified
  // name.  Deletes any existing file with the same name and creates a
  // new file.  On success, stores a pointer to the new file in
  // *result and returns OK.  On failure stores NULL in *result and
  // returns non-OK.
  //
  // The returned file will only be accessed by one thread at a time.
  /* 创建一个以指定名称写入新文件的对象, 删除所有已存在的同名文件并创建一个新文件
   * 成功: 同上
   * 失败: 同上
   * Note. 一次只能一个线程访问该文件对象
   */
  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) = 0;

  // Create an object that either appends to an existing file, or
  // writes to a new file (if the file does not exist to begin with).
  // On success, stores a pointer to the new file in *result and
  // returns OK.  On failure stores NULL in *result and returns
  // non-OK.
  //
  // The returned file will only be accessed by one thread at a time.
  //
  // May return an IsNotSupportedError error if this Env does
  // not allow appending to an existing file.  Users of Env (including
  // the leveldb implementation) must be prepared to deal with
  // an Env that does not support appending.
  /* 创建一个文件对象, 如果存在则追加内容, 如果不存在则创建新文件写入
   * 成功: 同上
   * 失败: 同上
   * Note: 一次只能一个线程访问该文件对象;
   * 	   如果系统不支持追加文件操作, 则返回IsNotSupportedError
   */
  virtual Status NewAppendableFile(const std::string& fname,
                                   WritableFile** result);

  // Returns true iff the named file exists.
  /* 判断文件是否已经存在 */
  virtual bool FileExists(const std::string& fname) = 0;

  // Store in *result the names of the children of the specified directory.
  // The names are relative to "dir".
  // Original contents of *results are dropped.
  /* 在result中存储dir目录下的子目录和文件名称, 保存的是相对路径 */
  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) = 0;

  // Delete the named file.
  /* 删除文件 */
  virtual Status DeleteFile(const std::string& fname) = 0;

  // Create the specified directory.
  /* 创建目录 */
  virtual Status CreateDir(const std::string& dirname) = 0;

  // Delete the specified directory.
  /* 删除目录 */
  virtual Status DeleteDir(const std::string& dirname) = 0;

  // Store the size of fname in *file_size.
  /* file_size保存指定文件的大小 */
  virtual Status GetFileSize(const std::string& fname, uint64_t* file_size) = 0;

  // Rename file src to target.
  /* 重命名文件 */
  virtual Status RenameFile(const std::string& src,
                            const std::string& target) = 0;

  // Lock the specified file.  Used to prevent concurrent access to
  // the same db by multiple processes.  On failure, stores NULL in
  // *lock and returns non-OK.
  //
  // On success, stores a pointer to the object that represents the
  // acquired lock in *lock and returns OK.  The caller should call
  // UnlockFile(*lock) to release the lock.  If the process exits,
  // the lock will be automatically released.
  //
  // If somebody else already holds the lock, finishes immediately
  // with a failure.  I.e., this call does not wait for existing locks
  // to go away.
  //
  // May create the named file if it does not already exist.
  /* 锁定文件, *lock中存放的对象表示fname指定的文件已被锁定, FileLock仅仅是一个标记,
   * 并未用到真正的锁,
   * 如果文件已被其他线程锁定, lock=NULL;
   * 如果文件不存在, 则会新建文件
   */
  virtual Status LockFile(const std::string& fname, FileLock** lock) = 0;

  // Release the lock acquired by a previous successful call to LockFile.
  // REQUIRES: lock was returned by a successful LockFile() call
  // REQUIRES: lock has not already been unlocked.
  /* 释放文件锁
   * 要求: 1. LockFile之前调用成功
   * 	   2. 没有调用过UnlockFile
   */
  virtual Status UnlockFile(FileLock* lock) = 0;

  // Arrange to run "(*function)(arg)" once in a background thread.
  //
  // "function" may run in an unspecified thread.  Multiple functions
  // added to the same Env may run concurrently in different threads.
  // I.e., the caller may not assume that background work items are
  // serialized.
  /* 后台任务, 由一个线程执行,
   * @function 函数指针, 参数是void*
   * @arg 函数参数
   */
  virtual void Schedule(
      void (*function)(void* arg),
      void* arg) = 0;

  // Start a new thread, invoking "function(arg)" within the new thread.
  // When "function(arg)" returns, the thread will be destroyed.
  /* 创建一个线程 */
  virtual void StartThread(void (*function)(void* arg), void* arg) = 0;

  // *path is set to a temporary directory that can be used for testing. It may
  // or many not have just been created. The directory may or may not differ
  // between runs of the same process, but subsequent calls will return the
  // same directory.
  /* 获得测试目录 */
  virtual Status GetTestDirectory(std::string* path) = 0;

  // Create and return a log file for storing informational messages.
  /* 创建新的日志文件, result存储日志对象 */
  virtual Status NewLogger(const std::string& fname, Logger** result) = 0;

  // Returns the number of micro-seconds since some fixed point in time. Only
  // useful for computing deltas of time.
  /* 当前时间, 以微秒显示 */
  virtual uint64_t NowMicros() = 0;

  // Sleep/delay the thread for the prescribed number of micro-seconds.
  /* sleep, 单位微秒 */
  virtual void SleepForMicroseconds(int micros) = 0;

 private:
  // No copying allowed
  Env(const Env&);
  void operator=(const Env&);
};

// A file abstraction for reading sequentially through a file
class SequentialFile {
 public:
  SequentialFile() { }
  virtual ~SequentialFile();

  // Read up to "n" bytes from the file.  "scratch[0..n-1]" may be
  // written by this routine.  Sets "*result" to the data that was
  // read (including if fewer than "n" bytes were successfully read).
  // May set "*result" to point at data in "scratch[0..n-1]", so
  // "scratch[0..n-1]" must be live when "*result" is used.
  // If an error was encountered, returns a non-OK status.
  //
  // REQUIRES: External synchronization
  virtual Status Read(size_t n, Slice* result, char* scratch) = 0;

  // Skip "n" bytes from the file. This is guaranteed to be no
  // slower that reading the same data, but may be faster.
  //
  // If end of file is reached, skipping will stop at the end of the
  // file, and Skip will return OK.
  //
  // REQUIRES: External synchronization
  virtual Status Skip(uint64_t n) = 0;

 private:
  // No copying allowed
  SequentialFile(const SequentialFile&);
  void operator=(const SequentialFile&);
};

// A file abstraction for randomly reading the contents of a file.
class RandomAccessFile {
 public:
  RandomAccessFile() { }
  virtual ~RandomAccessFile();

  // Read up to "n" bytes from the file starting at "offset".
  // "scratch[0..n-1]" may be written by this routine.  Sets "*result"
  // to the data that was read (including if fewer than "n" bytes were
  // successfully read).  May set "*result" to point at data in
  // "scratch[0..n-1]", so "scratch[0..n-1]" must be live when
  // "*result" is used.  If an error was encountered, returns a non-OK
  // status.
  //
  // Safe for concurrent use by multiple threads.
  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const = 0;

 private:
  // No copying allowed
  RandomAccessFile(const RandomAccessFile&);
  void operator=(const RandomAccessFile&);
};

// A file abstraction for sequential writing.  The implementation
// must provide buffering since callers may append small fragments
// at a time to the file.
class WritableFile {
 public:
  WritableFile() { }
  virtual ~WritableFile();

  virtual Status Append(const Slice& data) = 0;
  virtual Status Close() = 0;
  virtual Status Flush() = 0;
  virtual Status Sync() = 0;

 private:
  // No copying allowed
  WritableFile(const WritableFile&);
  void operator=(const WritableFile&);
};

// An interface for writing log messages.
class Logger {
 public:
  Logger() { }
  virtual ~Logger();

  // Write an entry to the log file with the specified format.
  virtual void Logv(const char* format, va_list ap) = 0;

 private:
  // No copying allowed
  Logger(const Logger&);
  void operator=(const Logger&);
};


// Identifies a locked file.
class FileLock {
 public:
  FileLock() { }
  virtual ~FileLock();
 private:
  // No copying allowed
  FileLock(const FileLock&);
  void operator=(const FileLock&);
};

// Log the specified data to *info_log if info_log is non-NULL.
extern void Log(Logger* info_log, const char* format, ...)
#   if defined(__GNUC__) || defined(__clang__)
    __attribute__((__format__ (__printf__, 2, 3)))
#   endif
    ;

// A utility routine: write "data" to the named file.
extern Status WriteStringToFile(Env* env, const Slice& data,
                                const std::string& fname);

// A utility routine: read contents of named file into *data
extern Status ReadFileToString(Env* env, const std::string& fname,
                               std::string* data);

// An implementation of Env that forwards all calls to another Env.
// May be useful to clients who wish to override just part of the
// functionality of another Env.
class EnvWrapper : public Env {
 public:
  // Initialize an EnvWrapper that delegates all calls to *t
  explicit EnvWrapper(Env* t) : target_(t) { }
  virtual ~EnvWrapper();

  // Return the target to which this Env forwards all calls
  Env* target() const { return target_; }

  // The following text is boilerplate that forwards all methods to target()
  Status NewSequentialFile(const std::string& f, SequentialFile** r) {
    return target_->NewSequentialFile(f, r);
  }
  Status NewRandomAccessFile(const std::string& f, RandomAccessFile** r) {
    return target_->NewRandomAccessFile(f, r);
  }
  Status NewWritableFile(const std::string& f, WritableFile** r) {
    return target_->NewWritableFile(f, r);
  }
  Status NewAppendableFile(const std::string& f, WritableFile** r) {
    return target_->NewAppendableFile(f, r);
  }
  bool FileExists(const std::string& f) { return target_->FileExists(f); }
  Status GetChildren(const std::string& dir, std::vector<std::string>* r) {
    return target_->GetChildren(dir, r);
  }
  Status DeleteFile(const std::string& f) { return target_->DeleteFile(f); }
  Status CreateDir(const std::string& d) { return target_->CreateDir(d); }
  Status DeleteDir(const std::string& d) { return target_->DeleteDir(d); }
  Status GetFileSize(const std::string& f, uint64_t* s) {
    return target_->GetFileSize(f, s);
  }
  Status RenameFile(const std::string& s, const std::string& t) {
    return target_->RenameFile(s, t);
  }
  Status LockFile(const std::string& f, FileLock** l) {
    return target_->LockFile(f, l);
  }
  Status UnlockFile(FileLock* l) { return target_->UnlockFile(l); }
  void Schedule(void (*f)(void*), void* a) {
    return target_->Schedule(f, a);
  }
  void StartThread(void (*f)(void*), void* a) {
    return target_->StartThread(f, a);
  }
  virtual Status GetTestDirectory(std::string* path) {
    return target_->GetTestDirectory(path);
  }
  virtual Status NewLogger(const std::string& fname, Logger** result) {
    return target_->NewLogger(fname, result);
  }
  uint64_t NowMicros() {
    return target_->NowMicros();
  }
  void SleepForMicroseconds(int micros) {
    target_->SleepForMicroseconds(micros);
  }
 private:
  Env* target_;
};

}  // namespace leveldb

#endif  // STORAGE_LEVELDB_INCLUDE_ENV_H_
