#include "internal.h"
#include <aws/cal/cal.h>
#include <aws/common/thread.h>
#include <aws/auth/credentials.h>
#include <aws/auth/signable.h>
/* Upstream signing constants are declared outside its C linkage block. */
extern "C" {
#include <aws/auth/signing.h>
}
#include <aws/auth/signing_result.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/common/uri.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/http/connection.h>
#include <aws/http/request_response.h>
#include <curl/curl.h>
#include <mbedtls/sha256.h>
#include <stdlib.h>
#include <string.h>

static aws_thread_once crypto_once = AWS_THREAD_ONCE_STATIC_INIT;

static void initialize_crypto(void *)
{
	aws_cal_library_init(aws_default_allocator());
}

void lx_crypto_initialize(void)
{
	aws_thread_call_once(&crypto_once, initialize_crypto, NULL);
}

static constexpr size_t chunk_size = 65536;

static void hex_digest(const unsigned char bytes[32], char text[65])
{
	static const char hex[] = "0123456789abcdef";

	for (size_t i = 0; i < 32; i++) {
		text[i * 2] = hex[bytes[i] >> 4];
		text[i * 2 + 1] = hex[bytes[i] & 15];
	}
	text[64] = 0;
}

launcher_status lx_read_bytes(lx_fs *fs, const char *path, size_t limit,
	char **out, size_t *size, launcher_error *error)
{
	if (!out || !size || limit == SIZE_MAX)
		return lx_fail(error, LAUNCHER_INVALID, "Invalid bounded read");
	*out = NULL;
	*size = 0;
	FILE *file = NULL;
	launcher_status status = lx_fs_read(fs, path, &file, error);

	if (status != LAUNCHER_OK)
		return status;
	char *buffer = NULL;
	size_t count = 0, capacity = 0;
	unsigned char chunk[chunk_size];

	for (;;) {
		size_t got = fread(chunk, 1, sizeof(chunk), file);

		if (got > limit - count) {
			status = lx_fail(error, LAUNCHER_METADATA,
				"File exceeds metadata bound: %s", path);
			break;
		}
		if (count + got + 1 > capacity) {
			size_t next = capacity ? capacity * 2 : chunk_size;

			if (next < count + got + 1)
				next = count + got + 1;
			if (next > limit + 1)
				next = limit + 1;
			char *grown = (char *)realloc(buffer, next);

			if (!grown) {
				status = lx_fail(error, LAUNCHER_MEMORY,
					"Allocating bounded file read");
				break;
			}
			buffer = grown;
			capacity = next;
		}
		memcpy(buffer + count, chunk, got);
		count += got;
		if (got < sizeof(chunk)) {
			if (ferror(file))
				status = lx_fail(error, LAUNCHER_IO,
					"Reading %s", path);
			break;
		}
	}
	if (fclose(file) && status == LAUNCHER_OK)
		status = lx_fail(error, LAUNCHER_IO, "Closing %s", path);
	if (status == LAUNCHER_OK) {
		buffer[count] = 0;
		*out = buffer;
		*size = count;
	} else {
		free(buffer);
	}
	return status;
}

launcher_status lx_save_bytes(lx_fs *fs, const char *path, const void *data,
	size_t size, launcher_error *error)
{
	if (!fs || !path || (!data && size))
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid metadata write");
	size_t length = strlen(path);
	char *temporary = (char *)malloc(length + 5);

	if (!temporary)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Allocating staging path");
	memcpy(temporary, path, length);
	memcpy(temporary + length, ".tmp", 5);
	launcher_status status = lx_fs_remove(fs, temporary, 0, error);
	FILE *file = NULL;

	if (status == LAUNCHER_OK)
		status = lx_fs_write(fs, temporary, 1, &file, error);
	if (status == LAUNCHER_OK && size &&
		fwrite(data, 1, size, file) != size)
		status = lx_fail(error, LAUNCHER_IO, "Writing %s", path);
	if (status == LAUNCHER_OK)
		status = lx_file_flush(file, error);
	if (file && fclose(file) && status == LAUNCHER_OK)
		status = lx_fail(error, LAUNCHER_IO, "Closing %s", path);
	if (status == LAUNCHER_OK)
		status = lx_fs_replace(fs, temporary, fs, path, error);
	if (status != LAUNCHER_OK)
		lx_fs_remove(fs, temporary, 0, NULL);
	free(temporary);
	return status;
}

