#include "internal.h"
#include <curl/curl.h>
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

static const size_t metadata_limit = 16U * 1024U * 1024U;

const char *launcher_status_name(launcher_status status)
{
	static const char *const names[] = {"ok", "invalid-argument",
		"invalid-metadata", "network-error", "integrity-error",
		"io-error", "conflict", "busy", "cancelled",
		"recovery-required", "not-installed", "committed",
		"out-of-memory", "unsupported"};
	return (unsigned)status < sizeof(names) / sizeof(names[0])
		? names[status]
		: "unknown";
}

static void config_free(launcher_config *config)
{
	for (size_t i = 0; i < config->file_repository_count; ++i) {
		free((void *)config->file_repositories[i].base_url);
		free((void *)config->file_repositories[i].s3_region);
	}
	free((void *)config->file_repositories);
	free((void *)config->base_url);
	free((void *)config->channel);
	free((void *)config->game);
	free((void *)config->runner);
	free((void *)config->ca_bundle);
	free((void *)config->s3_region);
	for (size_t i = 0; i < config->runner_arg_count; ++i)
		free((void *)config->runner_args[i]);
	for (size_t i = 0; i < config->game_arg_count; ++i)
		free((void *)config->game_args[i]);
	free((void *)config->runner_args);
	free((void *)config->game_args);
	*config = {};
}

static void credentials_free(launcher_s3_credentials *credentials)
{
	const char *strings[] = {credentials->access_key_id,
		credentials->secret_access_key, credentials->session_token};

	for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
		if (strings[i]) {
			volatile char *bytes = (volatile char *)strings[i];
			size_t length = strlen(strings[i]);

			while (length)
				bytes[--length] = 0;
			free((void *)strings[i]);
		}
	}
	*credentials = {};
}

static int signing_region_valid(const char *region)
{
	if (strlen(region) > 63)
		return 0;
	for (const char *p = region; *p; p++)
		if ((*p < 'a' || *p > 'z') && (*p < '0' || *p > '9') &&
			*p != '-')
			return 0;
	return 1;
}

static launcher_status signed_url_valid(const char *url,
	launcher_error *error)
{
	if (strncmp(url, "https://", 8))
		return lx_fail(error, LAUNCHER_INVALID,
			"Signed repositories require HTTPS");
	CURLU *parsed = curl_url();

	if (!parsed)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Validating signed repository URL");
	CURLUcode code = curl_url_set(parsed, CURLUPART_URL, url,
		CURLU_DISALLOW_USER | CURLU_PATH_AS_IS);

	curl_url_cleanup(parsed);
	return code == CURLUE_OK ? LAUNCHER_OK :
		lx_fail(error, LAUNCHER_INVALID,
			"Invalid signed repository URL");
}

static launcher_status copy_args(const char *const *args, size_t count,
	const char *const **out, launcher_error *error)
{
	*out = NULL;
	if (!count)
		return LAUNCHER_OK;
	if (!args || count > 4096)
		return lx_fail(
			error, LAUNCHER_INVALID, "Invalid argument array");
	char **copy = (char **)calloc(count, sizeof(char *));
	if (!copy)
		return lx_fail(error, LAUNCHER_MEMORY, "Cannot copy arguments");
	for (size_t i = 0; i < count; ++i) {
		if (!args[i] || !(copy[i] = lx_strdup(args[i]))) {
			launcher_status result =
				args[i] ? LAUNCHER_MEMORY : LAUNCHER_INVALID;
			for (size_t j = 0; j < i; ++j)
				free(copy[j]);
			free(copy);
			return lx_fail(error, result, "Cannot copy argument");
		}
	}
	*out = copy;
	return LAUNCHER_OK;
}

static launcher_status copy_endpoint(const char *url, const char *region,
	launcher_file_repository *out, launcher_error *error)
{
	if (!region)
		region = "";
	if (!url || (strncmp(url, "https://", 8) &&
		strncmp(url, "http://", 7)) || strlen(url) > 8192 ||
		strpbrk(url, "?#\r\n") || !signing_region_valid(region))
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid repository URL or signing region");
	if (*region) {
		launcher_status status = signed_url_valid(url, error);

		if (status != LAUNCHER_OK)
			return status;
	}
	size_t length = strlen(url);
	char *base = (char *)malloc(length + 2);
	char *copy_region = lx_strdup(region);

	if (!base || !copy_region) {
		free(base);
		free(copy_region);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Copying repository endpoint");
	}
	memcpy(base, url, length);
	if (base[length - 1] != '/')
		base[length++] = '/';
	base[length] = 0;
	out->base_url = base;
	out->s3_region = copy_region;
	return LAUNCHER_OK;
}

static launcher_status config_copy(const launcher_config *source,
	launcher_config *out, launcher_error *error)
{
	*out = {};
	const char *channel = source->channel ? source->channel : "live";
	if (!lx_identifier(channel) ||
		(source->game && *source->game &&
			!lx_path_valid(source->game, 0)) ||
		source->file_repository_count > 4096 ||
		(source->file_repository_count && !source->file_repositories))
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid channel, game path or file repository list");
	launcher_file_repository endpoint = {};
	launcher_status result = copy_endpoint(source->base_url,
		source->s3_region, &endpoint, error);

	if (result != LAUNCHER_OK)
		return result;
	out->base_url = endpoint.base_url;
	out->s3_region = endpoint.s3_region;
	if (source->file_repository_count) {
		launcher_file_repository *files = (launcher_file_repository *)
			calloc(source->file_repository_count, sizeof(*files));

		if (!files) {
			config_free(out);
			return lx_fail(error, LAUNCHER_MEMORY,
				"Copying file repository list");
		}
		out->file_repositories = files;
		out->file_repository_count = source->file_repository_count;
		for (size_t i = 0; i < source->file_repository_count; ++i) {
			const launcher_file_repository *file =
				source->file_repositories + i;

			result = copy_endpoint(file->base_url, file->s3_region,
				files + i, error);
			if (result != LAUNCHER_OK) {
				config_free(out);
				return result;
			}
		}
	}
	out->channel = lx_strdup(channel);
	out->game = lx_strdup(source->game ? source->game : "");
	out->runner = lx_strdup(source->runner ? source->runner : "");
	out->ca_bundle = lx_strdup(source->ca_bundle ? source->ca_bundle : "");
	if (!out->channel || !out->game || !out->runner || !out->ca_bundle ||
		!out->s3_region) {
		config_free(out);
		return lx_fail(error, LAUNCHER_MEMORY, "Cannot copy settings");
	}
	result = copy_args(source->runner_args,
		source->runner_arg_count, &out->runner_args, error);
	if (result == LAUNCHER_OK) {
		out->runner_arg_count = source->runner_arg_count;
		result = copy_args(source->game_args, source->game_arg_count,
			&out->game_args, error);
	}
	if (result == LAUNCHER_OK)
		out->game_arg_count = source->game_arg_count;
	else
		config_free(out);
	return result;
}

static int json_string(yyjson_mut_doc *doc, yyjson_mut_val *object,
	const char *key, const char *value)
{
	return yyjson_mut_obj_add_strcpy(doc, object, key, value ? value : "");
}

