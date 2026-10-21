#include "../descriptors.h"
#include <gtest/gtest.h>
#include <mbedtls/sha256.h>
#include <piggle/piggle.h>
#include "../support.h"
#include "../headers.h"
using launcher_test::Fixture;
using launcher_test::read;
using launcher_test::write;
namespace fs = std::filesystem;

TEST(Stored, CachedHeadersSurviveInstallHeaderOnlyUpdateAndOfflineRepair)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	std::string first("\0\xffhead\0", 7);
	launcher_test::publish_header(f.public_root, "r1", "r1",
		"assets/data.bin", first);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	auto archive = f.install / "piggs/assets.hogg";
	EXPECT_EQ(launcher_test::installed_header(archive, "assets/data.bin"),
		first);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	auto requests = f.server->object_requests.load();
	std::string second(65536, 'H');
	launcher_test::publish_header(f.public_root, "r1", "headers2",
		"assets/data.bin", second);
	f.pin("headers2");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->object_requests.load(), requests);
	EXPECT_EQ(launcher_test::installed_header(archive, "assets/data.bin"),
		second);
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_file *file = nullptr;
	pg_source_options options{PG_HOGG10, PG_WRITE, PG_CHECKSUM_LOGICAL};
	pg_metadata_options clear{PG_METADATA_HEADER, 0, nullptr, 0};
	pg_error error{};
	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_source_open(context, archive.string().c_str(), &options,
		&source, &error), PG_OK);
	ASSERT_EQ(pg_source_find(source, "assets/data.bin", &file, &error),
		PG_OK);
	ASSERT_EQ(pg_file_update_metadata(file, &clear, &error), PG_OK);
	ASSERT_EQ(pg_file_close(&file, &error), PG_OK);
	ASSERT_EQ(pg_source_close(&source, &error), PG_OK);
	ASSERT_EQ(pg_context_close(&context, &error), PG_OK);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	delete f.server;
	f.server = nullptr;
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(launcher_test::installed_header(archive, "assets/data.bin"),
		second);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}
TEST(Stored, HeaderObjectsSurviveInstallHeaderOnlyUpdateAndOfflineRepair)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	std::string first("\0\xffhead\0", 7);
	launcher_test::publish_header(f.public_root, "r1", "r1",
		"assets/data.bin", first, true);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	auto archive = f.install / "piggs/assets.hogg";
	EXPECT_EQ(launcher_test::installed_header(archive, "assets/data.bin"),
		first);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	auto requests = f.server->object_requests.load();
	std::string second(65536, 'H');
	launcher_test::publish_header(f.public_root, "r1", "headers2",
		"assets/data.bin", second, true);
	f.pin("headers2");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->object_requests.load(), requests + 1);
	EXPECT_EQ(launcher_test::installed_header(archive, "assets/data.bin"),
		second);
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_file *file = nullptr;
	pg_source_options options{PG_HOGG10, PG_WRITE, PG_CHECKSUM_LOGICAL};
	std::string damaged(second.size(), '!');
	pg_metadata_options clear{PG_METADATA_HEADER, 0, damaged.data(),
		damaged.size()};
	pg_error error{};
	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_source_open(context, archive.string().c_str(), &options,
		&source, &error), PG_OK);
	ASSERT_EQ(pg_source_find(source, "assets/data.bin", &file, &error),
		PG_OK);
	ASSERT_EQ(pg_file_update_metadata(file, &clear, &error), PG_OK);
	ASSERT_EQ(pg_file_close(&file, &error), PG_OK);
	ASSERT_EQ(pg_source_close(&source, &error), PG_OK);
	ASSERT_EQ(pg_context_close(&context, &error), PG_OK);
	std::string runner = fs::absolute(LAUNCHER_TEST_CHILD).string();
	const char *arguments[] = { "0", "0", "header-launch" };
	launcher_config config{};

	ASSERT_EQ(launcher_get_config(f.handle, &config, &f.error),
		LAUNCHER_OK);
	config.runner = runner.c_str();
	config.runner_args = arguments;
	config.runner_arg_count = 3;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	int code = -1;

	EXPECT_EQ(launcher_launch(f.handle, nullptr, 0, &code, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(code, 0);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	delete f.server;
	f.server = nullptr;
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(launcher_test::installed_header(archive, "assets/data.bin"),
		second);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}
TEST(Stored, BinaryHeadersSurviveEveryBase64TailLength)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	std::string previous = "r1";
	for (size_t length : {1u, 2u, 3u, 4u, 255u, 256u, 257u,
		65534u, 65535u, 65536u}) {
		std::string header(length, '\0');
		for (size_t i = 0; i < length; ++i)
			header[i] = (char)((i * 131 + 17) & 255);
		std::string release = "header" + std::to_string(length);
		launcher_test::publish_header(f.public_root, previous.c_str(),
			release.c_str(), "assets/data.bin", header);
		f.pin(release.c_str());
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		EXPECT_EQ(launcher_test::installed_header(f.install /
			"piggs/assets.hogg", "assets/data.bin"), header);
		previous = release;
	}
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}

