# Launcher API contract

The public declarations in `include/launcher/launcher.h` define the C11
interface. `launcher.hpp` imports those same free functions and types into
the `launcher` namespace for C++17; it does not introduce a separate ABI.
All functions return statuses rather than throwing exceptions. Diagnostic
pointers may be NULL. Non-NULL diagnostics are overwritten by each operation.

## Objects and ownership

A `launcher_handle` owns one installation session and its exclusive lock.
Open creates or validates a sibling `<install>.launcher` directory and
resumes a pending installation offline before returning a usable session.
It rejects unsafe roots, unrelated sidecars and cross-filesystem staging.
`launcher_close` clears the caller's pointer and releases every resource.

Decoded releases own their paths, records and original JSON. Accessors return
borrowed strings valid until `launcher_release_free`. Release cleanup clears
the caller's pointer and accepts an already NULL object. A plan owns a copy
of its target and is bound to the handle and configuration/installation epoch
that created it. Apply/configure and verification that removes corrupt cache
objects invalidate
older plans. Free plans before
closing their handle; never use stale borrowed pointers or released objects.

Input/output object pointers must initially contain NULL. Failed creation
leaves them NULL. Handles are caller-serialized and callbacks may not reenter
the same handle or throw. Independent installations may operate concurrently.

## Publisher metadata

Pure decoders accept at most 16 MiB of UTF-8 JSON. Duplicate/unknown fields,
invalid types or identities, unsafe paths, collisions, unsupported formats,
and inconsistent shared-digest metadata fail validation. Canonical names use
Piggle ASCII folding for both file kinds and preserve non-ASCII spelling.
Installation path safety checks apply independently.

A channel contains format 1 and `release`. An immutable format-2 release
contains exactly `format`, `release`, `groups`, `loose`, `entry_point`, and
`run_arguments`. Named Hogg groups contain `path` and fingerprint `files`
arrays. `loose` is a flat fingerprint array; `loose` is reserved as a group
name. There is no committed/draft field. Canonical names are globally unique.
Archive destinations cannot overlap other archives or loose files. Up to
100,000 files are allowed; Hogg logical sizes reach 4,294,967,294 bytes and
loose sizes reach 2^63-1. IDs are opaque canonical labels. Old manifest formats
are rejected; this format break requires fresh setup rather than migrating old
metadata. Existing game files can still be adopted normally.

`launcher_decode_release` takes a descriptor callback returning borrowed bytes
valid until its next invocation or decoder return. The decoder hashes exact
canonical JSON bytes, validates their schema, and retains them. Callbacks may
be NULL only for empty releases. Failure clears output; fingerprint mismatch
returns INTEGRITY and invalid schema returns METADATA.

Descriptors bind `name` and `kind` alongside sizes, encoding, BLOB `sha256`,
and `logical_md5`. Format 1 has no header; format 2 adds canonical base64
`cached_header`; format 3 references `cached_header_sha256` and
`cached_header_size`. Raw lengths match; zlib requires smaller stored bytes.
Loose content is raw. Fingerprints and BLOB digests are distinct SHA256 values.
Dates and aliases do not enter immutable descriptor identity.

`launcher_release_group_count` counts groups. `launcher_release_group` returns
borrowed name, kind, optional archive path, and file count. A nonempty top-level
`loose` array appears through these accessors as one synthesized loose group
named `loose`; an empty loose array contributes no group.
`launcher_release_group_file` enumerates that group's files, exposing canonical
path, fingerprint, stored SHA256, sizes, encoding, and MD5. Groups and files
use canonical order. Invalid indices clear outputs and return INVALID; a NULL
release has zero groups. Borrowed strings last until release free.
`launcher_release_get_launch` exposes the release-owned command and arguments.

## Configuration and workflows

Configuration is copied and persisted atomically. The HTTP(S) base URL points
to the release collection root; `file_repositories` and
`file_repository_count` supply an explicit ordered list (at most 4096) of
`launcher_file_repository` URL/region pairs. The shared CA bundle applies to
every endpoint. The channel defaults to `live`. Optional game selection
names a relative managed loose file. Runner and game arguments are arrays,
not shell command strings. Configuration getters return borrowed strings
until configure or close. Unset optional settings are returned as empty
strings. Installed release access returns NOT_INSTALLED
and NULL before a successful first installation.

