// The attach handle: what ATTACH stores on its RedshiftCatalog, which no SQL shows directly. No
// connection is involved.

#include "catch.hpp"

#include "duckdb.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/storage/storage_extension.hpp"
#include "redshift_attach.hpp"
#include "redshift_extension.hpp"
#include "storage/redshift_catalog.hpp"
#include "test_util.hpp"

using namespace duckdb;

namespace {

//! An in-memory database with the extension loaded. Persistent secrets are off, so a secret stored
//! in ~/.duckdb/stored_secrets never reaches a test.
struct Fixture {
	DuckDB db {nullptr, redshift_test::NoPersistentSecrets().get()};
	Connection con {db};

	Fixture() {
		db.LoadStaticExtension<RedshiftExtension>();
	}

	void Run(const string &sql) {
		auto result = con.Query(sql);
		INFO(sql << ": " << (result->HasError() ? result->GetError() : ""));
		REQUIRE_FALSE(result->HasError());
	}

	//! Attaches handle `rs` with an empty path and the configuration `options`. `verb` is ATTACH, ATTACH
	//! OR REPLACE or ATTACH IF NOT EXISTS.
	void Attach(const string &verb, const string &options) {
		Run(StringUtil::Format("%s '' AS rs (TYPE %s, %s)", verb, REDSHIFT_ATTACH_TYPE, options));
	}

	//! The configuration the handle named `handle` holds.
	RedshiftConfig HandleConfig(const string &handle) {
		RedshiftConfig config;
		con.context->RunFunctionInTransaction([&] {
			auto db = DatabaseManager::Get(*con.context).GetDatabase(*con.context, Identifier(handle));
			REQUIRE(db);
			config = db->GetCatalog().Cast<RedshiftCatalog>().Config();
		});
		return config;
	}
};

} // namespace

TEST_CASE("ATTACH OR REPLACE resolves the configuration again", "[attach][catalog]") {
	Fixture f;
	const string first = "REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain, WORKGROUP 'wg', ACCOUNT_ID "
	                     "'012345678901'";

	f.Attach("ATTACH", first);
	auto config = f.HandleConfig("rs");
	REQUIRE(config.region == "us-east-1");
	REQUIRE(config.profile.empty());
	REQUIRE(config.target == RedshiftTarget::SERVERLESS);
	REQUIRE(config.workgroup == "wg");

	// Another configuration.
	f.Attach("ATTACH OR REPLACE",
	         "REGION 'eu-west-1', CREDENTIAL_PROVIDER profile, PROFILE 'p2', CLUSTER 'c2', DATABASE 'd2'");
	config = f.HandleConfig("rs");
	REQUIRE(config.region == "eu-west-1");
	REQUIRE(config.profile == "p2");
	REQUIRE(config.target == RedshiftTarget::PROVISIONED);
	REQUIRE(config.cluster == "c2");
	REQUIRE(config.database == "d2");

	// Each replace resolves its own options; nothing of the previous handle's carries over.
	f.Attach("ATTACH OR REPLACE", "REGION 'ap-south-1', CREDENTIAL_PROVIDER aws_default_chain, CLUSTER 'c3'");
	config = f.HandleConfig("rs");
	REQUIRE(config.region == "ap-south-1");
	REQUIRE(config.profile.empty());
	REQUIRE(config.cluster == "c3");
	REQUIRE(config.database.empty());

	// IF NOT EXISTS keeps the existing handle and its configuration.
	f.Attach("ATTACH IF NOT EXISTS", first);
	REQUIRE(f.HandleConfig("rs").region == "ap-south-1");
}

