#include "internal.h"
#include <yyjson.h>
#include <aws/cal/hash.h>
#include <aws/common/error.h>
#include <aws/common/encoding.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static constexpr size_t metadata_limit = 16 * 1024 * 1024;
static constexpr size_t entry_limit = 100000;

launcher_status lx_fail(launcher_error *error, launcher_status status,
	const char *format, ...)
{
	if (error) {
		memset(error, 0, sizeof(*error));
		error->status = status;
		va_list args;

		va_start(args, format);
		vsnprintf(error->message, sizeof(error->message), format, args);
		va_end(args);
	}
	return status;
}

void lx_clear(launcher_error *error)
{
	if (error)
		memset(error, 0, sizeof(*error));
}

char *lx_strdup(const char *text)
{
	if (!text)
		return NULL;
	size_t length = strlen(text) + 1;
	char *copy = (char *)malloc(length);

	if (copy)
		memcpy(copy, text, length);
	return copy;
}

static unsigned char folded(unsigned char value)
{
	return value >= 'A' && value <= 'Z' ? value + ('a' - 'A') : value;
}

static int device_name(const char *text, size_t length)
{
	char stem[5] = {};
	size_t count = 0;

	while (count < length && text[count] != '.')
		count++;
	while (count && text[count - 1] == ' ')
		count--;
	if (count > 4)
		return 0;
	for (size_t i = 0; i < count; i++)
		stem[i] = (char)folded((unsigned char)text[i]);
	if (!strcmp(stem, "con") || !strcmp(stem, "prn") ||
		!strcmp(stem, "aux") || !strcmp(stem, "nul"))
		return 1;
	return count == 4 && stem[3] >= '1' && stem[3] <= '9' &&
		(!memcmp(stem, "com", 3) || !memcmp(stem, "lpt", 3));
}

int lx_identifier(const char *text)
{
	if (!text)
		return 0;
	size_t length = strlen(text);

	if (!length || length > 64 || device_name(text, length))
		return 0;
	for (size_t i = 0; i < length; i++) {
		char value = text[i];

		if ((value >= 'a' && value <= 'z') ||
			(value >= '0' && value <= '9'))
			continue;
		if (i && (value == '_' || value == '-'))
			continue;
		return 0;
	}
	return 1;
}

int lx_path_valid(const char *text, int lowercase)
{
	if (!text)
		return 0;
	size_t length = strlen(text);

	if (!length || length > 1024)
		return 0;
	size_t begin = 0;

	for (size_t i = 0; i <= length; i++) {
		unsigned char value = (unsigned char)text[i];

		if (!value || value == '/') {
			size_t count = i - begin;

			if (!count || count > 255 || text[i - 1] == '.' ||
				text[i - 1] == ' ' ||
				device_name(text + begin, count))
				return 0;
			begin = i + 1;
			continue;
		}
		if (value < 32 || value == 127 || value == '\\' ||
			strchr("<>:\"|?*", value) ||
			(lowercase && value >= 'A' && value <= 'Z'))
			return 0;
	}
	return 1;
}

int lx_progress(launcher_progress_fn callback, void *state,
	launcher_phase phase, const char *path, uint64_t done, uint64_t total,
	size_t objects_done, size_t objects_total)
{
	if (!callback)
		return 1;
	launcher_progress progress = {
		phase, path, done, total, objects_done, objects_total
	};

	return callback(&progress, state) != 0;
}

static const char *json_string(yyjson_val *value)
{
	const char *text = yyjson_get_str(value);

	if (!text || strlen(text) != yyjson_get_len(value))
		return NULL;
	return text;
}

static int fields(yyjson_val *object, const char *const *names,
	size_t count)
{
	if (!yyjson_is_obj(object) || yyjson_obj_size(object) != count)
		return 0;
	unsigned seen = 0;
	size_t index, maximum;
	yyjson_val *key, *value;

	yyjson_obj_foreach(object, index, maximum, key, value) {
		const char *name = json_string(key);
		size_t position = 0;

		if (!name)
			return 0;
		while (position < count && strcmp(name, names[position]))
			position++;
		if (position == count || (seen & (1U << position)))
			return 0;
		seen |= 1U << position;
	}
	return seen == (1U << count) - 1;
}

static int format_one(yyjson_val *object)
{
	yyjson_val *format = yyjson_obj_get(object, "format");

	return yyjson_is_int(format) && yyjson_get_uint(format) == 1;
}

static launcher_status read_json(const void *data, size_t size,
	yyjson_doc **out, launcher_error *error)
{
	if (!data || !size || size > metadata_limit)
		return lx_fail(error, LAUNCHER_METADATA,
			"Metadata must contain 1 through 16777216 bytes");
	yyjson_read_err detail = {};

	*out = yyjson_read_opts((char *)data, size, 0, NULL, &detail);
	if (!*out)
		return lx_fail(error, detail.code ==
			YYJSON_READ_ERROR_MEMORY_ALLOCATION ? LAUNCHER_MEMORY :
			LAUNCHER_METADATA,
			"Invalid JSON at byte %zu: %s", detail.pos,
			detail.msg ? detail.msg : "parse failure");
	return LAUNCHER_OK;
}

static void clear_channel(launcher_release *release)
{
	for (size_t i = 0; release->run_arguments &&
		i < release->run_argument_count; ++i)
		free((void *)release->run_arguments[i]);
	free(release->run_arguments);
	free(release->entry_point);
	free(release->channel_json);
	release->run_arguments = NULL;
	release->run_argument_count = 0;
	release->entry_point = NULL;
	release->channel_json = NULL;
}