Set `s3_region` to the AWS bucket region or `auto` for Cloudflare R2 to enable
AWS Signature V4 for the manifest endpoint. Each file endpoint has its own
`s3_region` for descriptor and payload requests. Signed repository
URLs require HTTPS and must not contain user information. Regions contain at
most 63 lowercase ASCII letters, digits or hyphens. NULL/empty `s3_region`
uses unsigned HTTP(S), including when the process has AWS credentials.
Configuration saves use format 3. Identical normalized settings preserve the
saved file; configuring still clears in-memory source selections and
invalidates plans.

After configuring a signed repository, call `launcher_set_s3_credentials`
with an access key ID, secret access key and optional session token. The setter
copies the strings atomically and never persists or exposes them through
configuration getters. Both keys must be nonempty and all supplied strings
must be free of control bytes. Invalid input or allocation failure preserves
the previous credentials. NULL clears credentials on any usable handle.
Use `launcher_set_file_s3_credentials(handle, index, ...)` for each zero-based
file endpoint index. Each endpoint owns independent credentials; NULL clears
only the selected endpoint, and the index must exist. Credentials expire with
close or a successful change to the normalized URL or region at that index;
channel and launch-setting changes retain
them. Credential rotation does not invalidate existing plans. Callers refresh
temporary credentials explicitly. Missing credentials return INVALID before
signed network I/O. Offline operations and recovery require no credentials.

`resolve` fetches a fresh channel and its immutable format-2 release. For
each uncached fingerprint it reads the file repository's format-2 marker and
`revisions/<fingerprint>.json` directly. Descriptor identity binds the BLOB
location `<kind>/<blob-sha256>/BLOB`; referenced headers are
`<kind>/<blob-sha256>/headers/<header-sha256>`. Named history is not an
installation dependency. Missing descriptors, BLOBs, or headers permit provider
fallback. Authentication, corruption, and transport errors stop resolution.

Validated installed snapshots supply retained descriptors before standalone
disk caches. Accepted descriptors are also cached on disk by fingerprint.
Endpoint selection is retained in the open handle through planning.
Configuration clears those
in-memory selections and invalidates plans. Cached descriptors remain reusable
across endpoint changes; their exact hashes and schema are revalidated.
Reestablishing a source preserves byte-identical descriptor cache files.
After reopening, missing payloads require current descriptor lookup to establish
a source, even though the installed release retains descriptor bytes. Accepted
local payloads remain reusable across endpoints. No manifest-endpoint fallback
or credential sharing is implicit.
`make_plan` copies the target and checks complete saved groups and launch
settings for an unchanged update before populating unnecessary cache objects.
For changed snapshots it retains physical destinations whose fingerprints and
metadata match the installed files: complete sets for Hogg archives, individual
selections for loose files. Group names and release identity do not affect
physical reuse. Metadata-only changes still commit the new installed snapshot;
their plans have `unchanged` false and may require no objects or physical
writes.
Required changed payloads and headers populate the cache. Reused archive
entries need no payload-cache copy. Objects use safe presence and
stored length checks, and accept first-adopted installed stored bytes by hashing
while copying. Each physical candidate is tried once per expected digest;
length mismatches are skipped before copying. A differing compression stream
requires the desired object download.
Missing-object inventory and download totals use stored digest and length.
These operations never change active game files; planning may populate the
private accepted payload cache; missing payloads may require descriptor I/O
to establish their sources.

`apply` accepts all missing inputs before mutating installed files, then
saves a frozen target and physical ownership plan. Existing valid HOGG10
archives are patched entry by entry; unchanged payloads remain in place.
Header-only changes retain payload ranges. New, missing, or corrupt archives
are built in full, as are explicit repair targets. Destinations named `.pigg`
also contain HOGG10. Retained destinations are rechecked before mutation; a
changed retained file requires replanning.

