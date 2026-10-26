// ResQ Dispatch backend - C++17, Crow (HTTP) + SQLite.
//
// This file is only the HTTP layer: it parses requests, checks the bearer token,
// calls Service (src/service.hpp) and writes the JSON reply. All business logic
// is in service.hpp.
//
// Needs Crow >= 1.0 (HTTPMethod::Post / Put / Delete spelling) and SQLite 3.
#include "crow.h"

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

#include "paths.hpp"
#include "service.hpp"

namespace {

std::string envOr(const char* name, const std::string& fallback) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : fallback;
}

std::string readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw std::runtime_error("Cannot read '" + path + "'. The db/ and public/ folders must be in the project folder (or set RESQ_ROOT to it).");
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

crow::response reply(const Resp& r) {
  crow::response res(r.code, r.body);
  res.set_header("Content-Type", "application/json; charset=utf-8");
  res.set_header("Cache-Control", "no-store");
  res.set_header("X-Content-Type-Options", "nosniff");
  return res;
}

crow::response fail(int code, const std::string& message) {
  return reply(Resp{code, jsonw::Obj().str("error", message).done()});
}
crow::response unauthorized() { return fail(401, "Authentication required. Please sign in again."); }
crow::response badJson() { return fail(400, "Request body must be a JSON object."); }

crow::response serveFile(const std::string& path, const char* contentType) {
  try {
    crow::response res(200, readFile(path));
    res.set_header("Content-Type", contentType);
    res.set_header("Cache-Control", "no-cache");
    res.set_header("X-Content-Type-Options", "nosniff");
    res.set_header("X-Frame-Options", "DENY");
    return res;
  } catch (const std::exception& e) {
    std::cerr << "[static] " << e.what() << "\n";
    return fail(404, "Page not found.");
  }
}

std::string bearer(const crow::request& req) {
  const std::string h = req.get_header_value("Authorization");
  const std::string prefix = "Bearer ";
  if (h.size() > prefix.size() && h.compare(0, prefix.size(), prefix) == 0) return h.substr(prefix.size());
  return "";
}

