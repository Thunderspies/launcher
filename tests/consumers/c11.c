#include <launcher/launcher.h>
#include <string.h>

int main(void)
{
	const char json[] = "{\"format\":1,\"release\":\"r1\"}";
	char release[65] = {0};
	launcher_error error = {0};
	launcher_status result = launcher_decode_channel(json,
		sizeof(json) - 1, "live", release, &error);

	launcher_file file = {0};
	file.encoding = LAUNCHER_ZLIB;
	file.stored_size = 14;
	file.logical_md5 = NULL;
	file.fingerprint = NULL;
	if (file.encoding == LAUNCHER_RAW || file.stored_size != 14)
		return 1;
	launcher_file_repository repository = {0};
	launcher_descriptor_fn descriptor = NULL;
	(void)repository;
	const char empty[] = "{\"format\":2,\"release\":\"r1\","
		"\"groups\":{},\"loose\":[],\"entry_point\":null,"
		"\"run_arguments\":[]}";
	launcher_release *target = NULL;
	if (launcher_decode_release(empty, sizeof(empty) - 1, "r1",
		descriptor, NULL, &target, &error) != LAUNCHER_OK)
		return 1;
	launcher_release_free(&target);
	launcher_launch_info info = {0};
	launcher_s3_credentials credentials = {0};
	return result != LAUNCHER_OK || strcmp(release, "r1") ||
		credentials.session_token != NULL ||
		launcher_set_s3_credentials(NULL, NULL, &error) !=
			LAUNCHER_INVALID ||
		launcher_set_file_s3_credentials(NULL, 0, NULL,
			&error) != LAUNCHER_INVALID ||
		launcher_release_get_launch(NULL, &info, &error) !=
			LAUNCHER_INVALID || info.entry_point != NULL;
}