launcher_status lx_hash_file(FILE *file, uint64_t expected,
	const char *digest, FILE *copy, launcher_error *error)
{
	if (!file || !digest || strlen(digest) != 64)
		return lx_fail(error, LAUNCHER_INVALID, "Invalid stream hash");
	mbedtls_sha256_context hash;
	mbedtls_sha256_init(&hash);
	launcher_status status = LAUNCHER_OK;
	uint64_t count = 0;
	unsigned char chunk[chunk_size];

	if (mbedtls_sha256_starts(&hash, 0))
		status = lx_fail(error, LAUNCHER_INTEGRITY, "Starting SHA-256");
	while (status == LAUNCHER_OK) {
		size_t got = fread(chunk, 1, sizeof(chunk), file);

		if (got > expected - count) {
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Payload exceeds declared length");
			break;
		}
		count += got;
		if (mbedtls_sha256_update(&hash, chunk, got))
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Updating SHA-256");
		if (status == LAUNCHER_OK && copy && got &&
			fwrite(chunk, 1, got, copy) != got)
			status = lx_fail(error, LAUNCHER_IO, "Copying payload");
		if (got < sizeof(chunk)) {
			if (ferror(file))
				status = lx_fail(error, LAUNCHER_IO,
					"Reading payload");
			break;
		}
	}
	unsigned char bytes[32];
	char actual[65];

	if (status == LAUNCHER_OK &&
		(count != expected || mbedtls_sha256_finish(&hash, bytes)))
		status = lx_fail(error, LAUNCHER_INTEGRITY,
			"Payload length differs from manifest");
	if (status == LAUNCHER_OK) {
		hex_digest(bytes, actual);
		if (strcmp(actual, digest))
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Payload SHA-256 differs from manifest");
	}
	mbedtls_sha256_free(&hash);
	return status;
}

static launcher_status copy_accepted(FILE *input, FILE *output,
	uint64_t expected, launcher_error *error)
{
	unsigned char chunk[chunk_size];
	uint64_t count = 0;

	for (;;) {
		size_t got = fread(chunk, 1, sizeof(chunk), input);

		if (got > expected - count)
			return lx_fail(error, LAUNCHER_INTEGRITY,
				"Accepted payload exceeds declared length");
		count += got;
		if (got && fwrite(chunk, 1, got, output) != got)
			return lx_fail(error, LAUNCHER_IO,
				"Copying accepted payload");
		if (got < sizeof(chunk)) {
			if (ferror(input))
				return lx_fail(error, LAUNCHER_IO,
					"Reading accepted payload");
			break;
		}
	}
	return count == expected ? LAUNCHER_OK :
		lx_fail(error, LAUNCHER_INTEGRITY,
			"Accepted payload length differs from manifest");
}

launcher_status lx_copy_object(lx_fs *from, const char *source,
	lx_fs *to, const char *dest, uint64_t size, const char *digest,
	launcher_error *error)
{
	FILE *input = NULL, *output = NULL;
	launcher_status status = lx_fs_read(from, source, &input, error);

	if (status == LAUNCHER_OK)
		status = lx_fs_write(to, dest, 1, &output, error);
	if (status == LAUNCHER_OK)
		status = digest ?
			lx_hash_file(input, size, digest, output, error) :
			copy_accepted(input, output, size, error);
	if (status == LAUNCHER_OK)
		status = lx_file_flush(output, error);
	if (input && fclose(input) && status == LAUNCHER_OK)
		status = lx_fail(error, LAUNCHER_IO, "Closing verified input");
	if (output && fclose(output) && status == LAUNCHER_OK)
		status = lx_fail(error, LAUNCHER_IO, "Closing verified copy");
	if (output && status != LAUNCHER_OK)
		lx_fs_remove(to, dest, 0, NULL);
	return status;
}

struct http_transfer {
	char *buffer;
	size_t capacity;
	size_t limit;
	FILE *file;
	mbedtls_sha256_context *hash;
	uint64_t expected;
	uint64_t count;
	launcher_progress_fn callback;
	void *state;
	const char *path;
	launcher_phase phase;
	size_t objects_done;
	size_t objects_total;
	launcher_status failure;
	int bad_encoding;
	long response;
};

static size_t receive_bytes(char *data, size_t width, size_t count,
	void *state)
{
	http_transfer *transfer = (http_transfer *)state;

	if (width && count > SIZE_MAX / width)
		return 0;
	size_t size = width * count;

	/* A complete missing-object response can outsize the object itself. */
	if (transfer->response == 404) {
		if (size > 16 * 1024 * 1024 - transfer->count) {
			transfer->failure = LAUNCHER_METADATA;
			return 0;
		}
		transfer->count += size;
		return size;
	}
	if (size > transfer->expected - transfer->count) {
		transfer->failure = transfer->file ? LAUNCHER_INTEGRITY :
			LAUNCHER_METADATA;
		return 0;
	}
	if (transfer->file) {
		if (fwrite(data, 1, size, transfer->file) != size) {
			transfer->failure = LAUNCHER_IO;
			return 0;
		}
		if (mbedtls_sha256_update(transfer->hash,
			(const unsigned char *)data, size)) {
			transfer->failure = LAUNCHER_INTEGRITY;
			return 0;
		}
	} else {
		size_t needed = (size_t)transfer->count + size + 1;

		if (needed > transfer->capacity) {
			size_t next = transfer->capacity ?
				transfer->capacity * 2 : chunk_size;

			if (next < needed)
				next = needed;
			if (next > transfer->limit + 1)
				next = transfer->limit + 1;
			char *grown = (char *)realloc(transfer->buffer, next);

			if (!grown) {
				transfer->failure = LAUNCHER_MEMORY;
				return 0;
			}
			transfer->buffer = grown;
			transfer->capacity = next;
		}
		memcpy(transfer->buffer + transfer->count, data, size);
	}
	transfer->count += size;
	return size;
}

