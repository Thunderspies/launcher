#include <launcher/launcher.h>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <stdlib.h>
#include <string.h>
#include "../support.h"

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static const char *test_executable;

static void replace_text(
	std::string &text, const std::string &from, const std::string &to)
{
	size_t position = text.find(from);
	ASSERT_NE(position, std::string::npos) << from;
	text.replace(position, from.size(), to);
}

static void release_mapping(launcher_test::Fixture &fixture, const char *id,
	const char *data, const char *hogg = "piggs/assets.hogg")
{
	std::string text =
		launcher_test::read(fixture.public_root / "releases/r1.json");
	replace_text(text, "\"r1\"", "\"" + std::string(id) + "\"");
	text = launcher_test::rename_in_manifest(fixture.public_root, text,
		"data.bin", data);
	replace_text(text, "piggs/assets.hogg", hogg);
	launcher_test::write(
		fixture.public_root / ("releases/" + std::string(id) + ".json"),
		text);
}

TEST(Installation, MultilevelDirectoryTransitionsAndArchiveRelocation)
{
	launcher_test::Fixture f;
	release_mapping(f, "flat", "node");
	release_mapping(f, "nested", "node/inner/data.bin", "new/archive.hogg");
	f.pin("flat");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK)
		<< f.error.message;
	f.pin("nested");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK)
		<< f.error.message;
	EXPECT_EQ(
		launcher_test::read(f.install / "node/inner/data.bin"), "abc");
	EXPECT_FALSE(fs::exists(f.install / "piggs/assets.hogg"));
	EXPECT_TRUE(fs::exists(f.install / "new/archive.hogg"));
	f.pin("flat");
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK)
		<< f.error.message;
	EXPECT_EQ(launcher_test::read(f.install / "node"), "abc");
	EXPECT_FALSE(fs::exists(f.install / "new/archive.hogg"));
}

TEST(Installation, RefusesDirectoryTransitionContainingUnrelatedFile)
{
	launcher_test::Fixture f;
	release_mapping(f, "flat", "node");
	release_mapping(f, "nested", "node/inner/data.bin");
	f.pin("nested");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK)
		<< f.error.message;
	launcher_test::write(f.install / "node/inner/personal", "save");
	f.pin("flat");
	EXPECT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_CONFLICT)
		<< f.error.message;
	EXPECT_EQ(
		launcher_test::read(f.install / "node/inner/personal"), "save");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK)
		<< f.error.message;
}

static int crash_activation(const launcher_progress *progress, void *opaque)
{
	unsigned long long point = *(unsigned long long *)opaque;
	if (progress->phase == LAUNCHER_ACTIVATE &&
		(progress->completed == point ||
			(point == ~0U && progress->path &&
				!strcmp(progress->path, "docs/info.txt"))))
		_Exit(80);
	return 1;
}

int launcher_test_command(int argc, char **argv)
{
	test_executable = argv[0];
	if (argc != 4 || strcmp(argv[1], "--crash-update"))
		return -1;
	unsigned long long point = strtoull(argv[3], nullptr, 10);
	launcher_handle *handle = nullptr;
	launcher_error error{};
	launcher_status status = launcher_open(argv[2], &handle, &error);
	if (status == LAUNCHER_OK)
		status = launcher_update(
			handle, crash_activation, &point, &error);
	launcher_close(&handle);
	if (status != LAUNCHER_OK)
		fprintf(stderr, "%s\n", error.message);
	return status == LAUNCHER_OK ? 0 : 81;
}

#ifndef _WIN32
static int interrupted_update(const fs::path &install, unsigned point)
{
	std::string root = install.string();
	std::string value = std::to_string(point);
	pid_t child = fork();
	if (child == 0) {
		execl(test_executable, test_executable, "--crash-update",
			root.c_str(), value.c_str(), (char *)nullptr);
		_Exit(82);
	}
	if (child < 0)
		return 83;
	int status = 0;
	if (waitpid(child, &status, 0) != child || !WIFEXITED(status))
		return 84;
	return WEXITSTATUS(status);
}