static launcher_status read_channel(const void *data, size_t size,
	const char *channel, char release[65], launcher_release *settings,
	launcher_error *error)
{
	lx_clear(error);
	if (release)
		release[0] = 0;
	if (!release || !lx_identifier(channel))
		return lx_fail(error, LAUNCHER_INVALID,
			"A canonical channel and release output are required");
	yyjson_doc *doc = NULL;
	launcher_status status = read_json(data, size, &doc, error);

	if (status != LAUNCHER_OK)
		return status;
	yyjson_val *root = yyjson_doc_get_root(doc);
	const char *names[] = { "format", "release" };
	const char *id = json_string(yyjson_obj_get(root, "release"));

	if (!fields(root, names, 2) || !format_one(root) ||
		!lx_identifier(id) || (settings && strcmp(settings->id, id))) {
		status = lx_fail(error, LAUNCHER_METADATA,
			"Invalid channel schema or release ID");
	} else if (settings) {
		settings->channel_json = (char *)malloc(size + 1);
		if (!settings->channel_json)
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Cannot retain channel snapshot");
		else {
			memcpy(settings->channel_json, data, size);
			settings->channel_json[size] = 0;
		}
	}
	if (status == LAUNCHER_OK)
		memcpy(release, id, strlen(id) + 1);
	yyjson_doc_free(doc);
	return status;
}

launcher_status launcher_decode_channel(const void *data, size_t size,
	const char *channel, char release[65], launcher_error *error)
{
	return read_channel(data, size, channel, release, NULL, error);
}

static int digest_valid(const char *text, size_t length = 64)
{
	if (!text || strlen(text) != length)
		return 0;
	for (size_t i = 0; i < length; i++)
		if (!((text[i] >= '0' && text[i] <= '9') ||
			(text[i] >= 'a' && text[i] <= 'f')))
			return 0;
	return 1;
}

static int group_order(const void *a, const void *b)
{
	return strcmp(((const lx_group *)a)->name, ((const lx_group *)b)->name);
}

static int entry_order(const void *a, const void *b)
{
	return strcmp(((const lx_entry *)a)->path, ((const lx_entry *)b)->path);
}

static launcher_status read_entries(yyjson_val *array, lx_entry *out,
	launcher_file_kind kind, launcher_error *error)
{
	size_t index, count;
	yyjson_val *value;

	yyjson_arr_foreach(array, index, count, value) {
		const char *fingerprint = json_string(value);

		if (!digest_valid(fingerprint))
			return lx_fail(error, LAUNCHER_METADATA,
				"Invalid file fingerprint at entry %zu", index);
		out[index].kind = kind;
		memcpy(out[index].fingerprint, fingerprint, 65);
	}
	return LAUNCHER_OK;
}

struct namespace_key {
	const lx_entry *entry;
	char *key;
};

static int namespace_compare(const void *left, const void *right)
{
	return strcmp(((const namespace_key *)left)->key,
		((const namespace_key *)right)->key);
}

static launcher_status validate_namespace(lx_entry **entries, size_t count,
	launcher_error *error)
{
	if (!count)
		return LAUNCHER_OK;
	size_t bytes = 0;

	for (size_t i = 0; i < count; ++i) {
		size_t length = strlen(entries[i]->path);

		if (length >= SIZE_MAX - bytes)
			return lx_fail(error, LAUNCHER_MEMORY,
				"Namespace keys exceed address space");
		bytes += length + 1;
	}
	namespace_key *keys = (namespace_key *)calloc(count, sizeof(*keys));
	char *storage = (char *)malloc(bytes);

	if (!keys || !storage) {
		free(keys);
		free(storage);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Building namespace keys");
	}
	char *at = storage;

	for (size_t i = 0; i < count; ++i) {
		const char *path = entries[i]->path;
		size_t length = strlen(path);

		keys[i] = {entries[i], at};
		/* Put descendants before punctuation after a file prefix. */
		for (size_t j = 0; j < length; ++j)
			at[j] = path[j] == '/' ? 1 :
				(char)folded((unsigned char)path[j]);
		at[length] = 0;
		at += length + 1;
	}
	qsort(keys, count, sizeof(*keys), namespace_compare);
	launcher_status status = LAUNCHER_OK;

	for (size_t i = 1; i < count; ++i) {
		const char *previous = keys[i - 1].key;
		const char *current = keys[i].key;
		size_t length = strlen(previous);

		if (strncmp(previous, current, length) ||
			(current[length] && current[length] != 1))
			continue;
		status = lx_fail(error, LAUNCHER_METADATA,
			"File destinations conflict: %s and %s",
			keys[i - 1].entry->path, keys[i].entry->path);
		break;
	}
	free(storage);
	free(keys);
	return status;
}

static int digest_compare(const void *left, const void *right)
{
	return strcmp((*(const lx_entry *const *)left)->digest,
		(*(const lx_entry *const *)right)->digest);
}

struct object_length {
	const char *digest;
	uint64_t size;
};

static int object_length_order(const void *a, const void *b)
{
	return strcmp(((const object_length *)a)->digest,
		((const object_length *)b)->digest);
}

