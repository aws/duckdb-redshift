//===----------------------------------------------------------------------===//
// Linux: whether another library has replaced the C library's allocator for the C library's own calls.
//===----------------------------------------------------------------------===//

#include "allocator_replacement.hpp"

#include <cstring>

#include <dlfcn.h>
#include <elf.h>
#include <gnu/lib-names.h>
#include <link.h>

namespace duckdb {
namespace redshift_driver {
namespace detail {

namespace {

static_assert(sizeof(void *) == 8, "the platforms with an embedded driver are 64-bit, with RELA relocations");

// A pointer from a loaded object's dynamic section: glibc stores most as run-time addresses on the platforms
// the driver ships for, and one still below the load address is an offset from it.
template <class T, class OBJECT>
const T *DynamicPointer(const OBJECT &object, ElfW(Addr) value) {
	return reinterpret_cast<const T *>(value < object.l_addr ? value + object.l_addr : value);
}

// The C library as dl_iterate_phdr reports it: its load address and its dynamic section.
struct CLibrary {
	ElfW(Addr) l_addr = 0;
	const ElfW(Dyn) *dynamic = nullptr;
};

int FindCLibrary(dl_phdr_info *info, size_t, void *data) {
	const ElfW(Dyn) *dynamic = nullptr;
	for (ElfW(Half) i = 0; i < info->dlpi_phnum; i++) {
		if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
			dynamic = reinterpret_cast<const ElfW(Dyn) *>(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
		}
	}
	if (dynamic == nullptr) {
		return 0;
	}
	CLibrary candidate;
	candidate.l_addr = info->dlpi_addr;
	candidate.dynamic = dynamic;
	const char *strings = nullptr;
	ElfW(Xword) soname = 0;
	bool has_soname = false;
	for (const ElfW(Dyn) *entry = dynamic; entry->d_tag != DT_NULL; entry++) {
		if (entry->d_tag == DT_STRTAB) {
			strings = DynamicPointer<char>(candidate, entry->d_un.d_ptr);
		} else if (entry->d_tag == DT_SONAME) {
			soname = entry->d_un.d_val;
			has_soname = true;
		}
	}
	if (strings == nullptr || !has_soname || std::strcmp(strings + soname, LIBC_SO) != 0) {
		return 0;
	}
	*static_cast<CLibrary *>(data) = candidate;
	return 1;
}

const char *const ALLOCATOR_NAMES[] = {"malloc", "free", "calloc", "realloc"};

bool IsAllocatorName(const char *name) {
	for (const char *allocator : ALLOCATOR_NAMES) {
		if (std::strcmp(name, allocator) == 0) {
			return true;
		}
	}
	return false;
}

} // namespace

AllocatorReplacement FindAllocatorReplacement() {
	AllocatorReplacement found;
	// The C library's object, by its soname among the loaded objects: nothing is opened, and no function's
	// address is taken, which a program's canonical PLT entry could answer for.
	CLibrary libc;
	::dl_iterate_phdr(FindCLibrary, &libc);
	Dl_info own;
	if (libc.dynamic == nullptr || ::dladdr(libc.dynamic, &own) == 0) {
		return found;
	}
	const CLibrary *map = &libc;
	const ElfW(Sym) *symbols = nullptr;
	const char *strings = nullptr;
	const ElfW(Rela) * tables[2] = {nullptr, nullptr};
	size_t sizes[2] = {0, 0};
	for (const ElfW(Dyn) *entry = map->dynamic; entry->d_tag != DT_NULL; entry++) {
		switch (entry->d_tag) {
		case DT_SYMTAB:
			symbols = DynamicPointer<ElfW(Sym)>(*map, entry->d_un.d_ptr);
			break;
		case DT_STRTAB:
			strings = DynamicPointer<char>(*map, entry->d_un.d_ptr);
			break;
		case DT_JMPREL:
			tables[0] = DynamicPointer<ElfW(Rela)>(*map, entry->d_un.d_ptr);
			break;
		case DT_PLTRELSZ:
			sizes[0] = entry->d_un.d_val;
			break;
		case DT_RELA:
			tables[1] = DynamicPointer<ElfW(Rela)>(*map, entry->d_un.d_ptr);
			break;
		case DT_RELASZ:
			sizes[1] = entry->d_un.d_val;
			break;
		default:
			break;
		}
	}
	for (size_t t = 0; symbols != nullptr && strings != nullptr && t < 2 && !found.replaced; t++) {
		for (size_t i = 0; tables[t] != nullptr && i < sizes[t] / sizeof(ElfW(Rela)); i++) {
			const auto &relocation = tables[t][i];
			const auto index = ELF64_R_SYM(relocation.r_info);
			if (index == 0 || !IsAllocatorName(strings + symbols[index].st_name)) {
				continue;
			}
			const char *name = strings + symbols[index].st_name;
			void *target = *reinterpret_cast<void *const *>(map->l_addr + relocation.r_offset);
			Dl_info info;
			if (::dladdr(target, &info) != 0 && info.dli_fbase == own.dli_fbase) {
				// Bound to the C library's own definition, or, in a lazily bound C library (Debian's and
				// Ubuntu's), not bound yet: it binds through the global scope, which dlsym(RTLD_DEFAULT) searches.
				target = ::dlsym(RTLD_DEFAULT, name);
			}
			if (target == reinterpret_cast<void *>(map->l_addr + symbols[index].st_value)) {
				continue; // the C library's own definition
			}
			void *entry = nullptr; // the ElfW(Sym) dladdr1 matched
			const bool named = ::dladdr1(target, &info, &entry, RTLD_DL_SYMENT) != 0;
			const auto *symbol = static_cast<const ElfW(Sym) *>(entry);
			if (named && symbol != nullptr && symbol->st_shndx == SHN_UNDEF) {
				continue; // a program's canonical PLT entry, which defines nothing
			}
			found.replaced = true;
			found.file = named && info.dli_fname != nullptr ? info.dli_fname : "";
			break;
		}
	}
	return found;
}

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
