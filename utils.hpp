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

#pragma once

#include <optional>
#include <string>

/**
 * Reads a file and returns its content as a string.
 * The content is guaranteed to be terminated with "\n\0".
 *
 * @param path The path to the file to read
 * @return std::optional<std::string> containing the file content on success,
 *         or std::nullopt on failure
 */
[[nodiscard]] std::optional<std::string> read_file(const std::string& path) noexcept;
