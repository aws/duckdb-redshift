//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// src/transport/driver/embedded_resources.hpp
//
// The loader's own, Windows only: the set's reader and the loader's inputs, declared for
// driver_embedded.cpp and its tests.
// MSVC has no .incbin, so the set is the RCDATA resources of each image that holds the loader
// (embedded_driver_pe.rc.in).
//===----------------------------------------------------------------------===//
#pragma once

#include "odbc_driver.hpp" // <windows.h> first on Windows
#include "driver_loader_windows.hpp"
#include "driver_windows.hpp"

#include <vector>

namespace duckdb {
namespace redshift_driver {
namespace detail {

// A file of the set as its resource holds it, with the SHA-256 driver_info.h records: the Windows
// loader's own SetFile, so the checked files are handed to it as they are.
using EmbeddedResource = windows_driver::SetFile;

// Reads each file of driver_info.h's list from `module`'s RCDATA resource of its stem in upper case,
// and checks its size and SHA-256 before any byte is used. On a missing or changed
// resource returns false with an EMBEDDED_BYTES_CORRUPT error naming the file, never a path.
bool ReadEmbeddedResources(HMODULE module, std::vector<EmbeddedResource> &files, DriverError &error);

// The loader's inputs: `files`, as ReadEmbeddedResources checked them, and driver_info.h's identity,
// environment lists and VC++ runtime minimums.
windows_driver::LoaderConfig EmbeddedConfig(const std::vector<EmbeddedResource> &files);

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
