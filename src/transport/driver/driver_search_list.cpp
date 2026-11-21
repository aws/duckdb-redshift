//===----------------------------------------------------------------------===//
// The driver's dependencies, found and checked before the driver is loaded.
// Linux and glibc: the search list, the "already loaded" test and the dlopen are platform hooks or calls
// (driver_loader_linux.cpp, driver_loader.cpp); the cache reader is here.
//
// dlopen of the driver would search for its not-yet-loaded dependencies (libkeyutils.so.1) in the list
// the process started with: LD_LIBRARY_PATH, the run paths of the program and of the extension, the
// cache /etc/ld.so.cache, the system directories. A directory there that another user could change lets
// that user supply a library the driver then runs. So the loader does not let glibc search: for each
// dependency no loaded object answers to, it takes the one path the cache gives for this platform (or, where the
// cache has none, the first file in glibc's system directories), checks that file and its directory, and loads
// it by that path just before the driver (driver_loader.cpp).
// glibc then finds it loaded by its soname and searches nothing for it. An empty or relative element of the
// list is still refused: glibc uses the same list for every later search in the process, which would then
// look in the current directory. Its other elements are not audited.
//
// Residual risk: the owner and mode of a file are only as strong as its file system's idea of identity; a
// library on a network or FUSE file system is judged by what the server or daemon reports.
//===----------------------------------------------------------------------===//

