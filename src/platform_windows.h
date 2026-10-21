/* Included by platform.cpp; all native paths stay beneath retained parents. */
static wchar_t *lx_wide(const char *text)
{
	int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, -1,
		NULL, 0);
	if (!n)
		return NULL;
	wchar_t *out = (wchar_t *)malloc((size_t)n * sizeof(*out));
	if (out && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text,
					-1, out, n)) {
		free(out);
		out = NULL;
	}
	return out;
}

static char *lx_utf8(const wchar_t *text)
{
	int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
		NULL, 0, NULL, NULL);
	if (!n)
		return NULL;
	char *out = (char *)malloc((size_t)n);
	if (out && !WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text,
					-1, out, n, NULL, NULL)) {
		free(out);
		out = NULL;
	}
	return out;
}

static wchar_t *lx_win_name(const char *text)
{
	wchar_t *plain = lx_wide(text);
	if (!plain)
		return NULL;
	for (wchar_t *p = plain; *p; ++p) {
		if (*p == '/')
			*p = L'\\';
	}
	if (!wcsncmp(plain, L"\\\\?\\", 4))
		return plain;
	size_t n = wcslen(plain);
	wchar_t *out = (wchar_t *)malloc((n + 5) * sizeof(*out));
	if (out) {
		memcpy(out, L"\\\\?\\", 4 * sizeof(*out));
		memcpy(out + 4, plain, (n + 1) * sizeof(*out));
	}
	free(plain);
	return out;
}

static int lx_win_missing(DWORD code)
{
	return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

static launcher_status lx_win_hold(lx_fs *fs, const char *path,
	int create, launcher_error *error)
{
	wchar_t *name = lx_win_name(path);
	if (!name)
		return lx_fail(error, LAUNCHER_MEMORY, "Native path failed");
	HANDLE handle = CreateFileW(name, GENERIC_READ,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
		FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
			NULL);
	DWORD code = GetLastError();
	if (handle == INVALID_HANDLE_VALUE && lx_win_missing(code) && create) {
		if (CreateDirectoryW(name, NULL) ||
			GetLastError() == ERROR_ALREADY_EXISTS)
			handle = CreateFileW(name, GENERIC_READ,
				FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
				OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
				FILE_FLAG_OPEN_REPARSE_POINT, NULL);
		code = GetLastError();
	}
	free(name);
	if (handle == INVALID_HANDLE_VALUE)
		return lx_native(error, (int)code, "Open directory", path);
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(handle, &info)) {
		code = GetLastError();
		CloseHandle(handle);
		return lx_native(error, (int)code, "Inspect directory", path);
	}
	if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
		!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		CloseHandle(handle);
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Unsafe directory component '%s'", path);
	}
	HANDLE *items = (HANDLE *)realloc(fs->ancestors,
		(fs->count + 1) * sizeof(*items));
	if (!items) {
		CloseHandle(handle);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Directory retention allocation failed");
	}
	fs->ancestors = items;
	fs->ancestors[fs->count++] = handle;
	fs->directory = handle;
	return LAUNCHER_OK;
}

void lx_fs_close(lx_fs **fs)
{
	if (!fs || !*fs)
		return;
	if ((*fs)->lock != INVALID_HANDLE_VALUE)
		CloseHandle((*fs)->lock);
	for (size_t i = (*fs)->count; i > 0; --i)
		CloseHandle((*fs)->ancestors[i - 1]);
	free((*fs)->ancestors);
	free((*fs)->path);
	free(*fs);
	*fs = NULL;
}

