// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <deque>
#include <limits>
#include <set>
#include "leveldb/env.h"
#include "leveldb/slice.h"
#include "port/port.h"
#include "util/logging.h"
#include "util/mutexlock.h"
#include "util/posix_logger.h"
#include "util/env_posix_test_helper.h"

namespace leveldb {

namespace {

static int open_read_only_file_limit = -1;
static int mmap_limit = -1;

static Status IOError(const std::string& context, int err_number) {
  return Status::IOError(context, strerror(err_number));
}

// Helper class to limit resource usage to avoid exhaustion.
// Currently used to limit read-only file descriptors and mmap file usage
// so that we do not end up running out of file descriptors, virtual memory,
// or running into kernel performance problems for very large databases.
/* 辅助类, 用于限制资源使用以免资源耗尽
 * 目前只用来限制只读文件的文件描述符个数和mmap文件，
 * 避免最终耗尽文件描述符、虚拟内存，或者运行到大型数据库的内核性能问题。
 */
class Limiter {
 public:
  // Limit maximum number of resources to |n|.
  /* 资源最大值 n */
  Limiter(intptr_t n) {
    SetAllowed(n);
  }

  // If another resource is available, acquire it and return true.
  // Else return false.
  /* 请求一个资源, 成功返回true, 失败返回false */
  bool Acquire() {
    if (GetAllowed() <= 0) {
      return false;
    }
    MutexLock l(&mu_);
    intptr_t x = GetAllowed();
    if (x <= 0) {
      return false;
    } else {
      SetAllowed(x - 1);
      return true;
    }
  }

  // Release a resource acquired by a previous call to Acquire() that returned
  // true.
  // 释放资源
  void Release() {
    MutexLock l(&mu_);
    SetAllowed(GetAllowed() + 1);
  }

 private:
  /* 互斥量 */
  port::Mutex mu_;
  /* 原子指针 */
  port::AtomicPointer allowed_;

  /* 获得之前保存的值 */
  intptr_t GetAllowed() const {
    return reinterpret_cast<intptr_t>(allowed_.Acquire_Load());
  }

  // REQUIRES: mu_ must be held
  /* 保存v */
  void SetAllowed(intptr_t v) {
    allowed_.Release_Store(reinterpret_cast<void*>(v));
  }

  Limiter(const Limiter&);
  void operator=(const Limiter&);
};

class PosixSequentialFile: public SequentialFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  PosixSequentialFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) { }
  virtual ~PosixSequentialFile() { fclose(file_); }

  virtual Status Read(size_t n, Slice* result, char* scratch) {
    Status s;
    size_t r = fread_unlocked(scratch, 1, n, file_);
    *result = Slice(scratch, r);
    if (r < n) {
      if (feof(file_)) {
        // We leave status as ok if we hit the end of the file
      } else {
        // A partial read with an error: return a non-ok status
        s = IOError(filename_, errno);
      }
    }
    return s;
  }

  virtual Status Skip(uint64_t n) {
    if (fseek(file_, n, SEEK_CUR)) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }
};

// pread() based random-access
class PosixRandomAccessFile: public RandomAccessFile {
 private:
  std::string filename_;
  bool temporary_fd_;  // If true, fd_ is -1 and we open on every read.
  int fd_;
  Limiter* limiter_;

 public:
  PosixRandomAccessFile(const std::string& fname, int fd, Limiter* limiter)
      : filename_(fname), fd_(fd), limiter_(limiter) {
    temporary_fd_ = !limiter->Acquire();
    if (temporary_fd_) {
      // Open file on every access.
      close(fd_);
      fd_ = -1;
    }
  }

  virtual ~PosixRandomAccessFile() {
    if (!temporary_fd_) {
      close(fd_);
      limiter_->Release();
    }
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    int fd = fd_;
    if (temporary_fd_) {
      fd = open(filename_.c_str(), O_RDONLY);
      if (fd < 0) {
        return IOError(filename_, errno);
      }
    }

    Status s;
    ssize_t r = pread(fd, scratch, n, static_cast<off_t>(offset));
    *result = Slice(scratch, (r < 0) ? 0 : r);
    if (r < 0) {
      // An error: return a non-ok status
      s = IOError(filename_, errno);
    }
    if (temporary_fd_) {
      // Close the temporary file descriptor opened earlier.
      close(fd);
    }
    return s;
  }
};

