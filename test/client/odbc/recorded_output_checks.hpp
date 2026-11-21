//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/recorded_output_checks.hpp
//
// How a recorded row should reach the caller, and through which protocol, shared by the offline cases
// that replay it through the fake and the [live] cases that read it through the client.
//===----------------------------------------------------------------------===//

#pragma once

#include "catch.hpp"
#include "odbc/odbc_connection.hpp"
#include "odbc/type_classification.hpp"
#include "recorded_driver_output.hpp"
#include "redshift_transport.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace redshift_test {

//! The recorded and stored-only selects whose rows no path returns: a column the client has no
//! carrier for, or a timestamp infinity, which each path either refuses or sends as a word outside
//! its text form.
const std::set<std::string> UNREADABLE {"infinities", "qualified_intervals", "spatial", "hllsketch",
                                        "negative_infinity"};

//! The select named `name` in `selects`; the case fails when there is none, so it never checks nothing.
inline const RecordedSelect &FindSelect(const std::string &name,
                                        const std::vector<RecordedSelect> &selects = RecordedSelects()) {
	const auto found =
	    std::find_if(selects.begin(), selects.end(), [&](const RecordedSelect &select) { return select.name == name; });
	REQUIRE(found != selects.end());
	return *found;
}

//! `columns` as the client classifies them.
inline duckdb::vector<duckdb::RedshiftColumn>
Classified(const std::vector<duckdb::redshift_driver::testing::FakeColumn> &columns) {
	duckdb::vector<duckdb::RedshiftColumn> classified;
	for (const auto &column : columns) {
		classified.push_back(duckdb::ClassifyColumn(
		    {column.name, column.type_name, column.sql_type, column.column_size, column.decimal_digits}));
	}
	return classified;
}

//! Whether the client runs a statement whose result has `columns` with SQLExecDirect, by the client's
//! own rule, so these cases follow any change to it.
inline bool RunsDirect(const std::vector<duckdb::redshift_driver::testing::FakeColumn> &columns) {
	duckdb::vector<SQLULEN> sizes;
	for (const auto &column : columns) {
		sizes.push_back(column.column_size);
	}
	return duckdb::SimpleProtocolKeepsText(Classified(columns), sizes);
}

//! Whether `value` is what the client should carry for `cell`, the driver's text for `column`: the
//! text or bytes unchanged, or the number the C library reads from the text.
inline bool CarriesRecordedCell(const duckdb::RedshiftColumn &column, const std::string &cell,
                                const duckdb::RedshiftValue &value) {
	using duckdb::RedshiftTypeId;
	using duckdb::RedshiftValueKind;
	switch (column.type) {
	case RedshiftTypeId::BOOLEAN:
		return value.kind == RedshiftValueKind::BOOLEAN && value.boolean == (cell == "1");
	case RedshiftTypeId::SMALLINT:
	case RedshiftTypeId::INTEGER:
	case RedshiftTypeId::BIGINT:
		return value.kind == RedshiftValueKind::INTEGER && value.integer == std::stoll(cell);
	case RedshiftTypeId::FLOAT:
	case RedshiftTypeId::DOUBLE: {
		const double expected = column.type == RedshiftTypeId::FLOAT ? std::strtof(cell.c_str(), nullptr)
		                                                             : std::strtod(cell.c_str(), nullptr);
		if (std::isnan(expected)) {
			return value.kind == RedshiftValueKind::REAL && std::isnan(value.real);
		}
		return value.kind == RedshiftValueKind::REAL && std::memcmp(&expected, &value.real, sizeof(double)) == 0;
	}
	case RedshiftTypeId::BLOB:
		return value.kind == RedshiftValueKind::BLOB && value.text == cell;
	default:
		return value.kind == RedshiftValueKind::TEXT && value.text == cell;
	}
}

} // namespace redshift_test