static int transfer_progress(void *state, curl_off_t total, curl_off_t done,
	curl_off_t, curl_off_t)
{
	http_transfer *transfer = (http_transfer *)state;
	uint64_t maximum = transfer->file ? transfer->expected :
		(total > 0 ? (uint64_t)total : 0);

	if (lx_progress(transfer->callback, transfer->state, transfer->phase,
		transfer->path, done > 0 ? (uint64_t)done : 0, maximum,
		transfer->objects_done, transfer->objects_total))
		return 0;
	transfer->failure = LAUNCHER_CANCELLED;
	return 1;
}

static int ascii_equal(const char *a, const char *b, size_t size)
{
	for (size_t i = 0; i < size; i++) {
		char left = a[i], right = b[i];

		if (left >= 'A' && left <= 'Z')
			left += 'a' - 'A';
		if (right >= 'A' && right <= 'Z')
			right += 'a' - 'A';
		if (left != right)
			return 0;
	}
	return 1;
}

static size_t receive_header(char *data, size_t width, size_t count,
	void *state)
{
	http_transfer *transfer = (http_transfer *)state;

	if (width && count > SIZE_MAX / width)
		return 0;
	size_t size = width * count;
	if (size > 5 && !memcmp(data, "HTTP/", 5)) {
		char line[128];
		size_t length = size < sizeof(line) - 1 ? size : sizeof(line) - 1;

		memcpy(line, data, length);
		line[length] = 0;
		if (sscanf(line, "HTTP/%*s %ld", &transfer->response) != 1)
			return 0;
	}
	const char name[] = "content-encoding:";

	if (size >= sizeof(name) - 1 &&
		ascii_equal(data, name, sizeof(name) - 1)) {
		size_t start = sizeof(name) - 1, end = size;

		while (start < end &&
			(data[start] == ' ' || data[start] == '\t'))
			start++;
		while (end > start && (data[end - 1] == '\r' ||
			data[end - 1] == '\n' || data[end - 1] == ' ' ||
			data[end - 1] == '\t'))
			end--;
		if (end - start != 8 ||
			!ascii_equal(data + start, "identity", 8))
			transfer->bad_encoding = 1;
	}
	return size;
}

struct crt_transfer {
	aws_mutex mutex;
	aws_condition_variable condition;
	aws_http_connection *connection;
	aws_http_message *request;
	unsigned char queue[chunk_size];
	size_t queued;
	int response;
	int bad_encoding;
	int native_error;
	int setup_done;
	int sign_done;
	int stream_done;
	int stream_destroyed;
	int connection_closed;
	int bootstrap_closed;
	int resolver_closed;
	int loops_closed;
};

static void crt_notify(crt_transfer *state, int *flag)
{
	aws_mutex_lock(&state->mutex);
	*flag = 1;
	aws_condition_variable_notify_all(&state->condition);
	aws_mutex_unlock(&state->mutex);
}

static void crt_setup(aws_http_connection *connection, int code, void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	aws_mutex_lock(&state->mutex);
	state->connection = connection;
	state->native_error = code;
	state->setup_done = 1;
	aws_condition_variable_notify_all(&state->condition);
	aws_mutex_unlock(&state->mutex);
}

static void crt_shutdown(aws_http_connection *, int, void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	crt_notify(state, &state->connection_closed);
}

static void crt_bootstrap_shutdown(void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	crt_notify(state, &state->bootstrap_closed);
}

static void crt_resolver_shutdown(void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	crt_notify(state, &state->resolver_closed);
}

static void crt_loops_shutdown(void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	crt_notify(state, &state->loops_closed);
}

static void crt_signed(aws_signing_result *result, int code, void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	if (!code && aws_apply_signing_result_to_http_request(state->request,
		aws_default_allocator(), result))
		code = aws_last_error();
	aws_mutex_lock(&state->mutex);
	state->native_error = code;
	state->sign_done = 1;
	aws_condition_variable_notify_all(&state->condition);
	aws_mutex_unlock(&state->mutex);
}

static int crt_headers(aws_http_stream *, aws_http_header_block,
	const aws_http_header *headers, size_t count, void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	aws_mutex_lock(&state->mutex);
	for (size_t i = 0; i < count; i++) {
		const aws_byte_cursor name = headers[i].name;
		const aws_byte_cursor value = headers[i].value;

		if (name.len != 16 || !ascii_equal((const char *)name.ptr,
			"content-encoding", 16))
			continue;
		size_t start = 0, end = value.len;

		while (start < end && (value.ptr[start] == ' ' ||
			value.ptr[start] == '\t'))
			start++;
		while (end > start && (value.ptr[end - 1] == ' ' ||
			value.ptr[end - 1] == '\t'))
			end--;
		if (end - start != 8 || !ascii_equal(
			(const char *)value.ptr + start, "identity", 8))
			state->bad_encoding = 1;
	}
	aws_mutex_unlock(&state->mutex);
	return AWS_OP_SUCCESS;
}

