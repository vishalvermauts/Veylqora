#include "veylqora/memory/transfer.hpp"

#include "veylqora/core/error.hpp"
#include "veylqora/core/project.hpp"

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace veylqora {
namespace fs = std::filesystem;
namespace {

using Record = std::map<std::string, std::string>;
using Fields = std::vector<std::pair<std::string, std::string>>;

std::string escape(std::string_view s) {
  constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(s.size() + 2);
  for (const char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (c == '"') {
      out += "\\\"";
    } else if (c == '\\') {
      out += "\\\\";
    } else if (c == '\n') {
      out += "\\n";
    } else if (c == '\r') {
      out += "\\r";
    } else if (c == '\t') {
      out += "\\t";
    } else if (u < 0x20) {
      out += "\\u00";
      out += kHex[u >> 4];
      out += kHex[u & 0xF];
    } else {
      out += c;  // UTF-8 bytes pass through unchanged
    }
  }
  return out;
}

// One object per line, every value a string (integers included), so the reader
// needs no type information.
std::string to_line(const std::string& kind, const Fields& fields) {
  std::string line = "{\"kind\":\"" + kind + "\"";
  for (const auto& [k, v] : fields) line += ",\"" + k + "\":\"" + escape(v) + "\"";
  line += "}";
  return line;
}

// Parses one line written by to_line. Anything else is rejected with its line number.
Record parse_line(std::string_view line, std::size_t number) {
  std::size_t i = 0;
  const auto fail = [&](const std::string& why) {
    return Error("import line " + std::to_string(number) + ": " + why);
  };
  const auto skip_ws = [&] {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
  };
  const auto read_string = [&]() -> std::string {
    if (i >= line.size() || line[i] != '"') throw fail("expected a string");
    ++i;
    std::string out;
    while (i < line.size() && line[i] != '"') {
      if (line[i] != '\\') {
        out += line[i++];
        continue;
      }
      if (++i >= line.size()) throw fail("truncated escape");
      const char e = line[i];
      if (e == '"' || e == '\\') {
        out += e;
      } else if (e == 'n') {
        out += '\n';
      } else if (e == 'r') {
        out += '\r';
      } else if (e == 't') {
        out += '\t';
      } else if (e == 'u') {
        if (i + 4 >= line.size()) throw fail("truncated \\u escape");
        const std::string hex(line.substr(i + 1, 4));
        if (hex.substr(0, 2) != "00") throw fail("only \\u00XX escapes are produced");
        out += static_cast<char>(std::stoi(hex, nullptr, 16));
        i += 4;
      } else {
        throw fail("unknown escape");
      }
      ++i;
    }
    if (i >= line.size()) throw fail("unterminated string");
    ++i;
    return out;
  };

  Record rec;
  skip_ws();
  if (i >= line.size() || line[i] != '{') throw fail("expected an object");
  ++i;
  skip_ws();
  if (i < line.size() && line[i] == '}') return rec;
  while (true) {
    skip_ws();
    std::string key = read_string();
    skip_ws();
    if (i >= line.size() || line[i] != ':') throw fail("expected ':'");
    ++i;
    skip_ws();
    rec[key] = read_string();
    skip_ws();
    if (i < line.size() && line[i] == ',') {
      ++i;
      continue;
    }
    if (i < line.size() && line[i] == '}') return rec;
    throw fail("expected ',' or '}'");
  }
}

}  // namespace

std::size_t export_memories(Database& db, const fs::path& root, const fs::path& out_file) {
  const std::string pid = project_id_for(root);
  std::ostringstream out;
  {
    Statement p(db, "SELECT id, name, root_path, created_at FROM project WHERE id = ?");
    p.bind(1, pid);
    if (!p.step()) throw Error("project is not in the database; index or create a memory first");
    out << to_line("project", {{"id", p.text(0)},
                               {"name", p.text(1)},
                               {"root_path", p.text(2)},
                               {"created_at", p.text(3)}})
        << '\n';
  }

  std::vector<std::string> ids;
  {
    Statement all(db, "SELECT id FROM memory WHERE project_id = ? ORDER BY id");
    all.bind(1, pid);
    while (all.step()) ids.push_back(all.text(0));
  }

  for (const std::string& id : ids) {
    Statement m(db,
                "SELECT id, project_id, type, content, state, scope, author, revision_no, "
                "COALESCE(superseded_by, ''), created_at, updated_at FROM memory WHERE id = ?");
    m.bind(1, id);
    m.step();
    out << to_line("memory", {{"id", m.text(0)},
                              {"project_id", m.text(1)},
                              {"type", m.text(2)},
                              {"content", m.text(3)},
                              {"state", m.text(4)},
                              {"scope", m.text(5)},
                              {"author", m.text(6)},
                              {"revision_no", std::to_string(m.integer(7))},
                              {"superseded_by", m.text(8)},
                              {"created_at", m.text(9)},
                              {"updated_at", m.text(10)}})
        << '\n';

    Statement r(db,
                "SELECT revision_no, content, state, actor, reason, changed_at FROM memory_revision "
                "WHERE memory_id = ? ORDER BY revision_no");
    r.bind(1, id);
    while (r.step()) {
      out << to_line("revision", {{"memory_id", id},
                                  {"revision_no", std::to_string(r.integer(0))},
                                  {"content", r.text(1)},
                                  {"state", r.text(2)},
                                  {"actor", r.text(3)},
                                  {"reason", r.text(4)},
                                  {"changed_at", r.text(5)}})
          << '\n';
    }

    Statement e(db,
                "SELECT from_state, to_state, actor, reason, at FROM memory_state_event "
                "WHERE memory_id = ? ORDER BY id");
    e.bind(1, id);
    while (e.step()) {
      out << to_line("event", {{"memory_id", id},
                               {"from_state", e.text(0)},
                               {"to_state", e.text(1)},
                               {"actor", e.text(2)},
                               {"reason", e.text(3)},
                               {"at", e.text(4)}})
          << '\n';
    }

    Statement v(db,
                "SELECT rel_path, symbol_fqn, content_fingerprint FROM evidence "
                "WHERE memory_id = ? ORDER BY id");
    v.bind(1, id);
    while (v.step()) {
      out << to_line("evidence", {{"memory_id", id},
                                  {"rel_path", v.text(0)},
                                  {"symbol_fqn", v.text(1)},
                                  {"content_fingerprint", v.text(2)}})
          << '\n';
    }
  }

  std::ofstream file(out_file, std::ios::binary | std::ios::trunc);
  if (!file) throw Error("cannot write " + out_file.string());
  file << out.str();
  return ids.size();
}

std::size_t import_memories(Database& db, const fs::path& in_file) {
  std::ifstream file(in_file, std::ios::binary);
  if (!file) throw Error("cannot read " + in_file.string());
  std::vector<Record> records;
  std::string line;
  for (std::size_t n = 1; std::getline(file, line); ++n) {
    if (line.empty()) continue;
    records.push_back(parse_line(line, n));
  }
  if (records.empty() || records.front()["kind"] != "project") {
    throw Error("import file must start with a project record");
  }

  Transaction tx(db);
  // A memory may cite a memory that appears later in the file (supersession),
  // so foreign keys are checked at commit rather than per row.
  db.exec("PRAGMA defer_foreign_keys = ON;");
  std::size_t memories = 0;
  for (const Record& r : records) {
    const std::string& kind = r.at("kind");
    if (kind == "project") {
      Statement p(db, "INSERT INTO project (id, name, root_path, created_at) VALUES (?, ?, ?, ?)");
      p.bind(1, r.at("id"));
      p.bind(2, r.at("name"));
      p.bind(3, r.at("root_path"));
      p.bind(4, r.at("created_at"));
      p.step();
    } else if (kind == "memory") {
      Statement m(db,
                  "INSERT INTO memory (id, project_id, type, content, state, scope, author, "
                  "revision_no, superseded_by, created_at, updated_at) "
                  "VALUES (?, ?, ?, ?, ?, ?, ?, ?, NULLIF(?, ''), ?, ?)");
      m.bind(1, r.at("id"));
      m.bind(2, r.at("project_id"));
      m.bind(3, r.at("type"));
      m.bind(4, r.at("content"));
      m.bind(5, r.at("state"));
      m.bind(6, r.at("scope"));
      m.bind(7, r.at("author"));
      m.bind(8, static_cast<std::int64_t>(std::stoll(r.at("revision_no"))));
      m.bind(9, r.at("superseded_by"));
      m.bind(10, r.at("created_at"));
      m.bind(11, r.at("updated_at"));
      m.step();
      ++memories;
    } else if (kind == "revision") {
      Statement q(db,
                  "INSERT INTO memory_revision (memory_id, revision_no, content, state, actor, "
                  "reason, changed_at) VALUES (?, ?, ?, ?, ?, ?, ?)");
      q.bind(1, r.at("memory_id"));
      q.bind(2, static_cast<std::int64_t>(std::stoll(r.at("revision_no"))));
      q.bind(3, r.at("content"));
      q.bind(4, r.at("state"));
      q.bind(5, r.at("actor"));
      q.bind(6, r.at("reason"));
      q.bind(7, r.at("changed_at"));
      q.step();
    } else if (kind == "event") {
      Statement q(db,
                  "INSERT INTO memory_state_event (memory_id, from_state, to_state, actor, reason, at) "
                  "VALUES (?, ?, ?, ?, ?, ?)");
      q.bind(1, r.at("memory_id"));
      q.bind(2, r.at("from_state"));
      q.bind(3, r.at("to_state"));
      q.bind(4, r.at("actor"));
      q.bind(5, r.at("reason"));
      q.bind(6, r.at("at"));
      q.step();
    } else if (kind == "evidence") {
      Statement q(db,
                  "INSERT INTO evidence (memory_id, project_id, rel_path, symbol_fqn, "
                  "content_fingerprint) SELECT ?, project_id, ?, ?, ? FROM memory WHERE id = ?");
      q.bind(1, r.at("memory_id"));
      q.bind(2, r.at("rel_path"));
      q.bind(3, r.at("symbol_fqn"));
      q.bind(4, r.at("content_fingerprint"));
      q.bind(5, r.at("memory_id"));
      q.step();
    } else {
      throw Error("unknown record kind: " + kind);
    }
  }
  tx.commit();
  return memories;
}

}  // namespace veylqora
