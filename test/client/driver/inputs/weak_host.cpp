// The test host of macos-hosts.sh's test of what weak-definition coalescing reaches, built at test time with
// the platform's clang++: it defines operator new and delete, counting the calls that come from
// the driver's code, loads weak_before (which defines, weak, one of the driver's weak-bound AWS SDK template
// instances), then the driver, as the loader does (RTLD_NOW | RTLD_LOCAL), runs the closed-port smoke on it (its
// environment allocated with ODBCINI at /dev/null, as the loader allocates it), then loads weak_after (which
// weak-binds one of the driver's exported weak definitions, as another DuckDB extension could). It prints one
// `<fact> <value>` line each; weak_slots.c, inserted into it, reads where each of the driver's own weak binds
// went. It first probes <control path>, which no configuration row lists, for the file trace macos-hosts.sh
// takes of the same run. The smoke names <CA file> as CaFile, as the ODBC client's connection string always
// names one, so the trace measures the driver as the extension uses it (with no CA file named, driver 2.3.1.0
// looks for one of its own).
//
//   weak_host <driver> <weak_before library> <weak_after library> <closed port> <control path> <CA file>
//
// Exits 0 when every step ran, 1 when one could not.
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include <dlfcn.h>
#include <mach-o/loader.h>
#include <unistd.h>

namespace {

// The driver's code, its __TEXT segment, once it is loaded: a call whose return address is there is the driver's
// own call. Compared by address alone, so operator new calls nothing that could take dyld's lock.
std::atomic<std::uintptr_t> g_text_begin {0};
std::atomic<std::uintptr_t> g_text_end {0};
std::atomic<long> g_driver_new {0};
std::atomic<long> g_driver_delete {0};

void Count(std::atomic<long> &counter, void *caller) {
	const auto at = reinterpret_cast<std::uintptr_t>(caller);
	if (at >= g_text_begin.load() && at < g_text_end.load()) {
		counter++;
	}
}

// The extent of an image's __TEXT segment, which starts at its header.
std::uintptr_t TextSize(const void *header) {
	const auto *image = static_cast<const mach_header_64 *>(header);
	const auto *command = reinterpret_cast<const load_command *>(image + 1);
	for (uint32_t i = 0; i < image->ncmds; i++) {
		if (command->cmd == LC_SEGMENT_64 &&
		    std::strncmp(reinterpret_cast<const segment_command_64 *>(command)->segname, "__TEXT", 16) == 0) {
			return reinterpret_cast<const segment_command_64 *>(command)->vmsize;
		}
		command = reinterpret_cast<const load_command *>(reinterpret_cast<const char *>(command) + command->cmdsize);
	}
	return 0;
}

void *Allocate(std::size_t size) {
	if (void *memory = std::malloc(size == 0 ? 1 : size)) {
		return memory;
	}
	throw std::bad_alloc();
}

} // namespace

void *operator new(std::size_t size) {
	Count(g_driver_new, __builtin_return_address(0));
	return Allocate(size);
}
void *operator new[](std::size_t size) {
	Count(g_driver_new, __builtin_return_address(0));
	return Allocate(size);
}
void operator delete(void *memory) noexcept {
	Count(g_driver_delete, __builtin_return_address(0));
	std::free(memory);
}
void operator delete[](void *memory) noexcept {
	Count(g_driver_delete, __builtin_return_address(0));
	std::free(memory);
}
void operator delete(void *memory, std::size_t) noexcept {
	Count(g_driver_delete, __builtin_return_address(0));
	std::free(memory);
}
void operator delete[](void *memory, std::size_t) noexcept {
	Count(g_driver_delete, __builtin_return_address(0));
	std::free(memory);
}

