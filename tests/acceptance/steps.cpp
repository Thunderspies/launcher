#include <gtest/gtest.h>
#include <piggle/piggle.h>
#include <cucumber-cpp/autodetect.hpp>
#include "../support.h"
#include <stdlib.h>

using cucumber::ScenarioScope;
namespace fs = std::filesystem;

struct Scenario {
	fs::path base;
	fs::path install;
	fs::path public_root;
	launcher_test::HttpServer *file_server = nullptr;
	launcher_test::HttpServer *empty_server = nullptr;
	launcher_handle *handle = nullptr;
	launcher_error error{};
	launcher_status result = LAUNCHER_OK;
	int exit_code = -1;
	std::map<fs::path, fs::file_time_type> retained;
	char access[32]{};
	char secret[32]{};
	char token[32]{};

	Scenario()
	{
		auto stamp = std::chrono::steady_clock::now()
			.time_since_epoch().count();
		base = fs::temp_directory_path() /
			("launcher-scenario-" + std::to_string(stamp));
		install = base / "game";
		fs::create_directories(base);
	}

	~Scenario()
	{
		launcher_close(&handle);
		delete file_server;
		delete empty_server;
		std::error_code ignored;
		fs::remove_all(base, ignored);
	}
};

GIVEN("^a publisher repository is served over HTTP$")
{
	ScenarioScope<Scenario> state;
	const char *root = getenv("LAUNCHER_HTTP_ROOT");
	const char *url = getenv("LAUNCHER_HTTP_URL");
	ASSERT_NE(root, nullptr);
	ASSERT_NE(url, nullptr);
	state->public_root = root;
	fs::remove_all(state->public_root);
	fs::copy(fs::path(LAUNCHER_FIXTURE_DIR) / "public", state->public_root,
		fs::copy_options::recursive | fs::copy_options::overwrite_existing);
	fs::copy(fs::path(LAUNCHER_FIXTURE_DIR) / "file-public",
		state->public_root, fs::copy_options::recursive |
			fs::copy_options::overwrite_existing);
	ASSERT_EQ(launcher_open(state->install.string().c_str(),
		&state->handle, &state->error), LAUNCHER_OK)
		<< state->error.message;
	launcher_config config{};
	config.base_url = url;
	launcher_file_repository files{url, nullptr};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.game = "game.exe";
	ASSERT_EQ(launcher_configure(state->handle, &config, &state->error),
		LAUNCHER_OK) << state->error.message;
}

GIVEN("^release r1 is installed$")
{
	ScenarioScope<Scenario> state;
	ASSERT_EQ(launcher_update(state->handle, nullptr, nullptr,
		&state->error), LAUNCHER_OK) << state->error.message;
}

static launcher_status set_credentials(Scenario *state)
{
	strcpy(state->access, "fixture-access-key");
	strcpy(state->secret, "fixture-secret-key");
	strcpy(state->token, "fixture-session-token");
	launcher_s3_credentials credentials = {
		state->access, state->secret, state->token
	};
	launcher_status status = launcher_set_s3_credentials(state->handle,
		&credentials, &state->error);

	if (status == LAUNCHER_OK)
		status = launcher_set_file_s3_credentials(state->handle, 0,
			&credentials, &state->error);
	memset(state->access, 'x', strlen(state->access));
	memset(state->secret, 'x', strlen(state->secret));
	memset(state->token, 'x', strlen(state->token));
	return status;
}

GIVEN("^the repository requires S3 credentials$")
{
	ScenarioScope<Scenario> state;
	const char *url = getenv("LAUNCHER_S3_URL");
	launcher_config config{};
	std::string ca = (fs::path(LAUNCHER_FIXTURE_DIR) /
		"localhost-cert.pem").string();

	ASSERT_NE(url, nullptr);
	ASSERT_EQ(launcher_get_config(state->handle, &config, &state->error),
		LAUNCHER_OK);
	config.base_url = url;
	config.s3_region = "auto";
	launcher_file_repository files{url, "auto"};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.ca_bundle = ca.c_str();
	ASSERT_EQ(launcher_configure(state->handle, &config, &state->error),
		LAUNCHER_OK) << state->error.message;
	ASSERT_EQ(set_credentials(&*state), LAUNCHER_OK)
		<< state->error.message;
}

GIVEN("^the installation is reopened with fresh S3 credentials$")
{
	ScenarioScope<Scenario> state;

	launcher_close(&state->handle);
	fs::remove_all(fs::path(state->install.string() + ".launcher") /
		"objects");
	ASSERT_EQ(launcher_open(state->install.string().c_str(),
		&state->handle, &state->error), LAUNCHER_OK)
		<< state->error.message;
	ASSERT_EQ(set_credentials(&*state), LAUNCHER_OK)
		<< state->error.message;
}

