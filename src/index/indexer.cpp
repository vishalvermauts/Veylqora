#include "veylqora/index/indexer.hpp"

#include "veylqora/core/error.hpp"
#include "veylqora/core/identity.hpp"
#include "veylqora/core/log.hpp"
#include "veylqora/core/project.hpp"
#include "veylqora/graph/resolver.hpp"
#include "veylqora/index/javascript_parser.hpp"
#include "veylqora/index/python_parser.hpp"
#include "veylqora/ingestion/source.hpp"
#include "veylqora/retrieval/text.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace veylqora {
namespace fs = std::filesystem;
namespace {

// The language of a file, by extension (ADR-0009). An empty result means the
// file is not indexed. .tsx is not indexed yet: it needs its own grammar (ADR-0019).
std::string_view language_of(const std::string& rel_path) {
  const auto ends = [&](std::string_view ext) {
    return rel_path.size() > ext.size() && rel_path.ends_with(ext);
  };
  if (ends(".py")) return "python";
  if (ends(".js") || ends(".mjs") || ends(".cjs") || ends(".jsx")) return "javascript";
  if (ends(".ts") || ends(".mts") || ends(".cts")) return "typescript";
  return {};
}

fs::path canonical_root(const fs::path& root) {
  std::error_code ec;
  const fs::path c = fs::weakly_canonical(root, ec);
  if (ec) throw Error("cannot resolve root " + root.string() + ": " + ec.message());
  return c;
}

std::string project_id(const fs::path& canonical) {
  return content_fingerprint(canonical.generic_string());
}

// Fingerprints a definition with its own declared name masked out, so a pure
// rename leaves the fingerprint unchanged (ADR-0014), while any other edit
// inside the body ? including calling a differently-named function ? changes it.
// Only the declaration's name is removed; a recursive self-call inside the body
// still counts.
std::string definition_fingerprint(const SourceSymbol& s, std::string_view language) {
  std::string text = s.body;
  const auto is_word = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' || c == '$';
  };
  if (language == "python") {
    for (std::size_t kw = 0; kw < text.size(); ++kw) {
      const std::string_view rest(text.data() + kw, text.size() - kw);
      std::size_t after_kw = 0;
      if (rest.starts_with("def ") || rest.starts_with("def\t")) {
        after_kw = kw + 4;
      } else if (rest.starts_with("class ") || rest.starts_with("class\t")) {
        after_kw = kw + 6;
      }
      if (after_kw == 0 || (kw > 0 && is_word(text[kw - 1]))) continue;
      while (after_kw < text.size() &&
             (text[after_kw] == ' ' || text[after_kw] == '\t')) {
        ++after_kw;
      }
      if (text.compare(after_kw, s.name.size(), s.name) == 0 &&
          (after_kw + s.name.size() >= text.size() ||
           !is_word(text[after_kw + s.name.size()]))) {
        text.erase(after_kw, s.name.size());
        break;
      }
    }
  } else {
    // JavaScript and TypeScript: the first whole-word occurrence of the name is
    // the name in the declaration ("function f", "class A", "const f =", "m(").
    for (std::size_t at = text.find(s.name); at != std::string::npos;
         at = text.find(s.name, at + 1)) {
      const bool before = at == 0 || !is_word(text[at - 1]);
      const std::size_t end = at + s.name.size();
      const bool after = end >= text.size() || !is_word(text[end]);
      if (before && after) {
        text.erase(at, s.name.size());
        break;
      }
    }
  }
  return content_fingerprint(text);
}

// "pkg/sub/mod.py" -> "pkg.sub.mod"; "pkg/__init__.py" -> "pkg".
std::string module_name(std::string rel_path) {
  constexpr std::string_view kInit = "/__init__.py";
  if (rel_path == "__init__.py") return "root";
  if (rel_path.ends_with(kInit)) {
    rel_path.resize(rel_path.size() - kInit.size());
  } else {
    rel_path.resize(rel_path.size() - 3);  // strip ".py"
  }
  for (char& c : rel_path) {
    if (c == '/') c = '.';
  }
  return rel_path;
}

