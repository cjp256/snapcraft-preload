/* -*- Mode: C; indent-tabs-mode: nil; tab-width: 4 -*-
 *
 * Copyright (C) 2015-2017 Canonical, Ltd.
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
#include <functional>
#include <iostream>
#include <sstream>
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

#ifndef SNAPCRAFT_LIBNAME_DEF
#define SNAPCRAFT_LIBNAME_DEF "libsnapcraft-preload.so"
#endif

#define LITERAL_STRLEN(s) (sizeof (s) - 1)

namespace
{
const std::string SNAPCRAFT_LIBNAME = SNAPCRAFT_LIBNAME_DEF;
const std::string SNAPCRAFT_PRELOAD = "SNAPCRAFT_PRELOAD";
const std::string LD_PRELOAD = "LD_PRELOAD";
const std::string LD_LINUX = "/lib/ld-linux.so.2";

const std::string DEFAULT_DEVSHM = "/dev/shm/";
const std::string SKYPE_DFLT_SHM_TMPFILE = "/dev/shm/.org.chromium.Chromium.";
const std::string SKYPE_SNAP_SHM_TMPFILE = "/dev/shm/snap.skype.xx.Chromium.";

std::string saved_snapcraft_preload;

std::string saved_snap_name;
std::string saved_snap_revision;
std::string saved_snap_devshm;

std::vector<std::string> saved_ld_preloads;

int (*_access) (const char *, int) = NULL;

template <typename dirent_t>
using filter_function_t = int (*)(const dirent_t *);
template <typename dirent_t>
using compar_function_t = int (*)(const dirent_t **, const dirent_t **);

using socket_action_t = int (*) (int, const struct sockaddr *, socklen_t);
using execve_t = int (*) (const char *, char *const[], char *const[]);

inline std::string
getenv_string(const std::string& varname)
{
    char *envvar = secure_getenv (varname.c_str());
    return envvar ? envvar : "";
}

inline bool
str_starts_with(const std::string& str, std::string const& prefix)
{
    return str.compare (0, prefix.size (), prefix) == 0;
}

inline bool
str_ends_with(const std::string& str, std::string const& sufix)
{
    if (str.size () < sufix.size ())
        return false;

    return str.compare (str.size() - sufix.size (), sufix.size (), sufix) == 0;
}

struct Initializer { Initializer (); };
static Initializer initalizer;

Initializer::Initializer()
{
    _access = (decltype(_access)) dlsym (RTLD_NEXT, "access");

    // We need to save LD_PRELOAD and SNAPCRAFT_PRELOAD in case we need to
    // propagate the values to an exec'd program.
    std::string const& ld_preload = getenv_string (LD_PRELOAD);
    if (ld_preload.empty ()) {
        //return;
    }

    saved_snapcraft_preload = getenv_string (SNAPCRAFT_PRELOAD);
    if (saved_snapcraft_preload.empty ()) {
        //return;
    }

    saved_snap_name = getenv_string ("SNAP_NAME");
    saved_snap_revision = getenv_string ("SNAP_REVISION");
    saved_snap_devshm = DEFAULT_DEVSHM + "snap." + getenv_string ("SNAP_NAME");

    // Pull out each absolute-pathed libsnapcraft-preload.so we find.  Better to
    // accidentally include some other libsnapcraft-preload than not propagate
    // ourselves.
    std::string p;
    std::istringstream ss (ld_preload);
    while (std::getline (ss, p, ':')) {
        if (str_ends_with (p, "/" SNAPCRAFT_LIBNAME_DEF)) {
            saved_ld_preloads.push_back (p);
	        //std::cerr << "snapcraft-preload: Initializer saving off " << p << "\n";
        }
    }
}

inline void
string_length_sanitize(std::string& path)
{
    if (path.size () >= PATH_MAX) {
        std::cerr << "snapcraft-preload: path '" << path << "' exceeds PATH_MAX size (" << PATH_MAX << ") and it will be cut.\n"
                  << "Expect undefined behavior";
        path.resize (PATH_MAX);
    }
}

std::string
redirect_path_full (std::string const& pathname, bool check_parent, bool only_if_absolute)
{
    //std::cerr << "snapcraft-preload: [redirect_path_full] " << pathname << "\n";

    if (pathname.empty ()) {
        return pathname;
    }

    const std::string& preload_dir = saved_snapcraft_preload;
    if (preload_dir.empty()) {
        return pathname;
    }

    if (only_if_absolute && pathname[0] != '/') {
        return pathname;
    }

    // Some apps want to open shared memory in random locations. Here we will confine it to the
    // snaps allowed path.
    if (str_starts_with (pathname, DEFAULT_DEVSHM) && !str_starts_with (pathname, saved_snap_devshm) && (pathname.length() > DEFAULT_DEVSHM.length())) {
        std::string redirected_pathname;

        if (str_starts_with (pathname, SKYPE_DFLT_SHM_TMPFILE )) {
            redirected_pathname = pathname;
            redirected_pathname.replace(0, SKYPE_SNAP_SHM_TMPFILE.length(), SKYPE_SNAP_SHM_TMPFILE);
        } else {
            std::string new_pathname = pathname.substr(DEFAULT_DEVSHM.size());
            redirected_pathname = saved_snap_devshm + '.' + new_pathname;
            string_length_sanitize (redirected_pathname);
        }

        return redirected_pathname;
    }

    return pathname;
}

inline std::string
redirect_path (std::string const& pathname)
{
    return redirect_path_full (pathname, /*check_parent*/ false, /*only_if_absolute*/ false);
}

