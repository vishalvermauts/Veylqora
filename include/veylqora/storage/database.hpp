#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

struct sqlite3;
struct sqlite3_stmt;

namespace veylqora {

/// A SQLite database in WAL mode with foreign keys on, migrated to the latest
/// schema on open. Single writer per file: concurrent writers are serialised by
/// SQLite's lock, and the daemon that owns writes arrives in M1.5 (ADR-0004).
class Database {
 public:
  explicit Database(const std::filesystem::path& file);
  ~Database();
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;

  void exec(std::string_view sql);
  sqlite3* handle() const noexcept { return db_; }

 private:
  void migrate();
  sqlite3* db_ = nullptr;
};

/// Groups writes so that they commit or roll back together.
class Transaction {
 public:
  explicit Transaction(Database& db);
  ~Transaction();
  Transaction(const Transaction&) = delete;
  Transaction& operator=(const Transaction&) = delete;

  void commit();

 private:
  Database& db_;
  bool open_ = true;
};

/// A prepared statement. Values are bound, never spliced into SQL text.
class Statement {
 public:
  Statement(Database& db, std::string_view sql);
  ~Statement();
  Statement(const Statement&) = delete;
  Statement& operator=(const Statement&) = delete;

  void bind(int index, std::string_view value);
  void bind(int index, std::int64_t value);
  void bind_null(int index);

  /// Rewinds for another execution with fresh bindings.
  void reset();

  /// Advances to the next row. Returns false when no rows remain.
  bool step();

  std::string text(int column) const;
  std::int64_t integer(int column) const;

 private:
  sqlite3* db_;
  sqlite3_stmt* stmt_ = nullptr;
};

}  // namespace veylqora
