#include "common/local_registry_store.h"
#include "hklmreg/reg_file.h"
#include "test_tmp.h"

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

using namespace hklmwrap;

namespace {

std::wstring MakeTempDbPath() {
  auto base = testutil::GetTestTempDir("db");
  REQUIRE_FALSE(base.empty());

  static size_t counter = 0;
  counter++;

  auto path = base / ("regfile-" + std::to_string(counter) + ".sqlite");
  std::error_code ec;
  std::filesystem::remove(path, ec);
  return path.wstring();
}

static bool ContainsLine(const std::wstring& text, const std::wstring& needleLine) {
  // `BuildRegExportContent` uses CRLF. Normalize by searching for the exact line.
  return text.find(needleLine + L"\r\n") != std::wstring::npos;
}

} // namespace

TEST_CASE("hklmreg reg import/export preserves empty keys", "[hklmreg][regfile]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  // Synthetic sample data only (no real-world app/vendor names).
  // Covers: empty keys, multiple subkeys, REG_SZ, REG_DWORD, REG_QWORD, REG_BINARY, and default value (@).
  const std::wstring regText =
      L"Windows Registry Editor Version 5.00\r\n\r\n"
      L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp]\r\n"
      L"@=\"Example Default\"\r\n"
      L"\"InstallDir\"=\"C:\\\\Program Files\\\\Example App\"\r\n"
      L"\"Answer\"=dword:0000002a\r\n"
      L"\"Big\"=hex(b):88,77,66,55,44,33,22,11\r\n"
      L"\"Blob\"=hex:de,ad,be,ef\r\n\r\n"
      L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp\\Settings]\r\n"
      L"\"Theme\"=\"Dark\"\r\n\r\n"
      L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp\\EmptyA]\r\n\r\n"
      L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp\\EmptyB\\Child]\r\n\r\n";

  REQUIRE(regfile::ImportRegText(store, regText));

  const auto rows = store.ExportAll();
  const std::wstring out = regfile::BuildRegExportContent(rows, L"");

  CHECK(ContainsLine(out, L"Windows Registry Editor Version 5.00"));
  CHECK(ContainsLine(out, L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp]"));
  CHECK(ContainsLine(out, L"@=\"Example Default\""));
  CHECK(ContainsLine(out, L"\"InstallDir\"=\"C:\\\\Program Files\\\\Example App\""));
  CHECK(ContainsLine(out, L"\"Answer\"=dword:0000002a"));
  CHECK(ContainsLine(out, L"\"Big\"=hex(b):88,77,66,55,44,33,22,11"));
  CHECK(ContainsLine(out, L"\"Blob\"=hex:de,ad,be,ef"));

  CHECK(ContainsLine(out, L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp\\Settings]"));
  CHECK(ContainsLine(out, L"\"Theme\"=\"Dark\""));

  // Empty keys should still be present as headers.
  CHECK(ContainsLine(out, L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp\\EmptyA]"));
  CHECK(ContainsLine(out, L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp\\EmptyB\\Child]"));
}

TEST_CASE("hklmreg reg import handles hex(n) typed values", "[hklmreg][regfile]") {
  LocalRegistryStore store;
  const std::wstring dbPath = MakeTempDbPath();
  REQUIRE(store.Open(dbPath));

  const std::wstring regText =
      L"Windows Registry Editor Version 5.00\r\n\r\n"
      L"[HKEY_LOCAL_MACHINE\\SOFTWARE\\ExampleVendor\\ExampleApp]\r\n"
      L"\"0\"=hex(0):\r\n"
      L"\"X\"=hex(2):01,02,0a,ff\r\n\r\n";

  REQUIRE(regfile::ImportRegText(store, regText));

  {
    const auto v = store.GetValue(L"HKLM\\SOFTWARE\\ExampleVendor\\ExampleApp", L"0");
    REQUIRE(v.has_value());
    CHECK(v->isDeleted == false);
    CHECK(v->type == 0u);
    CHECK(v->data.empty());
  }

  {
    const auto v = store.GetValue(L"HKLM\\SOFTWARE\\ExampleVendor\\ExampleApp", L"X");
    REQUIRE(v.has_value());
    CHECK(v->isDeleted == false);
    CHECK(v->type == 2u);
    REQUIRE(v->data.size() == 4);
    CHECK(v->data[0] == 0x01);
    CHECK(v->data[1] == 0x02);
    CHECK(v->data[2] == 0x0a);
    CHECK(v->data[3] == 0xff);
  }
}
