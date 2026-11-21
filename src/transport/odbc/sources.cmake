# The ODBC client's sources for REDSHIFT_CLIENT=odbc. New ODBC client sources are listed here only.
set(REDSHIFT_ODBC_SOURCES
    src/transport/odbc/build_identity.cpp
    src/transport/odbc/bulk_fetch.cpp
    src/transport/odbc/connection_settings.cpp
    src/transport/odbc/diagnostic_redaction.cpp
    src/transport/odbc/fetch_producer.cpp
    src/transport/odbc/odbc_connection.cpp
    src/transport/odbc/odbc_diagnostics.cpp
    src/transport/odbc/odbc_errors.cpp
    src/transport/odbc/odbc_message_framing.cpp
    src/transport/odbc/odbc_process_state.cpp
    src/transport/odbc/odbc_statement.cpp
    src/transport/odbc/odbc_transport.cpp
    src/transport/odbc/text_forms.cpp
    src/transport/odbc/type_classification.cpp)
# The settings of a Serverless workgroup named without an account: the public build's refusal, or
# a preview implementation where the tree carries one (CMakeLists.txt, redshift_select_hook).
redshift_select_hook(REDSHIFT_ODBC_SOURCES DEFAULT src/transport/odbc/accountless_settings.cpp PREVIEW
                     src/preview/odbc_accountless_settings.cpp)
# Finding the CA bundle reads the host's own configuration. Every resolver starts with the
# SSL_CERT_FILE step; Linux and macOS then search their own platform's locations, Windows exports its
# root store, and any other platform compiles trust_store_unsupported.cpp, which links and refuses
# the connection.
list(APPEND REDSHIFT_ODBC_SOURCES src/transport/odbc/trust_store_cert_file.cpp)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND REDSHIFT_ODBC_SOURCES src/transport/odbc/trust_store_search.cpp src/transport/odbc/trust_store_linux.cpp)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  list(APPEND REDSHIFT_ODBC_SOURCES src/transport/odbc/trust_store_search.cpp src/transport/odbc/trust_store_macos.cpp)
elseif(WIN32)
  list(APPEND REDSHIFT_ODBC_SOURCES src/transport/odbc/trust_store_export.cpp src/transport/odbc/trust_store_windows.cpp)
else()
  list(APPEND REDSHIFT_ODBC_SOURCES src/transport/odbc/trust_store_unsupported.cpp)
endif()

# A test build may aim its connections elsewhere, so the switch fails the
# configure in every build directory but this project's build/driver-e2e, compared by resolved path.
# Only connection_settings.cpp is compiled with it, and always told whether the configure set it, so
# compiler flags that define the macro on their own fail its compile.
option(REDSHIFT_TEST_ENDPOINTS "Let two test variables replace the derived endpoints (tests only)" OFF)
set(redshift_endpoints_configured 0)
if(REDSHIFT_TEST_ENDPOINTS)
  get_filename_component(redshift_build_real "${CMAKE_BINARY_DIR}" REALPATH)
  get_filename_component(redshift_e2e_real "${PROJECT_SOURCE_DIR}/build/driver-e2e" REALPATH)
  if(NOT redshift_build_real STREQUAL redshift_e2e_real)
    message(FATAL_ERROR "redshift: REDSHIFT_TEST_ENDPOINTS is on in the build directory '${CMAKE_BINARY_DIR}'. "
                        "It lets environment variables redirect the connection, so only this project's "
                        "build/driver-e2e may carry it; configure this build without it.")
  endif()
  message(WARNING "redshift: REDSHIFT_TEST_ENDPOINTS is on; this build is for testing only and must not be shipped")
  set_property(SOURCE src/transport/odbc/connection_settings.cpp APPEND PROPERTY COMPILE_DEFINITIONS
                                                                               REDSHIFT_TEST_ENDPOINTS)
  set(redshift_endpoints_configured 1)
endif()
set_property(
  SOURCE src/transport/odbc/connection_settings.cpp APPEND
  PROPERTY COMPILE_DEFINITIONS REDSHIFT_TEST_ENDPOINTS_CONFIGURED=${redshift_endpoints_configured})

# MSVC compiles the ODBC client's own files at /W4 with warnings as errors, the gate that stands in for
# clang-tidy on Windows-only code; DuckDB turns its narrowing warnings, C4244 and C4267, off for the
# whole build, so they come back on here. DuckDB's headers, the vendored ODBC headers and the Windows
# SDK's are external, so their warnings stay out; other compilers get no flags here.
if(MSVC)
  set_property(
    SOURCE ${REDSHIFT_ODBC_SOURCES} APPEND
    PROPERTY COMPILE_OPTIONS
             /W4
             /WX
             /w44244
             /w44267
             /external:W0
             /external:anglebrackets
             "/external:I${CMAKE_SOURCE_DIR}/src/include"
             "/external:I${CMAKE_SOURCE_DIR}/third_party"
             "/external:I${PROJECT_SOURCE_DIR}/third_party/unixodbc")
endif()