GIVEN("^the S3 credentials are invalid$")
{
	ScenarioScope<Scenario> state;
	launcher_s3_credentials credentials = {
		"fixture-access-key", "incorrect-secret",
		"fixture-session-token"
	};

	ASSERT_EQ(launcher_set_s3_credentials(state->handle, &credentials,
		&state->error), LAUNCHER_OK) << state->error.message;
}

GIVEN("^the release supplies a game command without local game settings$")
{
	ScenarioScope<Scenario> state;
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(state->handle, &config, &state->error),
		LAUNCHER_OK);
	config.game = nullptr;
	ASSERT_EQ(launcher_configure(state->handle, &config, &state->error),
		LAUNCHER_OK) << state->error.message;
	launcher_test::publish_launch(state->public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r1\",\"entry_point\":\"game.exe\","
		"\"run_arguments\":[\"-test\",\"two words\",\"\"]}");
}

WHEN("^the installation is reopened after the channel moves$")
{
	ScenarioScope<Scenario> state;
	ASSERT_EQ(state->result, LAUNCHER_OK) << state->error.message;
	launcher_test::publish_launch(state->public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r2\","
		"\"entry_point\":\"game.exe\","
		"\"run_arguments\":[\"changed\"]}");
	launcher_close(&state->handle);
	ASSERT_EQ(launcher_open(state->install.string().c_str(),
		&state->handle, &state->error), LAUNCHER_OK)
		<< state->error.message;
}

THEN("^the installed release retains its published game command$")
{
	ScenarioScope<Scenario> state;
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(state->handle, &installed,
		&state->error), LAUNCHER_OK);
	EXPECT_STREQ(launcher_release_id(installed), "r1");
	launcher_launch_info launch{};
	ASSERT_EQ(launcher_release_get_launch(installed, &launch,
		&state->error), LAUNCHER_OK);
	EXPECT_STREQ(launch.entry_point, "game.exe");
	ASSERT_EQ(launch.run_argument_count, 3u);
	EXPECT_STREQ(launch.run_arguments[0], "-test");
	EXPECT_STREQ(launch.run_arguments[1], "two words");
	EXPECT_STREQ(launch.run_arguments[2], "");
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(state->handle, &config, &state->error),
		LAUNCHER_OK);
	EXPECT_STREQ(config.game, "");
}

GIVEN("^the installation has unrelated personal settings$")
{
	ScenarioScope<Scenario> state;
	launcher_test::write(state->install / "settings.ini", "personal");
}

GIVEN("^the channel is pinned to "
	"(r1|r2|empty|compressed|headers|headerobjects|multi)$")
{
	REGEX_PARAM(std::string, release);
	ScenarioScope<Scenario> state;
	launcher_test::write(state->public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"" + release + "\"}\n");
}

GIVEN("^the installed game has been damaged$")
{
	ScenarioScope<Scenario> state;
	launcher_test::write(state->install / "game.exe", "damaged");
}

GIVEN("^the channel metadata is malformed$")
{
	ScenarioScope<Scenario> state;
	launcher_test::write(state->public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"r2\",\"unknown\":true}");
}

WHEN("^the launcher updates the installation$")
{
	ScenarioScope<Scenario> state;
	state->result = launcher_update(state->handle, nullptr, nullptr,
		&state->error);
}

WHEN("^the launcher repairs the installation$")
{
	ScenarioScope<Scenario> state;
	state->result = launcher_repair(state->handle, nullptr, nullptr,
		&state->error);
}

THEN("^release (r1|r2|empty|compressed|headers|headerobjects|multi)"
	" is installed and verifies$")
{
	REGEX_PARAM(std::string, release);
	ScenarioScope<Scenario> state;
	ASSERT_EQ(state->result, LAUNCHER_OK) << state->error.message;
	const launcher_release *installed = nullptr;
	ASSERT_EQ(launcher_get_installed(state->handle, &installed,
		&state->error), LAUNCHER_OK) << state->error.message;
	EXPECT_EQ(release, launcher_release_id(installed));
	ASSERT_EQ(launcher_verify(state->handle, nullptr, nullptr,
		&state->error), LAUNCHER_OK) << state->error.message;
}

THEN("^the game contains the bytes from (r1|r2)$")
{
	REGEX_PARAM(std::string, release);
	ScenarioScope<Scenario> state;
	EXPECT_EQ(launcher_test::read(state->install / "game.exe"),
		release == "r1" ? "game-one\n" : "game-two\n");
}

