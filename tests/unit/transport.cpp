#include "../descriptors.h"
#include "internal.h"
#include <gtest/gtest.h>
#include <string.h>
#include <vector>

static const char *abc_sha256 =
	"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";

TEST(Transport, StreamHashRequiresExactLengthAndDigest)
{
	FILE *file = tmpfile();
	ASSERT_NE(nullptr, file);
	ASSERT_EQ(3U, fwrite("abc", 1, 3, file));
	rewind(file);
	EXPECT_EQ(LAUNCHER_OK, lx_hash_file(file, 3, abc_sha256, NULL, NULL));
	rewind(file);
	EXPECT_EQ(LAUNCHER_INTEGRITY,
		lx_hash_file(file, 2, abc_sha256, NULL, NULL));
	rewind(file);
	EXPECT_EQ(LAUNCHER_INTEGRITY,
		lx_hash_file(file, 4, abc_sha256, NULL, NULL));
	rewind(file);
	EXPECT_EQ(LAUNCHER_INTEGRITY, lx_hash_file(file, 3,
		"00000000000000000000000000000000"
		"00000000000000000000000000000000",
		NULL, NULL));
	fclose(file);
}

TEST(Transport, HashEmptyPayloadAndCopyExactBytes)
{
	FILE *source = tmpfile();
	FILE *copy = tmpfile();
	ASSERT_NE(nullptr, source);
	ASSERT_NE(nullptr, copy);
	EXPECT_EQ(LAUNCHER_OK, lx_hash_file(source, 0,
		"e3b0c44298fc1c149afbf4c8996fb92427"
		"ae41e4649b934ca495991b7852b855",
		copy, NULL));
	ASSERT_EQ(3U, fwrite("abc", 1, 3, source));
	rewind(source);
	ASSERT_EQ(LAUNCHER_OK,
		lx_hash_file(source, 3, abc_sha256, copy, NULL));
	rewind(copy);
	char bytes[4] = {};
	EXPECT_EQ(3U, fread(bytes, 1, 4, copy));
	EXPECT_STREQ("abc", bytes);
	fclose(copy);
	fclose(source);
}

#include "../support.h"

using launcher_test::Fixture;
using launcher_test::HttpServer;
using launcher_test::read;
using launcher_test::write;
namespace fs = std::filesystem;

static std::string game_object(Fixture &fixture, const char *id)
{
	std::string json = read(fixture.public_root / "releases" /
		(std::string(id) + ".json"));
	launcher_test::FileDescriptors descriptors{fixture.public_root, {}};
	launcher_release *release = NULL;
	launcher_status status = launcher_decode_release(json.data(),
		json.size(), id, launcher_test::FileDescriptors::lookup,
		&descriptors, &release, &fixture.error);

	if (status != LAUNCHER_OK)
		throw std::runtime_error("Cannot decode fixture release");
	std::string digest;

	for (size_t i = 0; i < launcher_test::test_count(release,
		LAUNCHER_LOOSE); i++) {
		launcher_file file = {};

		launcher_test::test_file(release, LAUNCHER_LOOSE, i, &file, NULL);
		if (!strcmp(file.path, "game.exe"))
			digest = file.sha256;
	}
	launcher_release_free(&release);
	return "loose/" + digest + "/BLOB";
}

TEST(Transport, OnlyComplete200IdentityMetadataIsAccepted)
{
	for (int status : {206, 302, 404}) {
		Fixture f;
		ASSERT_EQ(LAUNCHER_OK, f.open()) << f.error.message;
		ASSERT_EQ(LAUNCHER_OK, f.configure()) << f.error.message;
		HttpServer::Response response;

		response.status = status;
		response.body = read(f.public_root / "channels/live.json");
		f.server->set_response("channels/live.json", response);
		launcher_release *release = NULL;

		EXPECT_EQ(LAUNCHER_NETWORK, launcher_resolve(f.handle, &release,
			NULL, NULL, &f.error));
		EXPECT_EQ(status, f.error.http_status);
		EXPECT_EQ(nullptr, release);
	}
	Fixture f;
	ASSERT_EQ(LAUNCHER_OK, f.open());
	ASSERT_EQ(LAUNCHER_OK, f.configure());
	HttpServer::Response response;

	response.body = read(f.public_root / "channels/live.json");
	response.encoding = "gzip";
	f.server->set_response("channels/live.json", response);
	launcher_release *release = NULL;

	EXPECT_EQ(LAUNCHER_NETWORK, launcher_resolve(f.handle, &release,
		NULL, NULL, &f.error));
	EXPECT_EQ(nullptr, release);
	EXPECT_FALSE(fs::exists(f.install / "game.exe"));
}

