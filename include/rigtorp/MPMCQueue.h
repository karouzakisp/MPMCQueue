/*
Copyright (c) 2020 Erik Rigtorp <erik@rigtorp.se>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
 */

#pragma once

#include <atomic>
#include <cassert>
#include <cstddef> // offsetof
#include <limits>
#include <memory>
#include <new> // std::hardware_destructive_interference_size
#include <stdexcept>

#ifndef __cpp_aligned_new
#ifdef _WIN32
#include <malloc.h> // _aligned_malloc
#else
#include <stdlib.h> // posix_memalign
#endif
#endif

#include <libpmemobj++/make_persistent_array_atomic.hpp>
#include <libpmemobj++/p.hpp>
#include <libpmemobj++/persistent_ptr.hpp>
#include <libpmemobj++/pool.hpp>

namespace {
// Toggle Memory Ordering Here
// constexpr auto LoadMemoryOrder = std::memory_order_acquire;
// constexpr auto StoreMemoryOrder = std::memory_order_release;
constexpr auto LoadMemoryOrder = std::memory_order_seq_cst;
constexpr auto StoreMemoryOrder = std::memory_order_seq_cst;
} // namespace

namespace rigtorp {
namespace mpmc {
#if defined(__cpp_lib_hardware_interference_size) && !defined(__APPLE__)
static constexpr size_t hardwareInterferenceSize =
    std::hardware_destructive_interference_size;
#else
static constexpr size_t hardwareInterferenceSize = 64;
#endif

#if defined(__cpp_aligned_new)
template <typename T>
using AlignedAllocator = std::allocator<T>;
#else
template <typename T>
struct AlignedAllocator {
  using value_type = T;

  T* allocate(std::size_t n) {
    if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
      throw std::bad_array_new_length();
    }
#ifdef _WIN32
    auto* p = static_cast<T*>(_aligned_malloc(sizeof(T) * n, alignof(T)));
    if (p == nullptr) {
      throw std::bad_alloc();
    }
#else
    T* p;
    if (posix_memalign(reinterpret_cast<void**>(&p), alignof(T),
                       sizeof(T) * n) != 0) {
      throw std::bad_alloc();
    }
#endif
    return p;
  }

  void deallocate(T* p, std::size_t) {
#ifdef _WIN32
    _aligned_free(p);
#else
    free(p);
#endif
  }
};
#endif

template <typename T>
struct Slot {
  ~Slot() noexcept {
    if (turn & 1) {
      destroy();
    }
  }

  template <typename... Args>
  void construct(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    new (&storage) T(std::forward<Args>(args)...);
  }

  void destroy() noexcept {
    static_assert(std::is_nothrow_destructible<T>::value,
                  "T must be nothrow destructible");
    reinterpret_cast<T*>(&storage)->~T();
  }

  T&& move() noexcept { return reinterpret_cast<T&&>(storage); }

  // Align to avoid false sharing between adjacent slots
  alignas(hardwareInterferenceSize) std::atomic<size_t> turn = {0};
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
};

template <typename T>
struct MySlot {
  ~MySlot() noexcept {
    if (turn & 1) {
      destroy();
    }
  }

  template <typename... Args>
  void construct(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    new (&storage) T(std::forward<Args>(args)...);
  }

  void destroy() noexcept {
    static_assert(std::is_nothrow_destructible<T>::value,
                  "T must be nothrow destructible");
    reinterpret_cast<T*>(&storage)->~T();
  }

  T&& move() noexcept { return reinterpret_cast<T&&>(storage); }

  alignas(hardwareInterferenceSize) std::atomic<size_t> turn = {0};
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
};

template <typename T, typename Allocator = AlignedAllocator<Slot<T>>>
class Queue {
private:
  using PSlot = pmem::obj::p<MySlot<T>>; // only persist slot
  using PSlotArray = PSlot[];
  using SlotArrayPPtr = pmem::obj::persistent_ptr<PSlotArray>;
  struct Root {
    SlotArrayPPtr pSlots_;
  };
  using RootPool = pmem::obj::pool<Root>;
  static_assert(std::is_nothrow_copy_assignable<T>::value ||
                    std::is_nothrow_move_assignable<T>::value,
                "T must be nothrow copy or move assignable");

  static_assert(std::is_nothrow_destructible<T>::value,
                "T must be nothrow destructible");

