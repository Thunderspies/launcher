#ifndef LAUNCHER_INTERNAL_H
#define LAUNCHER_INTERNAL_H

#include <launcher/launcher.h>
#include <piggle/piggle.h>
#include <stdio.h>

struct lx_entry {
	launcher_file_kind kind;
	char *path;
	char digest[65];
	char fingerprint[65];
	uint64_t size;
	uint64_t stored_size;
	launcher_encoding encoding;
	char logical_md5[33];
	/* Borrowed from the release-owned distinct descriptor. */
	unsigned char *cached_header;
	size_t cached_header_size;
	char cached_header_sha256[65];
};

struct lx_descriptor {
	char fingerprint[65];
	char *json;
	size_t size;
	unsigned char *cached_header;
};

struct lx_descriptor_cache {
	lx_descriptor descriptor;
	size_t repository;
	lx_descriptor_cache *next;
};

struct lx_group {
	char *name;
	launcher_file_kind kind;
	char *path;
	size_t first;
	size_t count;
};

struct launcher_release {
	lx_group *groups;
	size_t group_count;
	int borrowed_json;
	char *descriptor_backing;
	lx_descriptor *descriptors;
	size_t descriptor_count;
	char id[65];
	char *hogg;
	lx_entry *hogg_files;
	size_t hogg_count;
	lx_entry *loose_files;
	size_t loose_count;
	char *json;
	size_t json_size;
	char *channel_json;
	char *entry_point;
	const char **run_arguments;
	size_t run_argument_count;
};

struct lx_fs;

struct launcher_handle {
	lx_fs *install;
	lx_fs *private_fs;
	char *root;
	char *private_root;
	launcher_config config;
	launcher_s3_credentials s3_credentials;
	launcher_s3_credentials *file_credentials;
	lx_descriptor_cache *descriptors[4096];
	launcher_release *installed;
	char installed_tx[65];
	char **owned_dirs;
	size_t owned_dir_count;
	uint64_t epoch;
	int blocked;
};

struct lx_object {
	char digest[65];
	char fingerprint[65];
	size_t repository;
	uint64_t size;
	char remote[160];
	int available;
};

struct lx_destination {
	const char *path;
	size_t group;
	size_t entry;
	int retained;
	int incremental;
	size_t previous_group;
};

struct launcher_plan {
	launcher_handle *owner;
	uint64_t epoch;
	launcher_release *target;
	lx_object *objects;
	lx_destination *destinations;
	size_t destination_count;
	size_t object_count;
	launcher_plan_info info;
};

launcher_status lx_fail(
	launcher_error *error, launcher_status status, const char *format, ...);
void lx_clear(launcher_error *error);
void lx_crypto_initialize(void);
launcher_status lx_digest_bytes(const void *data, size_t size, char hex[65],
	launcher_error *error);
char *lx_strdup(const char *text);
int lx_identifier(const char *text);
int lx_path_valid(const char *text, int lowercase);
int lx_progress(launcher_progress_fn callback, void *state,
	launcher_phase phase, const char *path, uint64_t done, uint64_t total,
	size_t objects_done, size_t objects_total);
/* Saved-state JSON borrows the retained mutable parser input. */
launcher_status lx_decode_saved_release(const void *data, size_t size,
	const char *expected_release, launcher_descriptor_fn descriptor,
	void *state, launcher_release **out, launcher_error *error);
launcher_status lx_clone_release(const launcher_release *source,
	launcher_release **out, launcher_error *error);
/* Attach a validated channel snapshot to a fresh matching release. */
launcher_status lx_release_channel(launcher_release *release,
	const void *data, size_t size, const char *channel,
	launcher_error *error);
/*
	* Validate exact named-revision identity, canonical schema and lengths.
	* path and cached_header start NULL. The caller releases either
	* allocation with free() on success or failure. Other metadata fields
	* are unspecified on failure.
	*/
launcher_status lx_decode_descriptor(const void *data, size_t size,
	const char *fingerprint, lx_entry *out, launcher_error *error);
/* Borrow a retained descriptor for cloning or offline state encoding. */
launcher_status lx_release_descriptor(const char *fingerprint,
	const void **data, size_t *size, void *state, launcher_error *error);
void lx_group_view(const launcher_release *release, size_t index,
	launcher_release *out);
size_t lx_physical_count(const launcher_release *release);
const char *lx_physical_path(const launcher_release *release, size_t index);
int lx_same_release(const launcher_release *a, const launcher_release *b);
int lx_same_channel(const launcher_release *a, const launcher_release *b);

/*
	* Platform filesystem operations are anchored to retained directory
	* handles. Relative paths may name internal staging files; they still
	* reject . and .. and symlink/reparse components. Missing stat: kind=0;
	* regular=1; directory=2.
	*/
launcher_status lx_fs_open(
	const char *path, int create, lx_fs **out, launcher_error *error);
void lx_fs_close(lx_fs **fs);
const char *lx_fs_path(lx_fs *fs);
launcher_status lx_fs_identity(
	lx_fs *fs, char out[128], launcher_error *error);
launcher_status lx_fs_validate(lx_fs *fs, launcher_error *error);
launcher_status lx_fs_same_volume(lx_fs *a, lx_fs *b, launcher_error *error);
launcher_status lx_fs_lock(lx_fs *fs, launcher_error *error);
launcher_status lx_fs_stat(lx_fs *fs, const char *path, int *kind,
	uint64_t *size, launcher_error *error);
launcher_status lx_fs_mkdirs(
	lx_fs *fs, const char *path, launcher_error *error);
