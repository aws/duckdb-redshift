cmake_minimum_required(VERSION 3.5)

# Set extension name here
set(TARGET_NAME redshift)

set(EXTENSION_NAME ${TARGET_NAME}_extension)
set(LOADABLE_EXTENSION_NAME ${TARGET_NAME}_loadable_extension)

project(${TARGET_NAME})

set(CMAKE_CXX_STANDARD
    "17"
    CACHE STRING "C++ standard to enforce")
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(src/include)

# Which client is compiled in. The client is chosen by source file, not by preprocessor
# macro: REDSHIFT_CLIENT_SOURCES holds the files below src/include/redshift_transport.hpp
# for the selected client. The mock client is the default. Every test target runs with
# REDSHIFT_TEST_CLIENT set to the client its build
# was configured with, and every .test file starts with a require-env line naming the
# client it is written for, so `unittest "test/*"` runs only the tests for the client that
# was built.
set(REDSHIFT_CLIENT
    "mock"
    CACHE STRING "Client to build into the extension: mock or odbc")
set_property(CACHE REDSHIFT_CLIENT PROPERTY STRINGS mock odbc)

# Which driver an ODBC build carries. embedded: the official release, fetched, embedded and
# loaded by the driver loader. none: no driver, no fetch or embed step and no network access, for
# the offline client tests (make test_client). A mock build carries no driver either way.
set(REDSHIFT_DRIVER
    "embedded"
    CACHE STRING "Driver for REDSHIFT_CLIENT=odbc: embedded or none")
set_property(CACHE REDSHIFT_DRIVER PROPERTY STRINGS embedded none)
if(NOT REDSHIFT_DRIVER MATCHES "^(embedded|none)$")
  message(FATAL_ERROR "REDSHIFT_DRIVER must be 'embedded' or 'none', got '${REDSHIFT_DRIVER}'")
endif()

# Preview implementations. A neutral hook has a refusing default in its layer's own directory and
# may have a preview implementation, with its tests, in a preview directory, which the public tree
# does not carry. Exactly one of the two is
# compiled, chosen at configure time by whether the preview directory exists, with no setting;
# nothing selects between them at run time. Each hook is chosen on its own. The Makefile's test_mock
# and test_client build the public shape from a copy of a tree without the preview directories.

# redshift_select_hook(<list> DEFAULT <file> PREVIEW <file>) appends the file this build compiles
# for one hook to <list>. The pair is named
# explicitly rather than found by file name: where the preview file's directory exists, the file
# must too, so a misnamed or missing preview fails the configure instead of quietly compiling the
# refusing default. Where the directory is absent, as in the public tree, the default is
# compiled.
function(redshift_select_hook list_var)
  cmake_parse_arguments(HOOK "" "DEFAULT;PREVIEW" "" ${ARGN})
  if(NOT HOOK_DEFAULT
     OR NOT HOOK_PREVIEW
     OR HOOK_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "redshift_select_hook(<list> DEFAULT <file> PREVIEW <file>), got: ${ARGN}")
  endif()
  get_filename_component(preview_dir "${HOOK_PREVIEW}" DIRECTORY)
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/${preview_dir}")
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${HOOK_PREVIEW}")
      message(FATAL_ERROR "redshift: ${preview_dir}/ exists but has no ${HOOK_PREVIEW}, the preview "
                          "implementation paired with ${HOOK_DEFAULT}")
    endif()
    set(file ${HOOK_PREVIEW})
  else()
    set(file ${HOOK_DEFAULT})
  endif()
  set(${list_var}
      ${${list_var}} ${file}
      PARENT_SCOPE)
endfunction()

# Preview test sources join the suite of the part they test, the mock suite or the client suite,
# each area listing its files in its own sources.cmake, where the tree carries them.
macro(redshift_include_preview_tests area)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/test/preview/${area}/sources.cmake")
    include(test/preview/${area}/sources.cmake)
  endif()