TEST(Transport, MetadataRetrievalEnforcesInclusive16MiBBound)
{
	Fixture f;
	ASSERT_EQ(LAUNCHER_OK, f.open());
	ASSERT_EQ(LAUNCHER_OK, f.configure());
	HttpServer::Response response;

	response.body = read(f.public_root / "channels/live.json");
	response.body.resize(16 * 1024 * 1024, ' ');
	f.server->set_response("channels/live.json", response);
	launcher_release *release = NULL;

	ASSERT_EQ(LAUNCHER_OK, launcher_resolve(f.handle, &release,
		NULL, NULL, &f.error)) << f.error.message;
	EXPECT_STREQ("r1", launcher_release_id(release));
	launcher_release_free(&release);
	response.body.push_back(' ');
	f.server->set_response("channels/live.json", response);
	EXPECT_EQ(LAUNCHER_METADATA, launcher_resolve(f.handle, &release,
		NULL, NULL, &f.error)) << f.error.message;
	EXPECT_EQ(nullptr, release);
	EXPECT_FALSE(fs::exists(f.install / "piggs/assets.hogg"));
}

TEST(Transport, FailedObjectsKeepOldInstallationUsable)
{
	for (int failure = 0; failure < 7; failure++) {
		SCOPED_TRACE(failure);
		Fixture f;
		ASSERT_EQ(LAUNCHER_OK, f.open()) << f.error.message;
		ASSERT_EQ(LAUNCHER_OK, f.configure()) << f.error.message;
		ASSERT_EQ(LAUNCHER_OK, launcher_update(f.handle, NULL, NULL,
			&f.error)) << f.error.message;
		f.pin("r2");
		std::string object = game_object(f, "r2");
		HttpServer::Response response;

		response.body = read(f.public_root / object);
		launcher_status expected = LAUNCHER_INTEGRITY;

		switch (failure) {
		case 0:
			response.body += "excess";
			break;
		case 1:
			response.body.pop_back();
			break;
		case 2:
			response.body.assign(response.body.size(), 'x');
			break;
		case 3:
			response.declared_size = response.body.size();
			response.body.pop_back();
			expected = LAUNCHER_NETWORK;
			break;
		case 4:
			response.status = 404;
			response.body = "missing";
			expected = LAUNCHER_NETWORK;
			break;
		case 5:
			response.status = 206;
			expected = LAUNCHER_NETWORK;
			break;
		case 6:
			response.encoding = "gzip";
			expected = LAUNCHER_NETWORK;
			break;
		}
		f.server->set_response(object, response);
		EXPECT_EQ(expected, launcher_update(f.handle, NULL, NULL,
			&f.error)) << f.error.message;
		EXPECT_EQ("game-one\n", read(f.install / "game.exe"));
		EXPECT_EQ(LAUNCHER_OK, launcher_verify(f.handle, NULL, NULL,
			&f.error)) << f.error.message;
		const launcher_release *installed = NULL;

		ASSERT_EQ(LAUNCHER_OK, launcher_get_installed(f.handle,
			&installed, &f.error));
		EXPECT_STREQ("r1", launcher_release_id(installed));
		fs::path sidecar = f.install.string() + ".launcher";

		EXPECT_FALSE(fs::exists(sidecar / "objects" /
			(fs::path(object).parent_path().filename().string() + ".part")));
		EXPECT_FALSE(fs::exists(sidecar / "journal.json"));
	}
}

