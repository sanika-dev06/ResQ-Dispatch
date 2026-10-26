// db.hpp - Small RAII wrapper over the SQLite C API (prepared statements,
// transactions). Every statement uses bound parameters, never string
// concatenation of user input, so SQL injection is not possible through it.
#pragma once
#include <sqlite3.h>

#include <cstdint>
#include <stdexcept>
#include <string>

class DbError : public std::runtime_error {
 public:
  DbError(const std::string& msg, int code) : std::runtime_error(msg), code_(code) {}
  int code() const { return code_; }
  bool isConstraint() const { return code_ == SQLITE_CONSTRAINT; }

 private:
  int code_;
};

class Db {
 public:
  explicit Db(const std::string& path) {
    int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
      std::string msg = db_ ? sqlite3_errmsg(db_) : "cannot open database";
      if (db_) sqlite3_close(db_);
      db_ = nullptr;
      throw DbError(msg, rc);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA foreign_keys=ON;");
    exec("PRAGMA journal_mode=WAL;");
  }
  ~Db() {
    if (db_) sqlite3_close(db_);
  }
  Db(const Db&) = delete;
  Db& operator=(const Db&) = delete;

  // Runs one or more SQL statements (used for schema / seed scripts).
  void exec(const std::string& sql) {
    int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
    if (rc != SQLITE_OK) throw DbError(sqlite3_errmsg(db_), rc);
  }

  sqlite3* raw() { return db_; }
  int64_t lastInsertId() { return sqlite3_last_insert_rowid(db_); }
  int changes() { return sqlite3_changes(db_); }

 private:
  sqlite3* db_ = nullptr;
};

class Stmt {
 public:
  Stmt(Db& db, const std::string& sql) : db_(db) {
    int rc = sqlite3_prepare_v2(db.raw(), sql.c_str(), -1, &st_, nullptr);
    if (rc != SQLITE_OK) throw DbError(std::string(sqlite3_errmsg(db.raw())) + " [" + sql + "]", rc);
  }
  ~Stmt() {
    if (st_) sqlite3_finalize(st_);
  }
  Stmt(const Stmt&) = delete;
  Stmt& operator=(const Stmt&) = delete;

  Stmt& bindText(int i, const std::string& v) {
    check(sqlite3_bind_text(st_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT));
    return *this;
  }
  Stmt& bindInt(int i, int64_t v) {
    check(sqlite3_bind_int64(st_, i, v));
    return *this;
  }
  Stmt& bindReal(int i, double v) {
    check(sqlite3_bind_double(st_, i, v));
    return *this;
  }
  Stmt& bindNull(int i) {
    check(sqlite3_bind_null(st_, i));
    return *this;
  }

  // true = a row is available, false = finished.
  bool step() {
    int rc = sqlite3_step(st_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw DbError(sqlite3_errmsg(db_.raw()), rc);
  }
  // Run to completion, ignoring any rows.
  void run() {
    while (step()) {
    }
  }

  std::string getText(int c) {
    const unsigned char* p = sqlite3_column_text(st_, c);
    return p ? std::string(reinterpret_cast<const char*>(p)) : std::string();
  }
  int64_t getInt(int c) { return sqlite3_column_int64(st_, c); }
  double getReal(int c) { return sqlite3_column_double(st_, c); }
  bool isNull(int c) { return sqlite3_column_type(st_, c) == SQLITE_NULL; }

 private:
  void check(int rc) {
    if (rc != SQLITE_OK) throw DbError(sqlite3_errmsg(db_.raw()), rc);
  }
  Db& db_;
  sqlite3_stmt* st_ = nullptr;
};

// BEGIN IMMEDIATE ... COMMIT. Rolls back automatically if commit() was not reached.
class Tx {
 public:
  explicit Tx(Db& db) : db_(db) { db_.exec("BEGIN IMMEDIATE;"); }
  void commit() {
    db_.exec("COMMIT;");
    done_ = true;
  }
  ~Tx() {
    if (!done_) {
      try {
        db_.exec("ROLLBACK;");
      } catch (...) {
      }
    }
  }
  Tx(const Tx&) = delete;
  Tx& operator=(const Tx&) = delete;

 private:
  Db& db_;
  bool done_ = false;
};
