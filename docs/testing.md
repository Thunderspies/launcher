# Testing

GoogleTest checks public API contracts. Cucumber describes observable behavior
and workflows in Gherkin, with Cucumber-Cpp steps calling the public software
interfaces. The launcher has populated metadata, transport, filesystem,
transaction, TLS, process and publisher interoperability suites. All enabled
product suites must pass. Separate infrastructure regressions verify runner
behavior; their fixtures do not establish product coverage.

## Placement and maintenance

Put unit test sources under `tests/unit/`. Cover normal results, contract
boundaries, invalid inputs where specified, error results, and ownership or
cleanup obligations. Add coverage when an API contract changes and regression
tests when fixing a bug.

Put Gherkin `.feature` files under `features/` and compiled native step
definitions under `tests/acceptance/`. Add scenarios for new or newly clarified
observable behavior and meaningful workflows. Describe outcomes using the
language of a caller or user. Keep each scenario focused on one behavior;
avoid coupling scenarios to private implementation details.

Revise expectations when intended behavior changes. Implementation refactors
retain existing expectations, and bugs require fixing the implementation.
Remove coverage when behavior is intentionally retired or equivalent coverage
replaces it. Explain the reason in the change and remove unused steps and
fixtures together with the retired coverage.

## Build selections

Use the shared CMake presets from the repository root. Each selection has
matching configure, build, and test presets and its own build directory:

| Preset | Build directory | Test selection |
| --- | --- | --- |
| `default` | `build` | Unit tests |
| `acceptance` | `build/acceptance` | Both product suites and infrastructure |
| `acceptance-only` | `build/acceptance-only` | Acceptance and infrastructure |
| `no-suites` | `build/no-suites` | Both suite options off |
| `no-tests` | `build/no-tests` | Test infrastructure off |

All test options are independent under CMake's standard master switch:

| Option | Default | Purpose |
| --- | --- | --- |
| `BUILD_TESTING` | `ON` | Enable test infrastructure and dependencies |
| `BUILD_UNIT_TESTS` | `ON` | Build and discover GoogleTest unit cases |
| `BUILD_ACCEPTANCE_TESTS` | `OFF` | Build and run Cucumber acceptance tests |

`BUILD_TESTING=OFF` overrides both suite options. With both suite options off,
no test dependencies are fetched. GoogleTest is fetched whenever either suite
is enabled, because native Cucumber steps also use its assertions. Cucumber
and its dependencies are fetched only for acceptance tests. All third-party
dependencies use pinned CPM packages. Production dependencies
are loaded in every configuration; upstream dependency installation is excluded
from launcher installation.

Infrastructure regressions are enabled with acceptance testing and use the
same prerequisites. They have the `infrastructure` label, which is separate
from product labels `unit` and `acceptance`. The default configuration remains
Ruby-free. The `no-suites` and `no-tests` presets explicitly allow no CTest
entries; the other shared test presets fail when a filter selects no entries.

Library code and test tooling use C++17, with C11-compatible public headers.
Test code may follow GoogleTest and Cucumber framework conventions as
described in [CONTRIBUTING.md](../CONTRIBUTING.md).

## Local customization

Use the ignored `CMakeUserPresets.json` for special local environment options
or one-off tests. Inherit from a shared configure preset, choose a unique name,
and set a separate `binaryDir` such as `${sourceDir}/build-local`. Add matching
build and test presets that reference the local configure preset. Test presets
can inherit shared settings such as failure output, but must override
`configurePreset` to select the local build directory. Do not commit this file.

Set local compiler paths, generators, cache variables, and environment options
in user presets instead of passing configuration overrides on the command
line. For a multi-configuration generator, set the same `configuration` value,
such as `Debug`, in the local build and test presets. Use executables under the
corresponding configuration subdirectory. Set `CMAKE_INSTALL_PREFIX` in a user
configure preset to override the shared `$HOME/.local` installation prefix.

Add, remove, or alter supported project configurations in `CMakePresets.json`
and update their documented commands, as required by
[CONTRIBUTING.md](../CONTRIBUTING.md).

