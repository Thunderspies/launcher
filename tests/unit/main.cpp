#include <gtest/gtest.h>
#include <stdio.h>

#ifdef LAUNCHER_PRODUCT_UNIT_TESTS
int launcher_test_command(int argc, char **argv);
#endif

int main(int argc, char **argv)
{
#ifdef LAUNCHER_PRODUCT_UNIT_TESTS
	int command = launcher_test_command(argc, argv);
	if (command >= 0)
		return command;
#endif
	testing::InitGoogleTest(&argc, argv);
	int result = RUN_ALL_TESTS();

	if (!GTEST_FLAG_GET(list_tests) &&
		testing::UnitTest::GetInstance()->total_test_count() == 0) {
		fprintf(stderr, "Unit suite contains no cases. "
			"Add public API tests before running this suite.\n");
		return 1;
	}
	return result;
}
