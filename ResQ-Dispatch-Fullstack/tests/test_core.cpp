// Tests for the pure algorithm / crypto code (no database, no web framework).
#include <cassert>
#include <cmath>
#include <iostream>

#include "../src/crypto.hpp"
#include "../src/graph.hpp"
#include "../src/pqueue.hpp"

static int failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::cerr << "FAIL " << __FILE__ << ":" << __LINE__ << "  " #cond "\n"; \
      ++failures;                                                            \
    }                                                                        \
  } while (0)

static bool near(double a, double b) { return std::fabs(a - b) < 1e-9; }

static void testSha256() {
  CHECK(crypto::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
  CHECK(crypto::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  CHECK(crypto::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") ==
        "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");  // two-block message
  std::string million(1000000, 'a');
  CHECK(crypto::sha256Hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static void testHmacPbkdf2() {
  // RFC 4231, test case 2
  std::string key = "Jefe", msg = "what do ya want for nothing?";
  auto mac = crypto::hmacSha256(crypto::Bytes(key.begin(), key.end()), crypto::Bytes(msg.begin(), msg.end()));
  CHECK(crypto::toHex(mac) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
  // RFC 4231, test case 6: key longer than the block size
  crypto::Bytes longKey(131, 0xaa);
  std::string m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
  auto mac6 = crypto::hmacSha256(longKey, crypto::Bytes(m6.begin(), m6.end()));
  CHECK(crypto::toHex(mac6) == "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
  // RFC 7914 section 11 (first 32 bytes of the PBKDF2-HMAC-SHA256 outputs)
  crypto::Bytes salt1 = {'s', 'a', 'l', 't'};
  CHECK(crypto::pbkdf2Hex("passwd", salt1, 1) == "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc");
  crypto::Bytes salt2 = {'N', 'a', 'C', 'l'};
  CHECK(crypto::pbkdf2Hex("Password", salt2, 80000) == "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56");
}

static void testHexRandom() {
  crypto::Bytes b = {0x00, 0x0f, 0xa5, 0xff};
  CHECK(crypto::toHex(b) == "000fa5ff");
  CHECK(crypto::fromHex("000fa5ff") == b);
  bool threw = false;
  try { crypto::fromHex("xyz"); } catch (const std::invalid_argument&) { threw = true; }
  CHECK(threw);
  CHECK(crypto::randomHex(32).size() == 64);
  CHECK(crypto::randomHex(16) != crypto::randomHex(16));
  CHECK(crypto::constantTimeEquals("abc", "abc"));
  CHECK(!crypto::constantTimeEquals("abc", "abd"));
  CHECK(!crypto::constantTimeEquals("abc", "abcd"));
}

static void testHeap() {
  EmergencyHeap h;
  CHECK(h.empty());
  // (rank, id): High=0, Medium=1, Low=2
  h.push({2, 4});
  h.push({0, 2});
  h.push({1, 3});
  h.push({0, 1});
  CHECK(h.size() == 4);
  CHECK(!h.push({0, 1}));  // duplicate id rejected
  CHECK(h.top().id == 1);  // High, oldest first
  auto order = h.orderedSnapshot();
  CHECK(order.size() == 4);
  CHECK(order[0].id == 1 && order[1].id == 2 && order[2].id == 3 && order[3].id == 4);
  CHECK(h.size() == 4);  // snapshot did not consume the live heap

  CHECK(h.remove(2));  // remove from the middle
  CHECK(!h.remove(2));
  CHECK(!h.contains(2));
  CHECK(h.pop().id == 1);
  CHECK(h.pop().id == 3);
  CHECK(h.pop().id == 4);
  CHECK(h.empty());
  bool threw = false;
  try { h.pop(); } catch (const std::out_of_range&) { threw = true; }
  CHECK(threw);

  // Randomised check against a sorted reference, mixing pushes and removals.
  EmergencyHeap big;
  std::vector<QItem> ref;
  unsigned seed = 12345;
  auto rnd = [&]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
  for (int i = 1; i <= 500; ++i) {
    QItem q{static_cast<int>(rnd() % 3), i};
    big.push(q);
    ref.push_back(q);
    if (i % 7 == 0) {
      size_t victim = rnd() % ref.size();
      CHECK(big.remove(ref[victim].id));
      ref.erase(ref.begin() + victim);
    }
  }
  std::sort(ref.begin(), ref.end(), [](const QItem& a, const QItem& b) {
    return a.rank < b.rank || (a.rank == b.rank && a.id < b.id);
  });
  bool same = big.size() == ref.size();
  for (size_t i = 0; same && i < ref.size(); ++i) {
    QItem got = big.pop();
    same = got.id == ref[i].id && got.rank == ref[i].rank;
  }
  CHECK(same);
}

static void testDijkstra() {
  Graph g;
  for (int i = 1; i <= 6; ++i) g.addNode(i * 10);  // ids 10..60
  CHECK(g.addEdge(10, 20, 7));
  CHECK(g.addEdge(10, 30, 9));
  CHECK(g.addEdge(10, 60, 14));
  CHECK(g.addEdge(20, 30, 10));
  CHECK(g.addEdge(20, 40, 15));
  CHECK(g.addEdge(30, 40, 11));
  CHECK(g.addEdge(30, 60, 2));
  CHECK(g.addEdge(40, 50, 6));
  CHECK(g.addEdge(60, 50, 9));
  CHECK(!g.addEdge(10, 99, 1));   // unknown node
  CHECK(!g.addEdge(10, 20, 0));   // non-positive weight

  auto r = g.dijkstra(10);  // classic textbook example: 10->20=7, 30=9, 60=11, 40=20, 50=20
  CHECK(near(g.distanceTo(r, 10), 0));
  CHECK(near(g.distanceTo(r, 20), 7));
  CHECK(near(g.distanceTo(r, 30), 9));
  CHECK(near(g.distanceTo(r, 60), 11));
  CHECK(near(g.distanceTo(r, 40), 20));
  CHECK(near(g.distanceTo(r, 50), 20));
  auto path = g.pathTo(r, 50);
  CHECK((path == std::vector<int64_t>{10, 30, 60, 50}));
  CHECK((g.pathTo(r, 10) == std::vector<int64_t>{10}));

  // Disconnected node and unknown nodes
  g.addNode(70);
  auto r2 = g.dijkstra(10);
  CHECK(g.distanceTo(r2, 70) == Graph::INF);
  CHECK(g.pathTo(r2, 70).empty());
  CHECK(g.distanceTo(r2, 12345) == Graph::INF);
  auto r3 = g.dijkstra(12345);  // unknown source
  CHECK(g.distanceTo(r3, 10) == Graph::INF);
}

int main() {
  testSha256();
  testHmacPbkdf2();
  testHexRandom();
  testHeap();
  testDijkstra();
  if (failures == 0) std::cout << "test_core: ALL PASSED\n";
  else std::cout << "test_core: " << failures << " FAILURE(S)\n";
  return failures == 0 ? 0 : 1;
}
