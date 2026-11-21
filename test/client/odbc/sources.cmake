# C++ tests in test/client/odbc/, run by make test_client, and the configure steps some of them
# need; the [live] cases skip without the live variables. A new test file is listed here, not in
# CMakeLists.txt.
list(
  APPEND
  REDSHIFT_CLIENT_TEST_SOURCES
  test/client/odbc/buffer_ring_test.cpp
  test/client/odbc/bulk_fetch_test.cpp
  test/client/odbc/closed_state_test.cpp
  test/client/odbc/connect_error_test.cpp
  test/client/odbc/connection_environment_test.cpp
  test/client/odbc/connection_loss_live_test.cpp
  test/client/odbc/connection_loss_test.cpp
  test/client/odbc/connection_string_test.cpp
  test/client/odbc/connection_teardown_test.cpp
  test/client/odbc/connection_test.cpp
  test/client/odbc/credentials_test.cpp
  test/client/odbc/dead_peer_test.cpp
  test/client/odbc/detach_bystander_live_test.cpp
  test/client/odbc/diagnostic_reader_test.cpp
  test/client/odbc/diagnostic_redaction_test.cpp
  test/client/odbc/driver_output_live_test.cpp
  test/client/odbc/driver_output_test.cpp
  test/client/odbc/driver_source_hook.cpp
  test/client/odbc/driver_source_test.cpp
  test/client/odbc/exec_direct_test.cpp
  test/client/odbc/execute_test.cpp
  test/client/odbc/fetch_test.cpp
  test/client/odbc/first_query_test.cpp
  test/client/odbc/live_workgroup_test.cpp
  test/client/odbc/number_text_test.cpp
  test/client/odbc/output_canary_test.cpp
  test/client/odbc/prepare_once_live_test.cpp
  test/client/odbc/prepare_once_test.cpp
  test/client/odbc/process_environment_test.cpp
  test/client/odbc/process_state_test.cpp
  test/client/odbc/producer_thread_test.cpp
  test/client/odbc/redshift_query_recovery_test.cpp
  test/client/odbc/refused_configuration_test.cpp
  test/client/odbc/region_allowlist_test.cpp
  test/client/odbc/region_validation_test.cpp
  test/client/odbc/result_lifetime_test.cpp
  test/client/odbc/resume_retry_test.cpp
  test/client/odbc/resume_timeout_test.cpp
  test/client/odbc/second_region_live_test.cpp
  test/client/odbc/server_text_leak_test.cpp
  test/client/odbc/session_probe_test.cpp
  test/client/odbc/session_setup_test.cpp
  test/client/odbc/several_connections_test.cpp
  test/client/odbc/shape_check_test.cpp
  test/client/odbc/statement_error_live_test.cpp
  test/client/odbc/statement_error_test.cpp
  test/client/odbc/statement_test.cpp
  test/client/odbc/test_endpoints_test.cpp
  test/client/odbc/text_forms_test.cpp
  test/client/odbc/transfer_probe_test.cpp
  test/client/odbc/type_classification_test.cpp)
# Windows leaves out the files that need a POSIX facility, which test/README.md lists with reasons:
# SSL_CERT_FILE=/, /dev/null, setenv, or POSIX processes. The others connect through the fake driver
# as on Linux, with SSL_CERT_FILE naming the fixture's bundle.
if(WIN32)
  list(
    REMOVE_ITEM
    REDSHIFT_CLIENT_TEST_SOURCES
    test/client/odbc/connection_test.cpp
    test/client/odbc/dead_peer_test.cpp
    test/client/odbc/output_canary_test.cpp
    test/client/odbc/process_state_test.cpp
    test/client/odbc/refused_configuration_test.cpp
    test/client/odbc/region_allowlist_test.cpp)
  # The refusals that come before the driver source, set in the environment block, through the
  # extension. Not in the test-endpoint build, which refuses a test variable with its own message
  # (test_endpoints_test.cpp).
  if(NOT REDSHIFT_TEST_ENDPOINTS)
    list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/odbc/load_only_refusal_test.cpp)
  endif()
endif()
# A build without a driver is the one sure to give a connection the NOT_BUILT placeholder's failure,
# where a connect reaches the driver source.
if(REDSHIFT_DRIVER STREQUAL "none")
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/odbc/shipped_driver_source_test.cpp)
endif()

# connection_string_test.cpp ports the driver's parser, and a case fails when the driver lock names
# a version the port was not compared with. The configure reads the version as the driver loader's tools do,
# honouring RS_DRIVER_LOCK; no reader, no python3 or an unreadable lock leaves it empty.
if(WIN32)
  # The bash on a Windows PATH can be WSL's, so the configure runs check-lock.sh's reader, lock.py,
  # itself, on check-lock.sh's terms: RS_DRIVER_LOCK when set, a set but empty one refused, else
  # the checked-in lock. A change to the reader configures again, as a change to a lock does.
  set(redshift_lock_reader ${PROJECT_SOURCE_DIR}/third_party/redshift_odbc/tools/lib/lock.py)
  if(DEFINED ENV{RS_DRIVER_LOCK})
    set(redshift_driver_lock "$ENV{RS_DRIVER_LOCK}")
  else()
    set(redshift_driver_lock ${PROJECT_SOURCE_DIR}/third_party/redshift_odbc/driver.lock)
  endif()
  set(redshift_lock "")
  if(Python3_FOUND AND NOT redshift_driver_lock STREQUAL "")
    execute_process(COMMAND ${Python3_EXECUTABLE} -I ${redshift_lock_reader} ${redshift_driver_lock}
                    OUTPUT_VARIABLE redshift_lock WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${redshift_lock_reader})