TEST(Recovery, EveryActivationBoundaryResumesFrozenTargetOffline)
{
	for (unsigned point = 0; point <= 9; ++point) {
		launcher_test::Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK)
			<< f.error.message;
		launcher_test::write(f.install / "saves/keep", "personal");
		f.pin("r2");
		launcher_close(&f.handle);
		int child = interrupted_update(f.install, point);
		ASSERT_TRUE(child == 80 || child == 0)
			<< point << ": " << child;
		fs::remove_all(f.public_root);
		ASSERT_EQ(f.open(), LAUNCHER_OK)
			<< point << ": " << f.error.message;
		ASSERT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK)
			<< point << ": " << f.error.message;
		const launcher_release *release = nullptr;
		ASSERT_EQ(launcher_get_installed(f.handle, &release, &f.error),
			LAUNCHER_OK);
		std::string id = launcher_release_id(release);
		EXPECT_EQ(id, "r2");
		EXPECT_EQ(launcher_test::read(f.install / "game.exe"),
			id == "r1" ? "game-one\n" : "game-two\n");
		EXPECT_EQ(fs::exists(f.install / "data.bin"), id == "r1");
		EXPECT_EQ(fs::exists(f.install / "docs/info.txt"), id == "r2");
		EXPECT_EQ(launcher_test::read(f.install / "saves/keep"),
			"personal");
		EXPECT_FALSE(fs::exists(
			f.install.string() + ".launcher/journal.json"));
		launcher_close(&f.handle);
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	}
}


TEST(Recovery, MetadataOnlyTransactionsNeverMoveRetainedFiles)
{
	for (unsigned point = 0; point < 12; ++point) {
		launcher_test::Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK);
		auto path = f.install / "piggs/assets.hogg";
		auto stamp = fs::file_time_type::clock::now() -
			std::chrono::hours(24);
		fs::last_write_time(path, stamp);
		stamp = fs::last_write_time(path);
		std::string manifest = launcher_test::read(
			f.public_root / "releases/r1.json");
		replace_text(manifest, "\"r1\"", "\"copied\"");
		launcher_test::write(f.public_root / "releases/copied.json",
			manifest);
		f.pin("copied");
		launcher_close(&f.handle);
		int result = interrupted_update(f.install, point);
		ASSERT_TRUE(result == 80 || result == 0);
		fs::remove_all(f.public_root);
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		EXPECT_EQ(fs::last_write_time(path), stamp) << point;
		EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
	}
}

TEST(Recovery, MultipleArchivesCommitOrRestoreTogether)
{
	for (unsigned point = 0; point < 14; ++point) {
		launcher_test::Fixture f;
		launcher_test::multiple_archives(f.public_root);
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		f.pin("multi");
		launcher_close(&f.handle);
		int result = interrupted_update(f.install, point);

		ASSERT_TRUE(result == 80 || result == 0);
		fs::remove_all(f.public_root);
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		const launcher_release *installed = nullptr;
		ASSERT_EQ(launcher_get_installed(f.handle, &installed, &f.error),
			LAUNCHER_OK);
		bool committed = !strcmp(launcher_release_id(installed), "multi");

		EXPECT_EQ(fs::exists(f.install / "piggs/second.hogg"), committed);
		EXPECT_TRUE(fs::exists(f.install / "piggs/assets.hogg"));
	}
}

TEST(Recovery, PublishedCommandCommitsWithItsReleaseAfterInterruption)
{
	for (unsigned point : {0U, 4U, 7U, 8U, 9U}) {
		launcher_test::Fixture f;
		launcher_test::publish_launch(f.public_root / "channels/live.json",
			"{\"format\":1,\"release\":\"r1\","
			"\"entry_point\":\"game.exe\","
			"\"run_arguments\":[\"old\"]}");
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		launcher_test::publish_launch(f.public_root / "channels/live.json",
			"{\"format\":1,\"release\":\"r2\","
			"\"entry_point\":\"game.exe\","
			"\"run_arguments\":[\"new\"]}");
		launcher_close(&f.handle);
		int child = interrupted_update(f.install, point);
		ASSERT_TRUE(child == 80 || child == 0)
			<< point << ": " << child;
		fs::remove_all(f.public_root);
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		ASSERT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		const launcher_release *installed = nullptr;
		ASSERT_EQ(launcher_get_installed(
			f.handle, &installed, &f.error), LAUNCHER_OK);
		launcher_launch_info info{};
		ASSERT_EQ(launcher_release_get_launch(installed, &info,
			&f.error), LAUNCHER_OK);
		EXPECT_STREQ(info.entry_point, "game.exe");
		ASSERT_EQ(info.run_argument_count, 1u);
		EXPECT_STREQ(info.run_arguments[0],
			!strcmp(launcher_release_id(installed), "r1")
				? "old" : "new");
	}
}

