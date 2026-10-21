#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <time.h>
#endif

/*
	* Controlled process for argument, exit and survivor tests. Usage: child
	* EXIT_CODE SLEEP_MS OUTPUT [ARGS...]. Each payload argument is written
	* as its decimal byte length, colon, original bytes, and a newline.
	*/
int main(int argc, char **argv)
{
	if (argc < 4)
		return 125;
	FILE *output = fopen(argv[3], "wb");
	if (!output)
		return 124;
	for (int i = 4; i < argc; ++i) {
		size_t size = strlen(argv[i]);
		if (fprintf(output, "%zu:", size) < 0 ||
			fwrite(argv[i], 1, size, output) != size ||
			fputc('\n', output) == EOF) {
			fclose(output);
			return 123;
		}
	}
	if (fclose(output))
		return 122;
	unsigned long milliseconds = strtoul(argv[2], NULL, 10);
#ifdef _WIN32
	Sleep((DWORD)milliseconds);
#else
	timespec duration = {};
	duration.tv_sec = (time_t)(milliseconds / 1000);
	duration.tv_nsec = (long)(milliseconds % 1000) * 1000000;
	while (nanosleep(&duration, &duration) && errno == EINTR)
		;
#endif
	return atoi(argv[1]);
}
