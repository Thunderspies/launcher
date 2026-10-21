#ifndef LAUNCHER_LAUNCHER_H
#define LAUNCHER_LAUNCHER_H

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#ifdef LAUNCHER_SHARED
#ifdef LAUNCHER_BUILD
#define LAUNCHER_API __declspec(dllexport)
#else
#define LAUNCHER_API __declspec(dllimport)
#endif
#else
#define LAUNCHER_API
#endif
#else
#define LAUNCHER_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/*
	* Native platforms are Linux and Windows 10 or newer. Fallible
	* operations return statuses and accept a nullable diagnostic. No
	* operation throws. Handles are serialized by the caller. Independent
	* installations may be used concurrently. Callbacks run synchronously
	* and must not throw or reenter the same handle. Strings are UTF-8;
	* publisher names are UTF-8 with ASCII letters folded to lowercase.
	*/
typedef enum launcher_status {
	LAUNCHER_OK = 0,
	LAUNCHER_INVALID,
	LAUNCHER_METADATA,
	LAUNCHER_NETWORK,
	LAUNCHER_INTEGRITY,
	LAUNCHER_IO,
	LAUNCHER_CONFLICT,
	LAUNCHER_BUSY,
	LAUNCHER_CANCELLED,
	LAUNCHER_RECOVERY_REQUIRED,
	LAUNCHER_NOT_INSTALLED,
	LAUNCHER_COMMITTED,
	LAUNCHER_MEMORY,
	LAUNCHER_UNSUPPORTED
} launcher_status;

/*
	* Caller-owned diagnostic, overwritten by each operation. COMMITTED
	* means the new release is installed, but durable cleanup must be
	* retried. OK can retain a maintenance warning in message when optional
	* archive compaction failed after a completed installation.
	*/
typedef struct launcher_error {
	launcher_status status;
	int native_code;
	long http_status;
	char message[512];
} launcher_error;

/*
	* Library-owned opaque objects, released by their matching close/free
	* API.
	*/
typedef struct launcher_handle launcher_handle;
typedef struct launcher_release launcher_release;
typedef struct launcher_plan launcher_plan;

typedef enum launcher_file_kind {
	LAUNCHER_HOGG = 0,
	LAUNCHER_LOOSE = 1
} launcher_file_kind;

/* Stored object representation; loose records always use RAW. */
typedef enum launcher_encoding {
	LAUNCHER_RAW = 0,
	LAUNCHER_ZLIB = 1
} launcher_encoding;

/*
	* Borrowed release-owned strings remain valid until release_free.
	* size is logical; stored_size is the downloadable object length.
	* sha256 always identifies exact stored bytes. logical_md5 is the
	* lowercase logical digest from the descriptor for either file kind.
	* fingerprint identifies the exact canonical descriptor bytes.
	*/
typedef struct launcher_file {
	const char *path;
	const char *sha256;
	uint64_t size;
	uint64_t stored_size;
	launcher_encoding encoding;
	const char *logical_md5;
	const char *fingerprint;
} launcher_file;

/* Borrowed launch data, owned by its release. */
typedef struct launcher_launch_info {
	const char *entry_point;
	const char *const *run_arguments;
	size_t run_argument_count;
} launcher_launch_info;

/*
	* Ordered file endpoint. URL normalization, signing and trust follow the
	* manifest endpoint rules. Credentials are independent and runtime-only.
	*/
typedef struct launcher_file_repository {
	const char *base_url;
	const char *s3_region;
} launcher_file_repository;

/*
	* Complete settings. base_url is the HTTP(S) release-collection root
	* URL.
	* File endpoints are explicit; there is no implicit manifest fallback.
	* file_repositories may be NULL only when file_repository_count is zero;
	* the maximum count is 4096.
	* ca_bundle applies to every endpoint.
	* channel defaults to live when NULL. game is a relative loose-file
	* destination, or NULL to use the installed release's entry point.
	* Empty optional strings also use that default. game_args follow
	* published arguments when using the release entry point; a game
	* override uses local args. runner is optional (for example wine).
	* Argument arrays may be NULL only when their counts are zero.
	* Environment is inherited.
	*/
typedef struct launcher_config {
	const char *base_url;
	const char *channel;
	const char *game;
	const char *runner;
	const char *ca_bundle;
	const char *const *runner_args;
	size_t runner_arg_count;
	const char *const *game_args;
	size_t game_arg_count;
	/*
		* NULL/empty uses unsigned HTTP(S); otherwise HTTPS and SigV4.
		* Region: up to 63 lowercase ASCII letters, digits or '-'.
		* Use the bucket's AWS region or auto for Cloudflare R2.
		*/
	const char *s3_region;
	const launcher_file_repository *file_repositories;
	size_t file_repository_count;
} launcher_config;

/*
	* Runtime-only S3/R2 credentials. Both keys must be nonempty. The
	* optional session token may be NULL/empty. Strings must contain no
	* control bytes.
	*/
