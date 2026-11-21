//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// redshift_transport.hpp
//
// The interface between the extension and the client. Everything above it is DuckDB-facing
// (the attach handle and its configuration, redshift_query, type mapping); everything below it
// is the client that talks to Redshift.
//
// The shipped client is the bundled Amazon Redshift ODBC driver. This interface lets the
// client change -- a different driver, a different delivery model -- without the SQL
// surface being rewritten, and without paying for that in performance or accuracy.
//
// The rule in both directions:
//
//   * Nothing above the interface names a driver, a driver handle, or a driver type.
//   * Nothing below it names DuckDB's type system or value representation -- no
//     LogicalType, no Value, no Vector, no DataChunk. Mapping to those is owned
//     above the interface, where the precision rules and the refusals live.
//
// Exceptions are the one shared vocabulary. The client runs in-process inside the
// extension, so it throws DuckDB's exception types rather than a parallel error
// hierarchy that would only be translated at the boundary. A statement Redshift
// refuses to prepare, at Describe or at Execute, is a BinderException: it is the
// user's SQL. A statement Redshift accepted that fails during Execute or NextBatch
// has type ExceptionType::EXECUTOR; match the type, not the ExecutorException class, which
// has no extra_info constructor in the pinned DuckDB. Both keep the session, and neither
// invalidates the DuckDB database. A
// connection or credential failure is an IOException; InvalidInputException is kept for
// invalid local input. The one statement failure that does not keep the session is one after
// which the session cannot be trusted with later statements, though the connection was up (such
// as Redshift's XX000 "Internal Service Error" on a data-sharing write): it keeps the refusal's or
// failure's type, and its extra_info has `redshift_error` set to `session_unusable` beside its
// SQLSTATEs. The client has closed the connection, as for a lost one, and the caller discards it,
// says the session was replaced, and runs nothing again.
//
// Deliberately small. It carries the four things the SQL surface genuinely needs
// from a connection and nothing that either side can decide for itself.
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Configuration
//===--------------------------------------------------------------------===//

//! The kind of Redshift deployment a configuration reaches.
enum class RedshiftTarget : uint8_t {
	//! No target was resolved. Never valid below the interface: a client refuses it with
	//! InvalidInputException before it calls anything, so a resolver that forgets the
	//! target fails closed.
	UNSPECIFIED,
	//! A Redshift Serverless workgroup: `workgroup`, with `account_id`, or without one either
	//! with `endpoint_label` or as one the target hook accepts; and no `cluster`.
	SERVERLESS,
	//! A provisioned Redshift cluster: `cluster`, and no `workgroup` or `account_id`; with
	//! `endpoint_label` and `endpoint_port` for a cluster endpoint, or with neither.
	PROVISIONED
};

//! Resolved connection configuration, assembled above the interface from the
//! ATTACH options and path, and consumed below it.
//!
//! Only customer-facing parameters appear here. Everything else the client needs --
//! the control endpoint, a derived host and port, IAM auth, full TLS verification,
//! ApplicationName, streaming batch size -- is fixed or derived below the interface
//! from these fields, not passed down. The one port field belongs to an endpoint the
//! customer's path named: the URI's port, 0 for a workgroup's default of 5439, or 5439 for a
//! cluster endpoint whose URI gives none.
//!
//! Above the interface, the resolver sets a target and the fields it takes, and never
//! passes UNSPECIFIED once its target resolution is in place. Below it, the client
//! checks the target and the field combination again, validates each value, and
//! refuses a configuration that fails with InvalidInputException before any driver
//! call.
struct RedshiftConfig {
	//! AWS region of the *target* Redshift, not the caller's. Required: it cannot be
	//! inferred from the caller's credentials and has no safe default.
	string region;
	//! Named AWS profile for credential resolution. Empty means the client's default
	//! provider chain. Optional.
	string profile;
	//! The name of the DuckDB `aws` or `s3` secret the AWS credentials come from, which
	//! OpenRedshiftConnection is then given as RedshiftAwsCredentials. Empty otherwise, and empty
	//! whenever `profile` is set. A name, not credential material.
	string aws_secret;
	//! The deployment kind; see RedshiftTarget for the fields each one takes.
	RedshiftTarget target = RedshiftTarget::UNSPECIFIED;
	//! SERVERLESS: the workgroup name. Empty otherwise.
	string workgroup;
	//! SERVERLESS: the 12-digit AWS account ID that owns the workgroup, as text so a
	//! leading zero survives. Empty for a workgroup named without an account (one the target hook
	//! accepts, or one with `endpoint_label`) and for any other target.
	string account_id;
	//! PROVISIONED: the cluster identifier. Empty otherwise.
	string cluster;
	//! The database to connect to. Empty means `dev`. Must be empty for a workgroup the target hook
	//! accepts; a workgroup with `endpoint_label` takes one.
	string database;
	//! The second label of the endpoint host an ATTACH path named, for
	//!   * SERVERLESS with no `account_id`: a workgroup endpoint that carries no account,
	//!     `<workgroup>.<label>.<region>.redshift-serverless.amazonaws.com`, as a `redshift://` URI
	//!     or a long-form workgroup ARN names it;
	//!   * PROVISIONED: a cluster endpoint, `<cluster>.<label>.<region>.redshift.amazonaws.com`, as a
	//!     `redshift://` URI names it. The client then connects to exactly that host and
	//!     `endpoint_port`, rather than looking the cluster up by `cluster`.
	//! Empty otherwise, as for a cluster named by `cluster` alone. Not credential material.
	string endpoint_label;
	//! The port of the endpoint the connection goes to. SERVERLESS: the workgroup endpoint's port, from
	//! a path, 0 meaning 5439; never for a workgroup the target hook accepts. PROVISIONED with
	//! `endpoint_label`: the cluster endpoint's port, from a path, else Redshift's default,
	//! 5439. 0 for a cluster named by `cluster` alone, whose port the driver looks up. Not credential
	//! material.
	int endpoint_port = 0;