TEST(Transport, ExplicitVerificationEnablesRepairFromInstalledBytesOrNetwork)
{
	Fixture f;
	ASSERT_EQ(LAUNCHER_OK, f.open());
	ASSERT_EQ(LAUNCHER_OK, f.configure());
	ASSERT_EQ(LAUNCHER_OK, launcher_update(f.handle, NULL, NULL, &f.error))
		<< f.error.message;
	fs::path cache = fs::path(f.install.string() + ".launcher") /
		"objects" / fs::path(game_object(f, "r1")).parent_path().filename();
	size_t requests = f.server->object_requests;

	write(cache, "damaged!!");
	ASSERT_EQ(launcher_verify(f.handle, NULL, NULL, &f.error),
		LAUNCHER_INTEGRITY);
	EXPECT_FALSE(fs::exists(cache));
	ASSERT_EQ(LAUNCHER_OK, launcher_repair(f.handle, NULL, NULL, &f.error))
		<< f.error.message;
	EXPECT_EQ(requests, f.server->object_requests);
	EXPECT_EQ("game-one\n", read(cache));
	write(cache, "damaged!!");
	ASSERT_EQ(launcher_verify(f.handle, NULL, NULL, &f.error),
		LAUNCHER_INTEGRITY);
	EXPECT_FALSE(fs::exists(cache));
	write(f.install / "game.exe", "damaged!!");
	ASSERT_EQ(LAUNCHER_OK, launcher_repair(f.handle, NULL, NULL, &f.error))
		<< f.error.message;
	EXPECT_EQ(requests + 1, f.server->object_requests);
	EXPECT_EQ("game-one\n", read(cache));
	EXPECT_EQ("game-one\n", read(f.install / "game.exe"));
}

struct moving_channel {
	Fixture *fixture;
	bool changed;
};

static int move_channel_during_download(const launcher_progress *progress,
	void *state)
{
	moving_channel *move = (moving_channel *)state;

	if (progress->phase == LAUNCHER_DOWNLOAD && progress->completed &&
		!move->changed) {
		move->fixture->pin("r2");
		move->changed = true;
	}
	return 1;
}

TEST(Transport, ChannelMoveDuringDownloadKeepsResolvedTarget)
{
	Fixture f;
	ASSERT_EQ(LAUNCHER_OK, f.open());
	ASSERT_EQ(LAUNCHER_OK, f.configure());
	moving_channel move = { &f, false };

	ASSERT_EQ(LAUNCHER_OK, launcher_update(f.handle,
		move_channel_during_download, &move, &f.error))
		<< f.error.message;
	ASSERT_TRUE(move.changed);
	EXPECT_EQ("game-one\n", read(f.install / "game.exe"));
	const launcher_release *installed = NULL;

	ASSERT_EQ(LAUNCHER_OK, launcher_get_installed(f.handle, &installed,
		&f.error));
	EXPECT_STREQ("r1", launcher_release_id(installed));
	ASSERT_EQ(LAUNCHER_OK, launcher_update(f.handle, NULL, NULL, &f.error))
		<< f.error.message;
	EXPECT_EQ("game-two\n", read(f.install / "game.exe"));
}

static launcher_status split_config(Fixture &f, const std::string &first)
{
	launcher_config config{};
	launcher_file_repository files[] = {
		{first.c_str(), nullptr}, {f.server->url.c_str(), nullptr}
	};
	config.base_url = f.server->url.c_str();
	config.file_repositories = files;
	config.file_repository_count = 2;
	return launcher_configure(f.handle, &config, &f.error);
}

