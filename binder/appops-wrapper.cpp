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

#define LOG_TAG "su"

#include <binder/AppOpsManager.h>
#include <log/log.h>

#include <string_view>

#include "../su.hpp"

using namespace android;

int appops_start_op_su(uid_t uid, std::string_view pkg_name) {
    ALOGD("Checking whether app [uid:%u, pkgName: %s] is allowed to be root",
          uid, pkg_name.data());

    AppOpsManager ops;
    int mode = ops.startOpNoThrow(AppOpsManager::OP_SU, uid, String16(pkg_name.data()), false);
    if (mode == AppOpsManager::MODE_ALLOWED) {
        ALOGD("Privilege elevation allowed by appops");
        return 0;
    }

    ALOGD("Privilege elevation denied by appops");
    return 1;
}

void appops_finish_op_su(uid_t uid, std::string_view pkg_name) {
    ALOGD("Finishing su operation for app [uid:%u, pkgName: %s]", uid, pkg_name.data());
    AppOpsManager ops;
    ops.finishOp(AppOpsManager::OP_SU, uid, String16(pkg_name.data()));
}