TEST_CASE("an ATTACH that names the type loads the extension of the same name", "[attach]") {
	// DuckDB loads the extension named after an unregistered storage type before the attach
	// (DatabaseManager::GetDatabaseType), so an ATTACH ... (TYPE redshift) with its configuration as
	// options needs no LOAD while the aws extension is not loaded. Here the extension is linked in;
	// INSTALLed, it is the file of that name.
	auto config = redshift_test::NoPersistentSecrets();
	config->options.load_extensions = false;
	DuckDB db(nullptr, config.get());
	Connection con(db);
	REQUIRE_FALSE(db.ExtensionIsLoaded("redshift"));
	redshift_test::Run(con, redshift_test::Attach("rs"));
	CHECK(db.ExtensionIsLoaded("redshift"));
}

namespace {

//! Stands in for another extension's storage type of the same name, as duckdb-aws registers TYPE redshift.
unique_ptr<Catalog> StandInAttach(optional_ptr<StorageExtensionInfo>, ClientContext &, AttachedDatabase &,
                                  const string &, AttachInfo &, AttachOptions &) {
	throw InvalidInputException("the stand-in storage type was reached");
}

unique_ptr<TransactionManager> StandInTransactionManager(optional_ptr<StorageExtensionInfo>, AttachedDatabase &,
                                                         Catalog &) {
	throw InternalException("the stand-in's attach always throws");
}

void RegisterStandIn(DuckDB &db) {
	auto stand_in = make_shared_ptr<StorageExtension>();
	stand_in->attach = StandInAttach;
	// DuckDB calls a storage extension's attach only when it has both functions (CreateAttachedDatabase).
	stand_in->create_transaction_manager = StandInTransactionManager;
	StorageExtension::Register(DBConfig::GetConfig(*db.instance), REDSHIFT_ATTACH_TYPE, std::move(stand_in));
}

} // namespace

TEST_CASE("another extension's storage type of the same name belongs to the one registered last", "[attach]") {
	// DuckDB keeps one storage extension per name, and a registration replaces the one before it
	// (ExtensionCallbackManager::Register), so a type registered after this extension takes it; a
	// second load of this one does nothing (ExtensionManager::BeginLoad). The aws extension (duckdb-aws
	// 1b96925) registers its TYPE redshift stand-in only while nothing serves the type, so it never does.
	auto config = redshift_test::NoPersistentSecrets();
	config->options.load_extensions = false;
	DuckDB db(nullptr, config.get());
	Connection con(db);
	SECTION("registered before this extension loads, the other type is replaced") {
		RegisterStandIn(db);
		auto before = con.Query(redshift_test::Attach("rs"));
		REQUIRE(before->HasError());
		CHECK_THAT(before->GetError(), Catch::Contains("the stand-in storage type was reached"));
		db.LoadStaticExtension<RedshiftExtension>();
		redshift_test::Run(con, redshift_test::Attach("rs"));
	}
	SECTION("registered after, the other type takes it, and loading this extension again does not take it back") {
		db.LoadStaticExtension<RedshiftExtension>();
		RegisterStandIn(db);
		db.LoadStaticExtension<RedshiftExtension>();
		auto result = con.Query(redshift_test::Attach("rs"));
		REQUIRE(result->HasError());
		CHECK_THAT(result->GetError(), Catch::Contains("the stand-in storage type was reached"));
	}
}

TEST_CASE("a statically linked extension in a DuckDB started with external access off refuses a new handle",
          "[attach]") {
	// DuckDB refuses to load a loadable extension while external access is off, but loads a linked one
	// whatever the setting, so the attach check is reached with no run-time SET.
	auto config = redshift_test::NoPersistentSecrets();
	config->SetOptionByName("enable_external_access", Value::BOOLEAN(false));
	DuckDB db(nullptr, config.get());
	db.LoadStaticExtension<RedshiftExtension>();
	Connection con(db);
	auto result = con.Query(redshift_test::Attach("rs"));
	REQUIRE(result->HasError());
	CHECK(result->GetErrorType() == ExceptionType::PERMISSION);
	CHECK_THAT(result->GetError(), Catch::Contains("Cannot attach rs: the host disallows external access"));
}