namespace fs = std::filesystem;
#ifdef LAUNCHER_TEST_HASH_COUNTS
static std::atomic<unsigned> acceptance_hashes{0};
extern "C" int __real_mbedtls_sha256_finish(mbedtls_sha256_context *,
	unsigned char[32]);
extern "C" int __wrap_mbedtls_sha256_finish(mbedtls_sha256_context *context,
	unsigned char output[32])
{
	++acceptance_hashes;
	return __real_mbedtls_sha256_finish(context, output);
}
#endif

static std::string stored_entry(const fs::path &path, const char *name,
	pg_file_info *metadata = nullptr)
{
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_file *file = nullptr;
	pg_reader *reader = nullptr;
	pg_error error{};
	std::string bytes;
	EXPECT_EQ(pg_context_open(&context, &error), PG_OK);
	EXPECT_EQ(pg_source_open(context, path.string().c_str(), nullptr,
		&source, &error), PG_OK);
	EXPECT_EQ(pg_source_find(source, name, &file, &error), PG_OK);
	if (metadata)
		EXPECT_EQ(pg_file_inspect(file, metadata, &error), PG_OK);
	EXPECT_EQ(pg_reader_open(file, PG_READ_STORED_UNCHECKED, &reader,
		&error), PG_OK);
	for (;;) {
		char chunk[1024];
		size_t count = 0;
		pg_status status = pg_reader_read(reader, chunk, sizeof(chunk),
			&count, &error);
		if (status == PG_END)
			break;
		EXPECT_EQ(status, PG_OK);
		if (status != PG_OK)
			break;
		bytes.append(chunk, count);
	}
	EXPECT_EQ(pg_reader_close(&reader, &error), PG_OK);
	EXPECT_EQ(pg_file_close(&file, &error), PG_OK);
	EXPECT_EQ(pg_source_close(&source, &error), PG_OK);
	EXPECT_EQ(pg_context_close(&context, &error), PG_OK);
	return bytes;
}

