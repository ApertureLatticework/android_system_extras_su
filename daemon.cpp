/*
** Copyright 2010, Adam Shanks (@ChainsDD)
** Copyright 2008, Zinx Verituse (@zinxv)
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

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <log/log.h>

#include <array>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "pts.hpp"
#include "su.hpp"
#include "utils.hpp"

namespace {

bool is_daemon = false;
uid_t daemon_from_uid = 0;
pid_t daemon_from_pid = 0;

// Constants for the atty bitfield
constexpr int ATTY_IN = 1;
constexpr int ATTY_OUT = 2;
constexpr int ATTY_ERR = 4;

// List of signals which cause process termination
constexpr std::array<int, 7> quit_signals = {
    SIGALRM, SIGHUP, SIGPIPE, SIGQUIT, SIGTERM, SIGINT, 0
};

// RAII wrapper for file descriptors
struct FdGuard {
    int fd;
    explicit FdGuard(int f) : fd(f) {}
    ~FdGuard() { if (fd >= 0) close(fd); }
    FdGuard(const FdGuard&) = delete;
    FdGuard& operator=(const FdGuard&) = delete;
};

}  // namespace

/**
 * Receive a file descriptor from a Unix socket.
 * Contributed by @mkasick
 *
 * Returns the file descriptor on success, or -1 if a file
 * descriptor was not actually included in the message
 */
static int recv_fd(int sockfd) {
    char iovbuf;

    struct iovec iov = {
        .iov_base = &iovbuf,
        .iov_len = 1,
    };

    std::array<char, CMSG_SPACE(sizeof(int))> cmsgbuf{};

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsgbuf.data(),
        .msg_controllen = cmsgbuf.size(),
    };

    if (recvmsg(sockfd, &msg, MSG_WAITALL) != 1) {
        goto error;
    }

    // Was a control message actually sent?
    switch (msg.msg_controllen) {
        case 0:
            // No, so the file descriptor was closed and won't be used
            return -1;
        case CMSG_SPACE(sizeof(int)):
            // Yes, grab the file descriptor from it
            break;
        default:
            goto error;
    }

    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);

    if (cmsg == nullptr ||
        cmsg->cmsg_len != CMSG_LEN(sizeof(int)) ||
        cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS) {
        goto error;
    }

    return *static_cast<int*>(static_cast<void*>(CMSG_DATA(cmsg)));

error:
    ALOGE("unable to read fd");
    exit(-1);
}

/**
 * Send a file descriptor through a Unix socket.
 * Contributed by @mkasick
 *
 * fd may be -1, in which case the dummy data is sent,
 * but no control message with the FD is sent.
 */
static void send_fd(int sockfd, int fd) {
    struct iovec iov = {
        .iov_base = const_cast<char*>(""),
        .iov_len = 1,
    };

    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    std::array<char, CMSG_SPACE(sizeof(int))> cmsgbuf{};

    if (fd != -1) {
        // Is the file descriptor actually open?
        if (fcntl(fd, F_GETFD) == -1) {
            if (errno != EBADF) {
                goto error;
            }
            // It's closed, don't send a control message
        } else {
            // It's open, send the file descriptor in a control message
            msg.msg_control = cmsgbuf.data();
            msg.msg_controllen = cmsgbuf.size();

            struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
            if (!cmsg) {
                goto error;
            }

            cmsg->cmsg_len = CMSG_LEN(sizeof(int));
            cmsg->cmsg_level = SOL_SOCKET;
            cmsg->cmsg_type = SCM_RIGHTS;

            *static_cast<int*>(static_cast<void*>(CMSG_DATA(cmsg))) = fd;
        }
    }

    if (sendmsg(sockfd, &msg, 0) != 1) {
        goto error;
    }
    return;

error:
    PLOGE("unable to send fd");
    exit(-1);
}

static int read_int(int fd) {
    int val;
    ssize_t len = read(fd, &val, sizeof(int));
    if (len != sizeof(int)) {
        ALOGE("unable to read int: %zd", len);
        exit(-1);
    }
    return val;
}

static void write_int(int fd, int val) {
    ssize_t written = write(fd, &val, sizeof(int));
    if (written != sizeof(int)) {
        PLOGE("unable to write int");
        exit(-1);
    }
}

static std::string read_string(int fd) {
    int len = read_int(fd);
    if (len > PATH_MAX || len < 0) {
        ALOGE("invalid string length %d", len);
        exit(-1);
    }
    std::string val(len + 1, '\0');
    ssize_t amount = read(fd, val.data(), len);
    if (amount != len) {
        ALOGE("unable to read string");
        exit(-1);
    }
    return val;
}