static int json_args(yyjson_mut_doc *doc, yyjson_mut_val *object,
	const char *key, const char *const *args, size_t count)
{
	yyjson_mut_val *array = yyjson_mut_arr(doc);
	if (!array || !yyjson_mut_obj_add_val(doc, object, key, array))
		return 0;
	for (size_t i = 0; i < count; ++i)
		if (!yyjson_mut_arr_add_strcpy(doc, array, args[i]))
			return 0;
	return 1;
}

static launcher_status save_changed_bytes(lx_fs *fs, const char *path,
	const void *bytes, size_t size, launcher_error *error)
{
	int kind = 0;
	uint64_t previous_size = 0;
	launcher_status status = lx_fs_stat(fs, path, &kind, &previous_size,
		error);

	if (status != LAUNCHER_OK)
		return status;
	if (kind == 1 && previous_size == size) {
		char *previous = NULL;
		size_t length = 0;

		status = lx_read_bytes(fs, path, size, &previous, &length,
			error);
		int same = status == LAUNCHER_OK && length == size &&
			!memcmp(previous, bytes, size);

		free(previous);
		if (status != LAUNCHER_OK || same)
			return status;
	}
	return lx_save_bytes(fs, path, bytes, size, error);
}

static launcher_status config_save(launcher_handle *handle,
	const launcher_config *config, launcher_error *error)
{
	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	if (!doc)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot encode settings");
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	int valid = root && yyjson_mut_obj_add_int(doc, root, "format", 3) &&
		json_string(doc, root, "base_url", config->base_url) &&
		json_string(doc, root, "channel", config->channel) &&
		json_string(doc, root, "game", config->game) &&
		json_string(doc, root, "runner", config->runner) &&
		json_string(doc, root, "ca_bundle", config->ca_bundle) &&
		json_args(doc, root, "runner_args", config->runner_args,
			config->runner_arg_count) &&
		json_args(doc, root, "game_args", config->game_args,
			config->game_arg_count);
	yyjson_mut_val *files = yyjson_mut_arr(doc);
	valid = valid && files &&
		json_string(doc, root, "s3_region", config->s3_region) &&
		yyjson_mut_obj_add_val(doc, root, "file_repositories", files);
	for (size_t i = 0; valid && i < config->file_repository_count; ++i) {
		yyjson_mut_val *endpoint = yyjson_mut_obj(doc);

		valid = endpoint && yyjson_mut_arr_add_val(files, endpoint) &&
			json_string(doc, endpoint, "base_url",
				config->file_repositories[i].base_url) &&
			json_string(doc, endpoint, "s3_region",
				config->file_repositories[i].s3_region);
	}
	size_t size = 0;
	char *bytes = valid ? yyjson_mut_write(doc, 0, &size) : NULL;
	launcher_status result = bytes && size <= metadata_limit
		? save_changed_bytes(handle->private_fs, "config.json", bytes,
			size, error)
		: lx_fail(error, LAUNCHER_INVALID,
			"Settings exceed limits or contain invalid UTF-8");
	free(bytes);
	yyjson_mut_doc_free(doc);
	return result;
}

static const char *get_string(yyjson_val *object, const char *key)
{
	yyjson_val *value = yyjson_obj_get(object, key);
	if (!yyjson_is_str(value))
		return NULL;
	const char *text = yyjson_get_str(value);
	return strlen(text) == yyjson_get_len(value) ? text : NULL;
}

static launcher_status parse_args(yyjson_val *object, const char *key,
	const char *const **out, size_t *count, launcher_error *error)
{
	yyjson_val *value = yyjson_obj_get(object, key);
	if (!yyjson_is_arr(value) || yyjson_arr_size(value) > 4096)
		return lx_fail(
			error, LAUNCHER_METADATA, "Invalid saved arguments");
	*count = yyjson_arr_size(value);
	const char **args =
		(const char **)calloc(*count ? *count : 1, sizeof(char *));
	if (!args)
		return lx_fail(error, LAUNCHER_MEMORY, "Cannot read arguments");
	for (size_t i = 0; i < *count; ++i) {
		yyjson_val *arg = yyjson_arr_get(value, i);
		if (!yyjson_is_str(arg) ||
			strlen(yyjson_get_str(arg)) != yyjson_get_len(arg)) {
			free(args);
			return lx_fail(error, LAUNCHER_METADATA,
				"Invalid saved argument");
		}
		args[i] = yyjson_get_str(arg);
	}
	*out = args;
	return LAUNCHER_OK;
}

static launcher_status config_load(
	launcher_handle *handle, launcher_error *error)
{
	int kind = 0;
	launcher_status result = lx_fs_stat(
		handle->private_fs, "config.json", &kind, NULL, error);
	if (result != LAUNCHER_OK || !kind)
		return result;
	char *bytes = NULL;
	size_t size = 0;
	result = lx_read_bytes(handle->private_fs, "config.json",
		metadata_limit, &bytes, &size, error);
	if (result != LAUNCHER_OK)
		return result;
	yyjson_doc *doc = yyjson_read(bytes, size, 0);
	free(bytes);
	yyjson_val *root = yyjson_doc_get_root(doc);
	launcher_config borrowed = {};
	yyjson_val *format = yyjson_obj_get(root, "format");
	uint64_t version = yyjson_get_uint(format);
	int valid = yyjson_is_obj(root) && yyjson_is_uint(format) &&
		((version == 1 && yyjson_obj_size(root) == 8) ||
		 (version == 2 && yyjson_obj_size(root) == 9) ||
		 (version == 3 && yyjson_obj_size(root) == 10));
	borrowed.base_url = get_string(root, "base_url");
	borrowed.channel = get_string(root, "channel");
	borrowed.game = get_string(root, "game");
	borrowed.runner = get_string(root, "runner");
	borrowed.ca_bundle = get_string(root, "ca_bundle");
	borrowed.s3_region = version == 1 ? "" : get_string(root, "s3_region");
	valid = valid && borrowed.base_url && borrowed.channel &&
		borrowed.game && borrowed.runner && borrowed.ca_bundle &&
		borrowed.s3_region;
	result = valid
		? parse_args(root, "runner_args", &borrowed.runner_args,
				&borrowed.runner_arg_count, error)
		: lx_fail(error, LAUNCHER_METADATA, "Invalid saved settings");
	if (result == LAUNCHER_OK)
		result = parse_args(root, "game_args", &borrowed.game_args,
			&borrowed.game_arg_count, error);
	launcher_file_repository *repositories = NULL;
	if (result == LAUNCHER_OK && version == 3) {
		yyjson_val *files = yyjson_obj_get(root, "file_repositories");

		if (!yyjson_is_arr(files) || yyjson_arr_size(files) > 4096)
			result = lx_fail(error, LAUNCHER_METADATA,
				"Invalid saved file repositories");
		else {
			borrowed.file_repository_count = yyjson_arr_size(files);
			repositories = (launcher_file_repository *)calloc(
				borrowed.file_repository_count + 1,
				sizeof(*repositories));
			if (!repositories)
				result = lx_fail(error, LAUNCHER_MEMORY,
					"Loading file repositories");
			borrowed.file_repositories = repositories;
			for (size_t i = 0; result == LAUNCHER_OK &&
				i < borrowed.file_repository_count; ++i) {
				yyjson_val *endpoint = yyjson_arr_get(files, i);

				repositories[i].base_url = get_string(endpoint,
					"base_url");
				repositories[i].s3_region = get_string(endpoint,
					"s3_region");
				if (!yyjson_is_obj(endpoint) ||
					yyjson_obj_size(endpoint) != 2 ||
					!repositories[i].base_url ||
					!repositories[i].s3_region)
					result = lx_fail(error,
						LAUNCHER_METADATA,
						"Invalid saved file endpoint");
			}
		}
	}
	if (result == LAUNCHER_OK)
		result = config_copy(&borrowed, &handle->config, error);
	free(repositories);
	if (result == LAUNCHER_OK && handle->config.file_repository_count) {
		handle->file_credentials = (launcher_s3_credentials *)calloc(
			handle->config.file_repository_count,
			sizeof(launcher_s3_credentials));
		if (!handle->file_credentials)
			result = lx_fail(error, LAUNCHER_MEMORY,
				"Loading runtime file credentials");
	}
	free((void *)borrowed.runner_args);
	free((void *)borrowed.game_args);
	yyjson_doc_free(doc);
	return result;
}