TEST(Stored, PublisherBytesAndLogicalMetadataSurviveInstallation)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure("compressed"), LAUNCHER_OK);
	launcher_release *release = nullptr;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	launcher_file file{};
	ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_HOGG, 0, &file,
		&f.error), LAUNCHER_OK);
	EXPECT_STREQ(file.path, "a.bin");
	EXPECT_EQ(file.size, 4096u);
	EXPECT_EQ(file.encoding, LAUNCHER_ZLIB);
	EXPECT_NE(file.logical_md5, nullptr);
	std::string expected = read(f.public_root / "hogg" / file.sha256 / "BLOB");
	EXPECT_EQ(file.stored_size, expected.size());
	ASSERT_LT(file.stored_size, file.size);
	ASSERT_EQ(launcher_make_plan(f.handle, release, &plan, nullptr,
		nullptr, &f.error), LAUNCHER_OK) << f.error.message;
	launcher_plan_info info{};
	ASSERT_EQ(launcher_plan_get_info(plan, &info, &f.error), LAUNCHER_OK);
	EXPECT_EQ(info.missing_objects, 3u);
	EXPECT_EQ(info.download_bytes, expected.size() + 256);
	ASSERT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->object_requests, 3u);
	pg_file_info metadata{};
	EXPECT_EQ(stored_entry(f.install / "piggs/assets.hogg", "a.bin",
		&metadata), expected);
	EXPECT_EQ(metadata.logical_size, file.size);
	EXPECT_EQ(metadata.stored_size, file.stored_size);
	EXPECT_EQ(metadata.encoding, PG_ZLIB);
	EXPECT_EQ(metadata.checksum_domain, PG_CHECKSUM_LOGICAL);
	for (size_t i = 0; i < 4; ++i)
		EXPECT_EQ(metadata.digest[i], std::stoul(
			std::string(file.logical_md5 + i * 2, 2), nullptr, 16));
	EXPECT_EQ(stored_entry(f.install / "piggs/assets.hogg", "b.bin"),
		expected);
	EXPECT_EQ(stored_entry(f.install / "piggs/assets.hogg", "encoded.bin"),
		expected);
	EXPECT_EQ(read(f.install / "stored.bin"), expected);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_plan_free(&plan);
	launcher_release_free(&release);
}

TEST(Stored, UnchangedUpdateSkipsCachePopulationAndPayloadScans)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	fs::remove_all(f.base / "game.launcher/objects");
	write(f.install / "data.bin", "bad");
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_release *release = nullptr;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	ASSERT_EQ(launcher_make_plan(f.handle, release, &plan, nullptr,
		nullptr, &f.error), LAUNCHER_OK);
	launcher_plan_info info{};
	ASSERT_EQ(launcher_plan_get_info(plan, &info, &f.error), LAUNCHER_OK);
	EXPECT_TRUE(info.unchanged);
	EXPECT_EQ(info.download_bytes, 0u);
	EXPECT_EQ(info.missing_objects, 0u);
	EXPECT_TRUE(!fs::exists(f.base / "game.launcher/objects") ||
		fs::is_empty(f.base / "game.launcher/objects"));
	EXPECT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(read(f.install / "data.bin"), "bad");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	launcher_plan_free(&plan);
	launcher_release_free(&release);
}

