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

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstddef> // offsetof
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <new> // std::hardware_destructive_interference_size
#include <numeric>
#include <span>
#include <stdexcept>
#include <tuple>

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
constexpr auto LoadMemoryOrder = std::memory_order_acquire;
constexpr auto StoreMemoryOrder = std::memory_order_release;
// constexpr auto LoadMemoryOrder = std::memory_order_seq_cst;
// constexpr auto StoreMemoryOrder = std::memory_order_seq_cst;
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

  T&& move() noexcept {
    return reinterpret_cast<T&&>(storage);
  }

  // Align to avoid false sharing between adjacent slots
  alignas(hardwareInterferenceSize) std::atomic<size_t> turn = {0};
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
};

/* template <typename T>
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

  // Align to avoid false sharing between adjacent slots
  alignas(hardwareInterferenceSize) std::atomic<size_t> turn = {0};
  typename std::aligned_storage<sizeof(T), alignof(T)>::type storage;
}; */

template <typename T, typename Allocator = AlignedAllocator<Slot<T>>>
class Queue {
public:
  struct VSlot {
    std::size_t turn{};
    T storage{};
    bool operator==(const VSlot&) const = default;
  };
  using PSlot = pmem::obj::p<Slot<T>>; // only persist slot
  using PSlotArray = PSlot[];
  using PSlotArrayPPtr = pmem::obj::persistent_ptr<PSlotArray>;

private:
  struct Root {
    PSlotArrayPPtr pSlots_;
  };
  using RootPool = pmem::obj::pool<Root>;
  static_assert(std::is_nothrow_copy_assignable<T>::value ||
                    std::is_nothrow_move_assignable<T>::value,
                "T must be nothrow copy or move assignable");

  static_assert(std::is_nothrow_destructible<T>::value,
                "T must be nothrow destructible");

  auto RecoverValidatePre(std::span<const PSlot> slots) -> bool {
    bool preCondition;
    const auto [min, max] = std::ranges::minmax_element(slots, [](const auto& a, const auto& b) { return a.get_ro().turn < b.get_ro().turn; });
    preCondition = (max->get_ro().turn - min->get_ro().turn) <= 2;
    if (!preCondition) return false;
    return true;
  }
  auto RecoverValidatePost(std::span<const PSlot> slots) -> bool {
    return std::ranges::is_sorted(slots, [](const auto& a, const auto& b) { return a.get_ro().turn > b.get_ro().turn; });
  }
  auto CalculateTailHead(std::span<const PSlot> slots) -> std::tuple<size_t, size_t> {
    const auto firstZero = std::ranges::find_if(slots, [](const auto& a) { return a.get_ro().turn == 0; });
    size_t tail = std::accumulate(slots.begin(), firstZero, 0ULL, [](auto acc, const auto& slot) { return acc + slot.get_ro().turn / 2; });
    size_t head = std::accumulate(slots.begin(), firstZero, 0ULL, [](auto acc, const auto& slot) { return acc + (slot.get_ro().turn + 1) / 2; });
    return {tail, head};
  }
  auto CalculateTailHead(std::span<const VSlot> slots) -> std::tuple<size_t, size_t> {
    const auto firstZero = std::ranges::find_if(slots, [](const auto& a) { return a.turn == 0; });
    size_t tail = std::accumulate(slots.begin(), firstZero, 0ULL, [](auto acc, const auto& slot) { return acc + slot.turn / 2; });
    size_t head = std::accumulate(slots.begin(), firstZero, 0ULL, [](auto acc, const auto& slot) { return acc + (slot.turn + 1) / 2; });
    return {tail, head};
  }
  auto GetVSlots(std::span<const PSlot> pSlots) -> std::vector<VSlot> {
    std::vector<VSlot> vec{};
    for (const auto& s : pSlots) {
      vec.emplace_back(s.get_ro().turn.load(), *reinterpret_cast<const T*>(&(s.get_ro().storage)));
    }
    return vec;
  }
  auto GetPSlots(pmem::obj::pool_base pool, std::span<const VSlot> vSlots) -> std::span<PSlot> {
    const auto sz = vSlots.size();
    PSlotArrayPPtr pSlotArray{};
    pmem::obj::make_persistent_atomic<PSlotArray>(pool, pSlotArray, sz + 1);
    std::span<PSlot> pSlots{pSlotArray.get(), sz};
    for (auto i = 0u; i < sz; ++i) {
      auto& p = pSlots[i];
      auto& v = vSlots[i];
      p.get_rw().turn.store(v.turn);
      p.get_rw().construct(v.storage);
    }
    // pool.persist(pSlotArray.get(), sz);
    return pSlots;
  }

