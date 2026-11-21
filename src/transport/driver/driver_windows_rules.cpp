#include "driver_windows_rules.hpp"

#include <algorithm>

namespace duckdb {
namespace redshift_driver {
namespace windows_rules {

namespace {

char Lower(char c) {
	return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// The device names, matched on the part before the first dot, plus the console's.
bool IsDeviceName(const std::wstring &component) {
	std::string stem;
	for (wchar_t c : component.substr(0, component.find(L'.'))) {
		stem.push_back(Lower(static_cast<char>(c)));
	}
	if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" || stem == "conin$" || stem == "conout$") {
		return true;
	}
	return stem.size() == 4 && (stem.compare(0, 3, "com") == 0 || stem.compare(0, 3, "lpt") == 0) && stem[3] >= '0' &&
	       stem[3] <= '9';
}

std::string ComponentRule(const std::wstring &component) {
	if (component.empty()) {
		return "has an empty component";
	}
	if (component == L"." || component == L"..") {
		return "has a . or .. component";
	}
	if (component.back() == L'.' || component.back() == L' ') {
		return "has a component that ends with a dot or a space";
	}
	if (IsDeviceName(component)) {
		return "has a component that is a device name";
	}
	return "";
}

} // namespace

std::string PathFormRule(const std::wstring &path) {
	if (path.compare(0, 4, L"\\\\?\\") == 0 || path.compare(0, 4, L"\\\\.\\") == 0) {
		return "is a device or Win32-namespace path";
	}
	if (path.compare(0, 2, L"\\\\") == 0) {
		return "is a UNC path";
	}
	for (wchar_t c : path) {
		if (c > 0x7F) {
			return "is not ASCII, which the driver cannot use";
		}
		if (c < 0x20 || std::wstring(L"<>\"|?*").find(c) != std::wstring::npos) {
			return "holds a character a Windows file name cannot hold";
		}
		if (c == L'/') {
			return "holds a forward slash";
		}
	}
	// A short path reads its terminating null at path[size()], which no rule accepts.
	const wchar_t drive = static_cast<wchar_t>(path[0] | 0x20);
	if (drive < L'a' || drive > L'z' || path[1] != L':') {
		return "is not rooted at a drive letter";
	}
	if (path[2] != L'\\') {
		return "is relative to a drive's current folder";
	}
	if (path.find(L':', 2) != std::wstring::npos) {
		return "names an alternate data stream";
	}
	if (path.size() > 3 && path.back() == L'\\') {
		return "ends with a separator";
	}
	for (size_t start = 3; start < path.size();) {
		const size_t end = std::min(path.find(L'\\', start), path.size());
		const auto rule = ComponentRule(path.substr(start, end - start));
		if (!rule.empty()) {
			return rule;
		}
		start = end + 1;
	}
	return "";
}

std::string Narrow(const std::wstring &path) {
	std::string narrow;
	for (wchar_t c : path) {
		narrow.push_back(c > 0x7F ? '?' : static_cast<char>(c));
	}
	return narrow;
}

std::wstring Widen(const std::string &path) {
	return std::wstring(path.begin(), path.end());
}

std::string JoinPath(const std::string &dir, const std::string &name) {
	return !dir.empty() && dir.back() == '\\' ? dir + name : dir + "\\" + name;
}

std::string SystemFilePath(const std::string &system_dir, const std::string &name) {
	return system_dir.empty() ? "" : JoinPath(system_dir, name);
}

size_t LongestPathUnder(const std::string &base, const std::vector<std::string> &file_names) {
	size_t longest_name = 0;
	for (const auto &name : file_names) {
		longest_name = std::max(longest_name, name.size());
	}
	// <base>\.redshift-driver\<set hash>\<file>: the staging and set-aside names are shorter than the
	// set hash, and the lock (<root>\.lock) and the trust file (<base>\trust\<64 hex>.pem) shorter still.
	return JoinPath(base, DRIVER_ROOT_NAME).size() + 1 + SET_HASH_LENGTH + 1 + longest_name;
}

bool SamePath(const std::string &a, const std::string &b) {
	return a.size() == b.size() &&
	       std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
}

std::string AttributeRule(uint32_t attributes, bool directory) {
	if (((attributes & ATTRIBUTE_DIRECTORY) != 0) != directory) {
		return directory ? "is a file, not a folder" : "is a folder, not a file";
	}
	if ((attributes & LINK_OR_PLACEHOLDER) != 0) {
		return "is a junction, link, mount point or cloud-files placeholder";
	}
	return "";
}

std::string DriveTypeRule(unsigned type) {
	switch (type) {
	case DRIVE_KIND_FIXED:
	case DRIVE_KIND_REMOVABLE:
	case DRIVE_KIND_RAMDISK:
		return "";
	case DRIVE_KIND_REMOTE:
		return "the folder is on a network volume";
	default:
		return "the folder is not on a local fixed, removable or RAM drive";
	}
}

std::string SecurityRule(const SecurityFacts &facts, PathPlace place, const std::string &user) {
	const bool ancestor = place == PathPlace::ANCESTOR;
	// TrustedInstaller owns and holds full control of system folders above a base (C:\, Program Files).
	const auto trusted = [&](const std::string &sid) {
		// An empty SID is one that could not be read, never the user's.
		return !sid.empty() && (sid == user || sid == SYSTEM_SID || sid == ADMINISTRATORS_SID ||
		                        (ancestor && sid == TRUSTED_INSTALLER_SID));
	};
	if (!trusted(facts.owner)) {
		return "is owned by an account other than you, SYSTEM or Administrators";
	}
	if (!facts.has_dacl) {
		return "has a NULL access control list, which lets every account change it";
	}
	if (facts.unknown_entry) {
		return "has an access control entry of a type the loader does not read";
	}
	// Above the base only what could replace the next component counts, so C:\'s add-folder right for
	// Authenticated Users does not refuse every base, as a sticky /tmp does not on Linux.
	const uint32_t refused = ancestor ? RIGHT_DELETE | RIGHT_DELETE_CHILD | RIGHT_WRITE_DAC | RIGHT_WRITE_OWNER
	                                  : RIGHT_WRITE_DATA | RIGHT_APPEND_DATA | RIGHT_DELETE | RIGHT_DELETE_CHILD |
	                                        RIGHT_WRITE_DAC | RIGHT_WRITE_OWNER;
	for (const auto &entry : facts.entries) {
		// A deny entry is not taken to offset a grant: whom it covers depends on group membership the
		// loader does not read, so grants are judged as if no deny entry were there.
		if (!entry.allow || entry.inherit_only) {
			continue;
		}
		const auto &sid = entry.sid == CREATOR_OWNER_SID ? facts.owner : entry.sid;
		uint32_t mask = entry.mask;
		if (mask & RIGHT_GENERIC_ALL) {
			mask |= refused;
		}
		if (mask & RIGHT_GENERIC_WRITE) {
			mask |= RIGHT_WRITE_DATA | RIGHT_APPEND_DATA;
		}
		if (!trusted(sid) && (mask & refused) != 0) {
			return ancestor ? "lets another account replace what is below it through its access control list"
			                : "lets another account change it through its access control list";
		}
	}
	return "";
}

std::string TrustDirectoryRule(const std::string &dir) {
	if (dir.find('}') != std::string::npos) {
		return "holds a }, which the trust file's path cannot hold";
	}
	// The ODBC client refuses a control character in the trust path, and PathFormRule lets DEL through.
	if (dir.find('\x7f') != std::string::npos) {
		return "holds a DEL character, which the trust file's path cannot hold";
	}
	if (dir.size() + 1 + TRUST_FILE_NAME_LENGTH > MAX_TRUST_PATH_BYTES) {
		return "makes the trust file's path longer than 255 bytes";
	}
	return "";
}

std::string NoUsableDirectoryMessage(const std::vector<Attempt> &attempts) {
	std::string reasons;
	for (const auto &attempt : attempts) {
		reasons += (reasons.empty() ? "" : "; ") + attempt.reason;
	}
	return "no usable directory to install the Redshift ODBC driver in" +
	       (reasons.empty() ? "" : " (" + reasons + ")") +
	       ". Set REDSHIFT_DRIVER_DIR to an existing folder on a local drive, owned by you, SYSTEM or Administrators "
	       "and writable by no one else, whose path is ASCII, in its long form (not an 8.3 short name) and short "
	       "enough, with no }, no junction or link and no component that ends in a dot or a space";
}

std::string BaseName(const std::string &path) {
	// With no backslash, npos + 1 wraps to 0 and the whole path is the name.
	return path.substr(path.find_last_of('\\') + 1);
}

std::string PostLoadRule(const std::vector<LoadedModule> &modules, const std::vector<CheckedFile> &files) {
	// A module whose path cannot be read may answer to a set name, so it fails the check rather than
	// going uncounted; so does one another thread unloaded between the listing and the path's read.
	if (std::any_of(modules.begin(), modules.end(), [](const LoadedModule &module) { return module.path.empty(); })) {
		return "a module's path cannot be read, or it was unloaded while the modules were listed, so it may "
		       "answer to the name of a driver file";
	}
	for (const auto &file : files) {
		const auto name = BaseName(file.path);
		const LoadedModule *match = nullptr;
		size_t count = 0;
		for (const auto &module : modules) {
			if (SamePath(BaseName(module.path), name)) {
				match = &module;
				count++;
			}
		}
		if (count == 0) {
			return "a driver file is not among the process's modules after its load";
		}
		if (count > 1) {
			return "more than one module answers to the name of a driver file";
		}
		if (!SamePath(match->path, file.path) || !match->identified || !(match->id == file.id)) {
			return "the module that answers to a driver file's name is not the file that was checked";
		}
	}
	return "";
}

bool LoadedFromElsewhere(const std::vector<LoadedModule> &modules, const std::string &name,
                         const std::string &system_path) {
	return std::any_of(modules.begin(), modules.end(), [&](const LoadedModule &module) {
		return SamePath(BaseName(module.path), name) && !SamePath(module.path, system_path);
	});
}

std::string SystemCopyRule(const std::vector<LoadedModule> &modules, const std::string &name,
                           const std::string &system_path) {
	const auto count = std::count_if(modules.begin(), modules.end(),
	                                 [&](const LoadedModule &module) { return SamePath(BaseName(module.path), name); });
	if (count != 1) {
		return name + " is loaded " + std::to_string(count) + " times, not once from System32";
	}
	return LoadedFromElsewhere(modules, name, system_path) ? name + " is loaded from outside System32" : "";
}

bool TraceValueIsOff(uint32_t type, const std::string &bytes) {
	if (type != REGISTRY_STRING || bytes.size() % 2 != 0) {
		return false;
	}
	std::u16string text;
	for (size_t i = 0; i < bytes.size(); i += 2) {
		text.push_back(static_cast<char16_t>(static_cast<unsigned char>(bytes[i]) |
		                                     static_cast<unsigned char>(bytes[i + 1]) << 8));
	}
	while (!text.empty() && text.back() == u'\0') {
		text.pop_back();
	}
	return text.empty() || text == u"0";
}

bool ParseFileVersion(const std::string &text, uint64_t &version) {
	uint64_t parsed = 0;
	size_t parts = 0;
	for (size_t start = 0; start <= text.size(); parts++) {
		const size_t end = std::min(text.find('.', start), text.size());
		const auto part = text.substr(start, end - start);
		if (part.empty() || part.size() > 5 || part.find_first_not_of("0123456789") != std::string::npos ||
		    std::stoul(part) > 0xFFFF) {
			return false;
		}
		parsed = parsed << 16 | std::stoul(part);
		start = end + 1;
	}
	if (parts != 4) {
		return false;
	}
	version = parsed;
	return true;
}

std::string FileVersionText(uint64_t version) {
	std::string text;
	for (int shift = 48; shift >= 0; shift -= 16) {
		text += std::to_string((version >> shift) & 0xFFFF) + (shift > 0 ? "." : "");
	}
	return text;
}

std::string RuntimeMessage(const std::string &dll, const std::string &minimum, bool own_copy, RuntimeState state,
                           uint64_t version) {
	std::string found = "which is version " + FileVersionText(version);
	if (state == RuntimeState::MISSING) {
		found = "which is missing";
	} else if (state == RuntimeState::UNREADABLE) {
		found = "whose version cannot be read";
	}
	const std::string needs = "the Redshift ODBC driver needs " + dll + " version " + minimum + " or later; ";
	if (own_copy) {
		// The program's own copy, such as the one python.org Python ships beside python.exe, keeps
		// serving the process whatever System32 holds, so the redistributable alone changes nothing.
		return needs + "this process would use the copy loaded by the program, " + found +
		       "; installing Microsoft's Visual C++ redistributable does not replace that copy; update the program, "
		       "or the Visual C++ runtime it ships, so that copy is " +
		       minimum + " or later, then restart the program and connect again";
	}
	return needs + "this process would use System32's copy, " + found +
	       " (a copy beside the program is used only if the program has loaded it); install Microsoft's latest Visual "
	       "C++ v14 x64 redistributable, then restart the program and connect again";
}

} // namespace windows_rules
} // namespace redshift_driver
} // namespace duckdb
