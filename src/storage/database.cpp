#include "veylqora/storage/database.hpp"

#include "veylqora/core/error.hpp"

#include <array>
#include <sqlite3.h>

namespace veylqora {
namespace {

// Forward-only migrations. Entry N takes the schema from version N to N+1. An
// entry is never edited once a release has shipped (ADR-0008).
constexpr std::array kMigrations = {std::string_view{R"sql(
CREATE TABLE project (
  id TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  root_path TEXT NOT NULL UNIQUE,
  created_at TEXT NOT NULL
);

CREATE TABLE file (
  id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL REFERENCES project(id) ON DELETE CASCADE,
  rel_path TEXT NOT NULL,
  language TEXT NOT NULL,
  content_sha256 TEXT NOT NULL,
  size_bytes INTEGER NOT NULL,
  UNIQUE (project_id, rel_path)
);

CREATE TABLE symbol (
  id TEXT PRIMARY KEY,
  project_id TEXT NOT NULL REFERENCES project(id) ON DELETE CASCADE,
  file_id TEXT NOT NULL REFERENCES file(id) ON DELETE CASCADE,
  kind TEXT NOT NULL,
  name TEXT NOT NULL,
  fqn TEXT NOT NULL,
  overload INTEGER NOT NULL,
  body_fingerprint TEXT NOT NULL,
  start_line INTEGER NOT NULL,
  end_line INTEGER NOT NULL
);

CREATE INDEX symbol_by_name ON symbol (project_id, name);
)sql"},
    // v2: the code graph (ADR-0010). Facts are what the parser saw; edges are
    // derived from facts by the resolver and rebuilt on every completed run.
    std::string_view{R"sql(
CREATE TABLE fact (
  file_id TEXT NOT NULL REFERENCES file(id) ON DELETE CASCADE,
  src_symbol_id TEXT NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
  kind TEXT NOT NULL CHECK (kind IN ('call', 'inherits', 'import')),
  name TEXT NOT NULL,
  qualifier TEXT NOT NULL,
  scope TEXT NOT NULL,
  line INTEGER NOT NULL
);
CREATE INDEX fact_by_file ON fact (file_id);

CREATE TABLE binding (
  file_id TEXT NOT NULL REFERENCES file(id) ON DELETE CASCADE,
  local_name TEXT NOT NULL,
  target_fqn TEXT NOT NULL,
  PRIMARY KEY (file_id, local_name)
);

CREATE TABLE edge (
  project_id TEXT NOT NULL REFERENCES project(id) ON DELETE CASCADE,
  src_symbol_id TEXT NOT NULL REFERENCES symbol(id) ON DELETE CASCADE,
  dst_symbol_id TEXT REFERENCES symbol(id) ON DELETE CASCADE,
  dst_hint TEXT NOT NULL,
  kind TEXT NOT NULL CHECK (kind IN ('contains', 'imports', 'calls', 'inherits')),
  resolution TEXT NOT NULL CHECK (resolution IN ('resolved', 'heuristic', 'unresolved')),
  line INTEGER NOT NULL
);
CREATE INDEX edge_by_src ON edge (src_symbol_id, kind);
CREATE INDEX edge_by_dst ON edge (dst_symbol_id, kind);
)sql"},
};

}  // namespace

Database::Database(const std::filesystem::path& file) {
  const int rc = sqlite3_open_v2(file.string().c_str(), &db_,
                                 SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (rc != SQLITE_OK) {
    const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
    sqlite3_close(db_);
    db_ = nullptr;
    throw Error("cannot open " + file.string() + ": " + msg);
  }
  exec(
      "PRAGMA foreign_keys = ON; PRAGMA journal_mode = WAL; PRAGMA busy_timeout = 5000;");
  migrate();
}

Database::~Database() { sqlite3_close(db_); }

void Database::exec(std::string_view sql) {
  char* err = nullptr;
  const std::string text(sql);
  if (sqlite3_exec(db_, text.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
    const std::string msg = err ? err : "unknown error";
    sqlite3_free(err);
    throw Error("sql failed: " + msg);
  }
}

void Database::migrate() {
  exec("CREATE TABLE IF NOT EXISTS schema_version (version INTEGER NOT NULL);");

  std::int64_t current = 0;
  {
    Statement q(*this, "SELECT COALESCE(MAX(version), 0) FROM schema_version");
    q.step();
    current = q.integer(0);
  }
  if (static_cast<std::size_t>(current) > kMigrations.size()) {
    throw Error("database schema is newer than this build supports");
  }

  for (auto v = static_cast<std::size_t>(current); v < kMigrations.size(); ++v) {
    Transaction tx(*this);
    exec(std::string(kMigrations[v]));
    Statement record(*this, "INSERT INTO schema_version (version) VALUES (?)");
    record.bind(1, static_cast<std::int64_t>(v + 1));
    record.step();
    tx.commit();
  }
}

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }

Transaction::~Transaction() {
  if (open_) {
    try {
      db_.exec("ROLLBACK");
    } catch (const Error&) {
      // Nothing useful to do during unwinding; the original failure propagates.
    }
  }
}

void Transaction::commit() {
  db_.exec("COMMIT");
  open_ = false;
}

Statement::Statement(Database& db, std::string_view sql) : db_(db.handle()) {
  if (sqlite3_prepare_v2(db_, sql.data(), static_cast<int>(sql.size()), &stmt_,
                         nullptr) != SQLITE_OK) {
    throw Error(std::string("prepare failed: ") + sqlite3_errmsg(db_));
  }
}

Statement::~Statement() { sqlite3_finalize(stmt_); }

void Statement::bind(int index, std::string_view value) {
  if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                        SQLITE_TRANSIENT) != SQLITE_OK) {
    throw Error(std::string("bind failed: ") + sqlite3_errmsg(db_));
  }
}

void Statement::bind_null(int index) {
  if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
    throw Error(std::string("bind failed: ") + sqlite3_errmsg(db_));
  }
}

void Statement::reset() {
  sqlite3_reset(stmt_);
  sqlite3_clear_bindings(stmt_);
}

void Statement::bind(int index, std::int64_t value) {
  if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
    throw Error(std::string("bind failed: ") + sqlite3_errmsg(db_));
  }
}

bool Statement::step() {
  const int rc = sqlite3_step(stmt_);
  if (rc == SQLITE_ROW) return true;
  if (rc == SQLITE_DONE) return false;
  throw Error(std::string("step failed: ") + sqlite3_errmsg(db_));
}

std::string Statement::text(int column) const {
  const unsigned char* p = sqlite3_column_text(stmt_, column);
  if (p == nullptr) return {};
  const int n = sqlite3_column_bytes(stmt_, column);
  return std::string(reinterpret_cast<const char*>(p), static_cast<std::size_t>(n));
}

std::int64_t Statement::integer(int column) const {
  return sqlite3_column_int64(stmt_, column);
}

}  // namespace veylqora
