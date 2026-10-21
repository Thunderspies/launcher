Documentation committed to this repository serves both as information for users
and as a contract for the project's intent. Agents must follow this
documentation and must not modify repository documentation unless explicitly
instructed to. Agent notes must not be committed to this repository or treated
as authoritative.

All changes must evenutally comply with CONTRIBUTING.md. During development,
agents may temporarily deviate from its requirements if those deviations can be
fully corrected before review. Examples include failing tests, nonconforming
code, temporary architecture or dependency experiments, and unfinished commits.

Use the shared CMake presets for supported build configurations. Use the
ignored `CMakeUserPresets.json` for special options needed by the local
development environment or for one-off tests. Inherit from shared presets
where appropriate, give local configurations their own build directories,
and do not commit user presets. Add or change matching build and test presets
when needed; multi-configuration generators require matching `configuration`
values in those presets.

If temporary scratch space is needed for notes, resources, one-off tools, or
test harnesses that are not intended to be committed to this repo, consider
adding a space to `.git/info/exclude` for them.

Before presenting changes for review, resolve all temporary deviations and
complete the checks required by CONTRIBUTING.md.
