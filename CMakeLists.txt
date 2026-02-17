cmake_minimum_required(VERSION 3.20)

project(TwinShim LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(HKLM_WRAPPER_FETCH_MINHOOK "Fetch MinHook at configure time" ON)
option(HKLM_WRAPPER_ENABLE_TESTS "Build unit tests" ON)
option(HKLM_WRAPPER_MSVC_STATIC_RUNTIME "Use /MT runtime for wrapper/shim on MSVC" ON)
option(HKLM_WRAPPER_ENABLE_DGVOODOO_ADDON "Build dgVoodoo AddOn DLL (requires dgVoodoo AddOn SDK)" OFF)

include(CTest)

if(NOT WIN32)
  message(STATUS "This project targets Windows (Win32 API hooking + process injection).")
  message(STATUS "On non-Windows targets, no build targets are generated.")
  message(STATUS "If you meant to cross-compile from macOS/Linux, configure with a Windows toolchain (see README).")
else()

  if(MSVC AND HKLM_WRAPPER_MSVC_STATIC_RUNTIME)
    # Avoid runtime collisions with app-local MSVC redistributable DLLs in
    # injected processes by linking wrapper/shim against static runtime.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  endif()

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    add_link_options(
      -static
      -static-libgcc
      -static-libstdc++
    )
  endif()

  add_library(hklm_common STATIC
    src/common/arg_quote.cpp
    src/common/arg_quote.h
    src/common/local_registry_store.cpp
    src/common/local_registry_store.h
    src/common/path_util.cpp
    src/common/path_util.h
    src/common/utf8.cpp
    src/common/utf8.h
    src/common/win32_error.cpp
    src/common/win32_error.h
  )

  target_include_directories(hklm_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/config
  )

  target_compile_definitions(hklm_common PUBLIC
    UNICODE
    _UNICODE
    NOMINMAX
  )

  target_link_libraries(hklm_common PUBLIC advapi32)

  # --- SQLite3 dependency (prefer config packages over FindSQLite3 module mode) ---
  set(_sqlite_target "")

  find_package(unofficial-sqlite3 CONFIG QUIET)
  if(TARGET unofficial::sqlite3::sqlite3)
    set(_sqlite_target unofficial::sqlite3::sqlite3)
  endif()

  if(_sqlite_target STREQUAL "")
    find_package(SQLite3 CONFIG QUIET)
    if(TARGET SQLite::SQLite3)
      set(_sqlite_target SQLite::SQLite3)
    elseif(TARGET SQLite3::SQLite3)
      set(_sqlite_target SQLite3::SQLite3)
    endif()
  endif()

  if(_sqlite_target STREQUAL "")
    find_package(SQLite3 QUIET MODULE)
    if(TARGET SQLite::SQLite3)
      set(_sqlite_target SQLite::SQLite3)
    elseif(TARGET SQLite3::SQLite3)
      set(_sqlite_target SQLite3::SQLite3)
    endif()
  endif()

  if(_sqlite_target STREQUAL "")
    message(FATAL_ERROR
      "SQLite3 not found.\n"
      "Options:\n"
      "  - Use vcpkg (recommended): set CMAKE_TOOLCHAIN_FILE to vcpkg.cmake and install sqlite3\n"
      "  - Or install SQLite3 dev packages and ensure CMake can find them\n")
  endif()

  target_link_libraries(hklm_common PUBLIC ${_sqlite_target})

# --- MinHook dependency (for registry API hooking in the injected DLL) ---
  if(HKLM_WRAPPER_FETCH_MINHOOK)
    include(FetchContent)
    FetchContent_Declare(
      minhook
      GIT_REPOSITORY https://github.com/TsudaKageyu/minhook.git
      GIT_TAG v1.3.3
      GIT_SHALLOW TRUE
      GIT_PROGRESS TRUE
    )

    FetchContent_MakeAvailable(minhook)

    file(GLOB minhook_sources CONFIGURE_DEPENDS
      ${minhook_SOURCE_DIR}/src/*.c
      ${minhook_SOURCE_DIR}/src/hde/*.c
    )

    add_library(minhook_static STATIC ${minhook_sources})
    target_include_directories(minhook_static PUBLIC ${minhook_SOURCE_DIR}/include)
    target_compile_definitions(minhook_static PUBLIC UNICODE _UNICODE)
  else()
    message(FATAL_ERROR "HKLM_WRAPPER_FETCH_MINHOOK=OFF is not supported yet (provide MinHook manually).")
  endif()

  add_library(hklm_shim SHARED
    src/shim/dllmain.cpp
    src/shim/d3d9_surface_scaler.cpp
    src/shim/d3d9_surface_scaler.h
    src/shim/ddraw_surface_scaler.cpp
    src/shim/ddraw_surface_scaler.h
    src/shim/surface_scale_config.cpp
    src/shim/surface_scale_config.h
    src/shim/minhook_runtime.cpp
    src/shim/minhook_runtime.h
    src/shim/registry_hooks.cpp
    src/shim/registry_hooks.h
    src/shim/registry_hooks_hooks_core.inl
    src/shim/registry_hooks_hooks_legacy.inl
    src/shim/registry_hooks_hooks_ansi.inl
    src/shim/registry_hooks_trace.cpp
    src/shim/registry_hooks_trace.h
    src/shim/registry_hooks_utils.cpp
    src/shim/registry_hooks_utils.h
  )
  set_target_properties(hklm_shim PROPERTIES PREFIX "" OUTPUT_NAME "twinshim_shim")

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(hklm_shim PRIVATE -fpermissive)
  endif()

  target_link_libraries(hklm_shim PRIVATE hklm_common minhook_static dxguid shell32)
  target_compile_definitions(hklm_shim PRIVATE UNICODE _UNICODE NOMINMAX)

  if(HKLM_WRAPPER_ENABLE_DGVOODOO_ADDON)
    if(NOT MSVC)
      message(FATAL_ERROR
        "HKLM_WRAPPER_ENABLE_DGVOODOO_ADDON=ON currently requires MSVC because the dgVoodoo SDK ships MSVC .lib files (dgVoodooAddon.lib).\n"
        "Build the add-on on Windows with MSVC, or provide a MinGW-compatible import library.")
    endif()

    set(DGVOODOO_ADDON_SDK_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/dgvoodoo_addon_sdk" CACHE PATH "Path to dgVoodoo AddOn SDK root")
    set(DGVOODOO_ADDON_INC_DIR "${DGVOODOO_ADDON_SDK_DIR}/Inc" CACHE PATH "Path to dgVoodoo AddOn SDK 'Inc' directory")
    set(DGVOODOO_ADDON_LIB_DIR "${DGVOODOO_ADDON_SDK_DIR}/Lib/x86" CACHE PATH "Path to dgVoodoo AddOn SDK lib directory (e.g. Lib/x86)")

    if(NOT EXISTS "${DGVOODOO_ADDON_INC_DIR}/Addon/AddonDefs.hpp")
      message(FATAL_ERROR
        "dgVoodoo AddOn SDK headers not found under: ${DGVOODOO_ADDON_INC_DIR}/Addon/AddonDefs.hpp\n"
        "Expected SDK layout: <sdk>/Inc/Addon/*.hpp")
    endif()
    if(NOT EXISTS "${DGVOODOO_ADDON_LIB_DIR}/dgVoodooAddon.lib")
      message(FATAL_ERROR
        "dgVoodooAddon.lib not found under: ${DGVOODOO_ADDON_LIB_DIR}/dgVoodooAddon.lib\n"
        "Set -DDGVOODOO_ADDON_LIB_DIR=<path> to match your SDK/arch.")
    endif()

    add_library(hklm_dgvoodoo_addon SHARED
      src/dgvoodoo_addon/addon_main.cpp
      src/shim/surface_scale_config.cpp
      src/shim/surface_scale_config.h
    )
    set_target_properties(hklm_dgvoodoo_addon PROPERTIES PREFIX "" OUTPUT_NAME "SampleAddon")
    target_include_directories(hklm_dgvoodoo_addon PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
      ${DGVOODOO_ADDON_INC_DIR}
    )
    target_link_libraries(hklm_dgvoodoo_addon PRIVATE shell32 "${DGVOODOO_ADDON_LIB_DIR}/dgVoodooAddon.lib")
    target_compile_definitions(hklm_dgvoodoo_addon PRIVATE
      UNICODE
      _UNICODE
      NOMINMAX
    )

    install(TARGETS hklm_dgvoodoo_addon RUNTIME DESTINATION bin LIBRARY DESTINATION bin)
  endif()

  add_executable(hklm_wrapper WIN32
    src/wrapper/main.cpp
    src/wrapper/process_inject.cpp
    src/wrapper/process_inject.h
  )
  set_target_properties(hklm_wrapper PROPERTIES OUTPUT_NAME "twinshim")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hklm_wrapper PRIVATE -municode)
  endif()
  target_link_libraries(hklm_wrapper PRIVATE hklm_common shell32)
  target_compile_definitions(hklm_wrapper PRIVATE UNICODE _UNICODE NOMINMAX)

  add_executable(hklm_wrapper_cli
    src/wrapper/main.cpp
    src/wrapper/process_inject.cpp
    src/wrapper/process_inject.h
  )
  set_target_properties(hklm_wrapper_cli PROPERTIES OUTPUT_NAME "twinshim_cli")
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hklm_wrapper_cli PRIVATE -municode)
  endif()
  target_link_libraries(hklm_wrapper_cli PRIVATE hklm_common shell32)
  target_compile_definitions(hklm_wrapper_cli PRIVATE UNICODE _UNICODE NOMINMAX HKLM_WRAPPER_CONSOLE_APP)

  add_executable(hklmreg
    src/hklmreg/main.cpp
    src/hklmreg/reg_file.cpp
    src/hklmreg/reg_file.h
  )
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hklmreg PRIVATE -municode)
  endif()
  target_link_libraries(hklmreg PRIVATE hklm_common)
  target_compile_definitions(hklmreg PRIVATE UNICODE _UNICODE NOMINMAX)

  install(TARGETS
    hklm_wrapper
    hklm_wrapper_cli
    hklm_shim
    hklmreg
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin
  )
endif()

if(HKLM_WRAPPER_ENABLE_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()
