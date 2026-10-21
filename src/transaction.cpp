#include "internal.h"
#include <yyjson.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Freeze target, ownership and cached inputs before editing active files.
	* Recovery repeats the frozen difference while launch is blocked.
	*/
struct tx_file {
	char *path;
};

struct path_index {
	size_t *slots;
	size_t capacity;
};

struct tx_dir {
	char *path;
	int existed;
	int owned;
	int needed;
	int transition;
};

struct transaction {
	char id[65];
	launcher_release *target;
	tx_file *files;
	tx_file *writes;
	size_t write_count;
	tx_file *patches;
	size_t patch_count;
	tx_file *retained;
	size_t retained_count;
	size_t file_count;
	tx_dir *dirs;
	size_t dir_count;
	size_t file_capacity;
	size_t dir_capacity;
	path_index file_index;
	path_index dir_index;
	const char **old_files;
	size_t old_file_count;
	const char **old_dirs;
	size_t old_dir_count;
};

static const size_t private_limit = 256U * 1024U * 1024U;

static void transaction_free(transaction *tx)
{
	for (size_t i = 0; i < tx->file_count; ++i)
		free(tx->files[i].path);
	for (size_t i = 0; i < tx->dir_count; ++i)
		free(tx->dirs[i].path);
	for (size_t i = 0; i < tx->write_count; ++i)
		free(tx->writes[i].path);
	for (size_t i = 0; i < tx->retained_count; ++i)
		free(tx->retained[i].path);
	for (size_t i = 0; i < tx->patch_count; ++i)
		free(tx->patches[i].path);
	free(tx->patches);
	free(tx->retained);
	free(tx->writes);
	free(tx->files);
	free(tx->dirs);
	free(tx->file_index.slots);
	free(tx->dir_index.slots);
	free(tx->old_files);
	free(tx->old_dirs);
	launcher_release_free(&tx->target);
	*tx = {};
}

static const char *physical(const launcher_release *release, size_t index)
{
	return lx_physical_path(release, index);
}

static char *joined(const char *prefix, const char *path)
{
	size_t a = strlen(prefix), b = strlen(path);
	char *result = (char *)malloc(a + b + 1);
	if (!result)
		return NULL;
	memcpy(result, prefix, a);
	memcpy(result + a, path, b + 1);
	return result;
}

static char *staged(const char *path)
{
	return joined("transaction/new/", path);
}

static int file_compare(const void *a, const void *b)
{
	return strcmp(((const tx_file *)a)->path, ((const tx_file *)b)->path);
}

static int directory_compare(const void *a, const void *b)
{
	const char *left = ((const tx_dir *)a)->path;
	const char *right = ((const tx_dir *)b)->path;
	size_t x = strlen(left), y = strlen(right);
	if (x != y)
		return x < y ? -1 : 1;
	return strcmp(left, right);
}

static int path_compare(const char *a, const char *b)
{
#ifdef _WIN32
	for (; *a && *b; ++a, ++b) {
		unsigned char x = (unsigned char)*a;
		unsigned char y = (unsigned char)*b;
		if (x >= 'A' && x <= 'Z')
			x += 'a' - 'A';
		if (y >= 'A' && y <= 'Z')
			y += 'a' - 'A';
		if (x != y)
			return x < y ? -1 : 1;
	}
	return (unsigned char)*a - (unsigned char)*b;
#else
	return strcmp(a, b);
#endif
}

static int pointer_compare(const void *a, const void *b)
{
	return path_compare(*(const char *const *)a, *(const char *const *)b);
}

static size_t lower_path(
	const char *const *paths, size_t count, const char *path)
{
	size_t first = 0;
	while (first < count) {
		size_t middle = first + (count - first) / 2;
		if (path_compare(paths[middle], path) < 0)
			first = middle + 1;
		else
			count = middle;
	}
	return first;
}

static int was_owned(transaction *tx, const char *path)
{
	size_t index = lower_path(tx->old_files, tx->old_file_count, path);
	return index < tx->old_file_count &&
		!path_compare(tx->old_files[index], path);
}

static int owned_below(transaction *tx, const char *path)
{
	char *prefix = joined(path, "/");
	if (!prefix)
		return 0;
	size_t index = lower_path(tx->old_files, tx->old_file_count, prefix);
	size_t length = strlen(prefix);
	int found = 0;
	if (index < tx->old_file_count &&
		strlen(tx->old_files[index]) >= length) {
		char *part = lx_strdup(tx->old_files[index]);
		if (part) {
			part[length] = 0;
			found = !path_compare(part, prefix);
			free(part);
		}
	}
	free(prefix);
	return found;
}

static uint64_t path_hash(const char *path)
{
	uint64_t hash = UINT64_C(14695981039346656037);
	for (; *path; ++path) {
		unsigned char byte = (unsigned char)*path;
#ifdef _WIN32
		if (byte >= 'A' && byte <= 'Z')
			byte += 'a' - 'A';
#endif
		hash = (hash ^ byte) * UINT64_C(1099511628211);
	}
	return hash;
}

static const char *indexed_path(const void *items, size_t stride, size_t index)
{
	return *(char *const *)((const unsigned char *)items + stride * index);
}

static size_t index_find(
	path_index *index, const void *items, size_t stride, const char *path)
{
	if (!index->capacity)
		return SIZE_MAX;
	size_t slot = (size_t)path_hash(path) & (index->capacity - 1);
	while (index->slots[slot]) {
		size_t found = index->slots[slot] - 1;
		if (!path_compare(indexed_path(items, stride, found), path))
			return found;
		slot = (slot + 1) & (index->capacity - 1);
	}
	return SIZE_MAX;
}

static void index_insert(
	path_index *index, const void *items, size_t stride, size_t item)
{
	size_t slot = (size_t)path_hash(indexed_path(items, stride, item)) &
		(index->capacity - 1);
	while (index->slots[slot])
		slot = (slot + 1) & (index->capacity - 1);
	index->slots[slot] = item + 1;
}

static int index_reserve(
	path_index *index, const void *items, size_t stride, size_t count)
{
	if (index->capacity && (count + 1) * 4 < index->capacity * 3)
		return 1;
	size_t capacity = index->capacity ? index->capacity * 2 : 64;
	size_t *slots = (size_t *)calloc(capacity, sizeof(size_t));
	if (!slots)
		return 0;
	free(index->slots);
	index->slots = slots;
	index->capacity = capacity;
	for (size_t i = 0; i < count; ++i)
		index_insert(index, items, stride, i);
	return 1;
}