TEST(Recovery, FirstAdoptionResumesTargetAndPreservesUnrelatedFiles)
{
	for (unsigned point = 0; point <= 4; ++point) {
		launcher_test::Fixture f;
		launcher_test::write(f.install / "game.exe", "original game");
		launcher_test::write(f.install / "data.bin", "original data");
		launcher_test::write(f.install / "keep", "unrelated");
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
		launcher_close(&f.handle);
		ASSERT_EQ(interrupted_update(f.install, point), 80);
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		const launcher_release *release = nullptr;
		EXPECT_EQ(launcher_get_installed(f.handle, &release, &f.error),
			LAUNCHER_OK);
		EXPECT_EQ(launcher_test::read(f.install / "game.exe"),
			"game-one\n");
		EXPECT_EQ(launcher_test::read(f.install / "data.bin"), "abc");
		EXPECT_EQ(launcher_test::read(f.install / "keep"), "unrelated");
	}
}

TEST(Recovery, MultilevelFileToDirectoryResumesTarget)
{
	launcher_test::Fixture f;
	release_mapping(f, "flat", "node");
	release_mapping(f, "nested", "node/inner/data.bin");
	f.pin("flat");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK)
		<< f.error.message;
	f.pin("nested");
	launcher_close(&f.handle);
	ASSERT_EQ(interrupted_update(f.install, 2), 80);
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(
		launcher_test::read(f.install / "node/inner/data.bin"), "abc");
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK)
		<< f.error.message;
}

TEST(Recovery, InvalidWriteSetsPreserveJournalAndRetainedFiles)
{
	for (int failure = 0; failure < 6; ++failure) {
		launcher_test::Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK);
		f.pin("r2");
		launcher_close(&f.handle);
		ASSERT_EQ(interrupted_update(f.install, 0), 80);
		fs::path journal = f.base / "game.launcher/journal.json";

		launcher_test::edit_json(journal, [&](yyjson_mut_doc *doc,
			yyjson_mut_val *root) {
			yyjson_mut_val *writes = yyjson_mut_obj_get(root, "writes");

			if (failure == 0)
				yyjson_mut_arr_add_strcpy(doc, writes,
					yyjson_mut_get_str(yyjson_mut_arr_get(writes, 0)));
			else if (failure == 1)
				yyjson_mut_arr_add_str(doc, writes, "saves/unrelated");
			else if (failure == 2)
				yyjson_mut_arr_add_str(doc, writes, "../outside");
			else if (failure == 3)
				yyjson_mut_arr_clear(writes);
			else {
				const char *path = failure == 4 ? "docs/info.txt" :
					"piggs/assets.hogg";
				yyjson_mut_val *retained = yyjson_mut_obj_get(root,
					"retained");

				yyjson_mut_arr_add_strcpy(doc, retained, path);
				for (size_t i = 0; i < yyjson_mut_arr_size(writes); ++i)
					if (!strcmp(yyjson_mut_get_str(yyjson_mut_arr_get(
						writes, i)), path)) {
						yyjson_mut_arr_remove(writes, i);
						break;
					}
				if (failure == 5) {
					yyjson_mut_val *files = yyjson_mut_obj_get(root,
						"files");

					for (size_t i = 0; i < yyjson_mut_arr_size(files); ++i)
						if (!strcmp(yyjson_mut_get_str(yyjson_mut_arr_get(
							files, i)), path)) {
							yyjson_mut_arr_remove(files, i);
							break;
						}
				}
			}
		});
		std::string saved = launcher_test::read(journal);
		std::string game = launcher_test::read(f.install / "game.exe");

		EXPECT_EQ(f.open(), LAUNCHER_RECOVERY_REQUIRED) << failure;
		EXPECT_EQ(launcher_test::read(journal), saved);
		EXPECT_EQ(launcher_test::read(f.install / "game.exe"), game);
	}
}

