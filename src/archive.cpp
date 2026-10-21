#include "internal.h"
#include <mbedtls/sha256.h>
#include <stdlib.h>
#include <string.h>

static constexpr size_t chunk_size = 65536;

struct archive_view {
	lx_fs *root;
	pg_context *context;
	pg_source *source;
	lx_fs *parent;
	char *path;
};

static launcher_status piggle_failure(pg_status status, const pg_error *detail,
	const char *operation, launcher_error *error)
{
	launcher_status mapped = LAUNCHER_INTEGRITY;

	if (status == PG_NOMEM)
		mapped = LAUNCHER_MEMORY;
	else if (status == PG_IO)
		mapped = LAUNCHER_IO;
	else if (status == PG_BUSY)
		mapped = LAUNCHER_BUSY;
	else if (status == PG_CONFLICT || status == PG_STALE ||
			status == PG_RETRY)
		mapped = LAUNCHER_CONFLICT;
	launcher_status result = lx_fail(error, mapped, "%s: %s (Piggle %d)",
		operation, detail && detail->message[0] ? detail->message :
		"archive operation failed", (int)status);

	if (error && detail)
		error->native_code = detail->native_code;
	return result;
}

static launcher_status validate_view(archive_view *view,
	launcher_error *error)
{
	launcher_status status = lx_fs_validate(view->root, error);

	if (status == LAUNCHER_OK)
		status = lx_fs_validate(view->parent, error);
	return status;
}

static launcher_status close_archive(archive_view *view,
	launcher_status status, launcher_error *error)
{
	if (status == LAUNCHER_OK)
		status = validate_view(view, error);
	pg_error detail = {};
	pg_status pg = pg_source_close(&view->source, &detail);

	if (pg != PG_OK && status == LAUNCHER_OK)
		status = piggle_failure(pg, &detail, "Closing archive", error);
	pg = pg_context_close(&view->context, &detail);
	if (pg != PG_OK && status == LAUNCHER_OK)
		status = piggle_failure(pg, &detail,
			"Closing Piggle context", error);
	lx_fs_close(&view->parent);
	free(view->path);
	view->path = NULL;
	return status;
}

static launcher_status open_archive_mode(lx_fs *fs, const char *path,
	archive_view *view, int writable, launcher_error *error)
{
	view->root = fs;
	launcher_status status = lx_fs_pin(fs, path, 0, &view->parent,
		&view->path, error);
	pg_error detail = {};
	pg_status pg = PG_OK;

	if (status == LAUNCHER_OK)
		status = validate_view(view, error);
	if (status == LAUNCHER_OK)
		pg = pg_context_open(&view->context, &detail);
	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(pg, &detail,
			"Creating Piggle context", error);
	pg_source_options options = { PG_HOGG10,
		writable ? PG_WRITE : PG_READ, PG_CHECKSUM_LOGICAL };

	if (status == LAUNCHER_OK)
		pg = pg_source_open(view->context, view->path, &options,
			&view->source, &detail);
	if (status == LAUNCHER_OK && writable && pg == PG_RECOVERY_REQUIRED) {
		pg = pg_source_recover(view->context, view->path, &detail);
		if (pg == PG_OK)
			pg = pg_source_open(view->context, view->path, &options,
				&view->source, &detail);
	}
	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(pg, &detail, "Opening HOGG10", error);
	if (status == LAUNCHER_OK)
		status = validate_view(view, error);
	if (status != LAUNCHER_OK)
		status = close_archive(view, status, error);
	return status;
}

static launcher_status open_archive(lx_fs *fs, const char *path,
	archive_view *view, launcher_error *error)
{
	return open_archive_mode(fs, path, view, 0, error);
}

static int hash_matches(mbedtls_sha256_context *hash, const char *expected)
{
	unsigned char bytes[32];
	char digest[65];
	static const char hex[] = "0123456789abcdef";

	if (mbedtls_sha256_finish(hash, bytes))
		return 0;
	for (size_t i = 0; i < 32; i++) {
		digest[i * 2] = hex[bytes[i] >> 4];
		digest[i * 2 + 1] = hex[bytes[i] & 15];
	}
	digest[64] = 0;
	return !strcmp(digest, expected);
}

static launcher_status hash_stored(pg_file *file, const lx_entry *entry,
	FILE *copy, launcher_progress_fn callback, void *state,
	launcher_error *error)
{
	pg_reader *reader = NULL;
	pg_error detail = {};
	pg_status pg = pg_reader_open(file, PG_READ_STORED_UNCHECKED,
		&reader, &detail);

	if (pg != PG_OK)
		return piggle_failure(pg, &detail,
			"Opening stored payload", error);
	mbedtls_sha256_context hash;
	mbedtls_sha256_init(&hash);
	mbedtls_sha256_starts(&hash, 0);
	unsigned char chunk[chunk_size];
	uint64_t count = 0;
	launcher_status status = LAUNCHER_OK;

	for (;;) {
		size_t got = 0;

		pg = pg_reader_read(reader, chunk, sizeof(chunk), &got,
			&detail);
		if (pg != PG_OK && pg != PG_END) {
			status = piggle_failure(pg, &detail,
				"Reading stored payload", error);
			break;
		}
		if (got > entry->stored_size - count) {
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Archive entry exceeds declared size: %s",
				entry->path);
			break;
		}
		count += got;
		if (mbedtls_sha256_update(&hash, chunk, got)) {
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Hashing stored payload");
			break;
		}
		if (copy && got && fwrite(chunk, 1, got, copy) != got) {
			status = lx_fail(error, LAUNCHER_IO,
				"Copying stored archive payload");
			break;
		}
		if (!lx_progress(callback, state, LAUNCHER_VERIFY, entry->path,
			pg == PG_END ? 1 : 0, 1, 0, 0)) {
			status = lx_fail(error, LAUNCHER_CANCELLED,
				"Archive verification cancelled");
			break;
		}
		if (pg == PG_END)
			break;
	}
	if (status == LAUNCHER_OK &&
		(count != entry->stored_size ||
			!hash_matches(&hash, entry->digest)))
		status = lx_fail(error, LAUNCHER_INTEGRITY,
			"Archive entry length or SHA-256 differs: %s",
			entry->path);
	mbedtls_sha256_free(&hash);
	pg = pg_reader_close(&reader, &detail);
	if (pg != PG_OK && status == LAUNCHER_OK)
		status = piggle_failure(pg, &detail,
			"Closing stored reader", error);
	return status;
}