static launcher_status add_file(
	transaction *tx, const char *path, launcher_error *error)
{
	if (index_find(&tx->file_index, tx->files, sizeof(tx_file), path) !=
		SIZE_MAX)
		return LAUNCHER_OK;
	if (!index_reserve(&tx->file_index, tx->files, sizeof(tx_file),
			tx->file_count))
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot index originals");
	if (tx->file_count == tx->file_capacity) {
		size_t capacity =
			tx->file_capacity ? tx->file_capacity * 2 : 64;
		tx_file *files = (tx_file *)realloc(
			tx->files, capacity * sizeof(tx_file));
		if (!files)
			return lx_fail(error, LAUNCHER_MEMORY,
				"Cannot record originals");
		tx->files = files;
		tx->file_capacity = capacity;
	}
	tx->files[tx->file_count].path = lx_strdup(path);
	if (!tx->files[tx->file_count].path)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot record original");
	index_insert(
		&tx->file_index, tx->files, sizeof(tx_file), tx->file_count);
	++tx->file_count;
	return LAUNCHER_OK;
}

static launcher_status add_dir(launcher_handle *handle, transaction *tx,
	const char *path, int existed, int needed, launcher_error *error)
{
	size_t found =
		index_find(&tx->dir_index, tx->dirs, sizeof(tx_dir), path);
	if (found != SIZE_MAX) {
		tx->dirs[found].needed |= needed;
		return LAUNCHER_OK;
	}
	if (!index_reserve(
			&tx->dir_index, tx->dirs, sizeof(tx_dir),
			tx->dir_count))
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot index directories");
	if (tx->dir_count == tx->dir_capacity) {
		size_t capacity = tx->dir_capacity ? tx->dir_capacity * 2 : 64;
		tx_dir *dirs =
			(tx_dir *)realloc(tx->dirs, capacity * sizeof(tx_dir));
		if (!dirs)
			return lx_fail(error, LAUNCHER_MEMORY,
				"Cannot record directories");
		tx->dirs = dirs;
		tx->dir_capacity = capacity;
	}
	tx_dir *dir = &tx->dirs[tx->dir_count];
	*dir = {};
	if (existed && tx->old_files) {
		launcher_status spelling =
			lx_fs_spell(handle->install, path, &dir->path, error);
		if (spelling != LAUNCHER_OK)
			return spelling;
	} else {
		dir->path = lx_strdup(path);
	}
	if (!dir->path)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot record directory");
	dir->existed = existed;
	size_t owned = lower_path(tx->old_dirs, tx->old_dir_count, path);
	dir->owned = owned < tx->old_dir_count &&
		!path_compare(tx->old_dirs[owned], path);
	dir->needed = needed;
	index_insert(&tx->dir_index, tx->dirs, sizeof(tx_dir), tx->dir_count);
	++tx->dir_count;
	return LAUNCHER_OK;
}

struct inspect_state {
	launcher_handle *handle;
	transaction *tx;
	const char *parent;
	launcher_error *error;
	launcher_status result;
};

static launcher_status inspect_tree(launcher_handle *handle, transaction *tx,
	const char *path, launcher_error *error);

static int inspect_child(const char *name, int kind, void *opaque)
{
	inspect_state *state = (inspect_state *)opaque;
	char *prefix = joined(state->parent, "/");
	char *path = prefix ? joined(prefix, name) : NULL;
	free(prefix);
	if (!path) {
		state->result = lx_fail(state->error, LAUNCHER_MEMORY,
			"Cannot inspect directory transition");
		return 0;
	}
	if (kind == 2 && owned_below(state->tx, path))
		state->result = inspect_tree(
			state->handle, state->tx, path, state->error);
	else if (kind != 1 || !was_owned(state->tx, path))
		state->result = lx_fail(state->error, LAUNCHER_CONFLICT,
			"Transition would remove unrelated '%s'", path);
	free(path);
	return state->result == LAUNCHER_OK;
}

static launcher_status inspect_tree(launcher_handle *handle, transaction *tx,
	const char *path, launcher_error *error)
{
	launcher_status result = add_dir(handle, tx, path, 1, 0, error);
	if (result != LAUNCHER_OK)
		return result;
	size_t index =
		index_find(&tx->dir_index, tx->dirs, sizeof(tx_dir), path);
	tx->dirs[index].transition = 1;
	inspect_state state = {handle, tx, path, error, LAUNCHER_OK};
	result =
		lx_fs_list(handle->install, path, inspect_child, &state, error);
	return state.result == LAUNCHER_OK ? result : state.result;
}

static launcher_status inspect_path(launcher_handle *handle, transaction *tx,
	const char *path, int desired, launcher_error *error)
{
	char *copy = lx_strdup(path);
	if (!copy)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot inspect destination");
	launcher_status result = LAUNCHER_OK;
	int absent = 0;
	for (char *slash = strchr(copy, '/'); slash;
		slash = strchr(slash + 1, '/')) {
		*slash = 0;
		int kind = 0;
		if (!absent)
			result = lx_fs_stat(
				handle->install, copy, &kind, NULL, error);
		if (result == LAUNCHER_OK && kind == 1) {
			if (!was_owned(tx, copy))
				result = lx_fail(error, LAUNCHER_CONFLICT,
					"Unrelated file blocks directory '%s'",
					copy);
			else
				absent = 1;
		}
		if (result == LAUNCHER_OK)
			result = add_dir(
				handle, tx, copy, kind == 2, desired, error);
		*slash = '/';
		if (result != LAUNCHER_OK)
			break;
	}
	if (result == LAUNCHER_OK) {
		int kind = 0;
		if (!absent)
			result = lx_fs_stat(
				handle->install, path, &kind, NULL, error);
		if (result == LAUNCHER_OK && kind == 1) {
			char *spelling = NULL;
			result = lx_fs_spell(
				handle->install, path, &spelling, error);
			if (result == LAUNCHER_OK)
				result = add_file(tx, spelling, error);
			free(spelling);
		} else if (result == LAUNCHER_OK && kind == 2 && desired) {
			result = inspect_tree(handle, tx, path, error);
		}
	}
	free(copy);
	return result;
}

static launcher_status inventory(launcher_handle *handle,
	const launcher_release *target, transaction *tx, launcher_error *error)
{
	tx->old_file_count =
		lx_physical_count(handle->installed);
	tx->old_dir_count = handle->owned_dir_count;
	tx->old_files =
		(const char **)calloc(tx->old_file_count + 1, sizeof(char *));
	tx->old_dirs =
		(const char **)calloc(tx->old_dir_count + 1, sizeof(char *));
	if (!tx->old_files || !tx->old_dirs)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot index prior ownership");
	for (size_t i = 0; i < tx->old_file_count; ++i)
		tx->old_files[i] = physical(handle->installed, i);
	for (size_t i = 0; i < tx->old_dir_count; ++i)
		tx->old_dirs[i] = handle->owned_dirs[i];
	qsort(tx->old_files, tx->old_file_count, sizeof(char *),
		pointer_compare);
	qsort(tx->old_dirs, tx->old_dir_count, sizeof(char *), pointer_compare);
	launcher_status result = LAUNCHER_OK;
	if (handle->installed) {
		for (size_t i = 0; i < lx_physical_count(handle->installed); ++i) {
			result = inspect_path(handle, tx,
				physical(handle->installed, i), 0, error);
			if (result != LAUNCHER_OK)
				return result;
		}
	}
	for (size_t i = 0; i < lx_physical_count(target); ++i) {
		result =
			inspect_path(handle, tx, physical(target, i), 1, error);
		if (result != LAUNCHER_OK)
			return result;
	}
	if (tx->file_count > 1)
		qsort(tx->files, tx->file_count, sizeof(tx_file), file_compare);
	if (tx->dir_count > 1)
		qsort(tx->dirs, tx->dir_count, sizeof(tx_dir),
			directory_compare);
	return result;
}