static int crt_headers_done(aws_http_stream *stream,
	aws_http_header_block block, void *data)
{
	if (block != AWS_HTTP_HEADER_BLOCK_MAIN)
		return AWS_OP_SUCCESS;
	crt_transfer *state = (crt_transfer *)data;
	int response = 0;

	if (aws_http_stream_get_incoming_response_status(stream, &response))
		return AWS_OP_ERR;
	aws_mutex_lock(&state->mutex);
	state->response = response;
	/* Consume 404 responses completely so descriptor fallback can
	 * distinguish a complete miss from a failed or truncated transfer.
	 */
	int reject = (response != 200 && response != 404) ||
		state->bad_encoding;

	aws_condition_variable_notify_all(&state->condition);
	aws_mutex_unlock(&state->mutex);
	return reject ? aws_raise_error(AWS_ERROR_HTTP_STREAM_CANCELLED) :
		AWS_OP_SUCCESS;
}

static int crt_body(aws_http_stream *, const aws_byte_cursor *bytes,
	void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	aws_mutex_lock(&state->mutex);
	if (bytes->len > sizeof(state->queue) - state->queued) {
		aws_mutex_unlock(&state->mutex);
		return aws_raise_error(AWS_ERROR_HTTP_STREAM_CANCELLED);
	}
	memcpy(state->queue + state->queued, bytes->ptr, bytes->len);
	state->queued += bytes->len;
	aws_condition_variable_notify_all(&state->condition);
	aws_mutex_unlock(&state->mutex);
	return AWS_OP_SUCCESS;
}

static void crt_complete(aws_http_stream *, int code, void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	aws_mutex_lock(&state->mutex);
	state->native_error = code;
	state->stream_done = 1;
	aws_condition_variable_notify_all(&state->condition);
	aws_mutex_unlock(&state->mutex);
}

static void crt_destroyed(void *data)
{
	crt_transfer *state = (crt_transfer *)data;

	crt_notify(state, &state->stream_destroyed);
}

static int crt_progress(http_transfer *transfer)
{
	if (transfer->failure != LAUNCHER_OK)
		return 0;
	if (lx_progress(transfer->callback, transfer->state, transfer->phase,
		transfer->path, transfer->count,
		transfer->file ? transfer->expected : 0,
		transfer->objects_done, transfer->objects_total))
		return 1;
	transfer->failure = LAUNCHER_CANCELLED;
	return 0;
}

static void crt_wait(crt_transfer *state, int *flag,
	http_transfer *transfer)
{
	aws_mutex_lock(&state->mutex);
	while (!*flag) {
		aws_condition_variable_wait_for(&state->condition,
			&state->mutex, 100000000);
		aws_mutex_unlock(&state->mutex);
		if (transfer)
			crt_progress(transfer);
		aws_mutex_lock(&state->mutex);
	}
	aws_mutex_unlock(&state->mutex);
}

static int crt_initialize(void)
{
	lx_crypto_initialize();
	aws_auth_library_init(aws_default_allocator());
	return 1;
}