launcher_status lx_fs_open(const char *path, int create, lx_fs **out,
	launcher_error *error)
{
	if (!path || !*path || !out || *out)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid directory output");
	wchar_t *input = lx_wide(path);
	if (!input)
		return lx_fail(error, LAUNCHER_INVALID, "Invalid UTF-8 path");
	DWORD n = GetFullPathNameW(input, 0, NULL, NULL);
	wchar_t *native = (wchar_t *)malloc(((size_t)n + 1) * sizeof(*native));
	if (!n || !native || !GetFullPathNameW(input, n + 1, native, NULL)) {
		free(input);
		free(native);
		return lx_native(error, (int)GetLastError(), "Absolute path",
			path);
	}
	free(input);
	if (wcslen(native) < 3 || native[1] != L':' || native[2] != L'\\') {
		free(native);
		return lx_fail(error, LAUNCHER_UNSUPPORTED,
			"Installation must use a local Windows drive");
	}
	char *absolute = lx_utf8(native);
	free(native);
	if (!absolute)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Path allocation failed");
	for (char *p = absolute; *p; ++p) {
		if (*p == '\\')
			*p = '/';
	}
	size_t length = strlen(absolute);
	while (length > 3 && absolute[length - 1] == '/')
		absolute[--length] = 0;
	lx_fs *fs = (lx_fs *)calloc(1, sizeof(*fs));
	if (!fs) {
		free(absolute);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Directory allocation failed");
	}
	fs->lock = INVALID_HANDLE_VALUE;
	fs->path = absolute;
	char *copy = strdup(absolute);
	if (!copy) {
		lx_fs_close(&fs);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Path allocation failed");
	}
	char saved = copy[3];
	copy[3] = 0;
	launcher_status status = lx_win_hold(fs, copy, 0, error);
	copy[3] = saved;
	char *p = copy + 3;
	while (status == LAUNCHER_OK && *p) {
		char *slash = strchr(p, '/');
		if (slash)
			*slash = 0;
		status = lx_win_hold(fs, copy, create, error);
		if (!slash)
			break;
		*slash = '/';
		p = slash + 1;
	}
	free(copy);
	if (status != LAUNCHER_OK) {
		lx_fs_close(&fs);
		return status;
	}
	DWORD needed = GetFinalPathNameByHandleW(fs->directory, NULL, 0,
		FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
	wchar_t *canonical = (wchar_t *)malloc(((size_t)needed + 1) *
		sizeof(*canonical));
	if (!needed || !canonical || !GetFinalPathNameByHandleW(fs->directory,
		canonical, needed + 1, FILE_NAME_NORMALIZED |
			VOLUME_NAME_DOS)) {
		status = lx_native(error, (int)GetLastError(),
			"Canonical directory path", fs->path);
		free(canonical);
		lx_fs_close(&fs);
		return status;
	}
	char *spelling = lx_utf8(!wcsncmp(canonical, L"\\\\?\\", 4) ?
		canonical + 4 : canonical);
	free(canonical);
	if (!spelling) {
		lx_fs_close(&fs);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Canonical path allocation failed");
	}
	for (char *p = spelling; *p; ++p) {
		if (*p == '\\')
			*p = '/';
	}
	if (spelling[0] >= 'a' && spelling[0] <= 'z')
		spelling[0] -= 'a' - 'A';
	free(fs->path);
	fs->path = spelling;
	*out = fs;
	return LAUNCHER_OK;
}

static launcher_status lx_win_parent(lx_fs *fs, const char *path,
	int create, lx_fs **out, char **leaf, launcher_error *error)
{
	*out = NULL;
	*leaf = NULL;
	if (!fs || !lx_relative(path))
		return lx_fail(error, LAUNCHER_INVALID, "Unsafe relative path");
	lx_fs *parent = (lx_fs *)calloc(1, sizeof(*parent));
	if (!parent)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Parent allocation failed");
	parent->lock = INVALID_HANDLE_VALUE;
	parent->path = strdup(fs->path);
	parent->ancestors = (HANDLE *)calloc(fs->count, sizeof(HANDLE));
	if (!parent->path || !parent->ancestors) {
		lx_fs_close(&parent);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Parent allocation failed");
	}
	for (size_t i = 0; i < fs->count; ++i) {
		if (!DuplicateHandle(GetCurrentProcess(), fs->ancestors[i],
			GetCurrentProcess(), &parent->ancestors[i], 0, FALSE,
			DUPLICATE_SAME_ACCESS)) {
			launcher_status status = lx_native(error,
				(int)GetLastError(), "Retain parent", path);
			lx_fs_close(&parent);
			return status;
		}
		++parent->count;
	}
	parent->directory = parent->ancestors[parent->count - 1];
	char *copy = strdup(path);
	if (!copy) {
		lx_fs_close(&parent);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Path allocation failed");
	}
	char *part = copy, *slash;
	launcher_status status = LAUNCHER_OK;
	while ((slash = strchr(part, '/'))) {
		*slash = 0;
		char *next = lx_join(parent->path, part);
		if (!next) {
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Parent allocation failed");
			break;
		}
		status = lx_win_hold(parent, next, create, error);
		if (status != LAUNCHER_OK) {
			free(next);
			break;
		}
		free(parent->path);
		parent->path = next;
		part = slash + 1;
	}
	if (status == LAUNCHER_OK) {
		*leaf = strdup(part);
		if (!*leaf)
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Leaf allocation failed");
	}
	free(copy);
	if (status != LAUNCHER_OK) {
		lx_fs_close(&parent);
		return status;
	}
	*out = parent;
	return LAUNCHER_OK;
}

static launcher_status lx_win_file(lx_fs *parent, const char *leaf,
	DWORD access, DWORD disposition, HANDLE *out, launcher_error *error)
{
	*out = INVALID_HANDLE_VALUE;
	char *full = lx_join(parent->path, leaf);
	wchar_t *native = full ? lx_win_name(full) : NULL;
	if (!native) {
		free(full);
		return lx_fail(error, LAUNCHER_MEMORY, "Native path failed");
	}
	HANDLE handle = CreateFileW(native, access,
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, disposition,
		FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
			NULL);
	DWORD code = GetLastError();
	free(native);
	if (handle == INVALID_HANDLE_VALUE) {
		launcher_status status = lx_native(error, (int)code,
			"Open file",
			full);
		free(full);
		return status;
	}
	free(full);
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(handle, &info)) {
		code = GetLastError();
		CloseHandle(handle);
		return lx_native(error, (int)code, "Inspect file", leaf);
	}
	if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) {
		CloseHandle(handle);
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Reparse point is not allowed '%s'", leaf);
	}
	*out = handle;
	return LAUNCHER_OK;
}