TEST(Stored, ExplicitVerificationEvictsCorruptCacheAndInvalidatesPlans)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	launcher_file entry{};
	ASSERT_EQ(launcher_test::test_file(installed, LAUNCHER_LOOSE, 0, &entry,
		&f.error), LAUNCHER_OK);
	fs::path cached = f.base / "game.launcher/objects" / entry.sha256;
	write(cached, "bad");
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_make_plan(f.handle, installed, &plan, nullptr,
		nullptr, &f.error), LAUNCHER_OK);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	EXPECT_FALSE(fs::exists(cached));
	EXPECT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_INVALID);
	launcher_plan_free(&plan);
	write(f.install / "data.bin", "bad");
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(read(f.install / "data.bin"), "abc");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
}
#ifdef LAUNCHER_TEST_HASH_COUNTS
TEST(Stored, HashEachAcquiredObjectOnceAndTrustItAcrossRoutineOperations)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	acceptance_hashes = 0;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(acceptance_hashes, 2u);
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	acceptance_hashes = 0;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(acceptance_hashes, 0u);
	std::string runner = fs::absolute(LAUNCHER_TEST_CHILD).string();
	const char *args[] = { "0", "0", "arguments" };
	launcher_config config{};
	config.base_url = f.server->url.c_str();
	launcher_file_repository files{f.server->url.c_str(), nullptr};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.game = "game.exe";
	config.runner = runner.c_str();
	config.runner_args = args;
	config.runner_arg_count = 3;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	int code = -1;
	ASSERT_EQ(launcher_launch(f.handle, nullptr, 0, &code, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(code, 0);
	EXPECT_EQ(acceptance_hashes, 0u);
	ASSERT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(acceptance_hashes, 5u);
}
#endif

static void seed_archive(Fixture &f, const launcher_file &entry,
	const std::string &bytes)
{
	fs::create_directories(f.install / "piggs");
	pg_context *context = nullptr;
	pg_archive_builder *builder = nullptr;
	pg_writer *writer = nullptr;
	pg_error error{};
	pg_stored_write_options options{};
	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_create(context,
		(f.install / "piggs/assets.hogg").string().c_str(), PG_HOGG10,
		PG_OVERWRITE, &builder, &error), PG_OK);
	options.logical_size = entry.size;
	options.stored_size = bytes.size();
	options.encoding = PG_ZLIB;
	options.digest_kind = PG_DIGEST_MD5;
	for (size_t i = 0; i < 16; i++)
		options.digest[i] = (uint8_t)std::stoul(
			std::string(entry.logical_md5 + i * 2, 2), nullptr, 16);
	ASSERT_EQ(pg_writer_open_archive_builder_stored(builder, entry.path,
		&options, &writer, &error), PG_OK);
	size_t count;
	ASSERT_EQ(pg_writer_write(writer, bytes.data(), bytes.size(), &count,
		&error), PG_OK);
	ASSERT_EQ(pg_writer_finish(writer, &error), PG_OK);
	ASSERT_EQ(pg_writer_close(&writer, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_finish(builder, &error), PG_OK);
	ASSERT_EQ(pg_archive_builder_close(&builder, &error), PG_OK);
	ASSERT_EQ(pg_context_close(&context, &error), PG_OK);
}

TEST(Stored, AdoptionRequiresTheExactDesiredCompressionStream)
{
	for (bool different : {false, true}) {
		Fixture f;
		std::string json = read(
			f.public_root / "releases/compressed.json");
		launcher_test::FileDescriptors descriptors{f.public_root, {}};
		launcher_release *release = nullptr;
		ASSERT_EQ(launcher_decode_release(json.data(), json.size(),
			"compressed", launcher_test::FileDescriptors::lookup,
			&descriptors, &release, &f.error), LAUNCHER_OK);
		launcher_file entry{};
		ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_HOGG, 0,
			&entry, &f.error), LAUNCHER_OK);
		std::string expected = read(
			f.public_root / "hogg" / entry.sha256 / "BLOB");
		std::string bytes = expected;
		if (different)
			bytes[1] = '\x01';
		seed_archive(f, entry, bytes);
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure("compressed"), LAUNCHER_OK);
#ifdef LAUNCHER_TEST_HASH_COUNTS
		acceptance_hashes = 0;
#endif
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		EXPECT_EQ(f.server->object_requests, different ? 3u : 2u);
#ifdef LAUNCHER_TEST_HASH_COUNTS
		EXPECT_EQ(acceptance_hashes, different ? 4u : 3u);
#endif
		EXPECT_EQ(stored_entry(f.install / "piggs/assets.hogg",
			"a.bin"),
			expected);
		launcher_release_free(&release);
	}
}

TEST(Stored, RepairCopiesAcceptedCompressedBytesUntilExplicitVerification)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure("compressed"), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	const launcher_release *release = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &release,
		&f.error), LAUNCHER_OK);
	launcher_file entry{};
	ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_HOGG, 0, &entry,
		&f.error), LAUNCHER_OK);
	fs::path cached = f.base / "game.launcher/objects" / entry.sha256;
	std::string expected = read(cached);
	std::string bad(expected.size(), '!');
	write(cached, bad);
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
#ifdef LAUNCHER_TEST_HASH_COUNTS
	acceptance_hashes = 0;