	//! Human-readable target, for error messages. Never includes credential material --
	//! no field holds any, which is the point of keeping these the only ones; the AWS
	//! credentials cross separately (RedshiftAwsCredentials). An UNSPECIFIED target adds
	//! nothing to the region and credential source.
	string Describe() const {
		string text = "region " + region;
		if (!profile.empty()) {
			text += ", profile " + profile;
		}
		if (!aws_secret.empty()) {
			text += ", credentials from AWS secret " + aws_secret;
		}
		// No default label, so -Wswitch names a target added to the enum here.
		switch (target) {
		case RedshiftTarget::UNSPECIFIED:
			return text;
		case RedshiftTarget::SERVERLESS:
			text += ", workgroup " + workgroup;
			if (!account_id.empty()) {
				text += ", account " + account_id;
			}
			if (!endpoint_label.empty()) {
				text += ", endpoint label " + endpoint_label;
			}
			if (endpoint_port != 0) {
				text += ", port " + std::to_string(endpoint_port);
			}
			return WithDatabase(text);
		case RedshiftTarget::PROVISIONED:
			text += ", cluster " + cluster;
			if (!endpoint_label.empty()) {
				text += ", endpoint label " + endpoint_label + ", port " + std::to_string(endpoint_port);
			}
			return WithDatabase(text);
		}
		// A value outside the enum is refused below the interface; say so rather than read as a
		// configuration that names no target.
		return WithDatabase(text + ", unknown target");
	}

private:
	//! `text` with the database, if one is given.
	string WithDatabase(string text) const {
		if (!database.empty()) {
			text += ", database " + database;
		}
		return text;
	}
};

//! The AWS credentials of one connection open, read above the interface from the DuckDB `aws`
//! or `s3` secret that RedshiftConfig::aws_secret names, just before the call. Credential
//! material: the client uses them only to build that connection, and never stores, logs or
//! describes them. The ODBC driver keeps its own copy in the connection handle until the
//! connection is freed; driver 2.3.0.0 also kept one in its connection-string cache until the
//! process exited.
struct RedshiftAwsCredentials {
	string access_key_id;
	string secret_access_key;
	//! Empty for long-lived keys.
	string session_token;
};

//===--------------------------------------------------------------------===//
// Result shape
//===--------------------------------------------------------------------===//

//! Transport-neutral column type.
//!
//! Deliberately not a DuckDB LogicalType: the client below the interface must not
//! depend on DuckDB's type system, and the mapping from these to DuckDB types (with
//! its precision rules and its refusals) is owned above the interface.
enum class RedshiftTypeId : uint8_t {
	BOOLEAN,
	SMALLINT,
	INTEGER,
	BIGINT,
	FLOAT,
	DOUBLE,
	DECIMAL,
	VARCHAR,
	//! Redshift's blank-padded CHAR(n), carried as TEXT with the padding the driver sends.
	//! Also every other column the driver names CHARACTER, such as PostgreSQL's one-byte
	//! "char" in catalog tables. Redshift treats a CHAR value's trailing blanks as
	//! insignificant: they are ignored in comparisons, not counted by LENGTH, and removed in a
	//! conversion to another string type. What to do with them is decided above the interface.
	CHAR,
	DATE,
	TIME,
	TIMETZ,
	TIMESTAMP,
	TIMESTAMPTZ,
	BLOB,
	//! The client recognised a column it has no mapping for. Above the interface this
	//! must be refused rather than guessed at, so an unmapped type is never silently
	//! read as text.
	UNKNOWN
};

