#include <launcher/launcher.h>
#include <stdio.h>
#include <string.h>

/* ENOSPC interrupts an actual archive patch after the frozen journal exists.
	* Borrowed metadata survives while the handle blocks further actions.
	*/
int main(int argc, char **argv)
{
	if (argc != 2)
		return 2;
	launcher_handle *handle = NULL;
	launcher_error error = {};
	const launcher_release *before = NULL;
	const launcher_release *after = NULL;
	int result = 1;
	int exit_code = 42;
	launcher_status status = launcher_open(argv[1], &handle, &error);

	if (status != LAUNCHER_OK)
		goto done;
	status = launcher_get_installed(handle, &before, &error);
	if (status != LAUNCHER_OK ||
		strcmp(launcher_release_id(before), "r1"))
		goto done;
	status = launcher_update(handle, NULL, NULL, &error);
	if (status != LAUNCHER_IO && status != LAUNCHER_RECOVERY_REQUIRED) {
		fprintf(stderr, "Expected pending IO failure, got %s\n",
			launcher_status_name(status));
		goto done;
	}
	if (strcmp(launcher_release_id(before), "r1")) {
		fputs("Borrowed release changed after failed apply\n", stderr);
		goto done;
	}
	status = launcher_get_installed(handle, &after, &error);
	if (status != LAUNCHER_RECOVERY_REQUIRED || after) {
		fputs("Pending installation exposed usable metadata\n", stderr);
		goto done;
	}
	status = launcher_launch(handle, NULL, 0, &exit_code, &error);
	if (status != LAUNCHER_RECOVERY_REQUIRED || exit_code != -1) {
		fputs("Pending installation permitted launch\n", stderr);
		goto done;
	}
	result = 0;
done:
	if (result && error.message[0])
		fprintf(stderr, "%s\n", error.message);
	launcher_close(&handle);
	return result;
}