static int marker_matches(const char *bytes, size_t size,
	launcher_handle *handle, const char *identity)
{
	yyjson_doc *doc = yyjson_read(bytes, size, 0);
	yyjson_val *root = yyjson_doc_get_root(doc);
	const char *install = get_string(root, "install");
	const char *saved = get_string(root, "identity");
	int valid = yyjson_is_obj(root) && yyjson_obj_size(root) == 3 &&
		yyjson_is_uint(yyjson_obj_get(root, "format")) &&
		yyjson_get_uint(yyjson_obj_get(root, "format")) == 1 &&
		install && !strcmp(install, handle->root) && saved &&
		!strcmp(saved, identity);
	yyjson_doc_free(doc);
	return valid;
}

struct initializer_state {
	launcher_handle *handle;
	const char *identity;
};

static int initializer_artifact(const char *name, int kind, void *opaque)
{
	initializer_state *state = (initializer_state *)opaque;
	if (kind != 1 ||
		(strcmp(name, "lock") && strcmp(name, "marker.json.tmp")))
		return 0;
	uint64_t size = 0;
	if (lx_fs_stat(state->handle->private_fs, name, &kind, &size, NULL) !=
		LAUNCHER_OK)
		return 0;
	if (!strcmp(name, "lock"))
		return !size;
	if (!size)
		return 1;
	char *bytes = NULL;
	size_t length = 0;
	if (lx_read_bytes(state->handle->private_fs, name, metadata_limit,
			&bytes, &length, NULL) != LAUNCHER_OK)
		return 0;
	int valid =
		marker_matches(bytes, length, state->handle, state->identity);
	free(bytes);
	return valid;
}

static launcher_status validate_marker(
	launcher_handle *handle, int empty, launcher_error *error)
{
	char identity[128];
	launcher_status result =
		lx_fs_identity(handle->install, identity, error);
	if (result != LAUNCHER_OK)
		return result;
	int kind = 0;
	result = lx_fs_stat(
		handle->private_fs, "marker.json", &kind, NULL, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!kind && !empty) {
		/* Only empty locks and a matching complete (or empty) marker
			* temporary can originate from interrupted
			* initialization.
			* Other unmarked contents remain outside launcher
			* ownership.
			*/
		initializer_state state = {handle, identity};
		result = lx_fs_list(handle->private_fs, "",
			initializer_artifact, &state, error);
		if (result != LAUNCHER_OK)
			return lx_fail(error, LAUNCHER_CONFLICT,
				"Existing sidecar is not owned by this "
				"launcher");
	}
	if (!kind) {
		yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
		if (!doc)
			return lx_fail(
				error, LAUNCHER_MEMORY, "Cannot save owner");
		yyjson_mut_val *root = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, root);
		int valid = root &&
			yyjson_mut_obj_add_int(doc, root, "format", 1) &&
			json_string(doc, root, "install", handle->root) &&
			json_string(doc, root, "identity", identity);
		size_t size = 0;
		char *bytes = valid ? yyjson_mut_write(doc, 0, &size) : NULL;
		result = bytes
			? lx_save_bytes(handle->private_fs, "marker.json",
				bytes, size, error)
			: lx_fail(error, LAUNCHER_MEMORY,
				"Cannot encode sidecar ownership");
		free(bytes);
		yyjson_mut_doc_free(doc);
		return result;
	}
	char *bytes = NULL;
	size_t size = 0;
	result = lx_read_bytes(handle->private_fs, "marker.json",
		metadata_limit, &bytes, &size, error);
	if (result != LAUNCHER_OK)
		return result;
	int valid = marker_matches(bytes, size, handle, identity);
	free(bytes);
	return valid ? LAUNCHER_OK
			: lx_fail(error, LAUNCHER_CONFLICT,
				"Sidecar belongs to another installation or is "
				"corrupt");
}

launcher_status launcher_open(
	const char *install, launcher_handle **out, launcher_error *error)
{
	lx_clear(error);
	if (!out || *out || !install || !*install)
		return lx_fail(
			error, LAUNCHER_INVALID, "Invalid open arguments");
	launcher_handle *handle = (launcher_handle *)calloc(1, sizeof(*handle));
	if (!handle)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot allocate session");
	launcher_status result =
		lx_fs_open(install, 1, &handle->install, error);
	if (result == LAUNCHER_OK) {
		handle->root = lx_strdup(lx_fs_path(handle->install));
		size_t length = strlen(lx_fs_path(handle->install));
		handle->private_root = (char *)malloc(length + 10);
		if (!handle->root || !handle->private_root)
			result = lx_fail(error, LAUNCHER_MEMORY,
				"Cannot allocate installation paths");
		else
			snprintf(handle->private_root, length + 10,
				"%s.launcher", handle->root);
	}
	if (result == LAUNCHER_OK)
		result = lx_fs_open(
			handle->private_root, 1, &handle->private_fs, error);
	int empty = 0;
	if (result == LAUNCHER_OK)
		result = lx_fs_empty(handle->private_fs, "", &empty, error);
	if (result == LAUNCHER_OK)
		result = lx_fs_same_volume(
			handle->install, handle->private_fs, error);
	if (result == LAUNCHER_OK)
		result = lx_fs_lock(handle->private_fs, error);
	if (result == LAUNCHER_OK)
		result = validate_marker(handle, empty, error);
	if (result == LAUNCHER_OK)
		result = lx_process_busy(handle->private_fs, error);
	if (result == LAUNCHER_OK)
		result = lx_state_load(handle, error);
	if (result == LAUNCHER_OK)
		result = lx_transaction_recover(handle, error);
	if (result == LAUNCHER_OK)
		result = config_load(handle, error);
	if (result == LAUNCHER_OK)
		result = lx_fs_mkdirs(handle->private_fs, "objects", error);
	if (result != LAUNCHER_OK) {
		launcher_close(&handle);
		return result;
	}
	*out = handle;
	return LAUNCHER_OK;
}

static void descriptor_cache_free(lx_descriptor_cache **cache)
{
	while (*cache) {
		lx_descriptor_cache *entry = *cache;

		*cache = entry->next;
		free(entry->descriptor.json);
		free(entry);
	}
}

