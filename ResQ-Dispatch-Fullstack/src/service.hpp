// service.hpp - ResQ Dispatch business logic.
//
// Everything the API does lives here (authentication, profile, notifications,
// emergencies, priority-queue dispatching, Dijkstra vehicle selection, vehicle
// management). main.cpp only translates HTTP <-> these calls, so this layer can
// be tested without the web framework (see tests/test_service.cpp).
//
// Thread safety: every public method takes one mutex, so Crow's worker threads
// are serialised around the single SQLite connection.
#pragma once
#include <algorithm>
#include <chrono>
#include <cctype>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "crypto.hpp"
#include "db.hpp"
#include "graph.hpp"
#include "jsonw.hpp"
#include "pqueue.hpp"

// Current UTC time as ISO-8601 text, evaluated by SQLite.
#define NOW_SQL "strftime('%Y-%m-%dT%H:%M:%SZ','now')"

struct Resp {
  int code;
  std::string body;  // always JSON
};

struct User {
  int64_t id = 0;
  std::string name, username, email, role, createdAt;
};

class Service {
 public:
  explicit Service(Db& db) : db_(db) {}

  // ---------------------------------------------------------------- start-up
  void init(const std::string& schemaSql, const std::string& graphSeedSql, const std::string& demoSql,
            const std::string& adminPassword) {
    std::lock_guard<std::mutex> lk(mu_);
    db_.exec(schemaSql);
    demoSql_ = demoSql;
    if (metaGet("seeded").empty()) {
      Tx tx(db_);
      db_.exec(graphSeedSql);
      db_.exec(demoSql_);
      {
        Stmt s(db_, "INSERT OR REPLACE INTO meta(key,value) VALUES('seeded','1')");
        s.run();
      }
      tx.commit();
    }
    bool adminExists = false;
    {
      Stmt s(db_, "SELECT 1 FROM users WHERE username='admin'");
      adminExists = s.step();
    }
    if (!adminExists) {
      Tx tx(db_);
      int64_t id = insertUser("ResQ Admin", "admin", "admin@resqdispatch.local", adminPassword, "admin");
      welcomeNotifications(id);
      tx.commit();
    }
    loadGraph();
    rebuildHeap();
  }

  // ---------------------------------------------------------------- auth
  Resp login(const std::string& identifier, const std::string& password) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      const std::string key = lower(trim(identifier));
      if (key.empty() || password.empty()) return err(400, "Please enter both your username/email and password.");
      if (key.size() > 120 || password.size() > 128) return err(401, kBadLogin);

      auto now = std::chrono::steady_clock::now();
      auto f = fails_.find(key);
      if (f != fails_.end() && f->second.until > now)
        return err(429, "Too many failed attempts. Please wait a minute and try again.");

