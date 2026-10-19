#include "veylqora/index/indexer.hpp"

#include "veylqora/core/error.hpp"
#include "veylqora/core/identity.hpp"
#include "veylqora/core/log.hpp"
#include "veylqora/index/python_symbols.hpp"

#include <fstream>
#include <sstream>
#include <unordered_set>

namespace veylqora {
namespace fs = std::filesystem;
namespace {

constexpr std::uintmax_t kMaxFileBytes = 1024 * 1024;
constexpr std::string_view kLanguage = "python";

fs::path canonical_root(const fs::path& root) {
  std::error_code ec;
  fs::path canonical = fs::canonical(root, ec);
  if (ec) throw Error("cannot open " + root.string() + ": " + ec.message());
  return canonical;
}

// A project is identified by its canonical path, so the same tree always maps
// to the same id regardless of how it was named on the command line.
std::string project_id(const fs::path& canonical) {
  return content_fingerprint(canonical.generic_string());
}

std::string read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw Error("cannot read " + path.string());
  std::ostringstream buf;
  buf << in.rdbuf();
  return buf.str();
}

// "auth/tokens.py" -> "auth.tokens"
std::string module_name(std::string rel_path) {
  rel_path.resize(rel_path.size() - 3);  // strip ".py"
  for (char& c : rel_path) {
    if (c == '/') c = '.';
  }
  return rel_path;
}

}  // namespace

IndexStats index_project(Database& db, const Clock& clock, const fs::path& root) {
  const fs::path base = canonical_root(root);
  const std::string pid = project_id(base);
  IndexStats stats;

  Transaction tx(db);
  {
    Statement project(
        db,
        "INSERT INTO project (id, name, root_path, created_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (id) DO NOTHING");
    project.bind(1, pid);
    project.bind(2, base.filename().string());
    project.bind(3, base.generic_string());
    project.bind(4, to_iso8601(clock.now()));
    project.step();
  }

  std::unordered_set<std::string> present;
  for (auto it = fs::recursive_directory_iterator(base);
       it != fs::recursive_directory_iterator(); ++it) {
    const fs::directory_entry& entry = *it;
    if (entry.is_directory() && entry.path().filename() == ".git") {
      it.disable_recursion_pending();
      continue;
    }
    // Symlinks are never followed: a link can point outside the project.
    if (entry.is_symlink() || !entry.is_regular_file() ||
        entry.path().extension() != ".py") {
      continue;
    }
    const std::string rel = fs::relative(entry.path(), base).generic_string();
    if (entry.file_size() > kMaxFileBytes) {
      log::warn("skipping " + rel + ": larger than 1 MiB");
      continue;
    }

    const std::string contents = read_file(entry.path());
    const std::string sha = content_fingerprint(contents);
    present.insert(rel);

    Statement existing(
        db, "SELECT content_sha256 FROM file WHERE project_id = ? AND rel_path = ?");
    existing.bind(1, pid);
    existing.bind(2, rel);
    if (existing.step() && existing.text(0) == sha) {
      ++stats.files_unchanged;
      continue;
    }

    const std::string fid = content_fingerprint(pid + '\x1f' + rel);
    Statement upsert(
        db,
        "INSERT INTO file (id, project_id, rel_path, language, content_sha256, "
        "size_bytes) "
        "VALUES (?, ?, ?, ?, ?, ?) "
        "ON CONFLICT (project_id, rel_path) DO UPDATE SET "
        "content_sha256 = excluded.content_sha256, size_bytes = excluded.size_bytes");
    upsert.bind(1, fid);
    upsert.bind(2, pid);
    upsert.bind(3, rel);
    upsert.bind(4, kLanguage);
    upsert.bind(5, sha);
    upsert.bind(6, static_cast<std::int64_t>(contents.size()));
    upsert.step();

    Statement clear(db, "DELETE FROM symbol WHERE file_id = ?");
    clear.bind(1, fid);
    clear.step();

    for (const PySymbol& s : extract_python_symbols(module_name(rel), contents)) {
      const std::string sid = symbol_id(
          SymbolKey{pid, std::string(kLanguage), rel, s.fqn, s.kind, s.overload});
      Statement ins(
          db,
          "INSERT INTO symbol (id, project_id, file_id, kind, name, fqn, overload, "
          "body_fingerprint, start_line, end_line) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, "
          "?)");
      ins.bind(1, sid);
      ins.bind(2, pid);
      ins.bind(3, fid);
      ins.bind(4, to_string(s.kind));
      ins.bind(5, s.name);
      ins.bind(6, s.fqn);
      ins.bind(7, static_cast<std::int64_t>(s.overload));
      ins.bind(8, body_fingerprint(s.body));
      ins.bind(9, static_cast<std::int64_t>(s.start_line));
      ins.bind(10, static_cast<std::int64_t>(s.end_line));
      ins.step();
      ++stats.symbols_written;
    }
    ++stats.files_indexed;
  }

  std::vector<std::string> stored;
  {
    Statement all(db, "SELECT rel_path FROM file WHERE project_id = ?");
    all.bind(1, pid);
    while (all.step()) stored.push_back(all.text(0));
  }
  for (const std::string& rel : stored) {
    if (present.count(rel) != 0) continue;
    Statement del(db, "DELETE FROM file WHERE project_id = ? AND rel_path = ?");
    del.bind(1, pid);
    del.bind(2, rel);
    del.step();  // ON DELETE CASCADE removes its symbols
    ++stats.files_removed;
  }

  tx.commit();
  return stats;
}

std::vector<SymbolHit> find_symbols(Database& db, const fs::path& root,
                                    std::string_view name) {
  const std::string pid = project_id(canonical_root(root));
  Statement q(db,
              "SELECT f.rel_path, s.fqn, s.kind, s.start_line FROM symbol s "
              "JOIN file f ON f.id = s.file_id "
              "WHERE s.project_id = ? AND s.name = ? ORDER BY f.rel_path, s.start_line");
  q.bind(1, pid);
  q.bind(2, name);
  std::vector<SymbolHit> hits;
  while (q.step()) {
    hits.push_back(SymbolHit{q.text(0), q.text(1), q.text(2), q.integer(3)});
  }
  return hits;
}

}  // namespace veylqora