  auto RecoverImpl(pmem::obj::pool_base pool, std::span<PSlot> pSlots) -> std::tuple<std::span<PSlot>, size_t, size_t> {
    assert(!pSlots.empty());
    assert(RecoverValidatePre(pSlots));
    bool isSorted = std::ranges::is_sorted(pSlots, [](const auto& a, const auto& b) { return a.get_ro().turn > b.get_ro().turn; });
    if (isSorted) {
      const auto [tail, head] = CalculateTailHead(pSlots);
      return {pSlots, tail, head};
    }
    std::vector<VSlot> vSlots = GetVSlots(pSlots);
    // find max turn and its last index
    const auto lastMaxRIt = std::ranges::max_element(vSlots.rbegin(), vSlots.rend(), [](const auto& a, const auto& b) { return a.turn < b.turn; });
    assert(lastMaxRIt != vSlots.rend());
    const auto lastMaxIt = lastMaxRIt.base() - 1;
    const auto maxTurn = lastMaxIt->turn;
    if ((maxTurn % 2) == 0) {
      // dequeues present, mark incomplete dequeues as complete and sort the rest of the enqueues
      std::ranges::for_each(vSlots.begin(), lastMaxIt, [maxTurn](auto& a) { a.turn = maxTurn; });
      std::ranges::stable_sort(lastMaxIt + 1, vSlots.end(), [](const auto& a, const auto& b) { return a.turn > b.turn; });
    } else {
      // only enqueues present, sort whole array
      std::ranges::stable_sort(vSlots, [](const auto& a, const auto& b) { return a.turn > b.turn; });
    }
    const auto [tail, head] = CalculateTailHead(std::span<const VSlot>{vSlots.begin(), vSlots.end()});
    std::span<PSlot> newPSlots = GetPSlots(pool, vSlots);
    assert(RecoverValidatePost(newPSlots));
    return {newPSlots, tail, head};
  }

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
    const auto layout = std::filesystem::path{poolPath_}.filename().string();
    if (std::filesystem::exists(poolPath_) == false) {
      const std::size_t POOL_SIZE = 1024 * 1024 * 1024; // 1Gb
      pop_ = RootPool::create(poolPath_, layout, POOL_SIZE);
      pop_.close();
    }
    int checkPool = RootPool::check(poolPath_, layout);
    if (checkPool == 0) {
      assert(false && "Error: poolfile is in inconsistent state");
      throw std::runtime_error("poolfile is in inconsistent state");
    }
    pop_ = RootPool::open(poolPath_, layout);
    auto& rootPSlots = pop_.root()->pSlots_;
    if (rootPSlots == nullptr) {
      // Allocate one extra slot to prevent false sharing on the last slot
      pmem::obj::make_persistent_atomic<PSlotArray>(pop_, rootPSlots, capacity_ + 1);
      pop_.root().persist();
      // TODO: Make sure each pSlot is aligned. Honor the guarantees of the non-persistent constructor
      /*     if (reinterpret_cast<size_t>(slots_) % alignof(Slot<T>) != 0) {
            allocator_.deallocate(slots_, capacity_ + 1);
            throw std::bad_alloc();
          }
          for (size_t i = 0; i < capacity_; ++i) {
            new (&slots_[i]) Slot<T>();
          } */
    }
    pSlots_ = rootPSlots.get();

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
    auto& rootPSlots = pop_.root()->pSlots_;
    pmem::obj::delete_persistent_atomic<PSlotArray>(rootPSlots, capacity_ + 1);
    rootPSlots = nullptr;
    pop_.root().persist();
    pop_.close();
  }