static void descriptor_cache_clear(launcher_handle *handle)
{
	for (size_t i = 0; i < 4096; ++i)
		descriptor_cache_free(handle->descriptors + i);
}

static void file_credentials_free(launcher_handle *handle)
{
	if (handle->file_credentials)
		for (size_t i = 0;
			i < handle->config.file_repository_count; ++i)
			credentials_free(handle->file_credentials + i);
	free(handle->file_credentials);
	handle->file_credentials = NULL;
}

void launcher_close(launcher_handle **handle)
{
	if (!handle || !*handle)
		return;
	launcher_handle *owned = *handle;
	*handle = NULL;
	file_credentials_free(owned);
	descriptor_cache_clear(owned);
	config_free(&owned->config);
	credentials_free(&owned->s3_credentials);
	launcher_release_free(&owned->installed);
	for (size_t i = 0; i < owned->owned_dir_count; ++i)
		free(owned->owned_dirs[i]);
	free(owned->owned_dirs);
	lx_fs_close(&owned->private_fs);
	lx_fs_close(&owned->install);
	free(owned->root);
	free(owned->private_root);
	free(owned);
}

static launcher_status usable(launcher_handle *handle, launcher_error *error)
{
	if (!handle)
		return lx_fail(error, LAUNCHER_INVALID, "NULL installation");
	if (handle->blocked)
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Close and reopen the installation to retry recovery");
	return LAUNCHER_OK;
}

launcher_status launcher_configure(launcher_handle *handle,
	const launcher_config *config, launcher_error *error)
{
	lx_clear(error);
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!config)
		return lx_fail(error, LAUNCHER_INVALID, "NULL configuration");
	launcher_config copy = {};
	result = config_copy(config, &copy, error);
	launcher_s3_credentials *file_credentials = NULL;
	if (result == LAUNCHER_OK && copy.file_repository_count) {
		file_credentials = (launcher_s3_credentials *)calloc(
			copy.file_repository_count, sizeof(*file_credentials));
		if (!file_credentials)
			result = lx_fail(error, LAUNCHER_MEMORY,
				"Allocating runtime file credentials");
	}
	if (result == LAUNCHER_OK)
		result = config_save(handle, &copy, error);
	if (result == LAUNCHER_OK) {
		if (!handle->config.base_url ||
			strcmp(handle->config.base_url, copy.base_url) ||
			strcmp(handle->config.s3_region, copy.s3_region))
			credentials_free(&handle->s3_credentials);
		for (size_t i = 0; i < copy.file_repository_count &&
			i < handle->config.file_repository_count; ++i) {
			const launcher_file_repository *old =
				handle->config.file_repositories + i;
			const launcher_file_repository *next =
				copy.file_repositories + i;

			if (!strcmp(old->base_url, next->base_url) &&
				!strcmp(old->s3_region, next->s3_region)) {
				file_credentials[i] =
					handle->file_credentials[i];
				handle->file_credentials[i] = {};
			}
		}
		file_credentials_free(handle);
		handle->file_credentials = file_credentials;
		descriptor_cache_clear(handle);
		config_free(&handle->config);
		handle->config = copy;
		++handle->epoch;
	} else {
		free(file_credentials);
		config_free(&copy);
	}
	return result;
}

static launcher_status set_credentials(launcher_s3_credentials *target,
	const char *region, const launcher_s3_credentials *credentials,
	launcher_error *error)
{
	if (!credentials) {
		credentials_free(target);
		return LAUNCHER_OK;
	}
	if (!region || !*region)
		return lx_fail(error, LAUNCHER_INVALID,
			"Configure a signed repository before "
			"supplying credentials");
	const char *values[] = {credentials->access_key_id,
		credentials->secret_access_key, credentials->session_token};

	if (!values[0] || !*values[0] || !values[1] || !*values[1])
		return lx_fail(error, LAUNCHER_INVALID,
			"S3 credentials require both access and secret keys");
	for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++)
		for (const unsigned char *p =
			(const unsigned char *)(values[i] ? values[i] : "");
			*p; p++)
			if (*p < 32 || *p == 127)
				return lx_fail(error, LAUNCHER_INVALID,
					"S3 credentials contain control bytes");
	launcher_s3_credentials copy = {};

	copy.access_key_id = lx_strdup(values[0]);
	copy.secret_access_key = lx_strdup(values[1]);
	copy.session_token = lx_strdup(values[2] ? values[2] : "");
	if (!copy.access_key_id || !copy.secret_access_key ||
		!copy.session_token) {
		credentials_free(&copy);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Copying runtime credentials");
	}
	credentials_free(target);
	*target = copy;
	return LAUNCHER_OK;
}

launcher_status launcher_set_s3_credentials(launcher_handle *handle,
	const launcher_s3_credentials *credentials, launcher_error *error)
{
	lx_clear(error);
	launcher_status status = usable(handle, error);

	return status == LAUNCHER_OK ? set_credentials(&handle->s3_credentials,
		handle->config.s3_region, credentials, error) : status;
}

launcher_status launcher_set_file_s3_credentials(launcher_handle *handle,
	size_t index, const launcher_s3_credentials *credentials,
	launcher_error *error)
{
	lx_clear(error);
	launcher_status status = usable(handle, error);

	if (status != LAUNCHER_OK)
		return status;
	if (index >= handle->config.file_repository_count)
		return lx_fail(error, LAUNCHER_INVALID,
			"File repository index is out of range");
	return set_credentials(handle->file_credentials + index,
		handle->config.file_repositories[index].s3_region,
		credentials, error);
}

launcher_status launcher_get_config(
	launcher_handle *handle, launcher_config *out, launcher_error *error)
{
	lx_clear(error);
	if (out)
		*out = {};
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!out)
		return lx_fail(error, LAUNCHER_INVALID, "NULL settings output");
	if (!handle->config.base_url)
		return lx_fail(
			error, LAUNCHER_INVALID, "Configure repository first");
	*out = handle->config;
	return LAUNCHER_OK;
}

launcher_status launcher_get_installed(launcher_handle *handle,
	const launcher_release **out, launcher_error *error)
{
	lx_clear(error);
	if (out)
		*out = NULL;
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!out)
		return lx_fail(
			error, LAUNCHER_INVALID, "NULL inventory output");
	if (!handle->installed)
		return lx_fail(error, LAUNCHER_NOT_INSTALLED,
			"No successfully installed release");
	*out = handle->installed;
	return LAUNCHER_OK;
}

// Keep one validated shard per provider within this descriptor operation.
struct provider_snapshot {
	int marker;
};

struct descriptor_request {
	launcher_handle *handle;
	launcher_progress_fn progress;
	void *state;
	lx_descriptor_cache *pending;
	int current_source;
	size_t first_repository;
	provider_snapshot *providers;
};

static size_t descriptor_bucket(const char *fingerprint)
{
	size_t bucket = 0;

	for (size_t i = 0; i < 3; ++i) {
		unsigned char value = (unsigned char)fingerprint[i];

		bucket = bucket * 16 + (value <= '9' ? value - '0' :
			value - 'a' + 10);
	}
	return bucket;
}

