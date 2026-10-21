#include <gtest/gtest.h>
#include <piggle/piggle.h>
#include <yyjson.h>
#include "../support.h"

using launcher_test::Fixture;
using launcher_test::read;
using launcher_test::write;
namespace fs = std::filesystem;

TEST(Workflow, ConfigureSurvivesCloseAndDefaultsToLive)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(nullptr), LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(f.handle, &config, &f.error),
		LAUNCHER_OK);
	EXPECT_STREQ(config.base_url, f.server->url.c_str());
	EXPECT_STREQ(config.channel, "live");
	EXPECT_STREQ(config.game, "game.exe");
}

TEST(Workflow, InstallDeduplicatesAndVerifiesOffline)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->object_requests, 2u);
	EXPECT_EQ(read(f.install / "data.bin"), "abc");
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
	EXPECT_TRUE(fs::is_regular_file(f.install / "piggs/assets.hogg"));
	fs::remove_all(f.public_root);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	EXPECT_STREQ(launcher_release_id(installed), "r1");
}

TEST(Workflow, UpdatesRepairsAndRollsBackPreservingUnrelatedFiles)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	write(f.install / "notes/user.txt", "keep me");
	f.pin("r2");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(read(f.install / "game.exe"), "game-two\n");
	EXPECT_FALSE(fs::exists(f.install / "data.bin"));
	EXPECT_EQ(read(f.install / "docs/info.txt"), "new-data\n");
	write(f.install / "game.exe", "damaged");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	f.pin("r1");
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(read(f.install / "game.exe"), "game-two\n");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
	EXPECT_FALSE(fs::exists(f.install / "docs/info.txt"));
	EXPECT_EQ(read(f.install / "notes/user.txt"), "keep me");
}

TEST(Workflow, AdoptsDesiredFilesAndKeepsUnrelatedLooseFiles)
{
	Fixture f;
	write(f.install / "game.exe", "existing unmanaged version");
	write(f.install / "data.bin", "abc");
	write(f.install / "settings.ini", "personal settings");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
	EXPECT_EQ(read(f.install / "data.bin"), "abc");
	EXPECT_EQ(read(f.install / "settings.ini"), "personal settings");
	EXPECT_EQ(f.server->object_requests, 1u);
}

TEST(Workflow, EmptyReleaseRemovesManagedFilesAndKeepsEmptyHogg)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	f.pin("empty");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_FALSE(fs::exists(f.install / "game.exe"));
	EXPECT_FALSE(fs::exists(f.install / "data.bin"));
	EXPECT_TRUE(fs::exists(f.install / "piggs/assets.hogg"));
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}

TEST(Workflow, FailedObjectLeavesInstalledReleaseUnchanged)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	f.pin("r2");
	for (const auto &item : fs::recursive_directory_iterator(f.public_root))
		if (item.path().filename() == "BLOB")
			write(item.path(), "corrupt");
	EXPECT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY);
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}

static int cancel(const launcher_progress *progress, void *)
{
	return progress->phase != LAUNCHER_DOWNLOAD;
}

TEST(Workflow, CancellationLeavesActiveFilesUntouched)
{
	Fixture f;
	write(f.install / "game.exe", "original");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(launcher_update(f.handle, cancel, nullptr, &f.error),
		LAUNCHER_CANCELLED);
	EXPECT_EQ(read(f.install / "game.exe"), "original");
	const launcher_release *installed = nullptr;
	EXPECT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_NOT_INSTALLED);
	EXPECT_EQ(installed, nullptr);
}

TEST(Workflow, RepairRequiresSavedManifestAndPlanBindsConfiguration)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_NOT_INSTALLED);
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	launcher_release *target = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &target, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_make_plan(f.handle, target, &plan, nullptr,
		nullptr, &f.error), LAUNCHER_OK) << f.error.message;
	launcher_plan_info info{};
	ASSERT_EQ(launcher_plan_get_info(plan, &info, &f.error), LAUNCHER_OK);
	EXPECT_EQ(info.missing_objects, 2u);
	EXPECT_EQ(info.download_bytes, 12u);
	EXPECT_EQ(info.hogg_files, 1u);
	EXPECT_EQ(info.loose_files, 2u);
	ASSERT_EQ(f.configure("next"), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_INVALID);
	launcher_plan_free(&plan);
	launcher_release_free(&target);
	EXPECT_EQ(plan, nullptr);
	EXPECT_EQ(target, nullptr);
}

