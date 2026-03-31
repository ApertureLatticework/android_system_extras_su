/*
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

#include <cerrno>
#include <cstdlib>
#include <string_view>

#include "../utils.hpp"
#include "pm-wrapper.hpp"

namespace {

constexpr std::string_view PACKAGE_LIST_PATH = "/data/system/packages.list";

}  // namespace

std::optional<std::string> resolve_package_name(uid_t uid) noexcept {
    auto packages_opt = read_file(std::string(PACKAGE_LIST_PATH));
    if (!packages_opt) {
        return std::nullopt;
    }

    std::string_view remaining = *packages_opt;

    while (!remaining.empty()) {
        // Find end of line
        auto line_end = remaining.find('\n');
        std::string_view line = (line_end != std::string_view::npos)
            ? remaining.substr(0, line_end)
            : remaining;

        // Find package name (first token)
        auto space_pos = line.find(' ');
        if (space_pos != std::string_view::npos) {
            std::string_view pkg_name = line.substr(0, space_pos);

            // Find UID (second token)
            line.remove_prefix(space_pos + 1);
            auto next_space = line.find(' ');
            std::string_view uid_str = (next_space != std::string_view::npos)
                ? line.substr(0, next_space)
                : line;

            // Parse UID
            char* endptr = nullptr;
            errno = 0;
            long pkg_uid = std::strtol(uid_str.data(), &endptr, 10);

            if (errno == 0 && endptr == uid_str.data() + uid_str.size() &&
                static_cast<uid_t>(pkg_uid) == uid) {
                return std::string(pkg_name);
            }
        }

        // Move to next line
        if (line_end != std::string_view::npos) {
            remaining.remove_prefix(line_end + 1);
        } else {
            break;
        }
    }

    return std::nullopt;
}
