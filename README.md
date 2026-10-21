# Launcher

Launcher installs and starts games distributed by the sibling publisher's
public JSON v2 repositories. It downloads content through cURL, verifies
SHA-256, and uses Piggle to create and incrementally update local HOGG10
archives alongside loose files. A procedural native library provides the same
operations as the command-line application.

Windows 10 or newer and Linux are supported. On Linux, configure a Wine runner
to start
Windows games. Repository URLs and game settings are local configuration;
the publisher supplies file groups, fingerprints, and a launch command.

## Build

Install CMake 3.24 or newer, Git, C11/C++17 compilers, and Python 3 for tests.
The first configure
requires network access. CPM fetches pinned Piggle, cURL, yyjson and Mbed TLS
sources. CPM also fetches the pinned official AWS Common Runtime and its
dependencies for authenticated S3/R2 downloads. Linux uses its bundled
AWS-LC and s2n TLS libraries; system OpenSSL is not required by the launcher.

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
```

For unit and acceptance testing, install Ruby and Bundler as described in
[the testing guide](docs/testing.md), then run:

```sh
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Install the application, library and public headers with
`cmake --install build`. The shared presets default to `$HOME/.local`.

## Configure and install

The manifest URL points at a published release collection root. Configure
file repository roots separately, in search order. The launcher resolves
fingerprints through immutable `revisions/<fingerprint>.json` descriptors.
File repository markers use format 2; named history is an authoring index
and is not needed for installation. Missing descriptors, BLOBs, or headers
permit trying another
file endpoint; corruption, authorization, and transport failures stop the
operation. There is no implicit manifest fallback.
Configuration is saved beside the installation in `<install>.launcher`,
which also holds the verified object cache and recovery information.

```sh
launcher configure --install ./game \
  --repo https://downloads.example/game/ \
  --file-repo https://downloads.example/files/ --game game.exe
launcher update --install ./game
launcher launch --install ./game
```

Progress is written to stderr on an interactive terminal. Use
`--progress always` for progress in logs or `--progress never` to suppress it.
Updates, verification, and repair report phase changes and throttle repeated
reports. Ctrl-C requests cancellation. Before installation intent is saved,
cancellation leaves active files untouched; afterward it leaves pending work
that reopening resumes before play. An interrupted cancellable operation exits
with code 130.
`verify` is an optional full checksum check of installed files and the cache.

The default channel is `live`. Change it explicitly before updating:

```sh
launcher configure --install ./game --channel test
launcher update --install ./game
```

`configure` preserves settings omitted from an existing configuration.
Supplying one or more `--file-repo` options replaces the ordered saved list;
`--clear-file-repos` clears it. `--file-s3-region REGION` signs the preceding
file repository. `status` displays the ordered endpoints. An initial
configuration requires `--repo`. Game launch is optional; when
configured, `--game` must name a loose file in the installed release.
Repeated `--game-arg` and `--runner-arg` options save argument arrays.
For Wine, use `--runner wine`; the inherited environment supplies settings
such as `WINEPREFIX`. An optional `--ca-bundle` selects a trusted CA file.

`status` shows the saved configuration and installed release. `update`
resolves the channel once, accepts newly acquired content, and installs its
exact
release. A channel pin to an older release uses the same update operation.
`repair` restores the last successfully installed release, even if the
channel has moved. It may download missing objects but never resolves a
new channel target. `verify` and `launch` operate offline. Extra launch
arguments follow `--` and are passed directly without shell expansion.

### Private AWS S3 and Cloudflare R2

Use the HTTPS S3 API URL for the bucket and prefix containing publisher
repository roots, and set their signing regions. For AWS, use the bucket's
actual
region and regional endpoint:

```sh
launcher configure --install ./game \
  --repo https://BUCKET.s3.us-east-1.amazonaws.com/PREFIX/ \
  --s3-region us-east-1 \
  --file-repo https://FILES.s3.us-east-1.amazonaws.com/PREFIX/ \
  --file-s3-region us-east-1
```

For R2, use the account's S3 API endpoint, bucket name and optional prefix,
with region `auto`:

```sh
launcher configure --install ./game \
  --repo https://ACCOUNT.r2.cloudflarestorage.com/BUCKET/PREFIX/ \
  --s3-region auto \
  --file-repo https://ACCOUNT.r2.cloudflarestorage.com/FILES/PREFIX/ \
  --file-s3-region auto
```