else()
  set(redshift_check_lock ${PROJECT_SOURCE_DIR}/third_party/redshift_odbc/tools/check-lock.sh)
  # On failure check-lock.sh prints nothing on stdout (tools/lib/lock.py), so no version is found.
  # It runs from the checkout, so a relative RS_DRIVER_LOCK names the same file on the re-run a
  # build triggers from the build tree as at the first configure.
  execute_process(COMMAND bash ${redshift_check_lock} OUTPUT_VARIABLE redshift_lock
                  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR})
endif()
set(redshift_locked_version "")
if(redshift_lock MATCHES "\nversion ([^\n]+)")
  set(redshift_locked_version ${CMAKE_MATCH_1})
else()
  message(WARNING "redshift: the driver lock could not be read, so the parser port's version check fails")
endif()
set_source_files_properties(test/client/odbc/connection_string_test.cpp PROPERTIES
                            COMPILE_DEFINITIONS REDSHIFT_LOCKED_DRIVER_VERSION="${redshift_locked_version}")
# The version is read only at configure, so a rebuild after a lock change must configure again:
# watch both locks, the checked-in one and the one RS_DRIVER_LOCK names, which check-lock.sh reads on
# Linux and macOS and lock.py on Windows, where CMake reads a relative path from the checkout too.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
             ${PROJECT_SOURCE_DIR}/third_party/redshift_odbc/driver.lock $ENV{RS_DRIVER_LOCK})

# The python3 DuckDB's find_package found, or empty: Python3_EXECUTABLE holds a -NOTFOUND value when
# there is none, and the test then says python3 is missing rather than failing as a missing command.
if(Python3_FOUND)
  set(redshift_test_python "${Python3_EXECUTABLE}")
else()
  set(redshift_test_python "")
endif()
# PROJECT_SOURCE_DIR is this checkout; CMAKE_SOURCE_DIR is DuckDB's, the top-level project. Set on
# these test files alone, so no shipped source is compiled against a path from the machine that built it.
set_source_files_properties(
  test/client/odbc/region_allowlist_test.cpp test/client/odbc/output_canary_test.cpp
  test/client/odbc/environment_gate_test.cpp test/client/odbc/known_environment_live_test.cpp
  PROPERTIES COMPILE_DEFINITIONS
             "REDSHIFT_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\";REDSHIFT_PYTHON=\"${redshift_test_python}\"")

# test_endpoints_test.cpp checks the client's test-endpoint switch against the configure's own.
if(REDSHIFT_TEST_ENDPOINTS)
  set(redshift_test_endpoints_configured 1)
else()
  set(redshift_test_endpoints_configured 0)
endif()
set_source_files_properties(
  test/client/odbc/test_endpoints_test.cpp
  PROPERTIES COMPILE_DEFINITIONS "REDSHIFT_TEST_ENDPOINTS_CONFIGURED=${redshift_test_endpoints_configured}")

# Each platform tests the trust resolver its build compiles (src/transport/odbc/sources.cmake).
# The TLS cases run against tls-verification.sh's servers, on Windows through Git Bash.
# environment_gate_test.cpp and known_environment_live_test.cpp run their cases in new processes they
# trace with seccomp, /proc and inotify, which only Linux has.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(
    APPEND
    REDSHIFT_CLIENT_TEST_SOURCES
    test/client/odbc/trust_store_search_test.cpp
    test/client/odbc/trust_store_linux_test.cpp
    test/client/odbc/tls_verification_test.cpp
    test/client/odbc/environment_gate_test.cpp
    test/client/odbc/known_environment_live_test.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/odbc/trust_store_search_test.cpp
       test/client/odbc/trust_store_macos_test.cpp test/client/odbc/tls_verification_test.cpp)
elseif(WIN32)
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/odbc/trust_store_windows_test.cpp
       test/client/odbc/tls_verification_test.cpp)
else()
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/odbc/trust_store_unsupported_test.cpp)
endif()
# The Windows export's rules are plain C++, so every platform tests them; only Windows links them
# into the extension, and the others into the tests alone.
list(APPEND REDSHIFT_CLIENT_TEST_SOURCES test/client/odbc/trust_store_export_test.cpp)
if(NOT WIN32)
  list(APPEND REDSHIFT_CLIENT_TEST_SOURCES src/transport/odbc/trust_store_export.cpp)
endif()

# Which accountless-workgroup settings the build should compile, 1 for the preview's, for the files
# whose cases depend on it. Worked out apart from redshift_select_hook, as the mock probe's is, so a
# wrong selection fails a case instead of hiding one. Appended, after the properties set above.
if(IS_DIRECTORY "${PROJECT_SOURCE_DIR}/src/preview")
  set(redshift_test_accountless_preview 1)
else()
  set(redshift_test_accountless_preview 0)
endif()
set_property(
  SOURCE test/client/odbc/connection_string_test.cpp test/client/odbc/refused_configuration_test.cpp
  APPEND
  PROPERTY COMPILE_DEFINITIONS REDSHIFT_TEST_ACCOUNTLESS_PREVIEW=${redshift_test_accountless_preview})