endmacro()
# Preview implementations include what they share, such as redshift_preview_target.hpp, from
# their own directory. Only the mock suite's preview tests include it from elsewhere, so only that
# suite has the preview directory on its include path, and only in a tree that carries it: the two shapes
# differ in no other source's compile flags.
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/src/preview")
  set(PREVIEW_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src/preview)
  message(STATUS "redshift: src/preview/ present, compiling its preview implementations")
else()
  set(PREVIEW_INCLUDE_DIR)
  message(STATUS "redshift: no src/preview/, compiling every hook's default")
endif()

# Each area lists its own files in its own sources.cmake, so a change adds source files only in
# its own directory and never edits this file for that.
if(REDSHIFT_CLIENT STREQUAL "mock")
  set(REDSHIFT_CLIENT_SOURCES src/transport/mock_transport.cpp)
  # The extension's test-only pause points, for its C++ race tests
  # (storage/redshift_connection_ownership.hpp). Only in mock builds, whose artifacts are never
  # published.
  add_definitions(-DREDSHIFT_TEST_HOOKS)
  message(STATUS "redshift: client=mock")
elseif(REDSHIFT_CLIENT STREQUAL "odbc")
  include(src/transport/odbc/sources.cmake)
  include(src/transport/driver/sources.cmake)
  set(REDSHIFT_CLIENT_SOURCES ${REDSHIFT_ODBC_SOURCES} ${REDSHIFT_DRIVER_SOURCES})
  # The ODBC declarations come from the vendored, unmodified unixODBC headers, never from an
  # ODBC installation on the build machine.
  include_directories(third_party/unixodbc)
  # The client's sources and tests include the driver interface by name, as "odbc_driver.hpp", so an
  # include does not depend on how deep the including file sits.
  include_directories(src/transport)
  message(STATUS "redshift: client=odbc driver=${REDSHIFT_DRIVER}")
else()
  message(FATAL_ERROR "REDSHIFT_CLIENT must be 'mock' or 'odbc', got '${REDSHIFT_CLIENT}'")
endif()

# Every source file is listed explicitly, here or in its area's sources.cmake; there is no
# automatic discovery.
#
# The split mirrors the interface boundary declared in src/include/redshift_transport.hpp:
# files under src/transport/ are the client below the interface, everything
# else is DuckDB-facing.
set(EXTENSION_SOURCES
    src/redshift_extension.cpp
    src/redshift_attach.cpp
    src/redshift_attach_aws_forms.cpp
    src/redshift_attach_path.cpp
    src/redshift_aws_secret.cpp
    src/redshift_conversion.cpp
    src/redshift_log_sink.cpp
    src/redshift_query.cpp
    src/redshift_config_options.cpp
    src/storage/redshift_catalog.cpp
    src/storage/redshift_client_state.cpp
    src/storage/redshift_connection_calls.cpp
    src/storage/redshift_connection_ownership.cpp
    src/storage/redshift_transaction.cpp
    ${REDSHIFT_CLIENT_SOURCES})
# The target hook: whether a WORKGROUP without ACCOUNT_ID is accepted (redshift_target_hook.hpp).
redshift_select_hook(EXTENSION_SOURCES DEFAULT src/redshift_target_hook_default.cpp PREVIEW
                     src/preview/redshift_target_hook_preview.cpp)

# Both targets are built from the same source list so the two cannot drift apart:
# the static form links into a DuckDB binary for local testing, the loadable form is
# the shippable redshift.duckdb_extension. Both macros come from DuckDB's build
# system, which is why the DuckDB tree in duckdb/ (make duckdb) is required to configure this at all.
build_static_extension(${TARGET_NAME} ${EXTENSION_SOURCES})
build_loadable_extension(${TARGET_NAME} " " ${EXTENSION_SOURCES})

