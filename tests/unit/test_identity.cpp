#include "veylqora/core/identity.hpp"

#include <catch2/catch_test_macros.hpp>

using veylqora::body_fingerprint;
using veylqora::symbol_id;
using veylqora::SymbolKey;
using veylqora::SymbolKind;

namespace {

SymbolKey base_key() {
  return SymbolKey{.project = "proj-1",
                   .language = "python",
                   .path = "src/auth/login.py",
                   .fqn = "auth.login.verify_token",
                   .kind = SymbolKind::function,
                   .overload = 0};
}

}  // namespace

TEST_CASE("symbol ids are stable and well formed") {
  const auto id = symbol_id(base_key());
  CHECK(id.size() == 64);
  CHECK(id == symbol_id(base_key()));
}

TEST_CASE("identity distinguishes every identity-bearing field") {
  const auto id = symbol_id(base_key());

  auto other_project = base_key();
  other_project.project = "proj-2";
  CHECK(symbol_id(other_project) != id);

  auto other_path = base_key();
  other_path.path = "src/auth/logout.py";
  CHECK(symbol_id(other_path) != id);

  auto other_kind = base_key();
  other_kind.kind = SymbolKind::method;
  CHECK(symbol_id(other_kind) != id);

  auto other_overload = base_key();
  other_overload.overload = 1;
  CHECK(symbol_id(other_overload) != id);
}

TEST_CASE("field boundaries cannot be forged by moving text between fields") {
  // Without an unambiguous separator, ("a", "bc") and ("ab", "c") would hash
  // to the same value and two unrelated symbols would share an identity.
  auto left = base_key();
  left.language = "py";
  left.path = "thon/src/auth/login.py";

  auto right = base_key();
  right.language = "python";
  right.path = "src/auth/login.py";

  CHECK(symbol_id(left) != symbol_id(right));
}

TEST_CASE("body fingerprint ignores formatting-only changes") {
  const auto unix_body = "def f():\n    return 1\n";
  CHECK(body_fingerprint(unix_body) == body_fingerprint("def f():\r\n    return 1\r\n"));
  CHECK(body_fingerprint(unix_body) == body_fingerprint("def f():   \n    return 1\t\n"));
  CHECK(body_fingerprint(unix_body) ==
        body_fingerprint("\n\ndef f():\n    return 1\n\n\n"));
}

TEST_CASE("body fingerprint detects real changes") {
  CHECK(body_fingerprint("def f():\n    return 1\n") !=
        body_fingerprint("def f():\n    return 2\n"));

  // An interior blank line is a real difference, not formatting noise we can
  // normalise away without merging distinct bodies.
  CHECK(body_fingerprint("a\nb\n") != body_fingerprint("a\n\nb\n"));
}

TEST_CASE("empty and whitespace-only bodies fingerprint identically") {
  CHECK(body_fingerprint("") == body_fingerprint("\n\n   \n"));
}
