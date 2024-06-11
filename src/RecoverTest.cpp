#include <algorithm>
#include <cassert>
#include <iostream>
#include <numeric>
#include <span>
#include <tuple>
#include <vector>

#include "rigtorp/MPMCQueue.h"

namespace {
using Type = long;
using PSlot = rigtorp::mpmc::Queue<Type>::PSlot;
using VSlot = rigtorp::mpmc::Queue<Type>::VSlot;
using PSlotArray = PSlot[];
using PSlotArrayPPtr = pmem::obj::persistent_ptr<PSlotArray>;
struct Slots : public std::vector<VSlot> {
  //   auto operator<=>(const Slots&) const = default;

  Slots(size_t sz) : std::vector<VSlot>(sz) {}
  Slots(std::span<const PSlot> span) : Slots{span.size()} {
    for (auto i = 0u; i < span.size(); ++i) {
      const auto& pSlot = span[i];
      this->at(i).turn = pSlot.get_ro().turn.load();
      //   Type st = *reinterpret_cast<const Type*>(&(pSlot.get_ro().storage));
      Type st = pSlot.get_ro().storage;
      this->at(i).storage = st;
    }
  }
  Slots(const std::vector<Type>& vec) : Slots{vec.size()} {
    for (auto i = 0u; i < vec.size(); ++i) {
      const auto& v = vec.at(i);
      this->at(i).turn = static_cast<size_t>(v);
      this->at(i).storage = Type{};
    }
  }

  auto ToPSlots() const {
    std::vector<PSlot> vec(this->size());
    for (auto i = 0u; i < this->size(); ++i) {
      auto& v = this->at(i);
      vec.at(i).get_rw().turn.store(v.turn);
      vec.at(i).get_rw().construct(v.storage);
    }
    return vec;
  }
  bool operator==(const Slots&) const = default;

  /*   Slots(const Slots& other) : Slots{other.size()} {
      for (auto i = 0u; i < other.size(); ++i) {
        auto& v = other.at(i);
        this->at(i).turn.store(v.turn.load());
        Type st = *reinterpret_cast<const Type*>(&(v.storage));
        this->at(i).construct(st);
      }
    } */

  /*   Slots& operator=(const Slots& other) {
      if (this == &other) return *this;
      for (auto i = 0u; i < other.size(); ++i) {
        auto& v = other.at(i);
        this->at(i).turn.store(v.turn.load());
        Type st = *reinterpret_cast<const Type*>(&(v.storage));
        this->at(i).construct(st);
      }
      return *this;
    } */

  /*   bool operator==(const Slots& other) const {
      if (size() != other.size()) return false;
      for (auto i = 0u; i < other.size(); ++i) {
        const auto& a = at(i);
        const auto& b = other.at(i);
        auto GetStorage = [](const auto& x) -> Type { return *reinterpret_cast<const Type*>(&(x.storage)); };
        if (a.turn != b.turn || GetStorage(a) != GetStorage(b)) return false;
      }
      return true;
    } */
};
struct State {
  Slots slots;
  size_t tail; // dequeuers
  size_t head; // enqueuers
  bool operator==(const State&) const = default;
  //   auto operator<=>(const State&) const = default;
};
struct Test {
  Slots input;
  State expected;
  State result;
  Test(std::vector<Type> inputVec, std::vector<Type> expectedVec, size_t eTail, size_t eHead)
      : input{inputVec}, expected{expectedVec, eTail, eHead}, result{expectedVec.size(), 0u, 0u} {
    assert(inputVec.size() == expectedVec.size());
  }
};

std::ostream& operator<<(std::ostream& os, const VSlot& s) {
  os << s.turn;
  return os;
}
std::ostream& operator<<(std::ostream& os, const Slots& slots) {
  for (const auto& v : slots)
    os << v << ".";
  return os;
}
std::ostream& operator<<(std::ostream& os, const State& st) {
  os << st.slots << " T:" << st.tail << " H:" << st.head;
  return os;
}
std::ostream& operator<<(std::ostream& os, const Test& t) {
  os << t.input << "\tR-> " << t.result << "\tE-> " << t.expected;
  if (t.expected != t.result) os << "\t!";
  os << "\n";
  return os;
}

std::vector<Test> Tests{
    {{0, 0, 0, 0}, {0, 0, 0, 0}, 0, 0},
    {{0, 0, 0, 1}, {1, 0, 0, 0}, 0, 1},
    {{1, 0, 0, 1}, {1, 1, 0, 0}, 0, 2},
    {{0, 0, 0, 2}, {2, 2, 2, 2}, 4, 4},
    {{1, 1, 1, 1}, {1, 1, 1, 1}, 0, 4},
    {{1, 1, 1, 2}, {2, 2, 2, 2}, 4, 4},
    {{2, 1, 1, 2}, {2, 2, 2, 2}, 4, 4},
    {{2, 2, 2, 2}, {2, 2, 2, 2}, 4, 4},
    {{4, 2, 3, 2}, {4, 3, 2, 2}, 5, 6},
    {{2, 2, 2, 4}, {4, 4, 4, 4}, 8, 8},
    {{4, 2, 2, 4}, {4, 4, 4, 4}, 8, 8},
    {{4, 2, 3, 4}, {4, 4, 4, 4}, 8, 8},
    {{2, 3, 4, 2}, {4, 4, 4, 2}, 7, 7},
    {{0, 1, 1, 2}, {2, 2, 2, 2}, 4, 4}

};
} // namespace

int main() {
  struct RecoverTestRoot {};
  using PoolType = pmem::obj::pool<RecoverTestRoot>;
  const std::string filepath = "/mnt/pmem0/myrontsa/RecoverTest";
  const std::string layout = "RecoverTest";
  if (std::filesystem::exists(filepath) == false) {
    auto pool = PoolType::create(filepath, layout, 1024 * 1024 * 500);
    pool.close();
  }
  int checkPool = PoolType::check(filepath, layout);
  if (checkPool == 0) {
    assert(false && "Error: poolfile is in inconsistent state");
  }
  auto pool = PoolType::open(filepath, layout);

  rigtorp::mpmc::Queue<Type> q{1, false};
  for (auto& test : Tests) {
    auto pSlotsInput = test.input.ToPSlots();
    const auto& [pSlotsResult, tail, head] = q.RecoverTest(pool, pSlotsInput.data(), pSlotsInput.size());
    test.result = {Slots{pSlotsResult}, tail, head};
    PSlotArrayPPtr dealoc{pSlotsResult.data()};
    // pmem::obj::delete_persistent_atomic<PSlotArray>(dealoc, pSlotsResult.size() + 1);
  }
  for (const auto& test : Tests)
    std::cout << test << "\n";

  pool.close();
}
