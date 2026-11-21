# The driver's C++ tests in test/client/driver/, run by make test_driver. Add files here only.
# The loader's tests, where the loader is built (Linux or macOS); they reach its internal header in
# src/transport/driver/. Windows builds its loader's tests into this suite, and its sources too when
# the extension does not link them (below).
if(REDSHIFT_DRIVER_LOADER STREQUAL "linux" OR REDSHIFT_DRIVER_LOADER STREQUAL "macos")
  # The Windows loader's platform-neutral rules run here too, so Linux and macOS check them.
  set(redshift_loader_tests
      test/client/driver/sha256_test.cpp test/client/driver/driver_loader_test.cpp
      test/client/driver/driver_environment_test.cpp test/client/driver/driver_windows_rules_test.cpp
      src/transport/driver/driver_windows_rules.cpp)
  # The suites that are given the test inputs' paths and the policy directory.
  set(redshift_input_tests driver_loader_test driver_environment_test)
  # The decoy logs every call it gets; on Linux the isolation test preloads it.
  set(redshift_input_variants complete missing_entry decoy)
  if(REDSHIFT_DRIVER_LOADER STREQUAL "linux")
    list(APPEND redshift_loader_tests
         test/client/driver/driver_directory_test.cpp test/client/driver/driver_root_test.cpp
         test/client/driver/driver_install_test.cpp test/client/driver/driver_search_list_test.cpp
         test/client/driver/driver_isolation_test.cpp test/client/driver/driver_coexistence_test.cpp
         test/client/driver/driver_config_files_test.cpp)
    list(APPEND redshift_input_tests driver_search_list_test driver_isolation_test driver_coexistence_test
         driver_config_files_test)
    list(APPEND redshift_input_variants needs_absent)
  else()
    # On macOS, the suites whose cases hold there, their Linux-only cases left out by #if (descriptor names
    # in /proc, the library cache and search list, user namespaces), and the macOS cases
    # (driver_macos_test.cpp). driver_directory_test.cpp, driver_root_test.cpp and driver_install_test.cpp
    # stay Linux-only: they stand in for the C library by symbol (mkdir, chmod, and the install's openat,
    # renameat, fstatat, read, write, fsync and others) and use user namespaces and /proc throughout;
    # driver_macos_test.cpp covers the common code's rules on macOS instead.
    list(APPEND redshift_loader_tests test/client/driver/driver_macos_test.cpp)
    list(APPEND redshift_input_tests driver_macos_test)
  endif()
  # driver_loader_test.cpp replaces the suite's global operator new (one that can fail once, in a child).
  list(APPEND REDSHIFT_DRIVER_TEST_SOURCES ${redshift_loader_tests})
  set_source_files_properties(${redshift_loader_tests} PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/driver;${REDSHIFT_DRIVER_GEN_DIR}")

  # The ODBC client's connection allocating its environment through the loader, against the client's fake
  # driver and test-only source setter, which this suite compiles too.
  list(APPEND REDSHIFT_DRIVER_TEST_SOURCES test/client/driver/environment_allocation_test.cpp
       test/client/fake_driver/fake_odbc_driver.cpp test/client/odbc/driver_source_hook.cpp)
  # It reaches the loader's refusal sentence, to compare it with the client's.
  set_source_files_properties(test/client/driver/environment_allocation_test.cpp PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/driver")

  # Test-input drivers for the loader tests, built only with the driver suite: every entry point; all but
  # SQLGetDiagField; the decoy; and, on Linux, one that needs a library the tests never install.
  set(redshift_inputs "${CMAKE_CURRENT_SOURCE_DIR}/test/client/driver/inputs")
  foreach(variant ${redshift_input_variants})
    add_library(redshift_test_driver_${variant} SHARED EXCLUDE_FROM_ALL "${redshift_inputs}/fake_driver.c")
    set_target_properties(redshift_test_driver_${variant} PROPERTIES C_VISIBILITY_PRESET default
      SKIP_BUILD_RPATH ON LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_inputs")
  endforeach()
  target_compile_definitions(redshift_test_driver_missing_entry PRIVATE OMIT_GET_DIAG_FIELD)
  target_compile_definitions(redshift_test_driver_decoy PRIVATE RECORD_CALLS)
  # The test inputs are named relative to the test executable's own directory (loader_payload.hpp,
  # TestInputPath), where the build puts them, so a run that mounts only that directory finds them:
  # run-loader-tests-as-root.sh. The policy files are a real path, which that script mounts too.
  file(REAL_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/redshift_odbc/policy" redshift_policy_dir)
  foreach(loader_test ${redshift_input_tests})
    set_property(SOURCE test/client/driver/${loader_test}.cpp APPEND PROPERTY COMPILE_DEFINITIONS
      "REDSHIFT_TEST_DRIVER_COMPLETE=\"redshift_test_inputs/$<TARGET_FILE_NAME:redshift_test_driver_complete>\""
      "REDSHIFT_TEST_DRIVER_MISSING_ENTRY=\"redshift_test_inputs/$<TARGET_FILE_NAME:redshift_test_driver_missing_entry>\""
      "REDSHIFT_TEST_DRIVER_DECOY=\"redshift_test_inputs/$<TARGET_FILE_NAME:redshift_test_driver_decoy>\""
      "REDSHIFT_TEST_POLICY_DIR=\"${redshift_policy_dir}\"")
    set_property(SOURCE test/client/driver/${loader_test}.cpp APPEND PROPERTY OBJECT_DEPENDS
      "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h")
  endforeach()
  # Built before the suite (CMakeLists.txt adds REDSHIFT_DRIVER_TEST_DEPENDENCIES to it).
  list(APPEND REDSHIFT_DRIVER_TEST_DEPENDENCIES redshift_test_driver_complete redshift_test_driver_missing_entry
       redshift_test_driver_decoy)
endif()
if(REDSHIFT_DRIVER_LOADER STREQUAL "linux")
  # The needs_absent driver links a library the tests never install, without a build RPATH, so the platform
  # loader cannot find it unless a test puts it on LD_LIBRARY_PATH.
  add_library(redshift_test_absent SHARED EXCLUDE_FROM_ALL "${redshift_inputs}/absent.c")
  set_target_properties(redshift_test_absent PROPERTIES OUTPUT_NAME redshift_absent_test SOVERSION 1
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_absent")
  target_compile_definitions(redshift_test_driver_needs_absent PRIVATE NEEDS_ABSENT_LIBRARY)
  target_link_libraries(redshift_test_driver_needs_absent PRIVATE redshift_test_absent)
  # A library with a run path of its own (DT_RUNPATH), for the search-list case that reads an object's list.
  add_library(redshift_test_runpath_probe SHARED EXCLUDE_FROM_ALL "${redshift_inputs}/runpath_probe.c")
  set_target_properties(redshift_test_runpath_probe PROPERTIES OUTPUT_NAME redshift_runpath_probe_test
    SKIP_BUILD_RPATH ON LINK_FLAGS "-Wl,--enable-new-dtags,-rpath,/redshift-runpath-probe-dir"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_runpath")
  # A library with no soname, for the case that a name is loaded only as its soname or path is (glibc's rule).
  add_library(redshift_test_no_soname_probe SHARED EXCLUDE_FROM_ALL "${redshift_inputs}/runpath_probe.c")
  set_target_properties(redshift_test_no_soname_probe PROPERTIES OUTPUT_NAME redshift_no_soname_probe_test
    NO_SONAME ON SKIP_BUILD_RPATH ON LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_no_soname")
  # A library with the host prerequisite's file name and soname, for the case that plants one in a
  # directory the library cache names (run as root in a disposable container: run-loader-tests-as-root.sh).
  add_library(redshift_test_fake_keyutils SHARED EXCLUDE_FROM_ALL "${redshift_inputs}/absent.c")
  set_target_properties(redshift_test_fake_keyutils PROPERTIES OUTPUT_NAME keyutils SOVERSION 1
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_keyutils")
  foreach(loader_test ${redshift_input_tests})
    set_property(SOURCE test/client/driver/${loader_test}.cpp APPEND PROPERTY COMPILE_DEFINITIONS
      "REDSHIFT_TEST_DRIVER_NEEDS_ABSENT=\"redshift_test_inputs/$<TARGET_FILE_NAME:redshift_test_driver_needs_absent>\""
      "REDSHIFT_TEST_ABSENT_LIBRARY=\"redshift_test_absent/$<TARGET_FILE_NAME:redshift_test_absent>\""
      "REDSHIFT_TEST_RUNPATH_PROBE=\"redshift_test_runpath/$<TARGET_FILE_NAME:redshift_test_runpath_probe>\""
      "REDSHIFT_TEST_NO_SONAME_PROBE=\"redshift_test_no_soname/$<TARGET_FILE_NAME:redshift_test_no_soname_probe>\""
      "REDSHIFT_TEST_FAKE_KEYUTILS=\"redshift_test_keyutils/$<TARGET_FILE_NAME:redshift_test_fake_keyutils>\"")
  endforeach()
  list(APPEND REDSHIFT_DRIVER_TEST_DEPENDENCIES redshift_test_driver_needs_absent redshift_test_fake_keyutils
       redshift_test_runpath_probe redshift_test_no_soname_probe redshift_driver_probe)

  # For the coexistence suite's allocator rows: an allocator that replaces the C library's, which the loader
  # refuses, preloaded on every host, and the same replacing calloc or realloc alone; a non-PIE program that
  # takes malloc's and free's addresses, whose canonical PLT entries are not a replacement; and the same program
  # defining malloc itself, which is one. The programs are built without -fPIE, so that they have the entries.
  foreach(allocator allocator_interposer calloc_interposer realloc_interposer)
    add_library(redshift_test_${allocator} SHARED EXCLUDE_FROM_ALL "${redshift_inputs}/allocator_interposer.c")
    set_target_properties(redshift_test_${allocator} PROPERTIES SKIP_BUILD_RPATH ON
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_allocator")
  endforeach()
  # The same allocator replacing calloc alone, and realloc alone, with malloc the C library's.
  target_compile_definitions(redshift_test_calloc_interposer PRIVATE REDSHIFT_TEST_ONLY_CALLOC)
  target_compile_definitions(redshift_test_realloc_interposer PRIVATE REDSHIFT_TEST_ONLY_REALLOC)
  foreach(host non_pie_host own_malloc_host)
    add_executable(redshift_test_${host} EXCLUDE_FROM_ALL "${redshift_inputs}/non_pie_host.cpp"
                   src/transport/driver/allocator_replacement_linux.cpp)
    target_include_directories(redshift_test_${host} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/driver")
    set_target_properties(redshift_test_${host} PROPERTIES POSITION_INDEPENDENT_CODE OFF
      COMPILE_FLAGS "-fno-pie" LINK_FLAGS "-no-pie"
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_allocator")
    target_link_libraries(redshift_test_${host} PRIVATE ${CMAKE_DL_LIBS})
  endforeach()
  target_compile_definitions(redshift_test_own_malloc_host PRIVATE REDSHIFT_TEST_DEFINES_MALLOC)
  set_property(SOURCE test/client/driver/driver_coexistence_test.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    "REDSHIFT_TEST_ALLOCATOR_INTERPOSER=\"redshift_test_allocator/$<TARGET_FILE_NAME:redshift_test_allocator_interposer>\""
    "REDSHIFT_TEST_CALLOC_INTERPOSER=\"redshift_test_allocator/$<TARGET_FILE_NAME:redshift_test_calloc_interposer>\""
    "REDSHIFT_TEST_REALLOC_INTERPOSER=\"redshift_test_allocator/$<TARGET_FILE_NAME:redshift_test_realloc_interposer>\""
    "REDSHIFT_TEST_NON_PIE_HOST=\"redshift_test_allocator/$<TARGET_FILE_NAME:redshift_test_non_pie_host>\""
    "REDSHIFT_TEST_OWN_MALLOC_HOST=\"redshift_test_allocator/$<TARGET_FILE_NAME:redshift_test_own_malloc_host>\"")
  list(APPEND REDSHIFT_DRIVER_TEST_DEPENDENCIES redshift_test_allocator_interposer redshift_test_calloc_interposer
       redshift_test_realloc_interposer redshift_test_non_pie_host redshift_test_own_malloc_host)

  # The host probe: links the loader as the extension does, beside the suite, which
  # builds it; third_party/redshift_odbc/tools/host-probe.sh runs it.
  add_executable(redshift_driver_probe EXCLUDE_FROM_ALL test/client/driver/probe/driver_probe.cpp)
  target_include_directories(redshift_driver_probe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/driver"
                                                           "${CMAKE_CURRENT_SOURCE_DIR}/test/client/driver"
                                                           "${REDSHIFT_DRIVER_GEN_DIR}")
  set_source_files_properties(test/client/driver/probe/driver_probe.cpp PROPERTIES
    OBJECT_DEPENDS "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h")
  target_link_libraries(redshift_driver_probe duckdb_static)
  link_extension_libraries(redshift_driver_probe "")
endif()
# The Windows loader's tests, and its sources where the extension does not link them (a build without
# the embedded driver), so the suite holds one copy of each. It runs as a standard account, or
# elevated on a build host that offers only an administrator, where a case whose fixture an
# administrator's rights defeat says so in a warning.
if(WIN32)
  set(redshift_windows_tests
      test/client/driver/driver_windows_rules_test.cpp test/client/driver/driver_directory_windows_test.cpp
      test/client/driver/driver_install_windows_test.cpp test/client/driver/driver_loader_windows_test.cpp)
  if(NOT REDSHIFT_DRIVER_LOADER STREQUAL "windows")
    list(APPEND redshift_windows_tests
         src/transport/driver/driver_windows_rules.cpp src/transport/driver/driver_directory_windows.cpp
         src/transport/driver/driver_install_windows.cpp src/transport/driver/driver_loader_windows.cpp)
  endif()
  list(APPEND REDSHIFT_DRIVER_TEST_SOURCES ${redshift_windows_tests})
  set_source_files_properties(${redshift_windows_tests} PROPERTIES
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/driver")
  # The fake driver as a DLL, with every entry point and without SQLGetDiagField, which the load tests
  # copy under generated names; named by absolute path, as the suite is never moved on Windows.
  foreach(variant complete missing_entry)
    add_library(redshift_test_driver_${variant} SHARED EXCLUDE_FROM_ALL
                "${CMAKE_CURRENT_SOURCE_DIR}/test/client/driver/inputs/fake_driver.c")
    set_target_properties(redshift_test_driver_${variant} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/redshift_test_inputs")
  endforeach()
  target_compile_definitions(redshift_test_driver_missing_entry PRIVATE OMIT_GET_DIAG_FIELD)
  set_property(SOURCE test/client/driver/driver_loader_windows_test.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    "REDSHIFT_TEST_DRIVER_COMPLETE=\"$<TARGET_FILE:redshift_test_driver_complete>\""
    "REDSHIFT_TEST_DRIVER_MISSING_ENTRY=\"$<TARGET_FILE:redshift_test_driver_missing_entry>\"")
  list(APPEND REDSHIFT_DRIVER_TEST_DEPENDENCIES redshift_test_driver_complete redshift_test_driver_missing_entry)
endif()
# The embedded bytes, where the platform has an embed (src/transport/driver/sources.cmake, which
# also makes this suite depend on the fetch): the assembler's symbols on Linux and macOS, the
# resources on Windows. The loader's directory for embedded_test_helpers.hpp and the reader.
if(REDSHIFT_DRIVER_GEN_DIR)
  if(REDSHIFT_DRIVER_LOADER STREQUAL "windows")
    set(redshift_embed_test test/client/driver/embedded_resources_test.cpp)
  else()
    set(redshift_embed_test test/client/driver/embedded_driver_test.cpp)
  endif()
  list(APPEND REDSHIFT_DRIVER_TEST_SOURCES ${redshift_embed_test})
  set_source_files_properties(${redshift_embed_test} PROPERTIES
    OBJECT_DEPENDS "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h"
    INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/driver;${REDSHIFT_DRIVER_GEN_DIR}")
endif()
