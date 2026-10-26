// pqueue.hpp - Emergency priority queue.
//
// A binary min-heap ordered by (rank, id): lower rank first (High=0, Medium=1,
// Low=2), and for equal rank the older request (smaller id) first, i.e. FIFO
// inside a priority level. A position map makes remove-by-id O(log n), which is
// needed because the UI lets a dispatcher dispatch a specific request.
//
//   push    O(log n)    top    O(1)    pop    O(log n)    remove(id)  O(log n)
#pragma once
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <unordered_map>
#include <vector>

struct QItem {
  int rank;
  int64_t id;
};

class EmergencyHeap {
 public:
  bool empty() const { return a_.empty(); }
  size_t size() const { return a_.size(); }
  bool contains(int64_t id) const { return pos_.count(id) != 0; }

  // Returns false (and changes nothing) if the id is already queued.
  bool push(const QItem& item) {
    if (contains(item.id)) return false;
    a_.push_back(item);
    pos_[item.id] = a_.size() - 1;
    siftUp(a_.size() - 1);
    return true;
  }

  const QItem& top() const {
    if (a_.empty()) throw std::out_of_range("heap is empty");
    return a_.front();
  }

  QItem pop() {
    if (a_.empty()) throw std::out_of_range("heap is empty");
    QItem result = a_.front();
    removeAt(0);
    return result;
  }

  bool remove(int64_t id) {
    auto it = pos_.find(id);
    if (it == pos_.end()) return false;
    removeAt(it->second);
    return true;
  }

  void clear() {
    a_.clear();
    pos_.clear();
  }

  // Ids in dispatch order. Works on a copy, so the live heap is untouched.
  std::vector<QItem> orderedSnapshot() const {
    EmergencyHeap copy = *this;
    std::vector<QItem> out;
    out.reserve(copy.size());
    while (!copy.empty()) out.push_back(copy.pop());
    return out;
  }

 private:
  static bool before(const QItem& x, const QItem& y) {
    return x.rank < y.rank || (x.rank == y.rank && x.id < y.id);
  }

  void place(size_t i, const QItem& item) {
    a_[i] = item;
    pos_[item.id] = i;
  }

  void siftUp(size_t i) {
    QItem moving = a_[i];
    while (i > 0) {
      size_t parent = (i - 1) / 2;
      if (!before(moving, a_[parent])) break;
      place(i, a_[parent]);
      i = parent;
    }
    place(i, moving);
  }

  void siftDown(size_t i) {
    QItem moving = a_[i];
    const size_t n = a_.size();
    while (true) {
      size_t child = 2 * i + 1;
      if (child >= n) break;
      if (child + 1 < n && before(a_[child + 1], a_[child])) ++child;
      if (!before(a_[child], moving)) break;
      place(i, a_[child]);
      i = child;
    }
    place(i, moving);
  }

  void removeAt(size_t i) {
    const int64_t removedId = a_[i].id;
    const size_t last = a_.size() - 1;
    if (i == last) {
      a_.pop_back();
      pos_.erase(removedId);
      return;
    }
    QItem tail = a_[last];
    a_.pop_back();
    pos_.erase(removedId);
    place(i, tail);
    siftUp(i);
    siftDown(pos_[tail.id]);
  }

  std::vector<QItem> a_;
  std::unordered_map<int64_t, size_t> pos_;
};
