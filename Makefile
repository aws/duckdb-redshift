PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=redshift
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# Client selection. REDSHIFT_CLIENT picks the client compiled into the extension (mock
# by default). REDSHIFT_TEST_CLIENT names the client a test target
# built, never a caller's choice, so a test run always selects the tests written for that
# client: the require-env line at the top of each .test file does the selecting. The pinned
# test targets take REDSHIFT_CLIENT; the suite targets below set their own.
# `override` keeps the flag when a caller passes its own EXT_FLAGS on the command line.
REDSHIFT_CLIENT ?= mock
override REDSHIFT_TEST_CLIENT := $(REDSHIFT_CLIENT)
export REDSHIFT_TEST_CLIENT
override EXT_FLAGS += -DREDSHIFT_CLIENT=$(REDSHIFT_CLIENT)
# Which driver an ODBC build carries: embedded (the official release) or none (no download;
# the offline client tests). See CMakeLists.txt.
REDSHIFT_DRIVER ?= embedded
override EXT_FLAGS += -DREDSHIFT_DRIVER=$(REDSHIFT_DRIVER)
# A tree compiles the preview implementations exactly where it carries their directory
# (CMakeLists.txt), with no setting, and the suite targets run the preview tests where it carries
# theirs. The public tree carries neither, so it builds and tests the public shape. A tree that
# carries them tests that shape from PUBLIC_TREE, a copy of itself without them, with its own build
# directory: test_mock and test_client run there first and then here, and
# `make public_shape PUBLIC_TARGET=<target>` runs any target there.
PREVIEW_DIRS := $(wildcard $(PROJ_DIR)src/preview $(PROJ_DIR)test/preview)
PREVIEW_TESTS = $(if $(wildcard $(PROJ_DIR)test/preview),$(comma)$(TESTS_BASE_DIRECTORY)preview/*)
# Where the public shape's copy is made.
PUBLIC_TREE = build/public-shape/tree
comma := ,
# The variable that selects the end-to-end SQL tests (test/client/driver/sql/) is run-e2e.sh's alone, which
# sets it on the unit tester's command line under `env -i`, in a process with no network. Here it is cleared
# and not exported, whatever the caller's environment holds: a DUCKDB_REDSHIFT_TEST_E2E left set in a shell
# would otherwise select one of those files in a pinned test run (make test REDSHIFT_CLIENT=odbc), where
# every connect then refuses it and the file fails instead of skipping. Each of
# those files that attaches also selects itself with require-env DUCKDB_REDSHIFT_TEST_E2E, run-e2e.sh's fake
# keys and no AWS_SESSION_TOKEN; test/client/driver/e2e-selection_test.sh checks them.
override DUCKDB_REDSHIFT_TEST_E2E :=
unexport DUCKDB_REDSHIFT_TEST_E2E

# extension-ci-tools v2.0 runs the SQL tests through DuckDB's scripts/ci/run_tests.py when it
# exists, which in CI retries a failing batch and counts it as passed if a retry passes. The tests
# here run the unit tester directly, as on v1.5, so a test that fails once fails the run.
override TEST_RUNNER_SCRIPT := $(PROJ_DIR)no-test-runner

# A git hook or `git rebase -x` exports GIT_DIR and its kin for this repository, which a
# git run in duckdb/ would read instead of duckdb/'s own. This Makefile's git runs in duckdb/ drop
# them first, as scripts/clone-duckdb.sh does: the variables `git rev-parse --local-env-vars` lists,
# but for the caller's -c settings.
DROP_GIT_ENV = for v in $$(git rev-parse --local-env-vars 2>/dev/null); do [ $$v = GIT_CONFIG_PARAMETERS ] || [ $$v = GIT_CONFIG_COUNT ] || unset $$v; done

# DuckDB v2.0 stamps a build that is not given its version with the commit hash, and a stock DuckDB
# v2.0.1 then refuses to LOAD the extension. Take the version from the tag of the DuckDB tree in
# duckdb/ when it has one (`make duckdb` fetches it); DuckDB's CI sets its own.
OVERRIDE_GIT_DESCRIBE ?= $(shell $(DROP_GIT_ENV); git -C $(PROJ_DIR)duckdb describe --tags --exact-match 2>/dev/null)

# REDSHIFT_TEST_ENDPOINTS lets an ODBC build's test variables replace the endpoints it derives, so
# EXT_FLAGS passes it off whatever the command line sets. A recipe's later flags can still turn it
# on; src/transport/odbc/sources.cmake then fails the configure in any build directory but
# build/driver-e2e.
override EXT_FLAGS += -DREDSHIFT_TEST_ENDPOINTS=OFF

# The one Makefile the build uses from DuckDB's extension-ci-tools, vendored at the commit its first
# line names (third_party/extension-ci-tools/README.md).
include third_party/extension-ci-tools/duckdb_extension.Makefile

# The hooks below add prerequisites to the vendored Makefile's test_release, test_debug
# and test_reldebug, which run the tests through its RUN_TEST macro (v2.0) and do nothing when
# SKIP_TESTS=1. Fail loudly if a later version changes that, rather than letting the lint and
# client checks stop running.
ifndef RUN_TEST
$(error extension-ci-tools no longer runs the tests through RUN_TEST; update the test hooks below)
endif
ifneq ($(strip $(TEST_RUNNER)),)
$(error extension-ci-tools still runs the tests through $(TEST_RUNNER); update the TEST_RUNNER_SCRIPT override above)
endif

# DuckDB at the commit duckdb.commit pins, cloned into duckdb/, the tree every build target compiles
# against (DUCKDB_SRCDIR). No build target clones it, so no build reaches the network for DuckDB: a
# build from source runs `make duckdb` once first. A duckdb/ that is already there, such as a link to
# a DuckDB tree of your own, is accepted at that commit and refused at any other
# (scripts/clone-duckdb.sh). Phony, so that make checks an existing duckdb/ rather than taking it as
# up to date. test_clone_duckdb is the script's fixture test, with the checks that the tree carries
# no submodule and the vendored Makefile is unmodified.
.PHONY: duckdb test_clone_duckdb
duckdb:
	bash $(PROJ_DIR)scripts/clone-duckdb.sh $(PROJ_DIR)

test_clone_duckdb:
	bash $(PROJ_DIR)test/clone-duckdb_test.sh

# Builds compile duckdb/ as it is, so warn when it is a clone of its own (git's prefix there is
# empty, as it is not for a directory inside this repository's work tree) at another commit than
# duckdb.commit pins, as after a pull that moves the pin. A warning, not a refusal, so that a build
# stays offline and a tree moved on purpose still builds; once, not in every sub-make.
ifeq ($(MAKELEVEL),0)
DUCKDB_AT := $(shell $(DROP_GIT_ENV); cd $(PROJ_DIR)duckdb 2>/dev/null && [ -z "$$(git rev-parse --show-prefix 2>/dev/null || echo none)" ] && git rev-parse -q --verify HEAD)
ifneq ($(DUCKDB_AT),)
ifneq ($(DUCKDB_AT),$(shell cat $(PROJ_DIR)duckdb.commit 2>/dev/null))
$(warning duckdb/ is at $(DUCKDB_AT), but duckdb.commit pins $(shell cat $(PROJ_DIR)duckdb.commit); run make duckdb, which says how to fix it)
endif
endif
endif

# Test selection, as described in test/README.md.
#
# The pinned test targets above run the unit tester on "test/*" over an existing build. They do not
# build first: DuckDB's distribution CI builds and tests in separate steps with different
# flags, and a rebuild in the test step would replace the artifact it uploads. Instead, refuse
# a build configured for another client, and run the guard lint first: a .test file without
# its guard would otherwise run against whichever client happened to be built.
.PHONY: public_shape test_export_public_shape lint_tests test_lint_guards test_run_selected test_graph_check check_no_network test_mock test_mock_shape test_mock_loadable test_mock_tsan test_tsan_own_reports test_client test_client_shape test_client_tsan test_driver tsan driver_e2e test_client_e2e tidy_client tidy_top_level tidy_driver
# Like the tests themselves, the checks are skipped when SKIP_TESTS=1, as on hosts the pinned
# Makefile does not test on.
ifneq ($(SKIP_TESTS),1)
test_release: lint_tests check_built_client_release
test_debug: lint_tests check_built_client_debug
test_reldebug: lint_tests check_built_client_reldebug
endif

# Trailing whitespace is allowed because CMake on Windows ends the cache's lines with CRLF. Each
# shape has its own tree and so its own build directory, which never holds the other shape.
check_built_client_%:
	@grep -qs '^REDSHIFT_CLIENT:STRING=$(REDSHIFT_CLIENT)[[:space:]]*$$' build/$*/CMakeCache.txt || { \
	  echo "build/$* is not a REDSHIFT_CLIENT=$(REDSHIFT_CLIENT) build; run make $* REDSHIFT_CLIENT=$(REDSHIFT_CLIENT) first" >&2; \
	  exit 1; }

# The copy script's fixture test, with a check of the plan below, where the tree carries the script;
# keyed on the script, so a tree that has it but has lost the test still fails.
EXPORT_PUBLIC_SHAPE := $(wildcard $(PROJ_DIR)test/export-public-shape.sh)
test_export_public_shape:
ifneq ($(EXPORT_PUBLIC_SHAPE),)
	bash $(PROJ_DIR)test/export-public-shape_test.sh
else
	@echo "test_export_public_shape: SKIP: test/export-public-shape.sh is not in this tree"
endif

# Runs PUBLIC_TARGET in the public shape: in PUBLIC_TREE, copied afresh, where this tree carries a
# preview directory, and here where it carries none. Where it carries one, the copy script's test
# runs first. The copy's first build is a full one; later ones rebuild what changed. The + marks the
# lines a dry run still runs: the copy, so that the sub-make's dry run has a tree, and the sub-make,
# which shares the jobserver.
public_shape:
ifneq ($(PREVIEW_DIRS),)
	$(MAKE) test_export_public_shape
	+bash $(PROJ_DIR)test/export-public-shape.sh $(PROJ_DIR)
	+$(MAKE) -C $(PUBLIC_TREE) $(PUBLIC_TARGET)
else
	+$(MAKE) $(PUBLIC_TARGET)
endif

lint_tests:
	bash $(PROJ_DIR)test/lint-test-guards.sh

test_lint_guards:
	bash $(PROJ_DIR)test/lint-test-guards_test.sh

test_run_selected:
	bash $(PROJ_DIR)test/run-selected_test.sh

# The read-path benchmark's fixture test, where the tree carries the benchmark.
PERF_DIR := $(wildcard $(PROJ_DIR)test/client/perf)
test_graph_check:
	bash $(PROJ_DIR)test/client/odbc/check-build-graph-offline_test.sh
	bash $(PROJ_DIR)test/client/odbc/check-no-network_test.sh
	bash $(PROJ_DIR)test/client/odbc/check-no-test-hooks_test.sh
ifneq ($(PERF_DIR),)
	bash $(PROJ_DIR)test/client/perf/transfer_benchmark_test.sh
else
	@echo "test_graph_check: SKIP: test/client/perf/ is not in this tree"
endif

# Runs the unit tester on one directory and refuses a run in which nothing executed, so a
# target cannot pass by skipping every file.
RUN_SELECTED = bash $(PROJ_DIR)test/run-selected.sh

# The suite targets build the client they test, whatever REDSHIFT_CLIENT is set to.
test_mock test_mock_shape test_mock_loadable test_mock_tsan: override REDSHIFT_TEST_CLIENT := mock
test_client test_client_shape test_client_tsan test_driver: override REDSHIFT_TEST_CLIENT := odbc

# Runs `$(1)` in the public shape (public_shape) and then, where this tree carries a preview
# directory, here in the full tree's, and stops at the first that fails. The full tree goes last, so
# build/release is left in its shape, which `make test` and the pinned targets then find.
# $(call each_preview_shape,<target>)
# The + marks it a recursive make, so the sub-makes share the jobserver.
each_preview_shape = +@echo "==> $(1) in the public shape"; $(MAKE) public_shape PUBLIC_TARGET=$(1) && \
	{ [ -z "$(PREVIEW_DIRS)" ] || { echo "==> $(1) in the full tree"; $(MAKE) $(1); }; }

# The C++ suites are EXCLUDE_FROM_ALL, so a suite target builds the ones it runs by name: the
# release build must not compile or link a test executable. A suite whose target has gone fails
# that build command, so an area cannot pass by quietly losing its executable. `-w NoTests`
# then makes Catch2 exit 2 when no case ran (catch.hpp:13817-13818), which is what run-selected.sh
# does for the SQL tests; a suite whose areas are all empty cannot carry it.
# $(call build_suite,<build directory>,<CMake config>,<target>)
build_suite = cmake --build build/$(1) --config $(2) --target $(3)

# The extension's C++ suite and its SQL tests, against the shared mock client, in each preview shape.
# Needs no driver or network. test_mock_shape runs them in this tree's shape alone.
test_mock:
	$(call each_preview_shape,test_mock_shape)

test_mock_shape: lint_tests
	$(MAKE) release REDSHIFT_CLIENT=mock
	$(MAKE) check_built_client_release REDSHIFT_CLIENT=mock
	$(call build_suite,release,Release,redshift_mock_tests)
	./build/release/extension/$(EXT_NAME)/redshift_mock_tests -w NoTests
	$(RUN_SELECTED) ./build/release/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)mock/*$(PREVIEW_TESTS)"

# The extension's C++ suite on a thread-sanitized mock build, once, in this tree's shape, for its
# race and lifetime cases. Its build directory, build/tsan_mock/ (TSAN_MOCK_BUILD), is its own,
# because test_client_tsan's build/tsan/ caches the ODBC client; otherwise it configures as the
# tsan target below does. The engine's suppression list as test_client_tsan passes it. DuckDB's own
# ThreadSanitizer CI runs only some of its tests (duckdb/Makefile, unittest_threadsan), so that list
# does not cover the executor reports the mock suite's streams reach. The suite therefore runs with
# exitcode=0 and its reports in log files (print_suppressions=0, TSan's default, keeps the matched
# suppressions out of them), and test/mock/tsan-own-reports.sh decides the outcome:
# it fails on a failed or crashed suite, on any other ThreadSanitizer error, and on a report in this
# repository's code; test_tsan_own_reports is its fixture test. The suite's exit status follows its
# output, since tee's is the pipeline's: an abort after Catch's summary, in static teardown, leaves
# no ThreadSanitizer line.
TSAN_MOCK_BUILD ?= tsan_mock
test_tsan_own_reports:
	bash $(PROJ_DIR)test/mock/tsan-own-reports_test.sh

test_mock_tsan: ${EXTENSION_CONFIG_STEP} lint_tests test_tsan_own_reports
	mkdir -p build/$(TSAN_MOCK_BUILD)
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DENABLE_THREAD_SANITIZER=1 -DCMAKE_BUILD_TYPE=RelWithDebInfo -DREDSHIFT_CLIENT=mock -S $(DUCKDB_SRCDIR) -B build/$(TSAN_MOCK_BUILD)
	$(call build_suite,$(TSAN_MOCK_BUILD),RelWithDebInfo,redshift_mock_tests)
	rm -f build/$(TSAN_MOCK_BUILD)/tsan-report.*
	{ status=0; TSAN_OPTIONS="suppressions=$(PROJ_DIR)duckdb/.sanitizer-thread-suppressions.txt print_suppressions=0 exitcode=0 log_path=$(PROJ_DIR)build/$(TSAN_MOCK_BUILD)/tsan-report" ./build/$(TSAN_MOCK_BUILD)/extension/$(EXT_NAME)/redshift_mock_tests -w NoTests 2>&1 || status=$$?; echo "tsan suite exit status: $$status"; } | tee build/$(TSAN_MOCK_BUILD)/suite.out
	bash $(PROJ_DIR)test/mock/tsan-own-reports.sh $(PROJ_DIR)build/$(TSAN_MOCK_BUILD)/tsan-report $(PROJ_DIR) build/$(TSAN_MOCK_BUILD)/suite.out

# The extension's SQL tests against the mock build's loadable extension, which test_mock never loads: it
# links the extension into the unit tester, so `require redshift` finds it built in. This configures
# a second DuckDB build, build/loadable_host, as the pinned release recipe does
# (third_party/extension-ci-tools/duckdb_extension.Makefile, target release) but without this
# repository's extension configuration, so it links DuckDB's own extensions and not this one. Its shell installs
# build/release's redshift.duckdb_extension into the build's local extension repository, from which
# the unit tester's `require redshift` loads it (sqllogic_test_runner.cpp, LoadExtension), and
# test/mock/loadable.test, run only here, checks that it did. Before that, the shell runs an ATTACH
# that names the type with no LOAD, which loads the installed file because it has the type's name.
# It runs once, in this tree's shape: the copy refusal it checks does not differ between the
# shapes.
LOADABLE_HOST = build/loadable_host
test_mock_loadable: lint_tests
	$(MAKE) release REDSHIFT_CLIENT=mock
	cmake $(GENERATOR) $(filter-out -DDUCKDB_EXTENSION_CONFIGS=%,$(BUILD_FLAGS)) -DDUCKDB_EXTENSION_CONFIGS= $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Release -S $(DUCKDB_SRCDIR) -B $(LOADABLE_HOST)
	cmake --build $(LOADABLE_HOST) --config Release --target unittest shell
	rm -rf $(LOADABLE_HOST)/repository
	./$(LOADABLE_HOST)/duckdb -unsigned -c "SET extension_directory = '$(LOADABLE_HOST)/repository'; INSTALL 'build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension'"
	./$(LOADABLE_HOST)/duckdb -unsigned -c "SET extension_directory = '$(LOADABLE_HOST)/repository'; ATTACH '' AS rs (TYPE redshift, REGION 'us-east-1', CREDENTIAL_PROVIDER aws_default_chain, CLUSTER 'c')"
	REDSHIFT_TEST_LOADABLE=1 $(RUN_SELECTED) ./$(LOADABLE_HOST)/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)mock/*$(PREVIEW_TESTS)"

# A hand copy of the pinned reldebug recipe
# (third_party/extension-ci-tools/duckdb_extension.Makefile:190-193) with the flag its THREADSAN=1 adds (lines 154-156), which is
# how DuckDB's own threadsan job builds (THREADSAN=1 and `make reldebug`, duckdb/.github/workflows/
# Main.yml:1299-1307). Check it against those lines when the vendored Makefile moves. The build
# directory is this recipe's own, not reldebug's, because CMake caches the sanitizer flag there
# and an unsanitized reldebug build must stay available. RelWithDebInfo, as that job builds, so
# that reports carry line numbers.
tsan: ${EXTENSION_CONFIG_STEP}
	mkdir -p build/tsan
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DENABLE_THREAD_SANITIZER=1 -DCMAKE_BUILD_TYPE=RelWithDebInfo -S $(DUCKDB_SRCDIR) -B build/tsan
	cmake --build build/tsan --config RelWithDebInfo

# The end-to-end test build, never shipped: a hand copy of the pinned release recipe
# (third_party/extension-ci-tools/duckdb_extension.Makefile, target release) in build/driver-e2e, with
# the ODBC client and the switch on after BUILD_FLAGS so they win. Check it when the pin moves.
driver_e2e: ${EXTENSION_CONFIG_STEP}
	mkdir -p build/driver-e2e
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_RELEASE_FLAGS) $(VCPKG_MANIFEST_FLAGS) -DCMAKE_BUILD_TYPE=Release -DREDSHIFT_CLIENT=odbc -DREDSHIFT_TEST_ENDPOINTS=ON -S $(DUCKDB_SRCDIR) -B build/driver-e2e
	cmake --build build/driver-e2e --config Release

# The ODBC client's C++ client suite on driver_e2e's build, so the override's cases run where the switch is
# on. Without a driver, so the fake driver serves every connect; it has test_client's offline
# checks, and leaves build/driver-e2e a build without a driver. One shape, this tree's, as
# test_client_tsan runs.
test_client_e2e: check_no_network lint_tests test_graph_check
	$(MAKE) driver_e2e REDSHIFT_DRIVER=none
	bash $(PROJ_DIR)test/client/odbc/check-build-graph-offline.sh build/driver-e2e
	$(call build_suite,driver-e2e,Release,redshift_client_tests)
	./build/driver-e2e/extension/$(EXT_NAME)/redshift_client_tests -w NoTests

# The offline client tests: the offline checks' fixture tests, the interface tests in
# test/client/interface/, the ODBC client's C++ tests in test/client/{fake_driver,odbc}/ and the SQL
# tests in test/client/sql/. Builds the ODBC client with REDSHIFT_DRIVER=none, so it needs no
# driver download or network; the graph check keeps the embedded build's fetch, embed and
# download steps out of that build.
# test_client runs them in each preview shape and test_client_shape in this tree's shape alone.
# test_client_tsan runs the same tests once, in this tree's shape, on the thread-sanitized build, for the tests that drive the client from a second thread.
test_client_shape: CLIENT_BUILD = release
test_client_shape: CLIENT_CONFIG = Release
test_client_tsan: CLIENT_BUILD = tsan
test_client_tsan: CLIENT_CONFIG = RelWithDebInfo
# The engine reports races that DuckDB's own ThreadSanitizer CI suppresses, among them ones
# inside the jemalloc it links on Linux, so without the same list a loaded host fails the run
# on the engine rather than on this extension. Same file, through the same variable, as
# duckdb/.github/workflows/Main.yml:1285 sets for its threadsan job.
test_client_tsan: export TSAN_OPTIONS = suppressions=$(PROJ_DIR)duckdb/.sanitizer-thread-suppressions.txt
# test_client's and test_client_tsan's SQL tests run without the end-to-end test variables, which
# the C++ fixtures unset for their own cases, so a caller's environment cannot refuse their
# connects. The pinned test targets pass the caller's environment through.
WITHOUT_TEST_VARIABLES = env -u DUCKDB_REDSHIFT_TEST_E2E -u DUCKDB_REDSHIFT_TEST_ENDPOINT_URL -u DUCKDB_REDSHIFT_TEST_HOST
# The graph check cannot see a download CMake makes while configuring, so the client
# targets refuse to run with a network; REDSHIFT_ALLOW_NETWORK=1 runs them anyway, without that
# proof. The script says why.
check_no_network:
	@REDSHIFT_ALLOW_NETWORK="$(REDSHIFT_ALLOW_NETWORK)" bash $(PROJ_DIR)test/client/odbc/check-no-network.sh

test_client test_client_tsan: check_no_network lint_tests test_graph_check
test_client_shape: check_no_network lint_tests test_graph_check
test_client:
	$(call each_preview_shape,test_client_shape)

test_client_shape test_client_tsan:
	$(MAKE) $(CLIENT_BUILD) REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=none
	$(MAKE) check_built_client_$(CLIENT_BUILD) REDSHIFT_CLIENT=odbc
	bash $(PROJ_DIR)test/client/odbc/check-build-graph-offline.sh build/$(CLIENT_BUILD)
	$(call build_suite,$(CLIENT_BUILD),$(CLIENT_CONFIG),redshift_client_tests)
	bash $(PROJ_DIR)test/client/odbc/check-no-test-hooks.sh build/$(CLIENT_BUILD)
	./build/$(CLIENT_BUILD)/extension/$(EXT_NAME)/redshift_client_tests -w NoTests
	$(WITHOUT_TEST_VARIABLES) $(RUN_SELECTED) ./build/$(CLIENT_BUILD)/$(TEST_PATH) "$(TESTS_BASE_DIRECTORY)client/sql/*$(PREVIEW_TESTS)"

# The driver's tests: the tool tests and the C++ tests in test/client/driver/, against the
# embedded driver. Its only network access is one fetch of each locked release file (per Linux
# platform the package, its detached signature and certificate, the one universal macOS package,
# the Windows MSI, and the notices the lock takes from the release tag), cached under
# build/driver-package-cache/, and, for a lock with rename rows, one of each of the two pinned
# wheels rename_pin.py uses (delvewheel and pefile), cached under build/rename-tools/ or
# RS_RENAME_TOOLS, which an offline run pre-seeds with each as <sha256>.whl. No release
# file is fetched with a warm cache or an RS_DRIVER_MIRROR that holds each of those files (<sha256>,
# <sha256>-<notice> and, for Linux, <sha256>-signature.bin and <sha256>-certificate.pem); the tool
# tests reuse the build's copy of the host platform's package. Where the build
# embeds, by the platform the configure records, the graph check fails unless the embedded graph
# still has the fetch and embed steps its markers name, and the tool tests get the build's copy of
# the lock it embeds, which may be one the build directory keeps from an earlier RS_DRIVER_LOCK.
# On windows_amd64, run from Git's bash in a Visual Studio developer shell (which puts dumpbin on
# PATH), the tool tests do not run: they need POSIX file modes and links, and Linux runs every one.
# There the interface's import-table case reads the embedded loadable extension,
# embedded_resources.py checks the set's resources in the six images it lists (not DuckDB's
# plan_serializer.exe, which links the loader too), and run-windows-loading.sh runs the cases only a
# Windows host can. Any other platform (musl, FreeBSD) links the NOT_BUILT placeholder and has
# neither step, nor any driver case: there the C++ suite is skipped, with a message, rather than
# failing under -w NoTests. The exit 2 under -w NoTests (catch.hpp:13817-13818, as build_suite says) comes
# only from a test spec that matches nothing (catch.hpp:13614-13617), so the suite runs with the
# spec '~[.]': the same cases as no spec, but a suite that lost its cases fails rather than passes.
# The suite is built before the tool tests, and on Linux with it the probe that host-probe_test.sh runs:
# only there do the tool tests get RS_REQUIRE_PROBE=1 (macOS builds no probe, so that suite skips its
# real-probe cases). Likewise, only there, when the lock has rename rows, rename-tools.sh first
# fetches the pinned delvewheel and pefile, by their pinned URLs and SHA-256s, and the tool tests
# get RS_REQUIRE_RENAME_TOOLS=1, so rename_test.sh's pinned-tool cases must run. Before them the client
# tests are built too, so check-no-test-hooks.sh checks the shipped build, the driver loader's sources and the embed
# included, for test-only hooks (on Windows it skips, saying so).
# The suite runs without the caller's LD_LIBRARY_PATH: the platform loader parses it at start, and the
# loader refuses a list with an empty or relative element (what `export LD_LIBRARY_PATH=$LD_LIBRARY_PATH:/x`
# leaves), which would fail the cases that load the real driver for a reason that is the caller's. For the
# same reason it runs without the DYLD_ variables the macOS loader refuses (driver_loader_macos.cpp,
# PlatformLoaderVariables); Linux ignores them. test-driver-env_test.sh checks the list and that test_driver's recipe uses it.
DRIVER_SUITE_ENV = env -u LD_LIBRARY_PATH -u DYLD_LIBRARY_PATH -u DYLD_FRAMEWORK_PATH -u DYLD_IMAGE_SUFFIX \
	-u DYLD_VERSIONED_LIBRARY_PATH -u DYLD_VERSIONED_FRAMEWORK_PATH -u DYLD_FORCE_FLAT_NAMESPACE
test_driver: lint_tests
	$(MAKE) release REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=embedded
	$(call build_suite,release,Release,redshift_driver_tests)
	$(call build_suite,release,Release,redshift_client_tests)
	bash $(PROJ_DIR)test/client/odbc/check-no-test-hooks.sh build/release
	@platform="$$(sed -n 's/^platform //p' build/release/extension/$(EXT_NAME)/redshift_driver_identity.txt 2>/dev/null | tr -d '\r')"; \
	case "$$platform" in \
	linux_amd64 | linux_arm64) \
	  lock="$(abspath build/release/extension/$(EXT_NAME)/redshift_driver.lock)"; \
	  bash $(PROJ_DIR)test/client/odbc/check-build-graph-offline.sh --expect-embedded build/release && \
	  rename_tools="$$(bash $(PROJ_DIR)test/client/driver/rename-tools.sh "$$lock")" && \
	  RS_DRIVER_LOCK="$$lock" RS_REQUIRE_PROBE=1 RS_REQUIRE_RENAME_TOOLS="$$rename_tools" \
	    bash $(PROJ_DIR)test/client/driver/run-tool-tests.sh && \
	  $(DRIVER_SUITE_ENV) ./build/release/extension/$(EXT_NAME)/redshift_driver_tests -w NoTests '~[.]' ;; \
	osx_amd64 | osx_arm64) \
	  bash $(PROJ_DIR)test/client/odbc/check-build-graph-offline.sh --expect-embedded build/release && \
	  RS_DRIVER_LOCK="$(abspath build/release/extension/$(EXT_NAME)/redshift_driver.lock)" bash $(PROJ_DIR)test/client/driver/run-tool-tests.sh && \
	  $(DRIVER_SUITE_ENV) ./build/release/extension/$(EXT_NAME)/redshift_driver_tests -w NoTests '~[.]' ;; \
	windows_amd64) \
	  command -v dumpbin >/dev/null || { echo "test_driver: dumpbin is not on PATH; run from a Visual Studio developer shell" >&2; exit 1; }; \
	  bash $(PROJ_DIR)test/client/odbc/check-build-graph-offline.sh --expect-embedded build/release && \
	  $(call build_suite,release,Release,redshift_client_tests) && \
	  RS_REQUIRE_DUMPBIN=1 ./build/release/extension/$(EXT_NAME)/redshift_client_tests -w NoTests \
	    'The loadable extension imports no ODBC driver manager' && \
	  python3 -I $(PROJ_DIR)test/client/driver/embedded_resources.py build/release/extension/$(EXT_NAME)/redshift_driver/driver_info.h \
	    build/release/extension/$(EXT_NAME)/$(EXT_NAME).duckdb_extension build/release/duckdb.exe build/release/test/unittest.exe \
	    build/release/src/duckdb.dll build/release/extension/$(EXT_NAME)/redshift_client_tests.exe \
	    build/release/extension/$(EXT_NAME)/redshift_driver_tests.exe && \
	  $(DRIVER_SUITE_ENV) ./build/release/extension/$(EXT_NAME)/redshift_driver_tests -w NoTests '~[.]' && \
	  bash $(PROJ_DIR)test/client/driver/run-windows-loading.sh ;; \
	"") echo "test_driver: build/release records no platform in extension/$(EXT_NAME)/redshift_driver_identity.txt" >&2; exit 1 ;; \
	*) echo "test_driver: $$platform has no embed and links the NOT_BUILT placeholder: skipping the --expect-embedded graph check and the C++ suite, which has no case there"; \
	  bash $(PROJ_DIR)test/client/driver/run-tool-tests.sh ;; \
	esac

# A stock DuckDB shell of the pinned version, without this extension linked in, in build/stock-shell, so the
# end-to-end run can INSTALL and LOAD the loadable redshift.duckdb_extension as a user does (the release
# shell has the extension linked statically, so its LOAD opens nothing). Same version stamp, platform and macOS
# architecture as a native release: the -DOVERRIDE_GIT_DESCRIBE and -DDUCKDB_EXPLICIT_PLATFORM flags and
# $(OSX_BUILD_FLAG), from OSX_BUILD_ARCH, are copied from BUILD_FLAGS in
# third_party/extension-ci-tools/duckdb_extension.Makefile (line 127 at the vendored e38c2e1), so an osx_amd64
# build cross-built on Apple silicon gets an x86_64 shell; recheck them against that line when the pin moves.
.PHONY: stock_shell
stock_shell:
	mkdir -p build/stock-shell
	cmake $(GENERATOR) -DCMAKE_BUILD_TYPE=Release -DOVERRIDE_GIT_DESCRIBE="${OVERRIDE_GIT_DESCRIBE}" -DDUCKDB_EXPLICIT_PLATFORM='${DUCKDB_PLATFORM}' $(OSX_BUILD_FLAG) -DBUILD_UNITTESTS=0 -S $(DUCKDB_SRCDIR) -B build/stock-shell
	cmake --build build/stock-shell --config Release --target shell

# The extension end to end through SQL, offline, on driver_e2e's test-endpoint build/driver-e2e with the
# embedded driver, the one build whose connect takes the end-to-end variables: run-e2e.sh runs it from
# directories that hold an odbc.ini or that others can write, with a refused variable, with ODBCINI and with
# LD_LIBRARY_PATH, and a stock shell INSTALLs and LOADs the loadable extension. The driver must load and fail
# the connect itself, or the connection must be refused first: the SQL cases aim both endpoints at closed ports on
# 127.0.0.1, and any other ODBC build refuses the variable that selects them on every connect. Like test_driver,
# its only network access is the locked fetch (none with RS_DRIVER_MIRROR or a warm cache). Run it with no
# network all the same, as defence in depth and for the two shell cases, which go to the endpoint the region
# derives: a container with no network, or a network namespace that keeps the host's root mapped (a root
# `unshare -n`), not `unshare -rn`; run-e2e.sh checks both.
# On windows_amd64, by the platform the build records, from Git's bash with a 64-bit Python 3 as
# python3, it runs run-e2e.sh's Windows cases with build/stock-shell/duckdb.exe: no unshare and no
# /proc/net/dev there, so the run records that it has no no-network proof, and every case, the shell
# cases too, aims both endpoints at closed ports on 127.0.0.1.
# On osx_amd64 and osx_arm64 it runs macos-hosts.sh instead (run-e2e.sh reads /proc/net/dev, which macOS
# lacks): the loadable extension in the host applications it is given and the stock shell, each aimed at closed
# ports on 127.0.0.1 as on Windows, without the DYLD_ variables the loader refuses, and what weak-definition
# coalescing reaches between the driver and the process's other images.
# On Linux, after run-e2e.sh and so where its no-network check passed, it builds the client suite there and
# runs its offline [odbc] cases without the loader variables, as test_driver runs its suite, so the
# environment gate's cases run on the embedded driver. Catch counts a skip as a pass, so
# REDSHIFT_REQUIRE_DRIVER=1 fails the environment gate's driver cases if they would skip.
.PHONY: test_driver_e2e
test_driver_e2e: lint_tests
	$(MAKE) driver_e2e REDSHIFT_DRIVER=embedded
	$(MAKE) stock_shell
	@identity=build/driver-e2e/extension/$(EXT_NAME)/redshift_driver_identity.txt; \
	platform="$$(sed -n 's/^platform //p' "$$identity" 2>/dev/null | tr -d '\r')"; \
	stock=build/stock-shell/duckdb; \
	case "$$platform" in windows_amd64) stock=build/stock-shell/duckdb.exe ;; esac; \
	case "$$platform" in \
	osx_amd64 | osx_arm64) $(DRIVER_SUITE_ENV) bash $(PROJ_DIR)test/client/driver/macos-hosts.sh build/driver-e2e "$$stock"; exit $$? ;; \
	esac; \
	bash $(PROJ_DIR)test/client/driver/run-e2e.sh build/driver-e2e "$$stock" && \
	case "$$platform" in \
	linux_amd64 | linux_arm64) \
	  $(call build_suite,driver-e2e,Release,redshift_client_tests) && \
	  $(DRIVER_SUITE_ENV) REDSHIFT_REQUIRE_DRIVER=1 ./build/driver-e2e/extension/$(EXT_NAME)/redshift_client_tests -w NoTests '[odbc]~[live]' ;; \
	"") echo "test_driver_e2e: build/driver-e2e records no platform in extension/$(EXT_NAME)/redshift_driver_identity.txt" >&2; exit 1 ;; \
	esac

# The pinned format;tidy check configures from BUILD_FLAGS, which carry the default
# REDSHIFT_CLIENT=mock, and run-clang-tidy only sees the files in that compile database. No
# client source is in a mock build, so this target runs the same check over the ODBC build and
# belongs in the client validation; without it, the pinned tidy check alone would let the
# client's findings accumulate unseen.
tidy_client:
	$(MAKE) tidy-check REDSHIFT_CLIENT=odbc REDSHIFT_DRIVER=none

# The pinned tidy-check (third_party/extension-ci-tools/duckdb_extension.Makefile:295-299) passes
# run-clang-tidy the file regex 'src/.*/', which needs a slash after src/, so it checks only the
# files in src/'s subdirectories and skips those directly in src/, redshift_query.cpp among them.
# This adds them as a prerequisite of tidy-check, so `make tidy-check` and tidy_client cover
# every file in src/. The extension needs it, since its sources are the ones in src/; it configures
# build/tidy with the pinned recipe's own commands. Check it against those lines when the
# vendored Makefile moves. It runs in the public shape (public_shape), so the extension's refusing
# target-hook default, which the public tree ships and which lives directly in src/, is in its
# compile database. In the full tree that is the copy's build/tidy, and the pinned recipe then
# configures this tree's own for its check of src/'s subdirectories, which so covers the preview
# implementations; in a tree without them the pinned configure finds build/tidy
# current. The ODBC client's default, src/transport/odbc/accountless_settings.cpp, is in
# neither check: this pass covers only the files directly in src/, and in the full tree the pinned
# pass compiles the preview in its place, in tidy_client too.
# `make public_shape PUBLIC_TARGET=tidy_client` checks it.
tidy-check: tidy_top_level
ifneq ($(PREVIEW_DIRS),)
tidy_top_level:
	$(MAKE) public_shape PUBLIC_TARGET=tidy_top_level