launcher_status lx_transaction_preflight(launcher_handle *handle,
	const launcher_release *target, launcher_error *error)
{
	transaction tx = {};
	launcher_status result = inventory(handle, target, &tx, error);
	transaction_free(&tx);
	return result;
}

static const char *text_field(yyjson_val *value, const char *key)
{
	yyjson_val *field = yyjson_obj_get(value, key);
	if (!yyjson_is_str(field))
		return NULL;
	const char *text = yyjson_get_str(field);
	return strlen(text) == yyjson_get_len(field) ? text : NULL;
}

static int valid_private(yyjson_val *root, size_t fields, int version = 2)
{
	yyjson_val *format = yyjson_obj_get(root, "format");
	return yyjson_is_obj(root) && yyjson_obj_size(root) == fields &&
	       yyjson_is_uint(format) &&
	       yyjson_get_uint(format) == (unsigned)version;
}

static int descriptor_order(const void *a, const void *b)
{
	return strcmp(((const lx_descriptor *)a)->fingerprint,
		((const lx_descriptor *)b)->fingerprint);
}

static launcher_status read_descriptors(yyjson_val *object,
	launcher_release *saved, launcher_error *error)
{
	if (!yyjson_is_obj(object) || yyjson_obj_size(object) > 100000)
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Saved record has no bounded descriptor inventory");
	saved->descriptor_count = yyjson_obj_size(object);
	saved->descriptors = (lx_descriptor *)calloc(
		saved->descriptor_count + 1, sizeof(lx_descriptor));
	if (!saved->descriptors)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Reading saved descriptors");
	int ordered = 1;
	size_t index, count;
	yyjson_val *key, *value;

	yyjson_obj_foreach(object, index, count, key, value) {
		const char *fingerprint = yyjson_get_str(key);
		const char *bytes = yyjson_get_str(value);

		if (!fingerprint || yyjson_get_len(key) != 64 || !bytes ||
			strlen(bytes) != yyjson_get_len(value))
			return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Invalid saved descriptor entry");
		for (size_t i = 0; i < 64; ++i)
			if ((fingerprint[i] < '0' || fingerprint[i] > '9') &&
				(fingerprint[i] < 'a' || fingerprint[i] > 'f'))
				return lx_fail(error,
					LAUNCHER_RECOVERY_REQUIRED,
					"Invalid saved descriptor fingerprint");
		lx_descriptor *descriptor = saved->descriptors + index;

		memcpy(descriptor->fingerprint, fingerprint, 64);
		descriptor->json = (char *)bytes;
		descriptor->size = yyjson_get_len(value);
		if (index && strcmp(saved->descriptors[index - 1].fingerprint,
			fingerprint) > 0)
			ordered = 0;
	}
	if (!ordered)
		qsort(saved->descriptors, saved->descriptor_count,
			sizeof(lx_descriptor), descriptor_order);
	for (size_t i = 1; i < saved->descriptor_count; ++i)
		if (!strcmp(saved->descriptors[i - 1].fingerprint,
			saved->descriptors[i].fingerprint))
			return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Duplicate saved descriptor fingerprint");
	return LAUNCHER_OK;
}

static int save_descriptors(yyjson_mut_doc *doc, yyjson_mut_val *root,
	const launcher_release *release)
{
	yyjson_mut_val *files = yyjson_mut_obj(doc);

	if (!files || !yyjson_mut_obj_add_val(doc, root, "descriptors", files))
		return 0;
	for (size_t i = 0; i < release->descriptor_count; ++i) {
		const lx_descriptor *descriptor = release->descriptors + i;

		if (!yyjson_mut_obj_add_strcpy(doc, files,
			descriptor->fingerprint, descriptor->json))
			return 0;
	}
	return 1;
}

struct saved_descriptor_state {
	launcher_release release;
	size_t next;
};

static launcher_status saved_descriptor(const char *fingerprint,
	const void **data, size_t *size, void *opaque, launcher_error *error)
{
	saved_descriptor_state *state = (saved_descriptor_state *)opaque;

	if (state->next < state->release.descriptor_count) {
		lx_descriptor *record =
			state->release.descriptors + state->next;

		if (!strcmp(fingerprint, record->fingerprint)) {
			++state->next;
			*data = record->json;
			*size = record->size;
			return LAUNCHER_OK;
		}
	}
	return lx_release_descriptor(fingerprint, data, size,
		&state->release, error);
}

static launcher_status decode_saved(
	const char *manifest, yyjson_val *descriptors, int borrow,
	launcher_release **out, launcher_error *error)
{
	size_t length = strlen(manifest);
	if (length > 16U * 1024U * 1024U)
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Saved manifest exceeds public metadata limit");
	yyjson_doc *doc = yyjson_read(manifest, length, 0);
	const char *identity = text_field(yyjson_doc_get_root(doc), "release");
	char id[65] = {};
	if (lx_identifier(identity))
		strcpy(id, identity);
	yyjson_doc_free(doc);
	if (!*id)
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Saved manifest has no canonical release identity");
	saved_descriptor_state saved = {};
	launcher_status status = read_descriptors(descriptors,
		&saved.release, error);

	if (status == LAUNCHER_OK)
		status = (borrow ? lx_decode_saved_release :
			launcher_decode_release)(manifest, length, id,
				saved_descriptor, &saved, out, error);
	free(saved.release.descriptors);
	if (status == LAUNCHER_OK &&
		(*out)->descriptor_count != yyjson_obj_size(descriptors)) {
		launcher_release_free(out);
		status = lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Saved descriptor inventory contains extra records");
	}
	return status;
}