int main(int argc, char **argv) {
	if (argc != 7) {
		std::fprintf(stderr, "usage: weak_host <driver> <weak_before library> <weak_after library> <closed port> "
		                     "<control path> <CA file>\n");
		return 2;
	}
	// The control of the file trace: a path no configuration row lists, probed before anything else.
	(void)::access(argv[5], F_OK);
	void *before = ::dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
	auto before_call = before ? reinterpret_cast<int (*)()>(::dlsym(before, "redshift_weak_before_call")) : nullptr;
	if (before_call == nullptr) {
		std::printf("error weak_before: %s\n", ::dlerror());
		return 1;
	}
	// The control: the library's own call reaches its own definition.
	std::printf("before_own_call %s\n", before_call() == 1 ? "own" : "elsewhere");
	void *driver = ::dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
	using AllocHandle = short (*)(short, void *, void **);
	using SetEnvAttr = short (*)(void *, int, void *, int);
	using DriverConnect =
	    short (*)(void *, void *, unsigned char *, short, unsigned char *, short, short *, unsigned short);
	using GetDiagRec = short (*)(short, void *, short, unsigned char *, int *, unsigned char *, short, short *);
	using FreeHandle = short (*)(short, void *);
	auto alloc = driver ? reinterpret_cast<AllocHandle>(::dlsym(driver, "SQLAllocHandle")) : nullptr;
	auto set = driver ? reinterpret_cast<SetEnvAttr>(::dlsym(driver, "SQLSetEnvAttr")) : nullptr;
	auto connect = driver ? reinterpret_cast<DriverConnect>(::dlsym(driver, "SQLDriverConnect")) : nullptr;
	auto diag = driver ? reinterpret_cast<GetDiagRec>(::dlsym(driver, "SQLGetDiagRec")) : nullptr;
	auto free_handle = driver ? reinterpret_cast<FreeHandle>(::dlsym(driver, "SQLFreeHandle")) : nullptr;
	Dl_info info;
	if (alloc == nullptr || set == nullptr || connect == nullptr || diag == nullptr || free_handle == nullptr ||
	    ::dladdr(reinterpret_cast<void *>(alloc), &info) == 0) {
		std::printf("error driver: %s\n", driver ? "an entry point is missing" : ::dlerror());
		return 1;
	}
	g_text_end = reinterpret_cast<std::uintptr_t>(info.dli_fbase) + TextSize(info.dli_fbase);
	g_text_begin = reinterpret_cast<std::uintptr_t>(info.dli_fbase);
	// The closed-port smoke, the environment allocated with ODBCINI at /dev/null as the loader allocates it.
	// SQL_HANDLE_ENV 1, SQL_HANDLE_DBC 2, SQL_ATTR_ODBC_VERSION 200, SQL_OV_ODBC3 3, SQL_NTS -3,
	// SQL_DRIVER_NOPROMPT 0; 0 is SQL_SUCCESS.
	void *env = nullptr;
	void *dbc = nullptr;
	::setenv("ODBCINI", "/dev/null", 1);
	const short env_status = alloc(1, nullptr, &env);
	::unsetenv("ODBCINI");
	if (env_status != 0 || set(env, 200, reinterpret_cast<void *>(3), 0) != 0 || alloc(2, env, &dbc) != 0) {
		std::printf("error the driver's handles could not be allocated\n");
		return 1;
	}
	std::string text = std::string("CaFile=") + argv[6] + ";Host=127.0.0.1;Port=" + argv[4] +
	                   ";Database=dev;UID=nobody;PWD=not-a-password;SSLMode=require;LoginTimeout=5";
	std::vector<unsigned char> in(text.begin(), text.end());
	in.push_back(0);
	connect(dbc, nullptr, in.data(), -3, nullptr, 0, nullptr, 0);
	unsigned char state[6] = {0};
	unsigned char message[1024] = {0};
	int native = 0;
	short length = 0;
	diag(2, dbc, 1, state, &native, message, sizeof(message), &length);
	std::printf("smoke %s %s\n", reinterpret_cast<char *>(state), reinterpret_cast<char *>(message));
	free_handle(2, dbc);
	free_handle(1, env);
	std::printf("driver_operator_new %ld\ndriver_operator_delete %ld\n", g_driver_new.load(), g_driver_delete.load());
	void *after = ::dlopen(argv[3], RTLD_NOW | RTLD_LOCAL);
	auto after_target = after ? reinterpret_cast<void *(*)()>(::dlsym(after, "redshift_weak_after_target")) : nullptr;
	auto name = after ? reinterpret_cast<const char *(*)()>(::dlsym(after, "redshift_weak_after_name")) : nullptr;
	if (after_target == nullptr || name == nullptr) {
		std::printf("error weak_after: %s\n", ::dlerror());
		return 1;
	}
	// Where the later library's weak bind went: the driver's definition, its own, or another image's.
	void *reached = after_target();
	const char *where = reached == ::dlsym(driver, name())  ? "driver"
	                    : reached == ::dlsym(after, name()) ? "own"
	                                                        : nullptr;
	if (where == nullptr) {
		where = ::dladdr(reached, &info) != 0 ? info.dli_fname : "unknown";
	}
	std::printf("after_bind %s\n", where);
	return 0;
}
