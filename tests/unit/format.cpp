#include <launcher/launcher.h>
#include <gtest/gtest.h>
#include "../descriptors.h"
#include <piggle/piggle.h>
#include "../headers.h"
#include <atomic>
#include <thread>

using namespace launcher_test;

static const char *digest =
	"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

static std::string named(Descriptors &files, const std::string &name,
	const std::string &kind = "hogg")
{
	return files.add(descriptor(digest, "3", "3", "raw",
		"900150983cd24fb0d6963f7d28e17f72", name, kind));
}

static std::string group(const std::string &key,
	const std::string &kind = "hogg", const std::string &path = "a.hogg")
{
	(void)kind;
	return "{\"files\":[\"" + key + "\"],\"path\":\"" + path + "\"}";
}

static std::string manifest(const std::string &groups = "{}",
	const std::string &entry = "null", const std::string &args = "[]",
	const std::string &loose = "[]")
{
	return "{\"format\":2,\"release\":\"r1\",\"groups\":" + groups +
		",\"loose\":" + loose + ",\"entry_point\":" + entry +
		",\"run_arguments\":" + args + "}";
}

static launcher_status decode(const std::string &json, Descriptors &files,
	launcher_release **release, launcher_error *error = nullptr)
{
	return launcher_decode_release(json.data(), json.size(), "r1",
		Descriptors::lookup, &files, release, error);
}

TEST(Format, EmptyReleaseAllowsNoGroupsAndNullCallback)
{
	std::string json = manifest();
	launcher_release *release = nullptr;
	ASSERT_EQ(launcher_decode_release(json.data(), json.size(), "r1",
		nullptr, nullptr, &release, nullptr), LAUNCHER_OK);
	EXPECT_EQ(launcher_release_group_count(release), 0u);
	launcher_file_group selected{};
	EXPECT_EQ(launcher_release_group(release, 0, &selected, nullptr),
		LAUNCHER_INVALID);
	launcher_release_free(&release);
	EXPECT_EQ(release, nullptr);
	launcher_release_free(&release);
}

TEST(Format, GroupsExposeNamedFingerprintsAndReleaseLaunchSettings)
{
	Descriptors files;
	std::string a = named(files, "assets/data.bin");
	std::string b = named(files, "game.exe", "loose");
	std::string json =
		manifest("{\"assets\":" + group(a) + "}", "\"game.exe\"",
			"[\"--literal\",\"two words\"]", "[\"" + b + "\"]");
	launcher_release *release = nullptr;
	launcher_error error{};
	ASSERT_EQ(decode(json, files, &release, &error), LAUNCHER_OK)
		<< error.message;
	EXPECT_EQ(files.calls, 2u);
	EXPECT_EQ(launcher_release_group_count(release), 2u);
	launcher_file_group selected{};
	ASSERT_EQ(launcher_release_group(release, 0, &selected, nullptr),
		LAUNCHER_OK);
	EXPECT_STREQ(selected.name, "assets");
	EXPECT_STREQ(selected.hogg_path, "a.hogg");
	EXPECT_EQ(selected.file_count, 1u);
	launcher_file file{};
	ASSERT_EQ(launcher_release_group_file(release, 0, 0, &file, nullptr),
		LAUNCHER_OK);
	EXPECT_STREQ(file.path, "assets/data.bin");
	EXPECT_STREQ(file.sha256, digest);
	EXPECT_STREQ(file.fingerprint, a.c_str());
	launcher_launch_info launch{};
	ASSERT_EQ(launcher_release_get_launch(release, &launch, nullptr),
		LAUNCHER_OK);
	EXPECT_STREQ(launch.entry_point, "game.exe");
	EXPECT_EQ(launch.run_argument_count, 2u);
	EXPECT_STREQ(launch.run_arguments[1], "two words");
	launcher_release_free(&release);
}

TEST(Format, RejectUnsafeGroupsDraftsAndUnknownOrDuplicateFields)
{
	Descriptors files;
	for (std::string json : {manifest("{\"bad/name\":{}}"),
		manifest("{\"a\":{\"kind\":\"loose\",\"files\":[],"
			"\"path\":\"a\"}}"),
		manifest("{\"a\":{\"kind\":\"hogg\",\"files\":[],"
			"\"path\":\"../a\"}}"),
		manifest("{\"a\":{\"kind\":\"loose\",\"files\":[]},"
			"\"a\":{\"kind\":\"loose\",\"files\":[]}}")}) {
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(json, files, &release), LAUNCHER_METADATA);
		EXPECT_EQ(release, nullptr);
	}
	for (const std::string &extra : {",\"format\":1", ",\"unknown\":0"}) {
		std::string json = manifest();
		json.insert(json.size() - 1, extra);
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(json, files, &release), LAUNCHER_METADATA);
	}
	std::string json = manifest();
	json.insert(json.size() - 1, ",\"committed\":true");
	launcher_release *release = nullptr;
	EXPECT_EQ(decode(json, files, &release), LAUNCHER_METADATA);
}

