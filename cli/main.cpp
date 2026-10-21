#include <launcher/launcher.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <signal.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#else
#include <time.h>
#include <unistd.h>
#endif

struct options {
	const char *command;
	const char *install;
	const char *repo;
	const char *channel;
	const char *game;
	const char *runner;
	const char *ca_bundle;
	const char *s3_region;
	const char **runner_args;
	size_t runner_count;
	const char **game_args;
	size_t game_count;
	const char **extra_args;
	size_t extra_count;
	launcher_file_repository *file_repositories;
	size_t file_repository_count;
	int replace_file_repositories;
	int settings;
	int progress;
};

static void usage(FILE *out)
{
	fputs("Usage: launcher COMMAND --install DIR [OPTIONS]\n"
		"Commands: configure, status, update, verify, repair, launch\n"
		"Progress: --progress auto|always|never (default: auto)\n"
		"Configure options:\n"
		"  --repo URL          Hosted manifest repository\n"
		"  --s3-region REGION  Sign S3 GETs (R2: auto; empty: off)\n"
		"  --file-repo URL     File endpoint (repeatable, ordered)\n"
		"  --file-s3-region R  Sign the preceding file endpoint\n"
		"  --clear-file-repos  Clear saved file endpoints\n"
		"  --channel NAME      Update channel (default: live)\n"
		"  --game PATH         Optional local game override\n"
		"  --runner PROGRAM    Optional runner, such as wine\n"
		"  --runner-arg ARG    Runner argument (repeatable)\n"
		"  --game-arg ARG      Saved game argument (repeatable)\n"
		"  --ca-bundle FILE    TLS CA certificate bundle\n"
		"Launch options: -- ARG... appends arguments "
		"for this launch.\n"
		"Signed update/repair credentials: AWS_ACCESS_KEY_ID,\n"
		"AWS_SECRET_ACCESS_KEY, optional AWS_SESSION_TOKEN.\n"
		"File N: LAUNCHER_FILE_N_AWS_ACCESS_KEY_ID,\n"
		"LAUNCHER_FILE_N_AWS_SECRET_ACCESS_KEY and optional\n"
		"LAUNCHER_FILE_N_AWS_SESSION_TOKEN (N starts at 1).\n",
		out);
}

static int parse(int argc, char **argv, options *out)
{
	for (int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if (!strcmp(arg, "--")) {
			for (++i; i < argc; ++i)
				out->extra_args[out->extra_count++] = argv[i];
			break;
		}
		if (strncmp(arg, "--", 2)) {
			if (out->command)
				return 0;
			out->command = arg;
			continue;
		}
		if (!strcmp(arg, "--clear-file-repos")) {
			out->settings = 1;
			out->replace_file_repositories = 1;
			out->file_repository_count = 0;
			continue;
		}
		if (++i == argc)
			return 0;
		const char *value = argv[i];
		if (!strcmp(arg, "--install")) {
			out->install = value;
			continue;
		}
		if (!strcmp(arg, "--progress")) {
			if (!strcmp(value, "auto"))
				out->progress = 0;
			else if (!strcmp(value, "always"))
				out->progress = 1;
			else if (!strcmp(value, "never"))
				out->progress = 2;
			else
				return 0;
			continue;
		}
		out->settings = 1;
		if (!strcmp(arg, "--repo"))
			out->repo = value;
		else if (!strcmp(arg, "--file-repo")) {
			out->replace_file_repositories = 1;
			out->file_repositories[out->file_repository_count++] =
				{value, NULL};
		} else if (!strcmp(arg, "--file-s3-region")) {
			if (!out->file_repository_count)
				return 0;
			out->file_repositories[out->file_repository_count - 1]
				.s3_region = value;
		} else if (!strcmp(arg, "--channel"))
			out->channel = value;
		else if (!strcmp(arg, "--game"))
			out->game = value;
		else if (!strcmp(arg, "--runner"))
			out->runner = value;
		else if (!strcmp(arg, "--ca-bundle"))
			out->ca_bundle = value;
		else if (!strcmp(arg, "--s3-region"))
			out->s3_region = value;
		else if (!strcmp(arg, "--runner-arg"))
			out->runner_args[out->runner_count++] = value;
		else if (!strcmp(arg, "--game-arg"))
			out->game_args[out->game_count++] = value;
		else
			return 0;
	}
	if (!out->command || !out->install || !*out->install)
		return 0;
	const char *command = out->command;
	if (strcmp(command, "configure") && strcmp(command, "status") &&
		strcmp(command, "update") && strcmp(command, "verify") &&
		strcmp(command, "repair") && strcmp(command, "launch"))
		return 0;
	if (out->settings && strcmp(command, "configure"))
		return 0;
	if (out->extra_count && strcmp(command, "launch"))
		return 0;
	return 1;
}

