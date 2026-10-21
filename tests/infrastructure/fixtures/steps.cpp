#include <gtest/gtest.h>
#include <cucumber-cpp/autodetect.hpp>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

static void record_server(void)
{
	const char *directory = getenv("RUNNER_FIXTURE_DIR");
	char path[4096];

	ASSERT_NE(directory, nullptr);
	int length = snprintf(path, sizeof(path), "%s/native_server.pid",
		directory);

	ASSERT_GT(length, 0);
	ASSERT_LT(static_cast<size_t>(length), sizeof(path));
	FILE *output = fopen(path, "w");

	ASSERT_NE(output, nullptr);
#ifdef _WIN32
	fprintf(output, "%d\n", _getpid());
#else
	fprintf(output, "%ld\n", static_cast<long>(getpid()));
#endif
	fclose(output);
}

GIVEN("a native fixture passes")
{
	record_server();
	ASSERT_FALSE(testing::Test::HasFatalFailure());
}

GIVEN("a native fixture fails")
{
	record_server();
	FAIL() << "Intentional native assertion failure";
}

GIVEN("a native fixture is pending")
{
	record_server();
	pending("Intentional pending fixture");
}
