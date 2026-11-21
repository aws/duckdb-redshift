/* A test host that opts in to quarantine, for macos-hosts.sh: inserted into the stock DuckDB shell at its start
 * (DYLD_INSERT_LIBRARIES), it gives the process the quarantine an app that sets LSFileQuarantineEnabled runs with,
 * through libquarantine, part of libSystem, as LaunchServices gives it to such an app: from then on every file the
 * process creates carries com.apple.quarantine. A test run may have no login session to launch an app bundle in, so
 * the host applies it to itself. A host where libquarantine lacks a call it uses exits 97 saying which; the case
 * fails unless every file of the set carries the attribute, so it cannot pass without the quarantine. */
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* libquarantine's calls, by dlsym, under the name its private header gives each (_qtn_proc_..., which its
 * qtn_proc_... names stand for): they are in no SDK header. */
static void *Call(const char *name) {
	void *call = dlsym(RTLD_DEFAULT, name);
	if (call == NULL) {
		call = dlsym(RTLD_DEFAULT, name + 1);
	}
	if (call == NULL) {
		fprintf(stderr, "quarantine_self: libquarantine has no %s\n", name);
		exit(97);
	}
	return call;
}

__attribute__((constructor)) static void QuarantineSelf(void) {
	void *(*alloc)(void) = (void *(*)(void))Call("_qtn_proc_alloc");
	void (*init)(void *) = (void (*)(void *))Call("_qtn_proc_init");
	void (*set_flags)(void *, uint32_t) = (void (*)(void *, uint32_t))Call("_qtn_proc_set_flags");
	void (*apply)(void *) = (void (*)(void *))Call("_qtn_proc_apply_to_self");
	void *quarantine = alloc();
	if (quarantine == NULL) {
		fprintf(stderr, "quarantine_self: libquarantine allocated no process quarantine\n");
		exit(97);
	}
	/* What these return is not documented (an x86_64 process under Rosetta 2 got no status from them), so the
	 * case checks their effect instead: every file of the set must carry the attribute. QTN_FLAG_DOWNLOAD (1):
	 * files the process creates are quarantined as downloaded ones are. */
	init(quarantine);
	set_flags(quarantine, 1);
	apply(quarantine);
}