TEST(Repositories, Ordered404FallbackDeduplicatesAndCachesDescriptors)
{
	Fixture f;
	HttpServer missing(f.base / "missing");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(split_config(f, missing.url), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(missing.descriptor_requests, 0u);
	EXPECT_EQ(missing.object_requests, 0u);
	EXPECT_EQ(f.server->descriptor_requests, 3u);
	EXPECT_EQ(f.server->object_requests, 2u);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(missing.descriptor_requests, 0u);
	EXPECT_EQ(f.server->descriptor_requests, 3u);
	EXPECT_EQ(f.server->object_requests, 2u);
	EXPECT_EQ(f.server->authenticated_requests, 0u);
	EXPECT_EQ(missing.authenticated_requests, 0u);
}

TEST(Repositories, DescriptorFailuresAreTerminal)
{
	for (int failure = 0; failure < 9; ++failure) {
		Fixture f;
		fs::path first_root = f.base / "first";
		fs::create_directories(first_root);
		fs::copy(f.public_root, first_root, fs::copy_options::recursive);
		HttpServer first(first_root);
		fs::path directory = first_root;
		for (const auto &file : fs::recursive_directory_iterator(directory)) {
			if (file.path().parent_path().filename() != "revisions")
				continue;
			HttpServer::Response response;
			response.body = read(file.path());
			if (failure < 3)
				response.status = failure == 0 ? 403 :
					failure == 1 ? 500 : 206;
			if (failure == 3)
				response.encoding = "gzip";
			if (failure == 4)
				response.body += ' ';
			if (failure == 5) {
				response.declared_size = response.body.size();
				response.body.pop_back();
			}
			if (failure == 7 || failure == 8) {
				response.status = 404;
				if (failure == 7) {
					response.declared_size =
						response.body.size();
					response.body.pop_back();
				} else
					response.encoding = "gzip";
			}
			if (failure == 6)
				response.body.resize(16 * 1024 * 1024 + 1, ' ');
			std::string path = fs::relative(file.path(), first_root)
				.generic_string();

			first.set_response(path, response);
		}
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(split_config(f, first.url), LAUNCHER_OK);
		launcher_release *release = nullptr;
		EXPECT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
			&f.error), failure == 4 ? LAUNCHER_INTEGRITY :
			failure == 6 ? LAUNCHER_METADATA : LAUNCHER_NETWORK);
		EXPECT_EQ(release, nullptr);
		EXPECT_EQ(first.descriptor_requests, 1u);
		EXPECT_EQ(f.server->descriptor_requests, 0u);
		EXPECT_FALSE(fs::exists(f.install / "game.exe"));
	}
}

TEST(Repositories, Payload404TriesTheNextLiveProvider)
{
	Fixture f;
	fs::path first_root = f.base / "first";
	fs::copy(f.public_root, first_root, fs::copy_options::recursive);
	for (const auto &file : fs::recursive_directory_iterator(first_root))
		if (file.path().filename() == "BLOB")
			fs::remove(file.path());
	HttpServer first(first_root);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(split_config(f, first.url), LAUNCHER_OK);
	EXPECT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(first.object_requests, 2u);
	EXPECT_EQ(f.server->object_requests, 2u);
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
}

TEST(Repositories, ReopenedRepairLooksUpCurrentSourceOnlyForMissingPayload)
{
	Fixture f;
	HttpServer first(f.public_root);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(split_config(f, first.url), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	fs::remove_all(f.base / "game.launcher/files");
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(first.descriptor_requests, 3u);
	std::string object = game_object(f, "r1");
	fs::remove(f.base / "game.launcher/objects" /
		fs::path(object).parent_path().filename());
	write(f.install / "game.exe", "damaged!");
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(first.descriptor_requests, 4u);
	EXPECT_EQ(first.object_requests, 3u);
	EXPECT_EQ(f.server->object_requests, 0u);
	EXPECT_EQ(read(f.install / "game.exe"), "game-one\n");
}

TEST(Repositories, EmptyListDoesNotFallBackToManifestEndpoint)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config{};
	config.base_url = f.server->url.c_str();
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	launcher_release *release = nullptr;
	EXPECT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_NETWORK);
	EXPECT_EQ(release, nullptr);
	EXPECT_EQ(f.server->descriptor_requests, 0u);
	EXPECT_EQ(f.server->object_requests, 0u);
}

TEST(Repositories, SharedPayloadUsesLexicographicallyFirstFingerprint)
{
	Fixture f;
	launcher_test::FileDescriptors descriptors{f.public_root, {}};
	std::string json = read(f.public_root / "releases/compressed.json");
	launcher_release *release = nullptr;
	ASSERT_EQ(launcher_decode_release(json.data(), json.size(),
		"compressed",
		launcher_test::FileDescriptors::lookup, &descriptors, &release,
		&f.error), LAUNCHER_OK);
	launcher_file compressed{}, raw{};
	ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_HOGG, 0, &compressed,
		nullptr), LAUNCHER_OK);
	ASSERT_EQ(launcher_test::test_file(release, LAUNCHER_LOOSE, 0, &raw,
		nullptr), LAUNCHER_OK);
	ASSERT_STREQ(compressed.sha256, raw.sha256);
	std::string last = (std::max)(std::string(compressed.fingerprint),
		std::string(raw.fingerprint));
	fs::path first_root = f.base / "first";
	write(first_root / "repository.json", read(f.public_root / "repository.json"));
	launcher_test::publish_record(first_root, read(
		launcher_test::record_path(f.public_root, last)));
	HttpServer first(first_root);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config{};
	launcher_file_repository files[] = {
		{first.url.c_str(), nullptr}, {f.server->url.c_str(), nullptr}
	};
	config.base_url = f.server->url.c_str();
	config.channel = "compressed";
	config.file_repositories = files;
	config.file_repository_count = 2;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(first.object_requests, 0u);
	EXPECT_EQ(f.server->object_requests, 3u);
	launcher_release_free(&release);
}

