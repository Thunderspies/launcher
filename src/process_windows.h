static unsigned long long lx_creation(HANDLE process)
{
	FILETIME creation, exit, kernel, user;
	if (!GetProcessTimes(process, &creation, &exit, &kernel, &user))
		return 0;
	return ((unsigned long long)creation.dwHighDateTime << 32) |
		creation.dwLowDateTime;
}

launcher_status lx_process_busy(lx_fs *state, launcher_error *error)
{
	char text[256];
	int found;
	launcher_status status = lx_read_marker(state, text, &found, error);
	if (status != LAUNCHER_OK || !found)
		return status;
	unsigned long long pid, start;
	int end = 0;
	if (sscanf(text, "{\"pid\":%llu,\"start\":%llu}%n", &pid, &start,
			&end) != 2 ||
			text[end] || !pid || pid > MAXDWORD || !start)
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Invalid running process identity");
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION |
		SYNCHRONIZE, FALSE, (DWORD)pid);
	if (!process) {
		DWORD code = GetLastError();
		if (code != ERROR_INVALID_PARAMETER)
			return lx_fail(error, LAUNCHER_BUSY,
				"Cannot verify recorded process identity");
	} else {
		unsigned long long actual = lx_creation(process);
		DWORD waited = WaitForSingleObject(process, 0);
		CloseHandle(process);
		if (!actual || waited == WAIT_FAILED ||
			(actual == start && waited == WAIT_TIMEOUT))
			return lx_fail(error, LAUNCHER_BUSY,
				"Game process is still running");
	}
	return lx_fs_remove(state, "running.json", 0, error);
}

static wchar_t *lx_command(const char *const *args, size_t count)
{
	wchar_t **wide = (wchar_t **)calloc(count, sizeof(*wide));
	if (!wide)
		return NULL;
	size_t length = 1;
	int valid = 1;
	for (size_t i = 0; i < count; ++i) {
		wide[i] = args[i] ? lx_wide(args[i]) : NULL;
		if (!wide[i]) {
			valid = 0;
			break;
		}
		size_t n = wcslen(wide[i]);
		if (n > 32767 || n * 2 + 3 > 32767 - length) {
			valid = 0;
			break;
		}
		length += 3 + n * 2;
	}
	wchar_t *command = valid ?
		(wchar_t *)malloc(length * sizeof(*command)) : NULL;
	if (command) {
		wchar_t *out = command;
		for (size_t i = 0; i < count; ++i) {
			if (i)
				*out++ = L' ';
			*out++ = L'"';
			const wchar_t *p = wide[i];
			while (*p) {
				size_t slashes = 0;
				while (*p == L'\\') {
					++slashes;
					++p;
				}
				if (*p == L'"' || !*p)
					slashes *= 2;
				while (slashes--)
					*out++ = L'\\';
				if (*p == L'"')
					*out++ = L'\\';
				if (*p)
					*out++ = *p++;
			}
			*out++ = L'"';
		}
		*out = 0;
	}
	for (size_t i = 0; i < count; ++i)
		free(wide[i]);
	free(wide);
	return command;
}