launcher_status lx_fs_same_volume(lx_fs *a, lx_fs *b,
	launcher_error *error)
{
	if (!a || !b)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directories");
	BY_HANDLE_FILE_INFORMATION first, second;
	if (!GetFileInformationByHandle(a->directory, &first) ||
		!GetFileInformationByHandle(b->directory, &second))
		return lx_native(error, (int)GetLastError(), "Inspect volume",
			"");
	if (first.dwVolumeSerialNumber != second.dwVolumeSerialNumber)
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Installation and state must share a filesystem");
	return LAUNCHER_OK;
}

launcher_status lx_fs_validate(lx_fs *fs, launcher_error *error)
{
	if (!fs)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directory");
	lx_fs *current = NULL;
	launcher_status status = lx_fs_open(fs->path, 0, &current, error);
	BY_HANDLE_FILE_INFORMATION before, after;
	if (status == LAUNCHER_OK) {
		if (!GetFileInformationByHandle(fs->directory, &before) ||
			!GetFileInformationByHandle(current->directory, &after))
			status = lx_native(error, (int)GetLastError(),
				"Validate directory", fs->path);
		else if (before.dwVolumeSerialNumber !=
				after.dwVolumeSerialNumber ||
				before.nFileIndexHigh != after.nFileIndexHigh ||
				before.nFileIndexLow != after.nFileIndexLow)
			status = lx_fail(error, LAUNCHER_CONFLICT,
				"Directory was substituted '%s'", fs->path);
	}
	lx_fs_close(&current);
	return status;
}

