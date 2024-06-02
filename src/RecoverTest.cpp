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
using SlotSpan = std::span<rigtorp::mpmc::Slot<Type>>;
struct Slots : public std::vector<rigtorp::mpmc::Slot<Type>> {
  auto operator<=>(const Slots&) const = default;
  Slots(size_t sz) : std::vector<rigtorp::mpmc::Slot<Type>>(sz) {}
  Slots(SlotSpan span) : Slots{span.size()} {
    for (auto i = 0u; i < span.size(); ++i) {
      auto& v = span[i];
      this->at(i).turn.store(v.turn.load());
      Type st = *reinterpret_cast<const Type*>(&(v.storage));
      this->at(i).construct(st);
    }
  }
  Slots(const std::vector<Type>& vec) : Slots{vec.size()} {
    for (auto i = 0u; i < vec.size(); ++i) {
      auto& v = vec.at(i);
      this->at(i).turn.store(static_cast<size_t>(v));
      this->at(i).construct(v);
    }
  }

  Slots(const Slots& other) : Slots{other.size()} {
    for (auto i = 0u; i < other.size(); ++i) {
      auto& v = other.at(i);
      this->at(i).turn.store(v.turn.load());
      Type st = *reinterpret_cast<const Type*>(&(v.storage));
      this->at(i).construct(st);
    }
  }

  Slots& operator=(const Slots& other) {
    if (this == &other) return *this;
    for (auto i = 0u; i < other.size(); ++i) {
      auto& v = other.at(i);
      this->at(i).turn.store(v.turn.load());
      Type st = *reinterpret_cast<const Type*>(&(v.storage));
      this->at(i).construct(st);
    }
    return *this;
  }

  bool operator==(const Slots& other) const {
    if (size() != other.size()) return false;
    for (auto i = 0u; i < other.size(); ++i) {
      const auto& a = at(i);
      const auto& b = other.at(i);
      auto GetStorage = [](const auto& x) -> Type { return *reinterpret_cast<const Type*>(&(x.storage)); };
      if (a.turn != b.turn || GetStorage(a) != GetStorage(b)) return false;
    }
    return true;
  }
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

std::ostream& operator<<(std::ostream& os, const rigtorp::mpmc::Slot<Type>& s) {
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
    {{0, 1, 1, 2}, {2, 2, 2, 2}, 4, 4}};
} // namespace

int main() {
  rigtorp::mpmc::Queue<Type> q{1, false};
  for (auto& test : Tests) {
    auto [slots, tail, head] = q.RecoverTest(test.input.data(), test.input.size());
    test.result = {slots, tail, head};
  }
  for (const auto& test : Tests)
    std::cout << test << "\n";
}
