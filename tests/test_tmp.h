#pragma once

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace hklmwrap::testutil {

inline std::filesystem::path GetConfiguredTempBaseDir() {
  if (const char* env = std::getenv("TWINSHIM_TEST_TMP_BASE"); env && *env) {
    return std::filesystem::path(env);
  }
  if (const char* env = std::getenv("HKLM_WRAPPER_TEST_TMP_BASE"); env && *env) {
    return std::filesystem::path(env);
  }

#ifdef HKLM_WRAPPER_TEST_TMP_BASE_DEFAULT
  return std::filesystem::path(HKLM_WRAPPER_TEST_TMP_BASE_DEFAULT);
#else
  return {};
#endif
}

inline std::filesystem::path GetSystemTempBaseDir() {
  std::error_code ec;
  auto base = std::filesystem::temp_directory_path(ec);
  if (ec || base.empty()) {
    return {};
  }
  return base;
}

inline std::filesystem::path GetTestTempDir(const char* subdir) {
  std::error_code ec;

  if (auto configured = GetConfiguredTempBaseDir(); !configured.empty()) {
    auto dir = configured / "twinshim-tests" / subdir;
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
      return dir;
    }
  }

  if (auto system = GetSystemTempBaseDir(); !system.empty()) {
    auto dir = system / "twinshim-tests" / subdir;
    std::filesystem::create_directories(dir, ec);
    if (!ec) {
      return dir;
    }
  }

  return {};
}

} // namespace hklmwrap::testutil