launcher_status lx_fs_identity(lx_fs *fs, char out[128],
	launcher_error *error)
{
	if (out)
		*out = 0;
	if (!fs || !out)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid identity output");
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(fs->directory, &info))
		return lx_native(error, (int)GetLastError(),
			"Directory identity", fs->path);
	snprintf(out, 128, "win32:%lu:%lu:%lu",
		(unsigned long)info.dwVolumeSerialNumber,
		(unsigned long)info.nFileIndexHigh,
		(unsigned long)info.nFileIndexLow);
	return LAUNCHER_OK;
}

launcher_status lx_fs_lock(lx_fs *fs, launcher_error *error)
{
	if (!fs)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directory");
	if (fs->lock != INVALID_HANDLE_VALUE)
		return LAUNCHER_OK;
	HANDLE handle;
	launcher_status status = lx_win_file(fs, "lock", GENERIC_READ |
		GENERIC_WRITE, OPEN_ALWAYS, &handle, error);
	if (status != LAUNCHER_OK)
		return status;
	OVERLAPPED position = {};
	if (!LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK |
		LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &position)) {
		DWORD code = GetLastError();
		CloseHandle(handle);
		if (code == ERROR_LOCK_VIOLATION)
			return lx_fail(error, LAUNCHER_BUSY,
				"Installation is already locked");
		return lx_native(error, (int)code, "Lock", fs->path);
	}
	fs->lock = handle;
	return LAUNCHER_OK;
}

launcher_status lx_fs_stat(lx_fs *fs, const char *path, int *kind,
	uint64_t *size, launcher_error *error)
{
	if (!kind)
		return lx_fail(error, LAUNCHER_INVALID, "Missing stat output");
	*kind = 0;
	if (size)
		*size = 0;
	lx_fs *parent;
	char *leaf;
	launcher_error native = {};
	launcher_status status = lx_win_parent(fs, path, 0, &parent, &leaf,
		&native);
	HANDLE handle = INVALID_HANDLE_VALUE;
	if (status == LAUNCHER_OK)
		status = lx_win_file(parent, leaf, FILE_READ_ATTRIBUTES,
			OPEN_EXISTING, &handle, &native);
	free(leaf);
	lx_fs_close(&parent);
	if (status != LAUNCHER_OK) {
		if (lx_win_missing((DWORD)native.native_code))
			return LAUNCHER_OK;
		if (error)
			*error = native;
		return status;
	}
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(handle, &info)) {
		status = lx_native(error, (int)GetLastError(), "Inspect file",
			path);
	} else {
		*kind = info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY
			? 2 : 1;
		if (size && *kind == 1)
			*size = ((uint64_t)info.nFileSizeHigh << 32) |
				info.nFileSizeLow;
	}
	CloseHandle(handle);
	return status;
}

launcher_status lx_fs_mkdirs(lx_fs *fs, const char *path,
	launcher_error *error)
{
	if (!fs || !path)
		return lx_fail(error, LAUNCHER_INVALID,
			"Missing directory path");
	if (!*path)
		return LAUNCHER_OK;
	char *sentinel = lx_join(path, "directory-sentinel");
	if (!sentinel)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Path allocation failed");
	lx_fs *parent;
	char *leaf;
	launcher_status status = lx_win_parent(fs, sentinel, 1, &parent, &leaf,
		error);
	lx_fs_close(&parent);
	free(leaf);
	free(sentinel);
	return status;
}

static launcher_status lx_win_recase(const wchar_t *source,
	const wchar_t *desired, const char *leaf, launcher_error *error)
{
	if (!MoveFileExW(source, desired, MOVEFILE_WRITE_THROUGH))
		return lx_native(error, (int)GetLastError(),
			"Apply directory case", leaf);
	WIN32_FIND_DATAW entry;
	HANDLE find = FindFirstFileW(desired, &entry);
	if (find == INVALID_HANDLE_VALUE)
		return lx_native(error, (int)GetLastError(),
			"Verify directory case", leaf);
	FindClose(find);
	char *actual = lx_utf8(entry.cFileName);
	if (!actual)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Case allocation failed");
	launcher_status status = LAUNCHER_OK;
	if (strcmp(actual, leaf))
		status = lx_fail(error, LAUNCHER_UNSUPPORTED,
			"Filesystem ignored directory case change '%s'", leaf);
	free(actual);
	return status;
}

