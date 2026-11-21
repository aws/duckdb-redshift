/* Where dyld left each of the driver's weak binds, for macos-hosts.sh's test of what weak-definition coalescing
 * reaches: inserted into a host at its start (DYLD_INSERT_LIBRARIES), it reads, when the process exits, the slot
 * of each weak bind REDSHIFT_WEAK_SLOTS lists (`<offset from the header, hex> <name>` lines, from
 * macos_bindings.py slots) in the loaded librsodbc64.dylib, and writes `<name> <image>` for each to
 * REDSHIFT_WEAK_SLOTS_OUT, the image being the file name of the one that holds the address the slot holds. At
 * exit, since the driver is loaded at the first connect and never unloaded. Writes nothing when the process
 * loaded no driver. */
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void Report(void) {
	const char *slots = getenv("REDSHIFT_WEAK_SLOTS");
	const char *out = getenv("REDSHIFT_WEAK_SLOTS_OUT");
	const char *driver = NULL;
	const char *leaf = "/librsodbc64.dylib";
	for (uint32_t i = 0; slots && out && driver == NULL && i < _dyld_image_count(); i++) {
		const char *name = _dyld_get_image_name(i);
		size_t length = name ? strlen(name) : 0;
		if (length >= strlen(leaf) && strcmp(name + length - strlen(leaf), leaf) == 0) {
			driver = (const char *)_dyld_get_image_header(i);
		}
	}
	if (driver == NULL) {
		return;
	}
	FILE *in = fopen(slots, "r");
	FILE *report = fopen(out, "w");
	char name[4096];
	unsigned long long offset;
	while (in && report && fscanf(in, "%llx %4095s", &offset, name) == 2) {
		Dl_info info;
		void *target = *(void *const *)(driver + offset);
		const char *image = dladdr(target, &info) && info.dli_fname ? info.dli_fname : "unknown";
		const char *slash = strrchr(image, '/');
		fprintf(report, "%s %s\n", name, slash ? slash + 1 : image);
	}
	if (in) {
		fclose(in);
	}
	if (report) {
		fclose(report);
	}
}

__attribute__((constructor)) static void Register(void) {
	atexit(Report);
}