struct RedshiftColumn {
	string name;
	RedshiftTypeId type = RedshiftTypeId::UNKNOWN;
	//! Meaningful for DECIMAL. Zero otherwise.
	int32_t precision = 0;
	int32_t scale = 0;
};

//! What a statement will return, learned by preparing it *without executing it*.
//!
//! DuckDB needs the result shape before execution in order to plan the surrounding
//! query, which is why this is a separate call from Execute and why it must have no
//! side effects on Redshift.
struct RedshiftStatementShape {
	//! False for a statement that produces no result set (INSERT, SET, CREATE TEMP
	//! TABLE, VACUUM, ...). The SQL surface binds a single `rowcount BIGINT` for those.
	bool returns_rows = false;
	//! Empty when `returns_rows` is false.
	vector<RedshiftColumn> columns;
};

//===--------------------------------------------------------------------===//
// Values and batches
//===--------------------------------------------------------------------===//

//! How a cell is carried across the interface.
//!
//! Tagged rather than text-only, and that is the whole point of the choice. A text-only
//! representation would be simpler, but it would make the interface cost something to
//! cross, in both of the ways the interface is supposed to protect against:
//!
//!   * Performance. A client that already holds a native value -- ODBC hands back a
//!     bound `SQLBIGINT`, ADBC an Arrow buffer -- would have to format it to text only
//!     for the caller to parse it straight back. A double conversion per cell, paid on
//!     every row of every result, to satisfy a boundary rather than a requirement.
//!   * Accuracy. Round-tripping a floating-point value through decimal text is exactly
//!     the kind of conversion that loses a bit and is hard to notice.
//!
//! So native values stay native. What deliberately stays text is anything whose exact
//! value is a decimal string: DECIMAL, DATE, TIME and the TIMESTAMP family. For those,
//! text *is* the lossless form -- a DECIMAL carries up to 38 significant digits, which
//! no double can hold, and DuckDB's own string parsers are the right ones to read the
//! date and time formats. Forcing those into a numeric slot is where precision would
//! actually be lost.
enum class RedshiftValueKind : uint8_t {
	NULL_VALUE,
	BOOLEAN,
	//! int2, int4, int8.
	INTEGER,
	//! float4, float8. Carried as a double so no decimal round-trip occurs.
	REAL,
	//! The value in Redshift's text form. The exact representation for DECIMAL, DATE,
	//! TIME, TIMETZ, TIMESTAMP, TIMESTAMPTZ, VARCHAR and CHAR.
	TEXT,
	//! Raw bytes, not text. Must not reach a textual parser: a VARBYTE containing 0x00
	//! or a literal "\x41" is either rejected or silently reinterpreted by one.
	BLOB
};

struct RedshiftValue {
	RedshiftValueKind kind = RedshiftValueKind::NULL_VALUE;
	bool boolean = false;
	int64_t integer = 0;
	double real = 0;
	//! Holds TEXT or BLOB. Empty otherwise.
	string text;
};

//! A bounded group of rows. The client chooses the batch size.
//!
//! This makes no assumption about how the client obtains the rows -- streaming them from
//! Redshift or buffering internally are both the client's choice. What it requires is
//! only that rows cross in bounded groups, so that a streaming client is never forced to
//! materialise a whole result just to cross the interface.
struct RedshiftBatch {
	//! Each row has one value per column of the statement's shape.
	vector<vector<RedshiftValue>> rows;
};

//===--------------------------------------------------------------------===//
// The interface
//===--------------------------------------------------------------------===//

//! A streaming result. One per executed statement.
class RedshiftResult {
public:
	virtual ~RedshiftResult() = default;

	//! Fill `batch` with the next group of rows. Returns false once drained, at which
	//! point `batch` is left empty. A zero-row batch with a true return is legal.
	virtual bool NextBatch(RedshiftBatch &batch) = 0;

	//! Rows affected, for a statement that produced no result set. Negative when the
	//! client could not obtain a count, which the SQL surface surfaces as NULL.
	virtual int64_t RowCount() const = 0;
};

//! One Redshift connection, carrying one Redshift session.
//!
//! The SQL surface owns one of these per (attach handle, DuckDB connection) pair and
//! calls it from one thread at a time. Implementations therefore need no internal
//! locking, and may assume a single in-flight result per connection.
class RedshiftConnection {
public:
	virtual ~RedshiftConnection() = default;

	//! Learn a statement's result shape without executing it. Must not alter session
	//! state. Throws if Redshift refuses the statement -- including a multi-statement
	//! string, which cannot be prepared.
	virtual RedshiftStatementShape Describe(const string &sql) = 0;