TEST(Workflow, SameNameLaunchChangePersistsAndRepairKeepsInstalledSnapshot)
{
	Fixture f;
	launcher_test::publish_launch(f.public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r1\",\"entry_point\":\"game.exe\","
		"\"run_arguments\":[\"first\"]}");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_test::publish_launch(f.public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r1\",\"entry_point\":\"game.exe\","
		"\"run_arguments\":[\"second\"]}");
	launcher_release *target = nullptr;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &target, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_make_plan(f.handle, target, &plan, nullptr,
		nullptr, &f.error), LAUNCHER_OK);
	launcher_plan_info details{};
	ASSERT_EQ(launcher_plan_get_info(plan, &details, &f.error),
		LAUNCHER_OK);
	EXPECT_FALSE(details.unchanged);
	ASSERT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_plan_free(&plan);
	launcher_release_free(&target);
	f.pin("r2");
	write(f.install / "game.exe", "damaged");
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	launcher_launch_info info{};
	ASSERT_EQ(launcher_release_get_launch(installed, &info, &f.error),
		LAUNCHER_OK);
	ASSERT_EQ(info.run_argument_count, 1u);
	EXPECT_STREQ(info.run_arguments[0], "second");
	EXPECT_STREQ(launcher_release_id(installed), "r1");
	launcher_test::publish_launch(f.public_root / "channels/live.json",
		"{\"release\":\"r1\",\"entry_point\":null,"
		"\"run_arguments\":[]}");
	f.pin("r1");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	ASSERT_EQ(launcher_release_get_launch(installed, &info, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(info.entry_point, nullptr);
	EXPECT_EQ(info.run_argument_count, 0u);
}

TEST(Workflow, InstalledSnapshotWorksWithoutChannelSnapshot)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	fs::path path = f.install.string() + ".launcher/installed-state.json";
	std::string bytes = read(path);
	yyjson_doc *doc = yyjson_read(bytes.data(), bytes.size(), 0);
	ASSERT_NE(doc, nullptr);
	yyjson_mut_doc *copy = yyjson_doc_mut_copy(doc, nullptr);
	ASSERT_NE(copy, nullptr);
	yyjson_mut_obj_remove_str(yyjson_mut_doc_get_root(copy), "channel");
	size_t size = 0;
	char *saved = yyjson_mut_write(copy, 0, &size);
	ASSERT_NE(saved, nullptr);
	write(path, std::string(saved, size));
	free(saved);
	yyjson_mut_doc_free(copy);
	yyjson_doc_free(doc);
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	launcher_launch_info info{};
	ASSERT_EQ(launcher_release_get_launch(installed, &info, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(info.entry_point, nullptr);
	EXPECT_EQ(info.run_argument_count, 0u);
}

TEST(Workflow, RebuildsAdoptedArchiveWithExactPublisherMembership)
{
	Fixture f;
	fs::create_directories(f.install / "piggs");
	pg_context *context = nullptr;
	pg_archive_builder *builder = nullptr;
	pg_error detail{};
	ASSERT_EQ(pg_context_open(&context, &detail), PG_OK);
	std::string path = (f.install / "piggs/assets.hogg").string();
	ASSERT_EQ(pg_archive_builder_create(context, path.c_str(), PG_HOGG10,
		0, &builder, &detail), PG_OK) << detail.message;
	ASSERT_EQ(pg_archive_builder_write_all(builder, "old/entry.bin",
		"old", 3, nullptr, &detail), PG_OK) << detail.message;
	ASSERT_EQ(pg_archive_builder_finish(builder, &detail), PG_OK)
		<< detail.message;
	ASSERT_EQ(pg_archive_builder_close(&builder, &detail), PG_OK);
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	pg_source *source = nullptr;
	pg_source_options options{PG_HOGG10, PG_READ, PG_CHECKSUM_LOGICAL};
	ASSERT_EQ(pg_source_open(context, path.c_str(), &options, &source,
		&detail), PG_OK) << detail.message;
	pg_cursor *cursor = nullptr;
	ASSERT_EQ(pg_source_files(source, nullptr, &cursor, &detail), PG_OK);
	pg_file *file = nullptr;
	ASSERT_EQ(pg_cursor_next(cursor, &file, &detail), PG_OK);
	pg_file_info info{};
	ASSERT_EQ(pg_file_inspect(file, &info, &detail), PG_OK);
	EXPECT_STREQ(info.canonical_name, "assets/data.bin");
	EXPECT_EQ(info.logical_size, 3u);
	pg_reader *reader = nullptr;
	ASSERT_EQ(pg_reader_open(file, PG_READ_LOGICAL, &reader, &detail),
		PG_OK);
	char bytes[8]{};
	size_t got = 0;
	pg_status read_status = pg_reader_read(reader, bytes, sizeof(bytes),
		&got, &detail);
	EXPECT_TRUE(read_status == PG_OK || read_status == PG_END);
	EXPECT_EQ(got, 3u);
	EXPECT_EQ(std::string(bytes, got), "abc");
	EXPECT_EQ(pg_reader_close(&reader, &detail), PG_OK);
	EXPECT_EQ(pg_file_close(&file, &detail), PG_OK);
	EXPECT_EQ(pg_cursor_next(cursor, &file, &detail), PG_END);
	EXPECT_EQ(pg_cursor_close(&cursor, &detail), PG_OK);
	EXPECT_EQ(pg_source_close(&source, &detail), PG_OK);
	EXPECT_EQ(pg_context_close(&context, &detail), PG_OK);
}

TEST(Workflow, InstallsEmptyAndBinaryPublisherObjectsWithoutConversion)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure("binary"), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	std::string binary;
	for (int i = 0; i < 256; ++i)
		binary.push_back((char)i);
	binary.append("\0\xff\r\n", 4);
	EXPECT_EQ(read(f.install / "loose-binary.bin"), binary);
	EXPECT_EQ(fs::file_size(f.install / "loose-zero.bin"), 0u);
	EXPECT_EQ(f.server->object_requests, 2u);
	ASSERT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_error detail{};
	pg_source_options options{PG_HOGG10, PG_READ, PG_CHECKSUM_LOGICAL};
	ASSERT_EQ(pg_context_open(&context, &detail), PG_OK);
	std::string path = (f.install / "piggs/assets.hogg").string();
	ASSERT_EQ(pg_source_open(context, path.c_str(), &options, &source,
		&detail), PG_OK) << detail.message;
	const char *names[] = {"binary/payload.bin", "zero.bin"};
	for (size_t i = 0; i < 2; ++i) {
		pg_reader *reader = nullptr;
		ASSERT_EQ(pg_reader_open_source(source, names[i],
			PG_READ_LOGICAL, &reader, &detail), PG_OK)
			<< detail.message;
		char bytes[512]{};
		size_t got = 0;
		pg_status status = pg_reader_read(reader, bytes, sizeof(bytes),
			&got, &detail);
		EXPECT_TRUE(status == PG_OK || status == PG_END);
		EXPECT_EQ(std::string(bytes, got), i ? "" : binary);
		EXPECT_EQ(pg_reader_close(&reader, &detail), PG_OK);
	}
	EXPECT_EQ(pg_source_close(&source, &detail), PG_OK);
	EXPECT_EQ(pg_context_close(&context, &detail), PG_OK);
}

TEST(Workflow, MultipleArchivesRelocateAndRemoveAsOneInventory)
{
	Fixture f;
	launcher_test::multiple_archives(f.public_root);
	f.pin("multi");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_TRUE(fs::exists(f.install / "piggs/assets.hogg"));
	EXPECT_TRUE(fs::exists(f.install / "piggs/second.hogg"));
	ASSERT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_test::edit_json(f.public_root / "releases/multi.json",
		[](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			yyjson_mut_val *groups = yyjson_mut_obj_get(root, "groups");

			yyjson_mut_obj_remove_str(groups, "assets");
			yyjson_mut_obj_put(yyjson_mut_obj_get(groups, "second"),
				yyjson_mut_str(doc, "path"),
				yyjson_mut_str(doc, "relocated/second.hogg"));
		});
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_FALSE(fs::exists(f.install / "piggs/assets.hogg"));
	EXPECT_FALSE(fs::exists(f.install / "piggs/second.hogg"));
	EXPECT_TRUE(fs::exists(f.install / "relocated/second.hogg"));
	write(f.install / "personal.txt", "keep");
	launcher_test::edit_json(f.public_root / "releases/multi.json",
		[](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			yyjson_mut_obj_put(root, yyjson_mut_str(doc, "groups"),
				yyjson_mut_obj(doc));
			yyjson_mut_obj_put(root, yyjson_mut_str(doc, "loose"),
				yyjson_mut_arr(doc));
		});
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_FALSE(fs::exists(f.install / "relocated/second.hogg"));
	EXPECT_FALSE(fs::exists(f.install / "game.exe"));
	EXPECT_EQ(read(f.install / "personal.txt"), "keep");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
}

TEST(Workflow, PublisherUnicodeCanonicalNamesSurviveArchiveInstallation)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure("unicode"), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	const launcher_release *release = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &release, &f.error),
		LAUNCHER_OK);
	launcher_file file{};
	ASSERT_EQ(launcher_release_group_file(release, 0, 0, &file, &f.error),
		LAUNCHER_OK);
	EXPECT_STREQ(file.path, "Ä/cafÉ.bin");
}