# C++ tests, in the Catch2 copy bundled with the pinned DuckDB.
# EXCLUDE_FROM_ALL: built only on request (`cmake --build build/release --target
# redshift_mock_tests`), so the release build never compiles them. It links what DuckDB's own
# unittest links: duckdb_static plus every statically linked extension, this one included.
# test/catch_main.cpp is the one Catch2 main in the repository, shared with the client's suites,
# so it sits in no suite's own test directory.
set(REDSHIFT_MOCK_TEST_SOURCES
    test/catch_main.cpp
    test/mock/cpp/test_attach_config.cpp
    test/mock/cpp/test_attach_handle.cpp
    test/mock/cpp/test_connect.cpp
    test/mock/cpp/test_connect_prompt.cpp
    test/mock/cpp/test_conversion_types.cpp
    test/mock/cpp/test_conversion_values.cpp
    test/mock/cpp/test_detach.cpp
    test/mock/cpp/test_fence.cpp
    test/mock/cpp/test_log_sink.cpp
    test/mock/cpp/test_mock_transport.cpp
    test/mock/cpp/test_preview_selection.cpp
    test/mock/cpp/test_sessions.cpp
    test/mock/cpp/test_transaction.cpp
    test/mock/cpp/test_transport_failures.cpp)
# A test-only hook that shows the preview selection compiles the file it should, in both shapes.
redshift_select_hook(REDSHIFT_MOCK_TEST_SOURCES DEFAULT test/mock/cpp/preview_probe_default.cpp PREVIEW
                     test/preview/mock/preview_probe.cpp)
# What the probe should report, worked out apart from redshift_select_hook: the preview where the
# probe's preview directory exists, otherwise the default.
if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/test/preview/mock")
  set(REDSHIFT_PROBE_EXPECTED preview)
else()
  set(REDSHIFT_PROBE_EXPECTED default)
endif()
redshift_include_preview_tests(mock)
add_executable(redshift_mock_tests EXCLUDE_FROM_ALL ${REDSHIFT_MOCK_TEST_SOURCES})
target_compile_definitions(redshift_mock_tests PRIVATE REDSHIFT_PROBE_EXPECTED="${REDSHIFT_PROBE_EXPECTED}")
# The version DuckDB gives this extension, from DuckDB's own variable (empty for a tree outside
# git), so the version test compares Version() with it without naming the macro
# redshift_extension.cpp reads.
string(TOUPPER ${TARGET_NAME} REDSHIFT_NAME_UPPERCASE)
target_compile_definitions(
  redshift_mock_tests PRIVATE REDSHIFT_EXPECTED_VERSION="${DUCKDB_EXTENSION_${REDSHIFT_NAME_UPPERCASE}_EXT_VERSION}")
# src/transport for the mock's test controls, included by name as "mock_transport.hpp", and
# test/mock/cpp for the headers its preview counterparts share.
target_include_directories(
  redshift_mock_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/catch ${CMAKE_CURRENT_SOURCE_DIR}/src/transport
                              ${CMAKE_CURRENT_SOURCE_DIR}/test/mock/cpp ${PREVIEW_INCLUDE_DIR})
# The CLI's own code, for test_connect_prompt.cpp's pin of the query its prompt sends while
# CONNECT-ed. Named plainly: DuckDB defines duckdb_shell (BUILD_SHELL, on in every build this
# repository makes) after this file is read, and CMake resolves a link name once every target exists,
# so a build without the shell fails to link the suite rather than pass it. The file gets the
# definition DuckDB compiles the shell with, so ShellState's inline members, rl_version's default
# among them, match the library's.
set_source_files_properties(test/mock/cpp/test_connect_prompt.cpp PROPERTIES COMPILE_DEFINITIONS HAVE_LINENOISE=1)
target_include_directories(redshift_mock_tests PRIVATE ${CMAKE_SOURCE_DIR}/tools/shell/include)
target_link_libraries(redshift_mock_tests duckdb_shell)
if(NOT AMALGAMATION_BUILD AND NOT WIN32)
  # As DuckDB's shell executable links it.
  target_link_libraries(redshift_mock_tests duckdb_utf8proc)
endif()
target_link_libraries(redshift_mock_tests duckdb_static)
link_extension_libraries(redshift_mock_tests "")
# MSVC's C4717 as an error here too, as for the client and driver suites below.
if(MSVC)
  target_compile_options(redshift_mock_tests PRIVATE /we4717)
endif()

# No third-party package to find_package() or link: DuckDB comes from duckdb/, and the driver
# through src/transport/driver/sources.cmake.