	//! Run one statement. `params` bind positionally to $1..$N as strings; Redshift
	//! casts each to the type its prepared statement infers. Values are never
	//! substituted into the SQL text.
	//!
	//! `expected_shape` is the shape the caller bound, from an earlier Describe of `sql`.
	//! The client prepares the statement and compares its shape with `expected_shape`
	//! before running it: `returns_rows`, and each column's name, type ID, and DECIMAL
	//! precision and scale. On any difference it runs nothing and throws BinderException
	//! whose extra_info has `redshift_error` set to `shape_changed` and no `sqlstate`,
	//! leaving the connection usable and SessionHoldsState() unchanged. This statement has
	//! not run, so the caller can say so, though other statements the caller made may have
	//! run, and a caller that keeps a bound shape must bind again. A change on the server
	//! after the preparation the check reads need not fail the run: Redshift may still run a
	//! prepared statement, with the columns it was prepared with or the changed ones, and a
	//! dropped column can return another column's values with no error. For a statement with
	//! no `params` the client may instead send the text again; the server may then fail it,
	//! or the client compares the result's columns with the checked shape before the first
	//! row and on a difference throws an executor error saying the statement may have run.
	//! Nothing is replayed. When there are no `params` and the last Describe or Execute on
	//! this connection that reached the server was Describe of the same `sql`, less than a
	//! second ago, the client may check the statement that Describe prepared rather than
	//! prepare it again, and then sends the text again. A statement with `params`, or one the
	//! client does not send again, is always prepared again, so its check reads the server's
	//! shape just before it runs.
	virtual unique_ptr<RedshiftResult> Execute(const string &sql, const vector<string> &params,
	                                           const RedshiftStatementShape &expected_shape) = 0;

	//! Whether this connection's Redshift session holds state -- a temp table, a SET
	//! value, a cursor, an open transaction.
	//!
	//! This exists because of the honesty contract on session loss: if the session
	//! ended while holding no state, a fresh connection is equivalent and the user
	//! need not be told; if it held state, the statement must fail loudly rather than
	//! silently run against an empty session. Only the client can know this, and only
	//! the SQL surface can act on it.
	virtual bool SessionHoldsState() const = 0;

	//! Human-readable target, for error messages. Never includes credential material.
	virtual string Target() const = 0;
};

//===--------------------------------------------------------------------===//
// Logging
//===--------------------------------------------------------------------===//

//! Where the client writes its log lines. The SQL surface supplies it, so the client
//! needs no ClientContext: a connection or result can be destroyed, and log, after the
//! DuckDB connection that opened it has closed.
//!
//! The client holds the sink by shared reference wherever it logs, so the sink outlives
//! every connection and result opened with it. It calls the sink from any thread,
//! including its own, and from destructors, so an implementation must be thread-safe.
//! It passes only redacted text -- never a credential, a token or a connection
//! string -- so the sink may write each line as it is.
class RedshiftLogSink {
public:
	virtual ~RedshiftLogSink() = default;

	//! Record one line at debug level. Never throws, because destructors call it; a line
	//! the sink cannot write is dropped. A call can close the DuckDB database on the calling
	//! thread (see CreateRedshiftLogSink), so call it holding no lock that teardown takes.
	virtual void Debug(const string &message) noexcept = 0;
};

//===--------------------------------------------------------------------===//
// Opening a connection
//===--------------------------------------------------------------------===//

//! Open a connection to the configured Redshift. The only place a concrete client is
//! named, so swapping ODBC for libpq or ADBC is a change to one function.
//!
//! The SQL surface calls this at ATTACH, for the attaching DuckDB connection, so that a target or
//! credentials it cannot connect with fail the ATTACH, on another DuckDB connection's first
//! statement for the handle, and on a pair's next statement after its connection was discarded;
//! never at CREATE SECRET. Each call resolves the credentials the driver
//! finds itself (the default chain, a profile) and mints the database token again. AWS credentials
//! from an AWS secret are read just before this call, but were resolved when that secret was created
//! and nothing refreshes them, so they may already have expired; the client then reports a
//! credential failure naming the AWS secret.
//!
//! `credentials` is given exactly when `config.aws_secret` is set; the client refuses either without
//! the other with InvalidInputException before any driver call, and keeps none of it after the call
//! (the driver's own copies aside, see RedshiftAwsCredentials).
//!
//! `log_sink` receives every line the connection and its results log. Never null.
unique_ptr<RedshiftConnection> OpenRedshiftConnection(const RedshiftConfig &config,
                                                      optional_ptr<const RedshiftAwsCredentials> credentials,
                                                      shared_ptr<RedshiftLogSink> log_sink);

//! A connection with no AWS credentials passed: the default chain or a profile.
inline unique_ptr<RedshiftConnection> OpenRedshiftConnection(const RedshiftConfig &config,
                                                             shared_ptr<RedshiftLogSink> log_sink) {
	return OpenRedshiftConnection(config, nullptr, std::move(log_sink));
}

} // namespace duckdb
