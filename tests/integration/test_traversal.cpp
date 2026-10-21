// M3.3 traversal and isolation: the static web mount never serves a file from
// outside its directory, and hostile path inputs to the API fail cleanly.

#include "veylqora/core/runtime.hpp"
#include "veylqora/server/server.hpp"
#include "veylqora/storage/database.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <httplib.h>
#include <string>
#include <thread>

#include "support/temp_dir.hpp"
#include <catch2/catch_test_macros.hpp>

namespace fs = std::filesystem;
using veylqora::test::TempDir;

namespace {

constexpr const char* kSecret = "TOP-SECRET-OUTSIDE-THE-WEB-ROOT";

void write(const fs::path& p, const std::string& text) {
  fs::create_directories(p.parent_path());
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << text;
}

// Serves with a mounted web directory for the lifetime of the object.
class WebServer {
 public:
  WebServer(veylqora::Database& db, const veylqora::Clock& clock, veylqora::IdGenerator& ids,
            const fs::path& web)
      : server_(db, clock, ids) {
    server_.mount_web(web.string());
    port_ = server_.bind("127.0.0.1", 0);
    thread_ = std::thread([this] { server_.serve(); });
    httplib::Client probe("http://127.0.0.1:" + std::to_string(port_));
    for (int i = 0; i < 200; ++i) {
      if (probe.Get("/api/v1/health")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  ~WebServer() {
    server_.stop();
    thread_.join();
  }
  int port() const { return port_; }

 private:
  veylqora::Server server_;
  int port_ = 0;
  std::thread thread_;
};

}  // namespace

TEST_CASE("static web mount: traversal to a file outside the root is never served") {
  TempDir tmp;
  const fs::path web = tmp.path() / "web";
  write(web / "index.html", "<html>ok</html>");
  write(tmp.path() / "secret.txt", kSecret);

  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  WebServer live(db, clock, ids, web);
  httplib::Client client("http://127.0.0.1:" + std::to_string(live.port()));

  const auto served = client.Get("/index.html");
  REQUIRE(served);
  CHECK(served->status == 200);
  CHECK(served->body.find("ok") != std::string::npos);

  const char* attacks[] = {
      "/../secret.txt",          "/..%2fsecret.txt",       "/%2e%2e/secret.txt",
      "/%2e%2e%2fsecret.txt",    "/..%5csecret.txt",       "/%2e%2e%5csecret.txt",
      "/%252e%252e/secret.txt",  "/..//secret.txt",        "/....//secret.txt",
      "/./../secret.txt",        "/index.html/../../secret.txt",
  };
  for (const char* path : attacks) {
    INFO("path: " << path);
    const auto res = client.Get(path);
    const bool leaked = res && res->body.find(kSecret) != std::string::npos;
    CHECK_FALSE(leaked);
  }
}

TEST_CASE("API path inputs that look like traversal fail cleanly, never with a server error") {
  TempDir tmp;
  const fs::path web = tmp.path() / "web";
  write(web / "index.html", "ok");

  veylqora::Database db(tmp.path() / "v.db");
  const veylqora::SystemClock clock;
  veylqora::RandomIds ids;
  WebServer live(db, clock, ids, web);
  httplib::Client client("http://127.0.0.1:" + std::to_string(live.port()));

  const char* paths[] = {
      "/api/v1/memories/..%2F..%2Fetc%2Fpasswd?root=/",
      "/api/v1/memories/../../secret?root=/",
      "/api/v1/status?root=..%2F..%2F",
      "/api/v1/retrieve?root=/&query=x&path_prefix=..%2F..%2F",
  };
  for (const char* path : paths) {
    INFO("path: " << path);
    const auto res = client.Get(path);
    REQUIRE(res);
    CHECK(res->status != 500);
    CHECK(res->body.find("root:") == std::string::npos);  // no file content echoed back
  }
}
