CPMAddPackage(
	NAME nlohmann_json
	VERSION 3.11.3
	GITHUB_REPOSITORY nlohmann/json
	GIT_TAG v3.11.3
	EXCLUDE_FROM_ALL YES
	OPTIONS
		"JSON_BuildTests OFF"
		"JSON_Install OFF"
)
