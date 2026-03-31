/*
 * Copyright 2013, Tan Chee Eng (@tan-ce)
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/**
 * pts.cpp
 *
 * Manages the pseudo-terminal driver on Linux/Android and provides some
 * helper functions to handle raw input mode and terminal window resizing
 */

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "pts.hpp"

// Helper function: ensures all data is written
static int write_blocking(int fd, const char* buf, size_t bufsz) {
    ssize_t ret;
    size_t written = 0;

    do {
        ret = write(fd, buf + written, bufsz - written);
        if (ret == -1) return -1;
        written += static_cast<size_t>(ret);
    } while (written < bufsz);

    return 0;
}

// Pump data from input FD to output FD
static void pump_ex(int input, int output, bool close_output) {
    char buf[4096];
    ssize_t len;

    while ((len = read(input, buf, sizeof(buf))) > 0) {
        if (write_blocking(output, buf, static_cast<size_t>(len)) == -1) break;
    }
    close(input);
    if (close_output) close(output);
}

// Pump data from input FD to output FD, will close output when done
static void pump(int input, int output) {
    pump_ex(input, output, true);
}

struct PumpArgs {
    int input;
    int output;
};

static void* pump_thread(void* data) {
    auto* args = static_cast<PumpArgs*>(data);
    pump(args->input, args->output);
    delete args;
    return nullptr;
}

static void pump_async(int input, int output) {
    pthread_t writer;
    auto* args = new PumpArgs{input, output};
    pthread_create(&writer, nullptr, pump_thread, args);
}

int pts_open(char* slave_name, size_t slave_name_size) {
    // Open master ptmx device
    int fdm = open("/dev/ptmx", O_RDWR);
    if (fdm == -1) return -1;

    // Use std::vector instead of VLA (Variable Length Array)
    // VLA is not supported in C++17
    std::vector<char> sn_tmp(slave_name_size);

    // Get the slave name
    if (ptsname_r(fdm, sn_tmp.data(), slave_name_size) != 0) {
        close(fdm);
        return -2;
    }

    if (strlcpy(slave_name, sn_tmp.data(), slave_name_size) >= slave_name_size) {
        close(fdm);
        return -1;
    }

    // Grant, then unlock
    if (grantpt(fdm) == -1) {
        close(fdm);
        return -1;
    }
    if (unlockpt(fdm) == -1) {
        close(fdm);
        return -1;
    }

    return fdm;
}

// Stores the previous termios of stdin
static struct termios old_stdin;
static bool stdin_is_raw = false;

int set_stdin_raw() {
    struct termios new_termios;

    // Save the current stdin termios
    if (tcgetattr(STDIN_FILENO, &old_stdin) < 0) {
        return -1;
    }

    // Start from the current settings
    new_termios = old_stdin;

    // Make the terminal like an SSH or telnet client
    new_termios.c_iflag |= IGNPAR;
    new_termios.c_iflag &= static_cast<tcflag_t>(~(ISTRIP | INLCR | IGNCR | ICRNL | IXON | IXANY | IXOFF));
    new_termios.c_lflag &= static_cast<tcflag_t>(~(ISIG | ICANON | ECHO | ECHOE | ECHOK | ECHONL));
    new_termios.c_oflag &= static_cast<tcflag_t>(~OPOST);
    new_termios.c_cc[VMIN] = 1;
    new_termios.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &new_termios) < 0) {
        return -1;
    }

    stdin_is_raw = true;
    return 0;
}

int restore_stdin() {
    if (!stdin_is_raw) return 0;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &old_stdin) < 0) {
        return -1;
    }

    stdin_is_raw = false;
    return 0;
}

// Flag indicating whether the sigwinch watcher should terminate
static volatile bool closing_time = false;

struct WatchArgs {
    int master;
    int slave;
};

// Thread process: wait for SIGWINCH to be received, then update terminal size
static void* watch_sigwinch(void* data) {
    sigset_t winch;
    int sig;
    auto* args = static_cast<WatchArgs*>(data);
    int master = args->master;
    int slave = args->slave;

    sigemptyset(&winch);
    sigaddset(&winch, SIGWINCH);

    do {
        // Wait for SIGWINCH
        sigwait(&winch, &sig);

        if (closing_time) break;

        // Get the new terminal size
        struct winsize w;
        if (ioctl(master, TIOCGWINSZ, &w) == -1) {
            continue;
        }

        // Set the new terminal size
        ioctl(slave, TIOCSWINSZ, &w);

    } while (true);

    delete args;
    return nullptr;
}

int watch_sigwinch_async(int master, int slave) {
    pthread_t watcher;
    auto* args = new WatchArgs{master, slave};

    // Block SIGWINCH so sigwait can later receive it
    sigset_t winch;
    sigemptyset(&winch);
    sigaddset(&winch, SIGWINCH);
    if (sigprocmask(SIG_BLOCK, &winch, nullptr) == -1) {
        delete args;
        return -1;
    }

    // Initialize and start the thread
    closing_time = false;
    int ret = pthread_create(&watcher, nullptr, &watch_sigwinch, args);
    if (ret != 0) {
        delete args;
        errno = ret;
        return -1;
    }

    // Set the initial terminal size
    raise(SIGWINCH);
    return 0;
}

void watch_sigwinch_cleanup() {
    closing_time = true;
    raise(SIGWINCH);
}

void pump_stdin_async(int outfd) {
    // Put stdin into raw mode
    set_stdin_raw();

    // Pump data from stdin to the PTY
    pump_async(STDIN_FILENO, outfd);
}

void pump_stdout_blocking(int infd) {
    // Pump data from stdout to PTY
    pump_ex(infd, STDOUT_FILENO, false /* Don't close output when done */);

    // Cleanup
    restore_stdin();
    watch_sigwinch_cleanup();
}
