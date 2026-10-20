#include "veylqora/server/server.hpp"

#include "veylqora/api/service.hpp"
#include "veylqora/core/error.hpp"

#include <cstddef>
#include <exception>
#include <stdexcept>

namespace veylqora {
namespace {

using api::Json;

constexpr std::size_t kMaxBodyBytes = 1024 * 1024;  // request size limit (SECURITY.md)

// Errors become a stable JSON shape and an HTTP status. Callers branch on
// `error.code`, not on the message text.
void reply_error(httplib::Response& res, const std::exception& e) {
  int status = 500;
  std::string code = "internal";
  if (dynamic_cast<const ConflictError*>(&e) != nullptr) {
    status = 409;
    code = "conflict";
  } else if (dynamic_cast<const NotFoundError*>(&e) != nullptr) {
    status = 404;
    code = "not_found";
  } else if (dynamic_cast<const Error*>(&e) != nullptr) {
    status = 400;
    code = "invalid";
  } else if (dynamic_cast<const Json::exception*>(&e) != nullptr) {
    status = 400;
    code = "invalid";
  }
  res.status = status;
  res.set_content(Json{{"error", {{"code", code}, {"message", e.what()}}}}.dump(),
                  "application/json");
}

void reply_json(httplib::Response& res, const Json& body, int status = 200) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

Json parse_body(const httplib::Request& req) {
  if (req.body.empty()) return Json::object();
  return Json::parse(req.body);
}

std::string param(const httplib::Request& req, const char* name) {
  return req.has_param(name) ? req.get_param_value(name) : std::string{};
}

std::filesystem::path root_param(const httplib::Request& req) {
  const std::string root = param(req, "root");
  if (root.empty()) throw Error("missing query parameter: root");
  return root;
}

}  // namespace

Server::Server(Database& db, const Clock& clock, IdGenerator& ids)
    : db_(db), clock_(clock), ids_(ids) {
  http_.set_payload_max_length(kMaxBodyBytes);
  add_routes();
}

int Server::bind(const std::string& host, int port) {
  if (host != "127.0.0.1" && host != "localhost" && host != "::1") {
    throw Error("refusing to bind to " + host +
                ": only loopback is allowed until authentication exists (ADR-0012)");
  }
  if (port == 0) {
    const int bound = http_.bind_to_any_port(host);
    if (bound < 0) throw Error("cannot bind to " + host);
    return bound;
  }
  if (!http_.bind_to_port(host, port)) {
    throw Error("cannot bind to " + host + ":" + std::to_string(port));
  }
  return port;
}

void Server::serve() { http_.listen_after_bind(); }

void Server::stop() { http_.stop(); }

// Every route runs `body` under the lock and maps its exceptions to a reply.
void Server::add_routes() {
  const auto route = [this](const std::string& method, const std::string& pattern,
                            auto&& body) {
    auto handler = [this, body](const httplib::Request& req, httplib::Response& res) {
      try {
        std::lock_guard<std::mutex> lock(mu_);
        reply_json(res, body(req));
      } catch (const std::exception& e) {
        reply_error(res, e);
      }
    };
    if (method == "GET") {
      http_.Get(pattern, handler);
    } else {
      http_.Post(pattern, handler);
    }
  };

  http_.Get("/api/v1/health", [](const httplib::Request&, httplib::Response& res) {
    reply_json(res, api::health());
  });

  route("GET", "/api/v1/status", [this](const httplib::Request& req) {
    return api::status(db_, root_param(req));
  });
  route("POST", "/api/v1/index", [this](const httplib::Request& req) {
    const Json body = parse_body(req);
    return api::index(db_, clock_, std::filesystem::path(body.at("root").get<std::string>()));
  });
  route("GET", "/api/v1/search", [this](const httplib::Request& req) {
    return api::search(db_, root_param(req), param(req, "name"));
  });
  route("GET", "/api/v1/graph", [this](const httplib::Request& req) {
    return api::graph(db_, root_param(req), param(req, "name"));
  });
  route("GET", "/api/v1/doctor", [this](const httplib::Request&) { return api::doctor(db_); });
  route("GET", "/api/v1/memories", [this](const httplib::Request& req) {
    return api::memory_list(db_, root_param(req), param(req, "state"));
  });
  route("POST", "/api/v1/memories", [this](const httplib::Request& req) {
    const Json body = parse_body(req);
    return api::memory_create(db_, clock_, ids_, std::filesystem::path(body.at("root").get<std::string>()), body);
  });

  http_.Get(R"(/api/v1/memories/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
    try {
      std::lock_guard<std::mutex> lock(mu_);
      reply_json(res, api::memory_inspect(db_, root_param(req), req.matches[1].str()));
    } catch (const std::exception& e) {
      reply_error(res, e);
    }
  });
  http_.Post(R"(/api/v1/memories/([^/]+)/update)",
             [this](const httplib::Request& req, httplib::Response& res) {
               try {
                 std::lock_guard<std::mutex> lock(mu_);
                 const Json body = parse_body(req);
                 const auto root = std::filesystem::path(body.at("root").get<std::string>());
                 reply_json(res, api::memory_update(db_, clock_, root, req.matches[1].str(), body));
               } catch (const std::exception& e) {
                 reply_error(res, e);
               }
             });
  http_.Post(R"(/api/v1/memories/([^/]+)/transition)",
             [this](const httplib::Request& req, httplib::Response& res) {
               try {
                 std::lock_guard<std::mutex> lock(mu_);
                 const Json body = parse_body(req);
                 const auto root = std::filesystem::path(body.at("root").get<std::string>());
                 reply_json(res, api::memory_transition(db_, clock_, root, req.matches[1].str(), body));
               } catch (const std::exception& e) {
                 reply_error(res, e);
               }
             });
}

}  // namespace veylqora