THEN("^the personal settings are preserved$")
{
	ScenarioScope<Scenario> state;
	EXPECT_EQ(launcher_test::read(state->install / "settings.ini"),
		"personal");
}

THEN("^the update reports invalid metadata$")
{
	ScenarioScope<Scenario> state;
	EXPECT_EQ(state->result, LAUNCHER_METADATA) << state->error.message;
}

THEN("^the update reports an authentication failure$")
{
	ScenarioScope<Scenario> state;

	EXPECT_EQ(state->result, LAUNCHER_NETWORK) << state->error.message;
	EXPECT_EQ(state->error.http_status, 403);
}

THEN("^the managed loose files are removed$")
{
	ScenarioScope<Scenario> state;
	EXPECT_FALSE(fs::exists(state->install / "game.exe"));
	EXPECT_FALSE(fs::exists(state->install / "data.bin"));
	EXPECT_TRUE(fs::exists(state->install / "piggs/assets.hogg"));
}

GIVEN("^an accepted cache object has same-size corruption$")
{
	ScenarioScope<Scenario> state;
	const launcher_release *release = nullptr;
	ASSERT_EQ(launcher_get_installed(state->handle, &release,
		&state->error), LAUNCHER_OK);
	launcher_file file{};
	ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_LOOSE, 0, &file,
		&state->error), LAUNCHER_OK);
	launcher_test::write(state->base / "game.launcher/objects" /
		file.sha256,
		std::string(file.stored_size, '!'));
}

WHEN("^the launcher verifies the installation$")
{
	ScenarioScope<Scenario> state;
	state->result = launcher_verify(state->handle, nullptr, nullptr,
		&state->error);
}

THEN("^verification reports integrity failure and discards the cache object$")
{
	ScenarioScope<Scenario> state;
	EXPECT_EQ(state->result, LAUNCHER_INTEGRITY) << state->error.message;
	const launcher_release *release = nullptr;
	ASSERT_EQ(launcher_get_installed(state->handle, &release,
		&state->error), LAUNCHER_OK);
	launcher_file file{};
	ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_LOOSE, 0, &file,
		&state->error), LAUNCHER_OK);
	EXPECT_FALSE(fs::exists(state->base / "game.launcher/objects" /
		file.sha256));
}

THEN("^the archive contains the exact publisher stored objects$")
{
	ScenarioScope<Scenario> state;
	const launcher_release *release = nullptr;
	ASSERT_EQ(launcher_get_installed(state->handle, &release,
		&state->error), LAUNCHER_OK);
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_error error{};
	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_source_open(context, (state->install /
		launcher_test::test_hogg(release)).string().c_str(), nullptr,
		&source, &error), PG_OK);
	for (size_t i = 0; i < launcher_test::test_count(release, LAUNCHER_HOGG);
		++i) {
		launcher_file file{};
		ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_HOGG, i,
			&file, &state->error), LAUNCHER_OK);
		pg_reader *reader = nullptr;
		ASSERT_EQ(pg_reader_open_source(source, file.path,
			PG_READ_STORED_UNCHECKED, &reader, &error), PG_OK);
		std::string bytes;
		for (;;) {
			char chunk[1024];
			size_t count;
			pg_status status = pg_reader_read(reader, chunk,
				sizeof(chunk), &count, &error);
			if (status == PG_END)
				break;
			ASSERT_EQ(status, PG_OK);
			bytes.append(chunk, count);
		}
		EXPECT_EQ(bytes, launcher_test::read(state->public_root /
			"hogg" / file.sha256 / "BLOB"));
		ASSERT_EQ(pg_reader_close(&reader, &error), PG_OK);
	}
	ASSERT_EQ(pg_source_close(&source, &error), PG_OK);
	ASSERT_EQ(pg_context_close(&context, &error), PG_OK);
}

