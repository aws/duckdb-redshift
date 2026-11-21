# Offline C++ tests in test/client/interface/, run by make test_client, and the test-only by-path
# source and stub libraries they need, which no shipped build links. Add files here only.
list(APPEND REDSHIFT_CLIENT_TEST_SOURCES
     test/client/interface/header_standalone_test.cpp
     test/client/interface/header_then_duckdb_test.cpp
     test/client/interface/header_unicode_test.cpp
     test/client/interface/odbc_driver_test.cpp
     test/client/interface/refused_environment_test.cpp
     test/client/interface/scoped_environment_variable_test.cpp
     test/client/interface/windows_undefs_test.cpp)
# Only a build without a driver is sure to link the NOT_BUILT placeholder; an embedded build links
# its platform's loader where the platform has one.
if(REDSHIFT_DRIVER STREQUAL "none")
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/interface/driver_not_built_test.cpp)
endif()
# The header's copy of DuckDB's Win32 macro list is read as text, so Linux checks it too.
set(redshift_undefs_test_definitions
    "REDSHIFT_INTERFACE_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/src/transport/odbc_driver.hpp\""
    "REDSHIFT_DUCKDB_WINDOWS_UNDEFS=\"${CMAKE_SOURCE_DIR}/src/include/duckdb/common/windows_undefs.hpp\"")
set_source_files_properties(test/client/interface/windows_undefs_test.cpp PROPERTIES COMPILE_DEFINITIONS
                                                                                     "${redshift_undefs_test_definitions}")

# The by-path source loads with dlopen and checks POSIX ownership, so Windows builds neither it, its
# stub libraries nor its tests. An MSVC build adds its own case instead, which reads the loadable
# extension's import table. Each platform's lines sit in its own branch; a line after endif() is
# built everywhere.
if(WIN32)
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/interface/windows_header_test.cpp)
  # dumpbin comes with MSVC, so the case is left out of other Windows toolchains. CMake looks for it
  # only beside the compiler, so it is that toolchain's, and an empty path makes the case skip.
  if(MSVC)
    get_filename_component(redshift_msvc_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_program(REDSHIFT_DUMPBIN dumpbin HINTS "${redshift_msvc_bin}" NO_DEFAULT_PATH)
    if(REDSHIFT_DUMPBIN)
      set(redshift_dumpbin "${REDSHIFT_DUMPBIN}")
    else()
      set(redshift_dumpbin "")
    endif()
    list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/interface/windows_imports_test.cpp)
    set(redshift_imports_test_definitions
        "REDSHIFT_LOADABLE_EXTENSION=\"$<TARGET_FILE:${LOADABLE_EXTENSION_NAME}>\""
        "REDSHIFT_DUMPBIN=\"${redshift_dumpbin}\"")
    set_source_files_properties(test/client/interface/windows_imports_test.cpp
                                PROPERTIES COMPILE_DEFINITIONS "${redshift_imports_test_definitions}")
    list(APPEND REDSHIFT_CLIENT_TEST_DEPENDENCIES ${LOADABLE_EXTENSION_NAME})
  endif()
