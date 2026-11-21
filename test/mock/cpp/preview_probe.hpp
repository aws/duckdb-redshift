// A test-only hook for the preview build selection (CMakeLists.txt, redshift_select_hook). Its
// default is test/mock/cpp/preview_probe_default.cpp; the build compiles exactly one implementation.

#pragma once

namespace redshift_test {

//! "default" or "preview", naming the implementation this build compiled.
const char *PreviewProbe();

} // namespace redshift_test
