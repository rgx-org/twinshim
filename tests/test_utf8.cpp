#include "common/utf8.h"

#include <catch2/catch_test_macros.hpp>

using namespace hklmwrap;

TEST_CASE("UTF-8 round trip works for ASCII and Unicode", "[utf8]") {
  const std::wstring text = L"Hello 世界 ✓";
  const std::string utf8 = WideToUtf8(text);
  REQUIRE(!utf8.empty());

  const std::wstring roundTrip = Utf8ToWide(utf8);
  CHECK(roundTrip == text);
}

TEST_CASE("UTF-8 conversion preserves embedded NUL", "[utf8]") {
  const std::wstring wide = std::wstring(L"A\0B", 3);
  const std::string utf8 = WideToUtf8(wide);
  REQUIRE(utf8.size() >= 3);

  const std::wstring decoded = Utf8ToWide(utf8);
  CHECK(decoded == wide);
  CHECK(decoded.size() == 3);
}
