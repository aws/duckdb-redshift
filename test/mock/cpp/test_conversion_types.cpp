// Column-type mapping: every RedshiftTypeId, DECIMAL metadata at and beyond DuckDB's limits,
// and the refusals. No connection is involved.

#include "catch.hpp"

#include "duckdb.hpp"
#include "redshift_conversion.hpp"
#include "test_util.hpp"

using namespace duckdb;

namespace {

RedshiftColumn Column(RedshiftTypeId type, int32_t precision = 0, int32_t scale = 0, const string &name = "c") {
	RedshiftColumn column;
	column.name = name;
	column.type = type;
	column.precision = precision;
	column.scale = scale;
	return column;
}

//! The message of the refusal the mapping throws for `column`. Fails the test unless it is a
//! BinderException.
string RefusalMessage(const RedshiftColumn &column, idx_t column_index = 0) {
	auto thrown = redshift_test::Capture([&] { RedshiftTypeMapping::ToLogicalType(column, column_index); });
	REQUIRE(thrown.type == ExceptionType::BINDER);
	return thrown.message;
}

} // namespace

TEST_CASE("every supported type ID maps to exactly its DuckDB type", "[conversion][types]") {
	struct Case {
		RedshiftTypeId id;
		LogicalType expected;
	};
	const Case cases[] = {
	    {RedshiftTypeId::BOOLEAN, LogicalType::BOOLEAN},     {RedshiftTypeId::SMALLINT, LogicalType::SMALLINT},
	    {RedshiftTypeId::INTEGER, LogicalType::INTEGER},     {RedshiftTypeId::BIGINT, LogicalType::BIGINT},
	    {RedshiftTypeId::FLOAT, LogicalType::FLOAT},         {RedshiftTypeId::DOUBLE, LogicalType::DOUBLE},
	    {RedshiftTypeId::VARCHAR, LogicalType::VARCHAR},     {RedshiftTypeId::DATE, LogicalType::DATE},
	    {RedshiftTypeId::TIME, LogicalType::TIME},           {RedshiftTypeId::TIMETZ, LogicalType::TIME_TZ},
	    {RedshiftTypeId::TIMESTAMP, LogicalType::TIMESTAMP}, {RedshiftTypeId::TIMESTAMPTZ, LogicalType::TIMESTAMP_TZ},
	    {RedshiftTypeId::BLOB, LogicalType::BLOB},           {RedshiftTypeId::CHAR, LogicalType::VARCHAR},
	};
	for (auto &c : cases) {
		INFO("expected " << c.expected.ToString());
		REQUIRE(RedshiftTypeMapping::ToLogicalType(Column(c.id), 0) == c.expected);
	}
}

TEST_CASE("DECIMAL keeps its precision and scale up to DuckDB's limits", "[conversion][types]") {
	REQUIRE(DecimalType::MaxWidth() == 38);
	const std::pair<int32_t, int32_t> legal[] = {{1, 0}, {1, 1}, {4, 2}, {9, 0}, {18, 6}, {19, 0}, {38, 0}, {38, 38}};
	for (auto &ps : legal) {
		INFO("DECIMAL(" << ps.first << "," << ps.second << ")");
		auto type = RedshiftTypeMapping::ToLogicalType(Column(RedshiftTypeId::DECIMAL, ps.first, ps.second), 0);
		REQUIRE(type.id() == LogicalTypeId::DECIMAL);
		REQUIRE(DecimalType::GetWidth(type) == ps.first);
		REQUIRE(DecimalType::GetScale(type) == ps.second);
	}
}

TEST_CASE("DECIMAL metadata DuckDB cannot represent is refused, never narrowed", "[conversion][types]") {
	// 300 and 256 would silently wrap if passed to LogicalType::DECIMAL's uint8_t parameters.
	const std::pair<int32_t, int32_t> illegal[] = {{0, 0},   {-1, 0},  {39, 0},  {256, 0}, {300, 2},
	                                               {10, 11}, {10, -1}, {38, 39}, {0, -1}};
	for (auto &ps : illegal) {
		INFO("DECIMAL(" << ps.first << "," << ps.second << ")");
		auto message = RefusalMessage(Column(RedshiftTypeId::DECIMAL, ps.first, ps.second, "amount"), 2);
		REQUIRE_THAT(message, Catch::Contains("column 3 (\"amount\")"));
		REQUIRE_THAT(message, Catch::Contains(StringUtil::Format("DECIMAL(%d,%d)", ps.first, ps.second)));
		REQUIRE_THAT(message, Catch::Contains("No Redshift statement was executed"));
	}

	// The whole message once, so no clause can be dropped unnoticed.
	REQUIRE(RefusalMessage(Column(RedshiftTypeId::DECIMAL, 300, 2, "amount"), 2) ==
	        "Redshift column 3 (\"amount\") is DECIMAL(300,2), which DuckDB cannot represent; DuckDB supports "
	        "precision 1 to 38 and scale 0 to the precision. Cast it to a supported type in the Redshift SQL. No "
	        "Redshift statement was executed.");
}