// "auth/tokens.ts" -> "auth.tokens"; an index file names its directory, so
// "auth/index.ts" -> "auth". This matches how import specifiers are resolved.
std::string js_module_name(std::string rel_path) {
  const std::size_t dot = rel_path.rfind('.');
  const std::size_t slash = rel_path.rfind('/');
  if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) {
    rel_path.resize(dot);
  }
  constexpr std::string_view kIndex = "/index";
  if (rel_path.ends_with(kIndex)) rel_path.resize(rel_path.size() - kIndex.size());
  for (char& c : rel_path) {
    if (c == '/') c = '.';
  }
  return rel_path;
}

// The directory of a project-relative path, '/'-separated, "" at the root.
std::string directory_of(const std::string& rel_path) {
  const std::size_t slash = rel_path.rfind('/');
  return slash == std::string::npos ? std::string{} : rel_path.substr(0, slash);
}

void drop_file(Database& db, const std::string& pid, const std::string& rel) {
  Statement del(db, "DELETE FROM file WHERE project_id = ? AND rel_path = ?");
  del.bind(1, pid);
  del.bind(2, rel);
  del.step();  // ON DELETE CASCADE removes its symbols
}

struct PreparedSymbol {
  std::string sid;
  std::string body_fp;
  std::vector<std::string> tokens;
  std::string vec_blob;
};

struct PreparedCandidate {
  std::string rel;
  std::string_view language;
  Skip skip = Skip::none;
  bool unchanged = false;
  std::string fid;
  std::string sha;
  std::int64_t size_bytes = 0;
  SourceFile parsed;
  std::vector<PreparedSymbol> symbols;
};

PreparedCandidate prepare_candidate(
    const std::string& pid, const fs::path& base, bool force, const std::string& rel,
    const std::unordered_map<std::string, std::string>& existing_shas) {
  PreparedCandidate out;
  out.rel = rel;
  out.language = language_of(rel);
  if (out.language.empty()) return out;

  const Source src = read_source(base / rel);
  out.skip = src.skip;
  if (src.skip != Skip::none) return out;

  out.sha = content_fingerprint(src.contents);
  if (!force) {
    const auto it = existing_shas.find(rel);
    if (it != existing_shas.end() && it->second == out.sha) {
      out.unchanged = true;
      return out;
    }
  }

  out.fid = content_fingerprint(pid + '\x1f' + rel);
  out.size_bytes = static_cast<std::int64_t>(src.contents.size());
  out.parsed =
      out.language == "python"
          ? parse_python(module_name(rel), src.contents, rel.ends_with("/__init__.py"))
          : parse_javascript(js_module_name(rel), directory_of(rel), src.contents,
                             out.language == "typescript" ? JsDialect::typescript
                                                          : JsDialect::javascript);

  out.symbols.reserve(out.parsed.symbols.size());
  const std::string lang_str(out.language);
  for (const SourceSymbol& s : out.parsed.symbols) {
    PreparedSymbol ps;
    ps.sid = symbol_id(SymbolKey{pid, lang_str, rel, s.fqn, s.kind, s.overload});
    ps.body_fp = definition_fingerprint(s, out.language);

    const auto name_tokens = tokenize(s.name);
    const auto fqn_tokens = tokenize(s.fqn);
    const auto body_tokens = tokenize(s.body);
    std::unordered_set<std::string> all(name_tokens.begin(), name_tokens.end());
    all.insert(fqn_tokens.begin(), fqn_tokens.end());
    all.insert(body_tokens.begin(), body_tokens.end());
    ps.tokens.assign(all.begin(), all.end());

    std::array<float, kVectorDims> v = embed(name_tokens, 3.0F);
    accumulate(v, embed(fqn_tokens, 2.0F));
    accumulate(v, embed(body_tokens, 1.0F));
    normalise(v);
    ps.vec_blob.resize(sizeof(v), '\0');
    std::memcpy(ps.vec_blob.data(), v.data(), sizeof(v));

    out.symbols.push_back(std::move(ps));
  }
  return out;
}

struct BatchStatements {
  Statement upsert_file;
  Statement clear_sym;
  Statement clear_bind;
  Statement ins_sym;
  Statement ins_token;
  Statement ins_vec;
  Statement ins_fact;
  Statement ins_bind;