static int entry_compare(const void *left, const void *right)
{
	return strcmp((*(const lx_entry *const *)left)->path,
		(*(const lx_entry *const *)right)->path);
}

static unsigned hex_byte(const char *text)
{
	unsigned value = 0;

	for (size_t i = 0; i < 2; i++)
		value = value * 16 + (unsigned)(text[i] <= '9' ?
			text[i] - '0' : text[i] - 'a' + 10);
	return value;
}

static launcher_status check_metadata(const pg_file_info *info,
	const lx_entry *entry, launcher_error *error)
{
	if (info->logical_size != entry->size ||
		info->stored_size != entry->stored_size ||
		info->encoding != (entry->encoding == LAUNCHER_ZLIB ?
			(uint32_t)PG_ZLIB : (uint32_t)PG_LOGICAL) ||
		info->checksum_domain != PG_CHECKSUM_LOGICAL ||
		info->digest_kind != PG_DIGEST_MD5_32 ||
		info->cached_header_size != entry->cached_header_size)
		return LAUNCHER_INTEGRITY;
	if (entry->cached_header_sha256[0]) {
		char digest[65];
		launcher_status status = lx_digest_bytes(info->cached_header,
			info->cached_header_size, digest, error);

		if (status != LAUNCHER_OK)
			return status;
		if (strcmp(digest, entry->cached_header_sha256))
			return LAUNCHER_INTEGRITY;
	} else if (info->cached_header_size && memcmp(info->cached_header,
		entry->cached_header, info->cached_header_size)) {
		return LAUNCHER_INTEGRITY;
	}
	for (size_t i = 0; i < 4; i++)
		if (info->digest[i] != hex_byte(entry->logical_md5 + 2 * i))
			return LAUNCHER_INTEGRITY;
	return LAUNCHER_OK;
}

static launcher_status verify_archive(lx_fs *fs, const char *path,
	const launcher_release *release, launcher_progress_fn callback,
	void *state, int payload, launcher_error *error)
{
	archive_view view = {};
	launcher_status status = open_archive(fs, path, &view, error);

	if (status != LAUNCHER_OK)
		return status;
	const lx_entry **sorted = (const lx_entry **)calloc(
		release->hogg_count ? release->hogg_count : 1, sizeof(*sorted));

	if (!sorted)
		return close_archive(&view, lx_fail(error, LAUNCHER_MEMORY,
			"Sorting archive inventory"), error);
	for (size_t i = 0; i < release->hogg_count; i++)
		sorted[i] = release->hogg_files + i;
	qsort(sorted, release->hogg_count, sizeof(*sorted), entry_compare);
	pg_cursor *cursor = NULL;
	pg_error detail = {};
	pg_status pg = pg_source_files(view.source, NULL, &cursor, &detail);

	if (pg != PG_OK)
		status = piggle_failure(pg, &detail,
			"Listing archive", error);
	size_t count = 0;

	while (status == LAUNCHER_OK) {
		pg_file *file = NULL;

		pg = pg_cursor_next(cursor, &file, &detail);
		if (pg == PG_END)
			break;
		if (pg != PG_OK) {
			status = piggle_failure(pg, &detail,
			"Listing archive", error);
			break;
		}
		pg_file_info info = {};

		pg = pg_file_inspect(file, &info, &detail);
		if (pg != PG_OK)
			status = piggle_failure(pg, &detail,
				"Inspecting archive entry", error);
		else if (count == release->hogg_count ||
			strcmp(info.canonical_name, sorted[count]->path))
			status = LAUNCHER_INTEGRITY;
		else
			status = check_metadata(&info, sorted[count], error);
		if (status == LAUNCHER_INTEGRITY)
			status = lx_fail(error, status,
				"Archive membership differs from "
				"installed inventory");
		if (status == LAUNCHER_OK && payload)
			status = hash_stored(file, sorted[count], NULL,
				callback, state, error);
		pg = pg_file_close(&file, &detail);
		if (pg != PG_OK && status == LAUNCHER_OK)
			status = piggle_failure(pg, &detail,
				"Closing archive entry", error);
		count++;
	}
	if (status == LAUNCHER_OK && count != release->hogg_count)
		status = lx_fail(error, LAUNCHER_INTEGRITY,
			"Archive is missing desired entries");
	pg = pg_cursor_close(&cursor, &detail);
	if (pg != PG_OK && status == LAUNCHER_OK)
		status = piggle_failure(pg, &detail,
			"Closing inventory", error);
	free(sorted);
	return close_archive(&view, status, error);
}