launcher_status lx_state_load(launcher_handle *handle, launcher_error *error)
{
	int kind = 0;
	launcher_status result = lx_fs_stat(
		handle->private_fs, "installed-state.json", &kind, NULL, error);
	if (result != LAUNCHER_OK || !kind)
		return result;
	char *bytes = NULL;
	size_t size = 0;
	result = lx_read_bytes(handle->private_fs, "installed-state.json",
		private_limit, &bytes, &size, error);
	if (result != LAUNCHER_OK)
		return result;
	char *padded = (char *)realloc(bytes, size + YYJSON_PADDING_SIZE);

	if (!padded) {
		free(bytes);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Padding saved-state input");
	}
	bytes = padded;
	memset(bytes + size, 0, YYJSON_PADDING_SIZE);
	yyjson_doc *doc = yyjson_read_opts(
		bytes, size, YYJSON_READ_INSITU, NULL, NULL);
	yyjson_val *root = yyjson_doc_get_root(doc);
	const char *manifest = text_field(root, "manifest");
	const char *id = text_field(root, "transaction");
	yyjson_val *channel_value = yyjson_obj_get(root, "channel");
	const char *channel = text_field(root, "channel");
	yyjson_val *dirs = yyjson_obj_get(root, "directories");
	int valid = valid_private(root, channel_value ? 6 : 5) &&
		(!channel_value || channel) && manifest && id &&
		lx_identifier(id) && yyjson_is_arr(dirs) &&
		yyjson_arr_size(dirs) <= 2000000;
	if (!valid) {
		yyjson_doc_free(doc);
		free(bytes);
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Invalid installed state; preserve recovery evidence");
	}
	launcher_release *release = NULL;
	result = decode_saved(manifest, yyjson_obj_get(root, "descriptors"),
		1, &release, error);
	if (result == LAUNCHER_OK && channel)
		result = lx_release_channel(release, channel, strlen(channel),
			"saved", error);
	size_t count = yyjson_arr_size(dirs);
	char **owned = (char **)calloc(count ? count : 1, sizeof(char *));
	if (result == LAUNCHER_OK && !owned)
		result = lx_fail(
			error, LAUNCHER_MEMORY, "Cannot load directories");
	for (size_t i = 0; result == LAUNCHER_OK && i < count; ++i) {
		yyjson_val *value = yyjson_arr_get(dirs, i);
		const char *path = yyjson_get_str(value);
		if (!path || strlen(path) != yyjson_get_len(value) ||
			!lx_path_valid(path, 0))
			result = lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Invalid owned directory");
		else if (!(owned[i] = lx_strdup(path)))
			result = lx_fail(error, LAUNCHER_MEMORY,
				"Cannot load owned directory");
	}
	if (result == LAUNCHER_OK) {
		int unchanged = handle->installed &&
			!strcmp(handle->installed_tx, id) &&
			lx_same_channel(handle->installed, release) &&
			handle->installed->json_size == release->json_size &&
			!memcmp(handle->installed->json, release->json,
				release->json_size);
		/* Reloading an uncommitted update preserves borrowed
			* releases.
			*/
		if (unchanged) {
			launcher_release_free(&release);
		} else {
			launcher_release_free(&handle->installed);
			release->descriptor_backing = bytes;
			bytes = NULL;
			handle->installed = release;
		}
		for (size_t i = 0; i < handle->owned_dir_count; ++i)
			free(handle->owned_dirs[i]);
		free(handle->owned_dirs);
		handle->owned_dirs = owned;
		handle->owned_dir_count = count;
		strcpy(handle->installed_tx, id);
	} else {
		launcher_release_free(&release);
		if (owned)
			for (size_t i = 0; i < count; ++i)
				free(owned[i]);
		free(owned);
	}
	yyjson_doc_free(doc);
	free(bytes);
	return result;
}

static int destination_path_compare(const void *a, const void *b)
{
	return path_compare(((const lx_destination *)a)->path,
		((const lx_destination *)b)->path);
}

static launcher_status validate_writes(launcher_handle *handle, transaction *tx,
	launcher_error *error)
{
	size_t count = 0, previous_count = 0;
	lx_destination *targets = lx_destinations(tx->target, &count);
	lx_destination *previous = lx_destinations(handle->installed,
		&previous_count);
	const char **writes = (const char **)calloc(tx->write_count + 1,
		sizeof(char *));
	const char **retained = (const char **)calloc(tx->retained_count + 1,
		sizeof(char *));

	if (!targets || !previous || !writes || !retained) {
		free(targets);
		free(previous);
		free(writes);
		free(retained);
		return lx_fail(error, LAUNCHER_MEMORY, "Checking journal write set");
	}
	for (size_t i = 0; i < tx->write_count; ++i)
		writes[i] = tx->writes[i].path;
	for (size_t i = 0; i < tx->retained_count; ++i)
		retained[i] = tx->retained[i].path;
	qsort(targets, count, sizeof(*targets), destination_path_compare);
	qsort(previous, previous_count, sizeof(*previous),
		destination_path_compare);
	qsort(writes, tx->write_count, sizeof(char *), pointer_compare);
	qsort(retained, tx->retained_count, sizeof(char *), pointer_compare);
	int valid = tx->write_count + tx->retained_count == count;
	int committed = !strcmp(handle->installed_tx, tx->id);

	for (size_t i = 0; valid && i < tx->write_count; ++i)
		valid = !i || path_compare(writes[i - 1], writes[i]);
	for (size_t i = 0; valid && i < tx->retained_count; ++i)
		valid = !i || path_compare(retained[i - 1], retained[i]);
	for (size_t i = 0; valid && i < count; ++i) {
		const char *path = targets[i].path;
		size_t w = lower_path(writes, tx->write_count, path);
		size_t r = lower_path(retained, tx->retained_count, path);
		int written = w < tx->write_count && !path_compare(writes[w], path);
		int kept = r < tx->retained_count && !path_compare(retained[r], path);

		valid = written != kept;
		if (valid && kept && !committed) {
			lx_destination *old = (lx_destination *)bsearch(targets + i,
				previous, previous_count, sizeof(*previous),
				destination_path_compare);

			valid = old && lx_same_destination(tx->target, targets + i,
				handle->installed, old);
		}
	}
	for (size_t i = 0; valid && i < tx->file_count; ++i) {
		lx_destination key = {};

		key.path = tx->files[i].path;
		if (bsearch(&key, targets, count, sizeof(*targets),
			destination_path_compare)) {
			size_t w = lower_path(writes, tx->write_count, key.path);

			valid = w < tx->write_count &&
				!path_compare(writes[w], key.path);
		}
	}
	free(targets);
	free(previous);
	free(writes);
	free(retained);
	return valid ? LAUNCHER_OK : lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
		"Invalid journal write set; preserve recovery evidence");
}

static launcher_status select_writes(transaction *tx,
	const launcher_plan *plan, launcher_error *error)
{
	const char **retained = (const char **)calloc(plan->destination_count + 1,
		sizeof(char *));
	tx->patches = (tx_file *)calloc(plan->destination_count + 1,
		sizeof(tx_file));
	tx->writes = (tx_file *)calloc(plan->destination_count + 1,
		sizeof(tx_file));
	tx->retained = (tx_file *)calloc(plan->destination_count + 1,
		sizeof(tx_file));
	if (!retained || !tx->writes || !tx->retained || !tx->patches) {
		free(retained);
		return lx_fail(error, LAUNCHER_MEMORY, "Selecting physical writes");
	}
	size_t count = 0;
	launcher_status status = LAUNCHER_OK;

	for (size_t i = 0; i < plan->destination_count; ++i) {
		const lx_destination *destination = plan->destinations + i;

		char *path = lx_strdup(destination->path);

		if (!path) {
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Retaining physical write set");
			break;
		}
		if (destination->incremental) {
			char *patch = lx_strdup(destination->path);

			if (!patch) {
				free(path);
				status = LAUNCHER_MEMORY;
				break;
			}
			tx->patches[tx->patch_count++].path = patch;
		}
		if (destination->retained) {
			retained[count++] = path;
			tx->retained[tx->retained_count++].path = path;
		} else
			tx->writes[tx->write_count++].path = path;
	}
	qsort(retained, count, sizeof(char *), pointer_compare);
	size_t at = 0;

	for (size_t i = 0; i < tx->file_count; ++i) {
		size_t found = lower_path(retained, count, tx->files[i].path);

		if (found < count && !path_compare(retained[found],
			tx->files[i].path))
			free(tx->files[i].path);
		else
			tx->files[at++] = tx->files[i];
	}
	tx->file_count = at;
	free(retained);
	return status;
}