static lx_descriptor_cache *cached_descriptor(lx_descriptor_cache *cache,
	const char *fingerprint)
{
	for (; cache; cache = cache->next)
		if (!strcmp(cache->descriptor.fingerprint, fingerprint))
			return cache;
	return NULL;
}

static launcher_config file_config(launcher_handle *handle, size_t index)
{
	launcher_config config = {};

	config.base_url = handle->config.file_repositories[index].base_url;
	config.s3_region = handle->config.file_repositories[index].s3_region;
	config.ca_bundle = handle->config.ca_bundle;
	return config;
}

static int provider_fields(yyjson_val *root, const char *const *names,
	size_t count)
{
	if (!yyjson_is_obj(root) || yyjson_obj_size(root) != count)
		return 0;
	unsigned seen = 0;
	size_t index, maximum;
	yyjson_val *key, *value;

	yyjson_obj_foreach(root, index, maximum, key, value) {
		const char *name = yyjson_get_str(key);
		if (!name || strlen(name) != yyjson_get_len(key))
			return 0;
		size_t field = 0;

		while (field < count && strcmp(name, names[field]))
			++field;
		if (field == count || (seen & (1U << field)))
			return 0;
		seen |= 1U << field;
	}
	return seen == (1U << count) - 1;
}

static int provider_string(yyjson_val *root, const char *key,
	const char *expected)
{
	yyjson_val *value = yyjson_obj_get(root, key);
	const char *text = yyjson_get_str(value);

	return text && strlen(text) == yyjson_get_len(value) &&
		!strcmp(text, expected);
}

static launcher_status provider_json(launcher_handle *handle, size_t index,
	const char *path, yyjson_doc **out, launcher_progress_fn progress,
	void *state, launcher_error *error)
{
	launcher_config config = file_config(handle, index);
	char *bytes = NULL;
	size_t size = 0;
	launcher_status status = lx_http_bytes(&config,
		handle->file_credentials + index, path, metadata_limit, &bytes,
		&size, progress, state, error);

	*out = status == LAUNCHER_OK ? yyjson_read(bytes, size, 0) : NULL;
	free(bytes);
	if (status == LAUNCHER_OK && !*out)
		status = lx_fail(error, LAUNCHER_METADATA,
			"Invalid file repository JSON: %s", path);
	return status;
}

static void descriptor_request_clear(descriptor_request *request)
{
	descriptor_cache_free(&request->pending);
	free(request->providers);
	request->providers = NULL;
}

static launcher_status provider_descriptor(descriptor_request *request,
	size_t index, const char *fingerprint, char **bytes, size_t *size,
	launcher_error *error)
{
	launcher_handle *handle = request->handle;

	if (!request->providers) {
		request->providers = (provider_snapshot *)calloc(
			handle->config.file_repository_count,
			sizeof(*request->providers));
		if (!request->providers)
			return lx_fail(error, LAUNCHER_MEMORY,
				"Retaining file repository markers");
	}
	provider_snapshot *snapshot = request->providers + index;
	launcher_status status = LAUNCHER_OK;

	if (snapshot->marker < 0) {
		launcher_status absent = lx_fail(error, LAUNCHER_NETWORK,
			"File repository marker is absent");

		if (error)
			error->http_status = 404;
		return absent;
	}
	if (!snapshot->marker) {
		yyjson_doc *doc = NULL;

		status = provider_json(handle, index, "repository.json", &doc,
			request->progress, request->state, error);
		const char *fields[] = {"format", "kind"};
		yyjson_val *root = yyjson_doc_get_root(doc);
		yyjson_val *format = yyjson_obj_get(root, "format");

		if (status == LAUNCHER_OK && (!yyjson_is_uint(format) ||
			yyjson_get_uint(format) != 2 ||
			!provider_fields(root, fields, 2) ||
			!provider_string(root, "kind", "files")))
			status = lx_fail(error, LAUNCHER_METADATA,
				"Invalid file repository marker");
		yyjson_doc_free(doc);
		if (status != LAUNCHER_OK) {
			if (status == LAUNCHER_NETWORK && error &&
				error->http_status == 404 &&
				!error->native_code)
				snapshot->marker = -1;
			return status;
		}
		snapshot->marker = 1;
	}
	char path[96];
	launcher_config config = file_config(handle, index);

	snprintf(path, sizeof(path), "revisions/%s.json", fingerprint);
	status = lx_http_bytes(&config, handle->file_credentials + index,
		path, metadata_limit, bytes, size, request->progress,
		request->state, error);
	lx_entry metadata = {};

	if (status == LAUNCHER_OK)
		status = lx_decode_descriptor(*bytes, *size, fingerprint,
			&metadata, error);
	free(metadata.path);
	free(metadata.cached_header);
	if (status != LAUNCHER_OK) {
		free(*bytes);
		*bytes = NULL;
	}
	return status;
}

static launcher_status request_descriptor(const char *fingerprint,
	const void **data, size_t *size, void *opaque, launcher_error *error)
{
	descriptor_request *request = (descriptor_request *)opaque;
	launcher_handle *handle = request->handle;
	lx_descriptor_cache *cached = cached_descriptor(
		handle->descriptors[descriptor_bucket(fingerprint)],
		fingerprint);

	if (cached && !request->current_source) {
		*data = cached->descriptor.json;
		*size = cached->descriptor.size;
		return LAUNCHER_OK;
	}
	if (!request->current_source && handle->installed &&
		lx_release_descriptor(fingerprint, data, size, handle->installed,
			NULL) == LAUNCHER_OK)
		return LAUNCHER_OK;
	char path[80];

	snprintf(path, sizeof(path), "files/%s.json", fingerprint);
	if (!request->current_source) {
		int kind = 0;
		launcher_status status = lx_fs_stat(handle->private_fs, path,
			&kind, NULL, error);

		if (status != LAUNCHER_OK)
			return status;
		if (kind) {
			cached = (lx_descriptor_cache *)calloc(1,
				sizeof(*cached));
			if (!cached)
				return lx_fail(error, LAUNCHER_MEMORY,
					"Reading cached descriptor");
			status = lx_read_bytes(handle->private_fs, path,
				metadata_limit, &cached->descriptor.json,
				&cached->descriptor.size, error);
			if (status != LAUNCHER_OK) {
				free(cached);
				return status;
			}
			memcpy(cached->descriptor.fingerprint, fingerprint, 65);
			cached->repository = SIZE_MAX;
			cached->next = request->pending;
			request->pending = cached;
			*data = cached->descriptor.json;
			*size = cached->descriptor.size;
			return LAUNCHER_OK;
		}
	}
	launcher_error detail = {};
	launcher_status status = LAUNCHER_NETWORK;
	char *bytes = NULL;
	size_t length = 0;
	size_t index;

	for (index = request->first_repository;
		index < handle->config.file_repository_count; ++index) {
		status = provider_descriptor(request, index, fingerprint,
			&bytes, &length, &detail);
		if (status == LAUNCHER_OK)
			break;
		if (status != LAUNCHER_NETWORK || detail.http_status != 404 ||
			detail.native_code) {
			if (error)
				*error = detail;
			return status;
		}
	}
	if (index == handle->config.file_repository_count)
		return lx_fail(error, LAUNCHER_NETWORK,
			"No configured file repository contains descriptor %s",
			fingerprint);
	cached = (lx_descriptor_cache *)calloc(1, sizeof(*cached));
	if (!cached) {
		free(bytes);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Retaining descriptor source");
	}
	memcpy(cached->descriptor.fingerprint, fingerprint, 65);
	cached->descriptor.json = bytes;
	cached->descriptor.size = length;
	cached->repository = index;
	cached->next = request->pending;
	request->pending = cached;
	*data = bytes;
	*size = length;
	return LAUNCHER_OK;
}