inline std::string
redirect_path_target (std::string const& pathname)
{
    return redirect_path_full (pathname, /*check_parent*/ true, /*only_if_absolute*/ false);
}

inline std::string
redirect_path_if_absolute (std::string const& pathname)
{
    return redirect_path_full (pathname, /*check_parent*/ false, /*only_if_absolute*/ true);
}

// helper class
template<typename R, template<typename...> class Params, typename... Args, std::size_t... I>
inline R call_helper(std::function<R(Args...)> const&func, Params<Args...> const&params, std::index_sequence<I...>)
{
    return func (std::get<I>(params)...);
}

template<typename R, template<typename...> class Params, typename... Args>
inline R call_with_tuple_args(std::function<R(Args...)> const&func, Params<Args...> const&params)
{
    return call_helper (func, params, std::index_sequence_for<Args...>{});
}

struct NORMAL_REDIRECT {
    static inline std::string redirect (const std::string& path) { return redirect_path (path); }
};

struct ABSOLUTE_REDIRECT {
    static inline std::string redirect (const std::string& path) { return redirect_path_if_absolute (path); }
};

struct TARGET_REDIRECT {
    static inline std::string redirect (const std::string& path) { return redirect_path_target (path); }
};

template<typename R, const char *FUNC_NAME, typename REDIRECT_PATH_TYPE, size_t PATH_IDX, typename... Ts>
inline R
redirect_n(Ts... as)
{
    std::tuple<Ts...> tpl(as...);
    const char *path = std::get<PATH_IDX>(tpl);
    static std::function<R(Ts...)> func (reinterpret_cast<R(*)(Ts...)> (dlsym (RTLD_NEXT, FUNC_NAME)));

    if (path != NULL) {
        std::string const& new_path = REDIRECT_PATH_TYPE::redirect (path);
        std::get<PATH_IDX>(tpl) = new_path.c_str ();

        //std::cerr << "snapcraft-preload: [pre-call] " << FUNC_NAME << " " << std::get<PATH_IDX>(tpl) << "\n";
        R result = call_with_tuple_args (func, tpl);
        //std::cerr << "snapcraft-preload: [post-call] " << FUNC_NAME << " " << std::get<PATH_IDX>(tpl) << " return value: " << result << "\n";

        std::get<PATH_IDX>(tpl) = path;
        return result;
    }

    return func (std::forward<Ts>(as)...);
}

template<typename R, const char *FUNC_NAME, typename REDIRECT_PATH_TYPE, size_t PATH_IDX, typename... Ts>
inline R
redirect_n_nonconst_path(Ts... as)
{
    std::tuple<Ts...> tpl(as...);
    char *path = std::get<PATH_IDX>(tpl);
    static std::function<R(Ts...)> func (reinterpret_cast<R(*)(Ts...)> (dlsym (RTLD_NEXT, FUNC_NAME)));

    if (path != NULL) {
        std::string const& new_path = REDIRECT_PATH_TYPE::redirect (path);
        char *buf = NULL;

        if (new_path.length() <= strlen(path)) {
            buf = strdup(new_path.c_str());
            std::get<PATH_IDX>(tpl) = buf;
        } else {
            std::cerr << "snapcraft-preload: cannot safely redirect path=" << path << " (path=" << new_path << " is too long)\n";
        }

        //std::cerr << "snapcraft-preload: [pre-call] " << FUNC_NAME << " " << std::get<PATH_IDX>(tpl) << "\n";
        R result = call_with_tuple_args (func, tpl);
        //std::cerr << "snapcraft-preload: [post-call] " << FUNC_NAME << " " << std::get<PATH_IDX>(tpl) << " return value: " << result << "\n";

        if (buf) {
            strncpy(path, buf, strlen(path));
            free(buf);
            std::get<PATH_IDX>(tpl) = path;
        }
        return result;
    }

    return func (std::forward<Ts>(as)...);
}

