// graph.hpp - Weighted undirected road graph + Dijkstra's shortest-path algorithm.
//
// Nodes are locations (rows of the `nodes` table), edge weights are road
// distances in km (rows of the `edges` table). One Dijkstra run from the
// emergency's node gives the distance to every vehicle in a single pass:
// O((V + E) log V) with a binary-heap priority queue.
#pragma once
#include <cstdint>
#include <functional>
#include <limits>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

class Graph {
 public:
  static constexpr double INF = std::numeric_limits<double>::infinity();

  struct Result {
    std::vector<double> dist;  // dist[i] = km from the source to node index i
    std::vector<int> prev;     // prev[i] = previous node index on the shortest path (-1 = none)
    int source = -1;           // node index of the source (-1 = unknown node)
  };

  void clear() {
    index_.clear();
    ids_.clear();
    adj_.clear();
  }

  void addNode(int64_t id) {
    if (index_.count(id)) return;
    index_[id] = static_cast<int>(ids_.size());
    ids_.push_back(id);
    adj_.emplace_back();
  }

  // Undirected edge. Both endpoints must already exist; weight must be > 0.
  bool addEdge(int64_t a, int64_t b, double km) {
    auto ia = index_.find(a), ib = index_.find(b);
    if (ia == index_.end() || ib == index_.end() || !(km > 0)) return false;
    adj_[ia->second].push_back({ib->second, km});
    adj_[ib->second].push_back({ia->second, km});
    return true;
  }

  bool hasNode(int64_t id) const { return index_.count(id) != 0; }
  size_t nodeCount() const { return ids_.size(); }

  Result dijkstra(int64_t sourceId) const {
    Result r;
    r.dist.assign(ids_.size(), INF);
    r.prev.assign(ids_.size(), -1);
    auto it = index_.find(sourceId);
    if (it == index_.end()) return r;  // unknown source: everything unreachable
    r.source = it->second;
    r.dist[r.source] = 0.0;

    using Entry = std::pair<double, int>;  // (distance, node index)
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;
    pq.push({0.0, r.source});
    while (!pq.empty()) {
      auto [d, u] = pq.top();
      pq.pop();
      if (d > r.dist[u]) continue;  // stale queue entry
      for (const Edge& e : adj_[u]) {
        double nd = d + e.km;
        if (nd < r.dist[e.to]) {
          r.dist[e.to] = nd;
          r.prev[e.to] = u;
          pq.push({nd, e.to});
        }
      }
    }
    return r;
  }

  // Distance in km from the run's source to a node id; INF if unreachable/unknown.
  double distanceTo(const Result& r, int64_t nodeId) const {
    auto it = index_.find(nodeId);
    if (it == index_.end() || r.source < 0) return INF;
    return r.dist[it->second];
  }

  // Node ids from the source to the target (inclusive). Empty if unreachable.
  std::vector<int64_t> pathTo(const Result& r, int64_t targetId) const {
    std::vector<int64_t> path;
    auto it = index_.find(targetId);
    if (it == index_.end() || r.source < 0 || r.dist[it->second] == INF) return path;
    for (int v = it->second; v != -1; v = r.prev[v]) path.push_back(ids_[v]);
    std::vector<int64_t> forward(path.rbegin(), path.rend());
    return forward;
  }

 private:
  struct Edge {
    int to;
    double km;
  };
  std::unordered_map<int64_t, int> index_;
  std::vector<int64_t> ids_;
  std::vector<std::vector<Edge>> adj_;
};
