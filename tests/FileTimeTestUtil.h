#ifndef KYTY_TESTS_FILE_TIME_TEST_UTIL_H_
#define KYTY_TESTS_FILE_TIME_TEST_UTIL_H_

#include <chrono>
#include <cstdint>
#include <filesystem>

#if defined(_WIN32)
#include <Windows.h>
#endif

// Sets a host file's write time to `unix_ns` nanoseconds since the Unix epoch.
inline bool SetUnixWriteTime(const std::filesystem::path& path, int64_t unix_ns) {
#if defined(_WIN32)
	constexpr int64_t WINDOWS_EPOCH_100NS = 116444736000000000;
	const int64_t    file_time_100ns      = unix_ns / 100 + WINDOWS_EPOCH_100NS;
	if (file_time_100ns < 0) {
		return false;
	}

	const auto ticks = static_cast<uint64_t>(file_time_100ns);
	FILETIME   write_time {static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
	HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
	                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
	                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	const bool success = SetFileTime(file, nullptr, nullptr, &write_time) != 0;
	CloseHandle(file);
	return success;
#else
	const std::chrono::sys_time<std::chrono::nanoseconds> time {std::chrono::nanoseconds(unix_ns)};
	const auto file_time = std::chrono::file_clock::from_sys(time);
	std::error_code ec;
	std::filesystem::last_write_time(
	    path, std::chrono::floor<std::filesystem::file_time_type::duration>(file_time), ec);
	return !ec;
#endif
}

#endif // KYTY_TESTS_FILE_TIME_TEST_UTIL_H_
