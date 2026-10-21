#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Test-only syscall faults are scoped to one installation and sidecar. */
static int armed;
static int read_failed;
static int journal_published;
static int compact_renamed;
static int compact_failed;
static dev_t private_device;
static ino_t private_inode;

static const char *mode(void)
{
	const char *value = getenv("LAUNCHER_FAULT_MODE");

	return value ? value : "";
}

static void record(const char *event)
{
	const char *path = getenv("LAUNCHER_FAULT_LOG");
	if (!path)
		return;
	int file = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
	if (file >= 0) {
		ssize_t written = write(file, event, strlen(event));
		(void)written;
		close(file);
	}
}

static int private_fd(int descriptor)
{
	struct stat info;

	return !fstat(descriptor, &info) && S_ISDIR(info.st_mode) &&
		info.st_dev == private_device && info.st_ino == private_inode;
}

static void initialize(void)
{
	static int initialized;
	if (initialized)
		return;
	initialized = 1;
	const char *root = getenv("LAUNCHER_FAULT_PRIVATE_ROOT");
	struct stat info;
	if (root && !stat(root, &info)) {
		private_device = info.st_dev;
		private_inode = info.st_ino;
		armed = getenv("LAUNCHER_FAULT_ARMED") != NULL;
		journal_published = armed;
	}
}

static void published(int result, int directory, const char *path)
{
	initialize();
	if (!result && !strcmp(path, "journal.json") &&
		private_fd(directory)) {
		journal_published = 1;
		record("journal-published\n");
		if (!strcmp(mode(), "journal-kill"))
			_exit(86);
	}
	if (!result && !strcmp(path, "installed-state.json") &&
		private_fd(directory)) {
		armed = 1;
		record("renamed-state\n");
	}
}

static int fail_read(int directory, const char *path, int flags)
{
	initialize();
	if (armed && !read_failed && !strcmp(mode(), "read-state-once") &&
		(flags & O_ACCMODE) == O_RDONLY &&
		!strcmp(path, "installed-state.json") &&
		private_fd(directory)) {
		read_failed = 1;
		record("failed-state-read\n");
		errno = EIO;
		return 1;
	}
	return 0;
}

static void symbol(const char *name, void *out, size_t size)
{
	void *address = dlsym(RTLD_NEXT, name);
	if (!address || size != sizeof(address))
		_exit(121);
	memcpy(out, &address, size);
}

int openat(int directory, const char *path, int flags, ...)
{
	static int (*next)(int, const char *, int, ...);
	if (!next)
		symbol("openat", &next, sizeof(next));
	mode_t permissions = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list args;
		va_start(args, flags);
		permissions = (mode_t)va_arg(args, int);
		va_end(args);
	}
	if (fail_read(directory, path, flags))
		return -1;
	return next(directory, path, flags, permissions);
}

int openat64(int directory, const char *path, int flags, ...)
{
	static int (*next)(int, const char *, int, ...);
	if (!next)
		symbol("openat64", &next, sizeof(next));
	mode_t permissions = 0;
	if ((flags & O_CREAT) || (flags & O_TMPFILE) == O_TMPFILE) {
		va_list args;
		va_start(args, flags);
		permissions = (mode_t)va_arg(args, int);
		va_end(args);
	}
	if (fail_read(directory, path, flags))
		return -1;
	return next(directory, path, flags, permissions);
}

int __openat_2(int directory, const char *path, int flags)
{
	static int (*next)(int, const char *, int);
	if (!next)
		symbol("__openat_2", &next, sizeof(next));
	if (fail_read(directory, path, flags))
		return -1;
	return next(directory, path, flags);
}

int __openat64_2(int directory, const char *path, int flags)
{
	static int (*next)(int, const char *, int);
	if (!next)
		symbol("__openat64_2", &next, sizeof(next));
	if (fail_read(directory, path, flags))
		return -1;
	return next(directory, path, flags);
}

