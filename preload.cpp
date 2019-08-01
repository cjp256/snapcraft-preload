/* -*- Mode: C; indent-tabs-mode: nil; tab-width: 4 -*-
 *
 * Copyright (C) 2015-2019 Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#define __USE_GNU

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/un.h>
#include <sys/vfs.h>
#include <vector>
#include <unistd.h>

#define PRELOAD_DEBUG
#ifdef PRELOAD_DEBUG
#define debug_print(fmt, ...) \
        do { \
                fprintf(stderr, "%s:%d:%s(): " fmt, __FILE__, \
                        __LINE__, __FUNCTION__, ## __VA_ARGS__); \
        } while (0)
#else
#define debug_print(fmt, ...) \
        do {} while (0)
#endif

static char *
redirect_shm(const char *pathname)
{
    size_t max_len = PATH_MAX + 1;
    const char *from = "/dev/shm/shm-%s";
    const char *to = "/dev/shm/snap.skype.shm-";

    char remainder[max_len];

    if (strlen(pathname) >= max_len) {
        debug_print("path too long, ignoring: %s\n", pathname);
        return NULL;
    }

    int filled = sscanf(pathname, from, remainder);
    if (filled < 1) {
        return NULL;
    }

    char *redirected_path = (char *)malloc(max_len);
    if (!redirected_path) {
        return NULL;
    }

    snprintf(redirected_path, max_len, "%s%s", to, remainder);
    return redirected_path;
}

int mkstemp64(char *path_template)
{
    static int (*real_mkstemp64)(char *) = NULL;
    if (!real_mkstemp64) {
        real_mkstemp64 = (int (*)(char *))dlsym(RTLD_NEXT, "mkstemp64");
    }

    const char *from = "/dev/shm/.org.chromium.Chromium.XXXXXX";
    const char *to = "/dev/shm/snap.skype.xx.Chromium.XXXXXX";

    if (strcmp(path_template, from) == 0) {
        strncpy(path_template, to, strlen(from) + 1);
    }

    debug_print("[pre-call] path=%s\n", path_template);
    int result = real_mkstemp64(path_template);
    debug_print("[post-call] path=%s ret=0x%x\n", path_template, result);
    return result;
}

int open(const char *pathname, int flags, ...)
{
    va_list args;
    va_start(args, flags);

    mode_t mode = 0;

    if (flags & (O_CREAT | O_TMPFILE)) {
        mode = va_arg(args, mode_t);
    }

    va_end(args);

    static int (*real_open)(const char *, int, ...) = NULL;
    if (!real_open) {
        real_open = (int (*)(const char *, int, ...)) dlsym(RTLD_NEXT, "open");
    }

    char *redirected_path = redirect_shm(pathname);
    if (redirected_path == NULL) {
        return real_open(pathname, flags, mode);
    }

    debug_print("[pre-call] redirected_path=%s (from path=%s)\n", redirected_path, pathname);
    int result = real_open(redirected_path, flags, mode);
    debug_print("[post-call] redirected_path=%s ret=0x%x\n", redirected_path, result);

    free(redirected_path);

    return result;
}

int unlink(const char *pathname)
{
    static int (*real_unlink)(const char *) = NULL;
    if (!real_unlink) {
        real_unlink = (int (*)(const char *)) dlsym(RTLD_NEXT, "unlink");
    }

    char *redirected_path = redirect_shm(pathname);
    if (redirected_path == NULL) {
        return real_unlink(pathname);
    }

    debug_print("[pre-call] redirected_path=%s (from path=%s)\n", redirected_path, pathname);
    int result = real_unlink(redirected_path);
    debug_print("[post-call] redirected_path=%s ret=0x%x\n", redirected_path, result);
    return result;
}
