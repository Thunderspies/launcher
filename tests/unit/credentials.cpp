#include <gtest/gtest.h>
#include "../support.h"

using launcher_test::Fixture;
using launcher_test::read;
using launcher_test::write;

static launcher_config signed_config()
{
	launcher_config config{};
	config.base_url = "https://bucket.s3.us-east-1.amazonaws.com/game/";
	config.s3_region = "us-east-1";
	return config;
}

TEST(Credentials, SigningRegionRoundTripsAndRequiresHttps)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config = signed_config();
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error),
		LAUNCHER_OK) << f.error.message;
	std::string saved = read(f.base / "game.launcher/config.json");
	EXPECT_NE(saved.find("\"format\":3"), std::string::npos);
	EXPECT_NE(saved.find("\"s3_region\":\"us-east-1\""),
		std::string::npos);
	config.base_url = "http://localhost/game/";
	EXPECT_EQ(launcher_configure(f.handle, &config, &f.error),
		LAUNCHER_INVALID);
	config = signed_config();
	config.s3_region = "auto:s3";
	EXPECT_EQ(launcher_configure(f.handle, &config, &f.error),
		LAUNCHER_INVALID);
	config = signed_config();
	config.base_url = "https://user:password@example.com/game/";
	EXPECT_EQ(launcher_configure(f.handle, &config, &f.error),
		LAUNCHER_INVALID);
	EXPECT_EQ(saved, read(f.base / "game.launcher/config.json"));
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
	ASSERT_EQ(launcher_get_config(f.handle, &config, &f.error),
		LAUNCHER_OK);
	EXPECT_STREQ(config.s3_region, "us-east-1");
	EXPECT_STREQ(config.base_url,
		"https://bucket.s3.us-east-1.amazonaws.com/game/");
}

TEST(Credentials, UnsignedSettingsRemainCompatibleWithFormatOne)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(f.configure(), LAUNCHER_OK);
	std::string saved = read(f.base / "game.launcher/config.json");
	EXPECT_NE(saved.find("\"format\":3"), std::string::npos);
	EXPECT_NE(saved.find("s3_region"), std::string::npos);
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config{};
	ASSERT_EQ(launcher_get_config(f.handle, &config, &f.error),
		LAUNCHER_OK);
	EXPECT_STREQ(config.s3_region, "");
}

TEST(Credentials, RuntimeCredentialsAreValidatedAndNeverSaved)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_s3_credentials credentials{"test-access", "test-secret",
		"test-token"};
	EXPECT_EQ(launcher_set_s3_credentials(f.handle, &credentials,
		&f.error), LAUNCHER_INVALID);
	EXPECT_EQ(launcher_set_s3_credentials(f.handle, nullptr, &f.error),
		LAUNCHER_OK);
	launcher_config config = signed_config();
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error),
		LAUNCHER_OK);
	std::string saved = read(f.base / "game.launcher/config.json");
	ASSERT_EQ(launcher_set_s3_credentials(f.handle, &credentials,
		&f.error), LAUNCHER_OK) << f.error.message;
	credentials.secret_access_key = "";
	EXPECT_EQ(launcher_set_s3_credentials(f.handle, &credentials,
		&f.error), LAUNCHER_INVALID);
	credentials.secret_access_key = "test-secret";
	credentials.session_token = "token\r\nInjected: header";
	EXPECT_EQ(launcher_set_s3_credentials(f.handle, &credentials,
		&f.error), LAUNCHER_INVALID);
	EXPECT_EQ(saved, read(f.base / "game.launcher/config.json"));
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_release *release = nullptr;
	EXPECT_EQ(launcher_resolve(f.handle, &release, nullptr, nullptr,
		&f.error), LAUNCHER_INVALID);
	EXPECT_EQ(release, nullptr);
	EXPECT_EQ(f.error.http_status, 0);
	EXPECT_EQ(std::string(f.error.message).find("test-secret"),
		std::string::npos);
}

TEST(Credentials, RejectMalformedSavedSigningSettings)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config = signed_config();
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error),
		LAUNCHER_OK);
	launcher_close(&f.handle);
	std::string saved = read(f.base / "game.launcher/config.json");
	std::string malformed = saved;
	size_t pos = malformed.find("\"s3_region\":\"us-east-1\"");
	ASSERT_NE(pos, std::string::npos);
	malformed.replace(pos, 23, "\"unknown\":\"us-east-1\"");
	write(f.base / "game.launcher/config.json", malformed);
	EXPECT_EQ(f.open(), LAUNCHER_METADATA);
	EXPECT_EQ(f.handle, nullptr);
}

TEST(Credentials, OrderedFileSettingsAndIndependentRuntimeKeys)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_config config = signed_config();
	launcher_file_repository files[] = {
		{"http://public.example/files", nullptr},
		{"https://private.example/files", "auto"}
	};
	config.file_repositories = files;
	config.file_repository_count = 2;
	ASSERT_EQ(launcher_configure(f.handle, &config, &f.error), LAUNCHER_OK);
	launcher_s3_credentials keys{"private-access", "private-secret",
		nullptr};
	EXPECT_EQ(launcher_set_file_s3_credentials(f.handle, 0, &keys,
		&f.error), LAUNCHER_INVALID);
	EXPECT_EQ(launcher_set_file_s3_credentials(f.handle, 1, &keys,
		&f.error), LAUNCHER_OK);
	EXPECT_EQ(launcher_set_file_s3_credentials(f.handle, 2, nullptr,
		&f.error), LAUNCHER_INVALID);
	std::string saved = read(f.base / "game.launcher/config.json");
	EXPECT_EQ(saved.find("private-secret"), std::string::npos);
	launcher_close(&f.handle);
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	ASSERT_EQ(launcher_get_config(f.handle, &config, &f.error),
		LAUNCHER_OK);
	ASSERT_EQ(config.file_repository_count, 2u);
	EXPECT_STREQ(config.file_repositories[0].base_url,
		"http://public.example/files/");
	EXPECT_STREQ(config.file_repositories[0].s3_region, "");
	EXPECT_STREQ(config.file_repositories[1].s3_region, "auto");
	files[1].base_url = "http://invalid.example";
	config.file_repositories = files;
	EXPECT_EQ(launcher_configure(f.handle, &config, &f.error),
		LAUNCHER_INVALID);
	EXPECT_EQ(saved, read(f.base / "game.launcher/config.json"));
}

TEST(Credentials, OlderConfigurationLoadsWithoutFileEndpoints)
{
	Fixture f;
	ASSERT_EQ(f.open(), LAUNCHER_OK);
	launcher_close(&f.handle);
	for (int format : {1, 2}) {
		std::string settings = "{\"format\":" + std::to_string(format) +
			",\"base_url\":\"https://example.com/\","
			"\"channel\":\"live\","
			"\"game\":\"\",\"runner\":\"\",\"ca_bundle\":\"\","
			"\"runner_args\":[],\"game_args\":[]";
		if (format == 2)
			settings += ",\"s3_region\":\"auto\"";
		settings += "}";
		write(f.base / "game.launcher/config.json", settings);
		ASSERT_EQ(f.open(), LAUNCHER_OK) << f.error.message;
		launcher_config config{};
		ASSERT_EQ(launcher_get_config(f.handle, &config, &f.error),
			LAUNCHER_OK);
		EXPECT_EQ(config.file_repository_count, 0u);
		EXPECT_STREQ(config.s3_region, format == 2 ? "auto" : "");
		launcher_close(&f.handle);
	}
}