static void write_string(int fd, const std::string& val) {
    int len = static_cast<int>(val.size());
    write_int(fd, len);
    ssize_t written = write(fd, val.c_str(), len);
    if (written != len) {
        PLOGE("unable to write string");
        exit(-1);
    }
}

static int run_daemon_child(int infd, int outfd, int errfd, int argc, char** argv) {
    if (dup2(outfd, STDOUT_FILENO) == -1) {
        PLOGE("dup2 child outfd");
        exit(-1);
    }

    if (dup2(errfd, STDERR_FILENO) == -1) {
        PLOGE("dup2 child errfd");
        exit(-1);
    }

    if (dup2(infd, STDIN_FILENO) == -1) {
        PLOGE("dup2 child infd");
        exit(-1);
    }

    close(infd);
    close(outfd);
    close(errfd);

    return su_main(argc, argv, false);
}

static int daemon_accept(int fd) {
    is_daemon = true;
    pid_t pid = read_int(fd);
    int child_result;
    ALOGD("remote pid: %d", pid);
    std::string pts_slave = read_string(fd);
    ALOGD("remote pts_slave: %s", pts_slave.c_str());
    daemon_from_pid = read_int(fd);
    ALOGV("remote req pid: %d", daemon_from_pid);

    struct ucred credentials;
    socklen_t ucred_length = sizeof(struct ucred);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &ucred_length)) {
        ALOGE("could not obtain credentials from unix domain socket");
        exit(-1);
    }

    daemon_from_uid = credentials.uid;

    // Get the FDs for each of the streams
    int infd = recv_fd(fd);
    int outfd = recv_fd(fd);
    int errfd = recv_fd(fd);

    int argc = read_int(fd);
    if (argc < 0 || argc > 512) {
        ALOGE("unable to allocate args: %d", argc);
        exit(-1);
    }
    ALOGV("remote args: %d", argc);

    // Read arguments
    std::vector<std::string> argv_strings(argc);
    for (int i = 0; i < argc; i++) {
        argv_strings[i] = read_string(fd);
    }

    // Convert to char** for exec
    std::vector<char*> argv(argc + 1, nullptr);
    for (int i = 0; i < argc; i++) {
        argv[i] = const_cast<char*>(argv_strings[i].c_str());
    }

    // Ack
    write_int(fd, 1);

    // Fork the child process
    pid_t child = fork();
    if (child < 0) {
        PLOGE("unable to fork");
        write(fd, &child, sizeof(int));
        close(fd);
        return child;
    }

    if (child != 0) {
        // In parent, wait for the child to exit
        int code, status;

        ALOGD("waiting for child exit");
        if (waitpid(child, &status, 0) > 0) {
            code = WEXITSTATUS(status);
        } else {
            code = -1;
        }

        // Check if fd is open
        if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) {
            ALOGD("sending code");
            if (send(fd, &code, sizeof(int), MSG_NOSIGNAL) != sizeof(int)) {
                PLOGE("unable to write exit code");
            }
        }

        close(fd);
        ALOGD("child exited");
        return code;
    }

    // We are in the child now
    close(fd);

    // Become session leader
    if (setsid() == static_cast<pid_t>(-1)) {
        PLOGE("setsid");
    }

    int ptsfd = -1;
    if (!pts_slave.empty()) {
        ptsfd = open(pts_slave.c_str(), O_RDWR);
        if (ptsfd == -1) {
            PLOGE("open(pts_slave) daemon");
            exit(-1);
        }

        struct stat st;
        if (fstat(ptsfd, &st)) {
            PLOGE("failed to stat pts_slave");
            exit(-1);
        }

        if (st.st_uid != credentials.uid) {
            PLOGE("caller doesn't own proposed PTY");
            exit(-1);
        }

        if (!S_ISCHR(st.st_mode)) {
            PLOGE("proposed PTY isn't a chardev");
            exit(-1);
        }

        if (infd < 0) {
            ALOGD("daemon: stdin using PTY");
            infd = ptsfd;
        }
        if (outfd < 0) {
            ALOGD("daemon: stdout using PTY");
            outfd = ptsfd;
        }
        if (errfd < 0) {
            ALOGD("daemon: stderr using PTY");
            errfd = ptsfd;
        }
    }

    child_result = run_daemon_child(infd, outfd, errfd, argc, argv.data());
    return child_result;
}

