#include "veylqora/retrieval/search.hpp"

#include "veylqora/core/error.hpp"
#include "veylqora/core/project.hpp"
#include "veylqora/retrieval/text.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <utility>

namespace veylqora {
namespace {

constexpr double kRrfK = 60.0;        // reciprocal rank fusion constant (ADR-0015)
constexpr double kGraphWeight = 0.5;  // graph neighbours count half as much
constexpr std::size_t kSeeds = 10;    // top results whose neighbours are explored

struct Cand {
  std::string id;
  std::string fqn;
  std::string path;
  std::string kind;
  std::int64_t line;
};

// A ranked list of symbol ids: best first, ties broken by id so order is fixed.
using Ranking = std::vector<std::string>;

void sort_by_score(std::vector<std::pair<std::string, double>>& scored) {
  std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });
}

// Candidates in this project that pass the metadata filters.
std::unordered_map<std::string, Cand> candidates(Database& db, const std::string& pid,
                                                 const RetrievalOptions& options) {
  std::unordered_map<std::string, Cand> out;
  Statement q(
      db,
      "SELECT s.id, s.fqn, f.rel_path, s.kind, s.start_line FROM symbol s "
      "JOIN file f ON f.id = s.file_id WHERE s.project_id = ? AND s.kind != 'module'");
  q.bind(1, pid);
  while (q.step()) {
    Cand c{q.text(0), q.text(1), q.text(2), q.text(3), q.integer(4)};
    if (!options.kind.empty() && c.kind != options.kind) continue;
    if (!options.path_prefix.empty() &&
        c.path.compare(0, options.path_prefix.size(), options.path_prefix) != 0) {
      continue;
    }
    out.emplace(c.id, std::move(c));
  }
  return out;
}

Ranking lexical_ranking(Database& db, const std::string& pid,
                        const std::vector<std::string>& tokens,
                        const std::unordered_map<std::string, Cand>& cands) {
  std::unordered_map<std::string, double> hits;
  for (const std::string& t : tokens) {
    Statement q(db, "SELECT symbol_id FROM token WHERE project_id = ? AND token = ?");
    q.bind(1, pid);
    q.bind(2, t);
    while (q.step()) {
      if (cands.count(q.text(0)) != 0) hits[q.text(0)] += 1.0;
    }
  }
  std::vector<std::pair<std::string, double>> scored(hits.begin(), hits.end());
  sort_by_score(scored);
  Ranking out;
  for (auto& [id, score] : scored) out.push_back(id);
  return out;
}

Ranking vector_ranking(Database& db, const std::string& pid,
                       const std::vector<std::string>& tokens,
                       const std::unordered_map<std::string, Cand>& cands) {
  const auto query = embed(tokens, 1.0F);
  std::vector<std::pair<std::string, double>> scored;
  Statement q(db, "SELECT symbol_id, vec FROM symbol_vec WHERE project_id = ?");
  q.bind(1, pid);
  while (q.step()) {
    if (cands.count(q.text(0)) == 0) continue;
    const std::string blob = q.blob(1);
    if (blob.size() != sizeof(float) * kVectorDims) continue;
    std::array<float, kVectorDims> v{};
    std::memcpy(v.data(), blob.data(), blob.size());
    const double score = dot(query, v);
    if (score > 0.0) scored.emplace_back(q.text(0), score);
  }
  sort_by_score(scored);
  Ranking out;
  for (auto& [id, score] : scored) out.push_back(id);
  return out;
}

// Symbols one step away from the seeds over resolved and heuristic edges, ranked
// by how many seeds reach them.
Ranking graph_ranking(Database& db, const Ranking& seeds,
                      const std::unordered_map<std::string, Cand>& cands) {
  std::unordered_map<std::string, double> reached;
  Statement out_q(db,
                  "SELECT dst_symbol_id FROM edge WHERE src_symbol_id = ? AND "
                  "dst_symbol_id IS NOT NULL "
                  "AND resolution != 'unresolved' AND kind != 'contains'");
  Statement in_q(db,
                 "SELECT src_symbol_id FROM edge WHERE dst_symbol_id = ? AND "
                 "resolution != 'unresolved' AND kind != 'contains'");
  for (const std::string& seed : seeds) {
    for (Statement* q : {&out_q, &in_q}) {
      q->reset();
      q->bind(1, seed);
      while (q->step()) {
        const std::string id = q->text(0);
        if (id != seed && cands.count(id) != 0) reached[id] += 1.0;
      }
    }
  }
  std::vector<std::pair<std::string, double>> scored(reached.begin(), reached.end());
  sort_by_score(scored);
  Ranking out;
  for (auto& [id, score] : scored) out.push_back(id);
  return out;
}

void fuse(std::unordered_map<std::string, double>& score, const Ranking& ranking,
          double weight) {
  for (std::size_t i = 0; i < ranking.size(); ++i) {
    score[ranking[i]] += weight / (kRrfK + static_cast<double>(i + 1));
  }
}

}  // namespace

std::vector<Hit> retrieve(Database& db, const std::filesystem::path& root,
                          std::string_view query, const RetrievalOptions& options) {
  if (query.size() > kMaxQueryChars) throw Error("query is longer than 512 characters");
  if (options.top_k == 0) throw Error("top_k must be at least 1");
  const std::vector<std::string> tokens = tokenize(query);
  if (tokens.empty()) throw Error("query has no searchable terms");

  const std::string pid = project_id_for(root);
  const auto cands = candidates(db, pid, options);
  const std::size_t k = std::min(options.top_k, kMaxResults);

  std::unordered_map<std::string, double> score;
  Ranking lexical;
  Ranking vec;
  if (options.mode != Mode::vector) lexical = lexical_ranking(db, pid, tokens, cands);
  if (options.mode != Mode::lexical) vec = vector_ranking(db, pid, tokens, cands);

  if (options.mode == Mode::hybrid) {
    fuse(score, lexical, 1.0);
    fuse(score, vec, 1.0);
    Ranking seeds;
    std::unordered_map<std::string, bool> seen;
    for (const Ranking* list : {&lexical, &vec}) {
      for (const std::string& id : *list) {
        if (seeds.size() >= kSeeds) break;
        if (seen.emplace(id, true).second) seeds.push_back(id);
      }
    }
    fuse(score, graph_ranking(db, seeds, cands), kGraphWeight);
  } else {
    fuse(score, options.mode == Mode::lexical ? lexical : vec, 1.0);
  }

  std::vector<std::pair<std::string, double>> ranked(score.begin(), score.end());
  sort_by_score(ranked);
  std::vector<Hit> out;
  for (const auto& [id, s] : ranked) {
    if (out.size() >= k) break;
    const Cand& c = cands.at(id);
    out.push_back(Hit{c.fqn, c.path, c.kind, c.line, s});
  }
  return out;
}

}  // namespace veylqora