#include "driver_loader_core.hpp"
#include "sha256.hpp"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <elf.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace duckdb {
namespace redshift_driver {
namespace detail {

namespace {

// ---- ELF64 DT_NEEDED --------------------------------------------------------------------------

template <typename T>
bool ReadAt(const unsigned char *data, size_t size, uint64_t offset, T &value) {
	if (offset > size || size - offset < sizeof(T)) {
		return false;
	}
	std::memcpy(&value, data + offset, sizeof(T));
	return true;
}

bool IsLibraryName(const std::string &name) {
	if (name.empty() || name.size() > 255) {
		return false;
	}
	for (const char c : name) {
		const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
		                c == '_' || c == '+' || c == '-';
		if (!ok) {
			return false;
		}
	}
	return true;
}

// ---- The resolved library -----------------------------------------------------------------------

// `dir` is a directory only the user and root can change: walked as given (links only the user's or
// root's) and as resolved, and not shared or writable even if sticky, since another user could add a
// library there. False with `rule`, no path.
bool CheckDirectory(const std::string &dir, uid_t user, std::string &where, std::string &rule) {
	std::string walked;
	if (!CheckGivenPath(dir, true, user, walked, where, rule)) {
		return false;
	}
	char resolved[PATH_MAX];
	if (::realpath(dir.c_str(), resolved) == nullptr) {
		const int error = errno;
		where = dir;
		rule = "cannot be resolved: " + ErrnoText(error);
		return false;
	}
	return CheckAncestry(resolved, AncestryEnd::BASE, user, where, rule);
}

// The file `path` names is a regular file the user or root owns that no one else can write, after
// following its links, each the user's or root's, and what they lead to is in a directory only the user
// and root can change. True with `resolved` its real path; false with `rule`, no path.
bool CheckLibraryFile(const std::string &path, uid_t user, std::string &resolved, std::string &rule) {
	std::string current = path;
	for (int links = 0;; links++) {
		struct stat st;
		if (::lstat(current.c_str(), &st) != 0) {
			const int error = errno;
			rule = error == ENOENT || error == ENOTDIR
			           ? "a dependency of the driver is a symbolic link that leads nowhere"
			           : "a dependency of the driver cannot be examined: " + ErrnoText(error);
			return false;
		}
		if (S_ISLNK(st.st_mode)) {
			if (st.st_uid != user && st.st_uid != 0) {
				rule = "a dependency of the driver is a symbolic link another user owns";
				return false;
			}
			std::vector<char> target(static_cast<size_t>(PATH_MAX) + 1);
			const auto length = ::readlink(current.c_str(), target.data(), target.size());
			if (length < 0 || static_cast<size_t>(length) >= target.size() || links >= SYMLINK_LIMIT) {
				rule = "a dependency of the driver is a symbolic link that cannot be followed";
				return false;
			}
			const std::string text(target.data(), static_cast<size_t>(length));
			current = !text.empty() && text[0] == '/' ? text : Parent(current) + "/" + text;
			continue;
		}
		if (!S_ISREG(st.st_mode)) {
			rule = "a dependency of the driver is not a regular file";
			return false;
		}
		if (st.st_uid != user && st.st_uid != 0) {
			rule = "a dependency of the driver is owned by another user";
			return false;
		}
		if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
			rule = "a dependency of the driver can be written by another user";
			return false;
		}
		// The real path, which is what is loaded, must lead to this very file.
		char real[PATH_MAX];
		struct stat at_real;
		if (::realpath(path.c_str(), real) == nullptr || ::lstat(real, &at_real) != 0 || at_real.st_dev != st.st_dev ||
		    at_real.st_ino != st.st_ino) {
			rule = "a dependency of the driver cannot be resolved to the file that was checked";
			return false;
		}
		std::string where, detail;
		if (!CheckAncestry(Parent(real), AncestryEnd::PARENT, user, where, detail)) {
			rule = "a dependency of the driver is in a directory another user could change";
			return false;
		}
		resolved = real;
		return true;
	}
}

constexpr size_t MAX_CACHE_BYTES = 256u << 20;

} // namespace

// ---- /etc/ld.so.cache ---------------------------------------------------------------------------

// The header says what this reads, and why the file is opened without blocking.
CacheRead CachePaths(const char *cache_path, const std::vector<std::string> &names, std::vector<std::string> &paths) {
	// UniqueFd, so an allocation failure in the resize below closes the descriptor on its way out.
	const UniqueFd fd(::open(cache_path, O_RDONLY | O_CLOEXEC | O_NONBLOCK));
	if (fd.get() < 0) {
		return errno == ENOENT || errno == ENOTDIR ? CacheRead::ABSENT : CacheRead::UNREADABLE;
	}
	struct stat st;
	std::vector<unsigned char> data;
	bool ok = ::fstat(fd.get(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
	          static_cast<uint64_t>(st.st_size) <= MAX_CACHE_BYTES;
	if (ok) {
		data.resize(static_cast<size_t>(st.st_size));
		size_t got = 0;
		while (got < data.size()) {
			const auto n = ::read(fd.get(), data.data() + got, data.size() - got);
			if (n <= 0) {
				ok = false;
				break;
			}
			got += static_cast<size_t>(n);
		}
	}
	if (!ok) {
		return CacheRead::UNREADABLE;
	}
	static const char NEW_MAGIC[] = "glibc-ld.so.cache1.1";
	static const char OLD_MAGIC[] = "ld.so-1.7.0";
	constexpr size_t NEW_MAGIC_SIZE = sizeof(NEW_MAGIC) - 1;
	constexpr size_t OLD_MAGIC_SIZE = sizeof(OLD_MAGIC) - 1;
	constexpr size_t NEW_HEADER = 48, NEW_ENTRY = 24, OLD_HEADER = 16, OLD_ENTRY = 12;
	const size_t size = data.size();
	size_t base = size; // where the new section starts; `size` when there is none
	if (size >= NEW_MAGIC_SIZE && std::memcmp(data.data(), NEW_MAGIC, NEW_MAGIC_SIZE) == 0) {
		base = 0;
	} else if (size >= OLD_HEADER && std::memcmp(data.data(), OLD_MAGIC, OLD_MAGIC_SIZE) == 0) {
		uint32_t old_count = 0;
		if (ReadAt(data.data(), size, 12, old_count)) {
			// The new section follows the old entries, aligned to 8: glibc's ALIGN_CACHE, the alignment of its
			// struct cache_file_new, whose entries' uint64_t hwcap makes it 8 on both 64-bit platforms.
			const uint64_t at = (OLD_HEADER + static_cast<uint64_t>(old_count) * OLD_ENTRY + 7) & ~uint64_t {7};
			if (at <= size && size - at >= NEW_MAGIC_SIZE &&
			    std::memcmp(data.data() + at, NEW_MAGIC, NEW_MAGIC_SIZE) == 0) {
				base = static_cast<size_t>(at);
			}
		}
	}
	uint32_t count = 0;
	if (base == size || !ReadAt(data.data(), size, base + 20, count) ||
	    static_cast<uint64_t>(count) * NEW_ENTRY + NEW_HEADER > size - base) {
		return CacheRead::UNREADABLE;
	}
	// A string at `offset` from the new section's start, up to its NUL and inside the file. After an old section
	// too: glibc's reader takes the new section's strings from there (elf/dl-cache.c, whose string table is
	// cache_new from 2.26 to 2.37 at least), and ldconfig writes them so (elf/cache.c: "the string table always
	// begins at the beginning of the new cache struct").
	const auto string_at = [&](uint32_t offset, std::string &out) {
		const uint64_t at = static_cast<uint64_t>(base) + offset;
		if (at >= size) {
			return false;
		}
		const auto *begin = reinterpret_cast<const char *>(data.data() + at);
		const auto *end = static_cast<const char *>(std::memchr(begin, '\0', size - static_cast<size_t>(at)));
		if (end == nullptr) {
			return false;
		}
		out.assign(begin, end);
		return true;
	};
	paths.assign(names.size(), std::string());
	for (uint32_t i = 0; i < count; i++) {
		int32_t flags = 0;
		uint32_t key = 0, value = 0;
		uint64_t hwcap = 0;
		const size_t at = base + NEW_HEADER + static_cast<size_t>(i) * NEW_ENTRY;
		if (!ReadAt(data.data(), size, at, flags) || !ReadAt(data.data(), size, at + 4, key) ||
		    !ReadAt(data.data(), size, at + 8, value) || !ReadAt(data.data(), size, at + 16, hwcap)) {
			return CacheRead::UNREADABLE;
		}
		// This platform's libraries only, and only those in the plain directory: an entry with hardware
		// capabilities is in a subdirectory for some processors.
		std::string key_text, value_text;
		if (flags != LIBRARY_CACHE_FLAGS || hwcap != 0 || !string_at(key, key_text) || !string_at(value, value_text) ||
		    value_text.empty() || value_text[0] != '/') {
			continue;
		}
		for (size_t n = 0; n < names.size(); n++) {
			if (paths[n].empty() && key_text == names[n]) {
				paths[n] = value_text;
			}
		}
	}
	return CacheRead::PARSED;
}

std::vector<std::string> ElfNeededLibraries(const unsigned char *data, size_t size) {
	std::vector<std::string> names;
	// ELF64, little endian, which Linux amd64 and arm64 both are. The fields are read by offset, not through
	// the structs, since the bytes need not be aligned.
	if (data == nullptr || size < sizeof(Elf64_Ehdr) || std::memcmp(data, ELFMAG, SELFMAG) != 0 ||
	    data[EI_CLASS] != ELFCLASS64 || data[EI_DATA] != ELFDATA2LSB) {
		return names;
	}
	Elf64_Off phoff = 0;
	Elf64_Half phentsize = 0, phnum = 0;
	if (!ReadAt(data, size, offsetof(Elf64_Ehdr, e_phoff), phoff) ||
	    !ReadAt(data, size, offsetof(Elf64_Ehdr, e_phentsize), phentsize) ||
	    !ReadAt(data, size, offsetof(Elf64_Ehdr, e_phnum), phnum) || phentsize < sizeof(Elf64_Phdr)) {
		return names;
	}
	struct Segment {
		uint64_t offset, vaddr, filesz;
	};
	std::vector<Segment> loads;
	Segment dynamic {0, 0, 0};
	bool has_dynamic = false;
	for (uint16_t i = 0; i < phnum; i++) {
		const uint64_t at = phoff + static_cast<uint64_t>(i) * phentsize;
		Elf64_Word type = 0;
		Segment segment {0, 0, 0};
		if (!ReadAt(data, size, at + offsetof(Elf64_Phdr, p_type), type) ||
		    !ReadAt(data, size, at + offsetof(Elf64_Phdr, p_offset), segment.offset) ||
		    !ReadAt(data, size, at + offsetof(Elf64_Phdr, p_vaddr), segment.vaddr) ||
		    !ReadAt(data, size, at + offsetof(Elf64_Phdr, p_filesz), segment.filesz)) {
			return std::vector<std::string>();
		}
		if (type == PT_LOAD) {
			loads.push_back(segment);
		} else if (type == PT_DYNAMIC && !has_dynamic) {
			dynamic = segment;
			has_dynamic = true;
		}
	}
	if (!has_dynamic || dynamic.offset > size || dynamic.filesz > size - dynamic.offset) {
		return names;
	}
	std::vector<uint64_t> needed;
	uint64_t strtab = 0, strsz = 0;
	bool has_strtab = false;
	for (uint64_t at = dynamic.offset; at + sizeof(Elf64_Dyn) <= dynamic.offset + dynamic.filesz;
	     at += sizeof(Elf64_Dyn)) {
		Elf64_Sxword tag = 0;
		Elf64_Xword value = 0;
		if (!ReadAt(data, size, at + offsetof(Elf64_Dyn, d_tag), tag) ||
		    !ReadAt(data, size, at + offsetof(Elf64_Dyn, d_un), value)) {
			break;
		}
		if (tag == DT_NULL) {
			break;
		}
		if (tag == DT_NEEDED) {
			needed.push_back(value);
		} else if (tag == DT_STRTAB) {
			strtab = value;
			has_strtab = true;
		} else if (tag == DT_STRSZ) {
			strsz = value;
		}
	}
	if (!has_strtab) {
		return names;
	}
	// The string table's address is a virtual one: find the segment that holds it.
	uint64_t table = 0;
	bool found = false;
	for (const auto &load : loads) {
		if (strtab >= load.vaddr && strtab - load.vaddr < load.filesz) {
			table = load.offset + (strtab - load.vaddr);
			found = true;
			break;
		}
	}
	if (!found || table > size) {
		return names;
	}
	const uint64_t limit = std::min<uint64_t>(strsz == 0 ? size - table : strsz, size - table);
	for (const auto index : needed) {
		if (index >= limit || names.size() >= 64) {
			continue;
		}
		const auto *begin = reinterpret_cast<const char *>(data + table + index);
		const auto *end = static_cast<const char *>(std::memchr(begin, '\0', static_cast<size_t>(limit - index)));
		if (end == nullptr) {
			continue;
		}
		std::string name(begin, end);
		if (IsLibraryName(name)) {
			names.push_back(name);
		}
	}
	return names;
}

bool CheckSearchList(const SearchInputs &inputs, SearchRefusal &refusal, std::vector<NeededLibrary> &preload) {
	refusal = SearchRefusal();
	preload.clear();
	const auto refuse = [&](SearchCause cause, const std::string &rule, const std::string &where) {
		refusal.cause = cause;
		refusal.rule = rule;
		refusal.where = where;
		preload.clear();
		return false;
	};
	std::vector<std::string> directories;
	if (inputs.list == nullptr || !inputs.list(directories)) {
		return refuse(SearchCause::UNREADABLE, "the search list cannot be read", "");
	}
	for (const auto &directory : directories) {
		if (directory.empty() || directory[0] != '/') {
			return refuse(SearchCause::RELATIVE,
			              "the directory is empty or relative, so it searches the current directory", directory);
		}
	}
	// The names to load first are the ones no loaded object answers to already, decided without opening
	// any file (inputs.loaded).
	std::vector<std::string> names;
	for (const auto &name : inputs.needed) {
		if (IsLibraryName(name) && (inputs.loaded == nullptr || !inputs.loaded(name))) {
			names.push_back(name);
		}
	}
	if (names.empty()) {
		return true;
	}
	std::vector<std::string> paths;
	const auto read = inputs.cache_path != nullptr ? CachePaths(inputs.cache_path, names, paths) : CacheRead::ABSENT;
	if (read == CacheRead::UNREADABLE) {
		return refuse(SearchCause::CACHE_UNREADABLE, "the library cache cannot be read", inputs.cache_path);
	}
	paths.resize(names.size());
	// Gone as glibc's open would find it: following links, so a link that leads nowhere is gone too.
	const auto absent = [](const std::string &path) {
		struct stat st;
		return path.empty() || (::stat(path.c_str(), &st) != 0 && (errno == ENOENT || errno == ENOTDIR));
	};
	for (size_t n = 0; n < names.size(); n++) {
		// Where the cache has no file, or its file is gone, glibc looks in its system directories, in order; so
		// does this, and what it finds is checked as a cached file is.
		for (size_t d = 0; absent(paths[n]) && d < inputs.system_directories.size(); d++) {
			paths[n] = inputs.system_directories[d] + "/" + names[n];
		}
		if (absent(paths[n])) {
			refusal.library = names[n];
			return refuse(SearchCause::MISSING,
			              "neither the library cache nor a system directory has a file for " + names[n],
			              inputs.cache_path != nullptr ? inputs.cache_path : "");
		}
		std::string where, rule, resolved;
		if (!CheckDirectory(Parent(paths[n]), inputs.user, where, rule)) {
			return refuse(SearchCause::CACHE, rule, where);
		}
		if (!CheckLibraryFile(paths[n], inputs.user, resolved, rule)) {
			return refuse(SearchCause::CACHE, rule, paths[n]);
		}
		preload.push_back({names[n], resolved});
	}
	return true;
}

std::string SearchRefusalMessage(const SearchRefusal &refusal) {
	// The list the platform loader gives has no mark for where an element came from, and the
	// environment is not read to find out, so the remedy is chosen from what the list shows.
	switch (refusal.cause) {
	case SearchCause::RELATIVE:
		// The cause says all the rule does, so the rule is not repeated.
		return "the platform loader's search list holds an empty or relative directory, which searches the current "
		       "directory, where the platform loader could find a library another user put there. Restart the "
		       "process without it (an empty element in LD_LIBRARY_PATH, such as a trailing ':', is one), or have an "
		       "administrator correct the run path that names it, to connect";
	case SearchCause::CACHE:
		return "the platform loader's library cache or system directory has a library the Redshift driver needs "
		       "in a place that cannot be used (" +
		       refusal.rule +
		       "). An administrator must fix that file or its directory (or, where the library cache names it, rebuild "
		       "the cache with ldconfig). (In a user namespace "
		       "that does not map the host's root user, as under `unshare -r`, the system's directories look owned "
		       "by another user: run outside such a namespace.)";
	case SearchCause::CACHE_UNREADABLE:
		return "the platform loader's library cache cannot be read, so the libraries the Redshift driver needs "
		       "cannot be found and checked. An administrator must look at this host's library cache (ldconfig)";
	case SearchCause::MISSING:
		// Never shown: a missing library is a MISSING_PREREQUISITE with its own sentence (SearchError), so
		// this takes the one below.
	case SearchCause::UNREADABLE:
		break;
	}
	return "the platform loader's search list cannot be read, so the Redshift driver is not loaded, as it cannot be "
	       "checked that the list does not search the current directory. An administrator must look at this host's "
	       "dynamic loader";
}

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