#endif
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
#ifdef LAUNCHER_TEST_HASH_COUNTS
	EXPECT_EQ(acceptance_hashes, 0u);
#endif
	EXPECT_EQ(stored_entry(f.install / "piggs/assets.hogg", "a.bin"), bad);
	EXPECT_EQ(f.server->object_requests, 3u);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	EXPECT_FALSE(fs::exists(cached));
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->object_requests, 4u);
	EXPECT_EQ(stored_entry(f.install / "piggs/assets.hogg",
		"a.bin"), expected);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
}

TEST(Stored, ExplicitVerificationAlsoEvictsUnreferencedCacheObjects)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	std::string digest =
		"e3b0c44298fc1c149afbf4c8996fb92427"
		"ae41e4649b934ca495991b7852b855";
	fs::path cached = f.base / "game.launcher/objects" / digest;
	write(cached, "changed");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	EXPECT_FALSE(fs::exists(cached));
	write(cached, "");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_TRUE(fs::exists(cached));
}

static int count_staging_entries(const launcher_progress *progress, void *state)
{
	unsigned *calls = (unsigned *)state;

	if (progress->phase == LAUNCHER_STAGE) {
		++*calls;
		EXPECT_LE(progress->completed, progress->total);
		EXPECT_LE(progress->total, 6u);
	}
	return 1;
}

TEST(Stored, StagingProgressStillCountsEntries)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure("compressed"), LAUNCHER_OK);
	unsigned calls = 0;

	ASSERT_EQ(launcher_update(f.handle, count_staging_entries, &calls,
		&f.error), LAUNCHER_OK) << f.error.message;
	EXPECT_GT(calls, 0u);
}

#ifdef LAUNCHER_TEST_HASH_COUNTS
TEST(Stored, DescriptorValidationIsSeparateFromPayloadHashInstrumentation)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	acceptance_hashes = 0;
	launcher_release *release = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	// One-shot descriptor hashes are excluded by the streaming wrapper.
	EXPECT_EQ(acceptance_hashes, 0u);
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_make_plan(f.handle, release, &plan, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	EXPECT_EQ(acceptance_hashes, 0u);
	ASSERT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(acceptance_hashes, 2u);
	launcher_plan_free(&plan);
	launcher_release_free(&release);
}
#endif

TEST(Stored, InstalledStateRetainsExactDescriptorsWithoutExternalCaches)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	launcher_file entry{};
	ASSERT_EQ(launcher_test::test_file(installed, LAUNCHER_LOOSE, 0, &entry,
		&f.error), LAUNCHER_OK);
	std::string key = entry.fingerprint;
	std::string bytes = read(launcher_test::record_path(f.public_root, key));
	launcher_close(&f.handle);
	fs::remove_all(f.base / "game.launcher/files");
	fs::remove_all(f.public_root);
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	ASSERT_EQ(launcher_test::test_file(installed, LAUNCHER_LOOSE, 0, &entry,
		&f.error), LAUNCHER_OK);
	EXPECT_EQ(std::string(entry.fingerprint), key);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	write(f.install / "data.bin", "bad");
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(read(f.install / "data.bin"), "abc");
	std::string state = read(f.base / "game.launcher/installed-state.json");
	std::string escaped;
	for (char byte : bytes) {
		if (byte == '\n')
			escaped += "\\n";
		else if (byte == '"')
			escaped += "\\\"";
		else
			escaped += byte;
	}
	EXPECT_NE(state.find(escaped), std::string::npos);
	EXPECT_EQ(f.server->descriptor_requests, 3u);
}