static launcher_status validate_object_lengths(launcher_release *release,
	launcher_error *error)
{
	size_t entries = release->hogg_count + release->loose_count;
	size_t headers = 0;

	for (size_t i = 0; i < entries; ++i) {
		const lx_entry *entry = i < release->hogg_count ?
			release->hogg_files + i :
			release->loose_files + i - release->hogg_count;

		headers += !!entry->cached_header_sha256[0];
	}
	if (!headers)
		return LAUNCHER_OK;
	object_length *objects = (object_length *)calloc(
		entries + headers, sizeof(*objects));

	if (!objects)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Checking shared header objects");
	size_t count = 0;

	for (size_t i = 0; i < entries; ++i) {
		const lx_entry *entry = i < release->hogg_count ?
			release->hogg_files + i :
			release->loose_files + i - release->hogg_count;

		objects[count++] = {entry->digest, entry->stored_size};
		if (entry->cached_header_sha256[0])
			objects[count++] = {entry->cached_header_sha256,
				entry->cached_header_size};
	}
	qsort(objects, count, sizeof(*objects), object_length_order);
	launcher_status status = LAUNCHER_OK;

	for (size_t i = 1; i < count; ++i) {
		if (strcmp(objects[i - 1].digest, objects[i].digest) ||
			objects[i - 1].size == objects[i].size)
			continue;
		status = lx_fail(error, LAUNCHER_METADATA,
			"Conflicting shared object lengths");
		break;
	}
	free(objects);
	return status;
}

static launcher_status validate_inventory(launcher_release *release,
	int paths, launcher_error *error)
{
	size_t count = release->hogg_count + release->loose_count;
	lx_entry **sorted = (lx_entry **)calloc(count ? count : 1,
		sizeof(*sorted));

	if (!sorted)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Sorting file inventory");
	for (size_t i = 0; i < release->hogg_count; i++)
		sorted[i] = release->hogg_files + i;
	launcher_status status = LAUNCHER_OK;

	for (size_t i = 0; i < release->loose_count; i++)
		sorted[release->hogg_count + i] = release->loose_files + i;
	if (paths) {
		size_t physical_count = lx_physical_count(release);
		lx_entry *physical = (lx_entry *)calloc(
			physical_count + 1, sizeof(*physical));
		lx_entry **physical_ptrs = (lx_entry **)calloc(
			physical_count + 1, sizeof(*physical_ptrs));

		if (!physical || !physical_ptrs)
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Checking physical destinations");
		for (size_t i = 0; status == LAUNCHER_OK &&
			i < physical_count; ++i) {
			physical[i].path = (char *)lx_physical_path(release, i);
			physical_ptrs[i] = physical + i;
		}
		if (status == LAUNCHER_OK)
			status = validate_namespace(physical_ptrs,
				physical_count, error);
		free(physical_ptrs);
		free(physical);
	}
	if (status == LAUNCHER_OK && paths)
		status = validate_namespace(sorted, count, error);
	if (status == LAUNCHER_OK && !paths) {
		qsort(sorted, count, sizeof(*sorted), digest_compare);
		const lx_entry *representations[2] = {};

		for (size_t i = 0; i < count; i++) {
			const lx_entry *entry = sorted[i];

			if (!i || strcmp(sorted[i - 1]->digest,
				entry->digest)) {
				representations[0] = representations[1] = NULL;
			} else if (sorted[i - 1]->stored_size !=
				entry->stored_size) {
				status = lx_fail(error, LAUNCHER_METADATA,
					"Conflicting stored lengths");
				break;
			}
			if (!entry->logical_md5[0])
				continue;
			const lx_entry *previous =
				representations[entry->encoding];

			if (previous && (previous->size != entry->size ||
				strcmp(previous->logical_md5,
					entry->logical_md5))) {
				status = lx_fail(error, LAUNCHER_METADATA,
					"Conflicting logical metadata");
				break;
			}
			representations[entry->encoding] = entry;
		}
	}
	free(sorted);
	return status == LAUNCHER_OK && !paths ?
		validate_object_lengths(release, error) : status;
}

static int base64_digit(unsigned char value)
{
	static const signed char digits[256] = {
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
		52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
		-1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
		15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
		-1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
		41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
		-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	};

	return digits[value];
}

static launcher_status decode_cached_header(const char *text, size_t length,
	lx_entry *out, launcher_error *error)
{
	if (!length || length % 4)
		return lx_fail(error, LAUNCHER_METADATA,
			"Invalid cached-header base64");
	size_t padding = text[length - 1] == '=' ?
		1 + (text[length - 2] == '=') : 0;
	int last = padding == 2 ?
		base64_digit((unsigned char)text[length - 3]) :
		base64_digit((unsigned char)text[length - 2]);

	if (memchr(text, '=', length - padding) || (padding &&
		(last < 0 || (last & (padding == 2 ? 15 : 3)))))
		return lx_fail(error, LAUNCHER_METADATA,
			"Cached header requires canonical padded base64");
	size_t size = length / 4 * 3 - padding;
	unsigned char *bytes = (unsigned char *)malloc(size);

	if (!bytes)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Allocating cached-header bytes");
	aws_byte_cursor input = aws_byte_cursor_from_array(text, length);
	aws_byte_buf output = aws_byte_buf_from_empty_array(bytes, size);

	if (aws_base64_decode(&input, &output) || output.len != size) {
		free(bytes);
		return lx_fail(error, LAUNCHER_METADATA,
			"Cached header requires canonical padded base64");
	}
	out->cached_header = bytes;
	out->cached_header_size = size;
	return LAUNCHER_OK;
}

