#pragma once

#include "veylqora/core/runtime.hpp"
#include "veylqora/observability/observability.hpp"
#include "veylqora/storage/database.hpp"

#include <httplib.h>
#include <mutex>
#include <string>

namespace veylqora {

/// The local HTTP API (ADR-0012). The server is the single writer of its
/// database. Every request runs under one mutex, so SQLite sees one writer and
/// one transaction at a time.
class Server {
 public:
  Server(Database& db, const Clock& clock, IdGenerator& ids);

  /// Binds to `host:port` and returns the bound port (0 picks a free one).
  /// Refuses any host other than the loopback address: remote exposure needs
  /// authentication, which this version does not have.
  int bind(const std::string& host, int port);

  /// Serves the built web UI from `dir` at "/". API routes take precedence.
  void mount_web(const std::string& dir);

  /// Query-log and retention settings. Applies to requests after the call.
  void set_observability(const ObservabilityOptions& options);

  /// Serves requests until stop() is called. Expired log rows are purged first.
  void serve();
  void stop();

 private:
  void add_routes();

  httplib::Server http_;
  Database& db_;
  const Clock& clock_;
  IdGenerator& ids_;
  ObservabilityOptions obs_;
  std::mutex mu_;
};

}  // namespace veylqora
