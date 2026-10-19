// Spike S3 (ADR-0002 addendum): how does USearch recall behave as vectors are
// deleted, and does a rebuild restore it?
//
// Throwaway measurement code. It is deliberately outside the product build.
// Corpus: seeded Gaussian mixture on the unit sphere, so neighbourhoods exist
// and the test is not trivially dominated by an unstructured uniform cloud.
// Ground truth is exact brute-force cosine over the live set at every step.

#include <usearch/index_dense.hpp>

namespace usearch = unum::usearch;

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <random>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::size_t kDims = 128;
constexpr std::size_t kN = 20000;
constexpr std::size_t kQueries = 200;
constexpr std::size_t kK = 10;
constexpr std::size_t kClusters = 50;
constexpr float kSpread = 0.35f;

using Vec = std::vector<float>;
using Key = std::uint64_t;
using Clock = std::chrono::steady_clock;

Vec normalise(Vec v) {
  float norm = 0;
  for (float x : v) norm += x * x;
  norm = std::sqrt(norm);
  for (float& x : v) x /= norm;
  return v;
}

float cosine_distance(const Vec& a, const Vec& b) {
  float dot = 0;
  for (std::size_t i = 0; i < kDims; ++i) dot += a[i] * b[i];
  return 1.0f - dot;
}

std::vector<Vec> make_points(std::size_t count, const std::vector<Vec>& centers,
                             std::mt19937& rng) {
  std::normal_distribution<float> noise(0.0f, kSpread);
  std::vector<Vec> points;
  points.reserve(count);
  for (std::size_t i = 0; i < count; ++i) {
    Vec p = centers[i % centers.size()];
    for (float& x : p) x += noise(rng);
    points.push_back(normalise(std::move(p)));
  }
  return points;
}

std::unordered_set<Key> exact_topk(const std::vector<Vec>& data, const std::vector<Key>& live,
                                   const Vec& query) {
  std::vector<std::pair<float, Key>> scored;
  scored.reserve(live.size());
  for (Key k : live) scored.emplace_back(cosine_distance(data[k], query), k);
  std::partial_sort(scored.begin(), scored.begin() + kK, scored.end());
  std::unordered_set<Key> top;
  for (std::size_t i = 0; i < kK; ++i) top.insert(scored[i].second);
  return top;
}

usearch::index_dense_t build_index(const std::vector<Vec>& data, const std::vector<Key>& keys) {
  const auto metric = usearch::metric_punned_t(kDims, usearch::metric_kind_t::cos_k,
                                               usearch::scalar_kind_t::f32_k);
  auto made = usearch::index_dense_t::make(metric);
  if (!made) {
    std::fprintf(stderr, "make failed: %s\n", made.error.what());
    std::exit(1);
  }
  usearch::index_dense_t index = std::move(made.index);
  index.reserve(usearch::index_limits_t(keys.size(), 1));
  for (Key k : keys) {
    auto r = index.add(k, data[k].data());
    if (!r) {
      std::fprintf(stderr, "add failed: %s\n", r.error.what());
      std::exit(1);
    }
  }
  return index;
}

struct Eval {
  double recall;
  double search_us;
  std::size_t leaked;  // returned keys that were deleted: must always be zero
};

Eval evaluate(const usearch::index_dense_t& index, const std::vector<Vec>& data,
              const std::vector<Vec>& queries, const std::vector<Key>& live,
              const std::unordered_set<Key>& deleted) {
  double hits = 0;
  std::size_t leaked = 0;
  double search_ns = 0;
  for (const Vec& q : queries) {
    const auto truth = exact_topk(data, live, q);
    const auto t0 = Clock::now();
    const auto matches = index.search(q.data(), kK);
    search_ns += std::chrono::duration<double, std::nano>(Clock::now() - t0).count();
    for (std::size_t i = 0; i < matches.size(); ++i) {
      const Key key = matches[i].member.key;
      if (deleted.count(key)) ++leaked;
      if (truth.count(key)) hits += 1;
    }
  }
  return Eval{hits / (static_cast<double>(queries.size()) * kK),
              search_ns / 1000.0 / static_cast<double>(queries.size()), leaked};
}

}  // namespace

int main() {
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> unit(-1.0f, 1.0f);

  std::vector<Vec> centers;
  for (std::size_t c = 0; c < kClusters; ++c) {
    Vec v(kDims);
    for (float& x : v) x = unit(rng);
    centers.push_back(normalise(std::move(v)));
  }
  const auto data = make_points(kN, centers, rng);
  const auto queries = make_points(kQueries, centers, rng);

  // A fixed random deletion order. Each step deletes a prefix of it, so the
  // deleted set only grows and every step is a superset of the previous one.
  std::vector<Key> order(kN);
  std::iota(order.begin(), order.end(), Key{0});
  std::shuffle(order.begin(), order.end(), rng);

  std::vector<Key> all(order.begin(), order.end());
  auto index = build_index(data, all);

  std::printf("corpus: n=%zu dims=%zu clusters=%zu queries=%zu k=%zu metric=cos f32\n", kN,
              kDims, kClusters, kQueries, kK);
  std::printf("%-10s %-8s %-11s %-12s %-14s %-12s %-10s\n", "deleted", "live", "index_size",
              "remove_ms", "recall@10", "rebuild@10", "search_us");

  std::unordered_set<Key> deleted;
  for (double frac : {0.0, 0.10, 0.25, 0.50}) {
    const std::size_t target = static_cast<std::size_t>(frac * kN);
    double remove_ms = 0;
    if (deleted.size() < target) {
      const auto t0 = Clock::now();
      while (deleted.size() < target) {
        const Key key = order[deleted.size()];
        auto r = index.remove(key);
        if (!r.completed) {
          std::fprintf(stderr, "remove of key %llu did not complete\n",
                       static_cast<unsigned long long>(key));
          return 1;
        }
        deleted.insert(key);
      }
      remove_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    }

    std::vector<Key> live;
    live.reserve(kN - deleted.size());
    for (Key k = 0; k < kN; ++k) {
      if (!deleted.count(k)) live.push_back(k);
    }

    const Eval tombstoned = evaluate(index, data, queries, live, deleted);
    // Compaction in this design is a rebuild from SQLite, so the rebuild figure
    // is what a compacted index would score.
    const auto rebuilt = build_index(data, live);
    const Eval compacted = evaluate(rebuilt, data, queries, live, deleted);

    std::printf("%-10.2f %-8zu %-11zu %-12.1f %-14.3f %-12.3f %-10.1f", frac, live.size(),
                index.size(), remove_ms, tombstoned.recall, compacted.recall,
                tombstoned.search_us);
    std::printf("  leaked=%zu\n", tombstoned.leaked);
  }
  return 0;
}
