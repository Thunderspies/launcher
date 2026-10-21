#ifndef LAUNCHER_TEST_HEADERS_H
#define LAUNCHER_TEST_HEADERS_H

#include "descriptors.h"
#include "support.h"
#include <mbedtls/base64.h>

namespace launcher_test {
inline std::string header_descriptor(std::string json,
	const std::string &encoded)
{
	for (;;) {
		auto previous = json.find("  \"cached_header");
		if (previous == std::string::npos)
			break;
		json.erase(previous, json.find('\n', previous) - previous + 1);
	}
	auto format = json.find("\"format\": ");
	if (format != std::string::npos)
		json.replace(format + 10, 1, "2");
	json.insert(2, "  \"cached_header\": \"" + encoded + "\",\n");
	return json;
}

inline std::string object_header_descriptor(std::string json,
	const std::string &digest, const std::string &size)
{
	json = header_descriptor(json, "");
	json.erase(2, json.find('\n', 2) - 2 + 1);
	json.replace(json.find("\"format\": ") + 10, 1, "3");
	json.insert(2, "  \"cached_header_sha256\": \"" + digest +
		"\",\n  \"cached_header_size\": " + size + ",\n");
	return json;
}

inline std::string base64_header(const std::string &header)
{
	std::string encoded((header.size() + 2) / 3 * 4 + 1, '\0');
	size_t size = 0;
	if (mbedtls_base64_encode((unsigned char *)encoded.data(),
		encoded.size(), &size, (const unsigned char *)header.data(),
		header.size()))
		throw std::runtime_error("Cannot encode test header");
	encoded.resize(size);
	return encoded;
}

inline void publish_header(const fs::path &root, const char *previous,
	const char *next, const char *resource, const std::string &header,
	bool object = false)
{
	std::string json = read(root / "releases" /
		(std::string(previous) + ".json"));
	std::string old;
	std::string record;
	fs::path directory;
	for (const auto &item : fs::recursive_directory_iterator(root)) {
		if (item.path().parent_path().filename() != "revisions")
			continue;
		std::string bytes = read(item.path());
		std::string key = item.path().stem().string();
		if (record_field(bytes, "name") == resource &&
			json.find(key) != std::string::npos) {
			old = key;
			record = bytes;
			directory = root / record_field(bytes, "kind") /
				record_field(bytes, "sha256");
			break;
		}
	}
	if (record.empty())
		throw std::runtime_error("Missing fixture resource");
	if (object) {
		std::string digest = fingerprint(header);
		write(directory / "headers" / digest, header);
		record = object_header_descriptor(record, digest,
			std::to_string(header.size()));
	} else {
		record = header_descriptor(record, base64_header(header));
	}
	std::string key = publish_record(root, record);
	size_t offset = json.find(old);
	json.replace(offset, 64, key);
	fs::path target = root / "releases" / (std::string(next) + ".json");
	write(target, json);
	edit_json(target, [&](yyjson_mut_doc *doc, yyjson_mut_val *root) {
		yyjson_mut_obj_put(root, yyjson_mut_str(doc, "release"),
			yyjson_mut_strcpy(doc, next));
	});
}

inline std::string installed_header(const fs::path &archive, const char *name)
{
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_file *file = nullptr;
	pg_file_info info{};
	pg_error error{};
	EXPECT_EQ(pg_context_open(&context, &error), PG_OK);
	EXPECT_EQ(pg_source_open(context, archive.string().c_str(), nullptr,
		&source, &error), PG_OK);
	EXPECT_EQ(pg_source_find(source, name, &file, &error), PG_OK);
	EXPECT_EQ(pg_file_inspect(file, &info, &error), PG_OK);
	std::string header;
	if (info.cached_header_size)
		header.assign((const char *)info.cached_header,
			info.cached_header_size);
	EXPECT_EQ(pg_file_close(&file, &error), PG_OK);
	EXPECT_EQ(pg_source_close(&source, &error), PG_OK);
	EXPECT_EQ(pg_context_close(&context, &error), PG_OK);
	return header;
}
}
#endif