Piggle preserves exact publisher encoding and supplied logical MD5 metadata on
`PG_CHECKSUM_LOGICAL`. New archive builders use accepted stored transfers;
existing archive writes validate changed stored input, which can decode and
hash compressed payloads without recompressing them. Archive metadata remains
checked. No routine whole-archive payload rewrite is needed for a small edit.

`update` composes resolve, plan and apply. `repair` always rebuilds the saved
installed manifest, bypassing the unchanged shortcut and retaining its channel
snapshot. `verify` explicitly hashes installed stored payloads and every present
final cache object offline, without decoding. It checks archive membership,
representation, lengths and MD5 metadata, reports integrity failures and
removes proven-corrupt cache copies. Eviction invalidates existing plans.
Incomplete `.part` files are not accepted objects and are ignored by verify.

Payload transfers accept complete HTTP 200 responses and identity encoding.
They stream bounded buffers and verify exact size and SHA-256 at EOF.
The first unsigned transfer initializes cURL for the process lifetime; closing
a handle does not tear down shared cURL global state.
Signed transfers use the official AWS Common Runtime C signer and streaming
HTTP transport, with the same bounded response and verification rules.
CRT global state is also initialized for the process lifetime; per-transfer
connections and worker resources shut down before returning.
Oversized, truncated or mismatched payloads are never accepted. Cache content
is accepted only after stored length/SHA-256 and durable writes succeed, then
published under its final digest filename. This filename carries acceptance
across restarts; no separate trust database is used. Later routine use checks
presence/size without payload scans. Cache files are never hardlinked to
writable game files.

## Transactions and errors

Installation is serialized. Before active files change, all required new
objects are cached and a format-3 journal durably records the frozen target,
exact descriptors, and owned removal, patch, write, and retained sets. New
archives and loose replacements are staged. Existing HOGG10 archives use
Piggle's per-entry redo recovery. Unchanged destinations are excluded from
writes. No whole-archive undo backup or whole-release rollback is promised.

A pending update blocks launch and resumes the same target on reopen, using
cached inputs without resolving channels, contacting providers, or depending
on a separate descriptor cache. Frozen ownership prevents recovery from
mistaking new files for unrelated user data. File/directory transitions remove
only empty directories and preserve unrelated descendants. Installed-state
format 2, containing the complete target and transaction ID, is saved after
all target metadata/filesystem checks pass. A successful save adopts the
prepared target and directory ownership in memory; only an ambiguous failed
save rereads installed state to identify the selected transaction. Private
snapshots and journals are bounded to 256 MiB. Old journal formats are rejected rather than migrated.

After installed state is saved, touched archives compact when
`reclaimable_bytes / physical_length >= 0.20`, using Piggle's estimate.
Compaction builds a separate HOGG10 on the destination filesystem, closes all
archive handles, then atomically replaces the archive. An interruption leaves
either equivalent installed archive usable. Abandoned temporary output is
removed on reopen; a completed installation is not replayed merely because
maintenance was interrupted. The threshold is a trigger, not a hard disk-space
ceiling.

OK means the target is installed. Optional space-reclamation failures such as
insufficient space or permissions can leave warning text in
`launcher_error.message` with OK; the CLI prints this warning to stderr.
Detected integrity failures or conflicting external changes remain errors.
COMMITTED means the target is installed but private cleanup needs retry.
RECOVERY_REQUIRED means pending evidence remains and further use or launch is
blocked. Other statuses distinguish invalid arguments, metadata, network,
integrity, filesystem, memory, conflicts, busy sessions, cancellation and absent
installed state. Preserve the sidecar when recovery needs retry.

Progress callbacks run synchronously with temporary borrowed path strings.
Returning zero requests cancellation. Before the installation journal is
published, cancellation changes no active files. Afterward, cancellation stops
at a recoverable boundary and reopening resumes the saved target before play.
Compaction reports a coarse STAGE event; Piggle provides no byte progress or
cancellation within a running compaction. For signed transfers, cancellation
during connection establishment waits for pending CRT setup to finish; no
HTTP request starts afterward.