      std::optional<User> user;
      std::string saltHex, hashHex;
      uint32_t iters = kIterations;
      {
        Stmt s(db_,
               "SELECT id,name,username,email,role,created_at,salt,password_hash,iterations FROM users "
               "WHERE username=?1 OR (email<>'' AND lower(email)=?1)");
        s.bindText(1, key);
        if (s.step()) {
          user = readUser(s);
          saltHex = s.getText(6);
          hashHex = s.getText(7);
          iters = static_cast<uint32_t>(s.getInt(8));
        }
      }
      bool good = false;
      if (user) {
        good = crypto::constantTimeEquals(crypto::pbkdf2Hex(password, crypto::fromHex(saltHex), iters), hashHex);
      } else {
        (void)crypto::pbkdf2Hex(password, crypto::Bytes(16, 0x42), kIterations);  // equalise timing
      }
      if (!good) {
        Fail& fl = fails_[key];
        if (++fl.count >= 5) {
          fl.count = 0;
          fl.until = now + std::chrono::seconds(60);
        }
        return err(401, kBadLogin);
      }
      fails_.erase(key);
      return sessionResp(200, *user);
    });
  }

  Resp signup(const std::string& nameIn, const std::string& usernameIn, const std::string& emailIn,
              const std::string& password) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      const std::string name = trim(nameIn), username = trim(usernameIn), email = lower(trim(emailIn));
      if (name.empty() || name.size() > 70) return err(400, "Please enter your full name.");
      if (!validUsername(username)) return err(400, "Username must be 3-30 characters (letters, numbers, . _ -).");
      if (lower(username) == "admin") return err(400, "That username is reserved. Please choose another.");
      if (!validEmail(email)) return err(400, "Please enter a valid email address.");
      if (password.size() < 6 || password.size() > 128) return err(400, "Password must be 6 to 128 characters.");
      {
        Stmt s(db_, "SELECT 1 FROM users WHERE username=?1 OR lower(email)=?2");
        s.bindText(1, username).bindText(2, email);
        if (s.step()) return err(409, "That username or email is already registered. Please sign in instead.");
      }
      int64_t id = 0;
      {
        Tx tx(db_);
        id = insertUser(name, username, email, password, "dispatcher");
        welcomeNotifications(id);
        tx.commit();
      }
      auto u = loadUser(id);
      return sessionResp(201, *u);
    });
  }

  Resp logout(const std::string& token) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      if (!token.empty()) {
        Stmt s(db_, "DELETE FROM sessions WHERE token_hash=?");
        s.bindText(1, crypto::sha256Hex(token));
        s.run();
      }
      return ok(200, jsonw::Obj().boolean("ok", true).done());
    });
  }

  std::optional<User> authenticate(const std::string& token) {
    if (token.empty() || token.size() > 200) return std::nullopt;
    std::lock_guard<std::mutex> lk(mu_);
    try {
      Stmt s(db_,
             "SELECT u.id,u.name,u.username,u.email,u.role,u.created_at FROM sessions s "
             "JOIN users u ON u.id=s.user_id WHERE s.token_hash=? AND s.expires_at > " NOW_SQL);
      s.bindText(1, crypto::sha256Hex(token));
      if (s.step()) return readUser(s);
    } catch (const std::exception& e) {
      std::cerr << "[auth] " << e.what() << "\n";
    }
    return std::nullopt;
  }

  Resp me(const User& u) { return ok(200, jsonw::Obj().raw("user", userJson(u)).done()); }

  // ---------------------------------------------------------------- profile
  Resp updateProfile(const User& actor, const std::string& nameIn, const std::string& emailIn) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      const std::string name = trim(nameIn), email = lower(trim(emailIn));
      if (name.empty() || name.size() > 70) return err(400, "Please enter your full name.");
      if (!email.empty() && !validEmail(email)) return err(400, "Please enter a valid email address.");
      if (!email.empty()) {
        Stmt s(db_, "SELECT 1 FROM users WHERE lower(email)=?1 AND id<>?2");
        s.bindText(1, email).bindInt(2, actor.id);
        if (s.step()) return err(409, "That email is already used by another account.");
      }
      {
        Tx tx(db_);
        {
          Stmt s(db_, "UPDATE users SET name=?1, email=?2 WHERE id=?3");
          s.bindText(1, name).bindText(2, email).bindInt(3, actor.id);
          s.run();
        }
        notifyUser(actor.id, "Profile updated", "Your profile information was saved.", kIconCheck);
        tx.commit();
      }
      return ok(200, jsonw::Obj().raw("user", userJson(*loadUser(actor.id))).done());
    });
  }

  Resp changePassword(const User& actor, const std::string& current, const std::string& next) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      if (next.size() < 6 || next.size() > 128) return err(400, "New password must be 6 to 128 characters.");
      std::string saltHex, hashHex;
      uint32_t iters = kIterations;
      {
        Stmt s(db_, "SELECT salt,password_hash,iterations FROM users WHERE id=?");
        s.bindInt(1, actor.id);
        if (!s.step()) return err(404, "Account not found.");
        saltHex = s.getText(0);
        hashHex = s.getText(1);
        iters = static_cast<uint32_t>(s.getInt(2));
      }
      if (!crypto::constantTimeEquals(crypto::pbkdf2Hex(current, crypto::fromHex(saltHex), iters), hashHex))
        return err(403, "Current password is incorrect.");
      const crypto::Bytes salt = crypto::randomBytes(16);
      Tx tx(db_);
      {
        Stmt s(db_, "UPDATE users SET password_hash=?1, salt=?2, iterations=?3 WHERE id=?4");
        s.bindText(1, crypto::pbkdf2Hex(next, salt, kIterations)).bindText(2, crypto::toHex(salt));
        s.bindInt(3, kIterations).bindInt(4, actor.id);
        s.run();
      }
      {
        Stmt s(db_, "DELETE FROM sessions WHERE user_id=?");  // force re-login everywhere
        s.bindInt(1, actor.id);
        s.run();
      }
      notifyUser(actor.id, "Password changed", "Your password was updated.", kIconCheck);
      tx.commit();
      return ok(200, jsonw::Obj().boolean("ok", true).boolean("reloginRequired", true).done());
    });
  }

  // ---------------------------------------------------------------- notifications
  Resp listNotifications(const User& u) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp { return ok(200, notificationsJson(u.id)); });
  }

  Resp markAllRead(const User& u) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      Stmt s(db_, "UPDATE notifications SET is_read=1 WHERE user_id=?");
      s.bindInt(1, u.id);
      s.run();
      return ok(200, notificationsJson(u.id));
    });
  }

  Resp clearAll(const User& u) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      Stmt s(db_, "DELETE FROM notifications WHERE user_id=?");
      s.bindInt(1, u.id);
      s.run();
      return ok(200, notificationsJson(u.id));
    });
  }

  // ---------------------------------------------------------------- dashboard state
  Resp state() {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      std::vector<std::string> ems, fleet, hist, queue;
      {
        Stmt s(db_, std::string("SELECT ") + kEmCols +
                        " FROM emergencies WHERE status<>'Completed' OR id > "
                        "(SELECT COALESCE(MAX(id),0)-100 FROM emergencies) ORDER BY id");
        while (s.step()) ems.push_back(emJson(readEm(s)));
      }
      {
        Stmt s(db_, std::string("SELECT ") + kVehCols + " FROM vehicles ORDER BY length(id), id");
        while (s.step()) fleet.push_back(vehJson(readVeh(s)));
      }
      {
        Stmt s(db_,
               "SELECT d.id,d.emergency_id,d.vehicle_id,d.status,d.distance_km,d.route,d.method,d.dispatched_at,"
               "d.completed_at,e.type,e.priority,e.location FROM dispatches d "
               "JOIN emergencies e ON e.id=d.emergency_id ORDER BY d.id DESC LIMIT 200");
        while (s.step()) {
          jsonw::Obj o;
          o.num("dispatchId", s.getInt(0)).num("id", s.getInt(1)).str("ambulance", s.getText(2));
          o.str("status", s.getText(3));
          if (s.isNull(4)) o.null("distanceKm"); else o.real("distanceKm", s.getReal(4));
          o.str("route", s.getText(5)).str("method", s.getText(6)).str("time", s.getText(7));
          if (s.isNull(8)) o.null("completedAt"); else o.str("completedAt", s.getText(8));
          o.str("type", s.getText(9)).str("priority", s.getText(10)).str("location", s.getText(11));
          hist.push_back(o.done());
        }
      }
      for (const QItem& q : heap_.orderedSnapshot()) queue.push_back(std::to_string(q.id));

      int64_t count = 0;
      {
        Stmt s(db_, "SELECT COUNT(*) FROM dispatches");
        if (s.step()) count = s.getInt(0);
      }
      const int64_t baseline = parseInt(metaGet("dispatched_baseline"), 0);
      std::string stats = jsonw::Obj().num("dispatchedCount", baseline + count).done();
      return ok(200, jsonw::Obj()
                         .raw("emergencies", jsonw::arr(ems))
                         .raw("fleet", jsonw::arr(fleet))
                         .raw("history", jsonw::arr(hist))
                         .raw("queue", jsonw::arr(queue))
                         .raw("stats", stats)
                         .done());
    });
  }

  // ---------------------------------------------------------------- emergencies
  Resp createEmergency(const User& actor, const std::string& typeIn, const std::string& priorityIn,
                       const std::string& locationIn, const std::string& patientIn, const std::string& detailsIn) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      const std::string type = trim(typeIn), priority = trim(priorityIn), location = trim(locationIn);
      const std::string patient = trim(patientIn), details = trim(detailsIn);
      if (type.empty() || type.size() > 60) return err(400, "Please choose the emergency type.");
      if (priority != "High" && priority != "Medium" && priority != "Low")
        return err(400, "Priority must be High, Medium or Low.");
      if (location.empty()) return err(400, "Please enter the patient location.");
      if (location.size() > 120) return err(400, "Location is too long (max 120 characters).");
      if (patient.size() > 80) return err(400, "Patient name is too long (max 80 characters).");
      if (details.size() > 500) return err(400, "Details are too long (max 500 characters).");

      const std::optional<int64_t> node = resolveNode(location);
      int64_t id = 0;
      {
        Tx tx(db_);
        {
          Stmt s(db_,
                 "INSERT INTO emergencies(type,priority,location,node_id,patient,details,status,created_by) "
                 "VALUES(?1,?2,?3,?4,?5,?6,'Waiting',?7)");
          s.bindText(1, type).bindText(2, priority).bindText(3, location);
          if (node) s.bindInt(4, *node); else s.bindNull(4);
          s.bindText(5, patient).bindText(6, details).bindInt(7, actor.id);
          s.run();
        }
        id = db_.lastInsertId();
        notifyAll("New emergency", priority + " priority: " + type + " at " + location + ".", kIconAlert);
        tx.commit();
      }
      heap_.push({rankOf(priority), id});
      EmRow e;
      loadEmergency(id, e);
      return ok(201, jsonw::Obj().raw("emergency", emJson(e)).num("queuePosition", queuePosition(id)).done());
    });
  }

  // Which vehicle would Dijkstra pick for this emergency? Read-only preview.
  Resp nearestPreview(int64_t emergencyId) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      EmRow em;
      if (!loadEmergency(emergencyId, em)) return err(404, "Emergency not found.");
      Selection sel = selectVehicle(em, std::nullopt);
      if (!sel.ok) return err(sel.code, sel.error);
      return ok(200, selectionJson(sel).done());
    });
  }

  // ---------------------------------------------------------------- dispatching
  // requestId absent  -> take the top of the priority queue.
  // ambulanceId absent -> Dijkstra picks the nearest suitable available vehicle.
  Resp dispatch(const User& actor, std::optional<int64_t> requestId, std::optional<std::string> ambulanceId) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      int64_t eid = 0;
      if (requestId) {
        if (!heap_.contains(*requestId)) return err(409, "This emergency is no longer waiting.");
        eid = *requestId;
      } else {
        while (true) {
          if (heap_.empty()) return err(409, "No waiting emergencies to dispatch.");
          eid = heap_.top().id;
          EmRow probe;
          if (loadEmergency(eid, probe) && probe.status == "Waiting") break;
          heap_.pop();  // stale entry, drop it
        }
      }
      EmRow em;
      if (!loadEmergency(eid, em) || em.status != "Waiting") {
        heap_.remove(eid);
        return err(409, "This emergency is no longer waiting.");
      }
      Selection sel = selectVehicle(em, ambulanceId);
      if (!sel.ok) return err(sel.code, sel.error);

      int64_t did = 0;
      {
        Tx tx(db_);
        {
          Stmt s(db_, "UPDATE emergencies SET status='Dispatched' WHERE id=?");
          s.bindInt(1, em.id);
          s.run();
        }
        {
          Stmt s(db_, "UPDATE vehicles SET status='On route', location=?1, node_id=?2 WHERE id=?3");
          s.bindText(1, em.location);
          std::optional<int64_t> node = em.nodeId ? em.nodeId : sel.veh.nodeId;
          if (node) s.bindInt(2, *node); else s.bindNull(2);
          s.bindText(3, sel.veh.id);
          s.run();
        }
        {
          Stmt s(db_,
                 "INSERT INTO dispatches(emergency_id,vehicle_id,status,distance_km,route,method,dispatched_by) "
                 "VALUES(?1,?2,'In Progress',?3,?4,?5,?6)");
          s.bindInt(1, em.id).bindText(2, sel.veh.id);
          if (sel.dist != Graph::INF) s.bindReal(3, sel.dist); else s.bindNull(3);
          s.bindText(4, sel.route).bindText(5, sel.method).bindInt(6, actor.id);
          s.run();
        }
        did = db_.lastInsertId();
        notifyAll("Ambulance dispatched", "Ambulance " + sel.veh.id + " assigned to " + em.type + " at " + em.location + ".",
                  kIconAmbulance);
        tx.commit();
      }
      heap_.remove(eid);

      EmRow after;
      loadEmergency(eid, after);
      sel.veh.status = "On route";
      sel.veh.location = em.location;
      return ok(201, selectionJson(sel).num("dispatchId", did).raw("emergency", emJson(after)).done());
    });
  }

  Resp completeDispatch(const User&, int64_t dispatchId) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      int64_t eid = 0;
      std::string vid, status;
      {
        Stmt s(db_, "SELECT emergency_id,vehicle_id,status FROM dispatches WHERE id=?");
        s.bindInt(1, dispatchId);
        if (!s.step()) return err(404, "Dispatch record not found.");
        eid = s.getInt(0);
        vid = s.getText(1);
        status = s.getText(2);
      }
      if (status != "In Progress") return err(409, "This trip is already completed.");
      const std::optional<int64_t> city = nodeByName("city centre");
      {
        Tx tx(db_);
        {
          Stmt s(db_, "UPDATE dispatches SET status='Completed', completed_at=" NOW_SQL " WHERE id=?");
          s.bindInt(1, dispatchId);
          s.run();
        }
        {
          Stmt s(db_, "UPDATE emergencies SET status='Completed' WHERE id=?");
          s.bindInt(1, eid);
          s.run();
        }
        {
          Stmt s(db_, "UPDATE vehicles SET status='Available', location='City Centre', node_id=?1 WHERE id=?2 AND status='On route'");
          if (city) s.bindInt(1, *city); else s.bindNull(1);
          s.bindText(2, vid);
          s.run();
        }
        notifyAll("Trip completed", "Ambulance " + vid + " is available again.", kIconCheck);
        tx.commit();
      }
      return ok(200, jsonw::Obj().boolean("ok", true).num("dispatchId", dispatchId).str("ambulance", vid).done());
    });
  }

  // ---------------------------------------------------------------- vehicles
  Resp listVehicles() {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      std::vector<std::string> out;
      Stmt s(db_, std::string("SELECT ") + kVehCols + " FROM vehicles ORDER BY length(id), id");
      while (s.step()) out.push_back(vehJson(readVeh(s)));
      return ok(200, jsonw::Obj().raw("vehicles", jsonw::arr(out)).done());
    });
  }

  Resp createVehicle(const User& actor, const std::string& idIn, const std::string& typeIn,
                     const std::string& locationIn) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      if (actor.role != "admin") return err(403, "Administrator access required.");
      const std::string id = upper(trim(idIn)), type = upper(trim(typeIn)), location = trim(locationIn);
      if (!validVehicleId(id)) return err(400, "Vehicle id must be 1-10 characters (letters, numbers, - _).");
      if (type != "ALS" && type != "BLS") return err(400, "Vehicle type must be ALS or BLS.");
      if (location.empty() || location.size() > 60) return err(400, "Please enter the vehicle's base location.");
      VehRow existing;
      if (loadVehicle(id, existing)) return err(409, "A vehicle with that id already exists.");
      const std::optional<int64_t> node = resolveNode(location);
      {
        Tx tx(db_);
        {
          Stmt s(db_, "INSERT INTO vehicles(id,type,status,location,node_id) VALUES(?1,?2,'Available',?3,?4)");
          s.bindText(1, id).bindText(2, type).bindText(3, location);
          if (node) s.bindInt(4, *node); else s.bindNull(4);
          s.run();
        }
        notifyAll("Vehicle added", "Ambulance " + id + " (" + type + ") joined the fleet.", kIconAmbulance);
        tx.commit();
      }
      VehRow v;
      loadVehicle(id, v);
      return ok(201, jsonw::Obj().raw("vehicle", vehJson(v)).boolean("onRoadGraph", node.has_value()).done());
    });
  }

  Resp updateVehicle(const User& actor, const std::string& idIn, const std::optional<std::string>& type,
                     const std::optional<std::string>& location, const std::optional<std::string>& status) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      if (actor.role != "admin") return err(403, "Administrator access required.");
      VehRow v;
      if (!loadVehicle(upper(trim(idIn)), v)) return err(404, "Vehicle not found.");
      if (v.status == "On route") return err(409, "This vehicle is on a trip. Complete the trip first.");
      if (type) {
        const std::string t = upper(trim(*type));
        if (t != "ALS" && t != "BLS") return err(400, "Vehicle type must be ALS or BLS.");
        v.type = t;
      }
      if (status) {
        const std::string st = trim(*status);
        if (st != "Available" && st != "On Duty" && st != "Maintenance")
          return err(400, "Status must be Available, On Duty or Maintenance.");
        v.status = st;
      }
      if (location) {
        const std::string loc = trim(*location);
        if (loc.empty() || loc.size() > 60) return err(400, "Please enter a valid location.");
        v.location = loc;
        v.nodeId = resolveNode(loc);
      }
      Stmt s(db_, "UPDATE vehicles SET type=?1,status=?2,location=?3,node_id=?4 WHERE id=?5");
      s.bindText(1, v.type).bindText(2, v.status).bindText(3, v.location);
      if (v.nodeId) s.bindInt(4, *v.nodeId); else s.bindNull(4);
      s.bindText(5, v.id);
      s.run();
      return ok(200, jsonw::Obj().raw("vehicle", vehJson(v)).done());
    });
  }

  Resp deleteVehicle(const User& actor, const std::string& idIn) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      if (actor.role != "admin") return err(403, "Administrator access required.");
      VehRow v;
      if (!loadVehicle(upper(trim(idIn)), v)) return err(404, "Vehicle not found.");
      if (v.status == "On route") return err(409, "This vehicle is on a trip. Complete the trip first.");
      try {
        Stmt s(db_, "DELETE FROM vehicles WHERE id=?");
        s.bindText(1, v.id);
        s.run();
      } catch (const DbError& e) {
        if (e.isConstraint())
          return err(409, "This vehicle has dispatch history and cannot be deleted. Set it to Maintenance instead.");
        throw;
      }
      return ok(200, jsonw::Obj().boolean("ok", true).done());
    });
  }

  // ---------------------------------------------------------------- admin
  Resp resetDemo(const User& actor) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      if (actor.role != "admin") return err(403, "Only an administrator can reset records.");
      {
        Tx tx(db_);
        db_.exec(demoSql_);
        notifyAll("Demo data reset", "Emergencies, fleet and dispatch history were reset.", kIconCheck);
        tx.commit();
      }
      rebuildHeap();
      return ok(200, jsonw::Obj().boolean("ok", true).done());
    });
  }

  Resp listUsers(const User& actor) {
    std::lock_guard<std::mutex> lk(mu_);
    return guarded([&]() -> Resp {
      if (actor.role != "admin") return err(403, "Administrator access required.");
      std::vector<std::string> out;
      Stmt s(db_, "SELECT id,name,username,email,role,created_at FROM users ORDER BY id");
      while (s.step()) out.push_back(userJson(readUser(s)));
      return ok(200, jsonw::Obj().raw("users", jsonw::arr(out)).done());
    });
  }

 private:
  // ---------------------------------------------------------------- constants
  static constexpr uint32_t kIterations = 100000;
  static constexpr const char* kBadLogin = "Invalid username/email or password. Please try again or create an account.";
  // UTF-8 byte sequences (kept as escapes so the source compiles regardless of the compiler's source charset).
  static constexpr const char* kIconCheck = "\xE2\x9C\x93";          // check mark
  static constexpr const char* kIconBolt = "\xE2\x9A\xA1";           // high voltage
  static constexpr const char* kIconPerson = "\xF0\x9F\x91\xA4";     // bust in silhouette
  static constexpr const char* kIconAlert = "\xF0\x9F\x9A\xA8";      // police car light
  static constexpr const char* kIconAmbulance = "\xF0\x9F\x9A\x91";  // ambulance
  static constexpr const char* kEmCols = "id,type,priority,location,patient,details,status,created_at,node_id";
  static constexpr const char* kVehCols = "id,type,status,location,node_id";

  struct EmRow {
    int64_t id = 0;
    std::string type, priority, location, patient, details, status, createdAt;
    std::optional<int64_t> nodeId;
  };
  struct VehRow {
    std::string id, type, status, location;
    std::optional<int64_t> nodeId;
  };
  struct NodeInfo {
    int64_t id;
    std::string name;
    std::vector<std::string> keys;  // lower-case name + aliases
  };
  struct Fail {
    int count = 0;
    std::chrono::steady_clock::time_point until{};
  };
  struct Selection {
    bool ok = false;
    int code = 409;
    std::string error;
    VehRow veh;
    double dist = Graph::INF;
    std::string method;  // dijkstra | manual | fallback
    bool suitable = true;
    std::string warning;
    std::string route;
  };

  // ---------------------------------------------------------------- small helpers
  static Resp err(int code, const std::string& msg) { return {code, jsonw::Obj().str("error", msg).done()}; }
  static Resp ok(int code, const std::string& body) { return {code, body}; }

  template <class F>
  Resp guarded(F&& f) {
    try {
      return f();
    } catch (const DbError& e) {
      std::cerr << "[db] " << e.what() << "\n";
      if (e.isConstraint()) return err(409, "That change conflicts with existing data.");
      return err(500, "Internal server error.");
    } catch (const std::exception& e) {
      std::cerr << "[error] " << e.what() << "\n";
      return err(500, "Internal server error.");
    }
  }

  static std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
  }
  static std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  }
  static std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
  }
  static int64_t parseInt(const std::string& s, int64_t fallback) {
    try {
      return s.empty() ? fallback : std::stoll(s);
    } catch (...) {
      return fallback;
    }
  }
  static bool validUsername(const std::string& s) {
    if (s.size() < 3 || s.size() > 30) return false;
    for (unsigned char c : s)
      if (!(std::isalnum(c) || c == '_' || c == '.' || c == '-')) return false;
    return true;
  }
  static bool validVehicleId(const std::string& s) {
    if (s.empty() || s.size() > 10) return false;
    for (unsigned char c : s)
      if (!(std::isalnum(c) || c == '_' || c == '-')) return false;
    return true;
  }
  static bool validEmail(const std::string& s) {
    if (s.size() < 5 || s.size() > 120) return false;
    const size_t at = s.find('@');
    if (at == std::string::npos || at == 0 || at != s.rfind('@')) return false;
    const size_t dot = s.find('.', at);
    if (dot == std::string::npos || dot == at + 1 || dot + 1 >= s.size()) return false;
    for (unsigned char c : s)
      if (std::isspace(c) || c < 0x20) return false;
    return true;
  }
  static int rankOf(const std::string& p) { return p == "High" ? 0 : (p == "Medium" ? 1 : 2); }
  static bool needsAls(const std::string& emergencyType) {
    return emergencyType == "Cardiac Arrest" || emergencyType == "Breathing Issue";
  }
  static bool suitableFor(const std::string& emergencyType, const std::string& vehicleType) {
    return !needsAls(emergencyType) || vehicleType == "ALS";
  }

  std::string metaGet(const std::string& key) {
    Stmt s(db_, "SELECT value FROM meta WHERE key=?");
    s.bindText(1, key);
    return s.step() ? s.getText(0) : std::string();
  }

  // ---------------------------------------------------------------- users / sessions
  static User readUser(Stmt& s) {
    User u;
    u.id = s.getInt(0);
    u.name = s.getText(1);
    u.username = s.getText(2);
    u.email = s.getText(3);
    u.role = s.getText(4);
    u.createdAt = s.getText(5);
    return u;
  }
  static std::string userJson(const User& u) {
    return jsonw::Obj()
        .num("id", u.id).str("name", u.name).str("username", u.username).str("email", u.email)
        .str("role", u.role).str("createdAt", u.createdAt).done();
  }
  std::optional<User> loadUser(int64_t id) {
    Stmt s(db_, "SELECT id,name,username,email,role,created_at FROM users WHERE id=?");
    s.bindInt(1, id);
    if (s.step()) return readUser(s);
    return std::nullopt;
  }
  int64_t insertUser(const std::string& name, const std::string& username, const std::string& email,
                     const std::string& password, const std::string& role) {
    const crypto::Bytes salt = crypto::randomBytes(16);
    Stmt s(db_, "INSERT INTO users(name,username,email,password_hash,salt,iterations,role) VALUES(?1,?2,?3,?4,?5,?6,?7)");
    s.bindText(1, name).bindText(2, username).bindText(3, email);
    s.bindText(4, crypto::pbkdf2Hex(password, salt, kIterations)).bindText(5, crypto::toHex(salt));
    s.bindInt(6, kIterations).bindText(7, role);
    s.run();
    return db_.lastInsertId();
  }
  Resp sessionResp(int code, const User& u) {
    const std::string token = crypto::randomHex(32);
    {
      Stmt s(db_,
             "INSERT INTO sessions(token_hash,user_id,expires_at) VALUES(?1,?2,"
             "strftime('%Y-%m-%dT%H:%M:%SZ','now','+12 hours'))");
      s.bindText(1, crypto::sha256Hex(token)).bindInt(2, u.id);
      s.run();
    }
    {
      Stmt s(db_, "DELETE FROM sessions WHERE expires_at <= " NOW_SQL);
      s.run();
    }
    return ok(code, jsonw::Obj().str("token", token).raw("user", userJson(u)).done());
  }

  // ---------------------------------------------------------------- notifications
  void notifyUser(int64_t uid, const std::string& title, const std::string& msg, const char* icon, bool read = false) {
    {
      Stmt s(db_, "INSERT INTO notifications(user_id,title,message,icon,is_read) VALUES(?1,?2,?3,?4,?5)");
      s.bindInt(1, uid).bindText(2, title).bindText(3, msg).bindText(4, icon).bindInt(5, read ? 1 : 0);
      s.run();
    }
    Stmt d(db_,
           "DELETE FROM notifications WHERE user_id=?1 AND id NOT IN "
           "(SELECT id FROM notifications WHERE user_id=?1 ORDER BY id DESC LIMIT 30)");
    d.bindInt(1, uid);
    d.run();
  }
  void notifyAll(const std::string& title, const std::string& msg, const char* icon) {
    std::vector<int64_t> ids;
    {
      Stmt s(db_, "SELECT id FROM users");
      while (s.step()) ids.push_back(s.getInt(0));
    }
    for (int64_t id : ids) notifyUser(id, title, msg, icon);
  }
  void welcomeNotifications(int64_t uid) {
    // Inserted oldest-first so the first message ends up on top of the list.
    notifyUser(uid, "Profile available", "You can view and update your account details from the profile menu.",
               kIconPerson, true);
    notifyUser(uid, "Priority queue reminder", "Review high-priority waiting requests before dispatching an ambulance.",
               kIconBolt);
    notifyUser(uid, "Dispatch centre ready", "The operations dashboard is ready for reviewing emergency requests.",
               kIconCheck);
  }
  std::string notificationsJson(int64_t uid) {
    std::vector<std::string> items;
    int64_t unread = 0;
    Stmt s(db_,
           "SELECT id,title,message,icon,is_read,created_at FROM notifications WHERE user_id=? "
           "ORDER BY id DESC LIMIT 30");
    s.bindInt(1, uid);
    while (s.step()) {
      const bool read = s.getInt(4) != 0;
      if (!read) ++unread;
      items.push_back(jsonw::Obj()
                          .num("id", s.getInt(0)).str("title", s.getText(1)).str("message", s.getText(2))
                          .str("icon", s.getText(3)).boolean("read", read).str("time", s.getText(5)).done());
    }
    return jsonw::Obj().raw("notifications", jsonw::arr(items)).num("unread", unread).done();
  }

  // ---------------------------------------------------------------- emergencies / vehicles rows
  static EmRow readEm(Stmt& s) {
    EmRow e;
    e.id = s.getInt(0);
    e.type = s.getText(1);
    e.priority = s.getText(2);
    e.location = s.getText(3);
    e.patient = s.getText(4);
    e.details = s.getText(5);
    e.status = s.getText(6);
    e.createdAt = s.getText(7);
    if (!s.isNull(8)) e.nodeId = s.getInt(8);
    return e;
  }
  static std::string emJson(const EmRow& e) {
    return jsonw::Obj()
        .num("id", e.id).str("type", e.type).str("priority", e.priority).str("location", e.location)
        .str("patient", e.patient).str("details", e.details).str("status", e.status)
        .str("createdAt", e.createdAt).boolean("onRoadGraph", e.nodeId.has_value()).done();
  }
  bool loadEmergency(int64_t id, EmRow& out) {
    Stmt s(db_, std::string("SELECT ") + kEmCols + " FROM emergencies WHERE id=?");
    s.bindInt(1, id);
    if (!s.step()) return false;
    out = readEm(s);
    return true;
  }
  static VehRow readVeh(Stmt& s) {
    VehRow v;
    v.id = s.getText(0);
    v.type = s.getText(1);
    v.status = s.getText(2);
    v.location = s.getText(3);
    if (!s.isNull(4)) v.nodeId = s.getInt(4);
    return v;
  }
  static std::string vehJson(const VehRow& v) {
    return jsonw::Obj().str("id", v.id).str("type", v.type).str("status", v.status).str("location", v.location).done();
  }
  bool loadVehicle(const std::string& id, VehRow& out) {
    Stmt s(db_, std::string("SELECT ") + kVehCols + " FROM vehicles WHERE id=?");
    s.bindText(1, id);
    if (!s.step()) return false;
    out = readVeh(s);
    return true;
  }
  std::vector<VehRow> availableVehicles() {
    std::vector<VehRow> out;
    Stmt s(db_, std::string("SELECT ") + kVehCols + " FROM vehicles WHERE status='Available' ORDER BY length(id), id");
    while (s.step()) out.push_back(readVeh(s));
    return out;
  }
  int64_t queuePosition(int64_t id) const {
    int64_t pos = 1;
    for (const QItem& q : heap_.orderedSnapshot()) {
      if (q.id == id) return pos;
      ++pos;
    }
    return 0;
  }

  // ---------------------------------------------------------------- graph + queue
  void loadGraph() {
    graph_.clear();
    nodes_.clear();
    nodeNames_.clear();
    {
      Stmt s(db_, "SELECT id,name,aliases FROM nodes ORDER BY id");
      while (s.step()) {
        NodeInfo n;
        n.id = s.getInt(0);
        n.name = s.getText(1);
        n.keys.push_back(lower(n.name));
        const std::string aliases = s.getText(2);
        size_t start = 0;
        while (start <= aliases.size()) {
          size_t comma = aliases.find(',', start);
          if (comma == std::string::npos) comma = aliases.size();
          const std::string a = lower(trim(aliases.substr(start, comma - start)));
          if (!a.empty()) n.keys.push_back(a);
          start = comma + 1;
        }
        graph_.addNode(n.id);
        nodeNames_[n.id] = n.name;
        nodes_.push_back(n);
      }
    }
    Stmt s(db_, "SELECT a,b,km FROM edges");
    while (s.step()) graph_.addEdge(s.getInt(0), s.getInt(1), s.getReal(2));
  }
  void rebuildHeap() {
    heap_.clear();
    Stmt s(db_, "SELECT id,priority FROM emergencies WHERE status='Waiting'");
    while (s.step()) heap_.push({rankOf(s.getText(1)), s.getInt(0)});
  }
  // Map free text ("MG Road, Pune") to a graph node: the longest node name/alias found in the text wins.
  std::optional<int64_t> resolveNode(const std::string& text) const {
    const std::string t = lower(text);
    size_t best = 0;
    std::optional<int64_t> found;
    for (const NodeInfo& n : nodes_)
      for (const std::string& k : n.keys)
        if (k.size() > best && t.find(k) != std::string::npos) {
          best = k.size();
          found = n.id;
        }
    return found;
  }
  std::optional<int64_t> nodeByName(const std::string& lowerName) const {
    for (const NodeInfo& n : nodes_)
      if (lower(n.name) == lowerName) return n.id;
    return std::nullopt;
  }

  // ---------------------------------------------------------------- vehicle selection (Dijkstra)
  Selection selectVehicle(const EmRow& em, const std::optional<std::string>& ambulanceId) {
    Selection sel;
    std::optional<Graph::Result> sp;
    if (em.nodeId) sp = graph_.dijkstra(*em.nodeId);

    if (ambulanceId) {  // dispatcher picked a unit by hand
      if (!loadVehicle(upper(trim(*ambulanceId)), sel.veh) || sel.veh.status != "Available") {
        sel.error = "No available ambulance right now.";
        return sel;
      }
      sel.method = "manual";
      sel.suitable = suitableFor(em.type, sel.veh.type);
      if (!sel.suitable)
        sel.warning = "Unit " + sel.veh.id + " is " + sel.veh.type + " but " + em.type + " normally needs an ALS unit.";
      if (sp && sel.veh.nodeId) sel.dist = graph_.distanceTo(*sp, *sel.veh.nodeId);
    } else {  // automatic: nearest suitable available unit by road distance
      const std::vector<VehRow> avail = availableVehicles();
      if (avail.empty()) {
        sel.error = "No available ambulance right now.";
        return sel;
      }
      std::vector<const VehRow*> candidates;
      for (const VehRow& v : avail)
        if (suitableFor(em.type, v.type)) candidates.push_back(&v);
      if (candidates.empty()) {
        sel.error = "No suitable ambulance available: this emergency needs an ALS unit.";
        return sel;
      }
      const VehRow* best = nullptr;
      double bestDist = Graph::INF;
      if (sp) {
        for (const VehRow* v : candidates) {
          if (!v->nodeId) continue;
          const double d = graph_.distanceTo(*sp, *v->nodeId);
          if (d < bestDist) {  // strict '<' keeps the lowest id on ties
            bestDist = d;
            best = v;
          }
        }
      }
      if (best) {
        sel.veh = *best;
        sel.dist = bestDist;
        sel.method = "dijkstra";
      } else {  // location not on the road graph: no distance known, take the first suitable unit
        sel.veh = *candidates.front();
        sel.method = "fallback";
        sel.warning = "Location was not found on the road graph, so the first suitable unit was chosen.";
      }
    }
    if (sp && em.nodeId && sel.veh.nodeId && sel.dist != Graph::INF) {
      // Path is computed from the emergency; show it from the ambulance to the patient.
      std::vector<int64_t> path = graph_.pathTo(*sp, *sel.veh.nodeId);
      std::reverse(path.begin(), path.end());
      for (size_t i = 0; i < path.size(); ++i) {
        if (i) sel.route += " > ";
        sel.route += nodeNames_.count(path[i]) ? nodeNames_.at(path[i]) : std::to_string(path[i]);
      }
    }
    sel.ok = true;
    return sel;
  }
  static jsonw::Obj selectionJson(const Selection& sel) {
    jsonw::Obj o;
    o.raw("ambulance", vehJson(sel.veh));
    if (sel.dist != Graph::INF) o.real("distanceKm", sel.dist); else o.null("distanceKm");
    o.str("route", sel.route).str("method", sel.method).boolean("suitable", sel.suitable).str("warning", sel.warning);
    return o;
  }

  // ---------------------------------------------------------------- members
  Db& db_;
  std::mutex mu_;
  std::string demoSql_;
  Graph graph_;
  std::vector<NodeInfo> nodes_;
  std::unordered_map<int64_t, std::string> nodeNames_;
  EmergencyHeap heap_;
  std::map<std::string, Fail> fails_;
};