static void cached_headers(Scenario *state, int clear)
{
	pg_context *context = nullptr;
	pg_source *source = nullptr;
	pg_source_options options{PG_HOGG10,
		clear ? (uint32_t)PG_WRITE : (uint32_t)PG_READ, 0};
	pg_error error{};
	ASSERT_EQ(pg_context_open(&context, &error), PG_OK);
	ASSERT_EQ(pg_source_open(context, (state->install /
		"piggs/assets.hogg").string().c_str(), &options, &source,
		&error), PG_OK);
	for (const char *name : {"a.bin", "b.bin"}) {
		pg_file *file = nullptr;
		ASSERT_EQ(pg_source_find(source, name, &file, &error), PG_OK);
		if (clear) {
			pg_metadata_options metadata{PG_METADATA_HEADER, 0,
				nullptr, 0};
			ASSERT_EQ(pg_file_update_metadata(file, &metadata,
				&error),
				PG_OK);
		} else {
			pg_file_info info{};
			ASSERT_EQ(pg_file_inspect(file, &info, &error), PG_OK);
			std::string expected = !strcmp(name, "a.bin") ?
				std::string("\0\xffhead\0", 7) : "other";
			ASSERT_EQ(info.cached_header_size, expected.size());
			EXPECT_EQ(memcmp(info.cached_header, expected.data(),
				expected.size()), 0);
		}
		ASSERT_EQ(pg_file_close(&file, &error), PG_OK);
	}
	ASSERT_EQ(pg_source_close(&source, &error), PG_OK);
	ASSERT_EQ(pg_context_close(&context, &error), PG_OK);
}

THEN("^the archive retains the published binary cached headers$")
{
	ScenarioScope<Scenario> state;
	cached_headers(&*state, 0);
}

GIVEN("^the archive cached headers are missing$")
{
	ScenarioScope<Scenario> state;
	cached_headers(&*state, 1);
}

GIVEN("^file descriptors use a separate repository after an empty mirror$")
{
	ScenarioScope<Scenario> state;
	fs::path files = state->base / "files";
	fs::copy(fs::path(LAUNCHER_FIXTURE_DIR) / "file-public", files,
		fs::copy_options::recursive | fs::copy_options::overwrite_existing);
	state->file_server = new launcher_test::HttpServer(files);
	state->empty_server = new launcher_test::HttpServer(
		state->base / "empty");
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(state->handle, &config, &state->error),
		LAUNCHER_OK);
	launcher_file_repository repositories[] = {
		{state->empty_server->url.c_str(), nullptr},
		{state->file_server->url.c_str(), nullptr}
	};
	config.file_repositories = repositories;
	config.file_repository_count = 2;
	ASSERT_EQ(launcher_configure(state->handle, &config, &state->error),
		LAUNCHER_OK);
}

THEN("^only the selected file repository supplies the payloads$")
{
	ScenarioScope<Scenario> state;
	EXPECT_EQ(state->empty_server->descriptor_requests, 0u);
	EXPECT_EQ(state->empty_server->object_requests, 0u);
	EXPECT_EQ(state->file_server->descriptor_requests, 3u);
	EXPECT_EQ(state->file_server->object_requests, 2u);
}

GIVEN("^the public manifest uses a private file repository$")
{
	ScenarioScope<Scenario> state;
	const char *url = getenv("LAUNCHER_S3_URL");
	ASSERT_NE(url, nullptr);
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(state->handle, &config, &state->error),
		LAUNCHER_OK);
	launcher_file_repository files{url, "auto"};
	std::string ca = (fs::path(LAUNCHER_FIXTURE_DIR) /
		"localhost-cert.pem").string();
	config.file_repositories = &files;
	config.file_repository_count = 1;
	config.ca_bundle = ca.c_str();
	ASSERT_EQ(launcher_configure(state->handle, &config, &state->error),
		LAUNCHER_OK);
	launcher_s3_credentials keys{"fixture-access-key", "fixture-secret-key",
		"fixture-session-token"};
	ASSERT_EQ(launcher_set_file_s3_credentials(state->handle, 0, &keys,
		&state->error), LAUNCHER_OK);
}

GIVEN("^the header objects are missing and file repositories are offline$")
{
	ScenarioScope<Scenario> state;
	for (const char *digest : {
		"b37515ea1faff2e1387156c999a1936aa"
		"ce2eb60180d35b145cf634bd646388d",
		"d9298a10d1b0735837dc4bd85dac641b"
		"0f3cef27a47e5d53a54f2f3f5b2fcffa"}) {
		fs::path path = state->base / "game.launcher/objects" / digest;
		ASSERT_TRUE(fs::remove(path));
	}
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(state->handle, &config, &state->error),
		LAUNCHER_OK);
	launcher_file_repository offline{"http://127.0.0.1:1/", nullptr};
	config.file_repositories = &offline;
	config.file_repository_count = 1;
	ASSERT_EQ(launcher_configure(state->handle, &config, &state->error),
		LAUNCHER_OK);
	launcher_close(&state->handle);
	ASSERT_EQ(launcher_open(state->install.string().c_str(),
		&state->handle, &state->error), LAUNCHER_OK);
}

