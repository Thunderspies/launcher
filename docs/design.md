# Launcher design

The publisher defines a complete desired installation, independent of the
launcher UI and storage implementation. This launcher consumes public JSON
v2 directly and exposes one procedural C contract shared by its CLI and C++
interface. Its responsibilities are content verification, installation
recovery and supervised game launch. Publisher source tracking and deployment
remain separate concerns.

## Content and archives

Manifest and file repositories are independent endpoints. Immutable release
snapshots contain named Hogg groups and flat loose-file selections. Their
fingerprints address root-level `revisions/<fingerprint>.json` descriptors;
file repository markers use format 2. Authoring history does not participate
in installation. Missing metadata, payloads, or referenced headers permit the
next subscribed provider. Authentication, corruption, and transport failures
remain errors. Descriptor bytes bind canonical name, kind, logical metadata,
stored digest, and header independently of source paths or provider locations.

Exact descriptor bytes are retained in decoded snapshots and format-2
installed state. They are cached by fingerprint. Resolve reads the channel's
selected immutable release; configuration format 3 stores independent ordered
file endpoints. Format-3 pending journals retain the target and all metadata
needed for offline completion. This format break requires fresh setup.

Objects are stored bytes addressed by SHA-256. The publisher selects RFC 1950
zlib at level 6 only when strictly smaller, otherwise raw; loose objects remain
raw. Logical MD5 supplies game archive metadata independently of stored object
identity. A sorted array groups objects by digest and stored length, permitting
one object across encodings, destinations and releases. Among selections needed
for changed entries, shared payloads use the lexicographically first
descriptor fingerprint to choose their source. Retained destinations neither
populate payload caches nor require source lookup.
Bounded buffers keep payload memory independent of file size.

Downloads and first adoption hash stored bytes once while copying. Only a
successful length/checksum check and durable write permits publication under
the final cache digest name. Final names carry acceptance across restarts;
routine use checks safe regular-file presence and size. Explicit verification
checks later corruption and evicts proven-corrupt cache copies, invalidating
plans whose availability may have changed.
Installed adoption considers missing objects only, visits each physical archive
once, and avoids repeating a candidate named by both current and target
inventories. A stored-length mismatch requires no temporary payload copy.
Descriptor source discovery preserves an identical existing cache file.

New or missing archives are built as complete HOGG10 files through Piggle's
accepted stored-entry writer with supplied logical MD5 metadata on
`PG_CHECKSUM_LOGICAL`. Existing valid archives receive only changed additions,
replacements, removals, and cached-header metadata edits. Piggle preserves
stored compressed bytes; its existing-source writers validate changed input
without recompression. Unchanged payloads stay in place except when metadata
table growth requires relocation. Every managed archive uses HOGG10, including
paths whose suffix is `.pigg`.

Routine checks avoid rescanning accepted payloads. Unchanged updates compare
complete groups and launch settings before populating caches. Changed snapshots
compare physical destinations independently of release names and group labels.
Retained files keep their identities. Explicit repair rebuilds the complete
saved release. Explicit verification hashes installed stored bytes and present
cache objects and checks archive metadata against the saved inventory.

## Fixed-directory recovery

A fixed game directory preserves conventional game paths and existing saves.
A sibling sidecar holds configuration, accepted objects, installed inventory,
staged new destinations, the installation lock, and a forward journal.
Requiring the same filesystem keeps publication and compaction renames local.

All required changed inputs are accepted before installed files change. The
journal freezes the target and owned removal, write, patch, and retained sets.
New archives and loose files are staged; existing archives are patched through
Piggle's per-entry redo journal. The launcher does not copy entire archives
for undo. If interrupted, it blocks launch and resumes toward the frozen target
offline. Repeating the saved changes is safe when interruption happened after
an entry committed but before outer progress was saved.

The complete installed target is saved only after metadata/filesystem checks
succeed. A matching transaction ID identifies completion on reopen, so cleanup
cannot replay a finished update. Ownership remains frozen through recovery;
file/directory transitions preserve unrelated descendants and remove only
empty directories. An adopted Hogg is one managed archive: unlisted logical
entries are removed when building its first managed version. Unrelated loose
files stay outside ownership. Missing ownership never authorizes recursive
cleanup.