launcher_status lx_fs_case(lx_fs *fs, const char *path,
	launcher_error *error)
{
	if (!fs || !path || (*path && !lx_relative(path)))
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid directory case");
	if (!*path)
		return LAUNCHER_OK;
	char *prefix = strdup(path);
	if (!prefix)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Case allocation failed");
	char *part = prefix;
	launcher_status status = LAUNCHER_OK;
	while (*part && status == LAUNCHER_OK) {
		char *slash = strchr(part, '/');
		if (slash)
			*slash = 0;
		lx_fs *parent;
		char *leaf;
		status = lx_win_parent(fs, prefix, 0, &parent, &leaf, error);
		if (status != LAUNCHER_OK)
			break;
		char *full = lx_join(parent->path, leaf);
		wchar_t *desired = full ? lx_win_name(full) : NULL;
		WIN32_FIND_DATAW entry;
		HANDLE find = desired ? FindFirstFileW(desired, &entry) :
			INVALID_HANDLE_VALUE;
		if (!desired) {
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Directory case allocation failed");
		} else if (find == INVALID_HANDLE_VALUE) {
			status = lx_native(error, (int)GetLastError(),
				"Inspect directory case", prefix);
		} else {
			FindClose(find);
			DWORD flags = entry.dwFileAttributes;
			if (!(flags & FILE_ATTRIBUTE_DIRECTORY) ||
				flags & FILE_ATTRIBUTE_REPARSE_POINT) {
				status = lx_fail(error, LAUNCHER_CONFLICT,
					"Unsafe directory case target");
			} else {
				char *actual_leaf = lx_utf8(entry.cFileName);
				char *actual = actual_leaf ?
					lx_join(parent->path,
						actual_leaf) : NULL;
				wchar_t *source = actual ? lx_win_name(actual) :
					NULL;
				if (!source)
					status = lx_fail(error, LAUNCHER_MEMORY,
						"Case allocation failed");
				else if (strcmp(actual_leaf, leaf))
					status = lx_win_recase(source, desired,
						leaf, error);
				free(source);
				free(actual);
				free(actual_leaf);
			}
		}
		free(desired);
		free(full);
		free(leaf);
		lx_fs_close(&parent);
		if (!slash)
			break;
		*slash = '/';
		part = slash + 1;
	}
	free(prefix);
	return status;
}