// mmap() based random-access
class PosixMmapReadableFile: public RandomAccessFile {
 private:
  std::string filename_;
  void* mmapped_region_;
  size_t length_;
  Limiter* limiter_;

 public:
  // base[0,length-1] contains the mmapped contents of the file.
  PosixMmapReadableFile(const std::string& fname, void* base, size_t length,
                        Limiter* limiter)
      : filename_(fname), mmapped_region_(base), length_(length),
        limiter_(limiter) {
  }

  virtual ~PosixMmapReadableFile() {
    munmap(mmapped_region_, length_);
    limiter_->Release();
  }

  virtual Status Read(uint64_t offset, size_t n, Slice* result,
                      char* scratch) const {
    Status s;
    if (offset + n > length_) {
      *result = Slice();
      s = IOError(filename_, EINVAL);
    } else {
      *result = Slice(reinterpret_cast<char*>(mmapped_region_) + offset, n);
    }
    return s;
  }
};

class PosixWritableFile : public WritableFile {
 private:
  std::string filename_;
  FILE* file_;

 public:
  PosixWritableFile(const std::string& fname, FILE* f)
      : filename_(fname), file_(f) { }

  ~PosixWritableFile() {
    if (file_ != NULL) {
      // Ignoring any potential errors
      fclose(file_);
    }
  }

  virtual Status Append(const Slice& data) {
    size_t r = fwrite_unlocked(data.data(), 1, data.size(), file_);
    if (r != data.size()) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }

  virtual Status Close() {
    Status result;
    if (fclose(file_) != 0) {
      result = IOError(filename_, errno);
    }
    file_ = NULL;
    return result;
  }

  virtual Status Flush() {
    if (fflush_unlocked(file_) != 0) {
      return IOError(filename_, errno);
    }
    return Status::OK();
  }

  Status SyncDirIfManifest() {
    const char* f = filename_.c_str();
    const char* sep = strrchr(f, '/');
    Slice basename;
    std::string dir;
    if (sep == NULL) {
      dir = ".";
      basename = f;
    } else {
      dir = std::string(f, sep - f);
      basename = sep + 1;
    }
    Status s;
    if (basename.starts_with("MANIFEST")) {
      int fd = open(dir.c_str(), O_RDONLY);
      if (fd < 0) {
        s = IOError(dir, errno);
      } else {
        if (fsync(fd) < 0) {
          s = IOError(dir, errno);
        }
        close(fd);
      }
    }
    return s;
  }

  virtual Status Sync() {
    // Ensure new files referred to by the manifest are in the filesystem.
    Status s = SyncDirIfManifest();
    if (!s.ok()) {
      return s;
    }
    if (fflush_unlocked(file_) != 0 ||
        fdatasync(fileno(file_)) != 0) {
      s = Status::IOError(filename_, strerror(errno));
    }
    return s;
  }
};

/* 设置获取记录锁 */
static int LockOrUnlock(int fd, bool lock) {
  errno = 0;
  struct flock f;
  memset(&f, 0, sizeof(f));
  /* lock为true加写锁, 否则解锁 */
  f.l_type = (lock ? F_WRLCK : F_UNLCK);
  f.l_whence = SEEK_SET;
  f.l_start = 0;
  f.l_len = 0;        // Lock/unlock entire file
  return fcntl(fd, F_SETLK, &f);
}

/* 继承于FileLock, 包含两个变量: 文件描述符和文件名 */
class PosixFileLock : public FileLock {
 public:
  int fd_;
  std::string name_;
};

// Set of locked files.  We keep a separate set instead of just
// relying on fcntrl(F_SETLK) since fcntl(F_SETLK) does not provide
// any protection against multiple uses from the same process.
/* FileLock table, 用set存储已经被锁定的文件 */
class PosixLockTable {
 private:
  port::Mutex mu_;
  std::set<std::string> locked_files_;
 public:
  bool Insert(const std::string& fname) {
    MutexLock l(&mu_);
    return locked_files_.insert(fname).second;
  }
  void Remove(const std::string& fname) {
    MutexLock l(&mu_);
    locked_files_.erase(fname);
  }
};

class PosixEnv : public Env {
 public:
  PosixEnv();
  virtual ~PosixEnv() {
    char msg[] = "Destroying Env::Default()\n";
    fwrite(msg, 1, sizeof(msg), stderr);
    abort();
  }