else
tidy_top_level:
	mkdir -p ./build/tidy
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) -DDISABLE_UNITY=1 -DCLANG_TIDY=1 -S $(DUCKDB_SRCDIR) -B build/tidy
	cp duckdb/.clang-tidy build/tidy/.clang-tidy
	cd build/tidy && python3 ../../duckdb/scripts/run-clang-tidy.py '$(PROJ_DIR)src/[^/]*\.cpp$$' -header-filter '$(PROJ_DIR)src/.*/' -quiet ${TIDY_THREAD_PARAMETER} ${TIDY_BINARY_PARAMETER} ${TIDY_PERFORM_CHECKS}
endif

# The driver loader's sources are only in the embedded ODBC build, which neither check above
# configures. This is a hand copy of the pinned tidy-check recipe
# (third_party/extension-ci-tools/duckdb_extension.Makefile:295-299) over that build, in a build directory of its own and over
# src/transport/driver/ only; check it against those lines when the pin moves. The -D options
# after BUILD_FLAGS override the client and driver it carries. The configure fetches nothing, so
# the check needs no network.
tidy_driver:
	mkdir -p ./build/tidy_driver
	cmake $(GENERATOR) $(BUILD_FLAGS) $(EXT_DEBUG_FLAGS) -DDISABLE_UNITY=1 -DCLANG_TIDY=1 -DREDSHIFT_CLIENT=odbc -DREDSHIFT_DRIVER=embedded -S $(DUCKDB_SRCDIR) -B build/tidy_driver
	cp duckdb/.clang-tidy build/tidy_driver/.clang-tidy
	cd build/tidy_driver && python3 ../../duckdb/scripts/run-clang-tidy.py '$(PROJ_DIR)src/transport/driver/' -header-filter '$(PROJ_DIR)src/transport/driver/' -quiet ${TIDY_THREAD_PARAMETER} ${TIDY_BINARY_PARAMETER} ${TIDY_PERFORM_CHECKS}
