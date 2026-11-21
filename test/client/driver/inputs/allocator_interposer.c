/* An allocator that replaces the C library's, as jemalloc or tcmalloc in LD_PRELOAD does, so the loader's
 * refusal of one is tested on every host. It forwards to glibc's own allocator, so the process using it is
 * safe; what the loader looks for is that the C library's own malloc, free, calloc and realloc are bound to
 * it. Built with REDSHIFT_TEST_ONLY_CALLOC or REDSHIFT_TEST_ONLY_REALLOC it replaces that one function
 * alone, and malloc stays the C library's, so each name is checked by itself. */
#include <stddef.h>

extern void *__libc_malloc(size_t size);
extern void __libc_free(void *pointer);
extern void *__libc_calloc(size_t count, size_t size);
extern void *__libc_realloc(void *pointer, size_t size);

#if !defined(REDSHIFT_TEST_ONLY_CALLOC) && !defined(REDSHIFT_TEST_ONLY_REALLOC)
void *malloc(size_t size) {
	return __libc_malloc(size);
}

void free(void *pointer) {
	__libc_free(pointer);
}
#endif

#if !defined(REDSHIFT_TEST_ONLY_REALLOC)
void *calloc(size_t count, size_t size) {
	return __libc_calloc(count, size);
}
#endif

#if !defined(REDSHIFT_TEST_ONLY_CALLOC)
void *realloc(void *pointer, size_t size) {
	return __libc_realloc(pointer, size);
}
#endif
