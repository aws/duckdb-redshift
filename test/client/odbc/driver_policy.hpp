//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/driver_policy.hpp
//
// The rows of the driver loader's policy files, which the environment cases read, so that a row
// added there is covered here without an edit, or fails a case until it is.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#ifndef REDSHIFT_SOURCE_DIR
#error "REDSHIFT_SOURCE_DIR must name the checkout; test/client/odbc/sources.cmake sets it"
#endif

namespace redshift_test {

//! The lines of third_party/redshift_odbc/policy/`file` that are not comments or blank, each split at
//! white space, a trailing `#` comment left out.
inline std::vector<std::vector<std::string>> PolicyRows(const std::string &file) {
	std::ifstream in(REDSHIFT_SOURCE_DIR "/third_party/redshift_odbc/policy/" + file);
	REQUIRE(in.is_open());
	std::vector<std::vector<std::string>> rows;
	for (std::string line; std::getline(in, line);) {
		std::istringstream fields(line.substr(0, line.find('#')));
		std::vector<std::string> row;
		for (std::string field; fields >> field;) {
			row.push_back(field);
		}
		if (!row.empty()) {
			rows.push_back(row);
		}
	}
	return rows;
}

} // namespace redshift_test