typedef struct launcher_s3_credentials {
	const char *access_key_id;
	const char *secret_access_key;
	const char *session_token;
} launcher_s3_credentials;

typedef enum launcher_phase {
	LAUNCHER_RESOLVE,
	LAUNCHER_VERIFY,
	LAUNCHER_DOWNLOAD,
	LAUNCHER_STAGE,
	LAUNCHER_ACTIVATE,
	LAUNCHER_RUN
} launcher_phase;

/*
	* path is callback-borrowed. total/completed are bytes for I/O and
	* entries for verification/staging. Returning zero requests
	* cancellation at a recoverable boundary. Before pending intent,
	* active files are untouched; afterward reopen resumes the frozen
	* target before launch. Compaction reports coarse STAGE progress and
	* cannot be cancelled once its Piggle operation has started.
	*/
typedef struct launcher_progress {
	launcher_phase phase;
	const char *path;
	uint64_t completed;
	uint64_t total;
	size_t objects_completed;
	size_t objects_total;
} launcher_progress;
typedef int (*launcher_progress_fn)(
	const launcher_progress *progress, void *state);

/*
	* Missing distinct objects and stored download bytes; unchanged plans
	* have zero missing objects/bytes and require no cache population.
	*/
typedef struct launcher_plan_info {
	size_t missing_objects;
	uint64_t download_bytes;
	size_t hogg_files;
	size_t loose_files;
	int unchanged;
} launcher_plan_info;

/*
	* Human-readable static status name; no ownership or side effects.
	*/
LAUNCHER_API const char *launcher_status_name(launcher_status status);

/*
	* Synchronous descriptor lookup for a lowercase SHA-256 fingerprint.
	* On OK, set *data and *size to borrowed exact bytes, valid until the
	* next callback or decoder return. The decoder validates and copies
	* them. Errors abort decoding; callbacks may fill the diagnostic.
	* Called once per distinct fingerprint in lexical order, after all
	* release group structure passes validation. Names are validated after
	* lookup. Must not throw.
	*/
typedef launcher_status (*launcher_descriptor_fn)(const char *fingerprint,
	const void **data, size_t *size, void *state, launcher_error *error);

/*
	* Pure bounded decoders. Channel/expected_release are canonical IDs.
	* Format-1 channels reference immutable format-2 releases. Named HOGG
	* groups have path/files; top-level loose lists loose fingerprints.
	* There is no committed/draft field. Callbacks may be NULL only for
	* empty releases. Successful releases retain exact descriptors and
	* published launch settings. Fingerprint mismatch returns INTEGRITY.
	* Schema or canonical byte violations return METADATA. Failure clears
	* output.
	* release_free accepts NULL and clears *out.
	*/
LAUNCHER_API launcher_status launcher_decode_channel(const void *data,
	size_t size, const char *channel, char release[65],
	launcher_error *error);
LAUNCHER_API launcher_status launcher_decode_release(const void *data,
	size_t size, const char *expected_release,
	launcher_descriptor_fn descriptor, void *state, launcher_release **out,
	launcher_error *error);
LAUNCHER_API void launcher_release_free(launcher_release **release);
LAUNCHER_API const char *launcher_release_id(const launcher_release *release);
/* Group and file strings borrow from their release until release_free. */
typedef struct launcher_file_group {
	const char *name;
	launcher_file_kind kind;
	/* NULL for synthesized loose group; physical path for HOGG groups. */
	const char *hogg_path;
	size_t file_count;
} launcher_file_group;

/* Outputs are cleared on INVALID. Groups and files use canonical order. */
LAUNCHER_API launcher_status launcher_release_get_launch(
	const launcher_release *release, launcher_launch_info *out,
	launcher_error *error);
/* Nonempty flat loose selections appear as one group named "loose". */
LAUNCHER_API size_t launcher_release_group_count(
	const launcher_release *release);
LAUNCHER_API launcher_status launcher_release_group(
	const launcher_release *release, size_t index,
	launcher_file_group *out, launcher_error *error);
LAUNCHER_API launcher_status launcher_release_group_file(
	const launcher_release *release, size_t group_index, size_t file_index,
	launcher_file *out, launcher_error *error);

/*
	* Open creates/validates <install>.launcher, locks the installation and
	* recovers interrupted activation offline. Existing unrelated sidecars,
	* symlinks/reparse points and cross-filesystem staging are rejected. out
	* must point to NULL. Close releases resources and clears *handle.
	*/
LAUNCHER_API launcher_status launcher_open(
	const char *install, launcher_handle **out, launcher_error *error);
LAUNCHER_API void launcher_close(launcher_handle **handle);

/*
	* Configure atomically saves a complete copied configuration. Getter
	* strings are borrowed until configure/close. Installed release is
	* borrowed until successful apply/close; NOT_INSTALLED sets *out to
	* NULL.
	*/
LAUNCHER_API launcher_status launcher_configure(launcher_handle *handle,
	const launcher_config *config, launcher_error *error);
LAUNCHER_API launcher_status launcher_get_config(
	launcher_handle *handle, launcher_config *out, launcher_error *error);