static launcher_status crt_http_get(const launcher_config *config,
	const launcher_s3_credentials *credentials, const char *relative,
	http_transfer *transfer, launcher_error *error)
{
	if (!credentials || !credentials->access_key_id ||
		!*credentials->access_key_id ||
		!credentials->secret_access_key ||
		!*credentials->secret_access_key)
		return lx_fail(error, LAUNCHER_INVALID,
			"S3 access key ID and secret access key required");
	static const int initialized = crt_initialize();
	(void)initialized;
	aws_allocator *allocator = aws_default_allocator();
	size_t base_size = strlen(config->base_url);
	size_t relative_size = strlen(relative);
	char *url = (char *)malloc(base_size + relative_size + 1);

	if (!url)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Allocating signed resource URL");
	memcpy(url, config->base_url, base_size);
	memcpy(url + base_size, relative, relative_size + 1);
	aws_uri uri = {};
	aws_byte_cursor url_cursor = aws_byte_cursor_from_c_str(url);
	int code = aws_uri_init_parse(&uri, allocator, &url_cursor);

	free(url);
	if (code || uri.scheme.len != 5 || !ascii_equal(
		(const char *)uri.scheme.ptr, "https", 5) ||
		!uri.host_name.len || uri.userinfo.len ||
		uri.query_string.len) {
		aws_uri_clean_up(&uri);
		return lx_fail(error, LAUNCHER_INVALID,
			"Signed repository requires an HTTPS URL");
	}
	crt_transfer state = {};

	if (aws_mutex_init(&state.mutex)) {
		aws_uri_clean_up(&uri);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Initializing signed transfer mutex");
	}
	if (aws_condition_variable_init(&state.condition)) {
		aws_mutex_clean_up(&state.mutex);
		aws_uri_clean_up(&uri);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Initializing signed transfer condition");
	}
	aws_shutdown_callback_options loops_shutdown = {
		crt_loops_shutdown, &state
	};
	aws_event_loop_group *loops = aws_event_loop_group_new_default(
		allocator, 1, &loops_shutdown);
	aws_shutdown_callback_options resolver_shutdown = {
		crt_resolver_shutdown, &state
	};
	aws_host_resolver_default_options resolver_options = {};

	resolver_options.max_entries = 1;
	resolver_options.el_group = loops;
	resolver_options.shutdown_options = &resolver_shutdown;
	aws_host_resolver *resolver = loops ? aws_host_resolver_new_default(
		allocator, &resolver_options) : NULL;
	aws_client_bootstrap_options bootstrap_options = {};

	bootstrap_options.event_loop_group = loops;
	bootstrap_options.host_resolver = resolver;
	bootstrap_options.on_shutdown_complete = crt_bootstrap_shutdown;
	bootstrap_options.user_data = &state;
	aws_client_bootstrap *bootstrap = resolver ? aws_client_bootstrap_new(
		allocator, &bootstrap_options) : NULL;
	aws_tls_ctx_options tls_options = {};
	aws_tls_connection_options tls_connection = {};
	aws_tls_ctx *tls = NULL;
	aws_credentials *keys = NULL;
	aws_signable *signable = NULL;
	aws_http_stream *stream = NULL;
	launcher_status status = LAUNCHER_OK;

	if (!bootstrap)
		status = lx_fail(error, LAUNCHER_MEMORY,
			"Initializing signed transport");
	aws_tls_ctx_options_init_default_client(&tls_options, allocator);
	aws_tls_ctx_options_set_verify_peer(&tls_options, true);
	if (status == LAUNCHER_OK && config->ca_bundle &&
		*config->ca_bundle &&
		aws_tls_ctx_options_override_default_trust_store_from_path(
			&tls_options, NULL, config->ca_bundle))
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Loading signed transport CA bundle");
	if (status == LAUNCHER_OK) {
		tls = aws_tls_client_ctx_new(allocator, &tls_options);
		if (!tls)
			status = lx_fail(error, LAUNCHER_NETWORK,
				"Initializing signed transport TLS");
	}
	if (status == LAUNCHER_OK) {
		aws_tls_connection_options_init_from_ctx(&tls_connection, tls);
		tls_connection.timeout_ms = 30000;
		if (aws_tls_connection_options_set_server_name(&tls_connection,
			allocator, &uri.host_name))
			status = lx_fail(error, LAUNCHER_NETWORK,
				"Setting signed transport TLS hostname");
	}
	if (status == LAUNCHER_OK) {
		state.request = aws_http_message_new_request(allocator);
		if (!state.request)
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Creating signed request");
	}
	if (status == LAUNCHER_OK) {
		aws_http_header headers[] = {
			{aws_byte_cursor_from_c_str("Host"), uri.authority,
				AWS_HTTP_HEADER_COMPRESSION_USE_CACHE},
			{aws_byte_cursor_from_c_str("Accept-Encoding"),
				aws_byte_cursor_from_c_str("identity"),
				AWS_HTTP_HEADER_COMPRESSION_USE_CACHE},
			{aws_byte_cursor_from_c_str("Cache-Control"),
				aws_byte_cursor_from_c_str("no-cache"),
				AWS_HTTP_HEADER_COMPRESSION_USE_CACHE},
			{aws_byte_cursor_from_c_str("User-Agent"),
				aws_byte_cursor_from_c_str("launcher/1"),
				AWS_HTTP_HEADER_COMPRESSION_USE_CACHE}
		};

		if (aws_http_message_set_request_method(state.request,
			aws_byte_cursor_from_c_str("GET")) ||
			aws_http_message_set_request_path(state.request,
				uri.path_and_query) ||
			aws_http_message_add_header_array(state.request,
				headers,
				sizeof(headers) / sizeof(headers[0])))
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Preparing signed request");
	}
	if (status == LAUNCHER_OK) {
		keys = aws_credentials_new(allocator,
			aws_byte_cursor_from_c_str(credentials->access_key_id),
			aws_byte_cursor_from_c_str(
				credentials->secret_access_key),
			aws_byte_cursor_from_c_str(credentials->session_token ?
				credentials->session_token : ""), UINT64_MAX);
		signable = aws_signable_new_http_request(allocator,
			state.request);
		if (!keys || !signable)
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Preparing S3 request signing");
	}
	if (status == LAUNCHER_OK) {
		aws_signing_config_aws signing = {};

		signing.config_type = AWS_SIGNING_CONFIG_AWS;
		signing.algorithm = AWS_SIGNING_ALGORITHM_V4;
		signing.signature_type = AWS_ST_HTTP_REQUEST_HEADERS;
		signing.region = aws_byte_cursor_from_c_str(config->s3_region);
		signing.service = aws_byte_cursor_from_c_str("s3");
		/* S3 retains separators and existing URL encoding. */
		signing.flags.use_double_uri_encode = 0;
		signing.flags.should_normalize_uri_path = 0;
		aws_date_time_init_now(&signing.date);
		signing.signed_body_value =
			g_aws_signed_body_value_empty_sha256;
		signing.signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;
		signing.credentials = keys;
		if (aws_sign_request_aws(allocator, signable,
			(const aws_signing_config_base *)&signing,
			crt_signed, &state))
			status = lx_fail(error, LAUNCHER_NETWORK,
				"Signing S3 request");
		else {
			crt_wait(&state, &state.sign_done, transfer);
			if (state.native_error)
				status = lx_fail(error, LAUNCHER_NETWORK,
					"Signing S3 request");
		}
	}
	aws_socket_options socket = {};

	socket.type = AWS_SOCKET_STREAM;
	socket.domain = memchr(uri.host_name.ptr, ':', uri.host_name.len) ?
		AWS_SOCKET_IPV6 : AWS_SOCKET_IPV4;
	socket.connect_timeout_ms = 30000;
	aws_http_connection_monitoring_options monitoring = {};

	monitoring.minimum_throughput_bytes_per_second = 1;
	monitoring.allowable_throughput_failure_interval_seconds = 60;
	aws_http1_connection_options http1 = {};

	http1.read_buffer_capacity = chunk_size;
	if (status == LAUNCHER_OK && crt_progress(transfer)) {
		aws_http_client_connection_options connection_options = {};

		connection_options.self_size = sizeof(connection_options);
		connection_options.allocator = allocator;
		connection_options.bootstrap = bootstrap;
		connection_options.host_name = uri.host_name;
		connection_options.port = uri.port ? uri.port : 443;
		connection_options.socket_options = &socket;
		connection_options.tls_options = &tls_connection;
		connection_options.monitoring_options = &monitoring;
		connection_options.response_first_byte_timeout_ms = 60000;
		connection_options.manual_window_management = true;
		connection_options.initial_window_size = chunk_size;
		connection_options.http1_options = &http1;
		connection_options.user_data = &state;
		connection_options.on_setup = crt_setup;
		connection_options.on_shutdown = crt_shutdown;
		if (aws_http_client_connect(&connection_options))
			status = lx_fail(error, LAUNCHER_NETWORK,
				"Connecting signed transport");
		else {
			crt_wait(&state, &state.setup_done, transfer);
			if (state.native_error)
				status = lx_fail(error, LAUNCHER_NETWORK,
					"Connecting signed transport");
		}
	}
	if (status == LAUNCHER_OK && transfer->failure == LAUNCHER_OK) {
		aws_http_make_request_options request_options = {};

		request_options.self_size = sizeof(request_options);
		request_options.request = state.request;
		request_options.user_data = &state;
		request_options.on_response_headers = crt_headers;
		request_options.on_response_header_block_done =
			crt_headers_done;
		request_options.on_response_body = crt_body;
		request_options.on_complete = crt_complete;
		request_options.on_destroy = crt_destroyed;
		stream = aws_http_connection_make_request(state.connection,
			&request_options);
		if (!stream || aws_http_stream_activate(stream))
			status = lx_fail(error, LAUNCHER_NETWORK,
				"Starting signed request");
	}
	if (status == LAUNCHER_OK && stream) {
		unsigned char chunk[chunk_size];
		int cancelled = 0;

		for (;;) {
			aws_mutex_lock(&state.mutex);
			if (!state.queued && !state.stream_done) {
				aws_condition_variable_wait_for(
					&state.condition, &state.mutex,
					100000000);
			}
			size_t got = state.queued;

			memcpy(chunk, state.queue, got);
			state.queued = 0;
			int completed = state.stream_done;
			transfer->response = state.response;

			aws_mutex_unlock(&state.mutex);
			if (got && transfer->failure == LAUNCHER_OK)
				receive_bytes((char *)chunk, 1, got, transfer);
			crt_progress(transfer);
			if (transfer->failure != LAUNCHER_OK && !cancelled) {
				aws_http_stream_cancel_default_error(stream);
				cancelled = 1;
			}
			if (completed)
				break;
			/* Return credit after the caller consumes the copy.
			 * Queued bytes and credit total at most 64 KiB.
			 */
			if (got && !cancelled)
				aws_http_stream_update_window(stream, got);
		}
	}
	if (status != LAUNCHER_OK && !state.native_error)
		state.native_error = aws_last_error();
	if (stream) {
		aws_http_stream_release(stream);
		crt_wait(&state, &state.stream_destroyed, NULL);
	}
	if (state.connection) {
		aws_http_connection_close(state.connection);
		crt_wait(&state, &state.connection_closed, NULL);
		aws_http_connection_release(state.connection);
	}
	if (bootstrap) {
		aws_client_bootstrap_release(bootstrap);
		crt_wait(&state, &state.bootstrap_closed, NULL);
	}
	if (resolver) {
		aws_host_resolver_release(resolver);
		crt_wait(&state, &state.resolver_closed, NULL);
	}
	if (loops) {
		aws_event_loop_group_release(loops);
		crt_wait(&state, &state.loops_closed, NULL);
	}
	if (transfer->failure == LAUNCHER_CANCELLED)
		status = lx_fail(error, LAUNCHER_CANCELLED,
			"Fetching %s cancelled", relative);
	else if (state.bad_encoding)
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Fetching %s returned nonidentity Content-Encoding",
			relative);
	else if (state.response && state.response != 200)
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Fetching %s requires HTTP 200; received %d", relative,
			state.response);
	else if (transfer->failure != LAUNCHER_OK)
		status = lx_fail(error, transfer->failure,
			"Receiving %s failed: bounded stream or I/O", relative);
	else if (status == LAUNCHER_OK && state.native_error)
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Fetching signed resource %s failed", relative);
	else if (status == LAUNCHER_OK && state.response != 200)
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Fetching %s requires HTTP 200; received %d", relative,
			state.response);
	if (error && status != LAUNCHER_OK) {
		error->native_code = state.native_error ? state.native_error :
			(transfer->failure != LAUNCHER_OK ?
				AWS_ERROR_HTTP_STREAM_CANCELLED : 0);
		error->http_status = state.response;
	}
	aws_signable_destroy(signable);
	aws_credentials_release(keys);
	aws_http_message_release(state.request);
	aws_tls_connection_options_clean_up(&tls_connection);
	aws_tls_ctx_release(tls);
	aws_tls_ctx_options_clean_up(&tls_options);
	aws_condition_variable_clean_up(&state.condition);
	aws_mutex_clean_up(&state.mutex);
	aws_uri_clean_up(&uri);
	return status;
}

