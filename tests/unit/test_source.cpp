#include "veylqora/ingestion/source.hpp"

#include <catch2/catch_test_macros.hpp>

using veylqora::is_valid_utf8;

TEST_CASE("well-formed UTF-8 is accepted, including multi-byte and 4-byte sequences") {
  CHECK(is_valid_utf8("plain ascii"));
  CHECK(is_valid_utf8("caf\xC3\xA9"));          // U+00E9, 2 bytes
  CHECK(is_valid_utf8("\xE2\x82\xAC"));         // U+20AC, 3 bytes
  CHECK(is_valid_utf8("\xF0\x9F\x98\x80"));     // U+1F600, 4 bytes
  CHECK(is_valid_utf8("\xEF\xBB\xBFwith bom")); // BOM is valid UTF-8
  CHECK(is_valid_utf8(""));
}

TEST_CASE("malformed UTF-8 is rejected") {
  CHECK_FALSE(is_valid_utf8("\x80"));              // stray continuation byte
  CHECK_FALSE(is_valid_utf8("\xC0\xAF"));          // overlong '/'
  CHECK_FALSE(is_valid_utf8("\xE0\x80\xAF"));      // overlong, 3-byte form
  CHECK_FALSE(is_valid_utf8("\xED\xA0\x80"));      // UTF-16 surrogate U+D800
  CHECK_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // above U+10FFFF
  CHECK_FALSE(is_valid_utf8("\xF5\x80\x80\x80"));  // lead byte out of range
  CHECK_FALSE(is_valid_utf8("\xE2\x82"));          // truncated sequence
  CHECK_FALSE(is_valid_utf8("\xFF"));              // never valid
}
