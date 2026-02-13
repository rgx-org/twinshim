#include "common/path_util.h"

#include <catch2/catch_test_macros.hpp>

using namespace hklmwrap;

TEST_CASE("NormalizeSlashes converts forward slashes", "[path]") {
  CHECK(NormalizeSlashes(L"A/B/C") == L"A\\B\\C");
}

TEST_CASE("GetDirectoryName/GetFileName/GetFileStem handle common forms", "[path]") {
  CHECK(GetDirectoryName(L"C:/Temp/file.txt") == L"C:\\Temp");
  CHECK(GetFileName(L"C:/Temp/file.txt") == L"file.txt");
  CHECK(GetFileStem(L"C:/Temp/file.txt") == L"file");
  CHECK(GetFileStem(L"archive.tar.gz") == L"archive.tar");
}

TEST_CASE("CombinePath handles empty and trailing separator cases", "[path]") {
  CHECK(CombinePath(L"", L"child") == L"child");
  CHECK(CombinePath(L"parent", L"") == L"parent");
  CHECK(CombinePath(L"parent", L"child") == L"parent\\child");
  CHECK(CombinePath(L"parent\\", L"child") == L"parent\\child");
}
