// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include <android-base/logging.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include "Fingerprint.h"

int main(int argc, char** argv) {
    ::android::base::InitLogging(argv, ::android::base::LogdLogger(::android::base::SYSTEM));
    CHECK_EQ(argc, 1);
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto service = ndk::SharedRefBase::make<lge::fingerprint::Fingerprint>(
            lge::fingerprint::Config::get());
    const auto instance = std::string(service->descriptor) + "/default";
    CHECK_EQ(AServiceManager_addService(service->asBinder().get(), instance.c_str()), STATUS_OK);
    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
