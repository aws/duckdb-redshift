# Driver sources for REDSHIFT_CLIENT=odbc builds. The platform loaders are added here; the
# driver interface owns the line for the NOT_BUILT placeholder, which every ODBC build without a
# loader links (REDSHIFT_DRIVER=none, or a platform without one).
set(REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_not_built.cpp)

# The embedded driver. Only REDSHIFT_DRIVER=embedded builds for a platform with an embed (glibc
# Linux, ELF; macOS, Mach-O; MSVC Windows, PE) fetch the locked driver and embed it; every other
# build has neither step, so it needs no network (make test_mock, make test_client). The platform is
# DUCKDB_EXPLICIT_PLATFORM, as DuckDB's distribution build passes it, or else CMake's target system
# and processor (for a single-architecture macOS build, CMAKE_OSX_ARCHITECTURES) in DuckDB's names
# where there is one (linux, osx, windows, freebsd; amd64, arm64) and as CMake names them otherwise,
# with no _musl, _mingw or _android suffix. For each platform with an embed a compile check
# confirms the compiler targets that system and architecture before anything is fetched, so a
# universal macOS build (two architectures, one of them not the platform's) fails there: an
# extension is built for one DuckDB platform.
if(REDSHIFT_DRIVER STREQUAL "embedded")
  if(DUCKDB_EXPLICIT_PLATFORM)
    set(REDSHIFT_DRIVER_PLATFORM "${DUCKDB_EXPLICIT_PLATFORM}")
  else()
    set(redshift_os "${CMAKE_SYSTEM_NAME}")
    set(redshift_arch "${CMAKE_SYSTEM_PROCESSOR}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      set(redshift_os linux)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
      set(redshift_os osx)
      list(LENGTH CMAKE_OSX_ARCHITECTURES redshift_osx_arch_count)
      if(redshift_osx_arch_count EQUAL 1)
        set(redshift_arch "${CMAKE_OSX_ARCHITECTURES}")
      endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
      set(redshift_os windows)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "FreeBSD")
      set(redshift_os freebsd)
    endif()
    if(redshift_arch MATCHES "^(x86_64|AMD64|amd64)$")
      set(redshift_arch amd64)
    elseif(redshift_arch MATCHES "^(aarch64|arm64|ARM64)$")
      set(redshift_arch arm64)
    endif()
    set(REDSHIFT_DRIVER_PLATFORM "${redshift_os}_${redshift_arch}")
  endif()
  # Which platform, and below which lock by path and content, this build embeds: a record, written
  # for every embedded build and rewritten only when it changes, so its time stamp says when the
  # platform or lock last changed, and embedded-build_test.sh and make test_driver can tell a
  # platform with no embed from a broken build. The fetch does not depend on it: the lock's
  # content reaches the build through the copy below, and the platform is on the fetch's command
  # line, which Ninja and CMake's Makefiles both run again when it changes.
  set(redshift_identity "platform ${REDSHIFT_DRIVER_PLATFORM}\n")
  set(redshift_identity_file "${CMAKE_CURRENT_BINARY_DIR}/redshift_driver_identity.txt")

  set(redshift_object_format)
  if(REDSHIFT_DRIVER_PLATFORM MATCHES "^linux_(amd64|arm64)$")
    set(redshift_object_format elf)
  elseif(REDSHIFT_DRIVER_PLATFORM MATCHES "^osx_(amd64|arm64)$")
    set(redshift_object_format macho)
  elseif(REDSHIFT_DRIVER_PLATFORM STREQUAL "windows_amd64")
    set(redshift_object_format pe)
  endif()
  if(redshift_object_format)
    if(CMAKE_VERSION VERSION_LESS 3.19)
      message(FATAL_ERROR "redshift: embedding the driver needs CMake 3.19 or newer (cmake_language DEFER)")
    endif()
    # The resource object reaches the executables through INTERFACE_LINK_LIBRARIES (below).
    if(redshift_object_format STREQUAL "pe" AND CMAKE_VERSION VERSION_LESS 3.21)
      message(FATAL_ERROR "redshift: embedding the driver for Windows needs CMake 3.21 or newer, "
                          "which links $<TARGET_OBJECTS> through a target's link interface")
    endif()
    set(redshift_target_key REDSHIFT_DRIVER_TARGETS_${REDSHIFT_DRIVER_PLATFORM})
    # The way out of a target mismatch on every platform, named once so neither hint splits it.
    set(redshift_platform_hint "set DUCKDB_PLATFORM to the target's DuckDB platform")
    if(redshift_object_format STREQUAL "elf")
      if(REDSHIFT_DRIVER_PLATFORM STREQUAL "linux_amd64")
        set(redshift_target_arch "__x86_64__")
      else()
        set(redshift_target_arch "__aarch64__")
      endif()
      set(redshift_target_check "#include <features.h>
#if !defined(__GLIBC__) || !defined(${redshift_target_arch}) || !defined(__LP64__)")
      set(redshift_target_name "64-bit glibc Linux")
      set(redshift_target_hint "${redshift_platform_hint}")
    elseif(redshift_object_format STREQUAL "pe")
      # DuckDB's windows_amd64 is the MSVC build; a MinGW compiler, whose platform is windows_amd64_mingw,
      # also defines _WIN64 but not _MSC_VER, so it is refused here rather than at the resource link.
      set(redshift_target_arch "_M_X64")
      set(redshift_target_check "#if !defined(_MSC_VER) || !defined(_WIN64) || !defined(_M_X64)")
      set(redshift_target_name "64-bit MSVC Windows")
      set(redshift_target_hint "${redshift_platform_hint}")
    else()
      if(REDSHIFT_DRIVER_PLATFORM STREQUAL "osx_amd64")
        set(redshift_target_arch "__x86_64__")
        set(redshift_osx_build_arch x86_64)
      else()
        set(redshift_target_arch "__arm64__")
        set(redshift_osx_build_arch arm64)
      endif()
      # TARGET_OS_OSX tells macOS from Apple's other systems, which share __APPLE__. The result is cached
      # under the architectures too: DuckDB rewrites CMAKE_OSX_ARCHITECTURES in an existing build directory.
      set(redshift_target_check "#include <TargetConditionals.h>
#if !defined(__APPLE__) || !TARGET_OS_OSX || !defined(${redshift_target_arch}) || !defined(__LP64__)")
      set(redshift_target_name "64-bit macOS")
      set(redshift_target_hint
          "build one architecture, with OSX_BUILD_ARCH=${redshift_osx_build_arch}, or ${redshift_platform_hint}")
      string(REPLACE ";" "_" redshift_osx_archs "${CMAKE_OSX_ARCHITECTURES}")
      string(APPEND redshift_target_key "_${redshift_osx_archs}")
    endif()
    include(CheckCSourceCompiles)
    check_c_source_compiles("${redshift_target_check}
#error not this platform
#endif
int main(void) { return 0; }" ${redshift_target_key})
    if(NOT ${redshift_target_key})
      message(FATAL_ERROR "redshift: the platform is ${REDSHIFT_DRIVER_PLATFORM}, but the C compiler does not target "
                          "${redshift_target_name} on ${redshift_target_arch}; ${redshift_target_hint}")
    endif()

    set(redshift_odbc_dir "${CMAKE_CURRENT_SOURCE_DIR}/third_party/redshift_odbc")
    # The lock this build embeds: RS_DRIVER_LOCK when it is set at configure time, for example to a
    # lock written from verify-release.sh's rows, otherwise the checked-in one. A lock given that
    # way is kept in the cache once it passes the checks below, so a later configure without the
    # variable, such as the one a build starts by itself when the lock changes, keeps it. Setting
    # RS_DRIVER_LOCK chooses another, and setting it to the checked-in lock's path goes back to that
    # one: an environment variable reaches every make target, where a -D option would not.
    set(redshift_checked_in_lock "${redshift_odbc_dir}/driver.lock")
    set(redshift_lock_hint "set RS_DRIVER_LOCK to another lock, or to ${redshift_checked_in_lock} for the checked-in one")
    set(REDSHIFT_DRIVER_LOCK "" CACHE FILEPATH
        "The driver lock to embed, set from RS_DRIVER_LOCK at configure time; empty for the checked-in lock")
    if(DEFINED ENV{RS_DRIVER_LOCK})
      set(redshift_lock_source "$ENV{RS_DRIVER_LOCK}")
      if(NOT IS_ABSOLUTE "${redshift_lock_source}" OR NOT EXISTS "${redshift_lock_source}")
        message(FATAL_ERROR "redshift: RS_DRIVER_LOCK must name an existing lock by absolute path, not '${redshift_lock_source}'")
      endif()
    elseif(REDSHIFT_DRIVER_LOCK)
      set(redshift_lock_source "${REDSHIFT_DRIVER_LOCK}")
      if(NOT IS_ABSOLUTE "${redshift_lock_source}" OR NOT EXISTS "${redshift_lock_source}")
        message(FATAL_ERROR "redshift: this build directory embeds the lock '${redshift_lock_source}' (REDSHIFT_DRIVER_LOCK), "
                            "which must be an existing file named by absolute path; ${redshift_lock_hint}")
      endif()
    else()
      set(redshift_lock_source "${redshift_checked_in_lock}")
    endif()
    # The build fetches from a copy of the lock, so it embeds exactly the bytes checked here, at every
    # configure (which every make target runs), where a bad lock or a lock without this platform
    # fails before the build starts. configure_file makes the lock an input of the configure, so a
    # build that finds the lock newer than its build files configures again first. The candidate is
    # checked in its own file, and only a lock that passes is cached and reaches the copy, so a
    # refused configure leaves the build directory's lock as it was. The copy is rewritten only when
    # its content changes, so its time stamp tells the build to fetch again: Make compares time
    # stamps only, and a lock replaced by an older file would otherwise leave the earlier set. Until
    # the next configure, the build embeds the lock as last configured.
    set(redshift_lock "${CMAKE_CURRENT_BINARY_DIR}/redshift_driver.lock")
    set(redshift_lock_candidate "${redshift_lock}.candidate")
    configure_file("${redshift_lock_source}" "${redshift_lock_candidate}" COPYONLY)
    execute_process(
      COMMAND bash "${redshift_odbc_dir}/tools/check-lock.sh" "${redshift_lock_candidate}"
      OUTPUT_VARIABLE redshift_lock_text RESULT_VARIABLE redshift_lock_status ERROR_VARIABLE redshift_lock_errors)
    if(NOT redshift_lock_status EQUAL 0)
      message(FATAL_ERROR "redshift: the driver lock ${redshift_lock_source} fails check-lock.sh; ${redshift_lock_hint}:\n"
                          "${redshift_lock_errors}")
    endif()
    # fetch-driver.sh leaves the driver set and driver_info.h here; driver_info.h, written last,
    # names each file by its absolute path for the assembler's .incbin and the resource script.
    set(REDSHIFT_DRIVER_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/redshift_driver")
    set(redshift_set_files)
    string(REPLACE "\n" ";" redshift_lock_lines "${redshift_lock_text}")
    foreach(line IN LISTS redshift_lock_lines)
      if(line MATCHES "^driver ${REDSHIFT_DRIVER_PLATFORM} ([^ ]+) ")
        list(INSERT redshift_set_files 0 "${REDSHIFT_DRIVER_GEN_DIR}/${CMAKE_MATCH_1}")
      elseif(line MATCHES "^notice ([^ ]+) ")
        list(APPEND redshift_set_files "${REDSHIFT_DRIVER_GEN_DIR}/${CMAKE_MATCH_1}")
      endif()
    endforeach()
    if(redshift_object_format STREQUAL "pe")
      # The Windows set also holds the libraries, each renamed file under its embedded name.
      include("${CMAKE_CURRENT_LIST_DIR}/set_files.cmake")
      redshift_windows_set_files(redshift_set_files "${REDSHIFT_DRIVER_PLATFORM}"
                                 "${REDSHIFT_DRIVER_GEN_DIR}" "${redshift_lock_lines}")
    endif()
    if(NOT redshift_lock_text MATCHES "(^|\n)driver ${REDSHIFT_DRIVER_PLATFORM} ")
      message(FATAL_ERROR "redshift: the driver lock ${redshift_lock_source} has no driver row for ${REDSHIFT_DRIVER_PLATFORM}; "
                          "${redshift_lock_hint}")
    endif()
    if(DEFINED ENV{RS_DRIVER_LOCK})
      set(REDSHIFT_DRIVER_LOCK "${redshift_lock_source}" CACHE FILEPATH
          "The driver lock to embed, set from RS_DRIVER_LOCK at configure time; empty for the checked-in lock" FORCE)
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${redshift_lock_candidate}" "${redshift_lock}"
                    RESULT_VARIABLE redshift_lock_status)
    if(NOT redshift_lock_status EQUAL 0)
      message(FATAL_ERROR "redshift: cannot copy the driver lock to ${redshift_lock}")
    endif()
    # The copy is written here, not by configure_file, so it is not a configure output: making it a
    # configure input is what lets a build that finds it missing configure again and recreate it.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${redshift_lock}")
    file(SHA256 "${redshift_lock}" redshift_lock_sha256)
    string(APPEND redshift_identity "lock ${redshift_lock_source}\nsha256 ${redshift_lock_sha256}\n")

    # A macOS package is a xar archive, which fetch-driver.sh reads with lib/xar.py. Each Windows input
    # the fetch reads is listed by file, as it joins the fetch: a directory in DEPENDS tracks only its
    # own time stamp, so an edit inside policy/windows/ would not fetch again.
    set(redshift_fetch_depends)
    if(redshift_object_format STREQUAL "macho")
      list(APPEND redshift_fetch_depends "${redshift_odbc_dir}/tools/lib/xar.py"
           "${redshift_odbc_dir}/policy/macos/signer.txt")
    elseif(redshift_object_format STREQUAL "pe")
      list(APPEND redshift_fetch_depends "${redshift_odbc_dir}/tools/lib/msi.py"
           "${redshift_odbc_dir}/tools/lib/cfb.py" "${redshift_odbc_dir}/tools/lib/cab.py"
           "${redshift_odbc_dir}/tools/lib/pe.py"
           "${redshift_odbc_dir}/tools/lib/rename_apply.py"
           "${redshift_odbc_dir}/policy/windows/runtime-names.txt")
    endif()
    add_custom_command(
      OUTPUT "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h" ${redshift_set_files}
      COMMAND "${CMAKE_COMMAND}" -E env "RS_DRIVER_LOCK=${redshift_lock}"
              bash "${redshift_odbc_dir}/tools/fetch-driver.sh" "${REDSHIFT_DRIVER_PLATFORM}" "${REDSHIFT_DRIVER_GEN_DIR}"
      DEPENDS "${redshift_lock}" "${redshift_odbc_dir}/LICENSE"
              "${redshift_odbc_dir}/NOTICE" "${redshift_odbc_dir}/THIRD_PARTY_LICENSES"
              "${redshift_odbc_dir}/tools/check-lock.sh" "${redshift_odbc_dir}/tools/fetch-driver.sh"
              "${redshift_odbc_dir}/tools/lib/lock.py"
              "${redshift_odbc_dir}/tools/lib/driver_info.py" "${redshift_odbc_dir}/tools/lib/cpio.py"
              "${redshift_odbc_dir}/tools/lib/rpm.py" "${redshift_odbc_dir}/tools/lib/download.sh"
              "${redshift_odbc_dir}/tools/lib/common.sh"
              "${redshift_odbc_dir}/policy/refused-environment.txt"
              "${redshift_odbc_dir}/policy/known-environment.txt"
              "${redshift_odbc_dir}/policy/host-prerequisites.txt" ${redshift_fetch_depends}
      COMMENT "Fetching the locked Redshift ODBC driver for ${REDSHIFT_DRIVER_PLATFORM} and checking it against the lock"
      VERBATIM)
    # One target owns the fetch, so a parallel Unix Makefiles build runs it once, and the targets
    # that read its outputs depend on that target, which the deferred call adds once they exist. The
    # set is not hashed again at every build: the fetch checked it, the build host is trusted, and
    # the loader hashes the embedded bytes before it writes them and every file before every load.
    add_custom_target(redshift_driver_set
      DEPENDS "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h" ${redshift_set_files})
    function(redshift_driver_depend_on_set)
      foreach(target ${EXTENSION_NAME} ${LOADABLE_EXTENSION_NAME} redshift_driver_tests redshift_driver_resources)
        if(TARGET ${target})
          add_dependencies(${target} redshift_driver_set)
        endif()
      endforeach()
    endfunction()
    cmake_language(DEFER CALL redshift_driver_depend_on_set)

    if(redshift_object_format STREQUAL "pe")
      # MSVC has no .incbin, so the set is RCDATA resources, from a script driver_info.py writes from
      # driver_info.h's list. A compiled resource has no symbol that pulls it out of a static library,
      # so it is an OBJECT library's, carried to every image that holds the loader (below).
      enable_language(RC)
      set(redshift_resource_template "${CMAKE_CURRENT_SOURCE_DIR}/src/transport/driver/embedded_driver_pe.rc.in")
      set(redshift_embed_source "${CMAKE_CURRENT_BINARY_DIR}/embedded_driver_pe.rc")
      add_custom_command(
        OUTPUT "${redshift_embed_source}"
        COMMAND "${Python3_EXECUTABLE}" -I "${redshift_odbc_dir}/tools/lib/driver_info.py" resources
                "${redshift_resource_template}" "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h" "${redshift_embed_source}"
        DEPENDS "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h" "${redshift_resource_template}"
                "${redshift_odbc_dir}/tools/lib/driver_info.py"
        COMMENT "Writing the resource script for the embedded Redshift ODBC driver set"
        VERBATIM)
      add_library(redshift_driver_resources OBJECT "${redshift_embed_source}")
      # The static extension's link interface carries the resource object into each image that links
      # it (the shell, unittest, duckdb.dll, the client and driver suites and DuckDB's
      # plan_serializer.exe); the property is set directly because DuckDB links that target with the
      # plain signature, which a keyword call would conflict with. BUILD_INTERFACE keeps it out of
      # the installed export. The loadable extension compiles its own sources, so it takes the object.
      function(redshift_driver_link_resources)
        set_property(TARGET ${EXTENSION_NAME} APPEND PROPERTY INTERFACE_LINK_LIBRARIES
                     "$<BUILD_INTERFACE:$<TARGET_OBJECTS:redshift_driver_resources>>")
        target_sources(${LOADABLE_EXTENSION_NAME} PRIVATE $<TARGET_OBJECTS:redshift_driver_resources>)
      endfunction()
      cmake_language(DEFER CALL redshift_driver_link_resources)
    else()
      enable_language(ASM)
      set(redshift_embed_source src/transport/driver/embedded_driver_${redshift_object_format}.S)
      # A clang-tidy configure (make tidy_driver) builds nothing, so the driver_info.h the assembler
      # file includes does not exist there, and clang-tidy has nothing to check in assembler.
      if(NOT CLANG_TIDY)
        list(APPEND REDSHIFT_DRIVER_SOURCES ${redshift_embed_source})
      endif()
    endif()
    # The set's files too, so the object is built again when one of them changes: neither the assembler
    # nor the resource compiler tracks a file it embeds.
    set_source_files_properties(${redshift_embed_source} PROPERTIES
      OBJECT_DEPENDS "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h;${redshift_set_files}"
      INCLUDE_DIRECTORIES "${REDSHIFT_DRIVER_GEN_DIR}")
    # The platform's loader replaces the NOT_BUILT placeholder. driver_embedded.cpp reads driver_info.h
    # and refers to the embedded bytes, so the loadable extension carries the loader and the bytes
    # once the ODBC client calls LoadEmbeddedDriver; an ELF or Mach-O build whose client never calls
    # it drops both as unreferenced, while a Windows image keeps its resources either way. The tests
    # that link the loader are added only where it is built (REDSHIFT_DRIVER_LOADER,
    # test/client/driver/sources.cmake). A clang-tidy configure builds nothing and fetches nothing,
    # so the driver_info.h that driver_embedded.cpp includes does not exist there; it gets a
    # stand-in with the same macros (below), so that file is checked too.
    # Windows has no POSIX directory, install or load code: it links the Windows loader's own files.
    list(REMOVE_ITEM REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_not_built.cpp)
    if(NOT redshift_object_format STREQUAL "pe")
      list(APPEND REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_directory.cpp
                                          src/transport/driver/driver_install.cpp
                                          src/transport/driver/driver_loader.cpp)
    endif()
    if(redshift_object_format STREQUAL "elf")
      set(REDSHIFT_DRIVER_LOADER linux)
      list(APPEND REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_loader_linux.cpp
                                          src/transport/driver/driver_search_list.cpp
                                          src/transport/driver/allocator_replacement_linux.cpp)
    elseif(redshift_object_format STREQUAL "macho")
      set(REDSHIFT_DRIVER_LOADER macos)
      list(APPEND REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_loader_macos.cpp)
    else()
      set(REDSHIFT_DRIVER_LOADER windows)
      list(APPEND REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_windows_rules.cpp
                                          src/transport/driver/driver_directory_windows.cpp
                                          src/transport/driver/driver_install_windows.cpp
                                          src/transport/driver/driver_loader_windows.cpp)
    endif()
    # The Windows rules need no Windows header, so a clang-tidy configure anywhere checks them.
    if(CLANG_TIDY AND NOT redshift_object_format STREQUAL "pe")
      list(APPEND REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_windows_rules.cpp)
    endif()
    list(APPEND REDSHIFT_DRIVER_SOURCES src/transport/driver/driver_embedded.cpp)
    if(CLANG_TIDY)
      # Never compiled or linked: clang-tidy only reads it. The values are made up; the macros are the
      # ones fetch-driver.sh writes (through tools/lib/driver_info.py), so a change to driver_embedded.cpp
      # that uses another fails here.
      set(redshift_tidy_info_dir "${CMAKE_CURRENT_BINARY_DIR}/redshift_driver_tidy")
      file(WRITE "${redshift_tidy_info_dir}/driver_info.h"
"#pragma once
#define REDSHIFT_DRIVER_VERSION \"0.0.0.0\"
#define REDSHIFT_DRIVER_PLATFORM \"tidy\"
#define REDSHIFT_DRIVER_DEVELOPMENT 1
#define REDSHIFT_DRIVER_SET_SHA256 \"0000000000000000000000000000000000000000000000000000000000000000\"
#define REDSHIFT_DRIVER_FILES(X) \\
	X(\"librsodbc64.so\", 1, \"0000000000000000000000000000000000000000000000000000000000000000\", driver) \\
	X(\"NOTICE\", 1, \"0000000000000000000000000000000000000000000000000000000000000000\", notice_notice)
#define REDSHIFT_DRIVER_REFUSED_ENVIRONMENT \"PGOPTIONS\"
#define REDSHIFT_DRIVER_LOAD_REFUSED_ENVIRONMENT \"PGOPTIONS\"
#define REDSHIFT_DRIVER_OVERRIDDEN_ENVIRONMENT {\"SSL_CERT_FILE\", \"CaFile\"}
#define REDSHIFT_DRIVER_HOST_PREREQUISITES \"libkeyutils.so.1\"
")
      set_source_files_properties(src/transport/driver/driver_embedded.cpp PROPERTIES
        INCLUDE_DIRECTORIES "${redshift_tidy_info_dir}")
    else()
      set_source_files_properties(src/transport/driver/driver_embedded.cpp PROPERTIES
        OBJECT_DEPENDS "${REDSHIFT_DRIVER_GEN_DIR}/driver_info.h"
        INCLUDE_DIRECTORIES "${REDSHIFT_DRIVER_GEN_DIR}")
    endif()
    message(STATUS "redshift: embedding the locked driver for ${REDSHIFT_DRIVER_PLATFORM} from ${redshift_lock_source}")
  else()
    message(STATUS "redshift: no embedded driver for ${REDSHIFT_DRIVER_PLATFORM}; the build links the NOT_BUILT placeholder")
  endif()
  set(redshift_identity_old "")
  if(EXISTS "${redshift_identity_file}")
    file(READ "${redshift_identity_file}" redshift_identity_old)
  endif()
  if(NOT redshift_identity STREQUAL redshift_identity_old)
    file(WRITE "${redshift_identity_file}" "${redshift_identity}")
  endif()
endif()
