// A non-PIE program that takes malloc's and free's addresses in its code, as python-build-standalone's Python
// does: the link gives it canonical PLT entries for both, the process's malloc is that entry, and the C
// library's GLOB_DAT slots for malloc and free point at the entries, undefined symbols of the program; only
// its JUMP_SLOT slots then show a preloaded allocator. Built with REDSHIFT_TEST_DEFINES_MALLOC, it also
// defines malloc itself, which is a replacement. Prints whether the process's malloc is the program's own
// ("canonical yes"), so the test knows the case is the one it means, and what FindAllocatorReplacement says
// ("replacement none", or the file).
#include "allocator_replacement.hpp"

#include <cstdio>
#include <cstdlib>

#include <dlfcn.h>

#if defined(REDSHIFT_TEST_DEFINES_MALLOC)
extern "C" void *__libc_malloc(size_t size);
extern "C" void *malloc(size_t size) {
	return __libc_malloc(size);
}
#endif

void *(*volatile taken_malloc)(size_t) = nullptr;
void (*volatile taken_free)(void *) = nullptr;

int main() {
	// Taken in code built without -fPIE, the addresses are absolute constants, which only canonical PLT
	// entries can give.
	taken_malloc = &std::malloc;
	taken_free = &std::free;
	Dl_info process, program;
	const bool canonical = ::dladdr(::dlsym(RTLD_DEFAULT, "malloc"), &process) != 0 &&
	                       ::dladdr(reinterpret_cast<void *>(&main), &program) != 0 &&
	                       process.dli_fbase == program.dli_fbase;
	const auto found = duckdb::redshift_driver::detail::FindAllocatorReplacement();
	std::printf("canonical %s\nreplacement %s\n", canonical ? "yes" : "no",
	            found.replaced ? found.file.c_str() : "none");
	return 0;
}
