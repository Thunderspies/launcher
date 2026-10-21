#ifndef LAUNCHER_TEST_DESCRIPTORS_H
#define LAUNCHER_TEST_DESCRIPTORS_H

#include <launcher/launcher.h>
#include <mbedtls/sha256.h>
#include <mbedtls/md5.h>
#include <map>
#include <string>
#include <fstream>
#include <filesystem>

namespace launcher_test {
inline std::string hex_digest(const unsigned char *bytes, size_t size)
{
	std::string result;
	const char *hex = "0123456789abcdef";
	for (size_t i = 0; i < size; ++i) {
		result += hex[bytes[i] >> 4];
		result += hex[bytes[i] & 15];
	}
	return result;
}

inline std::string fingerprint(const std::string &bytes)
{
	unsigned char hash[32];
	mbedtls_sha256((const unsigned char *)bytes.data(), bytes.size(),
		hash, 0);
	return hex_digest(hash, sizeof(hash));
}

inline std::string descriptor(const std::string &sha,
	const std::string &size = "3", const std::string &stored = "3",
	const std::string &encoding = "raw",
	const std::string &md5 = "900150983cd24fb0d6963f7d28e17f72",
	const std::string &name = "assets/data.bin",
	const std::string &kind = "hogg")
{
	return "{\n  \"encoding\": \"" + encoding +
		"\",\n  \"format\": 1,\n  \"kind\": \"" + kind +
		"\",\n  \"logical_md5\": \"" + md5 +
		"\",\n  \"name\": \"" + name +
		"\",\n  \"sha256\": \"" + sha + "\",\n  \"size\": " + size +
		",\n  \"stored_size\": " + stored + "\n}\n";
}

inline size_t test_count(const launcher_release *release,
	launcher_file_kind kind)
{
	size_t count = 0;
	for (size_t i = 0; i < launcher_release_group_count(release); ++i) {
		launcher_file_group group{};
		launcher_release_group(release, i, &group, nullptr);
		if (group.kind == kind)
			count += group.file_count;
	}
	return count;
}

inline launcher_status test_file(const launcher_release *release,
	launcher_file_kind kind, size_t index, launcher_file *out,
	launcher_error *error)
{
	for (size_t i = 0; i < launcher_release_group_count(release); ++i) {
		launcher_file_group group{};
		launcher_release_group(release, i, &group, error);
		if (group.kind != kind)
			continue;
		if (index < group.file_count)
			return launcher_release_group_file(release, i, index, out,
				error);
		index -= group.file_count;
	}
	if (out)
		*out = {};
	return LAUNCHER_INVALID;
}

inline const char *test_hogg(const launcher_release *release)
{
	for (size_t i = 0; i < launcher_release_group_count(release); ++i) {
		launcher_file_group group{};
		launcher_release_group(release, i, &group, nullptr);
		if (group.kind == LAUNCHER_HOGG)
			return group.hogg_path;
	}
	return nullptr;
}

struct Descriptors {
	std::map<std::string, std::string> bytes;
	size_t calls = 0;
	std::string add(const std::string &data)
	{
		std::string key = fingerprint(data);
		bytes[key] = data;
		return key;
	}
	static launcher_status lookup(const char *key, const void **data,
		size_t *size, void *opaque, launcher_error *)
	{
		auto &self = *(Descriptors *)opaque;
		++self.calls;
		auto found = self.bytes.find(key);
		if (found == self.bytes.end())
			return LAUNCHER_IO;
		*data = found->second.data();
		*size = found->second.size();
		return LAUNCHER_OK;
	}
};

struct FileDescriptors {
	std::filesystem::path root;
	std::string buffer;
	static launcher_status lookup(const char *key, const void **data,
		size_t *size, void *opaque, launcher_error *)
	{
		auto &self = *(FileDescriptors *)opaque;
		std::filesystem::path path;
		for (const auto &entry :
			std::filesystem::recursive_directory_iterator(self.root))
			if (entry.path().filename() == std::string(key) + ".json")
				path = entry.path();
		std::ifstream file(path, std::ios::binary);
		if (!file)
			return LAUNCHER_IO;
		self.buffer.assign(std::istreambuf_iterator<char>(file), {});
		*data = self.buffer.data();
		*size = self.buffer.size();
		return LAUNCHER_OK;
	}
};
}
#endif