static launcher_status journal_save(
	launcher_handle *handle, transaction *tx, launcher_error *error)
{
	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	if (!doc)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot allocate journal");
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_val *files = yyjson_mut_arr(doc);
	yyjson_mut_val *writes = yyjson_mut_arr(doc);
	yyjson_mut_val *retained = yyjson_mut_arr(doc);
	yyjson_mut_val *patches = yyjson_mut_arr(doc);
	yyjson_mut_val *dirs = yyjson_mut_arr(doc);
	int valid = root && files && writes && retained && patches && dirs &&
		yyjson_mut_obj_add_int(doc, root, "format", 3) &&
		yyjson_mut_obj_add_strcpy(doc, root, "transaction", tx->id) &&
		yyjson_mut_obj_add_strcpy(
			doc, root, "manifest", tx->target->json) &&
		save_descriptors(doc, root, tx->target) &&
		yyjson_mut_obj_add_val(doc, root, "files", files) &&
		yyjson_mut_obj_add_val(doc, root, "writes", writes) &&
		yyjson_mut_obj_add_val(doc, root, "retained", retained) &&
		yyjson_mut_obj_add_val(doc, root, "patches", patches) &&
		yyjson_mut_obj_add_val(doc, root, "directories", dirs);
	if (valid && tx->target->channel_json)
		valid = yyjson_mut_obj_add_strcpy(doc, root, "channel",
			tx->target->channel_json);
	for (size_t i = 0; valid && i < tx->patch_count; ++i)
		valid = yyjson_mut_arr_add_strcpy(doc, patches,
			tx->patches[i].path);
	for (size_t i = 0; valid && i < tx->file_count; ++i)
		valid = yyjson_mut_arr_add_strcpy(
			doc, files, tx->files[i].path);
	for (size_t i = 0; valid && i < tx->write_count; ++i)
		valid = yyjson_mut_arr_add_strcpy(
			doc, writes, tx->writes[i].path);
	for (size_t i = 0; valid && i < tx->retained_count; ++i)
		valid = yyjson_mut_arr_add_strcpy(
			doc, retained, tx->retained[i].path);
	for (size_t i = 0; valid && i < tx->dir_count; ++i) {
		yyjson_mut_val *dir = yyjson_mut_obj(doc);
		valid = dir && yyjson_mut_arr_add_val(dirs, dir) &&
			yyjson_mut_obj_add_strcpy(
				doc, dir, "path", tx->dirs[i].path) &&
			yyjson_mut_obj_add_bool(
				doc, dir, "existed", tx->dirs[i].existed) &&
			yyjson_mut_obj_add_bool(
				doc, dir, "owned", tx->dirs[i].owned) &&
			yyjson_mut_obj_add_bool(
				doc, dir, "needed", tx->dirs[i].needed) &&
			yyjson_mut_obj_add_bool(
				doc, dir, "transition", tx->dirs[i].transition);
	}
	size_t size = 0;
	char *bytes = valid ? yyjson_mut_write(doc, 0, &size) : NULL;
	launcher_status result = bytes && size <= private_limit
		? lx_save_bytes(handle->private_fs, "journal.json", bytes, size,
				error)
		: lx_fail(error, LAUNCHER_MEMORY,
				"Cannot encode bounded journal");
	free(bytes);
	yyjson_mut_doc_free(doc);
	return result;
}

static launcher_status read_target_paths(yyjson_val *values, tx_file **out,
	size_t *count, launcher_error *error)
{
	*out = (tx_file *)calloc(yyjson_arr_size(values) + 1, sizeof(tx_file));
	if (!*out)
		return lx_fail(error, LAUNCHER_MEMORY, "Reading journal paths");
	for (size_t i = 0; i < yyjson_arr_size(values); ++i) {
		yyjson_val *value = yyjson_arr_get(values, i);
		const char *path = yyjson_get_str(value);

		if (!path || strlen(path) != yyjson_get_len(value) ||
			!lx_path_valid(path, 1))
			return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Unsafe journal target destination");
		(*out)[i].path = lx_strdup(path);
		if (!(*out)[i].path)
			return lx_fail(error, LAUNCHER_MEMORY,
				"Reading journal target destination");
		++*count;
	}
	return LAUNCHER_OK;
}

