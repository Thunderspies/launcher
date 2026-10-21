#include "internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

struct lx_fs {
	char *path;
#ifdef _WIN32
	HANDLE *ancestors;
	size_t count;
	HANDLE directory;
	HANDLE lock;
#else
	int directory;
	int lock;
#endif
};

static launcher_status lx_native(launcher_error *error, int code,
	const char *operation, const char *path)
{
	launcher_status status = LAUNCHER_IO;
#ifdef _WIN32
	if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS ||
		code == ERROR_CANT_ACCESS_FILE || code == ERROR_DIRECTORY)
#else
	if (code == EEXIST || code == ELOOP || code == ENOTDIR)
#endif
		status = LAUNCHER_CONFLICT;
	lx_fail(error, status, "%s '%s' failed (%d)", operation,
		path ? path : "", code);
	if (error)
		error->native_code = code;
	return status;
}

static int lx_relative(const char *path)
{
	if (!path || !*path || *path == '/' || *path == '\\')
		return 0;
	const char *begin = path;
	for (const char *p = path;; ++p) {
		if (*p == '\\' || *p == ':')
			return 0;
		if (*p && *p != '/')
			continue;
		size_t n = (size_t)(p - begin);
		if (!n || (n == 1 && *begin == '.') ||
			(n == 2 && begin[0] == '.' && begin[1] == '.'))
			return 0;
		if (!*p)
			return 1;
		begin = p + 1;
	}
}

static char *lx_join(const char *base, const char *path)
{
	size_t a = strlen(base), b = strlen(path);
	if (a > SIZE_MAX - b - 2)
		return NULL;
	char *result = (char *)malloc(a + b + 2);
	if (!result)
		return NULL;
	memcpy(result, base, a);
	if (a && base[a - 1] != '/')
		result[a++] = '/';
	memcpy(result + a, path, b + 1);
	return result;
}

#ifndef _WIN32

static int lx_dir_error(int parent, const char *name, int code)
{
	struct stat info;
	if (!fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) &&
		!S_ISDIR(info.st_mode))
		return ELOOP;
	return code;
}

static launcher_status lx_parent(lx_fs *fs, const char *path, int create,
	int missing, int *out, char **leaf, launcher_error *error)
{
	*out = -1;
	*leaf = NULL;
	if (!fs || !lx_relative(path))
		return lx_fail(error, LAUNCHER_INVALID, "Unsafe relative path");
	char *copy = strdup(path);
	if (!copy)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Path allocation failed");
	int directory = fcntl(fs->directory, F_DUPFD_CLOEXEC, 0);
	if (directory < 0) {
		free(copy);
		return lx_native(error, errno, "Retain directory", path);
	}
	char *part = copy;
	char *slash;
	while ((slash = strchr(part, '/'))) {
		*slash = 0;
		int child = openat(directory, part,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0 && errno == ENOENT && create) {
			if (mkdirat(directory, part, 0700) && errno != EEXIST) {
				int code = errno;
				close(directory);
				free(copy);
				return lx_native(error, code,
					"Create directory",
					path);
			}
			if (fsync(directory)) {
				int code = errno;
				close(directory);
				free(copy);
				return lx_native(error, code, "Flush directory",
					path);
			}
			child = openat(directory, part,
				O_RDONLY | O_DIRECTORY | O_NOFOLLOW |
					O_CLOEXEC);
		}
		if (child < 0) {
			int code = lx_dir_error(directory, part, errno);
			close(directory);
			free(copy);
			if (missing && code == ENOENT)
				return LAUNCHER_OK;
			return lx_native(error, code, "Open parent", path);
		}
		close(directory);
		directory = child;
		part = slash + 1;
	}
	*leaf = strdup(part);
	free(copy);
	if (!*leaf) {
		close(directory);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Path allocation failed");
	}
	*out = directory;
	return LAUNCHER_OK;
}

