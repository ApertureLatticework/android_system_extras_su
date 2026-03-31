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

#pragma once

#include <optional>
#include <string>

/**
 * Tries to resolve a package name from a uid via the packages list file.
 *
 * If there is no matching uid, returns std::nullopt.
 *
 * Since packages may share UID, this function will return the first present
 * in packages.list.
 *
 * @param uid The UID to look up
 * @return std::optional<std::string> containing the package name on success,
 *         or std::nullopt on failure
 */
[[nodiscard]] std::optional<std::string> resolve_package_name(uid_t uid) noexcept;