TEST(Recovery, OmittedNewWritePreservesActiveFilesAndEvidence)
{
	launcher_test::Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	f.pin("r2");
	launcher_close(&f.handle);
	ASSERT_EQ(interrupted_update(f.install, ~0U), 80);
	ASSERT_TRUE(fs::exists(f.install / "docs/info.txt"));
	fs::path journal = f.base / "game.launcher/journal.json";

	launcher_test::edit_json(journal, [&](yyjson_mut_doc *,
		yyjson_mut_val *root) {
		yyjson_mut_val *writes = yyjson_mut_obj_get(root, "writes");

		for (size_t i = 0; i < yyjson_mut_arr_size(writes); ++i)
			if (!strcmp(yyjson_mut_get_str(yyjson_mut_arr_get(writes, i)),
				"docs/info.txt")) {
				yyjson_mut_arr_remove(writes, i);
				break;
			}
	});
	std::string saved = launcher_test::read(journal);
	std::string game = launcher_test::read(f.install / "game.exe");

	EXPECT_EQ(f.open(), LAUNCHER_RECOVERY_REQUIRED);
	EXPECT_EQ(launcher_test::read(journal), saved);
	EXPECT_EQ(launcher_test::read(f.install / "game.exe"), game);
	EXPECT_TRUE(fs::exists(f.install / "docs/info.txt"));
}

TEST(Recovery, CorruptDuplicateJournalInventoryKeepsEvidence)
{
	for (bool duplicate_directory : {false, true}) {
		launcher_test::Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK)
			<< f.error.message;
		f.pin("r2");
		launcher_close(&f.handle);
		ASSERT_EQ(interrupted_update(f.install, 0), 80);
		fs::path journal =
			f.install.string() + ".launcher/journal.json";
		std::string bytes = launcher_test::read(journal);
		if (duplicate_directory) {
			size_t start = bytes.find("\"directories\":[");
			ASSERT_NE(start, std::string::npos);
			start = bytes.find('{', start);
			size_t end = bytes.find('}', start);
			ASSERT_NE(end, std::string::npos);
			bytes.insert(start,
				bytes.substr(start, end - start + 1) + ",");
		} else {
			size_t start = bytes.find("\"files\":[");
			ASSERT_NE(start, std::string::npos);
			bytes.insert(
				start + strlen("\"files\":["), "\"data.bin\",");
		}
		launcher_test::write(journal, bytes);
		EXPECT_EQ(f.open(), LAUNCHER_RECOVERY_REQUIRED)
			<< f.error.message;
		EXPECT_TRUE(fs::exists(journal));
		EXPECT_EQ(launcher_test::read(f.install / "game.exe"),
			"game-one\n");
	}
}
#endif

static fs::path isolated_install()
{
	auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
	return fs::temp_directory_path() /
		("launcher-transaction-" + std::to_string(tick));
}

TEST(Installation, OpensNewRootAndLocksSidecar)
{
	fs::path root = isolated_install();
	launcher_handle *first = NULL;
	launcher_handle *second = NULL;
	launcher_error error = {};
	ASSERT_EQ(LAUNCHER_OK,
		launcher_open(root.string().c_str(), &first, &error))
		<< error.message;
	EXPECT_TRUE(fs::is_directory(root));
	EXPECT_TRUE(fs::is_directory(root.string() + ".launcher"));
	EXPECT_EQ(LAUNCHER_BUSY,
		launcher_open(root.string().c_str(), &second, &error));
	EXPECT_EQ(nullptr, second);
	launcher_close(&first);
	EXPECT_EQ(nullptr, first);
	EXPECT_EQ(LAUNCHER_OK,
		launcher_open(root.string().c_str(), &second, &error))
		<< error.message;
	launcher_close(&second);
	fs::remove_all(root);
	fs::remove_all(root.string() + ".launcher");
}

