// src/transport/driver/sha256.hpp (the loader's own)
// SHA-256 (FIPS 180-4) of memory and of a file, for the loader's checks against driver_info.h.
// Self-contained on purpose: the loader includes no DuckDB header and must not depend on a
// TLS library the driver itself might bring into the process.
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace duckdb {
namespace redshift_driver {
namespace detail {

#if !defined(_WIN32)
// The text for an errno value. std::strerror may return a buffer shared between threads (glibc
// before 2.32 does for an unknown value, and the loader supports glibc 2.28), so this uses
// strerror_r, whose GNU form returns the text and whose XSI form (macOS) fills the buffer; the
// result's type picks the overload.
inline std::string ErrnoFromStrerror(int status, const char *buffer, int error_number) {
	return status == 0 ? std::string(buffer) : "error " + std::to_string(error_number);
}
inline std::string ErrnoFromStrerror(const char *text, const char *, int) {
	return text;
}
// ErrnoMessage with any function of either strerror_r signature, so tests can pass a stand-in for
// the form this platform does not have.
template <class STRERROR_R>
std::string ErrnoMessageFrom(STRERROR_R strerror_r_function, int error_number) {
	char buffer[256] = {};
	return ErrnoFromStrerror(strerror_r_function(error_number, buffer, sizeof(buffer)), buffer, error_number);
}
inline std::string ErrnoMessage(int error_number) {
	return ErrnoMessageFrom(::strerror_r, error_number);
}

#endif

// The digest alone is shared with the Windows loader, which reads files through Win32 handles.
class Sha256 {
public:
	Sha256() {
		Reset();
	}

	void Reset() {
		static const uint32_t initial[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
		                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
		std::memcpy(state_, initial, sizeof(state_));
		length_ = 0;
		buffered_ = 0;
	}

	void Update(const void *data, size_t size) {
		auto bytes = static_cast<const uint8_t *>(data);
		length_ += size;
		if (buffered_ > 0) {
			size_t take = size < 64 - buffered_ ? size : 64 - buffered_;
			std::memcpy(buffer_ + buffered_, bytes, take);
			buffered_ += take;
			bytes += take;
			size -= take;
			if (buffered_ < 64) {
				return;
			}
			Block(buffer_);
			buffered_ = 0;
		}
		for (; size >= 64; bytes += 64, size -= 64) {
			Block(bytes);
		}
		std::memcpy(buffer_, bytes, size);
		buffered_ = size;
	}

	// Lowercase hex of the digest. Resets the object.
	std::string FinishHex() {
		const uint64_t bits = length_ * 8;
		static const uint8_t pad[64] = {0x80};
		Update(pad, buffered_ < 56 ? 56 - buffered_ : 120 - buffered_);
		uint8_t tail[8];
		for (int i = 0; i < 8; i++) {
			tail[i] = static_cast<uint8_t>(bits >> (56 - 8 * i));
		}
		Update(tail, 8);
		static const char hex[] = "0123456789abcdef";
		std::string out(64, '0');
		for (int i = 0; i < 8; i++) {
			for (int j = 0; j < 4; j++) {
				const uint8_t byte = static_cast<uint8_t>(state_[i] >> (24 - 8 * j));
				out[i * 8 + j * 2] = hex[byte >> 4];
				out[i * 8 + j * 2 + 1] = hex[byte & 15];
			}
		}
		Reset();
		return out;
	}

private:
	static uint32_t Rotr(uint32_t x, int n) {
		return (x >> n) | (x << (32 - n));
	}

	void Block(const uint8_t *p) {
		static const uint32_t k[64] = {
		    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
		    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
		    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
		    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
		    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
		    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
		    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
		    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
		uint32_t w[64];
		for (size_t i = 0; i < 16; i++) {
			w[i] = (uint32_t(p[4 * i]) << 24) | (uint32_t(p[4 * i + 1]) << 16) | (uint32_t(p[4 * i + 2]) << 8) |
			       uint32_t(p[4 * i + 3]);
		}
		for (int i = 16; i < 64; i++) {
			const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
			const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
		uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
		for (int i = 0; i < 64; i++) {
			const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
			const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			h = g;
			g = f;
			f = e;
			e = d + t1;
			d = c;
			c = b;
			b = a;
			a = t1 + t2;
		}
		state_[0] += a;
		state_[1] += b;
		state_[2] += c;
		state_[3] += d;
		state_[4] += e;
		state_[5] += f;
		state_[6] += g;
		state_[7] += h;
	}

	uint32_t state_[8];
	uint64_t length_;
	uint8_t buffer_[64];
	size_t buffered_;
};

inline std::string Sha256Hex(const void *data, size_t size) {
	Sha256 hash;
	hash.Update(data, size);
	return hash.FinishHex();
}

#if !defined(_WIN32)
// A file descriptor closed when it goes out of scope, so a throw (an allocation failing while a
// reason is built) cannot leak it.
class UniqueFd {
public:
	explicit UniqueFd(int fd) : fd_(fd) {
	}
	~UniqueFd() {
		if (fd_ >= 0) {
			::close(fd_);
		}
	}
	UniqueFd(const UniqueFd &) = delete;
	UniqueFd &operator=(const UniqueFd &) = delete;
	// Movable, so a descriptor taken inside a loop can be kept past it, held in a vector, and handed to a
	// caller with Release(). A moved-from UniqueFd holds -1 and closes nothing.
	UniqueFd(UniqueFd &&other) noexcept : fd_(other.fd_) {
		other.fd_ = -1;
	}
	UniqueFd &operator=(UniqueFd &&other) noexcept {
		if (this != &other) {
			if (fd_ >= 0) {
				::close(fd_);
			}
			fd_ = other.fd_;
			other.fd_ = -1;
		}
		return *this;
	}
	int get() const {
		return fd_;
	}
	// Gives up ownership: returns the descriptor, which the caller closes, and holds -1 from then on.
	int Release() {
		const int fd = fd_;
		fd_ = -1;
		return fd;
	}

private:
	int fd_;
};

// SHA-256 of the rest of the open file `fd`, which must be a regular file. Leaves `fd` open. On
// failure returns false with `error` set to a short reason.
inline bool Sha256Fd(int fd, std::string &hex, std::string &error) {
	// On the heap: a thread_local or static buffer would cost every thread of the host, and a
	// stack one this size is a risk on small thread stacks.
	std::vector<uint8_t> chunk(1 << 16);
	struct stat st;
	if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
		error = "is not a regular file";
		return false;
	}
	Sha256 hash;
	for (;;) {
		const ssize_t n = ::read(fd, chunk.data(), chunk.size());
		if (n == 0) {
			break;
		}
		if (n < 0) {
			const int err = errno;
			if (err == EINTR) {
				continue;
			}
			error = std::string("cannot be read: ") + ErrnoMessage(err);
			return false;
		}
		hash.Update(chunk.data(), static_cast<size_t>(n));
	}
	hex = hash.FinishHex();
	return true;
}

// SHA-256 of the regular file at `path`, opened without following a final symbolic link and
// without blocking (a FIFO planted at the name fails instead of hanging). On failure returns false
// with `error` set to a short reason; the path is the caller's to add.
inline bool Sha256File(const std::string &path, std::string &hex, std::string &error) {
	const UniqueFd file(::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
	if (file.get() < 0) {
		const int err = errno;
		error = err == ELOOP ? "is a symbolic link" : std::string("cannot be opened: ") + ErrnoMessage(err);
		return false;
	}
	return Sha256Fd(file.get(), hex, error);
}

#endif

} // namespace detail
} // namespace redshift_driver
} // namespace duckdb