TEST(Format, RejectDuplicateCanonicalNamesAcrossGroupsAndKindMismatch)
{
	Descriptors files;
	std::string a = named(files, "shared");
	std::string b = named(files, "shared", "loose");
	for (const std::string &groups : {
		"{\"a\":" + group(a) + ",\"b\":" + group(a, "hogg", "b.hogg") + "}",
		"{\"a\":" + group(a) + ",\"b\":" + group(b, "loose") + "}",
		"{\"a\":" + group(b) + "}"}) {
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(manifest(groups), files, &release),
			LAUNCHER_METADATA);
		EXPECT_EQ(release, nullptr);
	}
}

TEST(Format, RejectPhysicalArchiveAndLooseCollisions)
{
	Descriptors files;
	std::string a = named(files, "logical");
	std::string b = named(files, "piggs/main.hogg/child", "loose");
	for (const std::string &groups : {
		"{\"a\":" + group(a, "hogg", "piggs/main.hogg") +
			",\"b\":" + group(b, "loose") + "}",
		"{\"a\":" + group(a, "hogg", "same.hogg") +
			",\"b\":{\"kind\":\"hogg\",\"path\":\"same.hogg\",\"files\":[]}}"}) {
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(manifest(groups), files, &release),
			LAUNCHER_METADATA);
	}
}

TEST(Format, DescriptorHashAndCanonicalSerializationAreRequired)
{
	Descriptors files;
	std::string key = named(files, "asset");
	std::string json = manifest("{\"a\":" + group(key) + "}");
	files.bytes[key] += " ";
	launcher_release *release = nullptr;
	EXPECT_EQ(decode(json, files, &release), LAUNCHER_INTEGRITY);
	std::string bytes = descriptor(digest);
	bytes.erase(bytes.size() - 1);
	key = files.add(bytes);
	EXPECT_EQ(decode(manifest("{\"a\":" + group(key) + "}"), files,
		&release), LAUNCHER_METADATA);
}

TEST(Format, CanonicalUnicodeNamesAreEscapedAndAsciiIsFolded)
{
	Descriptors files;
	for (const std::string &name : {"caf\\u00e9.bin", "\\ud83d\\ude00.bin"}) {
		std::string key = named(files, name);
		launcher_release *release = nullptr;
		launcher_error error{};
		ASSERT_EQ(decode(manifest("{\"a\":" + group(key) + "}"),
			files, &release, &error), LAUNCHER_OK) << error.message;
		launcher_release_free(&release);
	}
	for (const std::string &name : {"Upper", "../a", "a\\\\b", "con", "a."}) {
		std::string key = named(files, name);
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(manifest("{\"a\":" + group(key) + "}"), files,
			&release), LAUNCHER_METADATA);
	}
}

TEST(Format, HeaderRepresentationsAndNumericLimitsRemainStrict)
{
	Descriptors files;
	for (const char *encoded : {"YQ==", "YWI=", "YWJj", "AP9oZWFkAA=="}) {
		std::string key = files.add(header_descriptor(descriptor(digest),
			encoded));
		launcher_release *release = nullptr;
		ASSERT_EQ(decode(manifest("{\"a\":" + group(key) + "}"), files,
			&release), LAUNCHER_OK);
		launcher_release_free(&release);
	}
	for (const char *encoded : {"", "YQ", "YR==", "YQ===", "@@=="}) {
		std::string key = files.add(header_descriptor(descriptor(digest),
			encoded));
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(manifest("{\"a\":" + group(key) + "}"), files,
			&release), LAUNCHER_METADATA);
	}
	for (auto sizes : {std::make_pair("3", "4"),
		std::make_pair("4294967295", "4294967295")}) {
		std::string key = files.add(descriptor(digest, sizes.first,
			sizes.second));
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(manifest("{\"a\":" + group(key) + "}"), files,
			&release), LAUNCHER_METADATA);
	}
}

