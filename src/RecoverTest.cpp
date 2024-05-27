#include <algorithm>
#include <cassert>
#include <initializer_list>
#include <iostream>
#include <numeric>
#include <span>
#include <tuple>
#include <vector>

#include "rigtorp/MPMCQueue.h"

namespace {
// template <typename T>
// struct Slot {
//   Slot(size_t t) : turn{t} {}
//   size_t turn;
//   T storage{};
//   auto operator<=>(const Slot&) const = default;
// };
using Type = long;
using Slots = std::vector<rigtorp::mpmc::Slot<Type>>;
struct State {
  Slots slots;
  size_t tail; // dequeuers
  size_t head; // enqueuers
  auto operator<=>(const State&) const = default;
};
struct Test {
  Slots input;
  State expected;
  State result;
  Test(std::initializer_list<Type> i, std::initializer_list<Type> e, size_t eTail, size_t eHead) {
    for (const auto& v : i) input.emplace_back(v, v);
    for (const auto& v : e) expected.slots.emplace_back(v, v);
    expected.tail = eTail;
    expected.head = eHead;
  }
};

template <typename T>
std::ostream& operator<<(std::ostream& os, const rigtorp::mpmc::Slot<T>& s) {
  os << s.turn;
  return os;
}
template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& vec) {
  for (const auto& v : vec)
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
  for (auto& test : Tests) {
    rigtorp::mpmc::Queue<Type> q{test.input.size()};
    auto [slots, tail, head] = q.RecoverTest({test.input});
    test.result = State{Slots{slots.begin(), slots.end()}, tail, head};
  }
  for (const auto& test : Tests)
    std::cout << test << "\n";
}
