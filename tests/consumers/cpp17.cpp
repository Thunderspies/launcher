#include <launcher/launcher.hpp>
#include <string.h>

int main()
{
	const char json[] = "{\"format\":1,\"release\":\"r1\"}";
	char release[65] = {};
	launcher::launcher_error error = {};
	launcher::launcher_status result = launcher::launcher_decode_channel(
		json, sizeof(json) - 1, "live", release, &error);

	launcher::launcher_file file = {};
	file.encoding = launcher::LAUNCHER_ZLIB;
	file.stored_size = 14;
	file.logical_md5 = nullptr;
	file.fingerprint = nullptr;
	if (file.encoding == launcher::LAUNCHER_RAW || file.stored_size != 14)
		return 1;
	launcher::launcher_file_repository repository = {0};
	launcher::launcher_descriptor_fn descriptor = nullptr;
	(void)repository;
	const char empty[] = "{\"format\":2,\"release\":\"r1\","
		"\"groups\":{},\"loose\":[],\"entry_point\":null,"
		"\"run_arguments\":[]}";
	launcher::launcher_release *target = nullptr;
	if (launcher::launcher_decode_release(empty, sizeof(empty) - 1, "r1",
		descriptor, nullptr, &target, &error) != LAUNCHER_OK)
		return 1;
	launcher::launcher_release_free(&target);
	launcher::launcher_launch_info info = {};
	launcher::launcher_s3_credentials credentials = {};
	return result != LAUNCHER_OK || strcmp(release, "r1") ||
		credentials.session_token != nullptr ||
		launcher::launcher_set_s3_credentials(nullptr, nullptr,
			&error) != LAUNCHER_INVALID ||
		launcher::launcher_set_file_s3_credentials(nullptr, 0, nullptr,
			&error) != LAUNCHER_INVALID ||
		launcher::launcher_release_get_launch(nullptr, &info, &error) !=
			LAUNCHER_INVALID || info.entry_point != nullptr;
}