/* Apply requested directory case after creating its parents. */
launcher_status lx_fs_case(
	lx_fs *fs, const char *path, launcher_error *error);
launcher_status lx_fs_read(
	lx_fs *fs, const char *path, FILE **out, launcher_error *error);
launcher_status lx_fs_write(lx_fs *fs, const char *path, int exclusive,
	FILE **out, launcher_error *error);
launcher_status lx_fs_move(lx_fs *from, const char *source, lx_fs *to,
	const char *dest, launcher_error *error);
launcher_status lx_fs_replace(lx_fs *from, const char *source, lx_fs *to,
	const char *dest, launcher_error *error);
/*
	* Retain parent until the caller closes external native file users.
	*/
launcher_status lx_fs_pin(lx_fs *fs, const char *path, int createparents,
	lx_fs **parent, char **absolute, launcher_error *error);
launcher_status lx_fs_remove(
	lx_fs *fs, const char *path, int directory, launcher_error *error);
launcher_status lx_fs_flush(lx_fs *fs, launcher_error *error);
launcher_status lx_file_flush(FILE *file, launcher_error *error);
launcher_status lx_fs_empty(
	lx_fs *fs, const char *path, int *empty, launcher_error *error);
/*
	* Lists immediate names without following links: 1=file, 2=dir, 3=other.
	* Empty path denotes the retained root. Zero callback return stops
	* CONFLICT.
	*/
launcher_status lx_fs_list(lx_fs *fs, const char *path,
	int (*visit)(const char *name, int kind, void *state), void *state,
	launcher_error *error);
launcher_status lx_fs_spell(
	lx_fs *fs, const char *path, char **out, launcher_error *error);
launcher_status lx_fs_executable(
	lx_fs *fs, const char *path, launcher_error *error);
launcher_status lx_process_busy(lx_fs *state, launcher_error *error);
launcher_status lx_process_run(lx_fs *state, const char *cwd,
	const char *const *args, size_t count, int *exit_code,
	launcher_error *error);

/*
	* Bounded byte I/O and hashing: output buffers are malloc-owned.
	*/
launcher_status lx_read_bytes(lx_fs *fs, const char *path, size_t limit,
	char **out, size_t *size, launcher_error *error);
launcher_status lx_save_bytes(lx_fs *fs, const char *path, const void *data,
	size_t size, launcher_error *error);
launcher_status lx_hash_file(FILE *file, uint64_t expected, const char *digest,
	FILE *copy, launcher_error *error);
/* digest NULL copies accepted bytes with length/durability checks only. */
launcher_status lx_copy_object(lx_fs *from, const char *source, lx_fs *to,
	const char *dest, uint64_t size, const char *digest,
	launcher_error *error);
launcher_status lx_http_bytes(const launcher_config *config,
	const launcher_s3_credentials *credentials,
	const char *relative, size_t limit, char **out, size_t *size,
	launcher_progress_fn callback, void *state, launcher_error *error);
launcher_status lx_http_object(const launcher_config *config,
	const launcher_s3_credentials *credentials,
	const char *relative, const char *digest, uint64_t size, lx_fs *cache,
	launcher_progress_fn callback, void *state, size_t done, size_t total,
	launcher_error *error);
launcher_status lx_cache_installed(launcher_handle *handle, launcher_plan *plan,
	launcher_progress_fn callback, void *state, launcher_error *error);
launcher_status lx_stage_hogg(launcher_handle *handle,
	const launcher_release *release, const char *dest,
	launcher_progress_fn callback, void *state, launcher_error *error);
const lx_entry *lx_find_entry(const launcher_release *release,
	const char *name);
int lx_same_payload(const lx_entry *a, const lx_entry *b);
launcher_status lx_patch_hogg(launcher_handle *handle,
	const launcher_release *before, const launcher_release *target,
	launcher_progress_fn callback, void *state, launcher_error *error);
launcher_status lx_compact_hogg(launcher_handle *handle, const char *path,
	launcher_error *error);
launcher_status lx_check_destination(launcher_handle *handle,
	const launcher_release *release, const lx_destination *destination,
	const char *prefix, launcher_progress_fn callback, void *state,
	launcher_error *error);
lx_destination *lx_destinations(const launcher_release *release,
	size_t *count);
int lx_same_destination(const launcher_release *a, const lx_destination *left,
	const launcher_release *b, const lx_destination *right);
const lx_entry *lx_destination_entries(const launcher_release *release,
	const lx_destination *destination, size_t *count);
launcher_status lx_verify_cache(launcher_handle *handle,
	launcher_progress_fn callback, void *state, launcher_error *error);
launcher_status lx_check_release(launcher_handle *handle,
	const launcher_release *release, const char *hogg_override,
	const char *loose_prefix, launcher_progress_fn callback, void *state,
	launcher_error *error);
launcher_status lx_verify_release(launcher_handle *handle,
	const launcher_release *release, const char *hogg_override,
	const char *loose_prefix, launcher_progress_fn callback, void *state,
	launcher_error *error);

launcher_status lx_state_load(launcher_handle *handle, launcher_error *error);
launcher_status lx_transaction_recover(
	launcher_handle *handle, launcher_error *error);
launcher_status lx_transaction_preflight(launcher_handle *handle,
	const launcher_release *target, launcher_error *error);
launcher_status lx_transaction_apply(launcher_handle *handle,
	launcher_plan *plan, launcher_progress_fn callback, void *state,
	launcher_error *error);

#endif