static launcher_status accept_descriptors(descriptor_request *request,
	launcher_error *error)
{
	for (lx_descriptor_cache *entry = request->pending; entry;
		entry = entry->next) {
		if (entry->repository == SIZE_MAX)
			continue;
		launcher_status status = lx_fs_mkdirs(
			request->handle->private_fs, "files", error);
		char path[80];

		snprintf(path, sizeof(path), "files/%s.json",
			entry->descriptor.fingerprint);
		if (status == LAUNCHER_OK)
			status = save_changed_bytes(request->handle->private_fs,
				path, entry->descriptor.json,
				entry->descriptor.size, error);
		if (status != LAUNCHER_OK)
			return status;
	}
	while (request->pending) {
		lx_descriptor_cache *accepted = request->pending;

		request->pending = accepted->next;
		size_t bucket = descriptor_bucket(
			accepted->descriptor.fingerprint);
		lx_descriptor_cache **previous = request->handle->descriptors +
			bucket;

		while (*previous) {
			lx_descriptor_cache *entry = *previous;

			if (!strcmp(entry->descriptor.fingerprint,
				accepted->descriptor.fingerprint)) {
				*previous = entry->next;
				free(entry->descriptor.json);
				free(entry);
				break;
			}
			previous = &entry->next;
		}
		accepted->next = request->handle->descriptors[bucket];
		request->handle->descriptors[bucket] = accepted;
	}
	return LAUNCHER_OK;
}

static launcher_status object_source(launcher_handle *handle,
	lx_object *object, descriptor_request *request, launcher_error *error)
{
	lx_descriptor_cache *source = cached_descriptor(handle->descriptors[
		descriptor_bucket(object->fingerprint)], object->fingerprint);

	if (!source || source->repository == SIZE_MAX) {
		const void *data = NULL;
		size_t size = 0;
		lx_entry metadata = {};
		launcher_status status = request_descriptor(object->fingerprint,
			&data, &size, request, error);

		if (status == LAUNCHER_OK)
			status = lx_decode_descriptor(data, size,
				object->fingerprint, &metadata, error);
		free(metadata.path);
		free(metadata.cached_header);
		if (status == LAUNCHER_OK)
			status = accept_descriptors(request, error);
		if (status != LAUNCHER_OK)
			return status;
		source = cached_descriptor(handle->descriptors[
			descriptor_bucket(object->fingerprint)],
			object->fingerprint);
	}
	lx_entry metadata = {};
	launcher_status status = lx_decode_descriptor(source->descriptor.json,
		source->descriptor.size, object->fingerprint, &metadata, error);

	if (status != LAUNCHER_OK)
		return status;
	const char *kind = metadata.kind == LAUNCHER_HOGG ? "hogg" : "loose";

	if (!strcmp(object->digest, metadata.digest))
		snprintf(object->remote, sizeof(object->remote), "%s/%s/BLOB",
			kind, metadata.digest);
	else if (!strcmp(object->digest, metadata.cached_header_sha256))
		snprintf(object->remote, sizeof(object->remote),
			"%s/%s/headers/%s", kind, metadata.digest, object->digest);
	else
		status = lx_fail(error, LAUNCHER_METADATA,
			"Object is not part of its named revision");
	free(metadata.path);
	free(metadata.cached_header);
	object->repository = source->repository;
	return status;
}

launcher_status launcher_resolve(launcher_handle *handle,
	launcher_release **out, launcher_progress_fn progress, void *state,
	launcher_error *error)
{
	lx_clear(error);
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!out || *out || !handle->config.base_url)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid resolve output or missing repository "
			"settings");
	char path[96];
	snprintf(
		path, sizeof(path), "channels/%s.json", handle->config.channel);
	char *bytes = NULL;
	size_t size = 0;
	char release[65] = {};
	result = lx_http_bytes(&handle->config, &handle->s3_credentials,
		path, metadata_limit, &bytes,
		&size, progress, state, error);
	if (result == LAUNCHER_OK)
		result = launcher_decode_channel(
			bytes, size, handle->config.channel, release, error);
	char *channel_bytes = bytes;
	size_t channel_size = size;
	bytes = NULL;
	if (result == LAUNCHER_OK) {
		snprintf(path, sizeof(path), "releases/%s.json", release);
		result = lx_http_bytes(&handle->config, &handle->s3_credentials,
			path, metadata_limit,
			&bytes, &size, progress, state, error);
	}
	descriptor_request request = {handle, progress, state, NULL, 0, 0, NULL};
	if (result == LAUNCHER_OK)
		result = launcher_decode_release(bytes, size, release,
			request_descriptor, &request, out, error);
	if (result == LAUNCHER_OK)
		result = accept_descriptors(&request, error);
	descriptor_request_clear(&request);
	if (result == LAUNCHER_OK)
		result = lx_release_channel(*out, channel_bytes, channel_size,
			handle->config.channel, error);
	if (result != LAUNCHER_OK)
		launcher_release_free(out);
	free(channel_bytes);
	free(bytes);
	return result;
}

static int object_compare(const void *a, const void *b)
{
	const lx_object *left = (const lx_object *)a;
	const lx_object *right = (const lx_object *)b;
	int order = strcmp(left->digest, right->digest);

	return order ? order : strcmp(left->fingerprint, right->fingerprint);
}

static int destination_compare(const void *a, const void *b)
{
	return strcmp(((const lx_destination *)a)->path,
		((const lx_destination *)b)->path);
}

lx_destination *lx_destinations(const launcher_release *release,
	size_t *count)
{
	*count = lx_physical_count(release);
	lx_destination *items = (lx_destination *)calloc(*count + 1,
		sizeof(*items));

	if (!items || !release)
		return items;
	size_t at = 0;

	for (size_t i = 0; i < release->group_count; ++i) {
		if (release->groups[i].kind != LAUNCHER_HOGG)
			continue;
		items[at].path = release->groups[i].path;
		items[at++].group = i;
	}
	for (size_t i = 0; i < release->loose_count; ++i) {
		items[at].path = release->loose_files[i].path;
		items[at].entry = i;
		items[at++].group = SIZE_MAX;
	}
	return items;
}