TEST_CASE("UNKNOWN is refused at bind, naming the column, and never read as VARCHAR", "[conversion][types]") {
	REQUIRE(RefusalMessage(Column(RedshiftTypeId::UNKNOWN, 0, 0, "geom"), 0) ==
	        "Redshift column 1 (\"geom\") has a type this extension does not support. Cast it to a supported type "
	        "in the Redshift SQL. No Redshift statement was executed.");

	// A quote in the name is doubled, as in a SQL identifier.
	REQUIRE_THAT(RefusalMessage(Column(RedshiftTypeId::UNKNOWN, 0, 0, "a\"b"), 0),
	             Catch::Contains("Redshift column 1 (\"a\"\"b\") has a type"));

	// An unnamed column is still identified by position.
	REQUIRE_THAT(RefusalMessage(Column(RedshiftTypeId::UNKNOWN, 0, 0, ""), 4),
	             Catch::Contains("Redshift column 5 has a type"));

	// A value outside the enum is treated as UNKNOWN, not as any mapped type.
	REQUIRE_THAT(RefusalMessage(Column(static_cast<RedshiftTypeId>(200))), Catch::Contains("has a type"));
}

TEST_CASE("precision and scale are ignored for types other than DECIMAL", "[conversion][types]") {
	REQUIRE(RedshiftTypeMapping::ToLogicalType(Column(RedshiftTypeId::VARCHAR, 300, -5), 0) == LogicalType::VARCHAR);
	REQUIRE(RedshiftTypeMapping::ToLogicalType(Column(RedshiftTypeId::BIGINT, 19, 0), 0) == LogicalType::BIGINT);
}

TEST_CASE("a shape maps column by column, and the first refusal names its column", "[conversion][types]") {
	vector<RedshiftColumn> columns {Column(RedshiftTypeId::INTEGER, 0, 0, "id"),
	                                Column(RedshiftTypeId::DECIMAL, 12, 2, "price"),
	                                Column(RedshiftTypeId::TIMESTAMPTZ, 0, 0, "at")};
	auto types = RedshiftTypeMapping::ToLogicalTypes(columns);
	REQUIRE(types.size() == 3);
	REQUIRE(types[0] == LogicalType::INTEGER);
	REQUIRE(types[1] == LogicalType::DECIMAL(12, 2));
	REQUIRE(types[2] == LogicalType::TIMESTAMP_TZ);

	REQUIRE(RedshiftTypeMapping::ToLogicalTypes({}).empty());

	// Two refusing columns: the error names the first and stops there.
	columns.push_back(Column(RedshiftTypeId::UNKNOWN, 0, 0, "shape"));
	columns.push_back(Column(RedshiftTypeId::DECIMAL, 300, 0, "total"));
	auto thrown = redshift_test::Capture([&] { RedshiftTypeMapping::ToLogicalTypes(columns); });
	REQUIRE(thrown.type == ExceptionType::BINDER);
	REQUIRE_THAT(thrown.message, Catch::Contains("column 4 (\"shape\")"));
	REQUIRE_THAT(thrown.message, !Catch::Contains("total"));
}

TEST_CASE("a row shape publishes its mapped columns with their names, in order", "[conversion][types]") {
	RedshiftStatementShape shape;
	shape.returns_rows = true;
	shape.columns = {Column(RedshiftTypeId::BIGINT, 0, 0, "id"), Column(RedshiftTypeId::DECIMAL, 12, 2, "total"),
	                 Column(RedshiftTypeId::VARCHAR, 0, 0, "Name")};
	auto schema = RedshiftTypeMapping::ToOutputSchema(shape, RedshiftCaller("rs"));
	REQUIRE(schema.types ==
	        vector<LogicalType> {LogicalType::BIGINT, LogicalType::DECIMAL(12, 2), LogicalType::VARCHAR});
	REQUIRE(schema.names == vector<Identifier> {Identifier("id"), Identifier("total"), Identifier("Name")});
}