// Reads a JSON string field; false if absent or not a string.
bool getString(const crow::json::rvalue& j, const char* key, std::string& out) {
  if (!j.has(key)) return false;
  if (j[key].t() != crow::json::type::String) return false;
  out = std::string(j[key].s());
  return true;
}
std::string strOr(const crow::json::rvalue& j, const char* key) {
  std::string s;
  getString(j, key, s);
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  // Works from any start folder (terminal, VS Code debugger, double-click): see paths.hpp.
  const std::string root = paths::findProjectRoot(argc > 0 ? argv[0] : nullptr, envOr("RESQ_ROOT", ""));
  const std::string dbPath = envOr("RESQ_DB", root + "/resq.db");
  const std::string publicDir = root + "/public";
  const std::string adminPassword = envOr("RESQ_ADMIN_PASSWORD", "ResQ@123");
  const int port = std::atoi(envOr("RESQ_PORT", "8080").c_str());
  const std::string bindAddr = envOr("RESQ_BIND", "127.0.0.1");

  try {
    Db db(dbPath);
    Service svc(db);
    svc.init(readFile(root + "/db/schema.sql"), readFile(root + "/db/seed.sql"), readFile(root + "/db/demo_data.sql"),
             adminPassword);

    crow::SimpleApp app;
    app.loglevel(crow::LogLevel::Warning);

    auto auth = [&](const crow::request& req) { return svc.authenticate(bearer(req)); };

    // ------------------------------------------------------------ pages
    CROW_ROUTE(app, "/")
    ([&]() { return serveFile(publicDir + "/index.html", "text/html; charset=utf-8"); });
    CROW_ROUTE(app, "/app.js")
    ([&]() { return serveFile(publicDir + "/app.js", "application/javascript; charset=utf-8"); });
    CROW_ROUTE(app, "/api/health")
    ([&]() { return reply(Resp{200, jsonw::Obj().boolean("ok", true).done()}); });

    // ------------------------------------------------------------ auth
    CROW_ROUTE(app, "/api/auth/login").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
      auto j = crow::json::load(req.body);
      if (!j || j.t() != crow::json::type::Object) return badJson();
      return reply(svc.login(strOr(j, "username"), strOr(j, "password")));
    });

    CROW_ROUTE(app, "/api/auth/signup").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
      auto j = crow::json::load(req.body);
      if (!j || j.t() != crow::json::type::Object) return badJson();
      return reply(svc.signup(strOr(j, "name"), strOr(j, "username"), strOr(j, "email"), strOr(j, "password")));
    });

    CROW_ROUTE(app, "/api/auth/logout").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
      return reply(svc.logout(bearer(req)));
    });

    CROW_ROUTE(app, "/api/auth/me")
    ([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      return reply(svc.me(*u));
    });

    // ------------------------------------------------------------ profile
    CROW_ROUTE(app, "/api/profile").methods(crow::HTTPMethod::Put)([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      auto j = crow::json::load(req.body);
      if (!j || j.t() != crow::json::type::Object) return badJson();
      return reply(svc.updateProfile(*u, strOr(j, "name"), strOr(j, "email")));
    });

    CROW_ROUTE(app, "/api/profile/password").methods(crow::HTTPMethod::Put)([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      auto j = crow::json::load(req.body);
      if (!j || j.t() != crow::json::type::Object) return badJson();
      return reply(svc.changePassword(*u, strOr(j, "currentPassword"), strOr(j, "newPassword")));
    });

    // ------------------------------------------------------------ notifications
    CROW_ROUTE(app, "/api/notifications")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Delete)([&](const crow::request& req) {
          auto u = auth(req);
          if (!u) return unauthorized();
          if (req.method == crow::HTTPMethod::Delete) return reply(svc.clearAll(*u));  // "Clear all"
          return reply(svc.listNotifications(*u));
        });

    CROW_ROUTE(app, "/api/notifications/read-all").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      return reply(svc.markAllRead(*u));  // "Mark all as read"
    });

    // ------------------------------------------------------------ dashboard / queue
    CROW_ROUTE(app, "/api/state")
    ([&](const crow::request& req) {
      if (!auth(req)) return unauthorized();
      return reply(svc.state());
    });

    CROW_ROUTE(app, "/api/emergencies").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      auto j = crow::json::load(req.body);
      if (!j || j.t() != crow::json::type::Object) return badJson();
      return reply(svc.createEmergency(*u, strOr(j, "type"), strOr(j, "priority"), strOr(j, "location"),
                                       strOr(j, "patient"), strOr(j, "details")));
    });

    // Preview which vehicle Dijkstra would pick (does not dispatch anything).
    CROW_ROUTE(app, "/api/emergencies/<int>/nearest")
    ([&](const crow::request& req, int id) {
      if (!auth(req)) return unauthorized();
      return reply(svc.nearestPreview(id));
    });

    // ------------------------------------------------------------ dispatching
    CROW_ROUTE(app, "/api/dispatch").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      std::optional<int64_t> requestId;
      std::optional<std::string> ambulanceId;
      if (!req.body.empty()) {
        auto j = crow::json::load(req.body);
        if (!j || j.t() != crow::json::type::Object) return badJson();
        if (j.has("requestId") && j["requestId"].t() == crow::json::type::Number) requestId = j["requestId"].i();
        std::string amb;
        if (getString(j, "ambulanceId", amb) && !amb.empty()) ambulanceId = amb;
      }
      return reply(svc.dispatch(*u, requestId, ambulanceId));
    });

    CROW_ROUTE(app, "/api/dispatches/<int>/complete").methods(crow::HTTPMethod::Post)([&](const crow::request& req, int id) {
      auto u = auth(req);
      if (!u) return unauthorized();
      return reply(svc.completeDispatch(*u, id));
    });

    // ------------------------------------------------------------ vehicles
    CROW_ROUTE(app, "/api/vehicles")
        .methods(crow::HTTPMethod::Get, crow::HTTPMethod::Post)([&](const crow::request& req) {
          auto u = auth(req);
          if (!u) return unauthorized();
          if (req.method == crow::HTTPMethod::Post) {
            auto j = crow::json::load(req.body);
            if (!j || j.t() != crow::json::type::Object) return badJson();
            return reply(svc.createVehicle(*u, strOr(j, "id"), strOr(j, "type"), strOr(j, "location")));
          }
          return reply(svc.listVehicles());
        });

    CROW_ROUTE(app, "/api/vehicles/<string>")
        .methods(crow::HTTPMethod::Put, crow::HTTPMethod::Delete)([&](const crow::request& req, std::string id) {
          auto u = auth(req);
          if (!u) return unauthorized();
          if (req.method == crow::HTTPMethod::Delete) return reply(svc.deleteVehicle(*u, id));
          auto j = crow::json::load(req.body);
          if (!j || j.t() != crow::json::type::Object) return badJson();
          std::optional<std::string> type, location, status;
          std::string tmp;
          if (getString(j, "type", tmp)) type = tmp;
          if (getString(j, "location", tmp)) location = tmp;
          if (getString(j, "status", tmp)) status = tmp;
          return reply(svc.updateVehicle(*u, id, type, location, status));
        });

    // ------------------------------------------------------------ admin
    CROW_ROUTE(app, "/api/admin/reset").methods(crow::HTTPMethod::Post)([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      return reply(svc.resetDemo(*u));
    });

    CROW_ROUTE(app, "/api/users")
    ([&](const crow::request& req) {
      auto u = auth(req);
      if (!u) return unauthorized();
      return reply(svc.listUsers(*u));
    });

    std::cout << "ResQ Dispatch backend\n"
              << "  project  : " << root << "\n"
              << "  database : " << dbPath << "\n"
              << "  open     : http://" << (bindAddr == "0.0.0.0" ? "localhost" : bindAddr) << ":" << port << "/\n"
              << "  admin    : username 'admin' (password from RESQ_ADMIN_PASSWORD, default is ResQ@123 - change it)\n"
              << "Press Ctrl+C to stop.\n";

    app.bindaddr(bindAddr).port(static_cast<uint16_t>(port)).multithreaded().run();
  } catch (const std::exception& e) {
    std::cerr << "Fatal: " << e.what() << "\n"
              << "Hint: if the port is already in use, start with a different one, e.g.  set RESQ_PORT=8081\n";
    return 1;
  }
  return 0;
}
