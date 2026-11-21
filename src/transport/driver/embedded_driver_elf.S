/* The locked driver set, embedded byte for byte as read-only data. driver_info.h lists the set's files; each gets a hidden begin and end symbol,
   so the embed adds nothing to the extension's dynamic symbol table. Nothing here is
   executed: the loader only copies the bytes out. fetch-driver.sh has checked every file against
   the lock, and driver_info.h names each by its absolute path, because the assembler would look
   for a bare name in its working directory first. */
#include "driver_info.h"

#define REDSHIFT_EMBED_FILE(stem, path)                                                                                \
	.global redshift_driver_##stem##_begin;                                                                            \
	.hidden redshift_driver_##stem##_begin;                                                                            \
	.global redshift_driver_##stem##_end;                                                                              \
	.hidden redshift_driver_##stem##_end;                                                                              \
	.balign 64;                                                                                                        \
	redshift_driver_##stem##_begin:                                                                                    \
	.incbin path;                                                                                                      \
	redshift_driver_##stem##_end:;

	.section .rodata
REDSHIFT_DRIVER_EMBED_FILES(REDSHIFT_EMBED_FILE)

/* The GNU property note: GNU ld marks a linked object as built for IBT and SHSTK (x86-64) or BTI,
   PAC and GCS (aarch64) only when every input declares them, so without it this object would
   clear the marking of a build hardened with -fcf-protection or -mbranch-protection. It holds no
   code, so it meets them all and declares them whatever the flags. One NT_GNU_PROPERTY_TYPE_0
   note: name "GNU", one 4-byte property padded to 8 bytes, as ELF64 requires. */
#if defined(__x86_64__)
#define REDSHIFT_FEATURE_1_AND 0xc0000002 /* GNU_PROPERTY_X86_FEATURE_1_AND */
#define REDSHIFT_FEATURE_1_BITS 3         /* IBT (1) | SHSTK (2) */
#elif defined(__aarch64__)
#define REDSHIFT_FEATURE_1_AND 0xc0000000 /* GNU_PROPERTY_AARCH64_FEATURE_1_AND */
#define REDSHIFT_FEATURE_1_BITS 7         /* BTI (1) | PAC (2) | GCS (4) */
#endif
#ifdef REDSHIFT_FEATURE_1_AND
	.section .note.gnu.property,"a"
	.p2align 3
	.long 4  /* n_namesz */
	.long 16 /* n_descsz */
	.long 5  /* n_type: NT_GNU_PROPERTY_TYPE_0 */
	.asciz "GNU"
	.long REDSHIFT_FEATURE_1_AND
	.long 4  /* pr_datasz */
	.long REDSHIFT_FEATURE_1_BITS
	.long 0  /* padding to 8 bytes */
#endif

	.section .note.GNU-stack,"",@progbits