static launcher_status journal_load(
	launcher_handle *handle, transaction *tx, launcher_error *error)
{
	char *bytes = NULL;
	size_t size = 0;
	launcher_status result = lx_read_bytes(handle->private_fs,
		"journal.json", private_limit, &bytes, &size, error);
	if (result != LAUNCHER_OK)
		return result;
	yyjson_doc *doc = yyjson_read(bytes, size, 0);
	free(bytes);
	yyjson_val *root = yyjson_doc_get_root(doc);
	const char *id = text_field(root, "transaction");
	const char *manifest = text_field(root, "manifest");
	yyjson_val *patches = yyjson_obj_get(root, "patches");
	const char *channel = text_field(root, "channel");
	yyjson_val *files = yyjson_obj_get(root, "files");
	yyjson_val *writes = yyjson_obj_get(root, "writes");
	yyjson_val *retained = yyjson_obj_get(root, "retained");
	yyjson_val *dirs = yyjson_obj_get(root, "directories");
	int valid = valid_private(root, channel ? 10 : 9, 3) &&
		id && lx_identifier(id) && manifest && yyjson_is_arr(files) &&
		yyjson_is_arr(patches) && yyjson_arr_size(patches) <= 100000 &&
		yyjson_is_arr(dirs) &&
		yyjson_arr_size(files) <= 200002 && yyjson_is_arr(writes) &&
		yyjson_arr_size(writes) <= 200002 && yyjson_is_arr(retained) &&
		yyjson_arr_size(retained) <= 200002 &&
		yyjson_arr_size(dirs) <= 2000000;
	if (!valid) {
		yyjson_doc_free(doc);
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Invalid journal; preserve recovery evidence");
	}
	strcpy(tx->id, id);
	result = decode_saved(manifest, yyjson_obj_get(root, "descriptors"),
		0, &tx->target, error);
	if (result == LAUNCHER_OK && channel)
		result = lx_release_channel(
			tx->target, channel, strlen(channel), "saved", error);
	if (result == LAUNCHER_OK)
		result = read_target_paths(
			patches, &tx->patches, &tx->patch_count, error);
	for (size_t i = 0; result == LAUNCHER_OK && i < yyjson_arr_size(files);
		++i) {
		yyjson_val *value = yyjson_arr_get(files, i);
		const char *path = yyjson_get_str(value);
		if (!path || strlen(path) != yyjson_get_len(value) ||
			!lx_path_valid(path, 0))
			result = lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Unsafe original in journal");
		else if (index_find(&tx->file_index, tx->files, sizeof(tx_file),
					path) != SIZE_MAX)
			result = lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Duplicate original in journal");
		else
			result = add_file(tx, path, error);
	}
	if (result == LAUNCHER_OK)
		result = read_target_paths(writes, &tx->writes, &tx->write_count,
			error);
	if (result == LAUNCHER_OK)
		result = read_target_paths(retained, &tx->retained,
			&tx->retained_count, error);
	if (result == LAUNCHER_OK)
		result = validate_writes(handle, tx, error);
	for (size_t i = 0; result == LAUNCHER_OK && i < tx->patch_count; i++) {
		const char *path = tx->patches[i].path;
		int target_found = 0, old_found = 0, write_found = 0;

		for (size_t j = 0; j < tx->target->group_count; j++)
			target_found |=
				tx->target->groups[j].kind == LAUNCHER_HOGG &&
				!strcmp(tx->target->groups[j].path, path);
		if (handle->installed)
			for (size_t j = 0; j < handle->installed->group_count;
				j++)
				old_found |=
					handle->installed->groups[j].kind ==
						LAUNCHER_HOGG &&
					!strcmp(handle->installed->groups[j]
							.path,
						path);
		for (size_t j = 0; j < tx->write_count; j++)
			write_found |= !strcmp(tx->writes[j].path, path);
		for (size_t j = 0; j < i; j++)
			if (!strcmp(tx->patches[j].path, path))
				target_found = 0;
		if (!target_found || !old_found || !write_found)
			result = lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Invalid journal archive patch set");
	}
	size_t directory_index, directory_max;
	yyjson_val *value;
	yyjson_arr_foreach(dirs, directory_index, directory_max, value)
	{
		if (result != LAUNCHER_OK)
			break;
		const char *path = text_field(value, "path");
		yyjson_val *existed = yyjson_obj_get(value, "existed");
		yyjson_val *owned = yyjson_obj_get(value, "owned");
		yyjson_val *needed = yyjson_obj_get(value, "needed");
		yyjson_val *transition = yyjson_obj_get(value, "transition");
		if (!yyjson_is_obj(value) || yyjson_obj_size(value) != 5 ||
			!path || !lx_path_valid(path, 0) ||
			!yyjson_is_bool(existed) || !yyjson_is_bool(owned) ||
			!yyjson_is_bool(needed) ||
			!yyjson_is_bool(transition)) {
			result = lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Unsafe directory in journal");
			break;
		}
		if (index_find(&tx->dir_index, tx->dirs, sizeof(tx_dir),
				path) != SIZE_MAX) {
			result = lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
				"Duplicate directory in journal");
			break;
		}
		result = add_dir(handle, tx, path, yyjson_get_bool(existed),
			yyjson_get_bool(needed), error);
		if (result == LAUNCHER_OK) {
			tx->dirs[tx->dir_count - 1].owned =
				yyjson_get_bool(owned);
			tx->dirs[tx->dir_count - 1].transition =
				yyjson_get_bool(transition);
		}
	}
	yyjson_doc_free(doc);
	return result;
}

struct remove_state {
	lx_fs *fs;
	const char *parent;
	launcher_error *error;
	launcher_status result;
};

static launcher_status remove_tree(
	lx_fs *fs, const char *path, launcher_error *error);

static int remove_child(const char *name, int kind, void *opaque)
{
	remove_state *state = (remove_state *)opaque;
	char *prefix = joined(state->parent, "/");
	char *path = prefix ? joined(prefix, name) : NULL;
	free(prefix);
	if (!path) {
		state->result = lx_fail(state->error, LAUNCHER_MEMORY,
			"Cannot clean transaction");
		return 0;
	}
	if (kind == 2)
		state->result = remove_tree(state->fs, path, state->error);
	else if (kind == 1)
		state->result = lx_fs_remove(state->fs, path, 0, state->error);
	else
		state->result = lx_fail(state->error, LAUNCHER_CONFLICT,
			"Unexpected special file in private transaction");
	free(path);
	return state->result == LAUNCHER_OK;
}

static launcher_status remove_tree(
	lx_fs *fs, const char *path, launcher_error *error)
{
	int kind = 0;
	launcher_status result = lx_fs_stat(fs, path, &kind, NULL, error);
	if (result != LAUNCHER_OK || !kind)
		return result;
	if (kind != 2)
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Expected private transaction directory");
	remove_state state = {fs, path, error, LAUNCHER_OK};
	result = lx_fs_list(fs, path, remove_child, &state, error);
	if (state.result != LAUNCHER_OK)
		return state.result;
	if (result == LAUNCHER_OK)
		result = lx_fs_remove(fs, path, 1, error);
	return result;
}

static launcher_status cleanup(launcher_handle *handle, launcher_error *error)
{
	/* The journal is removed last. Committed state remains the authority if
		* cleanup is interrupted after any private staging
		* deletion.
		*/
	launcher_status result = lx_fs_flush(handle->private_fs, error);

	if (result == LAUNCHER_OK)
		result = remove_tree(handle->private_fs, "transaction", error);
	if (result == LAUNCHER_OK)
		result = lx_fs_remove(
			handle->private_fs, "journal.json", 0, error);
	if (result == LAUNCHER_OK)
		result = lx_fs_flush(handle->private_fs, error);
	return result;
}

static launcher_status ensure_parent(
	lx_fs *fs, const char *path, launcher_error *error)
{
	char *copy = lx_strdup(path);
	if (!copy)
		return lx_fail(
			error, LAUNCHER_MEMORY, "Cannot allocate parent path");
	char *slash = strrchr(copy, '/');
	launcher_status result = LAUNCHER_OK;
	if (slash) {
		*slash = 0;
		result = lx_fs_mkdirs(fs, copy, error);
		if (result == LAUNCHER_OK)
			result = lx_fs_case(fs, copy, error);
	}
	free(copy);
	return result;
}

static launcher_status remove_empty(
	lx_fs *fs, const char *path, launcher_error *error)
{
	int kind = 0;
	launcher_status result = lx_fs_stat(fs, path, &kind, NULL, error);
	if (result != LAUNCHER_OK || !kind)
		return result;
	if (kind != 2)
		return LAUNCHER_OK;
	int empty = 0;
	result = lx_fs_empty(fs, path, &empty, error);
	if (result == LAUNCHER_OK && empty)
		result = lx_fs_remove(fs, path, 1, error);
	return result;
}

static launcher_status resume_forward(launcher_handle *handle,
	transaction *tx, launcher_progress_fn progress, void *state,
	launcher_error *error);