TEST(Repositories, ReopenedResolveReusesDescriptorsAndPlanFindsMissingSource)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_release *release = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK);
	EXPECT_EQ(f.server->descriptor_requests, 3u);
	fs::remove_all(f.base / "game.launcher/objects");
	write(f.install / "game.exe", "short");
	launcher_plan *plan = nullptr;
	ASSERT_EQ(launcher_make_plan(f.handle, release, &plan, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->descriptor_requests, 4u);
	ASSERT_EQ(launcher_apply(f.handle, plan, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	EXPECT_EQ(f.server->object_requests, 3u);
	launcher_plan_free(&plan);
	launcher_release_free(&release);
}

TEST(Repositories, CatalogAndMembershipAreNotConsumerInputs)
{
	Fixture f;
	write(f.public_root / "catalog/00.json", "malformed");
	write(f.public_root / "hogg" / abc_sha256 / "META", "malformed");
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	EXPECT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->request_count("catalog/00.json"), 0u);
	EXPECT_EQ(f.server->request_count(
		"hogg/" + std::string(abc_sha256) + "/META"), 0u);
}

static std::vector<std::string> shard_records(size_t count)
{
	std::vector<std::string> records;

	for (size_t i = 0; records.size() < count; ++i) {
		std::string record = launcher_test::descriptor(abc_sha256, "3", "3",
			"raw", "900150983cd24fb0d6963f7d28e17f72",
			"cache/" + std::to_string(i) + ".bin");

		if (launcher_test::fingerprint(record).substr(0, 2) == "00")
			records.push_back(record);
	}
	return records;
}

static void shard_manifest(Fixture &fixture,
	const std::vector<std::string> &records, size_t offset)
{
	std::string manifest = "{\"format\":2,\"release\":\"cache\","
		"\"groups\":{\"assets\":{"
		"\"path\":\"piggs/cache.hogg\",\"files\":[";

	for (size_t i = offset; i < offset + 3; ++i) {
		if (i != offset)
			manifest += ",";
		manifest += "\"" + launcher_test::publish_record(
			fixture.public_root, records[i]) + "\"";
	}
	manifest +=
		"]}},\"loose\":[],\"entry_point\":null,\"run_arguments\":[]}";
	write(fixture.public_root / "releases/cache.json", manifest);
	fixture.pin("cache");
}

TEST(Repositories, ResolveSharesValidatedShardsOnlyWithinOneOperation)
{
	Fixture f;
	auto records = shard_records(7);
	shard_manifest(f, records, 0);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	launcher_release *release = nullptr;
	std::string meta = "hogg/" + std::string(abc_sha256) + "/META";

	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->request_count("repository.json"), 1u);
	EXPECT_EQ(f.server->request_count("catalog/00.json"), 0u);
	EXPECT_EQ(f.server->request_count(meta), 0u);
	EXPECT_EQ(f.server->descriptor_requests, 3u);
	launcher_release_free(&release);
	shard_manifest(f, records, 3);
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->request_count("repository.json"), 2u);
	EXPECT_EQ(f.server->request_count("catalog/00.json"), 0u);
	EXPECT_EQ(f.server->request_count(meta), 0u);
	EXPECT_EQ(f.server->descriptor_requests, 6u);
	launcher_release_free(&release);
	std::string manifest = read(f.public_root / "releases/cache.json");
	std::string replacement = launcher_test::publish_record(f.public_root,
		records[6]);
	std::string previous = launcher_test::fingerprint(records[3]);

	manifest.replace(manifest.find(previous), previous.size(), replacement);
	write(f.public_root / "releases/cache.json", manifest);
	write(f.public_root / "repository.json",
		"{\"format\":1,\"kind\":\"releases\"}");
	EXPECT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_METADATA);
	EXPECT_EQ(f.server->request_count("repository.json"), 3u);
	EXPECT_EQ(f.server->request_count("catalog/00.json"), 0u);
	EXPECT_EQ(release, nullptr);
}

