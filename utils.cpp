/*
** Copyright 2012, The CyanogenMod Project
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
#include <sys/stat.h>
#include <unistd.h>

#include <optional>
#include <string>

#include "utils.hpp"

std::optional<std::string> read_file(const std::string& path) noexcept {
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return std::nullopt;

    // RAII file descriptor guard
    struct FdGuard {
        int fd;
        ~FdGuard() {
            if (fd >= 0) close(fd);
        }
    } guard{fd};

    struct stat st;
    if (fstat(fd, &st) != 0) return std::nullopt;

    std::string data;
    data.resize(st.st_size + 2);

    const ssize_t bytes_read = read(fd, data.data(), st.st_size);
    if (bytes_read != st.st_size) return std::nullopt;

    data[st.st_size] = '\n';
    data[st.st_size + 1] = '\0';
    return data;
}