TEST(Workflow, RemovedPayloadAfterPlanningFallsBackWithoutDiagnosticOutput)
{
	Fixture f;
	fs::path first_root = f.base / "first";
	fs::copy(f.public_root, first_root, fs::copy_options::recursive);
	launcher_test::HttpServer first(first_root);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config{};
	launcher_file_repository files[] = {
		{first.url.c_str(), nullptr}, {f.server->url.c_str(), nullptr}
	};
	config.base_url = f.server->url.c_str();
	config.file_repositories = files;
	config.file_repository_count = 2;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	launcher_release *release = nullptr;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_make_plan(f.handle, release, &plan, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	for (const auto &item : fs::recursive_directory_iterator(first_root))
		if (item.path().filename() == "BLOB")
			fs::remove(item.path());
	EXPECT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, nullptr),
		LAUNCHER_OK);
	EXPECT_EQ(first.object_requests, 2u);
	EXPECT_EQ(f.server->object_requests, 2u);
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
	launcher_plan_free(&plan);
	launcher_release_free(&release);
}

TEST(Workflow, RemovedHeaderAfterPlanningFallsBackToLiveProvider)
{
	Fixture f;
	fs::path first_root = f.base / "first";
	fs::copy(f.public_root, first_root, fs::copy_options::recursive);
	launcher_test::HttpServer first(first_root);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config{};
	launcher_file_repository files[] = {
		{first.url.c_str(), nullptr}, {f.server->url.c_str(), nullptr}
	};
	config.base_url = f.server->url.c_str();
	config.channel = "headerobjects";
	config.file_repositories = files;
	config.file_repository_count = 2;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	launcher_release *release = nullptr;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_make_plan(f.handle, release, &plan, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	for (const auto &item : fs::recursive_directory_iterator(first_root))
		if (item.path().parent_path().filename() == "headers")
			fs::remove(item.path());
	ASSERT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->object_requests, 2u);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_plan_free(&plan);
	launcher_release_free(&release);
}