int renameat(int from, const char *source, int to, const char *destination)
{
	static int (*next)(int, const char *, int, const char *);
	if (!next)
		symbol("renameat", &next, sizeof(next));
	initialize();
	int result = next(from, source, to, destination);

	published(result, to, destination);
	if (!result && !strcmp(source, "compact.hogg") && private_fd(from)) {
		compact_renamed = 1;
		record("compacted-archive-renamed\n");
		if (!strcmp(mode(), "compact-kill"))
			_exit(86);
	}
	return result;
}

int renameat2(int from, const char *source, int to, const char *destination,
	unsigned int flags)
{
	static int (*next)(int, const char *, int, const char *, unsigned int);
	if (!next)
		symbol("renameat2", &next, sizeof(next));
	int result = next(from, source, to, destination, flags);
	published(result, to, destination);
	return result;
}

static int archive_fd(int descriptor)
{
	const char *path = getenv("LAUNCHER_FAULT_ARCHIVE");
	struct stat current, selected;

	return path && !fstat(descriptor, &current) &&
		!stat(path, &selected) && S_ISREG(current.st_mode) &&
		current.st_dev == selected.st_dev &&
		current.st_ino == selected.st_ino;
}

static int no_space(int descriptor)
{
	initialize();
	if (journal_published && !strcmp(mode(), "low-space") &&
		archive_fd(descriptor)) {
		record("failed-archive-write\n");
		errno = ENOSPC;
		return 1;
	}
	return 0;
}

ssize_t pwrite(int descriptor, const void *bytes, size_t size, off_t offset)
{
	static ssize_t (*next)(int, const void *, size_t, off_t);
	if (!next)
		symbol("pwrite", &next, sizeof(next));
	return no_space(descriptor) ? -1
				    : next(descriptor, bytes, size, offset);
}

ssize_t pwrite64(int descriptor, const void *bytes, size_t size, off64_t offset)
{
	static ssize_t (*next)(int, const void *, size_t, off64_t);
	if (!next)
		symbol("pwrite64", &next, sizeof(next));
	return no_space(descriptor) ? -1
				    : next(descriptor, bytes, size, offset);
}

int fsync(int descriptor)
{
	static int (*next)(int);
	if (!next)
		symbol("fsync", &next, sizeof(next));
	initialize();
	if (compact_renamed && !compact_failed &&
		(!strcmp(mode(), "compact-flush") ||
		 !strcmp(mode(), "compact-corrupt"))) {
		compact_failed = 1;
		if (!strcmp(mode(), "compact-corrupt")) {
			const char *path = getenv("LAUNCHER_FAULT_ARCHIVE");
			int file = path ? open(path, O_WRONLY | O_CLOEXEC) : -1;

			if (file >= 0) {
				ssize_t amount = write(file, "broken", 6);

				(void)amount;
				close(file);
			}
			record("corrupted-compacted-canonical\n");
		}
		record("failed-compaction-flush\n");
		errno = EIO;
		return -1;
	}
	if (armed && !strcmp(mode(), "fsync-state") &&
		private_fd(descriptor)) {
		record("failed-private-fsync\n");
		errno = EIO;
		return -1;
	}
	int result = next(descriptor);

	if (!result && journal_published && archive_fd(descriptor) &&
		!strcmp(mode(), "entry-kill")) {
		record("flushed-archive-entry\n");
		_exit(86);
	}
	return result;
}

int unlinkat(int directory, const char *path, int flags)
{
	static int (*next)(int, const char *, int);
	if (!next)
		symbol("unlinkat", &next, sizeof(next));
	int result = next(directory, path, flags);

	if (!result && armed && !strcmp(mode(), "cleanup-kill") &&
		flags == AT_REMOVEDIR && !strcmp(path, "transaction") &&
		private_fd(directory)) {
		record("cleanup-removed-transaction\n");
		_exit(86);
	}
	return result;
}