int run_daemon() {
    if (getuid() != 0 || getgid() != 0) {
        PLOGE("daemon requires root. uid/gid not root");
        return -1;
    }

    int fd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (fd < 0) {
        PLOGE("socket");
        return -1;
    }
    if (fcntl(fd, F_SETFD, FD_CLOEXEC)) {
        PLOGE("fcntl FD_CLOEXEC");
        goto err;
    }

    struct sockaddr_un sun{};
    sun.sun_family = AF_LOCAL;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/su-daemon", DAEMON_SOCKET_PATH);

    unlink(sun.sun_path);
    unlink(DAEMON_SOCKET_PATH);

    mode_t previous_umask = umask(027);
    mkdir(DAEMON_SOCKET_PATH, 0711);

    if (bind(fd, reinterpret_cast<struct sockaddr*>(&sun), sizeof(sun)) < 0) {
        PLOGE("daemon bind");
        goto err;
    }

    chmod(DAEMON_SOCKET_PATH, 0711);
    chmod(sun.sun_path, 0666);

    umask(previous_umask);

    if (listen(fd, 10) < 0) {
        PLOGE("daemon listen");
        goto err;
    }

    int client;
    while ((client = accept(fd, nullptr, nullptr)) > 0) {
        if (fork_zero_fucks() == 0) {
            close(fd);
            return daemon_accept(client);
        } else {
            close(client);
        }
    }

    ALOGE("daemon exiting");
err:
    close(fd);
    return -1;
}

static void sighandler(int sig) {
    restore_stdin();

    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    struct sigaction act{};
    act.sa_handler = SIG_DFL;
    for (int sig_val : quit_signals) {
        if (sig_val == 0) break;
        sigaction(sig_val, &act, nullptr);
    }
}

static void setup_sighandlers() {
    struct sigaction act{};
    act.sa_handler = &sighandler;
    for (int sig_val : quit_signals) {
        if (sig_val == 0) break;
        sigaction(sig_val, &act, nullptr);
    }
}

int connect_daemon(int argc, char* argv[], pid_t ppid) {
    int ptmx = -1;
    std::array<char, PATH_MAX> pts_slave{};

    struct sockaddr_un sun{};

    int socketfd = socket(AF_LOCAL, SOCK_STREAM, 0);
    if (socketfd < 0) {
        PLOGE("socket");
        exit(-1);
    }
    if (fcntl(socketfd, F_SETFD, FD_CLOEXEC)) {
        PLOGE("fcntl FD_CLOEXEC");
        exit(-1);
    }

    sun.sun_family = AF_LOCAL;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/su-daemon", DAEMON_SOCKET_PATH);

    if (connect(socketfd, reinterpret_cast<struct sockaddr*>(&sun), sizeof(sun)) != 0) {
        PLOGE("connect");
        exit(-1);
    }

    ALOGV("connecting client %d", getpid());

    // Determine which streams are attached to a TTY
    int atty = 0;
    if (isatty(STDIN_FILENO)) atty |= ATTY_IN;
    if (isatty(STDOUT_FILENO)) atty |= ATTY_OUT;
    if (isatty(STDERR_FILENO)) atty |= ATTY_ERR;

    if (atty) {
        ptmx = pts_open(pts_slave.data(), pts_slave.size());
        if (ptmx < 0) {
            PLOGE("pts_open");
            exit(-1);
        }
    } else {
        pts_slave[0] = '\0';
    }

    // Send PID
    write_int(socketfd, getpid());
    // Send slave path
    write_string(socketfd, pts_slave.data());
    // Send parent PID
    write_int(socketfd, static_cast<int>(ppid));

    // Send stdin
    if (atty & ATTY_IN) {
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDIN_FILENO);
    }

    // Send stdout
    if (atty & ATTY_OUT) {
        watch_sigwinch_async(STDOUT_FILENO, ptmx);
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDOUT_FILENO);
    }

    // Send stderr
    if (atty & ATTY_ERR) {
        send_fd(socketfd, -1);
    } else {
        send_fd(socketfd, STDERR_FILENO);
    }

    // Number of command line arguments
    write_int(socketfd, argc);

    // Command line arguments
    for (int i = 0; i < argc; i++) {
        write_string(socketfd, argv[i]);
    }

    // Wait for acknowledgement from daemon
    read_int(socketfd);

    if (atty & ATTY_IN) {
        setup_sighandlers();
        pump_stdin_async(ptmx);
    }
    if (atty & ATTY_OUT) {
        pump_stdout_blocking(ptmx);
    }

    // Get the exit code
    int code = read_int(socketfd);
    close(socketfd);
    ALOGD("client exited %d", code);

    return code;
}

// Fork that doesn't care about the child process state
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
