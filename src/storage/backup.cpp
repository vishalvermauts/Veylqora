#include "veylqora/storage/backup.hpp"

#include "veylqora/core/error.hpp"

#include <sqlite3.h>
#include <string>
#include <system_error>

namespace veylqora {
namespace {

namespace fs = std::filesystem;

// Opens a raw SQLite handle; the caller closes it.
sqlite3* open_raw(const fs::path& file, int flags) {
  sqlite3* db = nullptr;
  if (sqlite3_open_v2(file.string().c_str(), &db, flags, nullptr) != SQLITE_OK) {
    const std::string msg = db ? sqlite3_errmsg(db) : "out of memory";
    sqlite3_close(db);
    throw Error("cannot open " + file.string() + ": " + msg);
  }
  return db;
}

// Runs SQLite's integrity check on a file and returns true only for "ok".
bool passes_integrity_check(const fs::path& file) {
  sqlite3* db = open_raw(file, SQLITE_OPEN_READONLY);
  sqlite3_stmt* stmt = nullptr;
  bool ok = false;
  if (sqlite3_prepare_v2(db, "PRAGMA integrity_check", -1, &stmt, nullptr) == SQLITE_OK &&
      sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = sqlite3_column_text(stmt, 0);
    ok = text != nullptr && std::string(reinterpret_cast<const char*>(text)) == "ok";
  }
  sqlite3_finalize(stmt);
  sqlite3_close(db);
  return ok;
}

// Copies every page of `from` into `to`, replacing what `to` holds.
void copy_pages(const fs::path& from, const fs::path& to, int to_flags) {
  sqlite3* src = open_raw(from, SQLITE_OPEN_READONLY);
  sqlite3* dst = open_raw(to, to_flags);
  sqlite3_backup* job = sqlite3_backup_init(dst, "main", src, "main");
  if (job == nullptr) {
    const std::string msg = sqlite3_errmsg(dst);
    sqlite3_close(dst);
    sqlite3_close(src);
    throw Error("backup failed: " + msg);
  }
  const int rc = sqlite3_backup_step(job, -1);
  sqlite3_backup_finish(job);
  const std::string msg = sqlite3_errmsg(dst);
  sqlite3_close(dst);
  sqlite3_close(src);
  if (rc != SQLITE_DONE) throw Error("backup failed: " + msg);
}

}  // namespace

void backup_database(Database& source, const fs::path& dest) {
  if (fs::exists(dest)) throw Error("backup target already exists: " + dest.string());

  sqlite3* dst = open_raw(dest, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
  sqlite3_backup* job = sqlite3_backup_init(dst, "main", source.handle(), "main");
  if (job == nullptr) {
    const std::string msg = sqlite3_errmsg(dst);
    sqlite3_close(dst);
    std::error_code ec;
    fs::remove(dest, ec);
    throw Error("backup failed: " + msg);
  }
  const int rc = sqlite3_backup_step(job, -1);
  sqlite3_backup_finish(job);
  const std::string msg = sqlite3_errmsg(dst);
  sqlite3_close(dst);
  if (rc != SQLITE_DONE) {
    std::error_code ec;
    fs::remove(dest, ec);
    throw Error("backup failed: " + msg);
  }
  if (!passes_integrity_check(dest)) {
    std::error_code ec;
    fs::remove(dest, ec);
    throw Error("backup failed its integrity check; it was discarded");
  }
}

void restore_database(const fs::path& backup, const fs::path& target) {
  if (!fs::exists(backup)) throw Error("backup not found: " + backup.string());
  if (!passes_integrity_check(backup)) {
    throw Error("backup failed its integrity check; the target was not changed");
  }
  // Open the target through the Database class so it is migrated like any other
  // database, then replace its pages with the backup's.
  { Database opened(target); }
  copy_pages(backup, target, SQLITE_OPEN_READWRITE);
  if (!passes_integrity_check(target)) {
    throw Error("restored database failed its integrity check");
  }
  // A backup from an older release is migrated to this build's schema here.
  { Database migrated(target); }
}

}  // namespace veylqora