TEST(Stored, CorruptHeaderObjectCannotBeInstalled)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	const std::string header("\0\xffheader", 8);
	launcher_test::publish_header(f.public_root, "r1", "r1",
		"assets/data.bin", header, true);
	for (const auto &item : fs::recursive_directory_iterator(f.public_root))
		if (item.path().parent_path().filename() == "headers" &&
			item.path().filename() == launcher_test::fingerprint(header))
			write(item.path(), std::string(header.size(), '!'));
	EXPECT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	const launcher_release *installed = nullptr;
	EXPECT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_NOT_INSTALLED);
}

TEST(Stored, ValidInstalledHeadersRestoreMissingHeaderObjectsOffline)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	const std::string header("\0\xffheader", 8);
	launcher_test::publish_header(f.public_root, "r1", "r1",
		"assets/data.bin", header, true);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	auto object = fs::path(f.install.string() + ".launcher") /
		"objects" / launcher_test::fingerprint(header);
	ASSERT_TRUE(fs::remove(object));
	delete f.server;
	f.server = nullptr;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(read(object), header);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}

TEST(Stored, HeaderObjectSharingPayloadBytesDownloadsThatObjectOnce)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	launcher_test::publish_header(f.public_root, "r1", "r1",
		"assets/data.bin", "abc", true);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->object_requests.load(), 2u);
	EXPECT_EQ(launcher_test::installed_header(f.install /
		"piggs/assets.hogg", "assets/data.bin"), "abc");
}

TEST(Reuse, HeaderOnlyChangeRebuildsOneArchiveAndPreservesAnother)
{
	Fixture f;
	launcher_test::multiple_archives(f.public_root, "r1");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	auto second = f.install / "piggs/second.hogg";
	auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(24);
	fs::last_write_time(second, stamp);
	stamp = fs::last_write_time(second);
	launcher_test::publish_header(f.public_root, "r1", "headers2",
		"assets/data.bin", "changed header");
	f.pin("headers2");
	auto cancel_staging = [](const launcher_progress *progress, void *) -> int {
		return progress->phase != LAUNCHER_STAGE;
	};
	EXPECT_EQ(launcher_update(f.handle, cancel_staging, nullptr, &f.error),
		LAUNCHER_CANCELLED);
	EXPECT_EQ(fs::last_write_time(second), stamp);
	EXPECT_EQ(launcher_test::installed_header(f.install /
		"piggs/assets.hogg", "assets/data.bin"), "");
	EXPECT_FALSE(fs::exists(f.base / "game.launcher/journal.json"));
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(fs::last_write_time(second), stamp);
	EXPECT_EQ(launcher_test::installed_header(f.install /
		"piggs/assets.hogg", "assets/data.bin"), "changed header");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
}

#ifdef LAUNCHER_TEST_HASH_COUNTS
TEST(Stored, FailedInstalledCandidatesAreHashedOnce)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	fs::remove(f.base / "game.launcher/objects" /
		launcher_test::fingerprint("game-one\n"));
	write(f.install / "game.exe", "damaged!!");
	acceptance_hashes = 0;
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(acceptance_hashes, 2u);
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
}

TEST(Stored, WrongLengthInstalledCandidateNeedsOnlyDownloadHash)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	fs::remove(f.base / "game.launcher/objects" /
		launcher_test::fingerprint("game-one\n"));
	write(f.install / "game.exe", "tiny");
	acceptance_hashes = 0;
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(acceptance_hashes, 1u);
}

TEST(Stored, FailedArchiveCandidateIsNotRetriedThroughBothInventories)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure("compressed"), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	launcher_file entry{};
	ASSERT_EQ(launcher_test::test_file(installed, LAUNCHER_HOGG, 0,
		&entry, &f.error), LAUNCHER_OK);
	auto cache = f.base / "game.launcher/objects" / entry.sha256;
	std::string bad = read(cache);
	ASSERT_FALSE(bad.empty());
	bad[1] = '\x01';
	seed_archive(f, entry, bad);
	fs::remove(cache);
	fs::remove(f.install / "stored.bin");
	acceptance_hashes = 0;
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(acceptance_hashes, 2u);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
}
#endif