  virtual Status NewSequentialFile(const std::string& fname,
                                   SequentialFile** result) {
    /* 只读方式打开文件 */
    FILE* f = fopen(fname.c_str(), "r");
    if (f == NULL) {
      *result = NULL;
      return IOError(fname, errno);
    } else {
      /* 打开成功, 创建 PosixSequentialFile */
      *result = new PosixSequentialFile(fname, f);
      return Status::OK();
    }
  }

  virtual Status NewRandomAccessFile(const std::string& fname,
                                     RandomAccessFile** result) {
    *result = NULL;
    Status s;
    /* 打开文件, 通过系统调用只读打开, 返回文件描述符 */
    int fd = open(fname.c_str(), O_RDONLY);
    if (fd < 0) {
      s = IOError(fname, errno);
    } else if (mmap_limit_.Acquire()) {
      /* 首先使用mmap */
      uint64_t size;
      s = GetFileSize(fname, &size);
      if (s.ok()) {
        void* base = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
        if (base != MAP_FAILED) {
          *result = new PosixMmapReadableFile(fname, base, size, &mmap_limit_);
        } else {
          s = IOError(fname, errno);
        }
      }
      close(fd);
      if (!s.ok()) {
	/* 如果 s.ok == true, 资源释放在result调用析构函数时 */
        mmap_limit_.Release();
      }
    } else {
      /* 如果mmap使用完, 则消耗fd */
      *result = new PosixRandomAccessFile(fname, fd, &fd_limit_);
    }
    return s;
  }

  virtual Status NewWritableFile(const std::string& fname,
                                 WritableFile** result) {
    Status s;
    /* 写方式打开文件 */
    FILE* f = fopen(fname.c_str(), "w");
    if (f == NULL) {
      *result = NULL;
      s = IOError(fname, errno);
    } else {
      /* 创建PosixWritableFile */
      *result = new PosixWritableFile(fname, f);
    }
    return s;
  }

  virtual Status NewAppendableFile(const std::string& fname,
                                   WritableFile** result) {
    Status s;
    /* 追加方式打开文件 */
    FILE* f = fopen(fname.c_str(), "a");
    if (f == NULL) {
      *result = NULL;
      s = IOError(fname, errno);
    } else {
      /* 创建PosixWritableFile */
      *result = new PosixWritableFile(fname, f);
    }
    return s;
  }

  virtual bool FileExists(const std::string& fname) {
    return access(fname.c_str(), F_OK) == 0;
  }

  virtual Status GetChildren(const std::string& dir,
                             std::vector<std::string>* result) {
    result->clear();
    DIR* d = opendir(dir.c_str());
    if (d == NULL) {
      return IOError(dir, errno);
    }
    struct dirent* entry;
    while ((entry = readdir(d)) != NULL) {
      result->push_back(entry->d_name);
    }
    closedir(d);
    return Status::OK();
  }

  virtual Status DeleteFile(const std::string& fname) {
    Status result;
    if (unlink(fname.c_str()) != 0) {
      result = IOError(fname, errno);
    }
    return result;
  }

  virtual Status CreateDir(const std::string& name) {
    Status result;
    if (mkdir(name.c_str(), 0755) != 0) {
      result = IOError(name, errno);
    }
    return result;
  }

  virtual Status DeleteDir(const std::string& name) {
    Status result;
    if (rmdir(name.c_str()) != 0) {
      result = IOError(name, errno);
    }
    return result;
  }

  /* 获得文件大小, 单位byte */
  virtual Status GetFileSize(const std::string& fname, uint64_t* size) {
    Status s;
    struct stat sbuf;
    if (stat(fname.c_str(), &sbuf) != 0) {
      *size = 0;
      s = IOError(fname, errno);
    } else {
      *size = sbuf.st_size;
    }
    return s;
  }

  virtual Status RenameFile(const std::string& src, const std::string& target) {
    Status result;
    if (rename(src.c_str(), target.c_str()) != 0) {
      result = IOError(src, errno);
    }
    return result;
  }