  void QueueInit() {
    if (capacity_ < 1) {
      throw std::invalid_argument("capacity < 1");
    }
    // Allocate one extra slot to prevent false sharing on the last slot
    slots_ = allocator_.allocate(capacity_ + 1);
    // Allocators are not required to honor alignment for over-aligned types
    // (see http://eel.is/c++draft/allocator.requirements#10) so we verify
    // alignment here
    if (reinterpret_cast<size_t>(slots_) % alignof(Slot<T>) != 0) {
      allocator_.deallocate(slots_, capacity_ + 1);
      throw std::bad_alloc();
    }
    for (size_t i = 0; i < capacity_; ++i) {
      new (&slots_[i]) Slot<T>();
    }
    static_assert(
        alignof(Slot<T>) == hardwareInterferenceSize,
        "Slot must be aligned to cache line boundary to prevent false sharing");
    static_assert(sizeof(Slot<T>) % hardwareInterferenceSize == 0,
                  "Slot size must be a multiple of cache line size to prevent "
                  "false sharing between adjacent slots");
    static_assert(sizeof(Queue) % hardwareInterferenceSize == 0,
                  "Queue size must be a multiple of cache line size to "
                  "prevent false sharing between adjacent queues");
    static_assert(
        offsetof(Queue, tail_) - offsetof(Queue, head_) ==
            static_cast<std::ptrdiff_t>(hardwareInterferenceSize),
        "head and tail must be a cache line apart to prevent false sharing");
  }
  void QueueDestroy() {
    for (size_t i = 0; i < capacity_; ++i) {
      slots_[i].~Slot();
    }
    allocator_.deallocate(slots_, capacity_ + 1);
  }

  void QueueInitPersistent() {
    if (capacity_ < 1) {
      throw std::invalid_argument("capacity < 1");
    }
    const char* filepath = "poolfile";
    const char* layout = "layout";
    if (RootPool::check(filepath, layout) == 1) {
      pop_ = RootPool::open(filepath, layout);
    } else {
      pop_ = RootPool::create(filepath, layout, 1024 * 1024 * 500);
    }

    // Allocate one extra slot to prevent false sharing on the last slot
    if (pop_.root()->pSlots_ == nullptr) {
      /*      struct myPExpSlot {
              int x;
              int y;
            };
            size_t MyPCapacity_ = 10'000'000;
            using MyPExpPSlotArray = myPExpSlot[];
            // struct myRoot {
            //  MyExpPSlotArray mypSlots_;
            //};
            // using myRootPool = pmem::obj::pool<myRoot>;
            // myRootPool myPop_;
            pmem::obj::persistent_ptr<MyPExpPSlotArray> myPExpSlots_;
            pmem::obj::make_persistent_atomic<MyPExpPSlotArray>(pop_, myPExpSlots_, MyPCapacity_ + 1);
      */
      pmem::obj::make_persistent_atomic<PSlotArray>(pop_, pSlots_, capacity_ + 1);
      pop_.root()->pSlots_ = pSlots_;
      // TODO: Make sure each pSlot is aligned. Honor the guarantees of the non-persistent constructor
      /*     if (reinterpret_cast<size_t>(slots_) % alignof(Slot<T>) != 0) {
            allocator_.deallocate(slots_, capacity_ + 1);
            throw std::bad_alloc();
          }
          for (size_t i = 0; i < capacity_; ++i) {
            new (&slots_[i]) Slot<T>();
          } */
    }

    // TODO: Call Recover() around here

    // TODO: Make sure each pSlot is aligned. Honor the guarantees of the non-persistent constructor
    static_assert(
        alignof(PSlot) == hardwareInterferenceSize,
        "Slot must be aligned to cache line boundary to prevent false sharing");
    static_assert(sizeof(PSlot) % hardwareInterferenceSize == 0,
                  "Slot size must be a multiple of cache line size to prevent "
                  "false sharing between adjacent slots");
    static_assert(sizeof(Queue) % hardwareInterferenceSize == 0,
                  "Queue size must be a multiple of cache line size to "
                  "prevent false sharing between adjacent queues");
    static_assert(
        offsetof(Queue, tail_) - offsetof(Queue, head_) ==
            static_cast<std::ptrdiff_t>(hardwareInterferenceSize),
        "head and tail must be a cache line apart to prevent false sharing");
  }

  void QueueDestroyPersistent() {
    pmem::obj::delete_persistent_atomic<PSlotArray>(pSlots_, capacity_ + 1);
    pSlots_ = nullptr;
    pop_.root()->pSlots_ = nullptr;
  }

public:
  explicit Queue(const size_t capacity, bool isPersistent_,
                 const Allocator& allocator = Allocator())
      : capacity_(capacity), allocator_(allocator), head_(0), tail_(0), isPersistent_(isPersistent_) {
    if (isPersistent_) QueueInitPersistent();
    else QueueInit();
  }

  ~Queue() noexcept {
    if (isPersistent_) QueueDestroyPersistent();
    else QueueDestroy();
  }

  // non-copyable and non-movable
  Queue(const Queue&) = delete;
  Queue& operator=(const Queue&) = delete;

  template <typename... Args>
  void emplace(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    auto const head = head_.fetch_add(1);
    auto& slot = slots_[idx(head)];
    while (turn(head) * 2 != slot.turn.load(std::memory_order_acquire));
    slot.construct(std::forward<Args>(args)...);
    slot.turn.store(turn(head) * 2 + 1, std::memory_order_release);
  }

  template <typename... Args>
  void emplace_p(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    auto const head = head_.fetch_add(1);
    pmem::obj::p<MySlot<T>>& slot = pSlots_[idx(head)];
    while (turn(head) * 2 != slot.get_ro().turn.load(LoadMemoryOrder));
    slot.get_rw().construct(std::forward<Args>(args)...);
    slot.get_rw().turn.store(turn(head) * 2 + 1, StoreMemoryOrder);
    pop_.persist(slot);
  }

