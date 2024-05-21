#include <algorithm>
#include <cassert>
#include <iostream>
#include <numeric>
#include <tuple>
#include <vector>

namespace {
template <typename T>
struct Slot {
  Slot(size_t t) : turn{t} {}
  size_t turn;
  T storage{};
  //   auto operator<=>(const Slot&) const = default;
  //   bool operator<(const Slot& other) const { return turn < other.turn; }
  //   bool operator==(const Slot&) const = default;
  // auto operator==(const Slot& other) {return turn < other.turn;}
  auto operator<=>(const Slot&) const = default;
};
using Slots = std::vector<Slot<std::string>>;
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
};

template <typename T>
std::ostream& operator<<(std::ostream& os, const Slot<T>& s) {
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
    {{0, 0, 0, 0}, {{0, 0, 0, 0}, 0, 0}, {}},
    {{0, 0, 0, 1}, {{1, 0, 0, 0}, 0, 1}, {}},
    {{1, 0, 0, 1}, {{1, 1, 0, 0}, 0, 2}, {}},
    {{0, 0, 0, 2}, {{2, 2, 2, 2}, 4, 4}, {}},
    {{1, 1, 1, 1}, {{1, 1, 1, 1}, 0, 4}, {}},
    {{1, 1, 1, 2}, {{2, 2, 2, 2}, 4, 4}, {}},
    {{2, 1, 1, 2}, {{2, 2, 2, 2}, 4, 4}, {}},
    {{2, 2, 2, 2}, {{2, 2, 2, 2}, 4, 4}, {}},
    {{4, 2, 3, 2}, {{4, 3, 2, 2}, 5, 6}, {}},
    {{2, 2, 2, 4}, {{4, 4, 4, 4}, 8, 8}, {}},
    {{4, 2, 2, 4}, {{4, 4, 4, 4}, 8, 8}, {}},
    {{4, 2, 3, 4}, {{4, 4, 4, 4}, 8, 8}, {}},
    {{2, 3, 4, 2}, {{4, 4, 4, 2}, 7, 7}, {}},
    {{0, 1, 1, 2}, {{2, 2, 2, 2}, 4, 4}, {}},
};

auto Validate(const Slots& input) -> bool {
  bool preCondition;
  const auto [min, max] = std::ranges::minmax_element(input, [](auto a, auto b) { return a.turn < b.turn; });
  preCondition = (max->turn - min->turn) <= 2;
  if (!preCondition) return false;
  //
  return true;
}
auto Recover(const Slots& input) -> State {
  assert(!input.empty());
  assert(Validate(input));
  Slots slots = input;
  // find max turn and its last index
  const auto lastMaxR = std::ranges::max_element(slots.rbegin(), slots.rend(), [](auto a, auto b) { return a.turn < b.turn; });
  assert(lastMaxR != slots.rend());
  const auto lastMax = lastMaxR.base() - 1;
  const auto maxTurn = lastMax->turn;
  if ((maxTurn % 2) == 0) {
    // dequeues present, mark incomplete dequeues as complete and sort the rest of the enqueues
    std::ranges::for_each(slots.begin(), lastMax, [maxTurn](auto& a) { a.turn = maxTurn; });
    std::ranges::stable_sort(lastMax + 1, slots.end(), [](auto a, auto b) { return a.turn > b.turn; });
  } else {
    // only enqueues present, sort whole array
    std::ranges::stable_sort(slots.begin(), slots.end(), [](auto a, auto b) { return a.turn > b.turn; });
  }
  // recover tail and head
  const auto firstZero = std::ranges::find_if(slots, [](auto a) { return a.turn == 0; });
  size_t tail = std::accumulate(slots.begin(), firstZero, 0ULL, [](auto acc, auto slot) { return acc + slot.turn / 2; });
  size_t head = std::accumulate(slots.begin(), firstZero, 0ULL, [](auto acc, auto slot) { return acc + (slot.turn + 1) / 2; });
  return {slots, tail, head};
}

} // namespace

int main() {
  for (auto& test : Tests)
    test.result = Recover(test.input);
  for (const auto& test : Tests)
    std::cout << test << "\n";
}