TEST(Format, ChannelContainsOnlyItsCursor)
{
	char id[65];
	const char valid[] = " {\"release\":\"r1\",\"format\":1}\n";
	ASSERT_EQ(launcher_decode_channel(valid, sizeof(valid) - 1, "live",
		id, nullptr), LAUNCHER_OK);
	EXPECT_STREQ(id, "r1");
	for (const std::string &json : {
		"{\"format\":1,\"release\":\"r1\",\"entry_point\":\"game.exe\"}",
		"{\"format\":1,\"release\":\"r1\",\"release\":\"r2\"}",
		"{\"format\":1,\"release\":\"../r1\"}",
		"{\"format\":true,\"release\":\"r1\"}"})
		EXPECT_EQ(launcher_decode_channel(json.data(), json.size(), "live",
			id, nullptr), LAUNCHER_METADATA);
}

TEST(Format, ConcurrentDecodeHasIndependentOwnership)
{
	std::atomic<unsigned> failures{0};
	std::thread workers[8];
	for (auto &worker : workers)
		worker = std::thread([&failures] {
			Descriptors files;
			std::string key = named(files, "asset");
			launcher_release *release = nullptr;
			if (decode(manifest("{\"a\":" + group(key) + "}"), files,
				&release) != LAUNCHER_OK)
				++failures;
			launcher_release_free(&release);
		});
	for (auto &worker : workers)
		worker.join();
	EXPECT_EQ(failures, 0u);
}

TEST(Format, RejectOversizedMetadataAndExcessiveInventoryBeforeLookup)
{
	Descriptors files;
	launcher_release *release = nullptr;
	std::string oversized(16 * 1024 * 1024 + 1, ' ');
	EXPECT_EQ(decode(oversized, files, &release), LAUNCHER_METADATA);
	std::string key(64, 'a');
	std::string list;
	for (size_t i = 0; i < 100001; ++i) {
		if (i)
			list += ',';
		list += "\"" + key + "\"";
	}
	std::string json = manifest("{\"large\":{\"kind\":\"loose\","
		"\"files\":[" + list + "]}}");
	EXPECT_EQ(decode(json, files, &release), LAUNCHER_METADATA);
	EXPECT_EQ(files.calls, 0u);
}

TEST(Format, PreserveInclusiveLargeIntegerLengthsAndHeaderReferences)
{
	Descriptors files;
	for (const std::string &size : {"9007199254740993", "9223372036854775807"}) {
		std::string key = files.add(descriptor(digest, size, size, "raw",
			"900150983cd24fb0d6963f7d28e17f72", "large", "loose"));
		launcher_release *release = nullptr;
		ASSERT_EQ(decode(manifest("{}", "null", "[]",
					 "[\"" + key + "\"]"),
				  files, &release),
			LAUNCHER_OK);
		launcher_file selected{};
		ASSERT_EQ(launcher_release_group_file(release, 0, 0, &selected,
			nullptr), LAUNCHER_OK);
		EXPECT_EQ(selected.stored_size, std::stoull(size));
		launcher_release_free(&release);
	}
	for (const std::string &size : {"0", "16777217"}) {
		std::string key = files.add(object_header_descriptor(
			descriptor(digest), digest, size));
		launcher_release *release = nullptr;
		EXPECT_EQ(decode(manifest("{\"a\":" + group(key) + "}"), files,
			&release), LAUNCHER_METADATA);
	}
}

TEST(Format, DuplicateGroupStructureDoesNotInvokeDescriptorCallback)
{
	Descriptors files;
	std::string key = named(files, "asset");
	std::string json = manifest("{\"same\":" + group(key) +
		",\"same\":" + group(key, "hogg", "b.hogg") + "}");
	launcher_release *release = nullptr;
	EXPECT_EQ(decode(json, files, &release), LAUNCHER_METADATA);
	EXPECT_EQ(files.calls, 0u);
	EXPECT_EQ(release, nullptr);
}

TEST(Format, ImmutableFormatTwoHasTopLevelLooseFiles)
{
	Descriptors files;
	std::string key = named(files, "game.exe", "loose");
	std::string json = "{\"format\":2,\"release\":\"r1\","
		"\"groups\":{},\"loose\":[\"" + key +
		"\"],\"entry_point\":\"game.exe\",\"run_arguments\":[]}";
	launcher_release *release = nullptr;
	launcher_error error{};
	ASSERT_EQ(decode(json, files, &release, &error), LAUNCHER_OK)
		<< error.message;
	EXPECT_EQ(test_count(release, LAUNCHER_LOOSE), 1u);
	launcher_release_free(&release);
}