launcher_status lx_process_run(lx_fs *state, const char *cwd,
	const char *const *args, size_t count, int *exit_code,
	launcher_error *error)
{
	if (exit_code)
		*exit_code = -1;
	if (!state || !cwd || !args || !count || !args[0] || !*args[0] ||
		!exit_code || count > 65535)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid launch arguments");
	for (size_t i = 0; i < count; ++i) {
		if (!args[i])
			return lx_fail(error, LAUNCHER_INVALID,
				"Null launch argument");
	}
	launcher_status status = lx_process_busy(state, error);
	if (status != LAUNCHER_OK)
		return status;
	lx_fs *directory = NULL;
	status = lx_fs_open(cwd, 0, &directory, error);
	if (status != LAUNCHER_OK)
		return status;
	wchar_t *command = lx_command(args, count);
	wchar_t *working = lx_wide(directory->path);
	if (!command || !working) {
		free(command);
		free(working);
		lx_fs_close(&directory);
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid or excessive process arguments");
	}
	for (wchar_t *p = working; *p; ++p) {
		if (*p == '/')
			*p = L'\\';
	}
	/* Atomically assign the suspended child to a temporary kill-on-close
		* job. A crash before marker persistence cannot strand an
		* untracked
		* suspended process. The job becomes non-owning after successful
		* start.
		*/
	HANDLE job = CreateJobObjectW(NULL, NULL);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
	limits.BasicLimitInformation.LimitFlags =
		JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	SIZE_T attribute_size = 0;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attribute_size);
	LPPROC_THREAD_ATTRIBUTE_LIST attributes =
		(LPPROC_THREAD_ATTRIBUTE_LIST)malloc(attribute_size);
	int initialized = attributes && InitializeProcThreadAttributeList(
		attributes, 1, 0, &attribute_size);
	if (!job || !SetInformationJobObject(job,
		JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
		!initialized || !UpdateProcThreadAttribute(attributes, 0,
		PROC_THREAD_ATTRIBUTE_JOB_LIST, &job, sizeof(job), NULL,
			NULL)) {
		DWORD code = GetLastError();
		if (initialized)
			DeleteProcThreadAttributeList(attributes);
		free(attributes);
		if (job)
			CloseHandle(job);
		free(command);
		free(working);
		lx_fs_close(&directory);
		return lx_native(error, (int)code,
			"Create process startup guard",
			args[0]);
	}
	STARTUPINFOEXW startup = {};
	startup.StartupInfo.cb = sizeof(startup);
	startup.lpAttributeList = attributes;
	PROCESS_INFORMATION child = {};
	if (!CreateProcessW(NULL, command, NULL, NULL, FALSE,
		CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
		EXTENDED_STARTUPINFO_PRESENT, NULL, working,
		&startup.StartupInfo, &child)) {
		status = lx_native(error, (int)GetLastError(), "Create process",
			args[0]);
	} else {
		unsigned long long creation = lx_creation(child.hProcess);
		char marker[256];
		if (!creation) {
			status = lx_native(error, (int)GetLastError(),
				"Get process identity", args[0]);
		} else {
			snprintf(marker, sizeof(marker),
				"{\"pid\":%llu,\"start\":%llu}",
				(unsigned long long)child.dwProcessId,
					creation);
			status = lx_write_marker(state, marker, error);
		}
		if (status == LAUNCHER_OK && ResumeThread(child.hThread) ==
			(DWORD)-1)
			status = lx_native(error, (int)GetLastError(),
				"Start process", args[0]);
		if (status == LAUNCHER_OK) {
			limits.BasicLimitInformation.LimitFlags = 0;
			if (!SetInformationJobObject(job,
				JobObjectExtendedLimitInformation, &limits,
				sizeof(limits)))
				status = lx_native(error, (int)GetLastError(),
					"Release process startup guard",
						args[0]);
		}
		if (status != LAUNCHER_OK)
			TerminateProcess(child.hProcess, 125);
		DWORD waited = WaitForSingleObject(child.hProcess, INFINITE);
		if (status == LAUNCHER_OK && waited != WAIT_OBJECT_0)
			status = lx_native(error, (int)GetLastError(),
				"Wait process", args[0]);
		DWORD code;
		if (status == LAUNCHER_OK &&
			!GetExitCodeProcess(child.hProcess, &code))
			status = lx_native(error, (int)GetLastError(),
				"Get exit code", args[0]);
		if (status == LAUNCHER_OK)
			*exit_code = (int)code;
		CloseHandle(child.hThread);
		CloseHandle(child.hProcess);
		launcher_error cleanup;
		launcher_status removed = lx_fs_remove(state, "running.json", 0,
			&cleanup);
		if (status == LAUNCHER_OK && removed != LAUNCHER_OK) {
			status = removed;
			if (error)
				*error = cleanup;
		}
	}
	DeleteProcThreadAttributeList(attributes);
	free(attributes);
	CloseHandle(job);
	free(command);
	free(working);
	lx_fs_close(&directory);
	return status;
}