template<typename R, const char *FUNC_NAME, typename REDIRECT_PATH_TYPE, typename REDIRECT_TARGET_TYPE, typename... Ts>
inline R
redirect_target(const char *path, const char *target, Ts... as)
{
    std::string const& new_target = REDIRECT_PATH_TYPE::redirect (target ? target : "");
    return redirect_n<R, FUNC_NAME, REDIRECT_PATH_TYPE, 0, const char*, const char*, Ts...> (path, new_target.c_str ());
}

struct va_separator {};
template<typename R, const char *FUNC_NAME, typename REDIRECT_PATH_TYPE, size_t PATH_IDX, typename... Ts>
inline R
redirect_open(Ts... as, va_separator, va_list va)
{
    mode_t mode = 0;
    int flags = std::get<PATH_IDX+1>(std::tuple<Ts...>(as...));

    if (flags & (O_CREAT|O_TMPFILE)) {
        mode = va_arg (va, mode_t);
    }

    return redirect_n<R, FUNC_NAME, REDIRECT_PATH_TYPE, PATH_IDX, Ts..., mode_t>(as..., mode);
}

} // unnamed namespace

extern "C"
{
#define ARG(A) , A
#define REDIRECT_NAME(NAME) _ ## NAME ## _preload
#define DECLARE_REDIRECT(NAME) \
constexpr const char REDIRECT_NAME(NAME)[] = #NAME;

#define REDIRECT_1_NONCONST_PATH(RET, NAME, REDIR_TYPE, SIG, ARGS) \
DECLARE_REDIRECT(NAME) \
RET NAME (char *path SIG) { return redirect_n_nonconst_path<RET, REDIRECT_NAME(NAME), REDIR_TYPE, 0>(path ARGS); }

#define REDIRECT_1(RET, NAME, REDIR_TYPE, SIG, ARGS) \
DECLARE_REDIRECT(NAME) \
RET NAME (const char *path SIG) { return redirect_n<RET, REDIRECT_NAME(NAME), REDIR_TYPE, 0>(path ARGS); }

#define REDIRECT_2(RET, NAME, REDIR_TYPE, T1, SIG, ARGS) \
DECLARE_REDIRECT(NAME) \
RET NAME (T1 a1, const char *path SIG) { return redirect_n<RET, REDIRECT_NAME(NAME), REDIR_TYPE, 1>(a1, path ARGS); }

#define REDIRECT_3(RET, NAME, REDIR_TYPE, T1, T2, SIG, ARGS) \
DECLARE_REDIRECT(NAME) \
RET NAME (T1 a1, T2 a2, const char *path SIG) { return redirect_n<RET, REDIRECT_NAME(NAME), REDIR_TYPE, 2>(a1, a2, path ARGS); }

#define REDIRECT_1_1(RET, NAME) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ,)

#define REDIRECT_1_1_NONCONST_PATH(RET, NAME) \
REDIRECT_1_NONCONST_PATH(RET, NAME, NORMAL_REDIRECT, ,)

#define REDIRECT_1_2(RET, NAME, T2) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ARG(T2 a2), ARG(a2))

#define REDIRECT_1_2_AT(RET, NAME, T2) \
REDIRECT_1(RET, NAME, ABSOLUTE_REDIRECT, ARG(T2 a2), ARG(a2))

#define REDIRECT_1_3(RET, NAME, T2, T3) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ARG(T2 a2) ARG(T3 a3), ARG(a2) ARG(a3))

#define REDIRECT_1_4(RET, NAME, T2, T3, T4) \
REDIRECT_1(RET, NAME, NORMAL_REDIRECT, ARG(T2 a2) ARG(T3 a3) ARG(T4 a4), ARG(a2) ARG(a3) ARG(a4))

#define REDIRECT_2_2(RET, NAME, T1) \
REDIRECT_2(RET, NAME, NORMAL_REDIRECT, T1, ,)

#define REDIRECT_2_3(RET, NAME, T1, T3) \
REDIRECT_2(RET, NAME, NORMAL_REDIRECT, T1, ARG(T3 a3), ARG(a3))

#define REDIRECT_2_3_AT(RET, NAME, T1, T3) \
REDIRECT_2(RET, NAME, ABSOLUTE_REDIRECT, T1, ARG(T3 a3), ARG(a3))

#define REDIRECT_2_4_AT(RET, NAME, T1, T3, T4) \
REDIRECT_2(RET, NAME, ABSOLUTE_REDIRECT, T1, ARG(T3 a3) ARG(T4 a4), ARG(a3) ARG(a4))

#define REDIRECT_2_5_AT(RET, NAME, T1, T3, T4, T5) \
REDIRECT_2(RET, NAME, ABSOLUTE_REDIRECT, T1, ARG(T3 a3) ARG(T4 a4) ARG(T5 a5), ARG(a3) ARG(a4) ARG(a5))

#define REDIRECT_3_5(RET, NAME, T1, T2, T4, T5) \
REDIRECT_3(RET, NAME, NORMAL_REDIRECT, T1, T2, ARG(T4 a4) ARG(T5 a5), ARG(a4) ARG(a5))

#define REDIRECT_TARGET(RET, NAME) \
DECLARE_REDIRECT(NAME) \
RET NAME (const char *path, const char *target) { return redirect_target<RET, REDIRECT_NAME(NAME), NORMAL_REDIRECT, TARGET_REDIRECT>(path, target); }

#define REDIRECT_OPEN(NAME) \
DECLARE_REDIRECT(NAME) \
int NAME (const char *path, int flags, ...) { va_list va; va_start(va, flags); int ret = redirect_open<int, REDIRECT_NAME(NAME), NORMAL_REDIRECT, 0, const char *, int>(path, flags, va_separator(), va); va_end(va); return ret; }

#define REDIRECT_OPEN_AT(NAME) \
DECLARE_REDIRECT(NAME) \
int NAME (int dirfp, const char *path, int flags, ...) { va_list va; va_start(va, flags); int ret = redirect_open<int, REDIRECT_NAME(NAME), ABSOLUTE_REDIRECT, 1, int, const char *, int>(dirfp, path, flags, va_separator(), va); va_end(va); return ret; }

REDIRECT_1_2(FILE *, fopen, const char *)
REDIRECT_1_1(int, unlink)
REDIRECT_2_3_AT(int, unlinkat, int, int)
REDIRECT_1_2(int, access, int)
REDIRECT_1_2(int, eaccess, int)
REDIRECT_1_2(int, euidaccess, int)
REDIRECT_2_4_AT(int, faccessat, int, int, int)
REDIRECT_1_2(int, stat, struct stat *)
REDIRECT_1_2(int, stat64, struct stat64 *)
REDIRECT_1_2(int, lstat, struct stat *)
REDIRECT_1_2(int, lstat64, struct stat64 *)
REDIRECT_1_2(int, creat, mode_t)
REDIRECT_1_2(int, creat64, mode_t)
REDIRECT_1_2(int, truncate, off_t)
REDIRECT_2_2(char *, bindtextdomain, const char *)
REDIRECT_2_3(int, xstat, int, struct stat *)
REDIRECT_2_3(int, __xstat, int, struct stat *)
REDIRECT_2_3(int, __xstat64, int, struct stat64 *)
REDIRECT_2_3(int, __lxstat, int, struct stat *)
REDIRECT_2_3(int, __lxstat64, int, struct stat64 *)
REDIRECT_3_5(int, __fxstatat, int, int, struct stat *, int)
REDIRECT_3_5(int, __fxstatat64, int, int, struct stat64 *, int)
REDIRECT_1_2(int, statfs, struct statfs *)
REDIRECT_1_2(int, statfs64, struct statfs64 *)
REDIRECT_1_2(int, statvfs, struct statvfs *)
REDIRECT_1_2(int, statvfs64, struct statvfs64 *)
REDIRECT_1_2(long, pathconf, int)
REDIRECT_1_3(int, mknod, mode_t, dev_t)
REDIRECT_1_1(DIR *, opendir)
REDIRECT_1_2(int, mkdir, mode_t)
REDIRECT_1_1(int, rmdir)
REDIRECT_1_3(int, chown, uid_t, gid_t)
REDIRECT_1_3(int, lchown, uid_t, gid_t)
REDIRECT_1_2(int, chmod, mode_t)
REDIRECT_1_2(int, lchmod, mode_t)
REDIRECT_1_1(int, chdir)
REDIRECT_1_3(ssize_t, readlink, char *, size_t)
REDIRECT_1_2(char *, realpath, char *)
REDIRECT_TARGET(int, link)
REDIRECT_TARGET(int, rename)
REDIRECT_OPEN(open)
REDIRECT_OPEN(open64)
REDIRECT_OPEN_AT(openat)
REDIRECT_OPEN_AT(openat64)
REDIRECT_2_3(int, inotify_add_watch, int, uint32_t)
REDIRECT_1_4(int, scandir, struct dirent ***, filter_function_t<struct dirent>, compar_function_t<struct dirent>);
REDIRECT_1_4(int, scandir64, struct dirent64 ***, filter_function_t<struct dirent64>, compar_function_t<struct dirent64>);
REDIRECT_2_5_AT(int, scandirat, int, struct dirent ***, filter_function_t<struct dirent>, compar_function_t<struct dirent>);
REDIRECT_2_5_AT(int, scandirat64, int, struct dirent64 ***, filter_function_t<struct dirent64>, compar_function_t<struct dirent64>);
REDIRECT_1_1_NONCONST_PATH(int, mkstemp)
REDIRECT_1_1_NONCONST_PATH(int, mkstemp64)

// non-absolute library paths aren't simply relative paths, they need
// a whole lookup algorithm
REDIRECT_1_2_AT(void *, dlopen, int);
}

