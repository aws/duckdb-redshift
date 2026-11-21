//===----------------------------------------------------------------------===//
//                         DuckDB - Redshift
//
// test/client/odbc/settings_shape.hpp
//
// Which accountless-workgroup settings the configure says the build compiles, for the cases whose
// outcome depends on it: the public build's refusal, or a preview implementation's. A case the
// build cannot run is hidden with Catch2's "[.]" tag, not passed, and tagged apart from "[odbc]", so
// a run selecting "[odbc]" leaves it hidden too; a case run in both shapes checks the selection.
//===----------------------------------------------------------------------===//

#pragma once

#ifndef REDSHIFT_TEST_ACCOUNTLESS_PREVIEW
#error "test/client/odbc/sources.cmake defines REDSHIFT_TEST_ACCOUNTLESS_PREVIEW for each file that includes this"
#endif

namespace redshift_test {

//! The tags of a case that checks the public build's refusal, which the preview replaces.
constexpr const char *PUBLIC_SETTINGS_TAGS = REDSHIFT_TEST_ACCOUNTLESS_PREVIEW ? "[.][odbc_other_settings]" : "[odbc]";

} // namespace redshift_test
