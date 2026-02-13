include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.7.1
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(hklm_common_tests
  test_arg_quote.cpp
  test_path_util.cpp
  test_utf8.cpp
  ../src/common/arg_quote.cpp
  ../src/common/path_util.cpp
  ../src/common/utf8.cpp
)

target_include_directories(hklm_common_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../src
)

target_link_libraries(hklm_common_tests PRIVATE Catch2::Catch2WithMain)

if(WIN32)
  target_compile_definitions(hklm_common_tests PRIVATE UNICODE _UNICODE NOMINMAX)
endif()

include(Catch)
catch_discover_tests(hklm_common_tests DISCOVERY_MODE PRE_TEST)

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

if(NOT _sqlite_target STREQUAL "")
  add_executable(hklm_store_tests
    test_local_registry_store.cpp
    ../src/common/local_registry_store.cpp
    ../src/common/utf8.cpp
  )

  target_include_directories(hklm_store_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )

  target_link_libraries(hklm_store_tests PRIVATE Catch2::Catch2WithMain ${_sqlite_target})

  if(WIN32)
    target_compile_definitions(hklm_store_tests PRIVATE UNICODE _UNICODE NOMINMAX)
  endif()

  catch_discover_tests(hklm_store_tests DISCOVERY_MODE PRE_TEST)
else()
  message(STATUS "SQLite3 not found: skipping hklm_store_tests")
endif()

if(WIN32)
  add_executable(hklm_workflow_probe
    workflow_probe_main.cpp
  )

  target_include_directories(hklm_workflow_probe PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )

  target_link_libraries(hklm_workflow_probe PRIVATE advapi32)
  target_compile_definitions(hklm_workflow_probe PRIVATE UNICODE _UNICODE NOMINMAX)

  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_link_options(hklm_workflow_probe PRIVATE -municode)
  endif()

  add_executable(hklm_shim_utils_tests
    test_registry_hooks_utils.cpp
    ../src/shim/registry_hooks_utils.cpp
  )

  target_include_directories(hklm_shim_utils_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/..
    ${CMAKE_CURRENT_SOURCE_DIR}/../src
  )

  target_link_libraries(hklm_shim_utils_tests PRIVATE Catch2::Catch2WithMain)
  target_compile_definitions(hklm_shim_utils_tests PRIVATE UNICODE _UNICODE NOMINMAX)

  catch_discover_tests(hklm_shim_utils_tests DISCOVERY_MODE PRE_TEST)

  if(NOT _sqlite_target STREQUAL "")
    add_executable(hklm_shim_integration_tests
      test_registry_hooks_integration.cpp
      ../src/common/local_registry_store.cpp
      ../src/common/utf8.cpp
      ../src/shim/registry_hooks_utils.cpp
    )

    target_include_directories(hklm_shim_integration_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/..
      ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_link_libraries(hklm_shim_integration_tests PRIVATE Catch2::Catch2WithMain ${_sqlite_target})
    target_compile_definitions(hklm_shim_integration_tests PRIVATE UNICODE _UNICODE NOMINMAX)

    catch_discover_tests(hklm_shim_integration_tests DISCOVERY_MODE PRE_TEST)

    add_executable(hklm_shim_workflow_tests
      test_wrapper_workflow.cpp
      ../src/common/arg_quote.cpp
      ../src/common/local_registry_store.cpp
      ../src/common/path_util.cpp
      ../src/common/utf8.cpp
    )

    target_include_directories(hklm_shim_workflow_tests PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/..
      ${CMAKE_CURRENT_SOURCE_DIR}/../src
    )

    target_link_libraries(hklm_shim_workflow_tests PRIVATE Catch2::Catch2WithMain ${_sqlite_target} advapi32)
    target_compile_definitions(hklm_shim_workflow_tests PRIVATE UNICODE _UNICODE NOMINMAX)

    catch_discover_tests(hklm_shim_workflow_tests DISCOVERY_MODE PRE_TEST)
  else()
    message(STATUS "SQLite3 not found: skipping hklm_shim_integration_tests and hklm_shim_workflow_tests")
  endif()
endif()