TEST(Repositories, MissingMarkerAndShardAreScopedToEachOperation)
{
	for (bool marker_missing : {false, true}) {
		SCOPED_TRACE(marker_missing);
		Fixture f;
		auto records = shard_records(6);
		shard_manifest(f, records, 0);
		fs::path first_root = f.base / "first";
		fs::create_directories(first_root);
		if (!marker_missing)
			write(first_root / "repository.json",
				"{\"format\":2,\"kind\":\"files\"}");
		HttpServer first(first_root);
		ASSERT_EQ(f.open(), LAUNCHER_OK);
		ASSERT_EQ(split_config(f, first.url), LAUNCHER_OK);
		launcher_release *release = nullptr;

		ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
			&f.error), LAUNCHER_OK) << f.error.message;
		EXPECT_EQ(first.request_count("repository.json"), 1u);
		EXPECT_EQ(first.request_count("catalog/00.json"),
			0u);
		EXPECT_EQ(f.server->request_count("repository.json"), 1u);
		EXPECT_EQ(f.server->request_count("catalog/00.json"), 0u);
		launcher_release_free(&release);
		shard_manifest(f, records, 3);
		fs::copy(f.public_root, first_root,
			fs::copy_options::recursive |
			fs::copy_options::overwrite_existing);
		ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
			&f.error), LAUNCHER_OK) << f.error.message;
		EXPECT_EQ(first.request_count("repository.json"), 2u);
		EXPECT_EQ(first.request_count("catalog/00.json"),
			0u);
		EXPECT_EQ(f.server->descriptor_requests, 3u);
		EXPECT_EQ(first.descriptor_requests, marker_missing ? 3u : 6u);
		launcher_release_free(&release);
	}
}

TEST(Repositories, ReopenedPlanSharesProviderValidationForMissingObjects)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_release *release = nullptr;
	ASSERT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	fs::remove_all(f.base / "game.launcher/objects");
	for (const auto &entry : fs::directory_iterator(f.install))
		fs::remove_all(entry.path());
	size_t marker_count = f.server->request_count("repository.json");
	launcher_plan *plan = nullptr;

	ASSERT_EQ(launcher_make_plan(f.handle, release, &plan, nullptr, nullptr,
		&f.error), LAUNCHER_OK) << f.error.message;
	EXPECT_EQ(f.server->request_count("repository.json"), marker_count + 1);
	EXPECT_EQ(f.server->descriptor_requests, 5u);
	launcher_plan_free(&plan);
	launcher_release_free(&release);
}

TEST(Repositories, MissingObjectSourceLookupPreservesCachedDescriptor)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	ASSERT_EQ(launcher_update(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
	auto sidecar = f.base / "game.launcher";
	std::map<fs::path, fs::file_time_type> stamps;
	auto stamp = fs::file_time_type::clock::now() -
		std::chrono::hours(24);
	for (const auto &entry : fs::directory_iterator(sidecar / "files")) {
		fs::last_write_time(entry.path(), stamp);
		stamps[entry.path()] = fs::last_write_time(entry.path());
	}
	auto cache = sidecar / "objects" /
		launcher_test::fingerprint("game-one\n");
	fs::remove(cache);
	write(f.install / "game.exe", "damaged!!");
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	auto requests = f.server->descriptor_requests.load();
	ASSERT_EQ(launcher_repair(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK) << f.error.message;
	EXPECT_GT(f.server->descriptor_requests.load(), requests);
	for (const auto &entry : stamps)
		EXPECT_EQ(fs::last_write_time(entry.first), entry.second);
	EXPECT_EQ(launcher_verify(f.handle, nullptr, nullptr, &f.error),
		LAUNCHER_OK);
}