  explicit BatchStatements(Database& db)
      : upsert_file(
            db,
            "INSERT INTO file (id, project_id, rel_path, language, content_sha256, "
            "size_bytes) VALUES (?, ?, ?, ?, ?, ?) "
            "ON CONFLICT (project_id, rel_path) DO UPDATE SET "
            "content_sha256 = excluded.content_sha256, size_bytes = excluded.size_bytes"),
        clear_sym(db, "DELETE FROM symbol WHERE file_id = ?"),
        clear_bind(db, "DELETE FROM binding WHERE file_id = ?"),
        ins_sym(db,
                "INSERT INTO symbol (id, project_id, file_id, kind, name, fqn, overload, "
                "body_fingerprint, start_line, end_line) VALUES (?, ?, ?, ?, ?, ?, ?, ?, "
                "?, ?)"),
        ins_token(db,
                  "INSERT OR IGNORE INTO token (project_id, token, symbol_id) VALUES (?, "
                  "?, ?)"),
        ins_vec(db,
                "INSERT INTO symbol_vec (symbol_id, project_id, vec) VALUES (?, ?, ?)"),
        ins_fact(db,
                 "INSERT INTO fact (file_id, src_symbol_id, kind, name, qualifier, "
                 "scope, line) "
                 "VALUES (?, ?, ?, ?, ?, ?, ?)"),
        ins_bind(db,
                 "INSERT OR REPLACE INTO binding (file_id, local_name, target_fqn) "
                 "VALUES (?, ?, ?)") {}
};

