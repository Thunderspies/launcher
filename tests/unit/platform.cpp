#include "../descriptors.h"
#include "internal.h"
#include "../support.h"
#include <gtest/gtest.h>
#include <mbedtls/sha256.h>
#include <filesystem>
#include <string>
#include <fstream>
#include <chrono>
#include <thread>
#ifndef _WIN32
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#else
#include <windows.h>
#endif

class Platform : public testing::Test {
protected:
	std::string root;
	lx_fs *fs = NULL;
	launcher_error error = {};

	void SetUp() override
	{
#ifndef _WIN32
		char name[] = "/tmp/launcher-platform-XXXXXX";
		ASSERT_NE(mkdtemp(name), nullptr);
		root = name;
#else
		std::string id = std::to_string(GetCurrentProcessId());
		auto temporary = std::filesystem::temp_directory_path();
		root = (temporary / ("launcher-platform-" + id)).string();
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
#endif
	}

	void TearDown() override
	{
		lx_fs_close(&fs);
		std::filesystem::remove_all(root);
	}

	void Open()
	{
		ASSERT_EQ(lx_fs_open(root.c_str(), 0, &fs, &error),
			LAUNCHER_OK) << error.message;
	}
};

TEST_F(Platform, AnchoredFilesAndMissingStat)
{
	Open();
	int kind = -1;
	uint64_t size = 99;
	ASSERT_EQ(lx_fs_stat(fs, "absent/file", &kind, &size, &error),
		LAUNCHER_OK);
	EXPECT_EQ(kind, 0);
	EXPECT_EQ(size, 0u);
	ASSERT_EQ(lx_fs_mkdirs(fs, "nested/deeper", &error), LAUNCHER_OK);
	FILE *file = NULL;
	ASSERT_EQ(lx_fs_write(fs, "nested/deeper/file", 1, &file, &error),
		LAUNCHER_OK);
	ASSERT_EQ(fwrite("data", 1, 4, file), 4u);
	ASSERT_EQ(lx_file_flush(file, &error), LAUNCHER_OK);
	fclose(file);
	ASSERT_EQ(lx_fs_stat(fs, "nested/deeper/file", &kind, &size, &error),
		LAUNCHER_OK);
	EXPECT_EQ(kind, 1);
	EXPECT_EQ(size, 4u);
	ASSERT_EQ(lx_fs_read(fs, "nested/deeper/file", &file, &error),
		LAUNCHER_OK);
	char bytes[5] = {};
	EXPECT_EQ(fread(bytes, 1, 4, file), 4u);
	EXPECT_STREQ(bytes, "data");
	fclose(file);
}

TEST_F(Platform, RejectsTraversalAndLinks)
{
	Open();
	FILE *file = NULL;
	EXPECT_EQ(lx_fs_write(fs, "../escape", 1, &file, &error),
		LAUNCHER_INVALID);
	EXPECT_EQ(file, nullptr);
#ifndef _WIN32
	ASSERT_EQ(symlink("/tmp", (root + "/link").c_str()), 0);
	EXPECT_EQ(lx_fs_write(fs, "link/escape", 1, &file, &error),
		LAUNCHER_CONFLICT);
	int kind;
	EXPECT_EQ(lx_fs_stat(fs, "link", &kind, NULL, &error),
		LAUNCHER_CONFLICT);
#endif
}

TEST_F(Platform, LockAndNoOverwriteMove)
{
	Open();
	ASSERT_EQ(lx_fs_lock(fs, &error), LAUNCHER_OK);
	lx_fs *other = NULL;
	ASSERT_EQ(lx_fs_open(root.c_str(), 0, &other, &error), LAUNCHER_OK);
	EXPECT_EQ(lx_fs_lock(other, &error), LAUNCHER_BUSY);
	lx_fs_close(&other);
	FILE *file = NULL;
	ASSERT_EQ(lx_fs_write(fs, "a", 1, &file, &error), LAUNCHER_OK);
	fclose(file);
	ASSERT_EQ(lx_fs_write(fs, "b", 1, &file, &error), LAUNCHER_OK);
	fclose(file);
	EXPECT_EQ(lx_fs_move(fs, "a", fs, "b", &error), LAUNCHER_CONFLICT);
	ASSERT_EQ(lx_fs_replace(fs, "a", fs, "b", &error), LAUNCHER_OK);
	int kind;
	ASSERT_EQ(lx_fs_stat(fs, "a", &kind, NULL, &error), LAUNCHER_OK);
	EXPECT_EQ(kind, 0);
}