struct descriptor_input {
	const char *data;
	size_t size;
};

static int descriptor_literal(descriptor_input *input, const char *text)
{
	size_t size = strlen(text);

	if (input->size < size || memcmp(input->data, text, size))
		return 0;
	input->data += size;
	input->size -= size;
	return 1;
}

static int descriptor_hex(descriptor_input *input, char *out, size_t size)
{
	if (input->size < size)
		return 0;
	for (size_t i = 0; i < size; ++i) {
		char value = input->data[i];

		if (!((value >= '0' && value <= '9') ||
			(value >= 'a' && value <= 'f')))
			return 0;
	}
	memcpy(out, input->data, size);
	out[size] = 0;
	input->data += size;
	input->size -= size;
	return 1;
}

static int descriptor_integer(descriptor_input *input, uint64_t *out)
{
	if (!input->size || input->data[0] < '0' || input->data[0] > '9' ||
		(input->data[0] == '0' && input->size > 1 &&
			input->data[1] >= '0' && input->data[1] <= '9'))
		return 0;
	uint64_t value = 0;

	while (input->size && input->data[0] >= '0' && input->data[0] <= '9') {
		unsigned digit = (unsigned)(input->data[0] - '0');

		if (value > ((uint64_t)INT64_MAX - digit) / 10)
			return 0;
		value = value * 10 + digit;
		++input->data;
		--input->size;
	}
	*out = value;
	return 1;
}

launcher_status lx_digest_bytes(const void *data, size_t size, char hex[65],
	launcher_error *error)
{
	unsigned char hash[32];

	lx_crypto_initialize();
	aws_byte_cursor input = aws_byte_cursor_from_array(data, size);
	aws_byte_buf output = aws_byte_buf_from_empty_array(hash, sizeof(hash));

	if (aws_sha256_compute(aws_default_allocator(), &input, &output, 0) ||
		output.len != sizeof(hash))
		return lx_fail(error, aws_last_error() == AWS_ERROR_OOM ?
			LAUNCHER_MEMORY : LAUNCHER_INTEGRITY,
			"Cannot compute SHA-256");
	static const char digits[] = "0123456789abcdef";

	for (size_t i = 0; i < 32; ++i) {
		hex[2 * i] = digits[hash[i] >> 4];
		hex[2 * i + 1] = digits[hash[i] & 15];
	}
	hex[64] = 0;
	return LAUNCHER_OK;
}

static int descriptor_name(descriptor_input *input, char **out)
{
	const char *begin = input->data;
	size_t length = 1;

	if (!input->size || begin[0] != '\"')
		return 0;
	while (length < input->size) {
		if (begin[length] == '\\') {
			length += 2;
			continue;
		}
		if (begin[length++] == '\"')
			break;
	}
	if (length > input->size || begin[length - 1] != '\"')
		return 0;
	yyjson_doc *doc = yyjson_read(begin, length, 0);
	const char *name = json_string(yyjson_doc_get_root(doc));
	int valid = lx_path_valid(name, 1);
	char *encoded = valid ? (char *)malloc(strlen(name) * 6 + 3) : NULL;

	if (!encoded)
		valid = 0;
	size_t at = 0;
	if (valid) {
		static const char digits[] = "0123456789abcdef";
		const unsigned char *cursor = (const unsigned char *)name;

		encoded[at++] = '\"';
		while (*cursor) {
			unsigned code = *cursor++;

			if (code < 128) {
				if (code == '\"' || code == '\\')
					encoded[at++] = '\\';
				encoded[at++] = (char)code;
				continue;
			}
			unsigned extra = code < 224 ? 1 : code < 240 ? 2 : 3;

			code &= (1U << (6 - extra)) - 1;
			for (unsigned j = 0; j < extra; ++j)
				code = (code << 6) | (*cursor++ & 63);
			unsigned units[2] = {code, 0};
			unsigned count = 1;

			if (code > 65535) {
				units[0] = 0xd800 + ((code - 65536) >> 10);
				units[1] = 0xdc00 + ((code - 65536) & 1023);
				count = 2;
			}
			for (unsigned j = 0; j < count; ++j) {
				encoded[at++] = '\\';
				encoded[at++] = 'u';
				for (int shift = 12; shift >= 0; shift -= 4)
					encoded[at++] = digits[
						(units[j] >> shift) & 15];
			}
		}
		encoded[at++] = '\"';
		valid = at == length && !memcmp(encoded, begin, length);
	}
	if (valid) {
		*out = lx_strdup(name);
		valid = *out != NULL;
	}
	free(encoded);
	yyjson_doc_free(doc);
	if (valid) {
		input->data += length;
		input->size -= length;
	}
	return valid;
}