namespace
{
void
ensure_in_ld_preload (std::string& ld_preload, const std::string& to_be_added)
{
    if (!ld_preload.empty ()) {
        bool found = false;

        std::string p;
        std::istringstream ss (ld_preload.substr (LD_PRELOAD.size () + 1, std::string::npos));
        while (std::getline (ss, p, ':') && !found) {
            if (p == to_be_added) {
                found = true;
            }
        }

        if (!found) {
            ld_preload += ':' + to_be_added;
        }
    } else {
        ld_preload = LD_PRELOAD + '=' + to_be_added;
    }
}

std::vector<std::string>
execve_copy_envp (char *const envp[])
{
    std::string ld_preload;
    std::vector<std::string> new_envp;

    for (unsigned i = 0; envp && envp[i]; ++i) {
        std::string env(envp[i]);
        new_envp.push_back (env);

        if (str_starts_with (env, LD_PRELOAD + '=')) {
            ld_preload = env; // point at last defined LD_PRELOAD index
        }
    }

    for (const std::string& saved_preload : saved_ld_preloads)
        ensure_in_ld_preload (ld_preload, saved_preload);

    if (!saved_ld_preloads.empty ())
        new_envp.push_back (ld_preload);

    if (!saved_snapcraft_preload.empty ()) {
        auto snapcraft_preload = SNAPCRAFT_PRELOAD + '=' + saved_snapcraft_preload;
        new_envp.push_back (snapcraft_preload);
    }

    return new_envp;
}

struct c_vector_holder
{
    c_vector_holder(const std::vector<std::string>& str_vector) {
        for (auto const& str : str_vector) {
            holder_.push_back (str.c_str ());
        }
        holder_.push_back (nullptr);
    }

    operator const char **() { return holder_.data (); }
    operator char* const*() { return (char* const*) holder_.data (); }

    private:
    std::vector<const char*> holder_;
};

int
execve_wrapper (const char *func, const char *path, char *const argv[], char *const envp[])
{
    int i, result;

    static execve_t _execve = (decltype(_execve)) dlsym (RTLD_NEXT, func);

    if (path == NULL) {
        return _execve (path, argv, envp);
    }

    std::string const& new_path = redirect_path (path);

    // Make sure we inject our original preload values, can't trust this
    // program to pass them along in envp for us.
    auto env_copy = execve_copy_envp (envp);
    c_vector_holder new_envp (env_copy);
    result = _execve (new_path.c_str (), argv, new_envp);

    return result;
}

} // anonymous namepsace

extern "C" int
execv (const char *path, char *const argv[])
{
    return execve (path, argv, environ);
}

extern "C" int
execve (const char *path, char *const argv[], char *const envp[])
{
    return execve_wrapper ("execve", path, argv, envp);
}

extern "C" int
__execve (const char *path, char *const argv[], char *const envp[])
{
    return execve_wrapper ("__execve", path, argv, envp);
}
