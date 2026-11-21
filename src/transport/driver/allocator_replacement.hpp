//===----------------------------------------------------------------------===//
// Linux: whether another library has replaced the C library's allocator for the C library's own calls.
//
// The driver is loaded with RTLD_DEEPBIND, so its own malloc and free are glibc's. Memory glibc allocates
// for it (strdup, realpath, backtrace_symbols) comes from whatever glibc's own malloc is bound to. When an
// allocator replaces glibc's (jemalloc or tcmalloc in LD_PRELOAD, or a program that defines malloc), the
// driver frees that memory with glibc's free and the process aborts, at the first call into the driver.
// The loader refuses such a process before it installs or loads anything, with its other refusals of the
// environment.
//
// No dependency beyond the C library, so a test host can link this file alone.
//===----------------------------------------------------------------------===//

#pragma once

#include <string>

namespace duckdb {
namespace redshift_driver {
namespace detail {

struct AllocatorReplacement {
	bool replaced = false;
	// The file of the object that holds the replacement, as the platform loader names it (the program's
	// own name for a program that defines malloc); empty when no loaded object holds the address.
	std::string file;
};

// Reads the C library's own relocation slots for malloc, free, calloc and realloc, the four a replacement
// allocator must provide, and through which glibc's own code calls them. A slot that points into the C
// library is read as the global binding (dlsym(RTLD_DEFAULT)), which is where a lazily bound slot not bound
// yet will go. A slot is replaced when it then points at a symbol another object defines. A program's
// canonical PLT entry, which a non-PIE program that takes malloc's address gives the C library's GLOB_DAT
// slots and dlsym's answer, is an undefined symbol and is not a replacement: calls through it reach whatever
// the program's own slot is bound to, which the C library's other slots show. A non-PIE program that takes
// the address of all four, under a lazily bound C library, is the one case this cannot see.
AllocatorReplacement FindAllocatorReplacement();

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