static launcher_status check_release(launcher_handle *handle,
	const launcher_release *release, const char *hogg_override,
	const char *loose_prefix, launcher_progress_fn callback, void *state,
	int payload, launcher_error *error)
{
	launcher_status status = LAUNCHER_OK;

	for (size_t i = 0; i < release->group_count; ++i) {
		if (release->groups[i].kind != LAUNCHER_HOGG)
			continue;
		launcher_release view = {};
		char *path = NULL;

		lx_group_view(release, i, &view);
		if (hogg_override) {
			path = (char *)malloc(strlen(hogg_override) +
				strlen(view.hogg) + 1);
			if (!path)
				return lx_fail(error, LAUNCHER_MEMORY,
					"Allocating archive staging path");
			strcpy(path, hogg_override);
			strcat(path, view.hogg);
		}
		status = verify_archive(hogg_override ? handle->private_fs :
			handle->install, path ? path : view.hogg, &view,
			callback, state, payload, error);
		free(path);
		if (status != LAUNCHER_OK)
			return status == LAUNCHER_IO ? lx_fail(error,
				LAUNCHER_INTEGRITY, "Managed archive unavailable") :
				status;
	}
	for (size_t i = 0; i < release->loose_count; i++) {
		const lx_entry *entry = release->loose_files + i;

		if (!lx_progress(callback, state, LAUNCHER_VERIFY, entry->path,
			i, release->loose_count, 0, 0))
			return lx_fail(error, LAUNCHER_CANCELLED,
				"Loose-file verification cancelled");
		char *path = NULL;
		const char *relative = entry->path;

		if (loose_prefix) {
			size_t prefix = strlen(loose_prefix);
			size_t length = strlen(entry->path);

			path = (char *)malloc(prefix + length + 1);
			if (!path)
				return lx_fail(error, LAUNCHER_MEMORY,
					"Allocating verification path");
			memcpy(path, loose_prefix, prefix);
			memcpy(path + prefix, entry->path, length + 1);
			relative = path;
		}
		lx_fs *root = loose_prefix ?
			handle->private_fs : handle->install;

		if (payload) {
			FILE *file = NULL;

			status = lx_fs_read(root, relative, &file, error);
			if (status == LAUNCHER_OK)
				status = lx_hash_file(file, entry->size,
					entry->digest,
					NULL, error);
			if (file && fclose(file) && status == LAUNCHER_OK)
				status = lx_fail(error, LAUNCHER_IO,
					"Closing loose file %s", entry->path);
		} else {
			int kind = 0;
			uint64_t size = 0;

			status = lx_fs_stat(root, relative,
				&kind, &size, error);
			if (status == LAUNCHER_OK &&
				(kind != 1 || size != entry->size))
				status = lx_fail(error, LAUNCHER_INTEGRITY,
					"Managed loose file length differs: %s",
					entry->path);
		}
		free(path);
		if (status != LAUNCHER_OK)
			return status == LAUNCHER_IO ? lx_fail(error,
				LAUNCHER_INTEGRITY,
				"Managed loose file unavailable: %s",
				entry->path) : status;
	}
	return LAUNCHER_OK;
}

launcher_status lx_verify_release(launcher_handle *handle,
	const launcher_release *release, const char *hogg_override,
	const char *loose_prefix, launcher_progress_fn callback, void *state,
	launcher_error *error)
{
	return check_release(handle, release, hogg_override, loose_prefix,
		callback, state, 1, error);
}

launcher_status lx_check_release(launcher_handle *handle,
	const launcher_release *release, const char *hogg_override,
	const char *loose_prefix, launcher_progress_fn callback, void *state,
	launcher_error *error)
{
	return check_release(handle, release, hogg_override, loose_prefix,
		callback, state, 0, error);
}

const lx_entry *lx_destination_entries(const launcher_release *release,
	const lx_destination *destination, size_t *count)
{
	if (destination->group == SIZE_MAX) {
		*count = 1;
		return release->loose_files + destination->entry;
	}
	const lx_group *group = release->groups + destination->group;

	*count = group->count;
	return release->hogg_files + group->first;
}

int lx_same_destination(const launcher_release *a, const lx_destination *left,
	const launcher_release *b, const lx_destination *right)
{
	if ((left->group == SIZE_MAX) != (right->group == SIZE_MAX) ||
		strcmp(left->path, right->path))
		return 0;
	size_t x, y;
	const lx_entry *before = lx_destination_entries(a, left, &x);
	const lx_entry *after = lx_destination_entries(b, right, &y);

	if (x != y)
		return 0;
	for (size_t i = 0; i < x; ++i)
		if (strcmp(before[i].fingerprint, after[i].fingerprint))
			return 0;
	return 1;
}

launcher_status lx_check_destination(launcher_handle *handle,
	const launcher_release *release, const lx_destination *destination,
	const char *prefix, launcher_progress_fn callback, void *state,
	launcher_error *error)
{
	if (!lx_progress(callback, state, LAUNCHER_VERIFY, destination->path,
		0, 1, 0, 0))
		return lx_fail(error, LAUNCHER_CANCELLED,
			"Destination verification cancelled");
	launcher_release view = {};
	lx_group group = {};
	size_t count = 0;
	const lx_entry *entries = lx_destination_entries(release, destination,
		&count);

	if (destination->group == SIZE_MAX) {
		view.loose_files = (lx_entry *)entries;
		view.loose_count = count;
	} else {
		group = release->groups[destination->group];
		group.first = 0;
		view.groups = &group;
		view.group_count = 1;
		view.hogg_files = (lx_entry *)entries;
		view.hogg_count = count;
	}
	return lx_check_release(handle, &view, prefix, prefix, callback, state,
		error);
}

static int object_compare(const void *left, const void *right)
{
	return strcmp((*(const lx_object *const *)left)->digest,
		(*(const lx_object *const *)right)->digest);
}

static lx_object *find_digest(lx_object **objects, size_t count,
	const char *digest, uint64_t size)
{
	size_t begin = 0, end = count;

	while (begin < end) {
		size_t middle = begin + (end - begin) / 2;
		int delta = strcmp(digest, objects[middle]->digest);

		if (!delta)
			return objects[middle]->size == size ?
				objects[middle] : NULL;
		if (delta < 0)
			end = middle;
		else
			begin = middle + 1;
	}
	return NULL;
}

static lx_object *find_object(lx_object **objects, size_t count,
	const lx_entry *entry)
{
	return find_digest(objects, count, entry->digest, entry->stored_size);
}

static void object_paths(const char *digest, char final[80], char part[85])
{
	snprintf(final, 80, "objects/%s", digest);
	snprintf(part, 85, "objects/%s.part", digest);
}

struct reuse_candidate {
	const lx_entry *entry;
	lx_object *object;
	int header;
};

static int candidate_compare(const void *a, const void *b)
{
	const reuse_candidate *left = (const reuse_candidate *)a;
	const reuse_candidate *right = (const reuse_candidate *)b;

	if (left->entry->kind != right->entry->kind)
		return left->entry->kind < right->entry->kind ? -1 : 1;
	int order = strcmp(left->entry->path, right->entry->path);

	if (!order)
		order = strcmp(left->object->digest, right->object->digest);
	return order ? order : left->header - right->header;
}

