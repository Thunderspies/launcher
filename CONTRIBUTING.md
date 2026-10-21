Contributions will be rejected unless they meet the following criteria.

# Code Style

Write library sources in C++17, with a C11-compatible public C header and
a procedural C++ header. Use free functions, plain data structs, opaque
handles, and explicit state machines. Do not introduce methods, inheritance,
polymorphism, templates (including STL containers or smart pointers), or
exception-based error handling. Keep public callbacks as explicit function
pointers with caller state, and report errors through the public status
contract.

Lambdas and closures, including generic lambdas, may be used in internal
implementation code. Their compiler-generated closure types and call operators
are exceptions to the method and template restrictions. This permission does
not apply to public headers.

Test tooling under `tests/` is compiled as C++17. Test code may use GoogleTest
and Cucumber-Cpp conventions, including their classes, templates, containers,
and exceptions. This exception does not apply to library code or public
headers. The formatting rules below apply only to C and C++ code maintained
here, including tests; upstream dependency code retains its style.

Follow the [Linux kernel coding style][style] with these extra restrictions.

- Keep line width 80 characters or under
- Declare local variables close to where they're first used
- Only indent with tabs and only use tabs for indenting
- Any number of spaces may be used for alignment/readability in these cases:
  - Never at the start of a line
  - Never for indenting

[style]: https://docs.kernel.org/process/coding-style.html

# Commits

Keep changes in commits as complete and concise as possible. Do not attempt to
fix two things at once, but don't itemize tasks and goals into individual
commits that are so small that any one commit would be pointless in isolation.
A commit should be treated as a product just as much as the code, because it
will be used by a maintainer to apply patches to other forks or revert changes
that introduce errors.

Commits before the current tagged release are considered immutable, so a large
commit to add a new feature or subsystem is valid and encouraged within a
release, but follow-up work after the release where it was introduced should be
itemized. This is because a maintainer would apply patches to the latest
release.

## Titles

A maximum of 72 characters is allowed for commit titles. Use imperative-tense
for titles so that the following statement makes sense when the title is used
to fill in the blank:

> The purpose of this commit is to BLANK

For instance, a commit title of "fix a segfault in the text formatter" still
makes sense when used in the sentence "The purpose of this commit is to *fix a
segfault in the text formatter*".

## Descriptions

The rest of the commit message should have a body explaining the following:

- The purpose of the commit
- High level implementation details
- Any known issues, limitations, or follow-up work introduced by this commit

## Fixups

If a commit is intended to fix an issue or complete follow-up work in another
commit *since the last tagged release*, then submit it as a fixup commit using
`git commit --fixup` on the other commit. A maintainer will later squash all
fixup commits into one concise commit before tagging the release.

# Dependencies

This project uses CPM in CMake to pull dependencies. If a dependency is needed,
add it with `CPMAddPackage`. Dependencies may be patched only so that they can
build with CMake and CPM. Use the `PATCHES` parameter in `CPMAddPackage` to
apply patches. Do not patch a dependency to port it to a specific compiler,
link with another library, fix errors, or add/remove/change features for
instance. Each dependency should be added as a CMake file in a subdir to
`cmake/dependencies` along with any patches for that dependency.

Introducing a new dependeny that cannot be included as a standard CPM package
requires project maintainer approval.

# Build Configurations

Use the shared presets in `CMakePresets.json` to configure, build, and test.
When a supported build configuration needs to be added, removed, or altered,
change its configure, build, and test presets instead of using direct CMake
commands with custom options. Keep the documented preset commands current.

For special local environment options or one-off tests, use the ignored
`CMakeUserPresets.json`, inheriting from shared presets where appropriate.
The [testing guide](docs/testing.md) describes local customization.

# Testing

All infrastructure regressions and populated product suites must pass. An
enabled product suite with no cases or selected scenarios is an error. The
unchanged template intentionally has no product coverage; when maintaining
the template, verify those failures as expected results rather than adding
placeholder product tests. Adopted projects must add coverage for enabled
suites or explicitly disable them.

Before review, configure and build the default unit suite and the opt-in
acceptance suite, then run CTest with both enabled:

```sh
bundle install
cmake --preset acceptance
cmake --build --preset acceptance
ctest --preset acceptance -L infrastructure
ctest --preset acceptance -L '^(unit|acceptance)$'
```

Also check `git diff --check` and the code style and dependency rules above.
For build or runner changes, verify the `default`, `acceptance`,
`acceptance-only`, `no-suites`, and `no-tests` presets, including failure
propagation and cleanup.
The [testing guide](docs/testing.md) describes prerequisites and commands.

Unit tests under `tests/unit/` check public API contracts, boundary and error
cases, and regressions. Changes to API contracts require corresponding
coverage. This project follows Behavior Driven Development with Cucumber:
new or clarified observable behavior and meaningful workflows need scenarios
under `features/`, backed by native steps under `tests/acceptance/`. Where
applicable, test interoperability with the previous release.

Revise expectations when intended behavior changes. Refactors retain existing
expectations, and bugs require implementation fixes. Remove tests only when
behavior is intentionally retired or equivalent coverage replaces it; explain
the reason and remove unused steps and fixtures. Keep scenarios isolated and
focused on outcomes. The initial scaffolding has no unit cases or acceptance
scenarios, so both enabled product suites initially fail. Infrastructure
fixtures under `tests/infrastructure/` verify runner behavior and must not be
counted as product cases or scenarios. CI runs these regressions before product
tests on each supported compiler configuration. Keep infrastructure fixture
sources and features outside product discovery.
