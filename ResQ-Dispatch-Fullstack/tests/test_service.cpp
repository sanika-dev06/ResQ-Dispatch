// Integration tests: Service + SQLite + heap + Dijkstra (everything except the HTTP layer).
// Run from the project root:  test_service  (it reads db/*.sql)
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

#include "../src/service.hpp"

static int failures = 0, checks = 0;
#define CHECK(cond)                                                              \
  do {                                                                           \
    ++checks;                                                                    \
    if (!(cond)) {                                                               \
      std::cerr << "FAIL line " << __LINE__ << ": " #cond "\n";                  \
      ++failures;                                                                \
    }                                                                            \
  } while (0)
#define CHECK_RESP(expr, status, needle)                                                        \
  do {                                                                                          \
    const Resp rr__ = (expr); /* evaluated exactly once: many calls have side effects */        \
    ++checks;                                                                                   \
    if (rr__.code != (status) || rr__.body.find(needle) == std::string::npos) {                 \
      std::cerr << "FAIL line " << __LINE__ << ": expected " << (status) << " containing \""   \
                << (needle) << "\" but got " << rr__.code << " " << rr__.body.substr(0, 300)    \
                << "\n";                                                                        \
      ++failures;                                                                               \
    }                                                                                           \
  } while (0)

static std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) { std::cerr << "cannot read " << p << "\n"; std::exit(2); }
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
// Value token after "key": (first occurrence) - good enough for these flat test payloads.
static std::string jget(const std::string& body, const std::string& key, size_t from = 0) {
  auto p = body.find("\"" + key + "\":", from);
  if (p == std::string::npos) return "";
  p += key.size() + 3;
  if (body[p] == '"') return body.substr(p + 1, body.find('"', p + 1) - p - 1);
  return body.substr(p, body.find_first_of(",}]", p) - p);
}
static size_t countOf(const std::string& hay, const std::string& needle) {
  size_t n = 0, p = 0;
  while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
  return n;
}

static const char* DBFILE = "resq_service_test.db";  // created in the working directory (project root)

