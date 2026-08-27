// The SPSC ring, placed in a POSIX shared-memory segment.
//
// Why a separate process rather than a Node native addon: the whole point of
// moving this path to C++ is to get it out from under V8's garbage collector.
// An N-API addon runs inside the same process, so a GC pause still stalls the
// thread feeding frames in, and the jitter you set out to remove is still there
// -- you just moved where it is measured. A separate process with shared memory
// between them isolates the hot path completely. It is also, not incidentally,
// the same shape as a market-data feed handler.
//
// Everything is mapped once, touched once (MAP_POPULATE) and pinned where
// permitted (mlock). A major page fault on the consumer loop is a multi-hundred
// microsecond event and would dominate the tail.
#pragma once

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <new>
#include <stdexcept>
#include <string>

#include "vhp/spsc_ring.hpp"

namespace vhp {

inline constexpr std::uint64_t kShmMagic = 0x5648'5052'494E'4701ULL;  // "VHPRING\1"
inline constexpr std::uint32_t kShmVersion = 1;

template <typename T, std::size_t Capacity>
struct ShmLayout {
  std::uint64_t magic;
  std::uint32_t version;
  std::uint32_t slot_bytes;
  std::uint64_t capacity;
  std::uint64_t payload_bytes;
  std::uint64_t reserved[4];
  alignas(kCacheLine) SpscRing<T, Capacity, true> ring;
};

template <typename T, std::size_t Capacity>
class ShmRing {
 public:
  using Layout = ShmLayout<T, Capacity>;

  // Creator: makes the segment, zeroes it, constructs the ring in place.
  static ShmRing create(const std::string& name, bool unlink_on_close = true) {
    ::shm_unlink(name.c_str());  // stale segment from a crashed run
    const int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) throw_errno("shm_open(create) " + name);
    if (::ftruncate(fd, static_cast<off_t>(sizeof(Layout))) != 0) {
      ::close(fd);
      ::shm_unlink(name.c_str());
      throw_errno("ftruncate " + name);
    }
    void* addr = map(fd, sizeof(Layout));
    ::close(fd);

    std::memset(addr, 0, sizeof(Layout));
    auto* layout = new (addr) Layout{};
    layout->magic = kShmMagic;
    layout->version = kShmVersion;
    layout->slot_bytes = static_cast<std::uint32_t>(sizeof(T));
    layout->capacity = Capacity;
    layout->payload_bytes = sizeof(T);
    return ShmRing(name, layout, unlink_on_close);
  }

  // Attacher: validates the header before trusting a single index out of it.
  // A layout mismatch between the two binaries is silent memory corruption
  // otherwise, and it is exactly the failure you get after changing a struct on
  // one side only.
  static ShmRing attach(const std::string& name) {
    const int fd = ::shm_open(name.c_str(), O_RDWR, 0600);
    if (fd < 0) throw_errno("shm_open(attach) " + name);
    struct stat st {};
    if (::fstat(fd, &st) != 0) { ::close(fd); throw_errno("fstat " + name); }
    if (static_cast<std::size_t>(st.st_size) < sizeof(Layout)) {
      ::close(fd);
      throw std::runtime_error("shm segment " + name + " is smaller than the expected layout");
    }
    void* addr = map(fd, sizeof(Layout));
    ::close(fd);

    auto* layout = static_cast<Layout*>(addr);
    if (layout->magic != kShmMagic) {
      ::munmap(addr, sizeof(Layout));
      throw std::runtime_error("shm segment " + name + " has a bad magic -- not a vhp ring");
    }
    if (layout->version != kShmVersion || layout->slot_bytes != sizeof(T) ||
        layout->capacity != Capacity) {
      ::munmap(addr, sizeof(Layout));
      throw std::runtime_error("shm segment " + name +
                               " layout mismatch: producer and consumer were built "
                               "against different headers");
    }
    return ShmRing(name, layout, /*unlink_on_close=*/false);
  }

  ShmRing(ShmRing&& other) noexcept
      : name_(std::move(other.name_)), layout_(other.layout_), unlink_(other.unlink_) {
    other.layout_ = nullptr;
  }
  ShmRing& operator=(ShmRing&&) = delete;
  ShmRing(const ShmRing&) = delete;
  ShmRing& operator=(const ShmRing&) = delete;

  ~ShmRing() {
    if (!layout_) return;
    ::munmap(layout_, sizeof(Layout));
    if (unlink_) ::shm_unlink(name_.c_str());
  }

  SpscRing<T, Capacity, true>& ring() noexcept { return layout_->ring; }
  const SpscRing<T, Capacity, true>& ring() const noexcept { return layout_->ring; }
  const std::string& name() const noexcept { return name_; }

 private:
  ShmRing(std::string name, Layout* layout, bool unlink)
      : name_(std::move(name)), layout_(layout), unlink_(unlink) {}

  static void* map(int fd, std::size_t bytes) {
    void* addr = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, fd, 0);
    if (addr == MAP_FAILED) { ::close(fd); throw_errno("mmap"); }
    // Best effort: without CAP_IPC_LOCK / a raised RLIMIT_MEMLOCK this fails,
    // and that is fine -- MAP_POPULATE has already faulted the pages in. It
    // matters under memory pressure, where they could otherwise be reclaimed.
    ::mlock(addr, bytes);
    return addr;
  }

  [[noreturn]] static void throw_errno(const std::string& what) {
    throw std::runtime_error(what + ": " + std::strerror(errno));
  }

  std::string name_;
  Layout* layout_ = nullptr;
  bool unlink_ = false;
};

}  // namespace vhp