else()
  # Runs the build's compiler on the header with _WIN32 alone, to show the 32-bit #error fires.
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/interface/windows_32bit_error_test.cpp)
  set(redshift_32bit_test_definitions
      "REDSHIFT_CXX_COMPILER=\"${CMAKE_CXX_COMPILER}\""
      "REDSHIFT_UNIXODBC_INCLUDE=\"${CMAKE_CURRENT_SOURCE_DIR}/third_party/unixodbc\""
      "REDSHIFT_INTERFACE_HEADER=\"${CMAKE_CURRENT_SOURCE_DIR}/src/transport/odbc_driver.hpp\"")
  set_source_files_properties(test/client/interface/windows_32bit_error_test.cpp
                              PROPERTIES COMPILE_DEFINITIONS "${redshift_32bit_test_definitions}")

  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/interface/by_path_driver.cpp
       test/client/interface/by_path_driver_test.cpp)

  # The stub libraries the by-path source's tests load (by_path_stub_driver.c names each one's role).
  # EXCLUDE_FROM_ALL, as the test suites are, so a plain `make` builds no test artefact. The tests
  # name each by absolute path, and the loadable extension too, which must carry none of the source.
  add_library(redshift_by_path_stub_driver MODULE EXCLUDE_FROM_ALL test/client/interface/by_path_stub_driver.c)
  add_library(redshift_by_path_partial_stub_driver MODULE EXCLUDE_FROM_ALL test/client/interface/by_path_stub_driver.c)
  target_compile_definitions(redshift_by_path_partial_stub_driver PRIVATE REDSHIFT_STUB_WITHOUT_ALLOC_HANDLE)
  set(redshift_by_path_test_artefacts redshift_by_path_stub_driver redshift_by_path_partial_stub_driver)
  set(redshift_by_path_test_definitions
      "REDSHIFT_TEST_STUB_DRIVER=\"$<TARGET_FILE:redshift_by_path_stub_driver>\""
      "REDSHIFT_TEST_PARTIAL_STUB_DRIVER=\"$<TARGET_FILE:redshift_by_path_partial_stub_driver>\""
      "REDSHIFT_LOADABLE_EXTENSION=\"$<TARGET_FILE:${LOADABLE_EXTENSION_NAME}>\"")
  # Built on Linux only, as the cases that load them are.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_library(redshift_by_path_unresolved_stub_driver MODULE EXCLUDE_FROM_ALL
                test/client/interface/by_path_stub_driver.c)
    target_compile_definitions(redshift_by_path_unresolved_stub_driver PRIVATE REDSHIFT_STUB_WITH_UNRESOLVED_SYMBOL)
    add_library(redshift_by_path_interposer MODULE EXCLUDE_FROM_ALL test/client/interface/by_path_stub_driver.c)
    target_compile_definitions(redshift_by_path_interposer PRIVATE REDSHIFT_STUB_INTERPOSER)
    # A stub whose dependency, found beside it through $ORIGIN, fails to load.
    add_library(redshift_by_path_failing_dependency SHARED EXCLUDE_FROM_ALL test/client/interface/by_path_stub_driver.c)
    target_compile_definitions(redshift_by_path_failing_dependency PRIVATE REDSHIFT_STUB_WITH_UNRESOLVED_SYMBOL)
    add_library(redshift_by_path_dependent_stub_driver MODULE EXCLUDE_FROM_ALL test/client/interface/by_path_stub_driver.c)
    target_compile_definitions(redshift_by_path_dependent_stub_driver PRIVATE REDSHIFT_STUB_WITH_FAILING_DEPENDENCY)
    target_link_libraries(redshift_by_path_dependent_stub_driver redshift_by_path_failing_dependency)
    set_target_properties(redshift_by_path_dependent_stub_driver PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH
                                                                            "\$ORIGIN")
    list(APPEND redshift_by_path_test_artefacts redshift_by_path_unresolved_stub_driver redshift_by_path_interposer
         redshift_by_path_dependent_stub_driver)
    list(APPEND redshift_by_path_test_definitions
         "REDSHIFT_TEST_UNRESOLVED_STUB_DRIVER=\"$<TARGET_FILE:redshift_by_path_unresolved_stub_driver>\""
         "REDSHIFT_TEST_INTERPOSER=\"$<TARGET_FILE:redshift_by_path_interposer>\""
         "REDSHIFT_TEST_DEPENDENT_STUB_DRIVER=\"$<TARGET_FILE:redshift_by_path_dependent_stub_driver>\""
         "REDSHIFT_TEST_FAILING_DEPENDENCY=\"$<TARGET_FILE:redshift_by_path_failing_dependency>\"")
  endif()
  set_source_files_properties(test/client/interface/by_path_driver_test.cpp PROPERTIES COMPILE_DEFINITIONS
                                                                                     "${redshift_by_path_test_definitions}")
  # Built before the suite, which `make test_client` builds by name, so no case reads an artefact
  # nothing had built.
  list(APPEND REDSHIFT_CLIENT_TEST_DEPENDENCIES ${redshift_by_path_test_artefacts} ${LOADABLE_EXTENSION_NAME})
endif()