static launcher_status configure(launcher_handle *handle,
	const options *input, launcher_error *error)
{
	launcher_config config = {};
	launcher_status status = launcher_get_config(handle, &config, error);
	if (status != LAUNCHER_OK && !input->repo)
		return status;
	if (input->repo)
		config.base_url = input->repo;
	if (input->channel)
		config.channel = input->channel;
	if (input->game)
		config.game = *input->game ? input->game : NULL;
	if (input->runner)
		config.runner = *input->runner ? input->runner : NULL;
	if (input->ca_bundle)
		config.ca_bundle = *input->ca_bundle ? input->ca_bundle : NULL;
	if (input->s3_region)
		config.s3_region = *input->s3_region ? input->s3_region : NULL;
	if (input->replace_file_repositories) {
		config.file_repositories = input->file_repositories;
		config.file_repository_count = input->file_repository_count;
	}
	if (input->runner_count) {
		config.runner_args = input->runner_args;
		config.runner_arg_count = input->runner_count;
	}
	if (input->game_count) {
		config.game_args = input->game_args;
		config.game_arg_count = input->game_count;
	}
	return launcher_configure(handle, &config, error);
}

static launcher_status show_status(launcher_handle *handle,
	launcher_error *error)
{
	launcher_config config = {};
	launcher_status status = launcher_get_config(handle, &config, error);
	if (status != LAUNCHER_OK)
		return status;
	const launcher_release *release = NULL;
	status = launcher_get_installed(handle, &release, error);
	if (status != LAUNCHER_OK && status != LAUNCHER_NOT_INSTALLED)
		return status;
	launcher_launch_info launch = {};
	if (release)
		launcher_release_get_launch(release, &launch, NULL);
	const char *game = config.game && *config.game
		? config.game : launch.entry_point;
	printf("Repository: %s\nChannel: %s\nGame: %s\n",
		config.base_url ? config.base_url : "none",
		config.channel ? config.channel : "live", game ? game : "none");
	if (config.s3_region && *config.s3_region)
		printf("S3 region: %s\n", config.s3_region);
	for (size_t i = 0; i < config.file_repository_count; ++i) {
		const launcher_file_repository *repo =
			config.file_repositories + i;

		printf("File repository %zu: %s\n", i + 1, repo->base_url);
		if (repo->s3_region && *repo->s3_region)
			printf("File S3 region %zu: %s\n", i + 1,
				repo->s3_region);
	}
	if (status == LAUNCHER_NOT_INSTALLED) {
		puts("Installed: none");
		return LAUNCHER_OK;
	}
	if (status == LAUNCHER_OK)
		printf("Installed: %s\n", launcher_release_id(release));
	return status;
}