/*
	* Atomically copy credentials for a configured signed repository. NULL
	* clears credentials on any usable handle. Failure preserves existing
	* credentials. Credentials are never persisted or returned by getters;
	* close or changing the normalized repository URL/region clears them.
	* Rotation preserves plans. Callers serialize access to the handle.
	* Missing credentials fail with INVALID before a signed request.
	*/
LAUNCHER_API launcher_status launcher_set_s3_credentials(
	launcher_handle *handle, const launcher_s3_credentials *credentials,
	launcher_error *error);
/*
	* Same credential contract for the zero-based file endpoint index.
	* Index must be less than the configured count, including when clearing.
	* Configuration replaces the list and clears credentials at any index
	* whose normalized URL or region changes. Rotation preserves plans.
	*/
LAUNCHER_API launcher_status launcher_set_file_s3_credentials(
	launcher_handle *handle, size_t index,
	const launcher_s3_credentials *credentials, launcher_error *error);
LAUNCHER_API launcher_status launcher_get_installed(launcher_handle *handle,
	const launcher_release **out, launcher_error *error);

/*
	* Resolve fetches a fresh channel once, then its complete immutable
	* format-2 manifest and root revisions/<fingerprint>.json descriptors.
	* Authoring history is not consulted. Lookup advances only on HTTP 404;
	* payloads use the selected file endpoint. Plan retains a copied target,
	* checks accepted cache presence and stored length, establishes sources
	* for missing objects, and hashes first-adopted installed stored bytes.
	* Saved release/channel identity and inventory determine unchanged
	* updates before unnecessary cache population. Neither operation changes
	* active game files. Outputs are NULL on failure. Plans belong to their
	* open installation and must be applied before another apply/configure
	* operation on that handle, or verify removing corrupt cache objects.
	*/
LAUNCHER_API launcher_status launcher_resolve(launcher_handle *handle,
	launcher_release **out, launcher_progress_fn progress, void *state,
	launcher_error *error);
LAUNCHER_API launcher_status launcher_make_plan(launcher_handle *handle,
	const launcher_release *target, launcher_plan **out,
	launcher_progress_fn progress, void *state, launcher_error *error);
LAUNCHER_API launcher_status launcher_plan_get_info(const launcher_plan *plan,
	launcher_plan_info *out, launcher_error *error);
LAUNCHER_API void launcher_plan_free(launcher_plan **plan);

/*
	* Apply accepts missing objects after stored length/SHA-256 and durable
	* writes, then saves the frozen target and ownership in a forward
	* journal. Existing valid HOGG10 archives receive only changed entries;
	* new archives and loose replacements are staged. Piggle preserves
	* encoded bytes and validates changed existing-source input. Interrupted
	* updates resume offline before launch. Installed state is saved before
	* touched archives compact at 20 percent estimated reclaimable space.
	* Maintenance can warn on OK; integrity/conflict remain errors. Recovery
	* failure blocks use. See docs/api.md for publication boundaries.
	*/
LAUNCHER_API launcher_status launcher_apply(launcher_handle *handle,
	launcher_plan *plan, launcher_progress_fn progress, void *state,
	launcher_error *error);

/*
	* High-level compositions. Update resolves a channel; repair uses only
	* the saved installed target and always rebuilds, bypassing unchanged
	* detection (network may supply missing objects).
	* Verify hashes installed stored bytes and present final cache objects
	* offline without decoding payloads. It checks archive membership,
	* encodings, lengths and supplied MD5 metadata, returning INTEGRITY
	* for corruption. Proven-corrupt cache copies are discarded; removal
	* invalidates existing plans.
	*/
LAUNCHER_API launcher_status launcher_update(launcher_handle *handle,
	launcher_progress_fn progress, void *state, launcher_error *error);
LAUNCHER_API launcher_status launcher_repair(launcher_handle *handle,
	launcher_progress_fn progress, void *state, launcher_error *error);
LAUNCHER_API launcher_status launcher_verify(launcher_handle *handle,
	launcher_progress_fn progress, void *state, launcher_error *error);

/*
	* Launch trusts accepted installed state and checks directory identity,
	* the safe regular entry point, recovery and process state offline.
	* Other installed files are not audited. Local entry-point content and
	* size changes are allowed; use verify/repair for integrity checks.
	* An empty configured game selects the saved
	* release entry point and arguments. The entry point must be a managed
	* loose file. Spawn order: runner, runner_args, entry point, published
	* arguments (when selected), game_args, extra_args. Waits while holding
	* the lock. A surviving process marker blocks
	* other launchers after a parent crash. The runner must stay alive for
	* the complete game session. exit_code is set to -1 on failure.
	* Concurrent external games are caller's responsibility. Native POSIX
	* game files receive their executable bit.
	*/
LAUNCHER_API launcher_status launcher_launch(launcher_handle *handle,
	const char *const *extra_args, size_t extra_count, int *exit_code,
	launcher_error *error);

#ifdef __cplusplus
}
#endif
#endif
