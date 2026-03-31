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

#pragma once

#ifdef LOG_TAG
#undef LOG_TAG
#endif
#define LOG_TAG "su"

#include <limits.h>
#include <sys/types.h>
#include <unistd.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Lineage-specific behavior
enum LineageRootAccess {
    LINEAGE_ROOT_ACCESS_DISABLED = 0,
    LINEAGE_ROOT_ACCESS_APPS_ONLY = 1,
    LINEAGE_ROOT_ACCESS_ADB_ONLY = 2,
    LINEAGE_ROOT_ACCESS_APPS_AND_ADB = 3,
};

constexpr const char* DAEMON_SOCKET_PATH = "/dev/socket/su-daemon/";
constexpr const char* DEFAULT_SHELL = "/system/bin/sh";

#define xstr(a) #a
#define str(a) xstr(a)

#ifndef VERSION_CODE
#define VERSION_CODE 16
#endif
#define VERSION xstr(VERSION_CODE) " cm-su"

constexpr int PROTO_VERSION = 1;

struct su_initiator {
    pid_t pid = -1;
    uid_t uid = 0;
    uid_t user = 0;
    std::string name;
    std::string bin;
    std::string args;
};

struct su_request {
    uid_t uid = 0;
    std::string name;
    bool login = false;
    bool keepenv = false;
    std::optional<std::string> shell;
    std::optional<std::string> command;
    std::vector<std::string> argv;
    int optind = 0;
};

struct su_context {
    su_initiator from;
    su_request to;
    mode_t umask = 022;
    std::string sock_path;
};

enum class policy_t {
    INTERACTIVE = 0,
    DENY = 1,
    ALLOW = 2,
};

void set_identity(uid_t uid);

[[nodiscard]] inline std::string_view get_command(const su_request& to) {
    if (to.command) return *to.command;
    if (to.shell) return *to.shell;
    if (static_cast<size_t>(to.optind) < to.argv.size() && !to.argv[to.optind].empty()) {
        return to.argv[to.optind];
    }
    return DEFAULT_SHELL;
}

int appops_start_op_su(uid_t uid, std::string_view pkg_name);
void appops_finish_op_su(uid_t uid, std::string_view pkg_name);

int run_daemon();
int connect_daemon(int argc, char* argv[], pid_t ppid);
int su_main(int argc, char* argv[], bool need_client);

// Fork that doesn't care about the child process state
int fork_zero_fucks();

#ifndef LOG_NDEBUG
#define LOG_NDEBUG 1
#endif

#include <cerrno>
#include <cstring>
#define PLOGE(fmt, args...) ALOGE(fmt " failed with %d: %s", ##args, errno, std::strerror(errno))
#define PLOGEV(fmt, err, args...) ALOGE(fmt " failed with %d: %s", ##args, err, std::strerror(err))