static launcher_status load_credentials(launcher_handle *handle,
	launcher_error *error)
{
	launcher_config config = {};
	launcher_status status = launcher_get_config(handle, &config, error);

	if (status != LAUNCHER_OK)
		return status;
	for (size_t endpoint = 0; endpoint <= config.file_repository_count;
		++endpoint) {
		const char *region = endpoint ?
			config.file_repositories[endpoint - 1].s3_region :
			config.s3_region;

		if (!region || !*region)
			continue;
		const char *suffix[] = {"AWS_ACCESS_KEY_ID",
			"AWS_SECRET_ACCESS_KEY", "AWS_SESSION_TOKEN"};
		const char *values[3] = {};

		for (size_t i = 0; i < 3; ++i) {
			char name[96];

			if (endpoint)
				snprintf(name, sizeof(name),
					"LAUNCHER_FILE_%zu_%s",
					endpoint, suffix[i]);
			else
				snprintf(name, sizeof(name), "%s", suffix[i]);
			values[i] = getenv(name);
		}
		if (!values[0] && !values[1] && !values[2])
			continue;
		launcher_s3_credentials credentials = {
			values[0], values[1], values[2]
		};

		status = endpoint ? launcher_set_file_s3_credentials(handle,
			endpoint - 1, &credentials, error) :
			launcher_set_s3_credentials(handle, &credentials,
				error);
		if (status != LAUNCHER_OK)
			return status;
	}
	return LAUNCHER_OK;
}

struct progress_output {
	int enabled;
	int terminal;
	int printed;
	int phase;
	int width;
	uint64_t last;
};

static volatile sig_atomic_t interrupted;

static void interrupt_command(int)
{
	interrupted = 1;
}

static uint64_t progress_time(void)
{
#ifdef _WIN32
	return GetTickCount64();
#else
	timespec now = {};

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
#endif
}

static int report_progress(const launcher_progress *progress, void *opaque)
{
	progress_output *output = (progress_output *)opaque;

	if (interrupted && progress->phase != LAUNCHER_ACTIVATE &&
		progress->phase != LAUNCHER_RUN)
		return 0;
	if (!output->enabled)
		return 1;
	uint64_t now = progress_time();

	if (output->printed && output->phase == (int)progress->phase &&
		now - output->last < 1000)
		return 1;
	const char *phases[] = {"Resolving", "Checking", "Downloading",
		"Staging", "Activating", "Running"};
	const char *label = progress->phase <= LAUNCHER_RUN ?
		phases[progress->phase] : "Working";

	if (output->terminal && output->printed)
		fputs("\r", stderr);
	int width = fprintf(stderr, "%s: %s (%" PRIu64 "/%" PRIu64 ")",
		label, progress->path ? progress->path : "",
		progress->completed, progress->total);
	if (progress->objects_total)
		width += fprintf(stderr, " [%zu/%zu objects]",
			progress->objects_completed,
			progress->objects_total);
	if (output->terminal) {
		for (int i = width; i >= 0 && i < output->width; ++i)
			fputc(' ', stderr);
	}
	output->width = width;
	if (!output->terminal)
		fputc('\n', stderr);
	fflush(stderr);
	output->printed = 1;
	output->phase = progress->phase;
	output->last = now;
	return 1;
}

static void progress_finish(progress_output *output)
{
	if (output->terminal && output->printed) {
		fputc('\n', stderr);
		fflush(stderr);
	}
	output->printed = 0;
}

static launcher_status execute(launcher_handle *handle,
	const options *input, progress_output *progress, int *exit_code,
	launcher_error *error)
{
	if (!strcmp(input->command, "configure"))
		return configure(handle, input, error);
	if (!strcmp(input->command, "status"))
		return show_status(handle, error);
	if (!strcmp(input->command, "update") ||
		!strcmp(input->command, "repair")) {
		launcher_status status = load_credentials(handle, error);

		if (status != LAUNCHER_OK)
			return status;
	}
	if (!strcmp(input->command, "update"))
		return launcher_update(handle, report_progress, progress, error);
	if (!strcmp(input->command, "verify"))
		return launcher_verify(handle, report_progress, progress, error);
	if (!strcmp(input->command, "repair"))
		return launcher_repair(handle, report_progress, progress, error);
	return launcher_launch(handle, input->extra_args, input->extra_count,
		exit_code, error);
}