install(
  TARGETS ${EXTENSION_NAME}
  EXPORT "${DUCKDB_EXPORT_SET}"
  LIBRARY DESTINATION "${INSTALL_LIB_DIR}"
  ARCHIVE DESTINATION "${INSTALL_LIB_DIR}")

# The client's offline C++ tests, in the Catch2 copy bundled with the pinned DuckDB. Each test area
# lists its files in its own sources.cmake, and all of them share the repository's one Catch2 main.
#
# Declared as the extension's suite above, so the repository has one C++ test convention, and behind
# DuckDB's own gate for a test target as well:
#   - EXCLUDE_FROM_ALL, because the pinned distribution build builds ALL and must not compile
#     or link a test executable;
#   - DuckDB's unittest link form: duckdb_static plus link_extension_libraries, which already
#     links this extension and DuckDB's generated loader for the statically linked extensions;
#   - DuckDB's gate for its own test tree (duckdb/CMakeLists.txt:1437-1438). BUILD_UNITTESTS is
#     already FALSE in a build that does not build the main library, which is what the pinned
#     wasm recipes configure (-DBUILD_EXTENSIONS_ONLY=1), so this covers the EMSCRIPTEN case
#     it replaces, and it keeps the suites out of a CLANG_TIDY configure.
#
# The suite targets (test_mock, test_client, test_driver) build each suite by name before running it, so an area whose sources.cmake
# is empty gets an executable with no cases, and an area that loses its target fails its
# make target rather than passing as an area without tests.
if(REDSHIFT_CLIENT STREQUAL "odbc"
   AND BUILD_UNITTESTS
   AND NOT CLANG_TIDY)
  set(REDSHIFT_TEST_SUITES client)
  set(REDSHIFT_CLIENT_TEST_SOURCES)
  set(REDSHIFT_CLIENT_TEST_DEPENDENCIES)
  include(test/client/interface/sources.cmake)
  include(test/client/fake_driver/sources.cmake)
  include(test/client/odbc/sources.cmake)
  # The read-path benchmark's file, where the tree carries the benchmark. Keyed on the directory, as
  # test_graph_check is, so a tree that has it but has lost its sources.cmake fails the configure.
  if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/test/client/perf")
    include(test/client/perf/sources.cmake)
  endif()
  redshift_include_preview_tests(client)
  # The driver loader's tests link its loader, which only the embedded build compiles; on Windows the suite
  # compiles the Windows loader's sources only where the extension does not link them
  # (REDSHIFT_DRIVER=none; test/client/driver/sources.cmake).
  if(REDSHIFT_DRIVER STREQUAL "embedded" OR WIN32)
    list(APPEND REDSHIFT_TEST_SUITES driver)
    set(REDSHIFT_DRIVER_TEST_SOURCES)
    set(REDSHIFT_DRIVER_TEST_DEPENDENCIES)
    include(test/client/driver/sources.cmake)
  endif()
  foreach(suite ${REDSHIFT_TEST_SUITES})
    string(TOUPPER "${suite}" suite_upper)
    add_executable(redshift_${suite}_tests EXCLUDE_FROM_ALL test/catch_main.cpp ${REDSHIFT_${suite_upper}_TEST_SOURCES})
    target_include_directories(redshift_${suite}_tests PRIVATE ${CMAKE_SOURCE_DIR}/third_party/catch)
    # MSVC's warning C4717, a function recursive on all its paths, as an error: optimised, such a
    # function loops for ever, so a case that reaches one (Catch2's GENERATE given a named value)
    # would hang the suite with no output rather than fail its build.
    if(MSVC)
      target_compile_options(redshift_${suite}_tests PRIVATE /we4717)
    endif()
    target_link_libraries(redshift_${suite}_tests duckdb_static)
    link_extension_libraries(redshift_${suite}_tests "")
    # The targets an area's sources.cmake lists for its tests to read, such as the interface
    # area's stub libraries, built first because the suite targets build each suite by name.
    if(REDSHIFT_${suite_upper}_TEST_DEPENDENCIES)
      add_dependencies(redshift_${suite}_tests ${REDSHIFT_${suite_upper}_TEST_DEPENDENCIES})
    endif()
  endforeach()
endif()