launcher_status lx_decode_descriptor(const void *data, size_t size,
	const char *fingerprint, lx_entry *out, launcher_error *error)
{
	out->path = NULL;
	out->cached_header = NULL;
	out->cached_header_size = 0;
	out->cached_header_sha256[0] = 0;
	if (!data || !size || size > metadata_limit)
		return lx_fail(error, LAUNCHER_METADATA,
			"Descriptor must contain 1 through 16777216 bytes");
	char hex[65];
	launcher_status status = lx_digest_bytes(data, size, hex, error);

	if (status != LAUNCHER_OK)
		return status;
	if (strcmp(hex, fingerprint))
		return lx_fail(error, LAUNCHER_INTEGRITY,
			"Descriptor fingerprint mismatch: %s", fingerprint);
	descriptor_input input = {(const char *)data, size};
	const char *header = NULL;
	size_t header_size = 0;
	char header_digest[65] = {};
	uint64_t referenced_size = 0;
	int version = 1;
	int valid = 1;

	if (descriptor_literal(&input, "{\n  \"cached_header\": \"")) {
		version = 2;
		const char *end = (const char *)memchr(
			input.data, '"', input.size);

		if (!end)
			return lx_fail(error, LAUNCHER_METADATA,
				"Unterminated cached header");
		header = input.data;
		header_size = (size_t)(end - input.data);
		input.data = end + 1;
		input.size -= header_size + 1;
		valid = descriptor_literal(&input, ",\n");
	} else if (descriptor_literal(&input,
		"{\n  \"cached_header_sha256\": \"")) {
		version = 3;
		valid = descriptor_hex(&input, header_digest, 64) &&
			descriptor_literal(&input,
				"\",\n  \"cached_header_size\": ") &&
			descriptor_integer(&input, &referenced_size) &&
			referenced_size && referenced_size <= metadata_limit &&
			descriptor_literal(&input, ",\n");
	} else {
		valid = descriptor_literal(&input, "{\n");
	}
	valid = valid && descriptor_literal(&input, "  \"encoding\": \"");
	if (valid) {
		if (descriptor_literal(&input, "raw"))
			out->encoding = LAUNCHER_RAW;
		else if (descriptor_literal(&input, "zlib"))
			out->encoding = LAUNCHER_ZLIB;
		else
			valid = 0;
	}
	const char *format = version == 1 ? "1" : version == 2 ? "2" : "3";

	valid = valid && descriptor_literal(&input, "\",\n  \"format\": ") &&
		descriptor_literal(&input, format) &&
		descriptor_literal(&input, ",\n  \"kind\": \"") &&
		(descriptor_literal(&input, "hogg") ?
			(out->kind = LAUNCHER_HOGG, 1) :
			descriptor_literal(&input, "loose") ?
			(out->kind = LAUNCHER_LOOSE, 1) : 0) &&
		descriptor_literal(&input, "\",\n  \"logical_md5\": \"") &&
		descriptor_hex(&input, out->logical_md5, 32) &&
		descriptor_literal(&input, "\",\n  \"name\": ") &&
		descriptor_name(&input, &out->path) &&
		descriptor_literal(&input, ",\n  \"sha256\": \"") &&
		descriptor_hex(&input, out->digest, 64) &&
		descriptor_literal(&input, "\",\n  \"size\": ") &&
		descriptor_integer(&input, &out->size) &&
		descriptor_literal(&input, ",\n  \"stored_size\": ") &&
		descriptor_integer(&input, &out->stored_size) &&
		descriptor_literal(&input, "\n}\n") && !input.size;
	if (valid)
		valid = out->encoding == LAUNCHER_RAW ?
			out->size == out->stored_size :
			out->stored_size && out->stored_size < out->size;
	if (!valid) {
		free(out->path);
		out->path = NULL;
		return lx_fail(error, LAUNCHER_METADATA,
			"Invalid descriptor schema, canonical bytes or "
			"representation");
	}
	if (version == 3) {
		memcpy(out->cached_header_sha256, header_digest, 65);
		out->cached_header_size = (size_t)referenced_size;
	}
	return version == 2 ?
		decode_cached_header(header, header_size, out, error) :
		LAUNCHER_OK;
}

static int fingerprint_compare(const void *a, const void *b)
{
	return strcmp((*(const lx_entry *const *)a)->fingerprint,
		(*(const lx_entry *const *)b)->fingerprint);
}