Root directories and private state must remain under the caller's exclusive
ownership while a handle is open. Concurrent external directory renaming or
modification is unsupported. Piggle accepts native paths rather than directory
handles, so its adapter retains checked ancestors and verifies their identity
before and after archive operations. Ordinary launcher filesystem writes use
anchored handles. Durability depends on filesystem/OS guarantees: POSIX flushes
files and directories; Windows flushes files and uses write-through renames.

## Launch

Launch trusts the accepted installed release. It checks installation and
sidecar identity, a safe regular executable, recovery and process state offline.
It does not audit archive membership, headers, other managed loose files, or
executable content/length; local executable size changes are allowed. Use
`verify` and `repair` for content integrity. It requires a release-owned or
locally overridden managed loose executable and directly spawns runner, runner
arguments, executable, game arguments and extra arguments in that order.
The installation is the working directory and environment is inherited.
Native Linux executables receive an executable permission bit.

The session holds its installation lock while waiting. A durable PID/creation
identity marker is saved before child execution is released; it prevents
another session updating a surviving game after a supervisor crash. A runner
must remain alive for the full game session. External games or wrappers that
background the actual game are outside this supervision contract. Exit code
is -1 on launcher failure and the child exit status on completed execution.

## Example

```c
launcher_handle *handle = NULL;
launcher_error error = {0};
launcher_config config = {0};
config.base_url = "https://downloads.example/game/";
launcher_file_repository files[] = {
	{"https://downloads.example/files/", NULL}
};
config.file_repositories = files;
config.file_repository_count = 1;
config.game = "game.exe";
if (launcher_open("./game", &handle, &error) == LAUNCHER_OK) {
	if (launcher_configure(handle, &config, &error) == LAUNCHER_OK)
		launcher_update(handle, NULL, NULL, &error);
	launcher_close(&handle);
}
```

With an open handle, configure a private bucket's HTTPS URL and region before
supplying credentials. Applications obtain secrets from their own source;
the library does not read the environment or AWS profiles:

```c
config.base_url = "https://ACCOUNT.r2.cloudflarestorage.com/BUCKET/PREFIX/";
config.s3_region = "auto";
launcher_s3_credentials credentials = {access_key, secret_key, session_token};
launcher_file_repository private_files[] = {
	{"https://ACCOUNT.r2.cloudflarestorage.com/FILES/", "auto"}
};
config.file_repositories = private_files;
config.file_repository_count = 1;
launcher_status status = launcher_configure(handle, &config, &error);
if (status == LAUNCHER_OK)
	status = launcher_set_s3_credentials(handle, &credentials, &error);
/* Supply independent file credentials from the application’s secret source. */
launcher_s3_credentials file_credentials = {
	file_access_key, file_secret_key, file_session_token
};
if (status == LAUNCHER_OK)
	status = launcher_set_file_s3_credentials(handle, 0, &file_credentials,
		&error);
if (status == LAUNCHER_OK)
	status = launcher_update(handle, NULL, NULL, &error);
/* Inspect status/error, then release the session as in the example above. */
```

Check every result, including COMMITTED, in application code. Version 0.1
establishes the initial source contract; binary compatibility with later
releases is not promised. Public structures use the target platform ABI.

For standalone decoding, return descriptor bytes from your own store. The
callback may reuse its buffer after the next call begins:

```c
launcher_status lookup(const char *fingerprint, const void **bytes,
	size_t *size, void *state, launcher_error *error);
launcher_release *release = NULL;
launcher_status status = launcher_decode_release(json, json_size, "r1",
	lookup, store, &release, &error);
/* On success inspect the complete release; errors leave release NULL. */
launcher_release_free(&release);
```

## Cached resource headers

Cached headers are opaque authenticated descriptor metadata. Inline base64
must be canonical and nonempty. Referenced headers are raw objects in their
owning BLOB directory and are bounded to 16 MiB. The launcher verifies their
length/digest on download, retains them with the installed snapshot, and
passes them to Piggle without decoding resource payloads. Header-only changes
can update archive metadata while retaining the existing payload range. Offline
verification checks archive headers against the saved snapshot.