void apply_prepared(Database& db, const std::string& pid, const PreparedCandidate& c,
                    BatchStatements& stmts, IndexStats& stats) {
  if (c.language.empty()) return;
  switch (c.skip) {
    case Skip::none:
      break;
    case Skip::unreadable:
      log::warn("cannot read " + c.rel + "; keeping its previous index entries");
      ++stats.files_unreadable;
      return;
    case Skip::missing:
    case Skip::symlink:
    case Skip::not_regular:
      drop_file(db, pid, c.rel);
      return;
    case Skip::too_large:
      log::warn("skipping " + c.rel + ": larger than 1 MiB");
      drop_file(db, pid, c.rel);
      ++stats.files_skipped;
      return;
    case Skip::binary:
      log::warn("skipping " + c.rel + ": binary content");
      drop_file(db, pid, c.rel);
      ++stats.files_skipped;
      return;
    case Skip::invalid_utf8:
      log::warn("skipping " + c.rel + ": not valid UTF-8");
      drop_file(db, pid, c.rel);
      ++stats.files_skipped;
      return;
  }
  if (c.unchanged) {
    ++stats.files_unchanged;
    return;
  }

  stmts.upsert_file.reset();
  stmts.upsert_file.bind(1, c.fid);
  stmts.upsert_file.bind(2, pid);
  stmts.upsert_file.bind(3, c.rel);
  stmts.upsert_file.bind(4, c.language);
  stmts.upsert_file.bind(5, c.sha);
  stmts.upsert_file.bind(6, c.size_bytes);
  stmts.upsert_file.step();

  stmts.clear_sym.reset();
  stmts.clear_sym.bind(1, c.fid);
  stmts.clear_sym.step();

  stmts.clear_bind.reset();
  stmts.clear_bind.bind(1, c.fid);
  stmts.clear_bind.step();

  if (c.parsed.has_syntax_errors) {
    log::warn(c.rel + ": syntax errors; indexing the parts that parsed");
    ++stats.files_with_syntax_errors;
  }

  for (std::size_t i = 0; i < c.parsed.symbols.size(); ++i) {
    const SourceSymbol& s = c.parsed.symbols[i];
    const PreparedSymbol& ps = c.symbols[i];
    stmts.ins_sym.reset();
    stmts.ins_sym.bind(1, ps.sid);
    stmts.ins_sym.bind(2, pid);
    stmts.ins_sym.bind(3, c.fid);
    stmts.ins_sym.bind(4, to_string(s.kind));
    stmts.ins_sym.bind(5, s.name);
    stmts.ins_sym.bind(6, s.fqn);
    stmts.ins_sym.bind(7, static_cast<std::int64_t>(s.overload));
    stmts.ins_sym.bind(8, ps.body_fp);
    stmts.ins_sym.bind(9, static_cast<std::int64_t>(s.start_line));
    stmts.ins_sym.bind(10, static_cast<std::int64_t>(s.end_line));
    stmts.ins_sym.step();

    for (const std::string& token : ps.tokens) {
      stmts.ins_token.reset();
      stmts.ins_token.bind(1, pid);
      stmts.ins_token.bind(2, token);
      stmts.ins_token.bind(3, ps.sid);
      stmts.ins_token.step();
    }

    stmts.ins_vec.reset();
    stmts.ins_vec.bind(1, ps.sid);
    stmts.ins_vec.bind(2, pid);
    stmts.ins_vec.bind_blob(3, ps.vec_blob);
    stmts.ins_vec.step();

    ++stats.symbols_written;
  }

  for (const SourceFact& f : c.parsed.facts) {
    std::string_view kind = "import";
    if (f.kind == FactKind::call) kind = "call";
    if (f.kind == FactKind::inherits) kind = "inherits";
    stmts.ins_fact.reset();
    stmts.ins_fact.bind(1, c.fid);
    stmts.ins_fact.bind(2, c.symbols[f.src].sid);
    stmts.ins_fact.bind(3, kind);
    stmts.ins_fact.bind(4, f.name);
    stmts.ins_fact.bind(5, f.qualifier);
    stmts.ins_fact.bind(6, f.scope);
    stmts.ins_fact.bind(7, static_cast<std::int64_t>(f.line));
    stmts.ins_fact.step();
  }

  for (const SourceBinding& b : c.parsed.bindings) {
    stmts.ins_bind.reset();
    stmts.ins_bind.bind(1, c.fid);
    stmts.ins_bind.bind(2, b.local);
    stmts.ins_bind.bind(3, b.target);
    stmts.ins_bind.step();
  }
  ++stats.files_indexed;
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
  std::erase_if(candidates,
                [](const std::string& rel) { return language_of(rel).empty(); });
  const std::size_t total = candidates.size();
  const std::size_t batch = std::max<std::size_t>(1, options.batch_size);
  const auto cancelled = [&] {
    return options.cancel != nullptr && options.cancel->load();
  };

  std::unordered_map<std::string, std::string> existing_shas;
  if (!options.force) {
    Statement q(db, "SELECT rel_path, content_sha256 FROM file WHERE project_id = ?");
    q.bind(1, pid);
    while (q.step()) existing_shas.emplace(q.text(0), q.text(1));
  }

  db.exec("PRAGMA wal_autocheckpoint = 0;");
  BatchStatements stmts(db);
  const std::size_t hw_threads =
      std::clamp<std::size_t>(std::thread::hardware_concurrency(), 1, 8);

  std::size_t done = 0;
  while (done < total && !cancelled()) {
    const std::size_t count = std::min(batch, total - done);
    std::vector<PreparedCandidate> prepared(count);
    const std::size_t workers = std::min(hw_threads, count);
    if (workers <= 1 || options.cancel != nullptr) {
      std::size_t processed = 0;
      for (; processed < count && !cancelled(); ++processed) {
        prepared[processed] = prepare_candidate(
            pid, base, options.force, candidates[done + processed], existing_shas);
      }
      prepared.resize(processed);
    } else {
      std::atomic<std::size_t> next{0};
      std::vector<std::thread> pool;
      pool.reserve(workers);
      for (std::size_t w = 0; w < workers; ++w) {
        pool.emplace_back([&] {
          for (;;) {
            const std::size_t idx = next.fetch_add(1, std::memory_order_relaxed);
            if (idx >= count) break;
            prepared[idx] = prepare_candidate(pid, base, options.force,
                                              candidates[done + idx], existing_shas);
          }
        });
      }
      for (std::thread& t : pool) t.join();
    }

    Transaction tx(db);
    for (std::size_t n = 0; n < prepared.size() && !cancelled(); ++n, ++done) {
      apply_prepared(db, pid, prepared[n], stmts, stats);
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
  out.symbols =
      count("SELECT COUNT(*) FROM symbol WHERE project_id = ? AND kind != 'module'");
  out.symbols_without_tokens = count(
      "SELECT COUNT(*) FROM symbol s WHERE s.project_id = ? AND s.kind != 'module' AND "
      "NOT EXISTS "
      "(SELECT 1 FROM token t WHERE t.symbol_id = s.id)");
  out.symbols_without_vectors = count(
      "SELECT COUNT(*) FROM symbol s WHERE s.project_id = ? AND s.kind != 'module' AND "
      "NOT EXISTS "
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
