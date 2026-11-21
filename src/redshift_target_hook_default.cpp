//===----------------------------------------------------------------------===//
// The default of the target hook: every Serverless workgroup needs its ACCOUNT_ID.
//===----------------------------------------------------------------------===//

#include "redshift_target_hook.hpp"

namespace duckdb {

bool AcceptsAccountlessWorkgroup(const string &workgroup) {
	return false;
}

} // namespace duckdb