static launcher_status lx_stream(lx_fs *fs, const char *path,
	int write, int exclusive, FILE **out, launcher_error *error)
{
	if (!out)
		return lx_fail(error, LAUNCHER_INVALID, "Missing file output");
	*out = NULL;
	lx_fs *parent;
	char *leaf;
	launcher_status status = lx_win_parent(fs, path, 0, &parent, &leaf,
		error);
	if (status != LAUNCHER_OK)
		return status;
	HANDLE handle;
	/* OPEN_ALWAYS validates reparse points before truncation. */
	status = lx_win_file(parent, leaf, write ? GENERIC_WRITE : GENERIC_READ,
		write ? (exclusive ? CREATE_NEW : OPEN_ALWAYS) : OPEN_EXISTING,
		&handle, error);
	free(leaf);
	lx_fs_close(&parent);
	if (status != LAUNCHER_OK)
		return status;
	BY_HANDLE_FILE_INFORMATION info;
	if (!GetFileInformationByHandle(handle, &info) ||
		(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
		CloseHandle(handle);
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Expected regular file");
	}
	if (write && !SetEndOfFile(handle)) {
		DWORD code = GetLastError();
		CloseHandle(handle);
		return lx_native(error, (int)code, "Truncate file", path);
	}
	int descriptor = _open_osfhandle((intptr_t)handle,
		_O_BINARY | (write ? _O_WRONLY : _O_RDONLY));
	if (descriptor < 0) {
		CloseHandle(handle);
		return lx_native(error, errno, "Open descriptor", path);
	}
	FILE *stream = _fdopen(descriptor, write ? "wb" : "rb");
	if (!stream) {
		_close(descriptor);
		return lx_native(error, errno, "Open stream", path);
	}
	*out = stream;
	return LAUNCHER_OK;
}

static launcher_status lx_move(lx_fs *from, const char *source,
	lx_fs *to, const char *dest, int replace, launcher_error *error)
{
	lx_fs *a = NULL, *b = NULL;
	char *leaf_a = NULL, *leaf_b = NULL;
	launcher_status status = lx_win_parent(from, source, 0, &a, &leaf_a,
		error);
	if (status == LAUNCHER_OK)
		status = lx_win_parent(to, dest, 0, &b, &leaf_b, error);
	int kind;
	if (status == LAUNCHER_OK)
		status = lx_fs_stat(from, source, &kind, NULL, error);
	if (status == LAUNCHER_OK && !kind)
		status = lx_native(error, ERROR_FILE_NOT_FOUND, "Move source",
			source);
	if (status == LAUNCHER_OK)
		status = lx_fs_stat(to, dest, &kind, NULL, error);
	if (status == LAUNCHER_OK && kind && !replace)
		status = lx_fail(error, LAUNCHER_CONFLICT,
			"Move destination exists '%s'", dest);
	char *full_a = a && leaf_a ? lx_join(a->path, leaf_a) : NULL;
	char *full_b = b && leaf_b ? lx_join(b->path, leaf_b) : NULL;
	wchar_t *name_a = full_a ? lx_win_name(full_a) : NULL;
	wchar_t *name_b = full_b ? lx_win_name(full_b) : NULL;
	if (status == LAUNCHER_OK && (!name_a || !name_b))
		status = lx_fail(error, LAUNCHER_MEMORY,
			"Move allocation failed");
	if (status == LAUNCHER_OK && !MoveFileExW(name_a, name_b,
		MOVEFILE_WRITE_THROUGH |
			(replace ? MOVEFILE_REPLACE_EXISTING : 0)))
		status = lx_native(error, (int)GetLastError(), "Move", dest);
	free(name_a);
	free(name_b);
	free(full_a);
	free(full_b);
	free(leaf_a);
	free(leaf_b);
	lx_fs_close(&a);
	lx_fs_close(&b);
	return status;
}

launcher_status lx_fs_pin(lx_fs *fs, const char *path, int createparents,
	lx_fs **out, char **absolute, launcher_error *error)
{
	if (!out || *out || !absolute)
		return lx_fail(error, LAUNCHER_INVALID, "Invalid pin outputs");
	*absolute = NULL;
	lx_fs *parent;
	char *leaf;
	launcher_status status = lx_win_parent(fs, path, createparents, &parent,
		&leaf, error);
	int kind;
	if (status == LAUNCHER_OK)
		status = lx_fs_stat(fs, path, &kind, NULL, error);
	if (status == LAUNCHER_OK && kind == 2)
		status = lx_fail(error, LAUNCHER_CONFLICT,
			"Pinned file is directory");
	char *full = parent && leaf ? lx_join(parent->path, leaf) : NULL;
	free(leaf);
	if (status == LAUNCHER_OK && !full)
		status = lx_fail(error, LAUNCHER_MEMORY,
			"Pin allocation failed");
	if (status != LAUNCHER_OK) {
		free(full);
		lx_fs_close(&parent);
		return status;
	}
	*out = parent;
	*absolute = full;
	return LAUNCHER_OK;
}

launcher_status lx_fs_remove(lx_fs *fs, const char *path, int directory,
	launcher_error *error)
{
	int kind;
	launcher_status status = lx_fs_stat(fs, path, &kind, NULL, error);
	if (status != LAUNCHER_OK || !kind)
		return status;
	if (kind != (directory ? 2 : 1))
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Unexpected removal type");
	lx_fs *parent;
	char *leaf;
	status = lx_win_parent(fs, path, 0, &parent, &leaf, error);
	if (status != LAUNCHER_OK)
		return status;
	char *full = lx_join(parent->path, leaf);
	wchar_t *name = full ? lx_win_name(full) : NULL;
	if (!name)
		status = lx_fail(error, LAUNCHER_MEMORY,
			"Removal allocation failed");
	else if (!(directory ? RemoveDirectoryW(name) : DeleteFileW(name)))
		status = lx_native(error, (int)GetLastError(), "Remove", path);
	free(name);
	free(full);
	free(leaf);
	lx_fs_close(&parent);
	return status;
}

launcher_status lx_fs_flush(lx_fs *fs, launcher_error *error)
{
	if (!fs)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directory");
	/* Win32 does not expose POSIX directory fsync. Published file renames
		* use WRITE_THROUGH and each file's data is explicitly
		* flushed. */
	if (!FlushFileBuffers(fs->directory)) {
		DWORD code = GetLastError();
		if (code != ERROR_INVALID_HANDLE && code != ERROR_ACCESS_DENIED)
			return lx_native(error, (int)code, "Flush directory",
				fs->path);
	}
	return LAUNCHER_OK;
}

launcher_status lx_file_flush(FILE *file, launcher_error *error)
{
	if (!file)
		return lx_fail(error, LAUNCHER_INVALID, "Missing stream");
	if (fflush(file))
		return lx_native(error, errno, "Flush stream", "");
	HANDLE handle = (HANDLE)_get_osfhandle(_fileno(file));
	if (!FlushFileBuffers(handle))
		return lx_native(error, (int)GetLastError(), "Flush file", "");
	return LAUNCHER_OK;
}

launcher_status lx_fs_empty(lx_fs *fs, const char *path, int *empty,
	launcher_error *error)
{
	if (!empty)
		return lx_fail(error, LAUNCHER_INVALID, "Missing empty output");
	*empty = 0;
	char *sentinel = path && !*path ? strdup("*") :
		lx_join(path ? path : "", "*");
	if (!sentinel)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Path allocation failed");
	lx_fs *parent;
	char *leaf;
	launcher_status status = lx_win_parent(fs, sentinel, 0, &parent, &leaf,
		error);
	free(sentinel);
	free(leaf);
	if (status != LAUNCHER_OK)
		return status;
	char *pattern = lx_join(parent->path, "*");
	wchar_t *name = pattern ? lx_win_name(pattern) : NULL;
	if (!name) {
		free(pattern);
		lx_fs_close(&parent);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Directory allocation failed");
	}
	WIN32_FIND_DATAW entry;
	HANDLE find = FindFirstFileW(name, &entry);
	DWORD code = GetLastError();
	free(name);
	free(pattern);
	if (find == INVALID_HANDLE_VALUE) {
		if (code == ERROR_FILE_NOT_FOUND)
			*empty = 1;
		else
			status = lx_native(error, (int)code, "List directory",
				path);
	} else {
		*empty = 1;
		do {
			if (wcscmp(entry.cFileName, L".") &&
				wcscmp(entry.cFileName, L"..")) {
				*empty = 0;
				break;
			}
		} while (FindNextFileW(find, &entry));
		code = GetLastError();
		if (*empty && code != ERROR_NO_MORE_FILES)
			status = lx_native(error, (int)code, "List directory",
				path);
		FindClose(find);
	}
	lx_fs_close(&parent);
	return status;
}

launcher_status lx_fs_spell(lx_fs *fs, const char *path, char **out,
	launcher_error *error)
{
	if (!out)
		return lx_fail(error, LAUNCHER_INVALID,
			"Missing spelling output");
	*out = NULL;
	if (!fs || !lx_relative(path))
		return lx_fail(error, LAUNCHER_INVALID, "Unsafe spelling path");
	int kind;
	launcher_status status = lx_fs_stat(fs, path, &kind, NULL, error);
	if (status != LAUNCHER_OK)
		return status;
	char *result = strdup(path);
	char *full = strdup(fs->path);
	if (!result || !full) {
		free(result);
		free(full);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Spelling allocation failed");
	}
	/* FindFirstFile returns preserved spelling for existing components. */
	char *part = result;
	while (*part) {
		char *slash = strchr(part, '/');
		if (slash)
			*slash = 0;
		char *next = lx_join(full, part);
		wchar_t *native = next ? lx_win_name(next) : NULL;
		WIN32_FIND_DATAW entry;
		HANDLE find = native ? FindFirstFileW(native, &entry) :
			INVALID_HANDLE_VALUE;
		free(native);
		if (find != INVALID_HANDLE_VALUE) {
			char *actual = lx_utf8(entry.cFileName);
			FindClose(find);
			/* Portable names use ASCII case folding, which
				* leaves byte lengths unchanged on Windows. */
			if (actual && strlen(actual) == strlen(part))
				memcpy(part, actual, strlen(part));
			free(actual);
		}
		free(full);
		full = next;
		if (!full) {
			status = lx_fail(error, LAUNCHER_MEMORY,
				"Spelling allocation failed");
			break;
		}
		if (!slash)
			break;
		*slash = '/';
		part = slash + 1;
	}
	free(full);
	if (status != LAUNCHER_OK) {
		free(result);
		return status;
	}
	*out = result;
	return LAUNCHER_OK;
}

launcher_status lx_fs_executable(lx_fs *fs, const char *path,
	launcher_error *error)
{
	int kind;
	launcher_status status = lx_fs_stat(fs, path, &kind, NULL, error);
	if (status == LAUNCHER_OK && kind != 1)
		status = lx_fail(error, LAUNCHER_CONFLICT,
			"Missing executable");
	return status;
}

launcher_status lx_fs_list(lx_fs *fs, const char *path,
	int (*visit)(const char *name, int kind, void *state), void *state,
	launcher_error *error)
{
	if (!fs || !path || !visit)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid list arguments");
	char *sentinel = !*path ? strdup("*") : lx_join(path, "*");
	if (!sentinel)
		return lx_fail(error, LAUNCHER_MEMORY,
			"List allocation failed");
	lx_fs *parent;
	char *leaf;
	launcher_status status = lx_win_parent(fs, sentinel, 0, &parent, &leaf,
		error);
	free(sentinel);
	free(leaf);
	if (status != LAUNCHER_OK)
		return status;
	char *pattern = lx_join(parent->path, "*");
	wchar_t *native = pattern ? lx_win_name(pattern) : NULL;
	free(pattern);
	if (!native) {
		lx_fs_close(&parent);
		return lx_fail(error, LAUNCHER_MEMORY,
			"List allocation failed");
	}
	WIN32_FIND_DATAW entry;
	HANDLE find = FindFirstFileW(native, &entry);
	free(native);
	DWORD code = GetLastError();
	if (find == INVALID_HANDLE_VALUE) {
		if (code != ERROR_FILE_NOT_FOUND)
			status = lx_native(error, (int)code, "List directory",
				path);
	} else {
		for (;;) {
			if (wcscmp(entry.cFileName, L".") &&
				wcscmp(entry.cFileName, L"..")) {
				char *name = lx_utf8(entry.cFileName);
				int kind = entry.dwFileAttributes &
					FILE_ATTRIBUTE_REPARSE_POINT ? 3 :
					(entry.dwFileAttributes &
						FILE_ATTRIBUTE_DIRECTORY ?
						2 : 1);
				if (!name)
					status = lx_fail(error, LAUNCHER_IO,
						"Invalid directory filename");
				else if (!visit(name, kind, state))
					status = lx_fail(error,
						LAUNCHER_CONFLICT,
						"Visitation stopped at '%s'",
						name);
				free(name);
				if (status != LAUNCHER_OK)
					break;
			}
			if (!FindNextFileW(find, &entry)) {
				code = GetLastError();
				if (code != ERROR_NO_MORE_FILES)
					status = lx_native(error, (int)code,
						"List directory", path);
				break;
			}
		}
		FindClose(find);
	}
	lx_fs_close(&parent);
	return status;
}
