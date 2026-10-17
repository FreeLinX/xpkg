/* lock.c - one xpkg at a time, and only root changes the system. */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/file.h>
#include "xpkg.h"

static int lock_fd = -1;

int xpkg_lock(void) {
    if (lock_fd >= 0) return 0;
    if (xpkg_mkdir_p(xpkg_db_dir(), 0755) != 0) {
        xpkg_err("cannot create %s: %s", xpkg_db_dir(), strerror(errno));
        return -1;
    }
    char path[XPKG_MAX_PATH];
    snprintf(path, sizeof(path), "%s/lock", xpkg_db_dir());
    lock_fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0) {
        xpkg_err("cannot open %s: %s", path, strerror(errno));
        return -1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
        if (errno != EWOULDBLOCK) {
            xpkg_err("cannot lock %s: %s", path, strerror(errno));
            return -1;
        }
        fprintf(stderr, "xpkg: waiting for another xpkg to finish...\n");
        if (flock(lock_fd, LOCK_EX) != 0) {
            xpkg_err("cannot lock %s: %s", path, strerror(errno));
            return -1;
        }
    }
    return 0;
}

void xpkg_unlock(void) {
    if (lock_fd >= 0) {
        flock(lock_fd, LOCK_UN);
        close(lock_fd);
        lock_fd = -1;
    }
}

int xpkg_require_root(void) {
    if (xpkg_root()[0] || xpkg_opts.dry_run) return 0; /* alternate root / plan only */
    if (geteuid() != 0) {
        xpkg_err("this command changes the system: run it as root (doas xpkg ...)");
        return -1;
    }
    return 0;
}
