// SPDX-License-Identifier: Apache-2.0
#include "allocation_failure.h"

#include <cstdlib>
#include <new>
#ifdef _MSC_VER
#include <malloc.h>
#endif

namespace {
thread_local int allocations_before_failure = -1;
thread_local bool allocation_failed = false;
thread_local bool persistent_failure = false;

void CheckAllocation() {
  if (allocations_before_failure < 0) {
    return;
  }
  if (allocations_before_failure-- == 0) {
    allocations_before_failure = persistent_failure ? 0 : -1;
    allocation_failed = true;
    throw std::bad_alloc();
  }
}

}  // namespace

namespace talon_test {

FailAllocation::FailAllocation(int index, bool persistent) noexcept {
  allocation_failed = false;
  allocations_before_failure = index;
  persistent_failure = persistent;
}

FailAllocation::~FailAllocation() {
  allocations_before_failure = -1;
  persistent_failure = false;
}

bool FailAllocation::Failed() noexcept { return allocation_failed; }
int FailAllocation::Remaining() noexcept { return allocations_before_failure; }

}  // namespace talon_test

// One-shot, submitting-thread-only injection. Worker/test framework allocations
// are unaffected; no assertions run while the injection is armed.
void* operator new(std::size_t size) {
  CheckAllocation();
  if (void* memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
  std::free(pointer);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
  return ::operator new(size, tag);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
  std::free(pointer);
}

void* operator new(std::size_t size, std::align_val_t alignment) {
  CheckAllocation();
  const auto align = static_cast<std::size_t>(alignment);
#ifdef _MSC_VER
  if (void* memory = _aligned_malloc(size == 0 ? 1 : size, align)) {
    return memory;
  }
#else
  void* memory = nullptr;
  if (posix_memalign(&memory, align, size == 0 ? 1 : size) == 0) {
    return memory;
  }
#endif
  throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
#ifdef _MSC_VER
  _aligned_free(pointer);
#else
  std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t,
                     std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t,
                       std::align_val_t alignment) noexcept {
  ::operator delete(pointer, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment,
                   const std::nothrow_t&) noexcept {
  try {
    return ::operator new(size, alignment);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t size, std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
  return ::operator new(size, alignment, tag);
}
void operator delete(void* pointer, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::align_val_t alignment,
                       const std::nothrow_t&) noexcept {
  ::operator delete(pointer, alignment);
}