public:
  explicit Queue(size_t capacity, bool isPersistent, std::string poolPath, const Allocator& allocator = Allocator())
      : capacity_(capacity), isPersistent_(isPersistent), poolPath_(poolPath), allocator_(allocator), head_(0), tail_(0) {
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

  auto Recover() -> void {
    auto [span, t, h] = RecoverImpl(pop_, std::span<PSlot>{pop_.root()->pSlots_.get(), capacity_});
    auto& rootPSlots = pop_.root()->pSlots_;
    auto prev = rootPSlots;
    rootPSlots = span.data();
    pop_.root().persist();
    pSlots_ = rootPSlots.get();
    tail_ = t;
    head_ = h;
    // if Failure here, then Persistent Memory Leak
    if (prev != rootPSlots)
      pmem::obj::delete_persistent_atomic<PSlotArray>(prev, capacity_ + 1);
  }

  template <typename... Args>
  void emplace_p(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    auto const head = head_.fetch_add(1);
    PSlot& slot = pSlots_[idx(head)];
    while (turn(head) * 2 != slot.get_ro().turn.load(LoadMemoryOrder))
      ;
    slot.get_rw().construct(std::forward<Args>(args)...);
    slot.get_rw().turn.store(turn(head) * 2 + 1, StoreMemoryOrder);
    pop_.persist(slot);
  }

  void pop_p(T& v) noexcept {
    auto const tail = tail_.fetch_add(1);
    PSlot& slot = pSlots_[idx(tail)];
    while (turn(tail) * 2 + 1 != slot.get_ro().turn.load(LoadMemoryOrder))
      ;
    // v = slot.move();
    // slot.destroy();
    v = slot.get_rw().move();
    slot.get_rw().turn.store(turn(tail) * 2 + 2, StoreMemoryOrder);
    pop_.persist(slot);
  }

  template <typename... Args>
  void emplace(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    auto const head = head_.fetch_add(1);
    auto& slot = slots_[idx(head)];
    while (turn(head) * 2 != slot.turn.load(LoadMemoryOrder))
      ;
    slot.construct(std::forward<Args>(args)...);
    slot.turn.store(turn(head) * 2 + 1, std::memory_order_release);
  }

  template <typename... Args>
  bool try_emplace(Args&&... args) noexcept {
    static_assert(std::is_nothrow_constructible<T, Args&&...>::value,
                  "T must be nothrow constructible with Args&&...");
    auto head = head_.load(std::memory_order_acquire);
    for (;;) {
      auto& slot = slots_[idx(head)];
      if (turn(head) * 2 == slot.turn.load(LoadMemoryOrder)) {
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
  template <typename P,
            typename = typename std::enable_if<
                std::is_nothrow_constructible<T, P&&>::value>::type>
  void push(P&& v) noexcept {
    emplace(std::forward<P>(v));
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

  void push_p(const T& v) noexcept {
    static_assert(std::is_nothrow_copy_constructible<T>::value,
                  "T must be nothrow copy constructible");
    emplace_p(v);
  }
  template <typename P,
            typename = typename std::enable_if<
                std::is_nothrow_constructible<T, P&&>::value>::type>
  void push_p(P&& v) noexcept {
    emplace_p(std::forward<P>(v));
  }

  void pop(T& v) noexcept {
    auto const tail = tail_.fetch_add(1);
    auto& slot = slots_[idx(tail)];
    while (turn(tail) * 2 + 1 != slot.turn.load(LoadMemoryOrder))
      ;
    v = slot.move();
    slot.destroy();
    slot.turn.store(turn(tail) * 2 + 2, std::memory_order_release);
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

  const size_t capacity_;
  bool isPersistent_;
  std::string poolPath_;
#if defined(__has_cpp_attribute) && __has_cpp_attribute(no_unique_address)
  Allocator allocator_ [[no_unique_address]];
#else
  Allocator allocator_;
#endif
  // Align to avoid false sharing between head_ and tail_
  alignas(hardwareInterferenceSize) std::atomic<size_t> head_;
  alignas(hardwareInterferenceSize) std::atomic<size_t> tail_;
  Slot<T>* slots_;

  RootPool pop_;
  PSlot* pSlots_;

public:
  auto RecoverTest(pmem::obj::pool_base pool, PSlot* input, std::size_t cap) {
    return RecoverImpl(pool, std::span<PSlot>{input, cap});
  }
};
} // namespace mpmc

template <typename T,
          typename Allocator = mpmc::AlignedAllocator<mpmc::Slot<T>>>
using MPMCQueue = mpmc::Queue<T, Allocator>;

} // namespace rigtorp
