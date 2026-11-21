/* The locked driver set, embedded byte for byte as read-only data, for macOS: the Mach-O counterpart of embedded_driver_elf.S, with the same symbols under
   Mach-O's leading underscore. driver_info.h lists the set's files; each gets a private external
   begin and end symbol, so the embed adds nothing to the extension's exported symbols. Both macOS
   builds embed the one universal library, signed by AWS, unchanged. Nothing here is executed: the
   loader only copies the bytes out. Mach-O has no stack note; the stack is never executable.

   No .subsections_via_symbols: with it the linker may treat each symbol as the start of its own
   block, to reorder or strip, and an end symbol would no longer follow its file's bytes. */
#include "driver_info.h"

/* The statement separator: Apple's arm64 assembler reads ';' as the start of a comment. */
#if defined(__arm64__) || defined(__aarch64__)
#define REDSHIFT_NEXT %%
#else
#define REDSHIFT_NEXT ;
#endif

#define REDSHIFT_EMBED_FILE(stem, path)                                                                                \
	.globl _redshift_driver_##stem##_begin REDSHIFT_NEXT                                                               \
	.private_extern _redshift_driver_##stem##_begin REDSHIFT_NEXT                                                      \
	.globl _redshift_driver_##stem##_end REDSHIFT_NEXT                                                                 \
	.private_extern _redshift_driver_##stem##_end REDSHIFT_NEXT                                                        \
	.p2align 6 REDSHIFT_NEXT                                                                                           \
	_redshift_driver_##stem##_begin: REDSHIFT_NEXT                                                                     \
	.incbin path REDSHIFT_NEXT                                                                                         \
	_redshift_driver_##stem##_end: REDSHIFT_NEXT

	.section __TEXT,__const
REDSHIFT_DRIVER_EMBED_FILES(REDSHIFT_EMBED_FILE)
/* A byte after the last end symbol, so no symbol sits at the very end of the section: a Mach-O
   linker need not attach such a symbol to the section, and zig's refuses it. */
	.byte 0