static launcher_status http_get(const launcher_config *config,
	const launcher_s3_credentials *credentials, const char *relative,
	http_transfer *transfer, launcher_error *error)
{
	if (!config || !config->base_url || !relative)
		return lx_fail(error, LAUNCHER_INVALID,
			"HTTP configuration required");
	if (config->s3_region && *config->s3_region) {
		return crt_http_get(config, credentials, relative, transfer,
			error);
	}
	static const CURLcode initialized =
		curl_global_init(CURL_GLOBAL_DEFAULT);

	if (initialized != CURLE_OK)
		return lx_fail(error, LAUNCHER_NETWORK, "Initializing cURL");
	size_t base_length = strlen(config->base_url);
	size_t relative_length = strlen(relative);
	char *url = (char *)malloc(base_length + relative_length + 1);

	if (!url)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Allocating resource URL");
	memcpy(url, config->base_url, base_length);
	memcpy(url + base_length, relative, relative_length + 1);
	CURL *curl = curl_easy_init();
	struct curl_slist *headers = NULL;
	launcher_status status = LAUNCHER_OK;
	char diagnostic[CURL_ERROR_SIZE] = {};
	long response = 0;

	if (!curl) {
		free(url);
		return lx_fail(error, LAUNCHER_MEMORY, "Creating cURL request");
	}
	headers = curl_slist_append(headers, "Cache-Control: no-cache");
	if (!headers) {
		curl_easy_cleanup(curl);
		free(url);
		return lx_fail(error, LAUNCHER_MEMORY, "Creating HTTP headers");
	}
	CURLcode code = CURLE_OK;
#define LX_CURL(option, value) \
	if (code == CURLE_OK) \
		code = curl_easy_setopt(curl, option, value)
	LX_CURL(CURLOPT_URL, url);
	LX_CURL(CURLOPT_PROTOCOLS_STR, "http,https");
	LX_CURL(CURLOPT_FOLLOWLOCATION, 0L);
	LX_CURL(CURLOPT_NOSIGNAL, 1L);
	LX_CURL(CURLOPT_CONNECTTIMEOUT, 30L);
	LX_CURL(CURLOPT_LOW_SPEED_LIMIT, 1L);
	LX_CURL(CURLOPT_LOW_SPEED_TIME, 60L);
	LX_CURL(CURLOPT_ACCEPT_ENCODING, "identity");
	LX_CURL(CURLOPT_HTTP_CONTENT_DECODING, 0L);
	LX_CURL(CURLOPT_SSL_VERIFYPEER, 1L);
	LX_CURL(CURLOPT_SSL_VERIFYHOST, 2L);
	LX_CURL(CURLOPT_HTTPHEADER, headers);
	LX_CURL(CURLOPT_USERAGENT, "launcher/1");
	LX_CURL(CURLOPT_WRITEFUNCTION, receive_bytes);
	LX_CURL(CURLOPT_WRITEDATA, transfer);
	LX_CURL(CURLOPT_HEADERFUNCTION, receive_header);
	LX_CURL(CURLOPT_HEADERDATA, transfer);
	LX_CURL(CURLOPT_NOPROGRESS, 0L);
	LX_CURL(CURLOPT_XFERINFOFUNCTION, transfer_progress);
	LX_CURL(CURLOPT_XFERINFODATA, transfer);
	LX_CURL(CURLOPT_ERRORBUFFER, diagnostic);
	if (config->ca_bundle && *config->ca_bundle)
		LX_CURL(CURLOPT_CAINFO, config->ca_bundle);
#undef LX_CURL
	if (code == CURLE_OK)
		code = curl_easy_perform(curl);
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response);
	if (transfer->failure == LAUNCHER_CANCELLED)
		status = lx_fail(error, LAUNCHER_CANCELLED,
			"Fetching %s cancelled", relative);
	else if (transfer->bad_encoding)
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Fetching %s returned nonidentity Content-Encoding",
			relative);
	else if (response && response != 200)
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Fetching %s requires HTTP 200; received %ld", relative,
			response);
	else if (transfer->failure != LAUNCHER_OK)
		status = lx_fail(error, transfer->failure,
			"Receiving %s failed: bounded stream or I/O",
			relative);
	else if (code != CURLE_OK)
		status = lx_fail(error, LAUNCHER_NETWORK, "Fetching %s: %s",
			relative, diagnostic[0] ? diagnostic :
			curl_easy_strerror(code));
	else if (response != 200)
		status = lx_fail(error, LAUNCHER_NETWORK,
			"Fetching %s requires HTTP 200; received %ld", relative,
			response);
	if (error && status != LAUNCHER_OK) {
		error->native_code = transfer->bad_encoding ?
			CURLE_BAD_CONTENT_ENCODING : (int)code;
		error->http_status = response;
	}
	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);
	free(url);
	return status;
}

