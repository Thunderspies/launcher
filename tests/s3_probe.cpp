#include <launcher/launcher.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <thread>

struct progress_state {
	std::thread::id caller;
	bool wrong_thread;
	bool cancelled;
};

static int cancel_download(const launcher_progress *progress, void *opaque)
{
	progress_state *state = (progress_state *)opaque;

	if (std::this_thread::get_id() != state->caller)
		state->wrong_thread = true;
	if (progress->phase == LAUNCHER_DOWNLOAD && progress->completed) {
		state->cancelled = true;
		return 0;
	}
	return 1;
}

static launcher_status credentials_case(launcher_handle *handle,
	const char *mode, const char *url,
	const launcher_s3_credentials *credentials, launcher_error *error)
{
	launcher_config config = {};
	launcher_status status = launcher_get_config(handle, &config, error);

	if (status != LAUNCHER_OK)
		return status;
	launcher_file_repository file = config.file_repositories[0];
	if (!strcmp(mode, "file-clear-endpoint") ||
		!strcmp(mode, "file-clear-region")) {
		std::string changed = std::string(file.base_url) + "another/";

		if (!strcmp(mode, "file-clear-region"))
			file.s3_region = "us-west-2";
		else
			file.base_url = changed.c_str();
		config.file_repositories = &file;
		status = launcher_configure(handle, &config, error);
	} else if (!strcmp(mode, "file-clear-credentials")) {
		status = launcher_set_file_s3_credentials(handle, 0,
			NULL, error);
	} else if (!strcmp(mode, "file-rotate-plan")) {
		launcher_release *release = NULL;
		launcher_plan *plan = NULL;
		launcher_s3_credentials wrong = {"wrong", "wrong", NULL};

		status = launcher_resolve(handle, &release, NULL, NULL, error);
		if (status == LAUNCHER_OK)
			status = launcher_make_plan(handle, release, &plan,
				NULL, NULL, error);
		if (status == LAUNCHER_OK)
			status = launcher_set_file_s3_credentials(handle, 0,
				&wrong, error);
		if (status == LAUNCHER_OK)
			status = launcher_set_file_s3_credentials(handle, 0,
				credentials, error);
		if (status == LAUNCHER_OK)
			status = launcher_apply(handle, plan, NULL, NULL,
				error);
		launcher_plan_free(&plan);
		launcher_release_free(&release);
		return status;
	} else if (!strcmp(mode, "retain-channel")) {
		config.channel = "next";
		status = launcher_configure(handle, &config, error);
		if (status == LAUNCHER_OK) {
			status = launcher_get_config(handle, &config, error);
			config.channel = "live";
		}
		if (status == LAUNCHER_OK)
			status = launcher_configure(handle, &config, error);
	} else if (!strcmp(mode, "clear-region")) {
		config.s3_region = "us-east-1";
		status = launcher_configure(handle, &config, error);
	} else if (!strcmp(mode, "clear-endpoint")) {
		std::string changed = std::string(url) + "another/";

		config.base_url = changed.c_str();
		status = launcher_configure(handle, &config, error);
	} else if (!strcmp(mode, "clear-credentials")) {
		status = launcher_set_s3_credentials(handle, NULL, error);
	} else if (!strcmp(mode, "failed-configure")) {
		config.s3_region = "INVALID";
		status = launcher_configure(handle, &config, error);
		if (status != LAUNCHER_INVALID)
			return LAUNCHER_CONFLICT;
		status = LAUNCHER_OK;
	} else if (!strcmp(mode, "failed-credentials")) {
		launcher_s3_credentials incomplete = {"incomplete", NULL, NULL};

		status = launcher_set_s3_credentials(handle, &incomplete,
			error);
		if (status != LAUNCHER_INVALID)
			return LAUNCHER_CONFLICT;
		status = LAUNCHER_OK;
	} else if (!strcmp(mode, "rotate-plan")) {
		launcher_release *release = NULL;
		launcher_plan *plan = NULL;
		launcher_s3_credentials wrong = {"wrong", "wrong", NULL};

		status = launcher_resolve(handle, &release, NULL, NULL, error);
		if (status == LAUNCHER_OK)
			status = launcher_make_plan(handle, release, &plan,
				NULL, NULL, error);
		if (status == LAUNCHER_OK)
			status = launcher_set_s3_credentials(handle, &wrong,
				error);
		if (status == LAUNCHER_OK)
			status = launcher_set_s3_credentials(handle,
				credentials, error);
		if (status == LAUNCHER_OK)
			status = launcher_apply(handle, plan, NULL, NULL,
				error);
		launcher_plan_free(&plan);
		launcher_release_free(&release);
		return status;
	}
	launcher_release *release = NULL;

	if (status == LAUNCHER_OK)
		status = launcher_resolve(handle, &release, NULL, NULL, error);
	launcher_release_free(&release);
	return status;
}

int main(int argc, char **argv)
{
	if (argc != 5)
		return 2;
	launcher_handle *handle = NULL;
	launcher_release *release = NULL;
	launcher_error error = {};
	launcher_config config = {};
	launcher_s3_credentials credentials = {
		"fixture-access-key", "fixture-secret-key",
		"fixture-session-token"
	};
	launcher_status status = launcher_open(argv[2], &handle, &error);

	config.base_url = argv[3];
	config.ca_bundle = argv[4];
	config.s3_region = "auto";
	launcher_file_repository files{argv[3], "auto"};
	config.file_repositories = &files;
	config.file_repository_count = 1;
	if (status == LAUNCHER_OK)
		status = launcher_configure(handle, &config, &error);
	if (status == LAUNCHER_OK)
		status = launcher_set_s3_credentials(handle, &credentials,
			&error);
	if (status == LAUNCHER_OK)
		status = launcher_set_file_s3_credentials(handle, 0,
			&credentials, &error);
	progress_state progress = {std::this_thread::get_id(), false, false};
	launcher_status expected = LAUNCHER_OK;
	bool cancel = !strcmp(argv[1], "cancel");

	if (cancel)
		expected = LAUNCHER_CANCELLED;
	else if (!strcmp(argv[1], "metadata-excess"))
		expected = LAUNCHER_METADATA;
	else if (!strncmp(argv[1], "clear-", 6) ||
		!strncmp(argv[1], "file-clear-", 11))
		expected = LAUNCHER_INVALID;
	if (status == LAUNCHER_OK) {
		if (cancel)
			status = launcher_update(handle, cancel_download,
				&progress, &error);
		else if (!strncmp(argv[1], "metadata-", 9))
			status = launcher_resolve(handle, &release, NULL, NULL,
				&error);
		else
			status = credentials_case(handle, argv[1], argv[3],
				&credentials, &error);
	}
	launcher_release_free(&release);
	launcher_close(&handle);
	if (status != expected || progress.wrong_thread ||
		(cancel && !progress.cancelled)) {
		fprintf(stderr, "%s: expected %s, received %s: %s\n", argv[1],
			launcher_status_name(expected),
			launcher_status_name(status), error.message);
		return 1;
	}
	return 0;
}
