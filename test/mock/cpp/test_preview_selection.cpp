// The preview build selection compiles one implementation of a hook: the preview where the tree
// carries it, the default otherwise.

#include "catch.hpp"

#include <string>

#include "preview_probe.hpp"

TEST_CASE("the build compiles the preview probe's implementation the tree's shape selects", "[preview]") {
	CHECK(std::string(redshift_test::PreviewProbe()) == REDSHIFT_PROBE_EXPECTED);
}