Set `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` in the environment before
running `update` or `repair`. For temporary credentials, also set
`AWS_SESSION_TOKEN`. R2 credentials use these same variable names. Credentials
need permission to read channel and release keys under the manifest prefix.
For file repository N (starting at 1), supply independent credentials through
`LAUNCHER_FILE_N_AWS_ACCESS_KEY_ID`,
`LAUNCHER_FILE_N_AWS_SECRET_ACCESS_KEY`, and optional
`LAUNCHER_FILE_N_AWS_SESSION_TOKEN`. There is no implicit sharing with `AWS_*`
or other file endpoints. File credentials need repository marker, descriptor,
BLOB, and referenced-header access.
The launcher copies credentials into memory for the command and never saves
them in its configuration or prints them in status output.

Signing uses the official AWS Common Runtime C authentication and streaming
HTTP libraries. No AWS profiles, role discovery or automatic credential
refresh are performed. Missing credentials fail before a signed request;
`configure`, `status`, `verify`, `launch` and recovery do not require them.
Public endpoints ignore their credential variables. The CA bundle applies
to all endpoints. `--s3-region ""` disables
signing. Signed requests require HTTPS and do not follow redirects; a wrong
AWS region or endpoint must be corrected in configuration.

## Installation behavior

Game files stay in the selected directory across releases. Existing regular
files at desired destinations are adopted automatically: matching bytes
are reused, and differing bytes are replaced through the update transaction.
Each desired Hogg archive is managed. Updates retain an archive when its
physical destination, complete fingerprint set, and metadata match the
installed group. Loose files are retained by destination, fingerprint, and
filesystem checks. Release names, group labels, and launch settings can change
without rewriting matching files or populating their payload cache. Existing
valid HOGG10 archives receive changed, added, and removed entries in place;
unchanged payloads remain in place. New, missing, or damaged archives are built
from the target. Every managed archive uses HOGG10, including destinations
named `.pigg`. Unrelated loose files, including saves and user settings, are
preserved. A file/directory conflict that would remove unrelated content is
rejected.

All required new objects are downloaded and accepted by stored length and
SHA-256 before installed files change. A durable journal saves the frozen
target and ownership plan. Interrupted updates resume toward that target
offline before launch; they do not restore a whole previous installation.
Keep the sidecar with the installation and on the same filesystem. Switching
to an older release is another update through the same path.

After saving the completed installation, touched archives automatically compact
when Piggle estimates at least 20% of their physical length is reclaimable.
Compaction writes a temporary HOGG10 on the destination filesystem, closes
archive handles, and atomically replaces the archive. It may rewrite all live
payloads and reports a coarse stage without byte progress or cancellation once
started. Space or permission failures leave the release installed and emit a
warning; detected corruption or conflicting external changes remain errors.
An interrupted compaction leaves an equivalent installed archive usable.

Launch starts the selected executable without scanning other game files and
waits for the game or runner. It checks the executable's safe regular-file
presence, installation identity, and recovery/process state. Local executable
changes, including size changes, are allowed. Use `verify` to check installed
content and `repair` to restore the saved release.
Another launcher cannot update while that session remains active, including
when its supervising launcher crashes. A runner must stay alive throughout
the game session; externally started games remain the caller's responsibility.

The CLI patches changed Hogg entries and downloads complete missing objects.
It does not provide a GUI, launcher self-update, repository signatures or
partial-download resume.

## Native API

Include `<launcher/launcher.h>` from C11 or `<launcher/launcher.hpp>` from
C++17 and link `launcher::launcher`. Interfaces use free functions, explicit
ownership, opaque handles and status diagnostics. See [the API contract](
docs/api.md) and [design rationale](docs/design.md).

HOGG objects retain the publisher's exact raw or zlib representation. SHA-256
identifies stored bytes; the supplied logical MD5 supplies game metadata.
Objects are accepted at download or first adoption. Final cache filenames
carry that acceptance across restarts. Routine reuse checks metadata and
filesystem state; changed-entry writes use Piggle validation while preserving
the supplied encoded bytes. New archive construction uses accepted stored
transfers without decoding. Use `verify` to detect later same-size corruption
and discard corrupt cache copies, then `repair` to rebuild the saved release.
Repair always rebuilds; unchanged updates avoid unnecessary cache population.
Configuration and pending journals use format 3. Installed snapshots use format
2 and retain exact descriptors, groups, and release-owned launch settings for
offline verification, repair, launch, and interrupted recovery. This format
break requires fresh setup; old repositories and pending state are not migrated.

Published format-2 releases contain named Hogg groups and a flat `loose`
fingerprint array. Their immutable named fingerprints identify canonical names
and stored representations independently of a provider. Channels are mutable
cursors selecting immutable complete release names. Native callers should
inspect diagnostics even after `LAUNCHER_OK`: optional compaction can leave a
maintenance warning in `launcher_error.message` while the release is installed.