static launcher_status resolve_descriptors(launcher_release *release,
	launcher_descriptor_fn callback, void *state, launcher_error *error)
{
	size_t count = release->hogg_count + release->loose_count;

	if (!count)
		return LAUNCHER_OK;
	if (!callback)
		return lx_fail(error, LAUNCHER_INVALID,
			"Nonempty releases require a descriptor callback");
	lx_entry **sorted = (lx_entry **)calloc(count, sizeof(*sorted));

	release->descriptors = (lx_descriptor *)calloc(count,
		sizeof(lx_descriptor));
	if (!sorted || !release->descriptors) {
		free(sorted);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Allocating distinct descriptors");
	}
	for (size_t i = 0; i < count; ++i)
		sorted[i] = i < release->hogg_count ? release->hogg_files + i :
			release->loose_files + i - release->hogg_count;
	qsort(sorted, count, sizeof(*sorted), fingerprint_compare);
	launcher_status status = LAUNCHER_OK;
	lx_entry metadata = {};

	for (size_t i = 0; i < count && status == LAUNCHER_OK; ++i) {
		lx_entry *entry = sorted[i];

		if (!i || strcmp(entry->fingerprint,
			sorted[i - 1]->fingerprint)) {
			free(metadata.path);
			metadata.path = NULL;
			const void *bytes = NULL;
			size_t size = 0;

			status = callback(entry->fingerprint, &bytes, &size,
				state, error);
			if (status == LAUNCHER_OK)
				status = lx_decode_descriptor(bytes, size,
					entry->fingerprint, &metadata, error);
			if (status != LAUNCHER_OK)
				break;
			lx_descriptor *descriptor = release->descriptors +
				release->descriptor_count;

			if (release->borrowed_json) {
				descriptor->json = (char *)bytes;
			} else {
				descriptor->json = (char *)malloc(size + 1);
				if (!descriptor->json) {
					free(metadata.cached_header);
					status = lx_fail(error, LAUNCHER_MEMORY,
						"Retaining descriptor bytes");
					break;
				}
				memcpy(descriptor->json, bytes, size);
				descriptor->json[size] = 0;
			}
			memcpy(descriptor->fingerprint, entry->fingerprint, 65);
			descriptor->size = size;
			descriptor->cached_header = metadata.cached_header;
			++release->descriptor_count;
		}
		if (entry->kind != metadata.kind) {
			status = lx_fail(error, LAUNCHER_METADATA,
				"Descriptor kind differs from its group");
			break;
		}
		entry->path = lx_strdup(metadata.path);
		if (!entry->path) {
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Retaining canonical file name");
			break;
		}
		memcpy(entry->digest, metadata.digest, 65);
		memcpy(entry->logical_md5, metadata.logical_md5, 33);
		entry->size = metadata.size;
		entry->stored_size = metadata.stored_size;
		entry->encoding = metadata.encoding;
		entry->cached_header = metadata.cached_header;
		entry->cached_header_size = metadata.cached_header_size;
		memcpy(entry->cached_header_sha256,
			metadata.cached_header_sha256, 65);
	}
	free(metadata.path);
	free(sorted);
	for (size_t i = 0; status == LAUNCHER_OK &&
		i < release->hogg_count; ++i)
		if (release->hogg_files[i].size > UINT64_C(4294967294) ||
			release->hogg_files[i].stored_size >
				UINT64_C(4294967294))
			status = lx_fail(error, LAUNCHER_METADATA,
				"HOGG descriptor exceeds size limits");
	for (size_t i = 0; status == LAUNCHER_OK &&
		i < release->loose_count; ++i)
		if (release->loose_files[i].encoding != LAUNCHER_RAW)
			status = lx_fail(error, LAUNCHER_METADATA,
				"Loose descriptor requires raw storage");
	return status;
}