GIVEN("^the release contains two HOGG groups$")
{
	ScenarioScope<Scenario> state;
	launcher_test::multiple_archives(state->public_root);
	launcher_test::write(state->public_root / "channels/live.json",
		"{\"format\":1,\"release\":\"multi\"}");
}

THEN("^both managed archives are installed$")
{
	ScenarioScope<Scenario> state;
	EXPECT_TRUE(fs::exists(state->install / "piggs/assets.hogg"));
	EXPECT_TRUE(fs::exists(state->install / "piggs/second.hogg"));
}

GIVEN("^the release is recommitted without any file groups$")
{
	ScenarioScope<Scenario> state;
	launcher_test::edit_json(state->public_root / "releases/multi.json",
		[](yyjson_mut_doc *doc, yyjson_mut_val *root) {
			yyjson_mut_obj_put(root, yyjson_mut_str(doc, "groups"),
				yyjson_mut_obj(doc));
			yyjson_mut_obj_put(root, yyjson_mut_str(doc, "loose"),
				yyjson_mut_arr(doc));
		});
}

THEN("^all managed archives and loose files are removed$")
{
	ScenarioScope<Scenario> state;
	EXPECT_FALSE(fs::exists(state->install / "piggs/assets.hogg"));
	EXPECT_FALSE(fs::exists(state->install / "piggs/second.hogg"));
	EXPECT_FALSE(fs::exists(state->install / "game.exe"));
	EXPECT_FALSE(fs::exists(state->install / "data.bin"));
}

GIVEN("^the installed physical files are marked for preservation$")
{
	ScenarioScope<Scenario> state;
	auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(24);

	for (const auto &item : fs::recursive_directory_iterator(state->install)) {
		if (!item.is_regular_file())
			continue;
		fs::last_write_time(item.path(), stamp);
		state->retained[item.path()] = fs::last_write_time(item.path());
	}
	fs::remove_all(fs::path(state->install.string() + ".launcher") /
		"objects");
}

GIVEN("^the release is recommitted with a different launch argument$")
{
	ScenarioScope<Scenario> state;
	launcher_test::publish_launch(state->public_root / "channels/live.json",
		"{\"release\":\"r1\",\"entry_point\":\"game.exe\","
		"\"run_arguments\":[\"updated\"]}");
}

THEN("^the installed physical files are preserved without cache population$")
{
	ScenarioScope<Scenario> state;
	for (const auto &item : state->retained)
		EXPECT_EQ(fs::last_write_time(item.first), item.second)
			<< item.first;
	EXPECT_TRUE(fs::is_empty(fs::path(state->install.string() +
		".launcher") / "objects"));
	const launcher_release *release = nullptr;
	ASSERT_EQ(launcher_get_installed(state->handle, &release,
		&state->error), LAUNCHER_OK);
	launcher_launch_info launch{};
	ASSERT_EQ(launcher_release_get_launch(release, &launch,
		&state->error), LAUNCHER_OK);
	ASSERT_EQ(launch.run_argument_count, 1u);
	EXPECT_STREQ(launch.run_arguments[0], "updated");
}

GIVEN("^the game uses a controlled runner$")
{
	ScenarioScope<Scenario> state;
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(state->handle, &config, &state->error),
		LAUNCHER_OK);
	std::string child = fs::absolute(LAUNCHER_TEST_CHILD).string();
	const char *arguments[] = { "0", "0", "launch-arguments" };

	config.runner = child.c_str();
	config.runner_args = arguments;
	config.runner_arg_count = 3;
	ASSERT_EQ(launcher_configure(state->handle, &config, &state->error),
		LAUNCHER_OK);
}

GIVEN("^managed files contain local changes$")
{
	ScenarioScope<Scenario> state;
	fs::remove(state->install / "piggs/assets.hogg");
	launcher_test::write(state->install / "game.exe", "local executable");
	launcher_test::write(state->install / "data.bin", "local managed data");
}

WHEN("^the launcher starts the saved game$")
{
	ScenarioScope<Scenario> state;
	state->result = launcher_launch(state->handle, nullptr, 0,
		&state->exit_code, &state->error);
}

THEN("^the controlled game completes successfully$")
{
	ScenarioScope<Scenario> state;
	ASSERT_EQ(state->result, LAUNCHER_OK) << state->error.message;
	EXPECT_EQ(state->exit_code, 0);
	EXPECT_TRUE(fs::is_regular_file(state->install / "launch-arguments"));
}

THEN("^verification reports local content changes$")
{
	ScenarioScope<Scenario> state;
	EXPECT_EQ(state->result, LAUNCHER_INTEGRITY) << state->error.message;
}
