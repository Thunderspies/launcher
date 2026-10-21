#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <process.h>
#include <string>
#else
#include <unistd.h>
#endif

int main(void)
{
	char *ruby = getenv("RUNNER_FIXTURE_RUBY");
	char *script = getenv("RUNNER_FIXTURE_SCRIPT");

	if (!ruby || !script) {
		fprintf(stderr, "Fixture Ruby and script are required.\n");
		return 1;
	}
#ifdef _WIN32
	// The Windows CRT joins argv into a command line without quoting it.
	std::string ruby_argument = std::string("\"") + ruby + "\"";
	std::string script_argument = std::string("\"") + script + "\"";
	const char *arguments[] = {
		ruby_argument.c_str(), script_argument.c_str(), NULL
	};
	intptr_t result = _spawnv(_P_WAIT, ruby, arguments);

	if (result != -1)
		return static_cast<int>(result);
#else
	char *arguments[] = { ruby, script, NULL };

	execv(ruby, arguments);
#endif
	perror("Fixture Ruby launch failed");
	return 1;
}