int main() {
  std::remove(DBFILE);
  std::remove((std::string(DBFILE) + "-wal").c_str());
  std::remove((std::string(DBFILE) + "-shm").c_str());
  const std::string schema = readFile("db/schema.sql"), seed = readFile("db/seed.sql"), demo = readFile("db/demo_data.sql");

  {
    Db db(DBFILE);
    Service svc(db);
    svc.init(schema, seed, demo, "AdminPass#1");

    // ---------------------------------------------------------------- authentication
    Resp r = svc.login("admin", "AdminPass#1");
    CHECK_RESP(r, 200, "\"role\":\"admin\"");
    const std::string adminToken = jget(r.body, "token");
    CHECK(adminToken.size() == 64);
    auto admin = svc.authenticate(adminToken);
    CHECK(admin && admin->username == "admin" && admin->role == "admin");
    CHECK(!svc.authenticate("not-a-token"));
    CHECK(!svc.authenticate(""));
    CHECK_RESP(svc.login("ADMIN", "AdminPass#1"), 200, "token");      // username is case-insensitive
    CHECK_RESP(svc.login("admin", "wrong"), 401, "Invalid username/email or password");
    CHECK_RESP(svc.login("nobody", "whatever1"), 401, "Invalid username/email or password");
    CHECK_RESP(svc.login("", ""), 400, "Please enter both");
    CHECK_RESP(svc.login("admin", "ResQ@123"), 401, "Invalid");       // old hard-coded demo password no longer works

    // signup
    r = svc.signup("Sanika Shelke", "sanika", "Sanika@Example.com", "secret12");
    CHECK_RESP(r, 201, "\"role\":\"dispatcher\"");
    const std::string sToken = jget(r.body, "token");
    auto sanika = svc.authenticate(sToken);
    CHECK(sanika && sanika->email == "sanika@example.com");           // email normalised to lower case
    CHECK_RESP(svc.signup("X", "sanika", "other@example.com", "secret12"), 409, "already registered");
    CHECK_RESP(svc.signup("X", "SANIKA", "other@example.com", "secret12"), 409, "already registered");
    CHECK_RESP(svc.signup("X", "other", "sanika@example.com", "secret12"), 409, "already registered");
    CHECK_RESP(svc.signup("X", "admin", "a@b.co", "secret12"), 400, "reserved");
    CHECK_RESP(svc.signup("X", "Admin", "a@b.co", "secret12"), 400, "reserved");
    CHECK_RESP(svc.signup("X", "ab", "a@b.co", "secret12"), 400, "Username must be");
    CHECK_RESP(svc.signup("X", "bad name", "a@b.co", "secret12"), 400, "Username must be");
    CHECK_RESP(svc.signup("X", "okname", "not-an-email", "secret12"), 400, "valid email");
    CHECK_RESP(svc.signup("X", "okname", "a@b.co", "123"), 400, "Password must be");
    CHECK_RESP(svc.signup("", "okname", "a@b.co", "secret12"), 400, "full name");
    CHECK_RESP(svc.login("sanika@example.com", "secret12"), 200, "\"username\":\"sanika\"");  // login by email
    CHECK_RESP(svc.login("sanika", "Secret12"), 401, "Invalid");      // passwords are case-sensitive

    // logout invalidates the token
    CHECK_RESP(svc.logout(sToken), 200, "\"ok\":true");
    CHECK(!svc.authenticate(sToken));
    r = svc.login("sanika", "secret12");
    const std::string sToken2 = jget(r.body, "token");
    sanika = svc.authenticate(sToken2);
    CHECK(sanika.has_value());

    // brute-force lockout (5 failures -> 429, even for the right password)
    svc.signup("Locky", "locky", "locky@example.com", "rightpass");
    for (int i = 0; i < 5; ++i) CHECK_RESP(svc.login("locky", "bad"), 401, "Invalid");
    CHECK_RESP(svc.login("locky", "bad"), 429, "Too many failed attempts");
    CHECK_RESP(svc.login("locky", "rightpass"), 429, "Too many failed attempts");
    CHECK_RESP(svc.login("sanika", "secret12"), 200, "token");        // other accounts unaffected

    // ---------------------------------------------------------------- profile
    r = svc.updateProfile(*sanika, "  Sanika S.  ", "sanika.new@example.com");
    CHECK_RESP(r, 200, "\"name\":\"Sanika S.\"");
    CHECK(r.body.find("sanika.new@example.com") != std::string::npos);
    CHECK_RESP(svc.updateProfile(*sanika, "", "a@b.co"), 400, "full name");
    CHECK_RESP(svc.updateProfile(*sanika, "S", "bad"), 400, "valid email");
    CHECK_RESP(svc.updateProfile(*sanika, "S", "admin@resqdispatch.local"), 409, "already used");
    CHECK_RESP(svc.updateProfile(*sanika, "Sanika S.", ""), 200, "\"email\":\"\"");          // email may be cleared
    CHECK_RESP(svc.updateProfile(*sanika, "Sanika S.", "sanika.new@example.com"), 200, "sanika.new");
    sanika = svc.authenticate(sToken2);
    CHECK(sanika && sanika->name == "Sanika S.");
    CHECK_RESP(svc.changePassword(*sanika, "wrongcurrent", "newsecret1"), 403, "incorrect");
    CHECK_RESP(svc.changePassword(*sanika, "secret12", "123"), 400, "6 to 128");
    CHECK_RESP(svc.changePassword(*sanika, "secret12", "newsecret1"), 200, "reloginRequired");
    CHECK(!svc.authenticate(sToken2));                                  // old sessions revoked
    CHECK_RESP(svc.login("sanika", "secret12"), 401, "Invalid");
    r = svc.login("sanika", "newsecret1");
    CHECK_RESP(r, 200, "token");
    const std::string sToken3 = jget(r.body, "token");
    sanika = svc.authenticate(sToken3);

    // ---------------------------------------------------------------- notifications
    r = svc.listNotifications(*sanika);
    CHECK_RESP(r, 200, "Dispatch centre ready");
    // 3 welcome + profile updated x3 + password changed = 7 for this user
    CHECK(countOf(r.body, "\"title\"") == 7);
    CHECK(r.body.find("\"unread\":6") != std::string::npos);            // 'Profile available' was seeded as read
    CHECK(r.body.find("Dispatch centre ready") < r.body.find("Priority queue reminder"));  // original ordering
    r = svc.markAllRead(*sanika);
    CHECK_RESP(r, 200, "\"unread\":0");
    CHECK(countOf(r.body, "\"read\":true") == 7);
    CHECK_RESP(svc.listNotifications(*admin), 200, "\"unread\":");      // admin's own list untouched
    CHECK(svc.listNotifications(*admin).body.find("\"unread\":0") == std::string::npos);
    r = svc.clearAll(*sanika);
    CHECK_RESP(r, 200, "\"notifications\":[]");
    CHECK_RESP(svc.listNotifications(*sanika), 200, "\"notifications\":[]");   // really deleted in the DB
    CHECK(countOf(svc.listNotifications(*admin).body, "\"title\"") >= 3);      // other user keeps theirs

    // ---------------------------------------------------------------- initial state
    r = svc.state();
    CHECK(countOf(r.body, "\"createdAt\"") == 4);
    CHECK_RESP(r, 200, "\"queue\":[1,2,3,4]");
    CHECK_RESP(r, 200, "\"dispatchedCount\":28");
    CHECK(r.body.find("\"id\":\"A8\"") != std::string::npos);

    // ---------------------------------------------------------------- emergencies + priority queue
    CHECK_RESP(svc.createEmergency(*sanika, "", "High", "X", "", ""), 400, "emergency type");
    CHECK_RESP(svc.createEmergency(*sanika, "Fever", "Urgent", "X", "", ""), 400, "Priority must be");
    CHECK_RESP(svc.createEmergency(*sanika, "Fever", "Low", "   ", "", ""), 400, "patient location");
    CHECK_RESP(svc.createEmergency(*sanika, "Fever", "Low", std::string(121, 'x'), "", ""), 400, "too long");

    // the <script> text must come back JSON-escaped, never altered or executed server-side
    r = svc.createEmergency(*sanika, "Road Accident", "High", "Baner, Pune", "<b>Ravi</b> \"R\"", "line1\nline2");
    CHECK_RESP(r, 201, "\"queuePosition\":3");                         // behind the two older High requests
    CHECK(jget(r.body, "id") == "5");
    CHECK(r.body.find("\\\"R\\\"") != std::string::npos && r.body.find("line1\\nline2") != std::string::npos);
    CHECK_RESP(svc.createEmergency(*sanika, "Cardiac Arrest", "High", "Kothrud", "", ""), 201, "\"queuePosition\":4");  // id 6
    CHECK_RESP(svc.createEmergency(*sanika, "Fever", "Low", "Mars Colony", "", ""), 201, "\"onRoadGraph\":false");     // id 7
    CHECK_RESP(svc.createEmergency(*sanika, "Fall", "Medium", "FC Road", "", ""), 201, "\"queuePosition\":6");         // id 8: after High 1,2,5,6 and Medium 3
    CHECK_RESP(svc.state(), 200, "\"queue\":[1,2,5,6,3,8,4,7]");

    // ---------------------------------------------------------------- Dijkstra vehicle selection
    // Road Accident at Baner (any unit): candidates A1..A5, nearest is A3 at Shivajinagar, 5.5 km.
    r = svc.nearestPreview(5);
    CHECK_RESP(r, 200, "\"method\":\"dijkstra\"");
    CHECK(r.body.find("\"id\":\"A3\"") != std::string::npos);
    CHECK_RESP(r, 200, "\"distanceKm\":5.50");
    CHECK_RESP(r, 200, "\"route\":\"Shivajinagar > Baner\"");
    // Cardiac Arrest at Kothrud needs ALS. A4 (BLS) is AT Kothrud (0 km) but is not suitable.
    // ALS candidates: A1 (MG Road) and A3 (Shivajinagar); A3 is 6.0 km away, A1 is 8.1 km away.
    r = svc.nearestPreview(6);
    CHECK_RESP(r, 200, "\"method\":\"dijkstra\"");
    CHECK(r.body.find("\"id\":\"A3\"") != std::string::npos);
    CHECK_RESP(r, 200, "\"distanceKm\":6.00");
    CHECK_RESP(r, 200, "\"suitable\":true");
    // Location not on the graph -> honest fallback, no fake distance
    r = svc.nearestPreview(7);
    CHECK_RESP(r, 200, "\"method\":\"fallback\"");
    CHECK_RESP(r, 200, "\"distanceKm\":null");
    CHECK_RESP(r, 200, "not found on the road graph");
    CHECK_RESP(svc.nearestPreview(9999), 404, "not found");
    // preview must not change anything
    CHECK_RESP(svc.state(), 200, "\"queue\":[1,2,5,6,3,8,4,7]");

    // ---------------------------------------------------------------- dispatching
    // No arguments -> head of the priority queue (#1 Cardiac Arrest at MG Road, ALS needed): A1 is at MG Road, 0 km.
    r = svc.dispatch(*sanika, std::nullopt, std::nullopt);
    CHECK_RESP(r, 201, "\"dispatchId\":1");
    CHECK(r.body.find("\"id\":\"A1\"") != std::string::npos);
    CHECK_RESP(r, 201, "\"distanceKm\":0.00");
    CHECK_RESP(r, 201, "\"status\":\"On route\"");
    CHECK_RESP(r, 201, "\"status\":\"Dispatched\"");
    CHECK_RESP(svc.state(), 200, "\"queue\":[2,5,6,3,8,4,7]");
    CHECK_RESP(svc.state(), 200, "\"dispatchedCount\":29");

    // Dispatch a specific request out of order (#5 Road Accident at Baner) -> A3, 5.5 km
    r = svc.dispatch(*sanika, 5, std::nullopt);
    CHECK_RESP(r, 201, "\"dispatchId\":2");
    CHECK(r.body.find("\"id\":\"A3\"") != std::string::npos);
    CHECK_RESP(r, 201, "\"distanceKm\":5.50");
    CHECK_RESP(svc.dispatch(*sanika, 5, std::nullopt), 409, "no longer waiting");     // already dispatched
    CHECK_RESP(svc.dispatch(*sanika, 424242, std::nullopt), 409, "no longer waiting");

    // Both ALS units are now out: Cardiac Arrest (#6) must be refused rather than get a BLS unit
    CHECK_RESP(svc.nearestPreview(6), 409, "needs an ALS unit");
    CHECK_RESP(svc.dispatch(*sanika, 6, std::nullopt), 409, "needs an ALS unit");
    CHECK_RESP(svc.state(), 200, "\"queue\":[2,6,3,8,4,7]");                          // refusal changed nothing

    // Manual override: dispatcher picks A2 (BLS) anyway -> allowed, but flagged
    r = svc.dispatch(*sanika, 6, std::string("A2"));
    CHECK_RESP(r, 201, "\"method\":\"manual\"");
    CHECK_RESP(r, 201, "\"suitable\":false");
    CHECK_RESP(r, 201, "normally needs an ALS unit");
    CHECK_RESP(svc.dispatch(*sanika, 2, std::string("A2")), 409, "No available ambulance");   // A2 now busy
    CHECK_RESP(svc.dispatch(*sanika, 2, std::string("A6")), 409, "No available ambulance");   // A6 is On Duty
    CHECK_RESP(svc.dispatch(*sanika, 2, std::string("ZZ")), 409, "No available ambulance");

    // Top of queue is now #2 (Road Accident, FC Road). Remaining available: A4 (Kothrud), A5 (City Centre).
    // FC Road -> City Centre = 1.0 + 2.4 = 3.4 km; FC Road -> Kothrud = 4.5 km  => A5 wins although A4 has the lower id.
    r = svc.nearestPreview(2);
    CHECK(r.body.find("\"id\":\"A5\"") != std::string::npos);
    CHECK_RESP(r, 200, "\"distanceKm\":3.40");
    r = svc.dispatch(*sanika, std::nullopt, std::nullopt);
    CHECK_RESP(r, 201, "\"dispatchId\":4");
    CHECK(r.body.find("\"id\":\"A5\"") != std::string::npos);

    // history + fleet reflect everything
    r = svc.state();
    CHECK(countOf(r.body, "\"dispatchId\"") == 4);
    CHECK_RESP(r, 200, "\"queue\":[3,8,4,7]");

    // ---------------------------------------------------------------- completing trips
    CHECK_RESP(svc.completeDispatch(*sanika, 1), 200, "\"ambulance\":\"A1\"");
    CHECK_RESP(svc.completeDispatch(*sanika, 1), 409, "already completed");
    CHECK_RESP(svc.completeDispatch(*sanika, 777), 404, "not found");
    r = svc.listVehicles();
    size_t a1 = r.body.find("\"id\":\"A1\"");
    CHECK(r.body.find("\"status\":\"Available\"", a1) < r.body.find("}", a1));
    CHECK(r.body.find("\"location\":\"City Centre\"", a1) < r.body.find("}", a1));
    // A1 is back and ALS: the previously refused Cardiac Arrest case... (#6 was already assigned manually),
    // so create a new one and confirm A1 (now at City Centre) is chosen.
    r = svc.createEmergency(*sanika, "Breathing Issue", "High", "Deccan", "", "");   // id 9
    CHECK(jget(r.body, "id") == "9");
    r = svc.nearestPreview(9);
    CHECK(r.body.find("\"id\":\"A1\"") != std::string::npos);
    CHECK_RESP(r, 200, "\"distanceKm\":2.40");
    CHECK_RESP(r, 200, "\"route\":\"City Centre > Deccan\"");

    // ---------------------------------------------------------------- vehicle management
    CHECK_RESP(svc.createVehicle(*sanika, "A9", "ALS", "Hadapsar"), 403, "Administrator");
    CHECK_RESP(svc.createVehicle(*admin, "A9", "ALS", "Hadapsar"), 201, "\"onRoadGraph\":true");
    CHECK_RESP(svc.createVehicle(*admin, "a9", "ALS", "Hadapsar"), 409, "already exists");
    CHECK_RESP(svc.createVehicle(*admin, "B1", "XYZ", "Hadapsar"), 400, "ALS or BLS");
    CHECK_RESP(svc.createVehicle(*admin, "bad id", "ALS", "Hadapsar"), 400, "Vehicle id");
    CHECK_RESP(svc.createVehicle(*admin, "B2", "BLS", "Nowhere Land"), 201, "\"onRoadGraph\":false");
    CHECK_RESP(svc.updateVehicle(*sanika, "A9", std::nullopt, std::nullopt, std::string("Maintenance")), 403, "Administrator");
    CHECK_RESP(svc.updateVehicle(*admin, "A9", std::nullopt, std::nullopt, std::string("Maintenance")), 200, "\"status\":\"Maintenance\"");
    CHECK_RESP(svc.updateVehicle(*admin, "A9", std::nullopt, std::nullopt, std::string("On route")), 400, "Status must be");
    CHECK_RESP(svc.updateVehicle(*admin, "A9", std::string("BLS"), std::string("Baner"), std::nullopt), 200, "\"type\":\"BLS\"");
    CHECK_RESP(svc.updateVehicle(*admin, "NOPE", std::nullopt, std::nullopt, std::nullopt), 404, "not found");
    CHECK_RESP(svc.updateVehicle(*admin, "A2", std::nullopt, std::nullopt, std::string("Available")), 409, "on a trip");
    CHECK_RESP(svc.deleteVehicle(*admin, "A2"), 409, "on a trip");
    CHECK_RESP(svc.deleteVehicle(*admin, "A1"), 409, "dispatch history");   // referenced by dispatches
    CHECK_RESP(svc.deleteVehicle(*sanika, "B2"), 403, "Administrator");
    CHECK_RESP(svc.deleteVehicle(*admin, "B2"), 200, "\"ok\":true");
    CHECK_RESP(svc.deleteVehicle(*admin, "B2"), 404, "not found");
    // a vehicle under maintenance is never dispatched
    CHECK_RESP(svc.updateVehicle(*admin, "A9", std::nullopt, std::nullopt, std::string("Maintenance")), 200, "Maintenance");
    r = svc.nearestPreview(3);   // Breathing Issue at Shivajinagar (needs ALS): A1 is the only available ALS
    CHECK(r.body.find("\"id\":\"A1\"") != std::string::npos);

    // ---------------------------------------------------------------- admin
    CHECK_RESP(svc.listUsers(*sanika), 403, "Administrator");
    r = svc.listUsers(*admin);
    CHECK_RESP(r, 200, "\"username\":\"admin\"");
    CHECK(r.body.find("password") == std::string::npos && r.body.find("salt") == std::string::npos);   // no secrets leak
    CHECK_RESP(svc.resetDemo(*sanika), 403, "administrator");
    CHECK_RESP(svc.resetDemo(*admin), 200, "\"ok\":true");
    r = svc.state();
    CHECK(countOf(r.body, "\"dispatchId\"") == 0);
    CHECK_RESP(r, 200, "\"queue\":[1,2,3,4]");
    CHECK_RESP(r, 200, "\"dispatchedCount\":28");
    r = svc.createEmergency(*sanika, "Fever", "Low", "Kothrud", "", "");
    CHECK(jget(r.body, "id") == "5");                                    // ids restart after reset
    CHECK_RESP(svc.dispatch(*sanika, std::nullopt, std::nullopt), 201, "\"dispatchId\":1");
    CHECK(svc.authenticate(adminToken).has_value());                     // sessions/users survive a reset
  }

  // ---------------------------------------------------------------- persistence across restarts
  {
    Db db(DBFILE);
    Service svc(db);
    svc.init(schema, seed, demo, "SomethingElse#9");   // second start: must NOT reseed or change the admin password
    CHECK_RESP(svc.login("admin", "AdminPass#1"), 200, "token");
    CHECK_RESP(svc.login("admin", "SomethingElse#9"), 401, "Invalid");
    CHECK_RESP(svc.login("sanika", "newsecret1"), 200, "token");
    Resp r = svc.state();
    CHECK_RESP(r, 200, "\"dispatchedCount\":29");                        // 28 baseline + 1 dispatch made after the reset
    CHECK(countOf(r.body, "\"dispatchId\"") == 1);
    CHECK_RESP(r, 200, "\"queue\":[2,3,4,5]");                           // heap rebuilt from the database
    auto admin = svc.authenticate("definitely-not-valid");
    CHECK(!admin);
    int64_t users = 0;
    Stmt s(db, "SELECT COUNT(*) FROM users WHERE username='admin'");
    if (s.step()) users = s.getInt(0);
    CHECK(users == 1);
  }

  std::remove(DBFILE);
  std::remove((std::string(DBFILE) + "-wal").c_str());
  std::remove((std::string(DBFILE) + "-shm").c_str());
  std::cout << (failures == 0 ? "test_service: ALL PASSED" : "test_service: FAILURES") << "  (" << checks
            << " checks, " << failures << " failed)\n";
  return failures == 0 ? 0 : 1;
}
