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
 * pts.hpp
 *
 * Manages the pseudo-terminal driver on Linux/Android and provides some
 * helper functions to handle raw input mode and terminal window resizing
 */

#pragma once

#include <cstddef>

/**
 * Opens a pts device and returns the name of the slave tty device.
 *
 * @param slave_name Buffer to store the name of the slave device
 * @param slave_name_size Size of the buffer
 * @return On failure, either -2 or -1 (errno set). On success, the file
 *         descriptor of the master device.
 */
[[nodiscard]] int pts_open(char* slave_name, size_t slave_name_size);

/**
 * Changes stdin to raw unbuffered mode, disables echo,
 * auto carriage return, etc.
 *
 * @return On failure -1 and errno is set, on success 0
 */
[[nodiscard]] int set_stdin_raw();

/**
 * Restore termios on stdin to the state it was before
 * set_stdin_raw() was called. If set_stdin_raw() was
 * never called, does nothing and doesn't return an error.
 *
 * This function is async-safe.
 *
 * @return On failure -1 and errno is set, on success 0
 */
int restore_stdin();

/**
 * After calling this function, if the application receives
 * SIGWINCH, the terminal window size will be read from
 * "master" and set on "slave".
 *
 * NOTE: This function blocks SIGWINCH and spawns a thread.
 *
 * @param master A file descriptor of the TTY window size to follow
 * @param slave A file descriptor of the TTY window size which is
 *              to be set on SIGWINCH
 * @return On failure -1 and errno will be set. On success, 0
 */
[[nodiscard]] int watch_sigwinch_async(int master, int slave);

/**
 * Cause the SIGWINCH watcher thread to terminate
 */
void watch_sigwinch_cleanup();

/**
 * Forward data from STDIN to the given FD in a separate thread
 */
void pump_stdin_async(int outfd);

/**
 * Forward data from the FD to STDOUT.
 * Returns when the remote end of the FD closes.
 *
 * Before returning, restores stdin settings.
 */
void pump_stdout_blocking(int infd);