TEST(Workflow, MissingImmutableDescriptorFallsBackToAnotherProvider)
{
	Fixture f;
	fs::path first_root = f.base / "first";
	fs::copy(f.public_root, first_root, fs::copy_options::recursive);
	std::string json = read(first_root / "releases/r1.json");
	launcher_test::rename_in_manifest(first_root, json,
		"assets/data.bin", "other");
	yyjson_doc *doc = yyjson_read(json.data(), json.size(), 0);
	yyjson_val *groups = yyjson_obj_get(yyjson_doc_get_root(doc), "groups");
	std::string key = yyjson_get_str(yyjson_arr_get(yyjson_obj_get(
		yyjson_obj_get(groups, "assets"), "files"), 0));
	yyjson_doc_free(doc);
	fs::remove(launcher_test::record_path(first_root, key));
	launcher_test::HttpServer first(first_root);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config{};
	launcher_file_repository files[] = {
		{first.url.c_str(), nullptr}, {f.server->url.c_str(), nullptr}
	};
	config.base_url = f.server->url.c_str();
	config.file_repositories = files;
	config.file_repository_count = 2;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(first.descriptor_requests, 3u);
	EXPECT_EQ(f.server->descriptor_requests, 1u);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}

static void copy_release(Fixture &fixture, const char *name,
	bool command_only = false)
{
	fs::path target = fixture.public_root / "releases" /
		(std::string(name) + ".json");
	write(target, read(fixture.public_root / "releases/r1.json"));
	launcher_test::edit_json(target, [&](yyjson_mut_doc *doc,
		yyjson_mut_val *root) {
		yyjson_mut_obj_put(root, yyjson_mut_str(doc, "release"),
			yyjson_mut_strcpy(doc, name));
		if (command_only) {
			yyjson_mut_val *arguments = yyjson_mut_arr(doc);
			yyjson_mut_arr_add_str(doc, arguments, "updated");
			yyjson_mut_obj_put(root, yyjson_mut_str(doc, "entry_point"),
				yyjson_mut_str(doc, "game.exe"));
			yyjson_mut_obj_put(root, yyjson_mut_str(doc, "run_arguments"),
				arguments);
		}
	});
	fixture.pin(name);
}