launcher_status lx_transaction_recover(launcher_handle *handle,
	launcher_error *error)
{
	int kind = 0;
	launcher_status result = lx_fs_stat(handle->private_fs, "journal.json",
		&kind, NULL, error);

	if (result != LAUNCHER_OK)
		return result;
	if (!kind) {
		result = lx_fs_remove(
			handle->private_fs, "compact.hogg", 0, error);
		return result == LAUNCHER_OK ? remove_tree(handle->private_fs,
						       "transaction", error)
					     : result;
	}
	transaction tx = {};

	result = journal_load(handle, &tx, error);
	if (result == LAUNCHER_OK) {
		if (!strcmp(handle->installed_tx, tx.id))
			result = cleanup(handle, error);
		else
			result = resume_forward(handle, &tx, NULL, NULL, error);
	}
	transaction_free(&tx);
	if (result != LAUNCHER_OK) {
		handle->blocked = 1;
		launcher_error cause = error ? *error : launcher_error{};

		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Pending installation needs recovery; preserve sidecar "
			"evidence: "
			"%s",
			cause.message);
	}
	return result;
}

static launcher_status state_save(
	launcher_handle *handle, transaction *tx, launcher_error *error)
{
	yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
	if (!doc)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Cannot encode installed state");
	yyjson_mut_val *root = yyjson_mut_obj(doc);
	yyjson_mut_doc_set_root(doc, root);
	yyjson_mut_val *dirs = yyjson_mut_arr(doc);
	int valid = root && dirs &&
		yyjson_mut_obj_add_int(doc, root, "format", 2) &&
		yyjson_mut_obj_add_strcpy(doc, root, "transaction", tx->id) &&
		yyjson_mut_obj_add_strcpy(
			doc, root, "manifest", tx->target->json) &&
		save_descriptors(doc, root, tx->target) &&
		yyjson_mut_obj_add_val(doc, root, "directories", dirs);
	if (valid && tx->target->channel_json)
		valid = yyjson_mut_obj_add_strcpy(doc, root, "channel",
			tx->target->channel_json);
	char **owned = valid ? (char **)calloc(
		tx->dir_count ? tx->dir_count : 1, sizeof(*owned)) : NULL;
	size_t owned_count = 0;

	if (valid && !owned) {
		yyjson_mut_doc_free(doc);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Preparing installed directory ownership");
	}
	launcher_status result = LAUNCHER_OK;
	for (size_t i = 0; valid && result == LAUNCHER_OK &&
		i < tx->dir_count; ++i) {
		if (!tx->dirs[i].needed ||
			!(tx->dirs[i].owned || !tx->dirs[i].existed))
			continue;
		char *actual = NULL;

		result = lx_fs_spell(
			handle->install, tx->dirs[i].path, &actual, error);
		if (result == LAUNCHER_OK) {
			owned[owned_count++] = actual;
			valid = yyjson_mut_arr_add_strcpy(doc, dirs, actual);
		}
	}
	size_t size = 0;
	char *bytes = valid && result == LAUNCHER_OK ?
		yyjson_mut_write(doc, 0, &size) : NULL;

	if (result == LAUNCHER_OK)
		result = bytes && size <= private_limit ?
			lx_save_bytes(handle->private_fs,
				"installed-state.json", bytes, size, error) :
			lx_fail(error, LAUNCHER_MEMORY,
				"Cannot encode bounded installed state");
	free(bytes);
	yyjson_mut_doc_free(doc);
	if (result == LAUNCHER_OK) {
		launcher_release_free(&handle->installed);
		handle->installed = tx->target;
		tx->target = NULL;
		for (size_t i = 0; i < handle->owned_dir_count; ++i)
			free(handle->owned_dirs[i]);
		free(handle->owned_dirs);
		handle->owned_dirs = owned;
		handle->owned_dir_count = owned_count;
		owned = NULL;
		owned_count = 0;
		strcpy(handle->installed_tx, tx->id);
	}
	for (size_t i = 0; i < owned_count; ++i)
		free(owned[i]);
	free(owned);
	return result;
}

static const lx_group *archive_group(const launcher_release *release,
	const char *path, size_t *index)
{
	if (release)
		for (size_t i = 0; i < release->group_count; i++)
			if (release->groups[i].kind == LAUNCHER_HOGG &&
				!strcmp(release->groups[i].path, path)) {
				*index = i;
				return release->groups + i;
			}
	return NULL;
}

static int includes_path(const tx_file *files, size_t count, const char *path)
{
	for (size_t i = 0; i < count; i++)
		if (!strcmp(files[i].path, path))
			return 1;
	return 0;
}

static int completed_target_ancestor(launcher_handle *handle,
	const launcher_release *target, const char *path)
{
	for (size_t i = 0; i < lx_physical_count(target); i++) {
		const char *ancestor = physical(target, i);
		size_t length = strlen(ancestor);

		if (strlen(path) <= length || strncmp(path, ancestor, length) ||
			path[length] != '/')
			continue;
		int kind = 0;

		if (lx_fs_stat(handle->install, ancestor, &kind, NULL, NULL) ==
			LAUNCHER_OK && kind == 1)
			return 1;
	}
	return 0;
}