launcher_status lx_http_bytes(const launcher_config *config,
	const launcher_s3_credentials *credentials,
	const char *relative, size_t limit, char **out, size_t *size,
	launcher_progress_fn callback, void *state, launcher_error *error)
{
	if (out)
		*out = NULL;
	if (size)
		*size = 0;
	if (!out || !size || limit == SIZE_MAX)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid metadata retrieval");
	http_transfer transfer = {};

	transfer.limit = limit;
	transfer.expected = limit;
	transfer.callback = callback;
	transfer.state = state;
	transfer.path = relative;
	transfer.phase = LAUNCHER_RESOLVE;
	launcher_status status = http_get(config, credentials, relative,
		&transfer, error);

	if (status == LAUNCHER_OK && !transfer.buffer) {
		transfer.buffer = (char *)malloc(1);
		if (!transfer.buffer)
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Allocating empty metadata response");
	}
	if (status == LAUNCHER_OK) {
		transfer.buffer[transfer.count] = 0;
		*out = transfer.buffer;
		*size = (size_t)transfer.count;
	} else {
		free(transfer.buffer);
	}
	return status;
}

launcher_status lx_http_object(const launcher_config *config,
	const launcher_s3_credentials *credentials,
	const char *remote, const char *digest, uint64_t size, lx_fs *cache,
	launcher_progress_fn callback, void *state, size_t done, size_t total,
	launcher_error *error)
{
	char relative[80], temporary[85];

	if (!digest || strlen(digest) != 64)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid object digest");
	snprintf(relative, sizeof(relative), "objects/%s", digest);
	snprintf(temporary, sizeof(temporary), "objects/%s.part", digest);
	launcher_status status = lx_fs_mkdirs(cache, "objects", error);

	if (status == LAUNCHER_OK)
		status = lx_fs_remove(cache, temporary, 0, error);
	FILE *file = NULL;

	if (status == LAUNCHER_OK)
		status = lx_fs_write(cache, temporary, 1, &file, error);
	if (status != LAUNCHER_OK)
		return status;
	mbedtls_sha256_context hash;
	mbedtls_sha256_init(&hash);
	mbedtls_sha256_starts(&hash, 0);
	http_transfer transfer = {};

	transfer.file = file;
	transfer.hash = &hash;
	transfer.expected = size;
	transfer.callback = callback;
	transfer.state = state;
	transfer.path = remote;
	transfer.phase = LAUNCHER_DOWNLOAD;
	transfer.objects_done = done;
	transfer.objects_total = total;
	status = http_get(config, credentials, remote, &transfer, error);
	unsigned char bytes[32];
	char actual[65];

	if (status == LAUNCHER_OK &&
		(transfer.count != size || mbedtls_sha256_finish(&hash, bytes)))
		status = lx_fail(error, LAUNCHER_INTEGRITY,
			"Object %s has incorrect length", digest);
	if (status == LAUNCHER_OK) {
		hex_digest(bytes, actual);
		if (strcmp(actual, digest))
			status = lx_fail(error, LAUNCHER_INTEGRITY,
				"Object %s has incorrect SHA-256", digest);
	}
	if (status == LAUNCHER_OK)
		status = lx_file_flush(file, error);
	if (fclose(file) && status == LAUNCHER_OK)
		status = lx_fail(error, LAUNCHER_IO,
			"Closing object %s", digest);
	mbedtls_sha256_free(&hash);
	if (status == LAUNCHER_OK)
		status = lx_fs_replace(cache, temporary, cache, relative,
			error);
	if (status != LAUNCHER_OK)
		lx_fs_remove(cache, temporary, 0, NULL);
	return status;
}