TEST(Reuse, MetadataOnlyAndCopiedReleasesPreservePhysicalFilesWithoutCache)
{
	for (bool command_only : {false, true}) {
		Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		const char *name = command_only ? "r1" : "copied";
		auto stamp = fs::file_time_type::clock::now() -
			std::chrono::hours(24);
		std::map<fs::path, fs::file_time_type> retained;

		for (const auto &item : fs::recursive_directory_iterator(f.install)) {
			if (!item.is_regular_file())
				continue;
			fs::last_write_time(item.path(), stamp);
			retained[item.path()] = fs::last_write_time(item.path());
		}
		fs::remove_all(f.base / "game.launcher/objects");
		copy_release(f, name, command_only);
		launcher_close(&f.handle);
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		for (const auto &item : retained)
			EXPECT_EQ(fs::last_write_time(item.first), item.second)
				<< item.first;
		EXPECT_TRUE(fs::is_empty(f.base / "game.launcher/objects"));
		const launcher_release *installed = nullptr;
		ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
			LAUNCHER_OK);
		EXPECT_STREQ(launcher_release_id(installed), name);
		if (command_only) {
			launcher_launch_info launch{};
			ASSERT_EQ(launcher_release_get_launch(installed, &launch,
				&f.error), LAUNCHER_OK);
			ASSERT_EQ(launch.run_argument_count, 1u);
			EXPECT_STREQ(launch.run_arguments[0], "updated");
		}
	}
}

TEST(Reuse, DamagedGroupRebuildsWhileOtherArchiveStaysUntouched)
{
	Fixture f;
	launcher_test::multiple_archives(f.public_root, "r1");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	fs::path archive = f.install / "piggs/second.hogg";
	auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(24);
	fs::last_write_time(archive, stamp);
	stamp = fs::last_write_time(archive);
	write(f.install / "piggs/assets.hogg", "broken");
	copy_release(f, "copied");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(fs::last_write_time(archive), stamp);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_NE(fs::last_write_time(archive), stamp);
}

TEST(Reuse, RetainedDestinationChangedAfterPlanningFailsBeforeActivation)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	copy_release(f, "copied");
	launcher_release *target = nullptr;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &target, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	ASSERT_EQ(launcher_make_plan(f.handle, target, &plan, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	write(f.install / "piggs/assets.hogg", "external change");
	EXPECT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_INTEGRITY) << f.error.message;
	EXPECT_EQ(read(f.install / "piggs/assets.hogg"), "external change");
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
		LAUNCHER_OK);
	EXPECT_STREQ(launcher_release_id(installed), "r1");
	EXPECT_FALSE(fs::exists(f.base / "game.launcher/journal.json"));
	launcher_plan_free(&plan);
	launcher_release_free(&target);
}

TEST(Reuse, ResolveUsesInstalledDescriptorsWithoutStandaloneCache)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	launcher_close(&f.handle);
	fs::remove_all(f.base / "game.launcher/files");
	fs::remove_all(f.public_root / "catalog");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_release *target = nullptr;
	EXPECT_EQ(launcher_resolve(f.handle, &target, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->descriptor_requests, 3u);
	launcher_release_free(&target);
}