TEST_F(Platform, EmptyDirectoryAndPinnedNativePath)
{
	Open();
	int empty = 0;
	ASSERT_EQ(lx_fs_empty(fs, "", &empty, &error), LAUNCHER_OK);
	EXPECT_EQ(empty, 1);
	ASSERT_EQ(lx_fs_mkdirs(fs, "dir", &error), LAUNCHER_OK);
	ASSERT_EQ(lx_fs_empty(fs, "dir", &empty, &error), LAUNCHER_OK);
	EXPECT_EQ(empty, 1);
	lx_fs *parent = NULL;
	char *path = NULL;
	ASSERT_EQ(lx_fs_pin(fs, "dir/file", 0, &parent, &path, &error),
		LAUNCHER_OK);
	FILE *file = fopen(path, "wb");
	ASSERT_NE(file, nullptr);
	fclose(file);
	free(path);
	lx_fs_close(&parent);
	ASSERT_EQ(lx_fs_empty(fs, "dir", &empty, &error), LAUNCHER_OK);
	EXPECT_EQ(empty, 0);
}

TEST_F(Platform, ListsKindsAndSupportsEarlyStop)
{
	Open();
	ASSERT_EQ(lx_fs_mkdirs(fs, "dir", &error), LAUNCHER_OK);
	FILE *file = NULL;
	ASSERT_EQ(lx_fs_write(fs, "file", 1, &file, &error), LAUNCHER_OK);
	fclose(file);
	int kinds[4] = {};
	auto visit = [](const char *, int kind, void *state) {
		++((int *)state)[kind];
		return 1;
	};
	ASSERT_EQ(lx_fs_list(fs, "", visit, kinds, &error), LAUNCHER_OK);
	EXPECT_EQ(kinds[1], 1);
	EXPECT_EQ(kinds[2], 1);
	EXPECT_EQ(lx_fs_list(fs, "", [](const char *, int, void *) {
		return 0;
	}, NULL, &error), LAUNCHER_CONFLICT);
}