static launcher_status retain_destinations(launcher_handle *handle,
	launcher_plan *plan, int force, launcher_progress_fn progress,
	void *state, launcher_error *error)
{
	plan->destinations = lx_destinations(plan->target,
		&plan->destination_count);
	if (!plan->destinations)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Allocating physical destinations");
	if (force || !handle->installed)
		return LAUNCHER_OK;
	size_t count = 0;
	lx_destination *previous = lx_destinations(handle->installed, &count);

	if (!previous)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Indexing installed destinations");
	qsort(previous, count, sizeof(*previous), destination_compare);
	launcher_status status = LAUNCHER_OK;

	for (size_t i = 0; i < plan->destination_count; ++i) {
		lx_destination *target = plan->destinations + i;
		size_t low = 0, high = count;

		while (low < high) {
			size_t mid = low + (high - low) / 2;

			if (strcmp(previous[mid].path, target->path) < 0)
				low = mid + 1;
			else
				high = mid;
		}
		if (low == count || strcmp(previous[low].path, target->path) ||
			(previous[low].group == SIZE_MAX) !=
			(target->group == SIZE_MAX))
			continue;
		int same = lx_same_destination(handle->installed,
			previous + low, plan->target, target);
		if (!same && target->group == SIZE_MAX)
			continue;
		status = lx_check_destination(handle, handle->installed,
			previous + low, NULL, progress, state, error);
		if (status == LAUNCHER_OK) {
			target->retained = same;
			target->incremental = !same;
			target->previous_group = previous[low].group;
		}
		else if (status == LAUNCHER_INTEGRITY || status == LAUNCHER_IO) {
			status = LAUNCHER_OK;
			lx_clear(error);
		} else
			break;
	}
	free(previous);
	return status;
}

static launcher_status make_plan(launcher_handle *handle,
	const launcher_release *target, int force, launcher_plan **out,
	launcher_progress_fn progress, void *state, launcher_error *error)
{
	lx_clear(error);
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!target || !out || *out)
		return lx_fail(
			error, LAUNCHER_INVALID, "Invalid planning arguments");
	launcher_plan *plan = (launcher_plan *)calloc(1, sizeof(*plan));
	if (!plan)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot allocate update plan");
	plan->owner = handle;
	plan->epoch = handle->epoch;
	result = lx_clone_release(target, &plan->target, error);
	plan->info.hogg_files = target->hogg_count;
	plan->info.loose_files = target->loose_count;
	if (result == LAUNCHER_OK)
		result = lx_transaction_preflight(handle, target, error);
	if (result == LAUNCHER_OK && !force &&
		lx_same_release(target, handle->installed) &&
		lx_same_channel(target, handle->installed)) {
		launcher_error detail = {};
		launcher_status checked = lx_check_release(handle, target,
			NULL, NULL, progress, state, &detail);

		if (checked == LAUNCHER_OK) {
			plan->info.unchanged = 1;
			*out = plan;
			return LAUNCHER_OK;
		}
		if (checked != LAUNCHER_INTEGRITY && checked != LAUNCHER_IO) {
			result = checked;
			if (error)
				*error = detail;
		}
	}
	descriptor_request request = {handle, progress, state, NULL, 1, 0, NULL};
	if (result == LAUNCHER_OK)
		result = retain_destinations(handle, plan, force, progress, state,
			error);
	size_t count = 0;

	for (size_t i = 0; result == LAUNCHER_OK &&
		i < plan->destination_count; ++i) {
		const lx_destination *destination = plan->destinations + i;

		if (destination->retained)
			continue;
		size_t entries = 0;
		const lx_entry *files = lx_destination_entries(plan->target,
			destination, &entries);

		count += entries;
		for (size_t j = 0; j < entries; ++j)
			count += !!files[j].cached_header_sha256[0];
	}
	if (result == LAUNCHER_OK) {
		plan->objects = (lx_object *)calloc(
			count ? count : 1, sizeof(lx_object));
		if (!plan->objects)
			result = lx_fail(error, LAUNCHER_MEMORY,
				"Cannot allocate distinct object set");
	}
	if (result == LAUNCHER_OK) {
		size_t at = 0;

		for (size_t i = 0; i < plan->destination_count; ++i) {
			const lx_destination *destination = plan->destinations + i;

			if (destination->retained)
				continue;
			size_t entries = 0;
			const lx_entry *files = lx_destination_entries(plan->target,
				destination, &entries);

			for (size_t j = 0; j < entries; ++j) {
				const lx_entry *entry = files + j;
				launcher_release before = {};
				const lx_entry *old = NULL;

				if (destination->incremental) {
					lx_group_view(handle->installed,
						destination->previous_group,
						&before);
					old = lx_find_entry(
						&before, entry->path);
				}
				if (old && !strcmp(old->fingerprint,
					entry->fingerprint))
					continue;
				lx_object *object;

				if (!lx_same_payload(old, entry)) {
					object = plan->objects + at++;
					memcpy(object->digest, entry->digest,
						65);
					object->size = entry->stored_size;
					memcpy(object->fingerprint,
						entry->fingerprint, 65);
				}
				if (!entry->cached_header_sha256[0])
					continue;
				object = plan->objects + at++;
				memcpy(object->digest, entry->cached_header_sha256, 65);
				object->size = entry->cached_header_size;
				memcpy(object->fingerprint, entry->fingerprint, 65);
			}
		}
		count = at;
		qsort(plan->objects, count, sizeof(lx_object), object_compare);
		for (size_t i = 0; i < count; ++i) {
			if (plan->object_count &&
				!strcmp(plan->objects[plan->object_count - 1]
						.digest,
					plan->objects[i].digest))
				continue;
			plan->objects[plan->object_count++] = plan->objects[i];
		}
	}
	if (result == LAUNCHER_OK)
		result = lx_cache_installed(
			handle, plan, progress, state, error);
	if (result == LAUNCHER_OK) {
		plan->info.hogg_files = target->hogg_count;
		plan->info.loose_files = target->loose_count;
		for (size_t i = 0; i < plan->object_count; ++i) {
			if (plan->objects[i].available)
				continue;
			result = object_source(handle, plan->objects + i, &request,
				error);
			if (result != LAUNCHER_OK)
				break;
			++plan->info.missing_objects;
			if (UINT64_MAX - plan->info.download_bytes <
				plan->objects[i].size) {
				result = lx_fail(error, LAUNCHER_METADATA,
					"Total download size exceeds uint64 "
					"range");
				break;
			}
			plan->info.download_bytes += plan->objects[i].size;
		}
	}
	descriptor_request_clear(&request);
	if (result != LAUNCHER_OK) {
		launcher_plan_free(&plan);
		return result;
	}
	plan->epoch = handle->epoch;
	*out = plan;
	return LAUNCHER_OK;
}

launcher_status launcher_make_plan(launcher_handle *handle,
	const launcher_release *target, launcher_plan **out,
	launcher_progress_fn progress, void *state, launcher_error *error)
{
	return make_plan(handle, target, 0, out, progress, state, error);
}

launcher_status launcher_plan_get_info(const launcher_plan *plan,
	launcher_plan_info *out, launcher_error *error)
{
	lx_clear(error);
	if (out)
		*out = {};
	if (!plan || !out)
		return lx_fail(
			error, LAUNCHER_INVALID, "Invalid plan info output");
	*out = plan->info;
	return LAUNCHER_OK;
}

void launcher_plan_free(launcher_plan **plan)
{
	if (!plan || !*plan)
		return;
	launcher_plan *owned = *plan;
	*plan = NULL;
	launcher_release_free(&owned->target);
	free(owned->objects);
	free(owned->destinations);
	free(owned);
}

