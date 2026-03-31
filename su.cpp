/*
** Copyright 2010, Adam Shanks (@ChainsDD)
** Copyright 2008, Zinx Verituse (@zinxv)
** Copyright 2017-2018, The LineageOS Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <getopt.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cutils/android_filesystem_config.h>
#include <cutils/properties.h>
#include <log/log.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "binder/pm-wrapper.hpp"
#include "su.hpp"
#include "utils.hpp"

namespace {

extern bool is_daemon;
extern uid_t daemon_from_uid;
extern pid_t daemon_from_pid;

}  // namespace

int fork_zero_fucks() {
    pid_t pid = fork();
    if (pid) {
        int status;
        waitpid(pid, &status, 0);
        return pid;
    } else {
        if ((pid = fork())) exit(0);
        return 0;
    }
}

static bool from_init(su_initiator& from, int argc, char* argv[]) {
    char path[PATH_MAX], exe[PATH_MAX];
    std::array<char, 4096> args{};
    char* argv0;
    char* argv_rest;
    int fd;
    ssize_t len;
    int i;
    int err;

    from.uid = getuid();
    from.pid = getppid();

    if (is_daemon) {
        from.uid = daemon_from_uid;
        from.pid = daemon_from_pid;
    }

    // Get the command line
    snprintf(path, sizeof(path), "/proc/%d/cmdline", from.pid);
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        PLOGE("Opening command line");
        return false;
    }
    len = read(fd, args.data(), args.size() - 1);
    err = errno;
    close(fd);
    if (len < 0) {
        PLOGEV("Reading command line", err);
        return false;
    }

    argv0 = args.data();
    argv_rest = nullptr;
    for (i = 0; i < len; i++) {
        if (args[i] == '\0') {
            if (!argv_rest) {
                argv_rest = &args[i + 1];
            } else {
                args[i] = ' ';
            }
        }
    }
    args[len] = '\0';

    if (argv_rest) {
        from.args = argv_rest;
    } else {
        from.args.clear();
    }

    // If this isn't app_process, use the real path instead of argv[0]
    snprintf(path, sizeof(path), "/proc/%d/exe", from.pid);
    len = readlink(path, exe, sizeof(exe));
    if (len < 0) {
        PLOGE("Getting exe path");
        return false;
    }
    exe[len] = '\0';
    if (strcmp(exe, "/system/bin/app_process") != 0) {
        argv0 = exe;
    }

    from.bin = argv0;

    struct passwd* pw = getpwuid(from.uid);
    if (pw && pw->pw_name) {
        from.name = pw->pw_name;
    }

    return true;
}

static void populate_environment(const su_context& ctx) {
    if (ctx.to.keepenv) return;

    struct passwd* pw = getpwuid(ctx.to.uid);
    if (pw) {
        setenv("HOME", pw->pw_dir, 1);
        if (ctx.to.shell) {
            setenv("SHELL", ctx.to.shell->c_str(), 1);
        } else {
            setenv("SHELL", DEFAULT_SHELL, 1);
        }
        if (ctx.to.login || ctx.to.uid) {
            setenv("USER", pw->pw_name, 1);
            setenv("LOGNAME", pw->pw_name, 1);
        }
    }
}

void set_identity(uid_t uid) {
    // Set effective uid back to root
    if (seteuid(0)) {
        PLOGE("seteuid (root)");
        exit(EXIT_FAILURE);
    }
    if (setresgid(uid, uid, uid)) {
        PLOGE("setresgid (%u)", uid);
        exit(EXIT_FAILURE);
    }
    if (setresuid(uid, uid, uid)) {
        PLOGE("setresuid (%u)", uid);
        exit(EXIT_FAILURE);
    }
}

[[noreturn]] static void usage(int status) {
    FILE* stream = (status == EXIT_SUCCESS) ? stdout : stderr;

    fprintf(stream,
            "Usage: su [options] [--] [-] [LOGIN] [--] [args...]\n\n"
            "Options:\n"
            "  --daemon                      start the su daemon agent\n"
            "  -c, --command COMMAND         pass COMMAND to the invoked shell\n"
            "  -h, --help                    display this help message and exit\n"
            "  -, -l, --login                pretend the shell to be a login shell\n"
            "  -m, -p,\n"
            "  --preserve-environment        do not change environment variables\n"
            "  -s, --shell SHELL             use SHELL instead of the default " DEFAULT_SHELL
            "\n"
            "  -v, --version                 display version number and exit\n"
            "  -V                            display version code and exit,\n"
            "                                this is used almost exclusively by Superuser.apk\n");
    exit(status);
}

[[noreturn]] static void deny(su_context& ctx) {
    std::string_view cmd = get_command(ctx.to);
    ALOGW("request rejected (%u->%u %s)", ctx.from.uid, ctx.to.uid, cmd.data());
    fprintf(stderr, "%s\n", std::strerror(EACCES));
    exit(EXIT_FAILURE);
}

[[noreturn]] static void allow(su_context& ctx, const std::string& package_name) {
    std::string arg0;
    int argc, err;

    umask(ctx.umask);

    std::string binary;
    argc = ctx.to.optind;
    if (ctx.to.command) {
        binary = ctx.to.shell.value_or(DEFAULT_SHELL);
        ctx.to.argv.insert(ctx.to.argv.begin() + argc, *ctx.to.command);
        ctx.to.argv.insert(ctx.to.argv.begin() + argc, "-c");
        argc -= 2;
    } else if (ctx.to.shell) {
        binary = *ctx.to.shell;
    } else {
        if (static_cast<size_t>(argc) < ctx.to.argv.size() && !ctx.to.argv[argc].empty()) {
            binary = ctx.to.argv[argc++];
        } else {
            binary = DEFAULT_SHELL;
        }
    }

    auto pos = binary.rfind('/');
    std::string base_name = (pos != std::string::npos) ? binary.substr(pos + 1) : binary;
    if (ctx.to.login) {
        arg0 = "-" + base_name;
    } else {
        arg0 = base_name;
    }

    populate_environment(ctx);
    set_identity(ctx.to.uid);

    // Build argv for exec
    std::vector<char*> exec_argv;
    exec_argv.push_back(const_cast<char*>(arg0.c_str()));
    for (size_t i = static_cast<size_t>(argc); i < ctx.to.argv.size(); i++) {
        exec_argv.push_back(const_cast<char*>(ctx.to.argv[i].c_str()));
    }
    exec_argv.push_back(nullptr);

    std::string cmd_str;
    for (size_t i = 1; i < exec_argv.size() - 1; i++) {
        if (i > 1) cmd_str += " ";
        cmd_str += exec_argv[i];
    }

    ALOGD("%u %s executing %u %s using binary %s : %s %s",
          ctx.from.uid, ctx.from.bin.c_str(), ctx.to.uid,
          get_command(ctx.to).data(), binary.c_str(),
          arg0.c_str(), cmd_str.c_str());

    pid_t pid = fork();
    if (!pid) {
        execvp(binary.c_str(), exec_argv.data());
        err = errno;
        PLOGE("exec");
        fprintf(stderr, "Cannot execute %s: %s\n", binary.c_str(), std::strerror(err));
        exit(EXIT_FAILURE);
    } else {
        int status, code;

        ALOGD("Waiting for pid %d.", pid);
        waitpid(pid, &status, 0);
        ALOGD("pid %d returned %d.", pid, status);
        code = WIFSIGNALED(status) ? WTERMSIG(status) + 128 : WEXITSTATUS(status);

        if (!package_name.empty()) {
            appops_finish_op_su(ctx.from.uid, package_name);
        }
        exit(code);
    }
}

static bool access_disabled(const su_initiator& from) {
    char lineage_version[PROPERTY_VALUE_MAX];
    char build_type[PROPERTY_VALUE_MAX];
    int enabled;

    // Only allow su on Lineage builds
    property_get("ro.lineage.version", lineage_version, "");
    if (strcmp(lineage_version, "") == 0) {
        ALOGE("Root access disabled on Non-Lineage builds");
        return true;
    }

    // Only allow su on debuggable builds
    if (!property_get_bool("ro.debuggable", false)) {
        ALOGE("Root access is disabled on non-debug builds");
        return true;
    }

    // Enforce persist.sys.root_access on non-eng builds for apps
    enabled = property_get_int32("persist.sys.root_access", LINEAGE_ROOT_ACCESS_ADB_ONLY);
    property_get("ro.build.type", build_type, "");
    if (strcmp("eng", build_type) != 0 &&
        from.uid != AID_SHELL && from.uid != AID_ROOT &&
        (enabled & LINEAGE_ROOT_ACCESS_APPS_ONLY) != LINEAGE_ROOT_ACCESS_APPS_ONLY) {
        ALOGE("Apps root access is disabled by system setting - "
              "enable it under settings -> developer options");
        return true;
    }

    // Disallow su in a shell if appropriate
    if (from.uid == AID_SHELL &&
        (enabled & LINEAGE_ROOT_ACCESS_ADB_ONLY) != LINEAGE_ROOT_ACCESS_ADB_ONLY) {
        ALOGE("Shell root access is disabled by a system setting - "
              "enable it under settings -> developer options");
        return true;
    }

    return false;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (getuid() != geteuid()) {
        ALOGE("must not be a setuid binary");
        return 1;
    }

    return su_main(argc, argv, true);
}

int su_main(int argc, char* argv[], bool need_client) {
    // Start up in daemon mode if prompted
    if (argc == 2 && strcmp(argv[1], "--daemon") == 0) {
        return run_daemon();
    }

    pid_t ppid = getppid();

    // Sanitize all secure environment variables
    static constexpr std::array<const char*, 27> unsec_vars = {
        "GCONV_PATH",
        "GETCONF_DIR",
        "HOSTALIASES",
        "LD_AUDIT",
        "LD_DEBUG",
        "LD_DEBUG_OUTPUT",
        "LD_DYNAMIC_WEAK",
        "LD_LIBRARY_PATH",
        "LD_ORIGIN_PATH",
        "LD_PRELOAD",
        "LD_PROFILE",
        "LD_SHOW_AUXV",
        "LD_USE_LOAD_BIAS",
        "LOCALDOMAIN",
        "LOCPATH",
        "MALLOC_TRACE",
        "MALLOC_CHECK_",
        "NIS_PATH",
        "NLSPATH",
        "RESOLV_HOST_CONF",
        "RES_OPTIONS",
        "TMPDIR",
        "TZDIR",
        "LD_AOUT_LIBRARY_PATH",
        "LD_AOUT_PRELOAD",
        "IFS",
        nullptr,
    };

    for (const char** cp = unsec_vars.data(); *cp; cp++) {
        unsetenv(*cp);
    }

    ALOGD("su invoked.");

    su_context ctx;
    ctx.to.uid = AID_ROOT;

    static struct option long_opts[] = {
        {"command", required_argument, nullptr, 'c'},
        {"help", no_argument, nullptr, 'h'},
        {"login", no_argument, nullptr, 'l'},
        {"preserve-environment", no_argument, nullptr, 'p'},
        {"shell", required_argument, nullptr, 's'},
        {"version", no_argument, nullptr, 'v'},
        {nullptr, 0, nullptr, 0},
    };

    int c;
    while ((c = getopt_long(argc, argv, "+c:hlmps:Vv", long_opts, nullptr)) != -1) {
        switch (c) {
            case 'c':
                ctx.to.shell = DEFAULT_SHELL;
                ctx.to.command = optarg;
                break;
            case 'h':
                usage(EXIT_SUCCESS);
                break;
            case 'l':
                ctx.to.login = true;
                break;
            case 'm':
            case 'p':
                ctx.to.keepenv = true;
                break;
            case 's':
                ctx.to.shell = optarg;
                break;
            case 'V':
                printf("%d\n", VERSION_CODE);
                exit(EXIT_SUCCESS);
            case 'v':
                printf("%s\n", VERSION);
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "\n");
                usage(2);
        }
    }

    // Store original argv for later
    for (int i = 0; i < argc; i++) {
        ctx.to.argv.push_back(argv[i]);
    }
    ctx.to.optind = optind;

    if (need_client) {
        ALOGD("starting daemon client %d %d", getuid(), geteuid());
        return connect_daemon(argc, argv, ppid);
    }

    if (optind < argc && strcmp(argv[optind], "-") == 0) {
        ctx.to.login = true;
        optind++;
    }

    // Username or uid
    if (optind < argc && strcmp(argv[optind], "--") != 0) {
        struct passwd* pw = getpwnam(argv[optind]);
        if (!pw) {
            char* endptr;
            errno = 0;
            unsigned long uid_val = strtoul(argv[optind], &endptr, 10);
            if (errno || *endptr) {
                ALOGE("Unknown id: %s\n", argv[optind]);
                fprintf(stderr, "Unknown id: %s\n", argv[optind]);
                exit(EXIT_FAILURE);
            }
            ctx.to.uid = static_cast<uid_t>(uid_val);
        } else {
            ctx.to.uid = pw->pw_uid;
            if (pw->pw_name) {
                ctx.to.name = pw->pw_name;
            }
        }
        optind++;
    }
    if (optind < argc && strcmp(argv[optind], "--") == 0) {
        optind++;
    }
    ctx.to.optind = optind;

    if (!from_init(ctx.from, argc, argv)) {
        deny(ctx);
    }

    ALOGE("SU from: %s", ctx.from.name.c_str());

    if (ctx.from.uid == AID_ROOT) {
        ALOGD("Allowing root.");
        allow(ctx, {});
    }

    // Check if superuser is disabled completely
    if (access_disabled(ctx.from)) {
        ALOGD("access_disabled");
        deny(ctx);
    }

    // Autogrant shell at this point
    if (ctx.from.uid == AID_SHELL) {
        ALOGD("Allowing shell.");
        allow(ctx, {});
    }

    auto package_name = resolve_package_name(ctx.from.uid);
    if (package_name) {
        if (appops_start_op_su(ctx.from.uid, *package_name) == 0) {
            ALOGD("Allowing via appops.");
            allow(ctx, *package_name);
        }
    }

    ALOGE("Allow chain exhausted, denying request");
    deny(ctx);
}