static launcher_status decode_release(const void *data, size_t size,
	const char *expected_release, launcher_descriptor_fn descriptor,
	void *state, int borrow, launcher_release **out,
	launcher_error *error)
{
	lx_clear(error);
	if (out)
		*out = NULL;
	if (!out || !lx_identifier(expected_release))
		return lx_fail(error, LAUNCHER_INVALID,
			"A canonical release ID and output are required");
	yyjson_doc *doc = NULL;
	launcher_status status = read_json(data, size, &doc, error);

	if (status != LAUNCHER_OK)
		return status;
	yyjson_val *root = yyjson_doc_get_root(doc);
	yyjson_val *groups = yyjson_obj_get(root, "groups");
	yyjson_val *loose = yyjson_obj_get(root, "loose");
	yyjson_val *entry = yyjson_obj_get(root, "entry_point");
	yyjson_val *arguments = yyjson_obj_get(root, "run_arguments");
	const char *root_names[] = { "format", "release", "groups",
		"loose", "entry_point", "run_arguments" };
	const char *id = json_string(yyjson_obj_get(root, "release"));
	launcher_release *release = NULL;

	if (!fields(root, root_names, 6) ||
		!yyjson_is_uint(yyjson_obj_get(root, "format")) ||
		yyjson_get_uint(yyjson_obj_get(root, "format")) != 2 ||
		!lx_identifier(id) || strcmp(id, expected_release) ||
		!yyjson_is_arr(loose) || yyjson_arr_size(loose) > entry_limit ||
		!yyjson_is_obj(groups) || yyjson_obj_size(groups) > entry_limit ||
		(!yyjson_is_null(entry) &&
			!lx_path_valid(json_string(entry), 1)) ||
		!yyjson_is_arr(arguments) || yyjson_arr_size(arguments) > 4096) {
		status = lx_fail(error, LAUNCHER_METADATA,
			"Invalid committed release schema, identity or limits");
		goto done;
	}
	release = (launcher_release *)calloc(1, sizeof(*release));
	if (!release) {
		status = lx_fail(error, LAUNCHER_MEMORY, "Allocating release");
		goto done;
	}
	release->borrowed_json = borrow;
	memcpy(release->id, id, strlen(id) + 1);
	release->json = borrow ? (char *)data : (char *)malloc(size + 1);
	release->loose_count = yyjson_arr_size(loose);
	release->group_count = yyjson_obj_size(groups) +
		(release->loose_count ? 1 : 0);
	release->groups = (lx_group *)calloc(release->group_count + 1,
		sizeof(lx_group));
	if (!release->json || !release->groups) {
		status = lx_fail(error, LAUNCHER_MEMORY, "Retaining release");
		goto done;
	}
	if (!borrow) {
		memcpy(release->json, data, size);
		release->json[size] = 0;
	}
	release->json_size = size;
	{
		size_t index, count;
		yyjson_val *key, *value;

		yyjson_obj_foreach(groups, index, count, key, value) {
			const char *name = json_string(key);
			yyjson_val *files = yyjson_obj_get(value, "files");
			const char *path = json_string(yyjson_obj_get(value,
				"path"));
			const char *names[] = {"files", "path"};
			lx_group *group = release->groups + index;

			if (!lx_identifier(name) || !strcmp(name, "loose") ||
				!fields(value, names, 2) ||
				!yyjson_is_arr(files) ||
				!lx_path_valid(path, 1) ||
				yyjson_arr_size(files) > entry_limit -
					release->hogg_count - release->loose_count) {
				status = lx_fail(error, LAUNCHER_METADATA,
					"Invalid release group");
				goto done;
			}
			group->name = lx_strdup(name);
			group->path = lx_strdup(path);
			group->kind = LAUNCHER_HOGG;
			group->count = yyjson_arr_size(files);
			group->first = release->hogg_count;
			if (!group->name || !group->path) {
				status = lx_fail(error, LAUNCHER_MEMORY,
					"Retaining group");
				goto done;
			}
			release->hogg_count += group->count;
		}
		release->hogg_files = (lx_entry *)calloc(
			release->hogg_count + 1, sizeof(lx_entry));
		release->loose_files = (lx_entry *)calloc(
			release->loose_count + 1, sizeof(lx_entry));
		if (!release->hogg_files || !release->loose_files) {
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Allocating file inventory");
			goto done;
		}
		yyjson_obj_foreach(groups, index, count, key, value) {
			lx_group *group = release->groups + index;
			lx_entry *files = group->kind == LAUNCHER_HOGG ?
				release->hogg_files : release->loose_files;

			status = read_entries(yyjson_obj_get(value, "files"),
				files + group->first, group->kind, error);
			if (status != LAUNCHER_OK)
				goto done;
		}
		status = read_entries(loose, release->loose_files,
			LAUNCHER_LOOSE, error);
		if (status != LAUNCHER_OK)
			goto done;
		if (release->loose_count) {
			lx_group *group =
				release->groups + yyjson_obj_size(groups);

			group->name = lx_strdup("loose");
			group->kind = LAUNCHER_LOOSE;
			group->count = release->loose_count;
			if (!group->name) {
				status = LAUNCHER_MEMORY;
				goto done;
			}
		}
		qsort(release->groups, release->group_count, sizeof(lx_group),
			group_order);
		for (size_t i = 1; i < release->group_count; ++i)
			if (!strcmp(release->groups[i - 1].name,
				release->groups[i].name)) {
				status = lx_fail(error, LAUNCHER_METADATA,
					"Duplicate group name");
				goto done;
			}
		status = resolve_descriptors(release, descriptor, state, error);
		if (status != LAUNCHER_OK)
			goto done;
		for (size_t i = 0; i < release->group_count; ++i) {
			lx_group *group = release->groups + i;
			lx_entry *files = group->kind == LAUNCHER_HOGG ?
				release->hogg_files : release->loose_files;

			qsort(files + group->first, group->count,
				sizeof(lx_entry), entry_order);
		}
		if (!yyjson_is_null(entry)) {
			release->entry_point = lx_strdup(json_string(entry));
			int found = 0;

			for (size_t i = 0; i < release->loose_count; ++i)
				found |= !strcmp(release->loose_files[i].path,
					json_string(entry));
			if (!found) {
				status = lx_fail(error, LAUNCHER_METADATA,
					"Entry point is not a managed loose file");
				goto done;
			}
			if (!release->entry_point) {
				status = LAUNCHER_MEMORY;
				goto done;
			}
		}
		release->run_argument_count = yyjson_arr_size(arguments);
		release->run_arguments = (const char **)calloc(
			release->run_argument_count + 1, sizeof(char *));
		if (!release->run_arguments) {
			status = LAUNCHER_MEMORY;
			goto done;
		}
		yyjson_arr_foreach(arguments, index, count, value) {
			const char *argument = json_string(value);

			if (!argument) {
				status = lx_fail(error, LAUNCHER_METADATA,
					"Launch arguments must be strings");
				goto done;
			}
			release->run_arguments[index] = lx_strdup(argument);
			if (!release->run_arguments[index]) {
				status = LAUNCHER_MEMORY;
				goto done;
			}
		}
	}
	status = validate_inventory(release, 1, error);
	if (status == LAUNCHER_OK)
		status = validate_inventory(release, 0, error);
	if (status == LAUNCHER_OK) {
		*out = release;
		release = NULL;
	}

done:
	launcher_release_free(&release);
	yyjson_doc_free(doc);
	return status;
}

launcher_status launcher_decode_release(const void *data, size_t size,
	const char *expected_release, launcher_descriptor_fn descriptor,
	void *state, launcher_release **out, launcher_error *error)
{
	return decode_release(data, size, expected_release, descriptor, state,
		0, out, error);
}

launcher_status lx_decode_saved_release(const void *data, size_t size,
	const char *expected_release, launcher_descriptor_fn descriptor,
	void *state, launcher_release **out, launcher_error *error)
{
	return decode_release(data, size, expected_release, descriptor, state,
		1, out, error);
}

void launcher_release_free(launcher_release **release)
{
	if (!release || !*release)
		return;
	launcher_release *value = *release;

	for (size_t i = 0; value->hogg_files && i < value->hogg_count; i++)
		free(value->hogg_files[i].path);
	for (size_t i = 0; value->loose_files && i < value->loose_count; i++)
		free(value->loose_files[i].path);
	for (size_t i = 0; i < value->descriptor_count; ++i) {
		if (!value->borrowed_json)
			free(value->descriptors[i].json);
		free(value->descriptors[i].cached_header);
	}
	free(value->descriptors);
	free(value->hogg_files);
	free(value->loose_files);
	for (size_t i = 0; i < value->group_count; ++i) {
		free(value->groups[i].name);
		free(value->groups[i].path);
	}
	free(value->groups);
	if (!value->borrowed_json)
		free(value->json);
	free(value->descriptor_backing);
	clear_channel(value);
	free(value);
	*release = NULL;
}