  template <typename... Args>
  bool try_emplace(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    auto head = head_.load(std::memory_order_acquire);
    for (;;) {
      auto& slot = slots_[idx(head)];
      if (turn(head) * 2 == slot.turn.load(std::memory_order_acquire)) {
        if (head_.compare_exchange_strong(head, head + 1)) {
          slot.construct(std::forward<Args>(args)...);
          slot.turn.store(turn(head) * 2 + 1, std::memory_order_release);
          return true;
        }
      } else {
        auto const prevHead = head;
        head = head_.load(std::memory_order_acquire);
        if (head == prevHead) {
          return false;
        }
      }
    }
  }

  void push(const T& v) noexcept {
    static_assert(std::is_nothrow_copy_constructible<T>::value,
                  "T must be nothrow copy constructible");
    emplace(v);
  }
  void push_p(const T& v) noexcept {
    static_assert(std::is_nothrow_copy_constructible<T>::value,
                  "T must be nothrow copy constructible");
    emplace_p(v);
  }

  template <typename P,
            typename = typename std::enable_if<
                std::is_nothrow_constructible<T, P&&>::value>::type>
  void push(P&& v) noexcept {
    emplace(std::forward<P>(v));
  }
  template <typename P,
            typename = typename std::enable_if<
                std::is_nothrow_constructible<T, P&&>::value>::type>
  void push_p(P&& v) noexcept {
    emplace_p(std::forward<P>(v));
  }

  bool try_push(const T& v) noexcept {
    static_assert(std::is_nothrow_copy_constructible<T>::value,
                  "T must be nothrow copy constructible");
    return try_emplace(v);
  }

  template <typename P,
            typename = typename std::enable_if<
                std::is_nothrow_constructible<T, P&&>::value>::type>
  bool try_push(P&& v) noexcept {
    return try_emplace(std::forward<P>(v));
  }

  void pop(T& v) noexcept {
    auto const tail = tail_.fetch_add(1);
    auto& slot = slots_[idx(tail)];
    while (turn(tail) * 2 + 1 != slot.turn.load(std::memory_order_acquire));
    v = slot.move();
    slot.destroy();
    slot.turn.store(turn(tail) * 2 + 2, std::memory_order_release);
  }

  void pop_p(T& v) noexcept {
    auto const tail = tail_.fetch_add(1);
    pmem::obj::p<MySlot<T>>& slot = pSlots_[idx(tail)];
    while (turn(tail) * 2 + 1 != slot.get_ro().turn.load(LoadMemoryOrder));
    // v = slot.move();
    // slot.destroy();
    v = slot.get_rw().move();
    slot.get_rw().turn.store(turn(tail) * 2 + 2, StoreMemoryOrder);
    pop_.persist(slot);
  }

  bool try_pop(T& v) noexcept {
    auto tail = tail_.load(std::memory_order_acquire);
    for (;;) {
      auto& slot = slots_[idx(tail)];
      if (turn(tail) * 2 + 1 == slot.turn.load(std::memory_order_acquire)) {
        if (tail_.compare_exchange_strong(tail, tail + 1)) {
          v = slot.move();
          slot.destroy();
          slot.turn.store(turn(tail) * 2 + 2, std::memory_order_release);
          return true;
        }
      } else {
        auto const prevTail = tail;
        tail = tail_.load(std::memory_order_acquire);
        if (tail == prevTail) {
          return false;
        }
      }
    }
  }

  /// Returns the number of elements in the queue.
  /// The size can be negative when the queue is empty and there is at least one
  /// reader waiting. Since this is a concurrent queue the size is only a best
  /// effort guess until all reader and writer threads have been joined.
  ptrdiff_t size() const noexcept {
    // TODO: How can we deal with wrapped queue on 32bit?
    return static_cast<ptrdiff_t>(head_.load(std::memory_order_relaxed) -
                                  tail_.load(std::memory_order_relaxed));
  }

  /// Returns true if the queue is empty.
  /// Since this is a concurrent queue this is only a best effort guess
  /// until all reader and writer threads have been joined.
  bool empty() const noexcept { return size() <= 0; }

private:
  constexpr size_t idx(size_t i) const noexcept { return i % capacity_; }

  constexpr size_t turn(size_t i) const noexcept { return i / capacity_; }

private:
  const size_t capacity_;
  Slot<T>* slots_;
#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
  Allocator allocator_ [[no_unique_address]];
#else
  Allocator allocator_;
#endif

  // Align to avoid false sharing between head_ and tail_
  alignas(hardwareInterferenceSize) std::atomic<size_t> head_;
  alignas(hardwareInterferenceSize) std::atomic<size_t> tail_;

  bool isPersistent_;
  RootPool pop_;
  SlotArrayPPtr pSlots_;
};
} // namespace mpmc

template <typename T,
          typename Allocator = mpmc::AlignedAllocator<mpmc::Slot<T>>>
using MPMCQueue = mpmc::Queue<T, Allocator>;

} // namespace rigtorp