static launcher_status resume_forward(launcher_handle *handle,
	transaction *tx, launcher_progress_fn progress, void *state,
	launcher_error *error)
{
	launcher_status result = LAUNCHER_OK;
	uint64_t completed = 0;
	uint64_t total = tx->file_count + tx->write_count + 2;

	if (!lx_progress(progress, state, LAUNCHER_ACTIVATE, NULL,
		completed++, total, 0, 0))
		return lx_fail(error, LAUNCHER_CANCELLED,
			"Update paused; reopen to finish the selected release");
	for (size_t i = 0; result == LAUNCHER_OK && i < tx->file_count; i++) {
		const char *path = tx->files[i].path;

		if (!includes_path(tx->writes, tx->write_count, path) &&
			!completed_target_ancestor(handle, tx->target, path)) {
			int kind = 0, needed_directory = 0;

			result = lx_fs_stat(
				handle->install, path, &kind, NULL, error);
			for (size_t j = 0; j < tx->dir_count; j++)
				needed_directory |= tx->dirs[j].needed &&
					!path_compare(tx->dirs[j].path, path);
			if (result == LAUNCHER_OK &&
				!(kind == 2 && needed_directory))
				result = lx_fs_remove(
					handle->install, path, 0, error);
		}
		if (result == LAUNCHER_OK && !lx_progress(progress, state,
			LAUNCHER_ACTIVATE, path, completed++, total, 0, 0))
			result = lx_fail(error, LAUNCHER_CANCELLED,
				"Update paused; reopen to finish the selected "
				"release");
	}
	for (size_t i = tx->dir_count; result == LAUNCHER_OK && i > 0; i--) {
		if (completed_target_ancestor(handle, tx->target,
			tx->dirs[i - 1].path))
			continue;
		if (tx->dirs[i - 1].needed && tx->dirs[i - 1].existed)
			continue;
		if (!tx->dirs[i - 1].needed && !tx->dirs[i - 1].owned &&
			!tx->dirs[i - 1].transition)
			continue;
		result = remove_empty(
			handle->install, tx->dirs[i - 1].path, error);
	}
	for (size_t i = 0; result == LAUNCHER_OK && i < tx->write_count; i++) {
		const char *path = tx->writes[i].path;
		size_t target_index = 0, previous_index = 0;
		const lx_group *group =
			archive_group(tx->target, path, &target_index);

		if (includes_path(tx->patches, tx->patch_count, path)) {
			launcher_release before = {}, target = {};

			if (!group || !archive_group(handle->installed, path,
				&previous_index))
				return lx_fail(error,
					LAUNCHER_RECOVERY_REQUIRED,
					"Pending archive has no saved "
					"baseline");
			lx_group_view(
				handle->installed, previous_index, &before);
			lx_group_view(tx->target, target_index, &target);
			result = lx_patch_hogg(handle, &before, &target,
				progress, state, error);
		} else {
			char *source = staged(path);
			int kind = 0;

			if (!source)
				return lx_fail(error, LAUNCHER_MEMORY,
					"Allocating pending destination");
			result = lx_fs_stat(
				handle->private_fs, source, &kind, NULL, error);
			if (result == LAUNCHER_OK && kind) {
				result = remove_empty(
					handle->install, path, error);
				if (result == LAUNCHER_OK)
					result = ensure_parent(
						handle->install, path, error);
				if (result == LAUNCHER_OK)
					result = lx_fs_replace(
						handle->private_fs, source,
						handle->install, path, error);
			}
			free(source);
		}
		if (result == LAUNCHER_OK && !lx_progress(progress, state,
			LAUNCHER_ACTIVATE, path, completed++, total, 0, 0))
			result = lx_fail(error, LAUNCHER_CANCELLED,
				"Update paused; reopen to finish the selected "
				"release");
	}
	if (result == LAUNCHER_OK)
		result = lx_check_release(handle, tx->target, NULL, NULL,
			NULL, NULL, error);
	if (result == LAUNCHER_OK)
		result = state_save(handle, tx, error);
	launcher_status loaded = LAUNCHER_OK;

	if (result != LAUNCHER_OK) {
		/* Replacement can succeed before a later directory flush fails.
		 * Read the selected state before deciding whether activation
		 * must resume.
		 */
		launcher_error selected_error = {};

		loaded = lx_state_load(handle, &selected_error);
	}
	int committed = loaded == LAUNCHER_OK &&
		!strcmp(handle->installed_tx, tx->id);

	if (!committed) {
		handle->blocked = 1;
		return result == LAUNCHER_OK
			       ? lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
					 "Pending installation needs retry")
			       : result;
	}
	++handle->epoch;
	launcher_error warning = {};

	for (size_t i = 0; i < tx->write_count; i++) {
		size_t index = 0;

		if (!archive_group(handle->installed, tx->writes[i].path,
			&index))
			continue;
		lx_progress(progress, state, LAUNCHER_STAGE,
			tx->writes[i].path, 0, 1, 0, 0);
		launcher_error maintenance = {};
		launcher_status compacted = lx_compact_hogg(handle,
			tx->writes[i].path, &maintenance);

		if (compacted == LAUNCHER_INTEGRITY ||
			compacted == LAUNCHER_CONFLICT) {
			if (error)
				*error = maintenance;
			return compacted;
		}
		if (compacted != LAUNCHER_OK)
			warning = maintenance;
	}
	launcher_status cleaned = cleanup(handle, error);

	if (cleaned != LAUNCHER_OK) {
		handle->blocked = 1;
		return lx_fail(error, LAUNCHER_COMMITTED,
			"Release installed; reopen to finish private cleanup");
	}
	lx_clear(error);
	if (warning.message[0] && error)
		snprintf(error->message, sizeof(error->message),
			"Release installed; archive space reclamation "
			"deferred: %.180s",
			warning.message);
	lx_progress(
		progress, state, LAUNCHER_ACTIVATE, NULL, total, total, 0, 0);
	return LAUNCHER_OK;
}

launcher_status lx_transaction_apply(launcher_handle *handle,
	launcher_plan *plan, launcher_progress_fn callback, void *state,
	launcher_error *error)
{
	transaction tx = {};
	launcher_status result =
		lx_clone_release(plan->target, &tx.target, error);

	if (result == LAUNCHER_OK)
		result = inventory(handle, plan->target, &tx, error);
	if (result == LAUNCHER_OK)
		result = select_writes(&tx, plan, error);
	if (result == LAUNCHER_OK)
		result = remove_tree(handle->private_fs, "transaction", error);
	if (result == LAUNCHER_OK)
		result = lx_fs_mkdirs(
			handle->private_fs, "transaction/new", error);
	for (size_t i = 0; result == LAUNCHER_OK && i < plan->destination_count;
		i++) {
		const lx_destination *destination = plan->destinations + i;

		if (destination->retained || destination->incremental)
			continue;
		char *dest = staged(destination->path);

		if (!dest) {
			result = LAUNCHER_MEMORY;
			break;
		}
		if (destination->group != SIZE_MAX) {
			launcher_release view = {};

			lx_group_view(plan->target, destination->group, &view);
			result = lx_stage_hogg(
				handle, &view, dest, callback, state, error);
		} else {
			const lx_entry *entry = plan->target->loose_files +
				destination->entry;
			char source[80];

			snprintf(source, sizeof(source), "objects/%s", entry->digest);
			result = ensure_parent(handle->private_fs, dest, error);
			if (result == LAUNCHER_OK)
				result = lx_copy_object(handle->private_fs, source,
					handle->private_fs, dest, entry->size, NULL, error);
		}
		free(dest);
	}
	for (size_t i = 0; result == LAUNCHER_OK && i < plan->destination_count;
		i++) {
		const lx_destination *destination = plan->destinations + i;

		if (destination->incremental) {
			lx_destination previous = *destination;

			previous.group = destination->previous_group;
			result = lx_check_destination(handle, handle->installed,
				&previous, NULL, callback, state, error);
		} else
			result = lx_check_destination(handle, plan->target,
				destination,
				destination->retained ? NULL
						      : "transaction/new/",
				callback, state, error);
	}
	if (result == LAUNCHER_OK && !lx_progress(callback, state,
		LAUNCHER_STAGE, NULL, 1, 1, 0, 0))
		result = lx_fail(error, LAUNCHER_CANCELLED,
			"Update cancelled before installation edits");
	if (result != LAUNCHER_OK) {
		remove_tree(handle->private_fs, "transaction", NULL);
		transaction_free(&tx);
		return result;
	}
	unsigned long long serial = handle->installed_tx[0] == 't' ?
		strtoull(handle->installed_tx + 1, NULL, 16) : 0;

	if (serial == ~0ULL) {
		transaction_free(&tx);
		return lx_fail(error, LAUNCHER_UNSUPPORTED,
			"Installation transaction sequence exhausted");
	}
	snprintf(tx.id, sizeof(tx.id), "t%llx", serial + 1);
	result = journal_save(handle, &tx, error);
	if (result == LAUNCHER_OK)
		result = resume_forward(handle, &tx, callback, state, error);
	if (result != LAUNCHER_OK)
		handle->blocked = 1;
	transaction_free(&tx);
	return result;
}