  /* 锁定文件 */
  virtual Status LockFile(const std::string& fname, FileLock** lock) {
    *lock = NULL;
    Status result;
    /* 打开文件, 不存在则创建 */
    int fd = open(fname.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
      result = IOError(fname, errno);
    } else if (!locks_.Insert(fname)) {
      /* 插入到PsixLockTable中, 插入失败说明已经有线程在使用 */
      close(fd);
      result = Status::IOError("lock " + fname, "already held by process");
    } else if (LockOrUnlock(fd, true) == -1) {
      /* 设置获取记录锁 */
      result = IOError("lock " + fname, errno);
      close(fd);
      locks_.Remove(fname);
    } else {
      /* 没有被其他线程锁定 */
      PosixFileLock* my_lock = new PosixFileLock;
      my_lock->fd_ = fd;
      my_lock->name_ = fname;
      *lock = my_lock;
    }
    return result;
  }

  /* 解锁文件 */
  virtual Status UnlockFile(FileLock* lock) {
    PosixFileLock* my_lock = reinterpret_cast<PosixFileLock*>(lock);
    Status result;
    if (LockOrUnlock(my_lock->fd_, false) == -1) {
      /* 设置获取记录锁, 传入flase, fcntl释放锁 */
      result = IOError("unlock", errno);
    }
    /* 从PosixLockTable中删除文件名表示没有被锁 */
    locks_.Remove(my_lock->name_);
    close(my_lock->fd_);
    delete my_lock;
    return result;
  }

  virtual void Schedule(void (*function)(void*), void* arg);

  virtual void StartThread(void (*function)(void* arg), void* arg);

  virtual Status GetTestDirectory(std::string* result) {
    const char* env = getenv("TEST_TMPDIR");
    if (env && env[0] != '\0') {
      *result = env;
    } else {
      char buf[100];
      snprintf(buf, sizeof(buf), "/tmp/leveldbtest-%d", int(geteuid()));
      *result = buf;
    }
    // Directory may already exist
    CreateDir(*result);
    return Status::OK();
  }

  /* 获得线程id */
  static uint64_t gettid() {
    pthread_t tid = pthread_self();
    uint64_t thread_id = 0;
    memcpy(&thread_id, &tid, std::min(sizeof(thread_id), sizeof(tid)));
    return thread_id;
  }

  virtual Status NewLogger(const std::string& fname, Logger** result) {
    /* 创建PosixLogger */
    FILE* f = fopen(fname.c_str(), "w");
    if (f == NULL) {
      *result = NULL;
      return IOError(fname, errno);
    } else {
      *result = new PosixLogger(f, &PosixEnv::gettid);
      return Status::OK();
    }
  }

  virtual uint64_t NowMicros() {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return static_cast<uint64_t>(tv.tv_sec) * 1000000 + tv.tv_usec;
  }

  virtual void SleepForMicroseconds(int micros) {
    usleep(micros);
  }

 private:
  /* thread错误输出 */
  void PthreadCall(const char* label, int result) {
    if (result != 0) {
      fprintf(stderr, "pthread %s: %s\n", label, strerror(result));
      abort();
    }
  }

  // BGThread() is the body of the background thread
  void BGThread();
  /* BGThread封装 */
  static void* BGThreadWrapper(void* arg) {
    reinterpret_cast<PosixEnv*>(arg)->BGThread();
    return NULL;
  }

  /* 互斥量 */
  pthread_mutex_t mu_;
  /* 条件变量 */
  pthread_cond_t bgsignal_;
  /* 线程 */
  pthread_t bgthread_;
  /* 后台工作线程是否已经创建 */
  bool started_bgthread_;

  // Entry per Schedule() call
  /* 后台任务封装 */
  struct BGItem { void* arg; void (*function)(void*); };
  /* 任务队列 */
  typedef std::deque<BGItem> BGQueue;
  BGQueue queue_;

  /* 记录需要锁定的文件 */
  PosixLockTable locks_;
  /* mmap限制 */
  Limiter mmap_limit_;
  /* fd限制 */
  Limiter fd_limit_;
};

// Return the maximum number of concurrent mmaps.
/* 最大mmaps, 64位下最多1000, 低于64位0个 */
static int MaxMmaps() {
  if (mmap_limit >= 0) {
    return mmap_limit;
  }
  // Up to 1000 mmaps for 64-bit binaries; none for smaller pointer sizes.
  mmap_limit = sizeof(void*) >= 8 ? 1000 : 0;
  return mmap_limit;
}