const char *launcher_release_id(const launcher_release *release)
{
	return release ? release->id : NULL;
}

launcher_status launcher_release_get_launch(const launcher_release *release,
	launcher_launch_info *out, launcher_error *error)
{
	lx_clear(error);
	if (out)
		*out = {};
	if (!release || !out)
		return lx_fail(error, LAUNCHER_INVALID,
			"A release and launch output are required");
	out->entry_point = release->entry_point;
	out->run_arguments = release->run_arguments;
	out->run_argument_count = release->run_argument_count;
	return LAUNCHER_OK;
}

launcher_status lx_release_channel(launcher_release *release,
	const void *data, size_t size, const char *channel,
	launcher_error *error)
{
	char id[65];
	if (!release || release->channel_json)
		return lx_fail(error, LAUNCHER_INVALID,
			"A fresh release is required for a channel snapshot");
	return read_channel(data, size, channel, id, release, error);
}

int lx_same_channel(const launcher_release *a, const launcher_release *b)
{
	return !strcmp(a->channel_json ? a->channel_json : "",
		b->channel_json ? b->channel_json : "");
}

size_t launcher_release_group_count(const launcher_release *release)
{
	return release ? release->group_count : 0;
}

launcher_status launcher_release_group(const launcher_release *release,
	size_t index, launcher_file_group *out, launcher_error *error)
{
	lx_clear(error);
	if (out)
		*out = {};
	if (!release || !out || index >= release->group_count)
		return lx_fail(error, LAUNCHER_INVALID, "Invalid group selection");
	const lx_group *group = release->groups + index;

	out->name = group->name;
	out->kind = group->kind;
	out->hogg_path = group->path;
	out->file_count = group->count;
	return LAUNCHER_OK;
}

launcher_status launcher_release_group_file(const launcher_release *release,
	size_t group_index, size_t file_index, launcher_file *out,
	launcher_error *error)
{
	lx_clear(error);
	if (out)
		*out = {};
	if (!release || !out || group_index >= release->group_count ||
		file_index >= release->groups[group_index].count)
		return lx_fail(error, LAUNCHER_INVALID, "Invalid file selection");
	const lx_group *group = release->groups + group_index;
	const lx_entry *entry = (group->kind == LAUNCHER_HOGG ?
		release->hogg_files : release->loose_files) +
		group->first + file_index;

	out->path = entry->path;
	out->sha256 = entry->digest;
	out->fingerprint = entry->fingerprint;
	out->size = entry->size;
	out->stored_size = entry->stored_size;
	out->encoding = entry->encoding;
	out->logical_md5 = entry->logical_md5;
	return LAUNCHER_OK;
}

void lx_group_view(const launcher_release *release, size_t index,
	launcher_release *out)
{
	*out = *release;
	const lx_group *group = release->groups + index;

	out->hogg = group->path;
	out->hogg_files = release->hogg_files + group->first;
	out->hogg_count = group->count;
}

size_t lx_physical_count(const launcher_release *release)
{
	if (!release)
		return 0;
	size_t count = release->loose_count;

	for (size_t i = 0; i < release->group_count; ++i)
		count += release->groups[i].kind == LAUNCHER_HOGG;
	return count;
}

const char *lx_physical_path(const launcher_release *release, size_t index)
{
	for (size_t i = 0; i < release->group_count; ++i) {
		if (release->groups[i].kind != LAUNCHER_HOGG)
			continue;
		if (!index--)
			return release->groups[i].path;
	}
	return release->loose_files[index].path;
}

launcher_status lx_release_descriptor(const char *fingerprint,
	const void **data, size_t *size, void *state, launcher_error *error)
{
	const launcher_release *release = (const launcher_release *)state;
	size_t low = 0, high = release->descriptor_count;

	while (low < high) {
		size_t mid = low + (high - low) / 2;
		const lx_descriptor *descriptor = release->descriptors + mid;
		int order = strcmp(fingerprint, descriptor->fingerprint);

		if (!order) {
			*data = descriptor->json;
			*size = descriptor->size;
			return LAUNCHER_OK;
		}
		if (order < 0)
			high = mid;
		else
			low = mid + 1;
	}
	return lx_fail(error, LAUNCHER_METADATA,
		"Saved release is missing descriptor %s", fingerprint);
}

launcher_status lx_clone_release(const launcher_release *source,
	launcher_release **out, launcher_error *error)
{
	if (!source || !out)
		return lx_fail(error, LAUNCHER_INVALID, "Release is required");
	launcher_status result = launcher_decode_release(
		source->json, source->json_size,
		source->id, lx_release_descriptor, (void *)source, out, error);
	if (result == LAUNCHER_OK && source->channel_json)
		result = lx_release_channel(*out, source->channel_json,
			strlen(source->channel_json), "saved", error);
	if (result != LAUNCHER_OK)
		launcher_release_free(out);
	return result;
}

int lx_same_release(const launcher_release *a, const launcher_release *b)
{
	if (!a || !b || a->json_size != b->json_size)
		return 0;
	return !memcmp(a->json, b->json, a->json_size);
}