TEST(Workflow, ChangedArchiveDoesNotRecacheRetainedPayloads)
{
	launcher_test::Fixture f;
	std::string payload(262144, 'x');
	std::string digest = launcher_test::fingerprint(payload);
	unsigned char md5[16];
	mbedtls_md5((const unsigned char *)payload.data(), payload.size(), md5);
	std::string record = launcher_test::descriptor(digest,
		std::to_string(payload.size()), std::to_string(payload.size()),
		"raw", launcher_test::hex_digest(md5, sizeof(md5)),
		"retained.bin");
	std::string key = launcher_test::publish_record(f.public_root, record);
	launcher_test::write(f.public_root / "hogg" / digest / "BLOB", payload);
	for (const char *id : {"r1", "r2"})
		launcher_test::edit_json(f.public_root / "releases" /
			(std::string(id) + ".json"), [&](yyjson_mut_doc *doc,
			yyjson_mut_val *root) {
			yyjson_mut_val *group = yyjson_mut_obj_get(
				yyjson_mut_obj_get(root, "groups"), "assets");
			yyjson_mut_arr_add_strcpy(doc,
				yyjson_mut_obj_get(group, "files"),
				key.c_str());
		});
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	fs::path cache = f.install.string() + ".launcher/objects/" + digest;
	fs::remove(cache);
	f.pin("r2");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_FALSE(fs::exists(cache));
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}

TEST(Workflow, TouchedArchiveReclaimsTwentyPercentAndKeepsPiggFilename)
{
	Fixture f;
	for (const char *id : {"r1", "r2"}) {
		std::string payload(131072, !strcmp(id, "r1") ? 'A' : 'B');
		std::string digest = launcher_test::fingerprint(payload);
		unsigned char md5[16];
		mbedtls_md5((const unsigned char *)payload.data(),
			payload.size(), md5);
		std::string record = launcher_test::descriptor(digest,
			std::to_string(payload.size()),
			std::to_string(payload.size()), "raw",
			launcher_test::hex_digest(md5, sizeof(md5)));
		std::string key =
			launcher_test::publish_record(f.public_root, record);
		write(f.public_root / "hogg" / digest / "BLOB", payload);
		launcher_test::edit_json(f.public_root / "releases" /
			(std::string(id) + ".json"), [&](yyjson_mut_doc *doc,
			yyjson_mut_val *root) {
			yyjson_mut_val *group = yyjson_mut_obj_get(
				yyjson_mut_obj_get(root, "groups"), "assets");
			yyjson_mut_val *files =
				yyjson_mut_obj_get(group, "files");
			yyjson_mut_arr_clear(files);
			yyjson_mut_arr_add_strcpy(doc, files, key.c_str());
			yyjson_mut_obj_put(group, yyjson_mut_str(doc, "path"),
				yyjson_mut_str(doc, "piggs/bin.pigg"));
		});
	}
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	f.pin("r2");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_source_info info{};
	pg_source_storage_info storage{};
	ASSERT_EQ(pg_context_open(&context, nullptr), PG_OK);
	ASSERT_EQ(pg_source_open(context, (f.install / "piggs/bin.pigg")
		.string().c_str(), nullptr, &source, nullptr), PG_OK);
	ASSERT_EQ(pg_source_inspect(source, &info, nullptr), PG_OK);
	EXPECT_EQ(info.format, (uint32_t)PG_HOGG10);
	ASSERT_EQ(pg_source_storage_inspect(source, &storage, nullptr), PG_OK);
	EXPECT_LT(storage.reclaimable_bytes, storage.physical_length / 5);
	EXPECT_LT(storage.physical_length, 160000u);
	EXPECT_EQ(pg_source_close(&source, nullptr), PG_OK);
	EXPECT_EQ(pg_context_close(&context, nullptr), PG_OK);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
}

TEST(Workflow, IdenticalNormalizedConfigurationPreservesSavedFile)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(nullptr), LAUNCHER_OK);
	auto saved = f.base / "game.launcher/config.json";
	auto stamp = fs::file_time_type::clock::now() -
		std::chrono::hours(24);
	fs::last_write_time(saved, stamp);
	stamp = fs::last_write_time(saved);
	launcher_release *target = nullptr;
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &target, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	ASSERT_EQ(launcher_make_plan(f.handle, target, &plan, nullptr,
		nullptr, &f.error), LAUNCHER_OK);
	ASSERT_EQ(f.configure("live"), LAUNCHER_OK);
	EXPECT_EQ(fs::last_write_time(saved), stamp);
	EXPECT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_INVALID);
	launcher_plan_free(&plan);
	launcher_release_free(&target);
}