static int run_launcher(int argc, char **argv)
{
	if (argc == 2 && !strcmp(argv[1], "--help")) {
		usage(stdout);
		return 0;
	}
	options input = {};
	input.runner_args = (const char **)calloc(argc, sizeof(char *));
	input.game_args = (const char **)calloc(argc, sizeof(char *));
	input.extra_args = (const char **)calloc(argc, sizeof(char *));
	input.file_repositories = (launcher_file_repository *)calloc(
		argc, sizeof(launcher_file_repository));
	int result = 1;
	launcher_handle *handle = NULL;
	launcher_error error = {};
	launcher_status status = LAUNCHER_MEMORY;
	int exit_code = -1;
	progress_output progress = {};
	void (*previous_interrupt)(int) = SIG_ERR;
	if (!input.runner_args || !input.game_args || !input.extra_args ||
		!input.file_repositories) {
		fputs("launcher: out of memory\n", stderr);
		goto done;
	}
	if (!parse(argc, argv, &input)) {
		usage(stderr);
		result = 2;
		goto done;
	}
#ifdef _WIN32
	progress.terminal = _isatty(_fileno(stderr));
#else
	progress.terminal = isatty(fileno(stderr));
#endif
	progress.enabled = input.progress == 1 ||
		(input.progress == 0 && progress.terminal);
	interrupted = 0;
	if (!strcmp(input.command, "update") ||
		!strcmp(input.command, "verify") ||
		!strcmp(input.command, "repair"))
		previous_interrupt = signal(SIGINT, interrupt_command);
	status = launcher_open(input.install, &handle, &error);
	if (status == LAUNCHER_OK)
		status = execute(handle, &input, &progress, &exit_code, &error);
	progress_finish(&progress);
	if (status != LAUNCHER_OK) {
		fprintf(stderr, "launcher: %s: %s\n",
			launcher_status_name(status), error.message);
		if (status == LAUNCHER_COMMITTED)
			result = 3;
		else if (status == LAUNCHER_CANCELLED && interrupted)
			result = 130;
		goto done;
	}
	if (error.message[0])
		fprintf(stderr, "launcher: warning: %s\n", error.message);
	result = 0;
	if (!strcmp(input.command, "launch"))
		result = exit_code >= 0 && exit_code <= 255 ? exit_code : 1;
	else if (strcmp(input.command, "status"))
		printf("%s completed\n", input.command);
done:
	launcher_close(&handle);
	if (previous_interrupt != SIG_ERR)
		signal(SIGINT, previous_interrupt);
	free(input.runner_args);
	free(input.game_args);
	free(input.extra_args);
	free(input.file_repositories);
	return result;
}

#ifdef _WIN32
static char *argument_utf8(const wchar_t *value)
{
	int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
		-1, NULL, 0, NULL, NULL);
	if (!size)
		return NULL;
	char *bytes = (char *)malloc((size_t)size);
	if (!bytes)
		return NULL;
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value,
		-1, bytes, size, NULL, NULL)) {
		free(bytes);
		return NULL;
	}
	return bytes;
}

int wmain(int argc, wchar_t **argv)
{
	char **arguments = (char **)calloc(
		(size_t)argc + 1, sizeof(char *));
	if (!arguments) {
		fputs("launcher: out of memory\n", stderr);
		return 1;
	}
	int result = 1;
	for (int i = 0; i < argc; ++i) {
		arguments[i] = argument_utf8(argv[i]);
		if (!arguments[i]) {
			fputs("launcher: cannot convert arguments to UTF-8\n",
				stderr);
			goto done;
		}
	}
	result = run_launcher(argc, arguments);
done:
	for (int i = 0; i < argc; ++i)
		free(arguments[i]);
	free(arguments);
	return result;
}
#else
int main(int argc, char **argv)
{
	return run_launcher(argc, argv);
}
#endif
