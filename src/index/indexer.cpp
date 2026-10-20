#include "veylqora/index/indexer.hpp"

#include "veylqora/core/error.hpp"
#include "veylqora/core/identity.hpp"
#include "veylqora/core/log.hpp"
#include "veylqora/core/project.hpp"
#include "veylqora/graph/resolver.hpp"
#include "veylqora/index/python_parser.hpp"
#include "veylqora/ingestion/source.hpp"
#include "veylqora/retrieval/text.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <unordered_set>

namespace veylqora {
namespace fs = std::filesystem;
namespace {

// Only Python is indexed in v0.1. Extension-based detection, as ADR-0009 records.
bool is_python(const std::string& rel_path) {
  return rel_path.size() > 3 && rel_path.ends_with(".py");
}

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

// The fingerprint of a definition without its own name. A rename or a move with
// an unchanged body then keeps its fingerprint, so its memories follow it
// (ADR-0014). The parameters are still part of the text, so a changed signature
// still counts.
std::string definition_fingerprint(const PySymbol& s) {
  std::string text = s.body;
  std::size_t at = 0;
  if (text.starts_with("async ")) at = 6;
  if (text.compare(at, 4, "def ") == 0) {
    at += 4;
  } else if (text.compare(at, 6, "class ") == 0) {
    at += 6;
  } else {
    return body_fingerprint(text);
  }
  if (text.compare(at, s.name.size(), s.name) == 0) text.erase(at, s.name.size());
  return body_fingerprint(text);
}

// "auth/tokens.py" -> "auth.tokens"; a package's __init__.py names the package,
// so "auth/__init__.py" -> "auth".
std::string module_name(std::string rel_path) {
  constexpr std::string_view kPackageInit = "/__init__.py";
  if (rel_path.ends_with(kPackageInit)) {
    rel_path.resize(rel_path.size() - kPackageInit.size());
  } else {
    rel_path.resize(rel_path.size() - 3);  // strip ".py"
  }
  for (char& c : rel_path) {
    if (c == '/') c = '.';
  }
  return rel_path;
}

void drop_file(Database& db, const std::string& pid, const std::string& rel) {
  Statement del(db, "DELETE FROM file WHERE project_id = ? AND rel_path = ?");
  del.bind(1, pid);
  del.bind(2, rel);
  del.step();  // ON DELETE CASCADE removes its symbols
}

// Lexical tokens and the hashed vector of one symbol: its name, qualified name
// and body (ADR-0015). Tokens are stored per project so lookups stay in-project.
void store_retrieval_rows(Database& db, const std::string& pid, const std::string& sid,
                          const PySymbol& s) {
  const auto name_tokens = tokenize(s.name);
  const auto fqn_tokens = tokenize(s.fqn);
  const auto body_tokens = tokenize(s.body);

  std::unordered_set<std::string> all(name_tokens.begin(), name_tokens.end());
  all.insert(fqn_tokens.begin(), fqn_tokens.end());
  all.insert(body_tokens.begin(), body_tokens.end());
  for (const std::string& token : all) {
    Statement ins(
        db,
        "INSERT OR IGNORE INTO token (project_id, token, symbol_id) VALUES (?, ?, ?)");
    ins.bind(1, pid);
    ins.bind(2, token);
    ins.bind(3, sid);
    ins.step();
  }

  std::array<float, kVectorDims> v = embed(name_tokens, 3.0F);
  accumulate(v, embed(fqn_tokens, 2.0F));
  accumulate(v, embed(body_tokens, 1.0F));
  normalise(v);
  std::string blob(sizeof(v), '\0');
  std::memcpy(blob.data(), v.data(), sizeof(v));
  Statement vec(db,
                "INSERT INTO symbol_vec (symbol_id, project_id, vec) VALUES (?, ?, ?)");
  vec.bind(1, sid);
  vec.bind(2, pid);
  vec.bind_blob(3, blob);
  vec.step();
}

// Writes or refreshes the rows for one file. Caller holds the transaction.
void index_one(Database& db, const std::string& pid, const std::string& rel,
               const std::string& contents, const std::string& sha, IndexStats& stats) {
  const std::string fid = content_fingerprint(pid + '\x1f' + rel);
  Statement upsert(
      db,
      "INSERT INTO file (id, project_id, rel_path, language, content_sha256, size_bytes) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT (project_id, rel_path) DO UPDATE SET "
      "content_sha256 = excluded.content_sha256, size_bytes = excluded.size_bytes");
  upsert.bind(1, fid);
  upsert.bind(2, pid);
  upsert.bind(3, rel);
  upsert.bind(4, std::string_view{"python"});
  upsert.bind(5, sha);
  upsert.bind(6, static_cast<std::int64_t>(contents.size()));
  upsert.step();

  // Deleting a file's symbols cascades to its facts and edges; bindings are
  // keyed by file, so they are cleared explicitly.
  Statement clear(db, "DELETE FROM symbol WHERE file_id = ?");
  clear.bind(1, fid);
  clear.step();
  Statement clear_bindings(db, "DELETE FROM binding WHERE file_id = ?");
  clear_bindings.bind(1, fid);
  clear_bindings.step();

  const bool is_package = rel.ends_with("/__init__.py");
  const PyFile parsed = parse_python(module_name(rel), contents, is_package);
  if (parsed.has_syntax_errors) {
    log::warn(rel + ": syntax errors; indexing the parts that parsed");
    ++stats.files_with_syntax_errors;
  }

  std::vector<std::string> sids;
  sids.reserve(parsed.symbols.size());
  for (const PySymbol& s : parsed.symbols) {
    const std::string sid =
        symbol_id(SymbolKey{pid, "python", rel, s.fqn, s.kind, s.overload});
    Statement ins(
        db,
        "INSERT INTO symbol (id, project_id, file_id, kind, name, fqn, overload, "
        "body_fingerprint, start_line, end_line) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, sid);
    ins.bind(2, pid);
    ins.bind(3, fid);
    ins.bind(4, to_string(s.kind));
    ins.bind(5, s.name);
    ins.bind(6, s.fqn);
    ins.bind(7, static_cast<std::int64_t>(s.overload));
    ins.bind(8, definition_fingerprint(s));
    ins.bind(9, static_cast<std::int64_t>(s.start_line));
    ins.bind(10, static_cast<std::int64_t>(s.end_line));
    ins.step();
    store_retrieval_rows(db, pid, sid, s);
    sids.push_back(sid);
    ++stats.symbols_written;
  }

  for (const PyFact& f : parsed.facts) {
    std::string_view kind = "import";
    if (f.kind == FactKind::call) kind = "call";
    if (f.kind == FactKind::inherits) kind = "inherits";
    Statement ins(
        db,
        "INSERT INTO fact (file_id, src_symbol_id, kind, name, qualifier, scope, line) "
        "VALUES (?, ?, ?, ?, ?, ?, ?)");
    ins.bind(1, fid);
    ins.bind(2, sids[f.src]);
    ins.bind(3, kind);
    ins.bind(4, f.name);
    ins.bind(5, f.qualifier);
    ins.bind(6, f.scope);
    ins.bind(7, static_cast<std::int64_t>(f.line));
    ins.step();
  }

  for (const PyBinding& b : parsed.bindings) {
    Statement ins(db,
                  "INSERT OR REPLACE INTO binding (file_id, local_name, target_fqn) "
                  "VALUES (?, ?, ?)");
    ins.bind(1, fid);
    ins.bind(2, b.local);
    ins.bind(3, b.target);
    ins.step();
  }
  ++stats.files_indexed;
}

// Decides what one candidate becomes in the database. Caller holds the transaction.
void process_candidate(Database& db, const std::string& pid, const fs::path& base, bool force,
                       const std::string& rel, IndexStats& stats) {
  if (!is_python(rel)) return;

  const Source src = read_source(base / rel);
  switch (src.skip) {
    case Skip::none:
      break;
    case Skip::unreadable:
      // Transient failure: keep whatever was stored before rather than deleting it.
      log::warn("cannot read " + rel + "; keeping its previous index entries");
      ++stats.files_unreadable;
      return;
    case Skip::missing:
    case Skip::symlink:
    case Skip::not_regular:
      drop_file(db, pid, rel);
      return;
    case Skip::too_large:
      log::warn("skipping " + rel + ": larger than 1 MiB");
      drop_file(db, pid, rel);
      ++stats.files_skipped;
      return;
    case Skip::binary:
      log::warn("skipping " + rel + ": binary content");
      drop_file(db, pid, rel);
      ++stats.files_skipped;
      return;
    case Skip::invalid_utf8:
      log::warn("skipping " + rel + ": not valid UTF-8");
      drop_file(db, pid, rel);
      ++stats.files_skipped;
      return;
  }

  const std::string sha = content_fingerprint(src.contents);
  Statement existing(
      db, "SELECT content_sha256 FROM file WHERE project_id = ? AND rel_path = ?");
  existing.bind(1, pid);
  existing.bind(2, rel);
  if (!force && existing.step() && existing.text(0) == sha) {
    ++stats.files_unchanged;
    return;
  }
  index_one(db, pid, rel, src.contents, sha, stats);
}

}  // namespace

IndexStats index_project(Database& db, const Clock& clock, const fs::path& root,
                         const IndexOptions& options) {
  const fs::path base = canonical_root(root);
  const std::string pid = project_id(base);
  IndexStats stats;

  {
    Transaction tx(db);
    Statement project(
        db,
        "INSERT INTO project (id, name, root_path, created_at) VALUES (?, ?, ?, ?) "
        "ON CONFLICT (id) DO NOTHING");
    project.bind(1, pid);
    project.bind(2, base.filename().string());
    project.bind(3, base.generic_string());
    project.bind(4, to_iso8601(clock.now()));
    project.step();
    tx.commit();
  }

  // Only supported file types count toward progress and the removal pass.
  std::vector<std::string> candidates = list_candidates(base);
  std::erase_if(candidates, [](const std::string& rel) { return !is_python(rel); });
  const std::size_t total = candidates.size();
  const std::size_t batch = std::max<std::size_t>(1, options.batch_size);
  const auto cancelled = [&] {
    return options.cancel != nullptr && options.cancel->load();
  };

  std::size_t done = 0;
  while (done < total && !cancelled()) {
    Transaction tx(db);
    for (std::size_t n = 0; n < batch && done < total && !cancelled(); ++n, ++done) {
      process_candidate(db, pid, base, options.force, candidates[done], stats);
    }
    tx.commit();
    if (options.on_progress) options.on_progress(done, total);
  }
  if (done < total) {
    stats.canceled = true;
    return stats;  // no removal pass: the listing was not fully applied
  }

  // Removal pass: rows for files that are no longer candidates.
  Transaction tx(db);
  const std::unordered_set<std::string> present(candidates.begin(), candidates.end());
  std::vector<std::string> stored;
  {
    Statement all(db, "SELECT rel_path FROM file WHERE project_id = ?");
    all.bind(1, pid);
    while (all.step()) stored.push_back(all.text(0));
  }
  for (const std::string& rel : stored) {
    if (present.count(rel) != 0) continue;
    drop_file(db, pid, rel);
    ++stats.files_removed;
  }

  // Edges depend on every file, so they are rebuilt only when something changed.
  if (stats.files_indexed + stats.files_removed + stats.files_skipped > 0) {
    rebuild_graph(db, pid);
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
              "WHERE s.project_id = ? AND s.name = ? AND s.kind != 'module' ORDER BY "
              "f.rel_path, s.start_line");
  q.bind(1, pid);
  q.bind(2, name);
  std::vector<SymbolHit> hits;
  while (q.step()) {
    hits.push_back(SymbolHit{q.text(0), q.text(1), q.text(2), q.integer(3)});
  }
  return hits;
}

Consistency check_consistency(Database& db, const fs::path& root) {
  const std::string pid = project_id_for(root);
  Consistency out;
  const auto count = [&](const char* sql) {
    Statement q(db, sql);
    q.bind(1, pid);
    q.step();
    return static_cast<std::size_t>(q.integer(0));
  };
  out.symbols = count("SELECT COUNT(*) FROM symbol WHERE project_id = ? AND kind != 'module'");
  out.symbols_without_tokens = count(
      "SELECT COUNT(*) FROM symbol s WHERE s.project_id = ? AND s.kind != 'module' AND NOT EXISTS "
      "(SELECT 1 FROM token t WHERE t.symbol_id = s.id)");
  out.symbols_without_vectors = count(
      "SELECT COUNT(*) FROM symbol s WHERE s.project_id = ? AND s.kind != 'module' AND NOT EXISTS "
      "(SELECT 1 FROM symbol_vec v WHERE v.symbol_id = s.id)");
  out.orphan_vectors = count(
      "SELECT COUNT(*) FROM symbol_vec v WHERE v.project_id = ? AND NOT EXISTS "
      "(SELECT 1 FROM symbol s WHERE s.id = v.symbol_id)");
  out.orphan_tokens = count(
      "SELECT COUNT(*) FROM token t WHERE t.project_id = ? AND NOT EXISTS "
      "(SELECT 1 FROM symbol s WHERE s.id = t.symbol_id)");
  return out;
}

}  // namespace veylqora
