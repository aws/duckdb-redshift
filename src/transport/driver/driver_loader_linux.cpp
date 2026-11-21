//===----------------------------------------------------------------------===//
// The Linux platform hooks of the driver loader.
//===----------------------------------------------------------------------===//

#include "driver_loader_core.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dlfcn.h>
#include <link.h>
#include <sys/statvfs.h>
#include <unistd.h>

namespace duckdb {
namespace redshift_driver {
namespace detail {

int PlatformDlopenFlags() {
	return RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND;
}

std::vector<std::string> PlatformLoaderVariables() {
	return {}; // glibc searches nothing for a path with a '/', which is all the loader dlopens
}

AclCheck AclRule(const std::string &, int, uid_t, bool) {
	return AclCheck(); // a POSIX ACL entry that grants a right shows in the group bits the mode rules check
}

bool ClearAcl(int) {
	return true;
}

std::string VolumeRule(const std::string &) {
	return std::string(); // every file system is allowed
}

bool LoadedFileIsChecked(const void *, const struct stat &) {
	return true; // the descriptor's own name was loaded, or, without /proc, the checked path, as decided
}

const std::string *MissingPrerequisite(const char *dlerror_text, const std::vector<std::string> &prerequisites) {
	// glibc words a refusal about a library as "<name>: <what>", with a path before the name when it
	// was found: "cannot open shared object file" (missing, or not readable), "version `X' not found"
	// (the wrong version), and the file-level failures of a library that is there but is no use.
	static const char *const forms[] = {
	    "cannot open shared object file", "version `", "wrong ELF class", "file too short", "invalid ELF header",
	    "cannot read file data"};
	const std::string text = dlerror_text != nullptr ? dlerror_text : "";
	for (const auto &name : prerequisites) {
		const std::string marker = name + ": ";
		for (auto at = text.find(marker); at != std::string::npos; at = text.find(marker, at + 1)) {
			if (at != 0 && text[at - 1] != '/' && text[at - 1] != ' ') {
				continue; // the end of a longer name
			}
			const auto rest = text.substr(at + marker.size());
			for (const char *form : forms) {
				if (rest.compare(0, std::strlen(form), form) == 0) {
					return &name;
				}
			}
		}
	}
	return nullptr;
}

int NoExecState(const std::string &dir) {
	struct statvfs st;
	if (::statvfs(dir.c_str(), &st) != 0) {
		return -1;
	}
	return (st.f_flag & ST_NOEXEC) != 0 ? 1 : 0;
}

std::string DescriptorPath(int fd) {
	// thread-self, not self: another thread of a host that also uses descriptor numbers cannot change
	// what this thread's number names.
	const auto path = "/proc/thread-self/fd/" + std::to_string(fd);
	return ::access(path.c_str(), F_OK) == 0 ? path : std::string();
}

namespace {

// Appends the search list of the object `handle` names (dlinfo RTLD_DI_SERINFO), without repeats.
bool AppendSearchList(void *handle, std::vector<std::string> &directories) {
	Dl_serinfo size;
	if (::dlinfo(handle, RTLD_DI_SERINFOSIZE, &size) != 0 || size.dls_size < sizeof(Dl_serinfo)) {
		return false;
	}
	// malloc's alignment suits the structure; the list follows it.
	auto *info = static_cast<Dl_serinfo *>(std::malloc(size.dls_size));
	if (info == nullptr) {
		return false;
	}
	*info = size;
	bool ok = false;
	if (::dlinfo(handle, RTLD_DI_SERINFO, info) == 0) {
		for (unsigned i = 0; i < info->dls_cnt; i++) {
			const char *name = info->dls_serpath[i].dls_name;
			const std::string directory = name != nullptr ? name : "";
			if (std::find(directories.begin(), directories.end(), directory) == directories.end()) {
				directories.push_back(directory);
			}
		}
		ok = true;
	}
	std::free(info);
	return ok;
}

} // namespace

bool ObjectSearchList(const void *address, std::vector<std::string> &directories) {
	Dl_info where;
	struct link_map *object = nullptr;
	return ::dladdr1(address, &where, reinterpret_cast<void **>(&object), RTLD_DL_LINKMAP) != 0 && object != nullptr &&
	       AppendSearchList(object, directories);
}

bool ProgramSearchList(std::vector<std::string> &directories) {
	// The main program's search list: LD_LIBRARY_PATH as glibc parsed it at process start (an empty
	// element as "."), the program's run paths, the system directories.
	void *program = ::dlopen(nullptr, RTLD_LAZY);
	if (program == nullptr) {
		return false;
	}
	const bool ok = AppendSearchList(program, directories);
	::dlclose(program);
	return ok;
}

bool PlatformSearchListFrom(bool (*program)(std::vector<std::string> &), const void *own,
                            std::vector<std::string> &directories) {
	// The main program's list, then the list of the object that holds `own`, this code for a loadable
	// extension: it adds that extension's own run paths, which the platform loader also searches for the
	// driver's dependencies. The object is found from an address, so no file is opened. A list that cannot
	// be read is not a list that was checked, whichever of the two it is.
	if (!program(directories)) {
		return false;
	}
	return ObjectSearchList(own, directories);
}

bool PlatformSearchListOf(const void *own, std::vector<std::string> &directories) {
	return PlatformSearchListFrom(&ProgramSearchList, own, directories);
}

bool PlatformSearchList(std::vector<std::string> &directories) {
	return PlatformSearchListOf(reinterpret_cast<const void *>(&PlatformSearchList), directories);
}

namespace {

// The object's DT_SONAME, or empty. The dynamic section is read from the loaded image, where glibc has
// already made the string table's address absolute (an unrelocated one, below the load address, is
// relocated here).
std::string SonameOf(const struct dl_phdr_info *info) {
	for (int i = 0; i < info->dlpi_phnum; i++) {
		const auto &header = info->dlpi_phdr[i];
		if (header.p_type != PT_DYNAMIC) {
			continue;
		}
		const auto *entry = reinterpret_cast<const ElfW(Dyn) *>(info->dlpi_addr + header.p_vaddr);
		ElfW(Addr) strings = 0;
		ElfW(Xword) soname = 0;
		bool has_soname = false;
		for (; entry->d_tag != DT_NULL; entry++) {
			if (entry->d_tag == DT_STRTAB) {
				strings = entry->d_un.d_ptr;
			} else if (entry->d_tag == DT_SONAME) {
				soname = entry->d_un.d_val;
				has_soname = true;
			}
		}
		if (!has_soname || strings == 0) {
			return std::string();
		}
		if (strings < info->dlpi_addr) {
			strings += info->dlpi_addr;
		}
		return std::string(reinterpret_cast<const char *>(strings) + soname);
	}
	return std::string();
}

} // namespace

bool LibraryIsMapped(const std::string &name) {
	// What glibc compares a bare name with (_dl_map_object): an object's own name, which for a library
	// loaded as a dependency is the path it was found at, and its DT_SONAME. Not the path's last
	// component: a file loaded by path under a name that is not its soname answers to neither.
	// Only objects of one namespace count, the one this code's dlopen loads the driver and its
	// dependencies into, which is the one glibc reuses objects from (_dl_map_object looks at the namespace
	// that is loading): dl_iterate_phdr reports the namespace of its caller and no other, so an object that
	// a dlmopen put in another namespace is not counted, and the name stays one to look for. (Measured
	// with dlmopen(LM_ID_NEWLM), and tested in driver_search_list_test.cpp.) Where this
	// code is itself in another namespace, its dlopen and this walk are both in that one.
	struct Search {
		const std::string *name;
		bool found;
	} search {&name, false};
	::dl_iterate_phdr(
	    [](struct dl_phdr_info *info, size_t, void *data) {
		    auto *state = static_cast<Search *>(data);
		    const char *path = info->dlpi_name;
		    if (path == nullptr || path[0] == '\0') {
			    return 0; // the main program
		    }
		    if (*state->name == path || *state->name == SonameOf(info)) {
			    state->found = true;
			    return 1;
		    }
		    return 0;
	    },
	    &search);
	return search.found;
}

bool IsNoExecMount(const std::string &dir) {
	return NoExecState(dir) == 1;
}

std::string MappedFilePath(const void *address, bool *mapped) {
	// Each line: start-end perms offset dev inode [path]. The kernel writes the path the file was
	// opened by, resolved, and appends " (deleted)" once it is removed.
	FILE *maps = std::fopen("/proc/self/maps", "re");
	if (maps == nullptr) {
		return std::string();
	}
	const auto target = reinterpret_cast<uintptr_t>(address);
	std::string found;
	// Whole lines, however long: a path has no length limit here, and a line read in pieces would
	// let the rest of a long path, which a user can choose, be parsed as a mapping of its own.
	char *line = nullptr;
	size_t capacity = 0;
	while (::getline(&line, &capacity, maps) > 0) {
		uintptr_t start = 0, end = 0;
		int path_at = 0;
		if (std::sscanf(line, "%" SCNxPTR "-%" SCNxPTR " %*s %*s %*s %*s %n", &start, &end, &path_at) < 2 ||
		    path_at == 0) {
			continue;
		}
		if (target < start || target >= end) {
			continue;
		}
		if (mapped != nullptr) {
			*mapped = true;
		}
		std::string path(line + path_at);
		// Only the newline: a path may end in spaces.
		if (!path.empty() && path.back() == '\n') {
			path.pop_back();
		}
		const std::string deleted = " (deleted)";
		const bool removed =
		    path.size() >= deleted.size() && path.compare(path.size() - deleted.size(), deleted.size(), deleted) == 0;
		// The kernel writes a newline in a path as \012 and a backslash as it is, so a path with a
		// backslash may not be the file's; it names nothing.
		if (!path.empty() && path[0] == '/' && !removed && path.find('\\') == std::string::npos) {
			found = path;
		}
		break;
	}
	std::free(line);
	std::fclose(maps);
	return found;
}

const char *PerUserCacheRule() {
	// Here, beside PerUserCacheBase, which reads the two variables: no common file names them.
	return "neither XDG_CACHE_HOME nor HOME is an absolute path";
}

std::string PerUserCacheBase() {
	auto absolute = [](const char *value) {
		return value != nullptr && value[0] == '/';
	};
	const char *xdg = std::getenv("XDG_CACHE_HOME");
	if (absolute(xdg)) {
		return std::string(xdg) + "/duckdb-redshift";
	}
	// From the environment only. The password database is not read: NSS may load modules or ask a
	// directory service. With neither variable, the cache is not a base.
	const char *home = std::getenv("HOME");
	return absolute(home) ? std::string(home) + "/.cache/duckdb-redshift" : std::string();
}

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