## Unit tests

Install CMake 3.24 or newer, Git, and a C/C++ toolchain supporting C++17 for
library sources and tests. Python 3 is required for CLI and TLS fixture tests.
Initial dependency downloads require network access. Ruby and Bundler are
unnecessary for the default configuration:

```sh
cmake --preset default
cmake --build --preset default
ctest --preset default
ctest --preset default -L unit
```

An empty `unit_tests` runner fails normal execution. CTest registers a failing
`unit_empty` entry when GoogleTest discovery finds zero cases, even when other
suites have tests. After adding cases, `gtest_discover_tests` automatically
discovers them, applies the `unit` label, and removes the empty-suite fallback.
Listing remains valid with no cases. List or focus cases with CTest or
GoogleTest:

```sh
ctest --preset default -N
ctest --preset default -L unit -R 'SuiteName.CaseName'
./build/tests/unit/unit_tests --gtest_list_tests
./build/tests/unit/unit_tests --gtest_filter='SuiteName.*'
```

For a multi-configuration generator, use local build and test presets with
matching `configuration` values, as described under local customization.

## Acceptance tests

Install Python 3, Ruby and Bundler, then install the locked gems from the
repository root. Also install `patch`, which CPM uses for CMake integration.
The Gemfile pins Cucumber 7.1.0 and cucumber-wire 6.2.1, matching
[Cucumber-Cpp 0.9.0's Gemfile][upstream-gems]. Use a Ruby version compatible
with these gems; the tooling has been verified with Ruby 3.3.6 and Bundler
2.6.9. Ruby and Bundler must be available on `PATH` during configuration.
To select other installations, set the `RUBY_EXECUTABLE` and
`BUNDLER_EXECUTABLE` cache variables in a user configure preset. Both must
belong to the same Ruby installation.

```sh
gem install bundler
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance
ctest --preset acceptance -L acceptance
```

To keep installed gems local, configure Bundler before installing:

```sh
bundle config set --local path vendor/bundle
bundle install
```

Those directories are ignored by Git. Keep the resolved `Gemfile.lock` in
version control. Configuration checks
prerequisites and the installed bundle without installing gems. If setup is
missing, follow the reported install command and configure again.

CTest registers one suite named `acceptance`, labeled `acceptance`. The Ruby
launcher starts `acceptance_steps`, using the upstream Cucumber-Cpp server,
bound to `127.0.0.1` on an ephemeral port. It waits up to ten seconds for the
server's readiness output, passes the selected port to the wire configuration,
and runs Cucumber in strict mode. Zero selected scenarios and undefined,
pending, or failed steps fail the suite. Cucumber support glue counts selected
scenarios using its event hooks, including after feature and tag filters.
The launcher propagates the result and stops its child processes on
completion, startup failure, or an interrupt. It escalates from termination
to forced cleanup after two seconds if necessary. Each invocation gets its
own server and port.

Use the launcher for feature paths, line selectors, and tag filters. Run these
commands from the repository root, substituting an existing feature and tag:

```sh
ruby tests/acceptance/run.rb build/acceptance/tests/acceptance/acceptance_steps
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps \
	features/workflow.feature
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps \
	features/workflow.feature:12
ruby tests/acceptance/run.rb \
	build/acceptance/tests/acceptance/acceptance_steps --tags '@workflow'
```

Additional Cucumber arguments are forwarded; strict mode remains enabled.
The launcher loads repository support glue and wire definitions even for
focused runs. For a multi-configuration generator, use the server executable
under `tests/acceptance/Debug/` after a Debug build.

[upstream-gems]: https://github.com/cucumber/cucumber-cpp/blob/v0.9.0/Gemfile

## Infrastructure regressions

After configuring and building an acceptance-enabled preset, run the permanent
runner checks from the repository root:

```sh
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Both commands must pass. Running `ctest --preset acceptance` executes both
groups. CI runs infrastructure checks first, then the populated product suites,
for each supported compiler entry.

Fixtures under `tests/infrastructure/` exercise real native wire steps and
controlled startup, exit, and process-tree behavior. The harness also checks
empty and populated unit discovery using an isolated CMake project. Acceptance
runs use temporary copies of the launcher, bundle manifest, and support glue,
so added product features and hooks do not affect these fixtures. Fixture
cases and features are not registered in the product suites. Temporary files,
ports, children, and descendants are cleaned up after each check, including
failure. The harness uses Ruby's standard library and the existing native test
dependencies.

Checks cover successful and focused execution, custom formatting, strict step
failures, empty selections, missing executables and gems, early exit, startup
timeout, excessive readiness output, server and consumer failures, forced
cleanup, interruption, and concurrent isolation. Repeated fixture invocations
also exercise port reuse. On POSIX, interruption uses an actual `SIGINT`. On
Windows, where CI may lack a console, a fixture raises the same exception as
the launcher's signal handler; delivery of console events remains environment
dependent.

If a regression fails, inspect its diagnostic and CTest's captured output.
Rebuild after modifying native fixtures and rerun the infrastructure label.
Avoid moving fixture cases into product directories to make empty suites pass.

## Common setup issues

- If configuration reports missing Ruby or Bundler, install them and ensure
  both executables are on `PATH`, or select matching paths in a user preset.
- If configuration or the launcher reports unavailable gems, run
  `bundle install` with the selected Ruby and Bundler, then configure again.
- If a unit suite contains no cases, register real product unit sources and
  rebuild. If acceptance selects no scenarios, add product features or correct
  the focused path, line selector, or tag filter.
- If a shared preset selects no CTest entries, check the label or regular
  expression. Use the explicit opt-out presets when intentionally disabling
  product testing.

## Opting out

Build acceptance tests alone, disable units, or disable all tests:

```sh
cmake --preset acceptance-only
cmake --build --preset acceptance-only
ctest --preset acceptance-only -L acceptance

cmake --preset no-suites
cmake --build --preset no-suites
ctest --preset no-suites

cmake --preset no-tests
cmake --build --preset no-tests
ctest --preset no-tests
```

The presets explicitly set the test selections on each configuration run.
Disabling acceptance later does not delete dependency sources already
downloaded in that build directory.

## Contributor workflow

Register each new `.cpp` file explicitly with `target_sources(unit_tests
PRIVATE ...)` in `tests/unit/CMakeLists.txt`, or with
`target_sources(acceptance_steps PRIVATE ...)` in
`tests/acceptance/CMakeLists.txt`. Rebuild to discover new unit cases. Gherkin
files need no CMake registration: Cucumber finds them under `features/` or
through a focused path. Extend the existing product cases and scenarios for
new behavior.

The installed CMake target is `launcher::launcher`. The C11/C++17 consumers
check its public headers and shared-library linkage; acceptance steps and CLI
checks exercise public workflows. Native unit cases also cover private
platform and transport contracts. On Windows shared builds, a private static
test library uses the maintained source list from the `launcher` target so
internal functions remain outside the DLL's public interface. Keep private
contract coverage alongside the public consumer and workflow checks. Register
native steps directly as executable sources so the linker retains their
static registrations.

For native steps using GoogleTest assertions, include `<gtest/gtest.h>` before
`<cucumber-cpp/autodetect.hpp>` so Cucumber selects its GoogleTest driver.
Use Cucumber scenario contexts and before/after hooks to establish and clean
up each scenario's resources. Reset state between scenarios, use unique
temporary paths, and release files, handles, and processes even on failure.
Scenarios must work alone and in any order. Share reusable steps without
depending on another scenario's actions or fixtures.

Before review, follow the checks in [CONTRIBUTING.md](../CONTRIBUTING.md).
Run both suites for affected behavior and inspect the discovered test and
scenario counts. For runner or build changes, exercise all five build
selections, failure results, focused runs, repeated runs, missing
prerequisites, startup failures, interrupts, and server cleanup.

## Publisher interoperability and fault tests

`tests/fixtures/public/` contains manifest distributions and
`tests/fixtures/file-public/` contains their descriptors and payloads, emitted
by the
actual publisher revision recorded in `tests/fixtures/producer.json`. Tests
serve copies through loopback HTTP and use real cURL and Piggle, without a
publisher installation or source tree at runtime. Regenerate distribution
fixtures with
`python3 tests/fixtures/generate.py --publisher ../publisher`. The TLS
certificate/key are isolated test credentials.

The `launcher_s3` unit entry uses a local HTTPS fixture with an independent
Signature V4 verifier. It checks AWS and R2 signing regions, temporary
credentials, bounded responses, cancellation, and credential lifetimes using
the CLI and a native public-API probe. The acceptance runner starts the same
fixture for private-bucket installation and repair scenarios. These checks
use isolated fake credentials and require no cloud account.

Product checks include native C11/C++17 consumer linkage, CLI configuration,
HTTPS certificate and hostname verification, strict metadata limits, exact
payload verification, adoption, repair, older-release switches,
incremental archive edits, and exact archive membership.
Process and transaction tests run subprocesses; supported platforms require
loopback sockets and child-process execution. POSIX crash tests terminate
update subprocesses at activation boundaries and verify offline
completion of the frozen target before launch.

For Wine testing, use a local cross-compilation preset with a matching
`CMAKE_CROSSCOMPILING_EMULATOR`. Test runners receive that emulator for native
consumer, CLI and unit-process fixtures. Actual Windows Schannel trust should
also be exercised by the Windows MSVC CI configuration.

The stored-object fixtures include compressed assets, raw binary/empty entries,
and identical bytes serving zlib, raw and loose destinations. Unit checks cover
exact installed stored bytes and logical MD5 metadata, adoption requiring the
desired compression stream, trust across restarts, and explicit cache eviction.
On Linux, the static unit-test library uses linker wrappers around SHA-256
finish and fsync. They count payload acceptance checks separately from
descriptor hashes, prove routine operations do not rehash, and check that each
distinct rename parent is flushed once.
C11/C++ consumers and acceptance steps continue to exercise the shared library.

Launch coverage must permit unrelated managed-file damage and local executable
size changes while explicit verification still reports them. Preserve missing,
directory and symlink executable rejection, installation/sidecar identity,
pending recovery, supervised process markers and literal arguments.

Cached-header coverage checks canonical version-2 descriptor base64, binary
headers, a 64 KiB header, shared payload identity across header-only updates,
damaged header detection, and offline repair from retained descriptors. The
`headers` acceptance fixture is generated with the sibling publisher API.

The format-2 interop suite must reject old manifests, use root-level immutable
descriptors without history lookups, and cover flat loose selections alongside
multiple and empty Hogg groups. Verify that changed-entry updates preserve
unrelated payload ranges and that changed inputs are accepted before mutation.
Exercise cancellation before and after pending intent, per-entry Piggle redo,
removed destinations, and restart with unavailable endpoints. Pending recovery
must use the saved target and ownership rather than refetching a channel or
undoing the previous release.

Compaction cases must cover the 20% reclaimable/physical trigger, untouched
archive exclusion, stored-byte preservation, same-filesystem replacement,
closed archive handles, insufficient-space/permission warnings, integrity and
external-change errors, and interruption before and after replacement. Installed
state must be saved and adopted before maintenance, and a maintenance
interruption must not replay completed updates. Fault tests must prove that a
successful save requires no installed-state readback while ambiguous publication
failures still inspect the selected transaction. Coarse progress must not imply cancellable byte-level
compaction. Native/API and CLI checks should inspect warning diagnostics on an
otherwise successful installation.

For this coordinated publisher/launcher format change, regenerate fixtures and
run both supported product configurations before review:

```sh
python3 tests/fixtures/generate.py --publisher ../publisher
bundle install
cmake --preset default
cmake --build --preset default
ctest --preset default
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
git diff --check
```

Report missing prerequisites or environment failures separately from product
failures. Do not report unexecuted checks as passing.