TEST_CASE("a repeated column name is published with DuckDB's suffix; the shape keeps the reported names",
          "[conversion][types]") {
	// As a Redshift join's SELECT * reports: DuckDB refuses a scan whose columns share a name, and
	// compares names without case. A suffix already in use is skipped.
	RedshiftStatementShape shape;
	shape.returns_rows = true;
	shape.columns = {Column(RedshiftTypeId::INTEGER, 0, 0, "id"), Column(RedshiftTypeId::INTEGER, 0, 0, "id_1"),
	                 Column(RedshiftTypeId::INTEGER, 0, 0, "id"), Column(RedshiftTypeId::INTEGER, 0, 0, "ID"),
	                 Column(RedshiftTypeId::INTEGER, 0, 0, "name")};
	auto schema = RedshiftTypeMapping::ToOutputSchema(shape, RedshiftCaller("rs"));
	REQUIRE(schema.names.size() == 5);
	CHECK(schema.names[0].GetIdentifierName() == "id");
	CHECK(schema.names[1].GetIdentifierName() == "id_1");
	CHECK(schema.names[2].GetIdentifierName() == "id_2");
	CHECK(schema.names[3].GetIdentifierName() == "ID_3");
	CHECK(schema.names[4].GetIdentifierName() == "name");
	CHECK(shape.columns[2].name == "id");

	// A later name that a generated one already took is renamed in turn, and an empty name is first
	// given DuckDB's name for its position.
	shape.columns = {Column(RedshiftTypeId::INTEGER, 0, 0, "a"), Column(RedshiftTypeId::INTEGER, 0, 0, "a"),
	                 Column(RedshiftTypeId::INTEGER, 0, 0, "a_1"), Column(RedshiftTypeId::INTEGER, 0, 0, ""),
	                 Column(RedshiftTypeId::INTEGER, 0, 0, "c3")};
	schema = RedshiftTypeMapping::ToOutputSchema(shape, RedshiftCaller("rs"));
	REQUIRE(schema.names.size() == 5);
	CHECK(schema.names[0].GetIdentifierName() == "a");
	CHECK(schema.names[1].GetIdentifierName() == "a_1");
	CHECK(schema.names[2].GetIdentifierName() == "a_1_1");
	CHECK(schema.names[3].GetIdentifierName() == "C3");
	CHECK(schema.names[4].GetIdentifierName() == "c3_1");
}

TEST_CASE("a shape with no result set publishes one rowcount BIGINT", "[conversion][types]") {
	RedshiftStatementShape shape;
	shape.returns_rows = false;
	auto schema = RedshiftTypeMapping::ToOutputSchema(shape, RedshiftCaller("rs"));
	REQUIRE(schema.types == vector<LogicalType> {LogicalType::BIGINT});
	REQUIRE(schema.names == vector<Identifier> {Identifier("rowcount")});
}

TEST_CASE("a shape whose flag and columns disagree is refused at bind, naming the handle", "[conversion][types]") {
	// No correct client reports either shape, so no mock directive can produce one;
	// the refusal is pinned here instead.
	RedshiftStatementShape rows_without_columns;
	rows_without_columns.returns_rows = true;
	auto thrown = redshift_test::Capture(
	    [&] { RedshiftTypeMapping::ToOutputSchema(rows_without_columns, RedshiftCaller("My RS")); });
	REQUIRE(thrown.type == ExceptionType::BINDER);
	REQUIRE(thrown.message == "redshift_query: the Redshift client described the statement on handle \"My RS\" as "
	                          "returning rows with no columns, which is not a valid result shape. This is a fault in "
	                          "the client, not in the SQL. No Redshift statement was executed.");

	RedshiftStatementShape no_rows_with_columns;
	no_rows_with_columns.returns_rows = false;
	no_rows_with_columns.columns = {Column(RedshiftTypeId::INTEGER), Column(RedshiftTypeId::INTEGER)};
	thrown = redshift_test::Capture(
	    [&] { RedshiftTypeMapping::ToOutputSchema(no_rows_with_columns, RedshiftCaller("rs")); });
	REQUIRE(thrown.type == ExceptionType::BINDER);
	REQUIRE(thrown.message == "redshift_query: the Redshift client described the statement on handle \"rs\" as "
	                          "returning no rows with 2 columns, which is not a valid result shape. This is a fault "
	                          "in the client, not in the SQL. No Redshift statement was executed.");
}

TEST_CASE("an output schema passes a column refusal through", "[conversion][types]") {
	RedshiftStatementShape shape;
	shape.returns_rows = true;
	shape.columns = {Column(RedshiftTypeId::INTEGER, 0, 0, "a"), Column(RedshiftTypeId::UNKNOWN, 0, 0, "b")};
	auto thrown = redshift_test::Capture([&] { RedshiftTypeMapping::ToOutputSchema(shape, RedshiftCaller("rs")); });
	REQUIRE(thrown.type == ExceptionType::BINDER);
	REQUIRE(thrown.message == RefusalMessage(shape.columns[1], 1));
}