TEST(Installation, RepairAndVerifyRequireInstalledManifest)
{
	fs::path root = isolated_install();
	launcher_handle *handle = NULL;
	launcher_error error = {};
	ASSERT_EQ(LAUNCHER_OK,
		launcher_open(root.string().c_str(), &handle, &error))
		<< error.message;
	const launcher_release *installed = NULL;
	EXPECT_EQ(LAUNCHER_NOT_INSTALLED,
		launcher_get_installed(handle, &installed, &error));
	EXPECT_EQ(nullptr, installed);
	EXPECT_EQ(LAUNCHER_NOT_INSTALLED,
		launcher_repair(handle, NULL, NULL, &error));
	EXPECT_EQ(LAUNCHER_NOT_INSTALLED,
		launcher_verify(handle, NULL, NULL, &error));
	launcher_close(&handle);
	fs::remove_all(root);
	fs::remove_all(root.string() + ".launcher");
}

TEST(Installation, ReplacedRootCannotReuseOldOwnershipRecord)
{
	launcher_test::Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	fs::rename(f.install, f.base / "original");
	launcher_test::write(f.install / "personal", "different directory");
	EXPECT_EQ(f.open(), LAUNCHER_CONFLICT) << f.error.message;
	EXPECT_EQ(launcher_test::read(f.install / "personal"),
		"different directory");
}

TEST(Installation, ResumesInterruptedInitializerWithoutClaimingOtherData)
{
	for (bool temporary_marker : {false, true}) {
		launcher_test::Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		launcher_close(&f.handle);
		fs::path state = f.install.string() + ".launcher";
		if (temporary_marker)
			fs::rename(state / "marker.json",
				state / "marker.json.tmp");
		else
			fs::remove(state / "marker.json");
		fs::remove(state / "objects");
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		launcher_close(&f.handle);
		fs::remove(state / "marker.json");
		launcher_test::write(state / "personal", "unrelated sidecar");
		EXPECT_EQ(f.open(), LAUNCHER_CONFLICT) << f.error.message;
		EXPECT_EQ(launcher_test::read(state / "personal"),
			"unrelated sidecar");
	}
}

TEST(Transaction, InvalidPrivateStatePreservesRecoveryEvidence)
{
	using launcher_test::Fixture;
	using launcher_test::read;
	using launcher_test::write;
	for (const char *name : {"installed-state.json", "journal.json"}) {
		Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		launcher_close(&f.handle);
		fs::path record = f.base / "game.launcher" / name;
		std::string old =
			"{\"format\":1,\"manifest\":\"invalid manifest\"}";
		write(record, old);
		EXPECT_EQ(f.open(), LAUNCHER_RECOVERY_REQUIRED);
		EXPECT_EQ(f.handle, nullptr);
		EXPECT_EQ(read(record), old);
		EXPECT_NE(std::string(f.error.message).find("preserve"),
			std::string::npos) << f.error.message;
	}
}

#ifndef _WIN32
TEST(Recovery, DirectoryToFileTransitionResumesAfterEveryMutation)
{
	for (unsigned point = 0; point < 12; point++) {
		launcher_test::Fixture f;
		release_mapping(f, "flat", "node");
		release_mapping(f, "nested", "node/inner/data.bin");
		f.pin("nested");
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
		launcher_test::write(f.install / "saves/keep", "personal");
		f.pin("flat");
		launcher_close(&f.handle);
		int child = interrupted_update(f.install, point);
		ASSERT_TRUE(child == 0 || child == 80);
		fs::remove_all(f.public_root);
		ASSERT_EQ(f.open(), LAUNCHER_OK)
			<< point << ": " << f.error.message;
		EXPECT_EQ(launcher_test::read(f.install / "node"), "abc");
		EXPECT_EQ(launcher_test::read(f.install / "saves/keep"),
			"personal");
		EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK) << f.error.message;
	}
}
#endif