// Return the maximum number of read-only files to keep open.
/* 返回只读文件的最大数量以保持打开状态 */
static intptr_t MaxOpenFiles() {
  if (open_read_only_file_limit >= 0) {
    return open_read_only_file_limit;
  }
  struct rlimit rlim;
  /* 获得进程能打开的最多文件数 */
  if (getrlimit(RLIMIT_NOFILE, &rlim)) {
    /* 调用失败 */
    // getrlimit failed, fallback to hard-coded default.
    open_read_only_file_limit = 50;
  } else if (rlim.rlim_cur == RLIM_INFINITY) {
    /* 无限制 */
    open_read_only_file_limit = std::numeric_limits<int>::max();
  } else {
    // Allow use of 20% of available file descriptors for read-only files.
    /* 一般只允许系统限制的20% */
    open_read_only_file_limit = rlim.rlim_cur / 5;
  }
  return open_read_only_file_limit;
}

/* 构造函数, 初始化变量 */
PosixEnv::PosixEnv()
    : started_bgthread_(false),
      mmap_limit_(MaxMmaps()),
      fd_limit_(MaxOpenFiles()) {
  PthreadCall("mutex_init", pthread_mutex_init(&mu_, NULL));
  PthreadCall("cvar_init", pthread_cond_init(&bgsignal_, NULL));
}

/* 任务调度 */
void PosixEnv::Schedule(void (*function)(void*), void* arg) {
  /* 加锁 */
  PthreadCall("lock", pthread_mutex_lock(&mu_));

  // Start background thread if necessary
  /* 创建后台线程执行任务 */
  if (!started_bgthread_) {
    started_bgthread_ = true;
    PthreadCall(
        "create thread",
	/* 创建线程, 底层实际调用是BGThread */
        pthread_create(&bgthread_, NULL,  &PosixEnv::BGThreadWrapper, this));
  }

  // If the queue is currently empty, the background thread may currently be
  // waiting.
  /* 如果是空, 唤醒后台线程 */
  if (queue_.empty()) {
    PthreadCall("signal", pthread_cond_signal(&bgsignal_));
  }

  // Add to priority queue
  queue_.push_back(BGItem());
  queue_.back().function = function;
  queue_.back().arg = arg;

  /* 解锁 */
  PthreadCall("unlock", pthread_mutex_unlock(&mu_));
}

/* 后台调度线程, 当调用Schedule时使用 */
void PosixEnv::BGThread() {
  /* 死循环 */
  while (true) {
    // Wait until there is an item that is ready to run
    /* 加锁 */
    PthreadCall("lock", pthread_mutex_lock(&mu_));
    /* 如果任务队列空则等待 */
    while (queue_.empty()) {
      PthreadCall("wait", pthread_cond_wait(&bgsignal_, &mu_));
    }

    /* 获取队列头对象的函数指针 */
    void (*function)(void*) = queue_.front().function;
    /* 获取队列头对象的函数参数 */
    void* arg = queue_.front().arg;
    /* 出队 */
    queue_.pop_front();
    /* 解锁 */
    PthreadCall("unlock", pthread_mutex_unlock(&mu_));
    /* 函数执行 */
    (*function)(arg);
  }
}

namespace {
struct StartThreadState {
  void (*user_function)(void*);
  void* arg;
};
}
static void* StartThreadWrapper(void* arg) {
  StartThreadState* state = reinterpret_cast<StartThreadState*>(arg);
  state->user_function(state->arg);
  delete state;
  return NULL;
}

/* 启动线程 */
void PosixEnv::StartThread(void (*function)(void* arg), void* arg) {
  pthread_t t;
  StartThreadState* state = new StartThreadState;
  state->user_function = function;
  state->arg = arg;
  PthreadCall("start thread",
              pthread_create(&t, NULL,  &StartThreadWrapper, state));
}

}  // namespace

static pthread_once_t once = PTHREAD_ONCE_INIT;
static Env* default_env;
static void InitDefaultEnv() { default_env = new PosixEnv; }

void EnvPosixTestHelper::SetReadOnlyFDLimit(int limit) {
  assert(default_env == NULL);
  open_read_only_file_limit = limit;
}

void EnvPosixTestHelper::SetReadOnlyMMapLimit(int limit) {
  assert(default_env == NULL);
  mmap_limit = limit;
}

/* 默认使用PosixEnv */
Env* Env::Default() {
  pthread_once(&once, InitDefaultEnv);
  return default_env;
}

}  // namespace leveldb
