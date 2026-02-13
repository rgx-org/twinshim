#include "common/arg_quote.h"

#include <catch2/catch_test_macros.hpp>

#include <vector>

using namespace hklmwrap;

TEST_CASE("QuoteWindowsCommandLineArg handles basic quoting", "[arg_quote]") {
  CHECK(QuoteWindowsCommandLineArg(L"") == L"\"\"");
  CHECK(QuoteWindowsCommandLineArg(L"plain") == L"plain");
  CHECK(QuoteWindowsCommandLineArg(L"has space") == L"\"has space\"");
  CHECK(QuoteWindowsCommandLineArg(L"a\"b") == L"\"a\\\"b\"");
}

TEST_CASE("QuoteWindowsCommandLineArg preserves trailing backslashes", "[arg_quote]") {
  CHECK(QuoteWindowsCommandLineArg(L"C:\\Path\\") == L"C:\\Path\\");
  CHECK(QuoteWindowsCommandLineArg(L"C:\\Path With Space\\") == L"\"C:\\Path With Space\\\\\"");
}

TEST_CASE("BuildCommandLine quotes executable and arguments", "[arg_quote]") {
  std::wstring exe = L"C:\\Program Files\\Tool\\app.exe";
  std::vector<std::wstring> args = {L"--mode", L"fast run", L"a\"b"};

  const std::wstring cmd = BuildCommandLine(exe, args);
  CHECK(cmd == L"\"C:\\Program Files\\Tool\\app.exe\" --mode \"fast run\" \"a\\\"b\"");
}

TEST_CASE("BuildCommandLine preserves embedded NUL bytes", "[arg_quote]") {
  const std::wstring embedded = std::wstring(L"ab\0cd", 5);
  const std::wstring cmd = BuildCommandLine(L"tool.exe", {embedded});

  REQUIRE(cmd.size() >= 7);
  CHECK(cmd.find(L'\0') != std::wstring::npos);
}
