// The name the host probe (probe/driver_probe.cpp) prints for each DriverErrorKind, shared with
// the driver suite, whose table case requires every kind to have its own. A kind without an arm here does
// not compile.
#pragma once

#include "driver_loader_core.hpp"

namespace redshift_driver_test {

inline const char *ErrorKindName(duckdb::redshift_driver::DriverErrorKind kind) {
	using duckdb::redshift_driver::DriverErrorKind;
#pragma GCC diagnostic push
#pragma GCC diagnostic error "-Wswitch"
	switch (kind) {
	case DriverErrorKind::NOT_BUILT:
		return "NOT_BUILT";
	case DriverErrorKind::EMBEDDED_BYTES_CORRUPT:
		return "EMBEDDED_BYTES_CORRUPT";
	case DriverErrorKind::NO_USABLE_DIRECTORY:
		return "NO_USABLE_DIRECTORY";
	case DriverErrorKind::MISSING_PREREQUISITE:
		return "MISSING_PREREQUISITE";
	case DriverErrorKind::LOAD_FAILED:
		return "LOAD_FAILED";
	case DriverErrorKind::MISSING_ENTRY_POINT:
		return "MISSING_ENTRY_POINT";
	case DriverErrorKind::REFUSED_ENVIRONMENT:
		return "REFUSED_ENVIRONMENT";
	}
#pragma GCC diagnostic pop
	return "UNKNOWN"; // a value no enumerator has
}

} // namespace redshift_driver_test