TEST_F(Platform, PreservesNativeArgumentsAndExitStatus)
{
	Open();
	std::string child = std::filesystem::absolute(
		LAUNCHER_TEST_CHILD).string();
	std::string output = root + "/arguments";
	const char *args[] = { child.c_str(), "7", "0", output.c_str(),
		"spaces here", "\"quoted\"", "a\\b", "", "tail\\",
		"line\nbreak", "slash\\\"quote" };
	int code = -1;
	ASSERT_EQ(lx_process_run(fs, root.c_str(), args, 11, &code, &error),
		LAUNCHER_OK) << error.message;
	EXPECT_EQ(code, 7);
	std::ifstream file(output, std::ios::binary);
	std::string bytes((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	EXPECT_EQ(bytes, "11:spaces here\n8:\"quoted\"\n3:a\\b\n0:\n"
		"5:tail\\\n10:line\nbreak\n12:slash\\\"quote\n");
	EXPECT_EQ(lx_process_busy(fs, &error), LAUNCHER_OK);
}

TEST_F(Platform, RejectsMalformedProcessMarker)
{
	Open();
	FILE *file = NULL;
	ASSERT_EQ(lx_fs_write(fs, "running.json", 1, &file, &error),
		LAUNCHER_OK);
	fputs("{\"pid\":123}", file);
	fclose(file);
	EXPECT_EQ(lx_process_busy(fs, &error), LAUNCHER_RECOVERY_REQUIRED);
}

TEST_F(Platform, DirectoryIdentityTracksTheRetainedObject)
{
	Open();
	char initial[128], repeated[128];
	ASSERT_EQ(lx_fs_identity(fs, initial, &error), LAUNCHER_OK);
	lx_fs *other = NULL;
	std::string same = root;
#ifdef _WIN32
	for (char &c : same) {
		if (c >= 'a' && c <= 'z')
			c -= 'a' - 'A';
	}
#endif
	ASSERT_EQ(lx_fs_open(same.c_str(), 0, &other, &error), LAUNCHER_OK);
	ASSERT_EQ(lx_fs_identity(other, repeated, &error), LAUNCHER_OK);
	EXPECT_STREQ(initial, repeated);
	EXPECT_STREQ(lx_fs_path(fs), lx_fs_path(other));
	lx_fs_close(&other);
	lx_fs_close(&fs);
	ASSERT_EQ(rename(root.c_str(), (root + "-moved").c_str()), 0);
	ASSERT_EQ(lx_fs_open(root.c_str(), 1, &fs, &error), LAUNCHER_OK);
	ASSERT_EQ(lx_fs_identity(fs, repeated, &error), LAUNCHER_OK);
	EXPECT_STRNE(initial, repeated);
	std::filesystem::remove_all(root + "-moved");
}

TEST(Launch, ConfiguredRunnerUsesOfflineManagedGameAndArguments)
{
	launcher_test::Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, NULL, NULL, &f.error), LAUNCHER_OK)
		<< f.error.message;
	std::string child = std::filesystem::absolute(
		LAUNCHER_TEST_CHILD).string();
	const char *runner_args[] = { "13", "0", "runner-arguments" };
	const char *game_args[] = { "configured", "spaces here" };
	launcher_config config = {};
	config.base_url = f.server->url.c_str();
	launcher_file_repository files{f.server->url.c_str(), nullptr};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.game = "game.exe";
	config.runner = child.c_str();
	config.runner_args = runner_args;
	config.runner_arg_count = 3;
	config.game_args = game_args;
	config.game_arg_count = 2;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK)
		<< f.error.message;
	delete f.server;
	f.server = NULL;
	const char *extra[] = { "", "backslash\\", "\"quoted\"" };
	int code = -1;
	ASSERT_EQ(launcher_launch(f.handle, extra, 3, &code, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(code, 13);
	std::string bytes = launcher_test::read(f.install / "runner-arguments");
	size_t colon = bytes.find(':');
	ASSERT_NE(colon, std::string::npos);
	size_t size = std::stoul(bytes.substr(0, colon));
	std::string game = bytes.substr(colon + 1, size);
	EXPECT_TRUE(std::filesystem::equivalent(game, f.install / "game.exe"));
	EXPECT_EQ(bytes.substr(colon + size + 2),
		"10:configured\n11:spaces here\n0:\n"
		"10:backslash\\\n8:\"quoted\"\n");
	launcher_test::write(f.install / "game.exe",
		"local executable override");
	std::filesystem::remove(f.install / "piggs/assets.hogg");
	launcher_test::write(f.install / "data.bin", "local managed data");
	EXPECT_EQ(launcher_launch(f.handle, NULL, 0, &code, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(code, 13);
	EXPECT_EQ(launcher_verify(f.handle, NULL, NULL, &f.error),
		LAUNCHER_INTEGRITY);
}

TEST(Launch, PublishedCommandPersistsAndRunsOfflineWithoutGameOption)
{
	launcher_test::Fixture f;
	launcher_test::publish_launch(f.public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r1\",\"entry_point\":\"game.exe\","
		"\"run_arguments\":[\"published\",\"two words\",\"\","
		"\"$(echo nope)\",\"\\\"quoted\\\"\"]}");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	std::string child = std::filesystem::absolute(
		LAUNCHER_TEST_CHILD).string();
	const char *runner_args[] = { "19", "0", "channel-arguments" };
	const char *game_args[] = { "local" };
	launcher_config config{};
	config.base_url = f.server->url.c_str();
	launcher_file_repository files{f.server->url.c_str(), nullptr};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.runner = child.c_str();
	config.runner_args = runner_args;
	config.runner_arg_count = 3;
	config.game_args = game_args;
	config.game_arg_count = 1;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK)
		<< f.error.message;
	launcher_release *target = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &target, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	launcher_launch_info info{};
	ASSERT_EQ(launcher_release_get_launch(target, &info, &f.error),
		LAUNCHER_OK);
	EXPECT_STREQ(info.entry_point, "game.exe");
	ASSERT_EQ(info.run_argument_count, 5u);
	EXPECT_STREQ(info.run_arguments[2], "");
	launcher_release_free(&target);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	delete f.server;
	f.server = nullptr;
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	const char *extra[] = { "extra" };
	int code = -1;
	ASSERT_EQ(launcher_launch(f.handle, extra, 1, &code, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(code, 19);
	std::string bytes = launcher_test::read(
		f.install / "channel-arguments");
	size_t colon = bytes.find(':');
	ASSERT_NE(colon, std::string::npos);
	size_t size = std::stoul(bytes.substr(0, colon));
	EXPECT_TRUE(std::filesystem::equivalent(bytes.substr(colon + 1, size),
		f.install / "game.exe"));
	EXPECT_EQ(bytes.substr(colon + size + 2),
		"9:published\n9:two words\n0:\n12:$(echo nope)\n"
		"8:\"quoted\"\n5:local\n5:extra\n");
}

TEST(Launch, LocalGameOverrideDoesNotUsePublishedArguments)
{
	launcher_test::Fixture f;
	launcher_test::publish_launch(f.public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r1\",\"entry_point\":\"data.bin\","
		"\"run_arguments\":[\"published\"]}");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	std::string child = std::filesystem::absolute(
		LAUNCHER_TEST_CHILD).string();
	const char *runner_args[] = { "0", "0", "override-arguments" };
	const char *game_args[] = { "local" };
	launcher_config config{};
	config.base_url = f.server->url.c_str();
	launcher_file_repository files{f.server->url.c_str(), nullptr};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.game = "game.exe";
	config.runner = child.c_str();
	config.runner_args = runner_args;
	config.runner_arg_count = 3;
	config.game_args = game_args;
	config.game_arg_count = 1;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	int code = -1;
	ASSERT_EQ(launcher_launch(f.handle, nullptr, 0, &code, &f.error),
		LAUNCHER_OK) << f.error.message;
	std::string bytes = launcher_test::read(
		f.install / "override-arguments");
	size_t colon = bytes.find(':');
	size_t size = std::stoul(bytes.substr(0, colon));
	EXPECT_TRUE(std::filesystem::equivalent(bytes.substr(colon + 1, size),
		f.install / "game.exe"));
	EXPECT_EQ(bytes.substr(colon + size + 2), "5:local\n");
}

TEST(Launch, PublishedEntryPointMustBeAnInstalledLooseFile)
{
	launcher_test::Fixture f;
	launcher_test::publish_launch(f.public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r1\","
		"\"entry_point\":\"assets/data.bin\",\"run_arguments\":[]}");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config{};
	config.base_url = f.server->url.c_str();
	launcher_file_repository files{f.server->url.c_str(), nullptr};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	EXPECT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_METADATA);
}

TEST(Launch, NativeManagedExecutableRunsOffline)
{
	launcher_test::Fixture f;
	std::string bytes = launcher_test::read(LAUNCHER_TEST_CHILD);
	ASSERT_FALSE(bytes.empty());
	unsigned char digest[32];
	ASSERT_EQ(mbedtls_sha256((const unsigned char *)bytes.data(),
		bytes.size(), digest, 0), 0);
	char hex[65];
	for (size_t i = 0; i < 32; ++i)
		snprintf(hex + i * 2, 3, "%02x", digest[i]);
	unsigned char md5[16];
	ASSERT_EQ(mbedtls_md5((const unsigned char *)bytes.data(),
		bytes.size(), md5), 0);
	std::string record = launcher_test::descriptor(hex,
		std::to_string(bytes.size()), std::to_string(bytes.size()),
		"raw", launcher_test::hex_digest(md5, sizeof(md5)),
		"game.exe", "loose");
	std::string key = launcher_test::fingerprint(record);
	launcher_test::publish_record(f.public_root, record);
	launcher_test::write(f.public_root / "loose" / hex / "BLOB", bytes);
	std::string manifest = "{\"format\":2,\"release\":\"native\","
			       "\"groups\":{},\"loose\":[\"" +
			       key +
			       "\"],\"entry_point\":null,"
			       "\"run_arguments\":[]}";
	launcher_test::write(f.public_root / "releases/native.json", manifest);
	f.pin("native");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(f.configure(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_update(f.handle, NULL, NULL, &f.error), LAUNCHER_OK)
		<< f.error.message;
	const char *game_args[] = { "17", "0", "native-arguments",
		"native payload", "" };
	launcher_config config = {};
	config.base_url = f.server->url.c_str();
	launcher_file_repository files{f.server->url.c_str(), nullptr};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.game = "game.exe";
	config.game_args = game_args;
	config.game_arg_count = 5;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK)
		<< f.error.message;
	delete f.server;
	f.server = NULL;
	int code = -1;
	ASSERT_EQ(launcher_launch(f.handle, NULL, 0, &code, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(code, 17);
	EXPECT_EQ(launcher_test::read(f.install / "native-arguments"),
		"14:native payload\n0:\n");
}

#ifdef _WIN32
TEST_F(Platform, DirectoryCaseAppliesEachRequestedComponent)
{
	Open();
	ASSERT_EQ(lx_fs_mkdirs(fs, "Data/Nested", &error), LAUNCHER_OK);
	ASSERT_EQ(lx_fs_case(fs, "data/nested", &error), LAUNCHER_OK)
		<< error.message;
	char *spelling = NULL;
	ASSERT_EQ(lx_fs_spell(fs, "data/nested", &spelling, &error),
		LAUNCHER_OK);
	EXPECT_STREQ(spelling, "data/nested");
	free(spelling);
}
#endif

#ifndef _WIN32
TEST_F(Platform, ProcessReportsExitAndClearsIdentity)
{
	Open();
	const char *args[] = { "/bin/sh", "-c", "exit 7" };
	int code = -1;
	ASSERT_EQ(lx_process_run(fs, root.c_str(), args, 3, &code, &error),
		LAUNCHER_OK) << error.message;
	EXPECT_EQ(code, 7);
	EXPECT_EQ(lx_process_busy(fs, &error), LAUNCHER_OK);
	const char *bad[] = { "/this-command-does-not-exist" };
	EXPECT_EQ(lx_process_run(fs, root.c_str(), bad, 1, &code, &error),
		LAUNCHER_IO);
	EXPECT_EQ(code, -1);
	EXPECT_EQ(lx_process_busy(fs, &error), LAUNCHER_OK);
}

TEST_F(Platform, PassesArgumentsExactlyAndUsesWorkingDirectory)
{
	Open();
	const char *args[] = { "/bin/sh", "-c",
		"printf '%s\\n' \"$@\" > arguments", "argument-check",
		"spaces here", "\"quoted\"", "a\\b", "" };
	int code = -1;
	ASSERT_EQ(lx_process_run(fs, root.c_str(), args, 8, &code, &error),
		LAUNCHER_OK) << error.message;
	EXPECT_EQ(code, 0);
	std::ifstream file(root + "/arguments");
	std::string bytes((std::istreambuf_iterator<char>(file)),
		std::istreambuf_iterator<char>());
	EXPECT_EQ(bytes, "spaces here\n\"quoted\"\na\\b\n\n");
}

TEST_F(Platform, DetectsDirectorySubstitution)
{
	Open();
	ASSERT_EQ(lx_fs_validate(fs, &error), LAUNCHER_OK);
	ASSERT_EQ(rename(root.c_str(), (root + "-moved").c_str()), 0);
	std::filesystem::create_directory(root);
	EXPECT_EQ(lx_fs_validate(fs, &error), LAUNCHER_CONFLICT);
	FILE *file = NULL;
	ASSERT_EQ(lx_fs_write(fs, "anchored", 1, &file, &error), LAUNCHER_OK);
	fclose(file);
	EXPECT_TRUE(std::filesystem::exists(root + "-moved/anchored"));
	EXPECT_FALSE(std::filesystem::exists(root + "/anchored"));
	lx_fs_close(&fs);
	std::filesystem::remove_all(root + "-moved");
}

TEST_F(Platform, SurvivingChildBlocksAfterSupervisorCrash)
{
	Open();
	pid_t supervisor = fork();
	ASSERT_GE(supervisor, 0);
	if (!supervisor) {
		const char *args[] = { "/bin/sleep", "2" };
		int code;
		launcher_status status = lx_process_run(fs, root.c_str(), args,
			2, &code, &error);
		_exit(status == LAUNCHER_OK ? 0 : 1);
	}
	launcher_status status = LAUNCHER_OK;
	for (int i = 0; i < 100; ++i) {
		status = lx_process_busy(fs, &error);
		if (status == LAUNCHER_BUSY)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_EQ(status, LAUNCHER_BUSY) << error.message;
	EXPECT_EQ(kill(supervisor, SIGKILL), 0);
	int wait_status;
	EXPECT_EQ(waitpid(supervisor, &wait_status, 0), supervisor);
	EXPECT_EQ(lx_process_busy(fs, &error), LAUNCHER_BUSY);
	for (int i = 0; i < 300; ++i) {
		status = lx_process_busy(fs, &error);
		if (status == LAUNCHER_OK)
			break;
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	EXPECT_EQ(status, LAUNCHER_OK) << error.message;
}
#endif

#ifndef _WIN32
TEST_F(Platform, ExecutablePermissionIsWrittenOnlyWhenMissing)
{
	Open();
	launcher_test::write(std::filesystem::path(root) / "native", "binary");
	ASSERT_EQ(chmod((root + "/native").c_str(), 0600), 0);
	ASSERT_EQ(lx_fs_executable(fs, "native", &error), LAUNCHER_OK);
	struct stat before = {};
	ASSERT_EQ(stat((root + "/native").c_str(), &before), 0);
	EXPECT_TRUE(before.st_mode & S_IXUSR);
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	ASSERT_EQ(lx_fs_executable(fs, "native", &error), LAUNCHER_OK);
	struct stat after = {};
	ASSERT_EQ(stat((root + "/native").c_str(), &after), 0);
	EXPECT_EQ(before.st_ctim.tv_sec, after.st_ctim.tv_sec);
	EXPECT_EQ(before.st_ctim.tv_nsec, after.st_ctim.tv_nsec);
	EXPECT_EQ(before.st_mode, after.st_mode);
}
#endif

static launcher_status configure_launch_runner(launcher_test::Fixture &f)
{
	std::string child = std::filesystem::absolute(
		LAUNCHER_TEST_CHILD).string();
	const char *arguments[] = { "0", "0", "launch-gate-output" };
	launcher_config config{};
	launcher_status status = launcher_get_config(f.handle, &config,
		&f.error);

	if (status != LAUNCHER_OK)
		return status;
	config.runner = child.c_str();
	config.runner_args = arguments;
	config.runner_arg_count = 3;
	return launcher_configure(f.handle, &config, &f.error);
}

TEST(Launch, SelectedExecutableMustRemainASafeRegularFile)
{
#ifdef _WIN32
	const unsigned cases = 2;
#else
	const unsigned cases = 3;
#endif
	for (unsigned issue = 0; issue < cases; ++issue) {
		launcher_test::Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK);
		ASSERT_EQ(configure_launch_runner(f), LAUNCHER_OK);
		std::filesystem::remove(f.install / "game.exe");
		if (issue == 1)
			std::filesystem::create_directory(
				f.install / "game.exe");
#ifndef _WIN32
		if (issue == 2)
			std::filesystem::create_symlink(f.install / "data.bin",
				f.install / "game.exe");
#endif
		int code = 99;

		EXPECT_NE(launcher_launch(f.handle, nullptr, 0, &code,
			&f.error), LAUNCHER_OK) << issue;
		EXPECT_EQ(code, -1);
		EXPECT_FALSE(std::filesystem::exists(
			f.install / "launch-gate-output"));
	}
}

#ifndef _WIN32
TEST(Launch, SubstitutedInstallationOrSidecarBlocksLaunch)
{
	for (bool sidecar : {false, true}) {
		launcher_test::Fixture f;
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(f.configure(), LAUNCHER_OK);
		ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
			LAUNCHER_OK);
		ASSERT_EQ(configure_launch_runner(f), LAUNCHER_OK);
		auto path = sidecar ? f.base / "game.launcher" : f.install;
		auto moved = f.base / "substituted";

		std::filesystem::rename(path, moved);
		std::filesystem::create_directory(path);
		int code = 99;

		EXPECT_EQ(launcher_launch(f.handle, nullptr, 0, &code,
			&f.error), LAUNCHER_CONFLICT)
			<< sidecar << ": " << f.error.message;
		EXPECT_EQ(code, -1);
		EXPECT_FALSE(std::filesystem::exists(
			f.install / "launch-gate-output"));
	}
}
#endif

#ifdef LAUNCHER_TEST_IO_COUNTS
static unsigned directory_flushes;
static bool count_directory_flushes;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int descriptor)
{
	struct stat info{};

	if (count_directory_flushes && !fstat(descriptor, &info) &&
		S_ISDIR(info.st_mode))
		++directory_flushes;
	return __real_fsync(descriptor);
}

TEST_F(Platform, RenameFlushesEachDistinctParentOnce)
{
	Open();
	launcher_test::write(std::filesystem::path(root) / "value.tmp", "data");
	directory_flushes = 0;
	count_directory_flushes = true;
	launcher_status status = lx_fs_replace(fs, "value.tmp", fs,
		"value", &error);
	count_directory_flushes = false;
	ASSERT_EQ(status, LAUNCHER_OK) << error.message;
	EXPECT_EQ(directory_flushes, 1u);

	ASSERT_EQ(lx_fs_mkdirs(fs, "other", &error), LAUNCHER_OK);
	directory_flushes = 0;
	count_directory_flushes = true;
	status = lx_fs_replace(fs, "value", fs, "other/value", &error);
	count_directory_flushes = false;
	ASSERT_EQ(status, LAUNCHER_OK) << error.message;
	EXPECT_EQ(directory_flushes, 2u);
}
#endif