static int reuse_path_compare(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static launcher_status reuse_candidates(const launcher_release *target,
	const launcher_release *installed, lx_object **objects, size_t count,
	reuse_candidate **out, size_t *out_count, launcher_error *error)
{
	const launcher_release *sources[2] = {target, installed};
	size_t capacity = 0;

	for (size_t i = 0; i < 2; ++i)
		if (sources[i])
			capacity += sources[i]->hogg_count * 2 +
				sources[i]->loose_count;
	reuse_candidate *values = (reuse_candidate *)calloc(
		capacity ? capacity : 1, sizeof(*values));

	if (!values)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Selecting installed object candidates");
	size_t used = 0;

	for (size_t i = 0; i < 2; ++i) {
		const launcher_release *release = sources[i];

		for (size_t j = 0; release &&
			j < release->hogg_count + release->loose_count; ++j) {
			const lx_entry *entry = j < release->hogg_count ?
				release->hogg_files + j : release->loose_files +
				j - release->hogg_count;
			lx_object *object = find_object(objects, count, entry);

			if (object && !object->available)
				values[used++] = {entry, object, 0};
			if (entry->kind != LAUNCHER_HOGG ||
				!entry->cached_header_sha256[0])
				continue;
			object = find_digest(objects, count,
				entry->cached_header_sha256,
				entry->cached_header_size);
			if (object && !object->available)
				values[used++] = {entry, object, 1};
		}
	}
	qsort(values, used, sizeof(*values), candidate_compare);
	*out_count = 0;
	for (size_t i = 0; i < used; ++i)
		if (!*out_count || candidate_compare(values + i,
			values + *out_count - 1))
			values[(*out_count)++] = values[i];
	*out = values;
	return LAUNCHER_OK;
}

static launcher_status reuse_loose(launcher_handle *handle,
	const reuse_candidate *candidates, size_t count,
	launcher_progress_fn callback, void *state, launcher_error *error)
{
	for (size_t i = 0; i < count; i++) {
		const lx_entry *entry = candidates[i].entry;
		lx_object *object = candidates[i].object;

		if (entry->kind != LAUNCHER_LOOSE || object->available)
			continue;
		if (!lx_progress(callback, state, LAUNCHER_VERIFY, entry->path,
			i, count, 0, 0))
			return lx_fail(error, LAUNCHER_CANCELLED,
				"Reuse cancelled");
		int kind = 0;
		uint64_t size = 0;

		if (lx_fs_stat(handle->install, entry->path, &kind, &size,
			NULL) != LAUNCHER_OK || kind != 1 ||
			size != object->size)
			continue;
		char final[80], part[85];

		object_paths(object->digest, final, part);
		launcher_status status = lx_fs_remove(handle->private_fs,
			part, 0, error);

		if (status != LAUNCHER_OK)
			return status;
		status = lx_copy_object(handle->install, entry->path,
			handle->private_fs, part, object->size,
			object->digest, NULL);
		if (status != LAUNCHER_OK)
			continue;
		status = lx_fs_replace(handle->private_fs, part,
			handle->private_fs, final, error);
		if (status != LAUNCHER_OK)
			return status;
		object->available = 1;
	}
	return LAUNCHER_OK;
}

static launcher_status reuse_header_object(launcher_handle *handle,
	pg_source *source, const lx_entry *entry, lx_object *object,
	launcher_error *error)
{
	if (!object || object->available)
		return LAUNCHER_OK;
	pg_file *file = NULL;

	if (pg_source_find(source, entry->path, &file, NULL) != PG_OK)
		return LAUNCHER_OK;
	pg_file_info info = {};
	launcher_status status = LAUNCHER_OK;

	if (pg_file_inspect(file, &info, NULL) == PG_OK &&
		info.cached_header_size == object->size) {
		char digest[65];

		status = lx_digest_bytes(info.cached_header,
			info.cached_header_size, digest, error);
		if (status == LAUNCHER_OK && !strcmp(digest, object->digest)) {
			char path[80];

			snprintf(path, sizeof(path), "objects/%s",
				object->digest);
			status = lx_save_bytes(handle->private_fs, path,
				info.cached_header,
				info.cached_header_size, error);
			if (status == LAUNCHER_OK)
				object->available = 1;
		}
	}
	pg_status pg = pg_file_close(&file, NULL);

	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(pg, NULL,
			"Closing reused header", error);
	return status;
}

static launcher_status reuse_archive_entries(launcher_handle *handle,
	pg_source *source, const reuse_candidate *candidates, size_t count,
	launcher_progress_fn callback, void *state, launcher_error *error)
{
	for (size_t i = 0; i < count; i++) {
		const lx_entry *entry = candidates[i].entry;
		lx_object *object = candidates[i].object;

		if (entry->kind != LAUNCHER_HOGG || object->available)
			continue;
		if (!lx_progress(callback, state, LAUNCHER_VERIFY, entry->path,
			i, count, 0, 0))
			return lx_fail(error, LAUNCHER_CANCELLED,
				"Reuse cancelled");
		if (candidates[i].header) {
			launcher_status status = reuse_header_object(handle,
				source, entry, object, error);

			if (status != LAUNCHER_OK)
				return status;
			continue;
		}
		pg_file *file = NULL;

		if (pg_source_find(source, entry->path, &file, NULL) != PG_OK)
			continue;
		pg_file_info info = {};
		pg_error detail = {};
		pg_status inspected = pg_file_inspect(file, &info, &detail);

		if (inspected != PG_OK || info.stored_size != object->size) {
			pg_file_close(&file, NULL);
			continue;
		}
		char final[80], part[85];

		object_paths(object->digest, final, part);
		launcher_status status = lx_fs_remove(handle->private_fs,
			part, 0, error);
		FILE *output = NULL;

		if (status == LAUNCHER_OK)
			status = lx_fs_write(handle->private_fs, part, 1,
				&output, error);
		if (status != LAUNCHER_OK) {
			pg_file_close(&file, NULL);
			return status;
		}
		status = hash_stored(file, entry, output, callback, state,
			error);
		if (status == LAUNCHER_OK)
			status = lx_file_flush(output, error);
		if (fclose(output) && status == LAUNCHER_OK)
			status = lx_fail(error, LAUNCHER_IO,
				"Closing reused object");
		pg_status pg = pg_file_close(&file, NULL);

		if (status == LAUNCHER_OK && pg != PG_OK)
			status = piggle_failure(pg, NULL,
				"Closing reused entry", error);
		if (status == LAUNCHER_OK)
			status = lx_fs_replace(handle->private_fs, part,
				handle->private_fs, final, error);
		if (status == LAUNCHER_OK)
			object->available = 1;
		else
			lx_fs_remove(handle->private_fs, part, 0, NULL);
		if (status == LAUNCHER_CANCELLED || status == LAUNCHER_MEMORY)
			return status;
	}
	return LAUNCHER_OK;
}

static int cache_complete(const launcher_plan *plan)
{
	for (size_t i = 0; i < plan->object_count; i++)
		if (!plan->objects[i].available)
			return 0;
	return 1;
}

launcher_status lx_cache_installed(launcher_handle *handle,
	launcher_plan *plan, launcher_progress_fn callback, void *state,
	launcher_error *error)
{
	launcher_status status = lx_fs_mkdirs(handle->private_fs,
		"objects", error);

	if (status != LAUNCHER_OK)
		return status;
	lx_object **sorted = (lx_object **)calloc(plan->object_count ?
		plan->object_count : 1, sizeof(*sorted));

	if (!sorted)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Sorting object inventory");
	for (size_t i = 0; i < plan->object_count; i++) {
		lx_object *object = plan->objects + i;
		char path[80];

		sorted[i] = object;
		snprintf(path, sizeof(path), "objects/%s",
				object->digest);
		if (!lx_progress(callback, state, LAUNCHER_VERIFY, path,
			i, plan->object_count, 0, 0)) {
			status = lx_fail(error, LAUNCHER_CANCELLED,
				"Cache verification cancelled");
			break;
		}
		int kind = 0;
		uint64_t size = 0;

		object->available = 0;
		status = lx_fs_stat(handle->private_fs, path,
			&kind, &size, error);
		if (status != LAUNCHER_OK)
			break;
		if (!kind)
			continue;
		if (kind != 1) {
			status = lx_fail(error, LAUNCHER_CONFLICT,
				"Cache object is not a regular file");
			break;
		}
		if (size == object->size) {
			object->available = 1;
		} else {
			status = lx_fs_remove(handle->private_fs, path,
				0, error);
			if (status != LAUNCHER_OK)
				break;
			++handle->epoch;
		}
	}
	if (status != LAUNCHER_OK) {
		free(sorted);
		return status;
	}
	if (cache_complete(plan)) {
		free(sorted);
		return LAUNCHER_OK;
	}
	qsort(sorted, plan->object_count, sizeof(*sorted), object_compare);
	reuse_candidate *candidates = NULL;
	size_t candidate_count = 0;

	status = reuse_candidates(plan->target, handle->installed, sorted,
		plan->object_count, &candidates, &candidate_count, error);
	if (status == LAUNCHER_OK)
		status = reuse_loose(handle, candidates, candidate_count,
			callback, state, error);
	const launcher_release *sources[2] = {plan->target,
		handle->installed};
	size_t capacity = plan->target->group_count +
		(handle->installed ? handle->installed->group_count : 0);
	const char **paths = NULL;
	size_t path_count = 0;

	if (status == LAUNCHER_OK) {
		paths = (const char **)calloc(capacity ? capacity : 1,
			sizeof(*paths));
		if (!paths)
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Selecting installed archive candidates");
	}
	for (size_t source = 0; source < 2 && status == LAUNCHER_OK;
		++source) {
		const launcher_release *release = sources[source];

		for (size_t i = 0; release && i < release->group_count; ++i)
			if (release->groups[i].kind == LAUNCHER_HOGG)
				paths[path_count++] = release->groups[i].path;
	}
	if (status == LAUNCHER_OK)
		qsort(paths, path_count, sizeof(*paths), reuse_path_compare);
	for (size_t i = 0; i < path_count && status == LAUNCHER_OK &&
		!cache_complete(plan); ++i) {
		if (i && !strcmp(paths[i - 1], paths[i]))
			continue;
		archive_view view = {};

		if (open_archive(handle->install, paths[i], &view, NULL) !=
			LAUNCHER_OK)
			continue;
		status = reuse_archive_entries(handle, view.source, candidates,
			candidate_count, callback, state, error);
		status = close_archive(&view, status, error);
	}

	free(paths);
	free(candidates);
	free(sorted);
	if (status == LAUNCHER_OK)
		lx_clear(error);
	return status;
}

static launcher_status stage_entry(launcher_handle *handle,
	pg_archive_builder *builder, const lx_entry *entry,
	launcher_progress_fn callback, void *state, launcher_error *error,
	pg_source *source = NULL)
{
	char path[80];

	snprintf(path, sizeof(path), "objects/%s", entry->digest);
	FILE *file = NULL;
	launcher_status status = lx_fs_read(handle->private_fs, path,
		&file, error);

	if (status != LAUNCHER_OK)
		return status;
	char *header = NULL;
	size_t header_size = 0;

	if (entry->cached_header_sha256[0]) {
		snprintf(path, sizeof(path), "objects/%s",
			entry->cached_header_sha256);
		status = lx_read_bytes(handle->private_fs, path,
			entry->cached_header_size, &header,
			&header_size, error);
		if (status == LAUNCHER_OK &&
			header_size != entry->cached_header_size)
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Cached header object length differs");
	}
	pg_stored_write_options options = {};

	options.logical_size = entry->size;
	options.stored_size = entry->stored_size;
	options.encoding = entry->encoding == LAUNCHER_ZLIB ?
		PG_ZLIB : PG_LOGICAL;
	options.digest_kind = PG_DIGEST_MD5;
	options.checksum_domain = PG_CHECKSUM_LOGICAL;
	options.cached_header = header ? (const void *)header :
		entry->cached_header;
	options.cached_header_size = entry->cached_header_size;
	for (size_t i = 0; i < 16; i++)
		options.digest[i] =
			(uint8_t)hex_byte(entry->logical_md5 + 2 * i);
	pg_writer *writer = NULL;
	pg_error detail = {};
	pg_status pg = PG_OK;

	if (status == LAUNCHER_OK && source) {
		pg_write_options checked = {};

		checked.logical_size = entry->size;
		checked.input_size = entry->stored_size;
		checked.encoding = options.encoding;
		checked.entry.compression = PG_COMPRESS_NEVER;
		checked.entry.digest_kind = PG_DIGEST_MD5;
		checked.entry.expected_digest_domain = PG_CHECKSUM_LOGICAL;
		memcpy(checked.entry.expected_digest, options.digest, 16);
		checked.entry.cached_header = options.cached_header;
		checked.entry.cached_header_size = options.cached_header_size;
		pg = pg_writer_open_source(source, entry->path, &checked,
			&writer, &detail);
	} else if (status == LAUNCHER_OK)
		pg = pg_writer_open_archive_builder_stored(builder,
			entry->path, &options, &writer, &detail);

	if (pg != PG_OK)
		status = piggle_failure(pg, &detail,
			"Staging stored archive entry", error);
	uint64_t count = 0;
	unsigned char chunk[chunk_size];

	while (status == LAUNCHER_OK) {
		size_t got = fread(chunk, 1, sizeof(chunk), file);

		if (got > entry->stored_size - count) {
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Cached object exceeds stored length");
			break;
		}
		count += got;
		size_t written = 0;

		pg = pg_writer_write(writer, chunk, got, &written, &detail);
		if (pg != PG_OK)
			status = piggle_failure(pg, &detail,
				"Writing stored archive entry", error);
		if (!lx_progress(callback, state, LAUNCHER_STAGE, entry->path,
			0, 1, 0, 0))
			status = lx_fail(error, LAUNCHER_CANCELLED,
				"Archive staging cancelled");
		if (got < sizeof(chunk)) {
			if (ferror(file))
				status = lx_fail(error, LAUNCHER_IO,
					"Reading cached archive object");
			break;
		}
	}
	if (status == LAUNCHER_OK && count != entry->stored_size)
		status = lx_fail(error, LAUNCHER_INTEGRITY,
			"Cached archive object stored length differs");
	if (fclose(file) && status == LAUNCHER_OK)
		status = lx_fail(error, LAUNCHER_IO, "Closing cached object");
	if (status == LAUNCHER_OK) {
		pg = pg_writer_finish(writer, &detail);
		if (pg != PG_OK)
			status = piggle_failure(pg, &detail,
				"Finishing stored archive entry", error);
	}
	pg = pg_writer_close(&writer, &detail);
	if (pg != PG_OK && status == LAUNCHER_OK)
		status = piggle_failure(pg, &detail,
			"Closing entry writer", error);
	if (status == LAUNCHER_OK && !lx_progress(callback, state,
		LAUNCHER_STAGE, entry->path, 1, 1, 0, 0))
		status = lx_fail(error, LAUNCHER_CANCELLED,
			"Archive staging cancelled");
	free(header);
	return status;
}

launcher_status lx_stage_hogg(launcher_handle *handle,
	const launcher_release *release, const char *dest,
	launcher_progress_fn callback, void *state, launcher_error *error)
{
	lx_fs *parent = NULL;
	char *absolute = NULL;
	launcher_status status = lx_fs_pin(handle->private_fs, dest, 1,
		&parent, &absolute, error);
	pg_context *context = NULL;
	pg_archive_builder *builder = NULL;
	pg_error detail = {};
	pg_status pg = PG_OK;

	if (status == LAUNCHER_OK)
		status = lx_fs_validate(handle->private_fs, error);
	if (status == LAUNCHER_OK)
		status = lx_fs_validate(parent, error);
	if (status == LAUNCHER_OK)
		pg = pg_context_open(&context, &detail);
	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(pg, &detail,
			"Creating builder context", error);
	pg_archive_options options = { PG_HOGG10, 0, PG_CHECKSUM_LOGICAL };

	if (status == LAUNCHER_OK)
		pg = pg_archive_builder_create_options(context, absolute,
			&options, &builder, &detail);
	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(pg, &detail,
			"Creating HOGG10 builder", error);
	for (size_t i = 0; status == LAUNCHER_OK &&
		i < release->hogg_count; i++) {
		if (!lx_progress(callback, state, LAUNCHER_STAGE,
			release->hogg_files[i].path, i, release->hogg_count,
			0, 0)) {
			status = lx_fail(error, LAUNCHER_CANCELLED,
				"Archive staging cancelled");
			break;
		}
		status = stage_entry(handle, builder, release->hogg_files + i,
			callback, state, error);
	}
	if (status == LAUNCHER_OK)
		status = lx_fs_validate(handle->private_fs, error);
	if (status == LAUNCHER_OK)
		status = lx_fs_validate(parent, error);
	if (status == LAUNCHER_OK) {
		pg = pg_archive_builder_finish(builder, &detail);
		if (pg != PG_OK && pg != PG_COMMITTED)
			status = piggle_failure(pg, &detail,
				"Finishing staged HOGG10", error);
	}
	pg = pg_archive_builder_close(&builder, &detail);
	if (pg != PG_OK && status == LAUNCHER_OK)
		status = piggle_failure(pg, &detail,
			"Closing archive builder", error);
	pg = pg_context_close(&context, &detail);
	if (pg != PG_OK && status == LAUNCHER_OK)
		status = piggle_failure(pg, &detail,
			"Closing builder context", error);
	if (status == LAUNCHER_OK)
		status = lx_fs_validate(handle->private_fs, error);
	if (status == LAUNCHER_OK)
		status = lx_fs_validate(parent, error);
	lx_fs_close(&parent);
	free(absolute);
	if (status != LAUNCHER_OK)
		lx_fs_remove(handle->private_fs, dest, 0, NULL);
	return status;
}

struct cache_verification {
	launcher_handle *handle;
	const lx_entry **entries;
	size_t count;
	launcher_progress_fn callback;
	void *state;
	launcher_status result;
	launcher_error error;
};

static int cache_digest_compare(const void *a, const void *b)
{
	return strcmp((*(const lx_entry *const *)a)->digest,
		(*(const lx_entry *const *)b)->digest);
}

static int verify_cache_object(const char *name, int kind, void *opaque)
{
	cache_verification *scan = (cache_verification *)opaque;

	/* Only final digest names denote accepted cache objects. */
	if (strlen(name) != 64)
		return 1;
	for (size_t i = 0; i < 64; i++)
		if (!((name[i] >= '0' && name[i] <= '9') ||
			(name[i] >= 'a' && name[i] <= 'f')))
			return 1;
	char path[80];

	snprintf(path, sizeof(path), "objects/%s", name);
	launcher_error detail = {};
	launcher_status status = LAUNCHER_OK;

	if (!lx_progress(scan->callback, scan->state, LAUNCHER_VERIFY, path,
		0, 1, 0, 0)) {
		scan->result = lx_fail(&scan->error, LAUNCHER_CANCELLED,
			"Cache verification cancelled");
		return 0;
	}
	uint64_t size = 0;
	int actual_kind = 0;

	status = lx_fs_stat(scan->handle->private_fs, path, &actual_kind,
		&size, &detail);
	if (status == LAUNCHER_OK && (kind != 1 || actual_kind != 1))
		status = lx_fail(&detail, LAUNCHER_CONFLICT,
			"Cache object is not a regular file");
	uint64_t expected = size;
	size_t begin = 0, end = scan->count;

	while (begin < end) {
		size_t middle = begin + (end - begin) / 2;
		int delta = strcmp(name, scan->entries[middle]->digest);

		if (!delta) {
			expected = scan->entries[middle]->stored_size;
			break;
		}
		if (delta < 0)
			end = middle;
		else
			begin = middle + 1;
	}
	if (status == LAUNCHER_OK && size != expected)
		status = lx_fail(&detail, LAUNCHER_INTEGRITY,
			"Cache object stored length differs: %s", name);
	FILE *file = NULL;

	if (status == LAUNCHER_OK)
		status = lx_fs_read(scan->handle->private_fs, path,
			&file, &detail);
	if (status == LAUNCHER_OK)
		status = lx_hash_file(file, expected, name, NULL, &detail);
	if (file && fclose(file) && status == LAUNCHER_OK)
		status = lx_fail(&detail, LAUNCHER_IO,
			"Closing verified cache object");
	if (status == LAUNCHER_INTEGRITY) {
		launcher_status removed = lx_fs_remove(scan->handle->private_fs,
			path, 0, NULL);

		if (removed == LAUNCHER_OK)
			++scan->handle->epoch;
	}
	if (status != LAUNCHER_OK && scan->result == LAUNCHER_OK) {
		scan->result = status;
		scan->error = detail;
	}
	return 1;
}

launcher_status lx_verify_cache(launcher_handle *handle,
	launcher_progress_fn callback, void *state, launcher_error *error)
{
	int kind = 0;
	uint64_t size = 0;
	launcher_status status = lx_fs_stat(handle->private_fs, "objects",
		&kind, &size, error);

	if (status != LAUNCHER_OK || !kind)
		return status;
	if (kind != 2)
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Cache directory is not a directory");
	cache_verification scan = {};

	scan.handle = handle;
	scan.callback = callback;
	scan.state = state;
	size_t hogg_count = handle->installed->hogg_count;

	scan.count = hogg_count + handle->installed->loose_count;
	scan.entries = (const lx_entry **)calloc(scan.count ? scan.count : 1,
		sizeof(*scan.entries));
	if (!scan.entries)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Sorting cache references");
	for (size_t i = 0; i < scan.count; i++)
		scan.entries[i] = i < hogg_count ?
			handle->installed->hogg_files + i :
			handle->installed->loose_files + i - hogg_count;
	qsort(scan.entries, scan.count, sizeof(*scan.entries),
		cache_digest_compare);
	status = lx_fs_list(handle->private_fs, "objects", verify_cache_object,
		&scan, error);
	free(scan.entries);
	if (scan.result != LAUNCHER_OK) {
		if (error)
			*error = scan.error;
		return scan.result;
	}
	return status;
}

const lx_entry *lx_find_entry(const launcher_release *release,
	const char *name)
{
	if (!release)
		return NULL;
	size_t low = 0, high = release->hogg_count;

	while (low < high) {
		size_t middle = low + (high - low) / 2;
		int order = strcmp(release->hogg_files[middle].path, name);

		if (!order)
			return release->hogg_files + middle;
		if (order < 0)
			low = middle + 1;
		else
			high = middle;
	}
	return NULL;
}

int lx_same_payload(const lx_entry *a, const lx_entry *b)
{
	return a && b && a->size == b->size &&
	       a->stored_size == b->stored_size && a->encoding == b->encoding &&
	       !strcmp(a->digest, b->digest) &&
	       !strcmp(a->logical_md5, b->logical_md5);
}

static launcher_status patch_header(launcher_handle *handle, pg_source *source,
	const lx_entry *entry, launcher_error *error)
{
	char *header = NULL;
	size_t size = entry->cached_header_size;
	launcher_status status = LAUNCHER_OK;

	if (entry->cached_header_sha256[0]) {
		char path[80];

		snprintf(path, sizeof(path), "objects/%s",
			entry->cached_header_sha256);
		status = lx_read_bytes(handle->private_fs, path, size, &header,
			&size, error);
		if (status == LAUNCHER_OK && size != entry->cached_header_size)
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Cached header length differs");
	}
	pg_file *file = NULL;
	pg_error detail = {};
	pg_status pg = PG_OK;

	if (status == LAUNCHER_OK)
		pg = pg_source_find(source, entry->path, &file, &detail);
	if (status == LAUNCHER_OK && pg == PG_OK) {
		pg_metadata_options options = {PG_METADATA_HEADER, 0,
			header ? (const void *)header : entry->cached_header,
			size};

		pg = pg_file_update_metadata(file, &options, &detail);
	}
	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(
			pg, &detail, "Updating cached header", error);
	pg = pg_file_close(&file, &detail);
	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(
			pg, &detail, "Closing updated header", error);
	free(header);
	return status;
}

launcher_status lx_patch_hogg(launcher_handle *handle,
	const launcher_release *before, const launcher_release *target,
	launcher_progress_fn callback, void *state, launcher_error *error)
{
	/* Replay the frozen difference. Repeated replacements are safe after
	 * an interruption between archive commitment and outer state saving.
	 */
	archive_view view = {};
	launcher_status status = open_archive_mode(
		handle->install, target->hogg, &view, 1, error);
	uint64_t completed = 0;
	uint64_t total = before->hogg_count + target->hogg_count;

	for (size_t i = 0; status == LAUNCHER_OK && i < before->hogg_count;
		i++) {
		const lx_entry *old = before->hogg_files + i;

		if (lx_find_entry(target, old->path))
			continue;
		pg_file *file = NULL;
		pg_error detail = {};
		pg_status pg =
			pg_source_find(view.source, old->path, &file, &detail);

		if (pg == PG_OK)
			pg = pg_file_delete(file, &detail);
		if (pg != PG_OK && pg != PG_NOT_FOUND)
			status = piggle_failure(
				pg, &detail, "Removing archive entry", error);
		pg = pg_file_close(&file, &detail);
		if (status == LAUNCHER_OK && pg != PG_OK)
			status = piggle_failure(
				pg, &detail, "Closing removed entry", error);
		if (status == LAUNCHER_OK && !lx_progress(callback, state,
			LAUNCHER_ACTIVATE, old->path, ++completed, total, 0, 0))
			status = lx_fail(error, LAUNCHER_CANCELLED,
				"Update paused; reopen to finish the selected "
				"release");
	}
	for (size_t i = 0; status == LAUNCHER_OK && i < target->hogg_count;
		i++) {
		const lx_entry *entry = target->hogg_files + i;
		const lx_entry *old = lx_find_entry(before, entry->path);

		if (old && !strcmp(old->fingerprint, entry->fingerprint))
			continue;
		if (lx_same_payload(old, entry))
			status =
				patch_header(handle, view.source, entry, error);
		else
			status = stage_entry(handle, NULL, entry, NULL, NULL,
				error, view.source);
		if (status == LAUNCHER_OK &&
			!lx_progress(callback, state, LAUNCHER_ACTIVATE,
				entry->path, ++completed, total, 0, 0))
			status = lx_fail(error, LAUNCHER_CANCELLED,
				"Update paused; reopen to finish the selected "
				"release");
	}
	return close_archive(&view, status, error);
}

launcher_status lx_compact_hogg(launcher_handle *handle, const char *path,
	launcher_error *error)
{
	archive_view view = {};
	launcher_status status = open_archive_mode(handle->install, path,
		&view, 1, error);
	pg_source_storage_info storage = {};
	pg_error detail = {};
	pg_status pg = PG_OK;

	if (status == LAUNCHER_OK)
		pg = pg_source_storage_inspect(view.source, &storage, &detail);
	if (status == LAUNCHER_OK && pg != PG_OK)
		status = piggle_failure(
			pg, &detail, "Inspecting reclaimable space", error);
	int needed = status == LAUNCHER_OK && storage.physical_length &&
		storage.reclaimable_bytes >= storage.physical_length / 5 +
			(storage.physical_length % 5 != 0);
	char *temporary = NULL;
	lx_fs *parent = NULL;

	if (needed) {
		status = lx_fs_remove(
			handle->private_fs, "compact.hogg", 0, error);
		if (status == LAUNCHER_OK)
			status = lx_fs_pin(handle->private_fs, "compact.hogg",
				0, &parent, &temporary, error);
		if (status == LAUNCHER_OK)
			pg = pg_source_compact(
				view.source, temporary, NULL, &detail);
		if (status == LAUNCHER_OK && pg != PG_OK) {
			if (pg == PG_COMMITTED || pg == PG_INDETERMINATE)
				status = lx_fail(error, LAUNCHER_IO,
					"Compacted output needs private "
					"cleanup");
			else
				status = piggle_failure(pg, &detail,
					"Reclaiming archive space", error);
		}
	}
	status = close_archive(&view, status, error);
	if (status == LAUNCHER_OK && needed) {
		status = lx_fs_replace(handle->private_fs, "compact.hogg",
			handle->install, path, error);
		if (status != LAUNCHER_OK) {
			launcher_error replacement =
				error ? *error : launcher_error{};
			launcher_status checked = LAUNCHER_INTEGRITY;

			for (size_t i = 0; handle->installed &&
				i < handle->installed->group_count; i++) {
				const lx_group *group =
					handle->installed->groups + i;

				if (group->kind != LAUNCHER_HOGG ||
					strcmp(group->path, path))
					continue;
				launcher_release target = {};

				lx_group_view(handle->installed, i, &target);
				checked = verify_archive(handle->install, path,
					&target, NULL, NULL, 0, error);
				break;
			}
			if (checked != LAUNCHER_OK)
				status = checked;
			else if (error)
				*error = replacement;
		}
	}
	/* A late flush error may follow a successful rename. Both complete
	 * archives describe the same installed target; never remove path.
	 */
	lx_fs_close(&parent);
	free(temporary);
	if (status != LAUNCHER_OK)
		lx_fs_remove(handle->private_fs, "compact.hogg", 0, NULL);
	return status;
}