launcher_status launcher_apply(launcher_handle *handle, launcher_plan *plan,
	launcher_progress_fn progress, void *state, launcher_error *error)
{
	lx_clear(error);
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!plan || plan->owner != handle || plan->epoch != handle->epoch)
		return lx_fail(error, LAUNCHER_INVALID,
			"Plan is stale or belongs to another installation");
	if (plan->info.unchanged) {
		result = lx_check_release(handle, plan->target, NULL, NULL,
			progress, state, error);
		if (result == LAUNCHER_OK)
			++handle->epoch;
		return result;
	}
	size_t done = 0;
	for (size_t i = 0; i < plan->object_count; ++i) {
		if (plan->objects[i].available)
			continue;
		lx_object *object = plan->objects + i;

		for (;;) {
			launcher_config config = file_config(handle,
				object->repository);

			launcher_error transfer_error = {};

			result = lx_http_object(&config,
				handle->file_credentials + object->repository,
				object->remote, object->digest, object->size,
				handle->private_fs, progress, state, done,
				plan->info.missing_objects, &transfer_error);
			if (error)
				*error = transfer_error;
			if (result == LAUNCHER_OK)
				break;
			if (result != LAUNCHER_NETWORK ||
				transfer_error.http_status != 404 ||
				transfer_error.native_code)
				return result;
			descriptor_request request = {handle, progress, state,
				NULL, 1, object->repository + 1, NULL};
			const void *bytes = NULL;
			size_t size = 0;

			result = request_descriptor(object->fingerprint, &bytes,
				&size, &request, error);
			if (result == LAUNCHER_OK)
				result = accept_descriptors(&request, error);
			if (result == LAUNCHER_OK)
				result = object_source(handle, object, &request, error);
			descriptor_request_clear(&request);
			if (result != LAUNCHER_OK)
				return result;
		}
		plan->objects[i].available = 1;
		++done;
	}
	return lx_transaction_apply(handle, plan, progress, state, error);
}

static launcher_status install_target(launcher_handle *handle,
	const launcher_release *target, launcher_progress_fn progress,
	void *state, launcher_error *error)
{
	launcher_plan *plan = NULL;
	launcher_status result = launcher_make_plan(
		handle, target, &plan, progress, state, error);
	if (result == LAUNCHER_OK)
		result = launcher_apply(handle, plan, progress, state, error);
	launcher_plan_free(&plan);
	return result;
}

launcher_status launcher_update(launcher_handle *handle,
	launcher_progress_fn progress, void *state, launcher_error *error)
{
	launcher_release *target = NULL;
	launcher_status result =
		launcher_resolve(handle, &target, progress, state, error);
	if (result == LAUNCHER_OK)
		result = install_target(handle, target, progress, state, error);
	launcher_release_free(&target);
	return result;
}

launcher_status launcher_repair(launcher_handle *handle,
	launcher_progress_fn progress, void *state, launcher_error *error)
{
	lx_clear(error);
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!handle->installed)
		return lx_fail(error, LAUNCHER_NOT_INSTALLED,
			"Repair requires a successfully installed manifest");
	launcher_plan *plan = NULL;

	result = make_plan(handle, handle->installed, 1, &plan, progress,
		state, error);
	if (result == LAUNCHER_OK)
		result = launcher_apply(handle, plan, progress, state, error);
	launcher_plan_free(&plan);
	return result;
}

launcher_status launcher_verify(launcher_handle *handle,
	launcher_progress_fn progress, void *state, launcher_error *error)
{
	lx_clear(error);
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!handle->installed)
		return lx_fail(error, LAUNCHER_NOT_INSTALLED,
			"Verify requires a successfully installed manifest");
	result = lx_verify_release(handle, handle->installed, NULL, NULL,
		progress, state, error);
	if (result != LAUNCHER_OK && result != LAUNCHER_INTEGRITY)
		return result;
	launcher_error cache_error = {};
	launcher_status cache = lx_verify_cache(handle, progress, state,
		&cache_error);

	if (cache == LAUNCHER_CANCELLED || result == LAUNCHER_OK) {
		if (error)
			*error = cache_error;
		return cache;
	}
	return result;
}

launcher_status launcher_launch(launcher_handle *handle,
	const char *const *extra_args, size_t extra_count, int *exit_code,
	launcher_error *error)
{
	lx_clear(error);
	if (exit_code)
		*exit_code = -1;
	launcher_status result = usable(handle, error);
	if (result != LAUNCHER_OK)
		return result;
	if (!exit_code || (extra_count && !extra_args) || extra_count > 4096)
		return lx_fail(
			error, LAUNCHER_INVALID, "Invalid launch arguments");
	if (!handle->installed)
		return lx_fail(error, LAUNCHER_NOT_INSTALLED,
			"Launch requires a successfully installed manifest");
	result = lx_fs_validate(handle->install, error);
	if (result == LAUNCHER_OK)
		result = lx_fs_validate(handle->private_fs, error);
	if (result != LAUNCHER_OK)
		return result;
	const char *game = handle->config.game;
	int published = !game || !*game;
	if (published)
		game = handle->installed->entry_point;
	int managed = 0;
	for (size_t i = 0; game && i < handle->installed->loose_count; ++i)
		if (!strcmp(handle->installed->loose_files[i].path, game))
			managed = 1;
	if (!managed)
		return lx_fail(error, LAUNCHER_INVALID,
			"Launch entry point must be an installed loose file");
	int kind = 0;

	result = lx_fs_stat(handle->install, game, &kind, NULL, error);
	if (result != LAUNCHER_OK)
		return result;
	if (kind != 1)
		return lx_fail(error, LAUNCHER_INTEGRITY,
			"Launch entry point is not an available regular file");
	launcher_config *config = &handle->config;
	int runner = config->runner && *config->runner;
	if (!runner) {
		result = lx_fs_executable(handle->install, game, error);
		if (result != LAUNCHER_OK)
			return result;
	}
	size_t published_count = published
		? handle->installed->run_argument_count : 0;
	size_t count = (runner ? 1 + config->runner_arg_count : 0) + 1 +
		published_count + config->game_arg_count + extra_count;
	const char **args = (const char **)calloc(count + 1, sizeof(char *));
	char *full = (char *)malloc(strlen(handle->root) + strlen(game) + 2);
	if (!args || !full) {
		free(args);
		free(full);
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot build launch argv");
	}
	sprintf(full, "%s/%s", handle->root, game);
	size_t used = 0;
	if (runner) {
		args[used++] = config->runner;
		for (size_t i = 0; i < config->runner_arg_count; ++i)
			args[used++] = config->runner_args[i];
	}
	args[used++] = full;
	for (size_t i = 0; i < published_count; ++i)
		args[used++] = handle->installed->run_arguments[i];
	for (size_t i = 0; i < config->game_arg_count; ++i)
		args[used++] = config->game_args[i];
	for (size_t i = 0; i < extra_count; ++i) {
		if (!extra_args[i]) {
			free(args);
			free(full);
			return lx_fail(
				error, LAUNCHER_INVALID, "NULL extra argument");
		}
		args[used++] = extra_args[i];
	}
	result = lx_process_run(
		handle->private_fs, handle->root, args, used, exit_code, error);
	free(args);
	free(full);
	return result;
}