After saving installed state, touched archives automatically compact when
Piggle estimates reclaimable bytes are at least 20% of physical length.
Compaction builds a complete temporary HOGG10 on the same filesystem, closes
archive handles, then atomically replaces the destination. It can rewrite all
live payloads and offers only coarse progress without cancellation once
started. A crash leaves an equivalent installed archive; abandoned temporary
output is discarded on reopening. Insufficient space or permissions can leave
a successful installation with a maintenance warning. Integrity and external
change failures remain errors. The threshold triggers maintenance without
promising a space ceiling after failure or interruption.

## Platform and process boundaries

POSIX operations use directory handles and no-follow opens. Windows uses
checked component handles and rejects reparse points, retaining ancestors
while operating on descendants. Checks occur at open and publication, not
only during initial metadata parsing.

Launch trusts the accepted snapshot rather than auditing all installed files.
It checks root identity and the selected safe regular executable, allowing
local content and size changes. Explicit verify/repair handles integrity.
Launch uses argument arrays without a shell. A supervised game/runner must
remain alive for the session. Startup is gated until a durable process
identity marker exists, so a launcher crash cannot make a surviving supervised
game look idle. Concurrent external games are the caller's responsibility.

## Dependencies and interfaces

CPM pins Piggle, cURL, yyjson and Mbed TLS. yyjson is a C parser whose decoded
objects are validated independently against publisher rules. Mbed TLS supplies
portable streaming SHA-256 and Linux TLS; Windows cURL uses native Schannel
trust. Linux uses its CA store and both platforms permit an explicit CA file.
Private AWS S3 and Cloudflare R2 use the official AWS Common Runtime C
authentication and streaming HTTP libraries, fetched through a pinned
`aws-crt-cpp` CPM package and its pinned submodules. Only the C library targets
are linked. Linux CRT TLS uses bundled AWS-LC and s2n; Windows uses native
Schannel. No custom cryptographic implementation or system OpenSSL dependency
is needed.

The local signing region enables authenticated HTTPS requests independently
of the publisher metadata. Credentials live only on the installation handle;
the CLI supplies manifest keys from `AWS_*` and file endpoint N keys from
`LAUNCHER_FILE_N_AWS_*`, while native callers set them explicitly by endpoint.
Public requests never inherit private credentials. Changing the repository URL
or region clears them.
The CRT signer and transport share the existing download verification path.
A bounded receive window and handoff queue connect CRT event-loop callbacks
to the synchronous calling thread, which consumes bytes and invokes progress
callbacks. Cancellation drains request callbacks before releasing their state.

Opaque handles and status returns keep ownership and failure effects explicit.
Low-level metadata, resolve, plan and apply operations remain useful to other
interfaces; update and repair compose them. The first implementation uses
synchronous bounded I/O and cancellation at recoverable boundaries. GUI,
self-update, signatures and resumed downloads require separate contracts.

## Opaque cached headers

Version-2 file descriptors carry cached-header bytes independently of stored
payloads. The publisher supplies their application meaning; the launcher
validates canonical base64 and descriptor identity, retains one header per
distinct descriptor, and passes it to Piggle without payload decoding. Archive
verification compares the retained bytes with Piggle metadata. Header-only
changes update archive metadata while retaining unchanged payload ranges.

## Release snapshots and groups

Each named Hogg group owns one archive destination; a flat loose-file array
contributes canonical relative destinations. The C API exposes nonempty loose
selections as a synthesized `loose` group. An empty Hogg group creates an empty
archive; a release can have no archives. Physical destinations are validated
against every other archive and loose file. Planning, adoption, mutation,
verification, and recovery cover the complete owned physical set.

Channels point to immutable complete release names. Switching release content
uses another name and moving a channel is explicit. Published entrypoint and
literal arguments belong to the release. Local game and runner overrides
compose with the saved command. Installed state retains exact metadata
independently of later channel changes or remote availability.