static char *lx_absolute(const char *path)
{
	if (!path || !*path)
		return NULL;
	char *input;
	if (*path == '/') {
		input = strdup(path);
	} else {
		char *cwd = getcwd(NULL, 0);
		if (!cwd)
			return NULL;
		input = lx_join(cwd, path);
		free(cwd);
	}
	if (!input)
		return NULL;
	char *result = (char *)malloc(strlen(input) + 2);
	if (!result) {
		free(input);
		return NULL;
	}
	size_t used = 1;
	result[0] = '/';
	char *cursor = input;
	while (*cursor) {
		while (*cursor == '/')
			++cursor;
		char *begin = cursor;
		while (*cursor && *cursor != '/')
			++cursor;
		size_t n = (size_t)(cursor - begin);
		if (!n || (n == 1 && begin[0] == '.'))
			continue;
		if (n == 2 && begin[0] == '.' && begin[1] == '.') {
			while (used > 1 && result[used - 1] != '/')
				--used;
			if (used > 1)
				--used;
			continue;
		}
		if (used > 1)
			result[used++] = '/';
		memcpy(result + used, begin, n);
		used += n;
	}
	result[used] = 0;
	free(input);
	return result;
}

launcher_status lx_fs_open(const char *path, int create, lx_fs **out,
	launcher_error *error)
{
	if (!out || *out || !path || !*path)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid directory output");
	char *absolute = lx_absolute(path);
	if (!absolute)
		return lx_fail(error, LAUNCHER_MEMORY, "Absolute path failed");
	int directory = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	char *copy = strdup(absolute + 1);
	if (directory < 0 || !copy) {
		int code = errno;
		if (directory >= 0)
			close(directory);
		free(absolute);
		free(copy);
		return lx_native(error, code, "Open root", path);
	}
	char *part = copy;
	while (*part) {
		char *slash = strchr(part, '/');
		if (slash)
			*slash = 0;
		int child = openat(directory, part,
			O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
		if (child < 0 && errno == ENOENT && create) {
			if (!mkdirat(directory, part, 0700) ||
				errno == EEXIST) {
				if (!fsync(directory))
					child = openat(directory, part,
						O_RDONLY |
						O_DIRECTORY | O_NOFOLLOW |
							O_CLOEXEC);
			}
		}
		if (child < 0) {
			int code = lx_dir_error(directory, part, errno);
			close(directory);
			free(absolute);
			free(copy);
			return lx_native(error, code, "Open directory", path);
		}
		close(directory);
		directory = child;
		if (!slash)
			break;
		part = slash + 1;
	}
	free(copy);
	lx_fs *fs = (lx_fs *)calloc(1, sizeof(*fs));
	if (!fs) {
		close(directory);
		free(absolute);
		return lx_fail(error, LAUNCHER_MEMORY,
			"Directory allocation failed");
	}
	fs->path = absolute;
	fs->directory = directory;
	fs->lock = -1;
	*out = fs;
	return LAUNCHER_OK;
}

void lx_fs_close(lx_fs **fs)
{
	if (!fs || !*fs)
		return;
	if ((*fs)->lock >= 0)
		close((*fs)->lock);
	close((*fs)->directory);
	free((*fs)->path);
	free(*fs);
	*fs = NULL;
}

launcher_status lx_fs_same_volume(lx_fs *a, lx_fs *b,
	launcher_error *error)
{
	struct stat first, second;
	if (!a || !b)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directories");
	if (fstat(a->directory, &first) || fstat(b->directory, &second))
		return lx_native(error, errno, "Inspect volume", "");
	if (first.st_dev != second.st_dev)
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Installation and state must share a filesystem");
	return LAUNCHER_OK;
}

launcher_status lx_fs_identity(lx_fs *fs, char out[128],
	launcher_error *error)
{
	if (out)
		*out = 0;
	if (!fs || !out)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid identity output");
	struct stat info;
	if (fstat(fs->directory, &info))
		return lx_native(error, errno, "Directory identity", fs->path);
	snprintf(out, 128, "posix:%llu:%llu",
		(unsigned long long)info.st_dev,
			(unsigned long long)info.st_ino);
	return LAUNCHER_OK;
}

launcher_status lx_fs_validate(lx_fs *fs, launcher_error *error)
{
	if (!fs)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directory");
	lx_fs *current = NULL;
	launcher_status status = lx_fs_open(fs->path, 0, &current, error);
	struct stat before, after;
	if (status == LAUNCHER_OK) {
		if (fstat(fs->directory, &before) ||
			fstat(current->directory, &after))
			status = lx_native(error, errno, "Validate directory",
				fs->path);
		else if (before.st_dev != after.st_dev ||
				before.st_ino != after.st_ino)
			status = lx_fail(error, LAUNCHER_CONFLICT,
				"Directory was substituted '%s'", fs->path);
	}
	lx_fs_close(&current);
	return status;
}

launcher_status lx_fs_lock(lx_fs *fs, launcher_error *error)
{
	if (!fs)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directory");
	if (fs->lock >= 0)
		return LAUNCHER_OK;
	int descriptor = openat(fs->directory, "lock",
		O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (descriptor < 0)
		return lx_native(error, errno, "Open lock", fs->path);
	struct stat info;
	if (fstat(descriptor, &info) || !S_ISREG(info.st_mode)) {
		close(descriptor);
		return lx_fail(error, LAUNCHER_CONFLICT, "Unsafe lock file");
	}
	if (flock(descriptor, LOCK_EX | LOCK_NB)) {
		int code = errno;
		close(descriptor);
		if (code == EWOULDBLOCK)
			return lx_fail(error, LAUNCHER_BUSY,
				"Installation is already locked");
		return lx_native(error, code, "Lock", fs->path);
	}
	fs->lock = descriptor;
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
	int parent;
	char *leaf;
	launcher_status status = lx_parent(fs, path, 0, 1, &parent, &leaf,
		error);
	if (status != LAUNCHER_OK || parent < 0)
		return status;
	struct stat info;
	int result = fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW);
	int code = errno;
	close(parent);
	free(leaf);
	if (result)
		return code == ENOENT ? LAUNCHER_OK :
			lx_native(error, code, "Inspect file", path);
	if (S_ISREG(info.st_mode)) {
		*kind = 1;
		if (size)
			*size = (uint64_t)info.st_size;
	} else if (S_ISDIR(info.st_mode)) {
		*kind = 2;
	} else {
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Unsafe file type at '%s'", path);
	}
	return LAUNCHER_OK;
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
	int parent;
	char *leaf;
	launcher_status status = lx_parent(fs, sentinel, 1, 0, &parent,
		&leaf, error);
	if (parent >= 0)
		close(parent);
	free(leaf);
	free(sentinel);
	return status;
}

launcher_status lx_fs_case(lx_fs *fs, const char *path,
	launcher_error *error)
{
	if (!fs || !path || (*path && !lx_relative(path)))
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid directory case");
	return LAUNCHER_OK;
}

static launcher_status lx_stream(lx_fs *fs, const char *path,
	int write, int exclusive, FILE **out, launcher_error *error)
{
	if (!out)
		return lx_fail(error, LAUNCHER_INVALID, "Missing file output");
	*out = NULL;
	int parent;
	char *leaf;
	launcher_status status = lx_parent(fs, path, 0, 0, &parent, &leaf,
		error);
	if (status != LAUNCHER_OK)
		return status;
	int flags = O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK |
		(write ? O_WRONLY : O_RDONLY);
	if (write)
		flags |= O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
	int descriptor = openat(parent, leaf, flags, 0600);
	int code = errno;
	close(parent);
	free(leaf);
	if (descriptor < 0)
		return lx_native(error, code, "Open file", path);
	struct stat info;
	if (fstat(descriptor, &info) || !S_ISREG(info.st_mode)) {
		close(descriptor);
		return lx_fail(error, LAUNCHER_CONFLICT,
			"Expected regular file at '%s'", path);
	}
	FILE *stream = fdopen(descriptor, write ? "wb" : "rb");
	if (!stream) {
		code = errno;
		close(descriptor);
		return lx_native(error, code, "Open stream", path);
	}
	*out = stream;
	return LAUNCHER_OK;
}

static launcher_status lx_move(lx_fs *from, const char *source,
	lx_fs *to, const char *dest, int replace, launcher_error *error)
{
	int a = -1, b = -1;
	char *name_a = NULL, *name_b = NULL;
	launcher_status status = lx_parent(from, source, 0, 0, &a, &name_a,
		error);
	if (status == LAUNCHER_OK)
		status = lx_parent(to, dest, 0, 0, &b, &name_b, error);
	struct stat info;
	if (status == LAUNCHER_OK) {
		if (fstatat(a, name_a, &info, AT_SYMLINK_NOFOLLOW))
			status = lx_native(error, errno, "Inspect source",
				source);
		else if (!S_ISREG(info.st_mode) && !S_ISDIR(info.st_mode))
			status = lx_fail(error, LAUNCHER_CONFLICT,
				"Unsafe move source '%s'", source);
	}
	if (status == LAUNCHER_OK) {
		if (!fstatat(b, name_b, &info, AT_SYMLINK_NOFOLLOW)) {
			if (!replace || (!S_ISREG(info.st_mode) &&
						!S_ISDIR(info.st_mode)))
				status = lx_fail(error, LAUNCHER_CONFLICT,
					"Move destination exists '%s'", dest);
		} else if (errno != ENOENT) {
			status = lx_native(error, errno, "Inspect destination",
				dest);
		}
	}
	int same_parent = 0;

	if (status == LAUNCHER_OK) {
		struct stat first, second;

		if (fstat(a, &first) || fstat(b, &second))
			status = lx_native(error, errno, "Inspect move parents",
				dest);
		else
			same_parent = first.st_dev == second.st_dev &&
				first.st_ino == second.st_ino;
	}
	if (status == LAUNCHER_OK) {
		int result;
		if (replace)
			result = renameat(a, name_a, b, name_b);
		else
			result = (int)syscall(SYS_renameat2, a, name_a, b,
				name_b, 1 /* RENAME_NOREPLACE */);
		if (result)
			status = lx_native(error, errno, "Move", dest);
		else if (fsync(a) || (!same_parent && fsync(b)))
			status = lx_native(error, errno, "Flush move", dest);
	}
	if (a >= 0)
		close(a);
	if (b >= 0)
		close(b);
	free(name_a);
	free(name_b);
	return status;
}

launcher_status lx_fs_pin(lx_fs *fs, const char *path, int createparents,
	lx_fs **out, char **absolute, launcher_error *error)
{
	if (!out || *out || !absolute)
		return lx_fail(error, LAUNCHER_INVALID, "Invalid pin outputs");
	*absolute = NULL;
	int directory;
	char *leaf;
	launcher_status status = lx_parent(fs, path, createparents, 0,
		&directory, &leaf, error);
	if (status != LAUNCHER_OK)
		return status;
	struct stat info;
	if (!fstatat(directory, leaf, &info, AT_SYMLINK_NOFOLLOW)) {
		if (!S_ISREG(info.st_mode))
			status = lx_fail(error, LAUNCHER_CONFLICT,
				"Unsafe pinned file '%s'", path);
	} else if (errno != ENOENT) {
		status = lx_native(error, errno, "Inspect pinned file", path);
	}
	lx_fs *parent = (lx_fs *)calloc(1, sizeof(*parent));
	char *native = lx_join(fs->path, path);
	free(leaf);
	if (status == LAUNCHER_OK && (!parent || !native))
		status = lx_fail(error, LAUNCHER_MEMORY,
			"Pin allocation failed");
	if (status != LAUNCHER_OK) {
		free(parent);
		free(native);
		close(directory);
		return status;
	}
	parent->directory = directory;
	parent->lock = -1;
	parent->path = native ? strdup(native) : NULL;
	if (parent->path) {
		char *slash = strrchr(parent->path, '/');
		if (slash == parent->path)
			slash[1] = 0;
		else if (slash)
			*slash = 0;
	}
	if (!parent->path) {
		lx_fs_close(&parent);
		free(native);
		return lx_fail(error, LAUNCHER_MEMORY, "Pin allocation failed");
	}
	*out = parent;
	*absolute = native;
	return LAUNCHER_OK;
}

launcher_status lx_fs_remove(lx_fs *fs, const char *path, int directory,
	launcher_error *error)
{
	int parent;
	char *leaf;
	launcher_status status = lx_parent(fs, path, 0, 1, &parent, &leaf,
		error);
	if (status != LAUNCHER_OK || parent < 0)
		return status;
	struct stat info;
	if (fstatat(parent, leaf, &info, AT_SYMLINK_NOFOLLOW)) {
		if (errno != ENOENT)
			status = lx_native(error, errno, "Inspect removal",
				path);
	} else if ((directory && !S_ISDIR(info.st_mode)) ||
			(!directory && !S_ISREG(info.st_mode))) {
		status = lx_fail(error, LAUNCHER_CONFLICT,
			"Unsafe removal type at '%s'", path);
	} else if (unlinkat(parent, leaf, directory ? AT_REMOVEDIR : 0)) {
		status = lx_native(error, errno, "Remove", path);
	} else if (fsync(parent)) {
		status = lx_native(error, errno, "Flush removal", path);
	}
	close(parent);
	free(leaf);
	return status;
}

launcher_status lx_fs_flush(lx_fs *fs, launcher_error *error)
{
	if (!fs)
		return lx_fail(error, LAUNCHER_INVALID, "Missing directory");
	if (fsync(fs->directory))
		return lx_native(error, errno, "Flush directory", fs->path);
	return LAUNCHER_OK;
}

launcher_status lx_file_flush(FILE *file, launcher_error *error)
{
	if (!file)
		return lx_fail(error, LAUNCHER_INVALID, "Missing stream");
	if (fflush(file) || fsync(fileno(file)))
		return lx_native(error, errno, "Flush file", "");
	return LAUNCHER_OK;
}

launcher_status lx_fs_empty(lx_fs *fs, const char *path, int *empty,
	launcher_error *error)
{
	if (!empty)
		return lx_fail(error, LAUNCHER_INVALID, "Missing empty output");
	*empty = 0;
	int parent;
	char *leaf;
	launcher_status status;
	if (fs && path && !*path) {
		parent = fcntl(fs->directory, F_DUPFD_CLOEXEC, 0);
		leaf = strdup(".");
		status = parent >= 0 && leaf ? LAUNCHER_OK :
			lx_fail(error, LAUNCHER_MEMORY,
				"Root retention failed");
	} else {
		status = lx_parent(fs, path, 0, 0, &parent, &leaf, error);
	}
	if (status != LAUNCHER_OK) {
		if (parent >= 0)
			close(parent);
		free(leaf);
		return status;
	}
	int descriptor = openat(parent, leaf,
		O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	int code = descriptor < 0 ? lx_dir_error(parent, leaf, errno) : 0;
	close(parent);
	free(leaf);
	if (descriptor < 0)
		return lx_native(error, code, "Open directory", path);
	DIR *directory = fdopendir(descriptor);
	if (!directory) {
		code = errno;
		close(descriptor);
		return lx_native(error, code, "Read directory", path);
	}
	*empty = 1;
	errno = 0;
	struct dirent *entry;
	while ((entry = readdir(directory))) {
		if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
			*empty = 0;
			break;
		}
	}
	code = errno;
	closedir(directory);
	if (code)
		return lx_native(error, code, "Read directory", path);
	return LAUNCHER_OK;
}

launcher_status lx_fs_spell(lx_fs *fs, const char *path, char **out,
	launcher_error *error)
{
	if (!out)
		return lx_fail(error, LAUNCHER_INVALID,
			"Missing spelling output");
	*out = NULL;
	int kind;
	launcher_status status = lx_fs_stat(fs, path, &kind, NULL, error);
	if (status != LAUNCHER_OK)
		return status;
	*out = strdup(path);
	return *out ? LAUNCHER_OK :
		lx_fail(error, LAUNCHER_MEMORY, "Spelling allocation failed");
}

launcher_status lx_fs_list(lx_fs *fs, const char *path,
	int (*visit)(const char *name, int kind, void *state), void *state,
	launcher_error *error)
{
	if (!fs || !path || !visit)
		return lx_fail(error, LAUNCHER_INVALID,
			"Invalid list arguments");
	int descriptor = -1;
	launcher_status status = LAUNCHER_OK;
	if (!*path) {
		descriptor = openat(fs->directory, ".", O_RDONLY | O_DIRECTORY |
			O_CLOEXEC);
	} else {
		int parent;
		char *leaf;
		status = lx_parent(fs, path, 0, 0, &parent, &leaf, error);
		if (status != LAUNCHER_OK)
			return status;
		descriptor = openat(parent, leaf, O_RDONLY | O_DIRECTORY |
			O_NOFOLLOW | O_CLOEXEC);
		int code = errno;
		close(parent);
		free(leaf);
		if (descriptor < 0)
			return lx_native(error, code, "List directory", path);
	}
	if (descriptor < 0)
		return lx_native(error, errno, "List root", path);
	DIR *directory = fdopendir(descriptor);
	if (!directory) {
		int code = errno;
		close(descriptor);
		return lx_native(error, code, "List directory", path);
	}
	for (;;) {
		errno = 0;
		struct dirent *entry = readdir(directory);
		if (!entry) {
			if (errno)
				status = lx_native(error, errno,
					"List directory",
					path);
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
			continue;
		struct stat info;
		if (fstatat(descriptor, entry->d_name, &info,
			AT_SYMLINK_NOFOLLOW)) {
			status = lx_native(error, errno, "Inspect listed file",
				entry->d_name);
			break;
		}
		int kind = S_ISREG(info.st_mode) ? 1 :
			(S_ISDIR(info.st_mode) ? 2 : 3);
		if (!visit(entry->d_name, kind, state)) {
			status = lx_fail(error, LAUNCHER_CONFLICT,
				"Directory visitation stopped at '%s'",
					entry->d_name);
			break;
		}
	}
	closedir(directory);
	return status;
}

launcher_status lx_fs_executable(lx_fs *fs, const char *path,
	launcher_error *error)
{
	int parent;
	char *leaf;
	launcher_status status = lx_parent(fs, path, 0, 0, &parent, &leaf,
		error);
	if (status != LAUNCHER_OK)
		return status;
	int descriptor = openat(parent, leaf, O_RDONLY | O_NOFOLLOW |
		O_CLOEXEC);
	int code = errno;
	close(parent);
	free(leaf);
	if (descriptor < 0)
		return lx_native(error, code, "Open executable", path);
	struct stat info;
	if (fstat(descriptor, &info) || !S_ISREG(info.st_mode)) {
		status = lx_fail(error, LAUNCHER_CONFLICT, "Unsafe executable");
	} else if (!(info.st_mode & S_IXUSR) &&
		(fchmod(descriptor, info.st_mode | S_IXUSR) ||
			fsync(descriptor))) {
		status = lx_native(error, errno, "Set executable permission",
			path);
	}
	close(descriptor);
	return status;
}

#else
#include "platform_windows.h"
#endif

const char *lx_fs_path(lx_fs *fs)
{
	return fs ? fs->path : NULL;
}

launcher_status lx_fs_read(lx_fs *fs, const char *path, FILE **out,
	launcher_error *error)
{
	return lx_stream(fs, path, 0, 0, out, error);
}

launcher_status lx_fs_write(lx_fs *fs, const char *path, int exclusive,
	FILE **out, launcher_error *error)
{
	return lx_stream(fs, path, 1, exclusive, out, error);
}

launcher_status lx_fs_move(lx_fs *from, const char *source, lx_fs *to,
	const char *dest, launcher_error *error)
{
	return lx_move(from, source, to, dest, 0, error);
}

launcher_status lx_fs_replace(lx_fs *from, const char *source, lx_fs *to,
	const char *dest, launcher_error *error)
{
	return lx_move(from, source, to, dest, 1, error);
}

static launcher_status lx_read_marker(lx_fs *fs, char text[256], int *found,
	launcher_error *error)
{
	*found = 0;
	int kind;
	launcher_status status = lx_fs_stat(fs, "running.json", &kind, NULL,
		error);
	if (status != LAUNCHER_OK || !kind)
		return status;
	FILE *file = NULL;
	status = lx_fs_read(fs, "running.json", &file, error);
	if (status != LAUNCHER_OK)
		return status;
	size_t count = fread(text, 1, 255, file);
	int invalid = ferror(file) || !feof(file);
	fclose(file);
	text[count] = 0;
	if (invalid || memchr(text, 0, count))
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Invalid running process marker");
	*found = 1;
	return LAUNCHER_OK;
}

static launcher_status lx_write_marker(lx_fs *fs, const char *text,
	launcher_error *error)
{
	FILE *file = NULL;
	launcher_status status = lx_fs_write(fs, "running.tmp", 0, &file,
		error);
	if (status != LAUNCHER_OK)
		return status;
	size_t size = strlen(text);
	if (fwrite(text, 1, size, file) != size)
		status = lx_native(error, errno, "Write process marker", "");
	if (status == LAUNCHER_OK)
		status = lx_file_flush(file, error);
	if (fclose(file) && status == LAUNCHER_OK)
		status = lx_native(error, errno, "Close process marker", "");
	if (status == LAUNCHER_OK)
		status = lx_fs_replace(fs, "running.tmp", fs, "running.json",
			error);
	return status;
}

#ifndef _WIN32

static int lx_boot(char out[64])
{
	FILE *file = fopen("/proc/sys/kernel/random/boot_id", "rb");
	if (!file)
		return 0;
	int ok = fgets(out, 64, file) != NULL;
	fclose(file);
	if (!ok)
		return 0;
	size_t n = strlen(out);
	if (n && out[n - 1] == '\n')
		out[--n] = 0;
	return n == 36;
}

static int lx_start(pid_t pid, unsigned long long *start)
{
	char path[64], text[4096];
	snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);
	FILE *file = fopen(path, "rb");
	if (!file)
		return errno == ENOENT ? 0 : -1;
	int ok = fgets(text, sizeof(text), file) != NULL;
	fclose(file);
	if (!ok)
		return -1;
	char *p = strrchr(text, ')');
	if (!p || p[1] != ' ')
		return -1;
	p += 2;
	if (*p == 'Z' || *p == 'X')
		return 0;
	for (int field = 3; field < 22; ++field) {
		p = strchr(p, ' ');
		if (!p)
			return -1;
		++p;
	}
	char *end;
	errno = 0;
	*start = strtoull(p, &end, 10);
	return errno || end == p || (*end != ' ' && *end != '\n') ? -1 : 1;
}

launcher_status lx_process_busy(lx_fs *state, launcher_error *error)
{
	char text[256], boot[64], saved[64];
	int found;
	launcher_status status = lx_read_marker(state, text, &found, error);
	if (status != LAUNCHER_OK || !found)
		return status;
	unsigned long long pid, start;
	int end = 0;
	if (sscanf(text,
		"{\"pid\":%llu,\"start\":%llu,\"boot\":\"%63[^\"]\"}%n",
			&pid, &start, saved, &end) != 3 || text[end] ||
		!pid || pid > 2147483647ULL || !lx_boot(boot))
		return lx_fail(error, LAUNCHER_RECOVERY_REQUIRED,
			"Invalid running process identity");
	unsigned long long actual;
	int alive = strcmp(boot, saved) ? 0 : lx_start((pid_t)pid, &actual);
	if (alive < 0)
		return lx_fail(error, LAUNCHER_BUSY,
			"Cannot verify recorded process identity");
	if (alive && actual == start)
		return lx_fail(error, LAUNCHER_BUSY,
			"Game process is still running");
	return lx_fs_remove(state, "running.json", 0, error);
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
	launcher_status status = lx_process_busy(state, error);
	if (status != LAUNCHER_OK)
		return status;
	char **argv = (char **)calloc(count + 1, sizeof(*argv));
	if (!argv)
		return lx_fail(error, LAUNCHER_MEMORY,
			"Argument allocation failed");
	for (size_t i = 0; i < count; ++i) {
		if (!args[i]) {
			free(argv);
			return lx_fail(error, LAUNCHER_INVALID,
				"Null launch argument");
		}
		argv[i] = (char *)args[i];
	}
	lx_fs *directory = NULL;
	status = lx_fs_open(cwd, 0, &directory, error);
	if (status != LAUNCHER_OK) {
		free(argv);
		return status;
	}
	int gate[2], report[2];
	if (pipe2(gate, O_CLOEXEC)) {
		status = lx_native(error, errno, "Create startup gate", "");
		lx_fs_close(&directory);
		free(argv);
		return status;
	}
	if (pipe2(report, O_CLOEXEC)) {
		status = lx_native(error, errno, "Create startup report", "");
		close(gate[0]);
		close(gate[1]);
		lx_fs_close(&directory);
		free(argv);
		return status;
	}
	pid_t pid = fork();
	if (!pid) {
		close(gate[1]);
		close(report[0]);
		/* The child must not retain the parent's installation lock. */
		if (state->lock >= 0)
			close(state->lock);
		char permit;
		ssize_t n;
		do {
			n = read(gate[0], &permit, 1);
		} while (n < 0 && errno == EINTR);
		close(gate[0]);
		if (n != 1 || permit != 'x')
			_exit(125);
		if (!fchdir(directory->directory))
			execvp(argv[0], argv);
		int code = errno;
		ssize_t reported;
		do {
			reported = write(report[1], &code, sizeof(code));
		} while (reported < 0 && errno == EINTR);
		(void)reported;
		_exit(126);
	}
	close(report[1]);
	if (pid < 0) {
		status = lx_native(error, errno, "Fork process", args[0]);
	} else {
		char boot[64], marker[256];
		unsigned long long start;
		if (!lx_boot(boot) || lx_start(pid, &start) != 1) {
			status = lx_fail(error, LAUNCHER_IO,
				"Cannot obtain child creation identity");
		} else {
			snprintf(marker, sizeof(marker),
				"{\"pid\":%llu,\"start\":%llu,\"boot\":\"%s\"}",
				(unsigned long long)pid, start, boot);
			status = lx_write_marker(state, marker, error);
		}
		if (status == LAUNCHER_OK) {
			char permit = 'x';
			/* Retaining our read endpoint until this write prevents
				* SIGPIPE if the child was externally killed. */
			if (write(gate[1], &permit, 1) != 1)
				status = lx_native(error, errno,
					"Start process",
					args[0]);
		}
	}
	close(gate[0]);
	close(gate[1]);
	if (pid > 0) {
		int exec_error = 0;
		ssize_t n;
		do {
			n = read(report[0], &exec_error, sizeof(exec_error));
		} while (n < 0 && errno == EINTR);
		if (status == LAUNCHER_OK && n != 0)
			status = lx_native(error, n > 0 ? exec_error : errno,
				"Execute", args[0]);
		int result;
		pid_t waited;
		do {
			waited = waitpid(pid, &result, 0);
		} while (waited < 0 && errno == EINTR);
		if (status == LAUNCHER_OK && waited < 0)
			status = lx_native(error, errno, "Wait process",
				args[0]);
		if (status == LAUNCHER_OK)
			*exit_code = WIFEXITED(result) ? WEXITSTATUS(result) :
				128 + WTERMSIG(result);
		launcher_error cleanup;
		launcher_status removed = lx_fs_remove(state, "running.json", 0,
			&cleanup);
		if (status == LAUNCHER_OK && removed != LAUNCHER_OK) {
			status = removed;
			if (error)
				*error = cleanup;
		}
	}
	close(report[0]);
	lx_fs_close(&directory);
	free(argv);
	return status;
}

#else
#include "process_windows.h"
#endif
