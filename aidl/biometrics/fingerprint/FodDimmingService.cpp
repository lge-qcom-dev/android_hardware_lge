/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LgeFodService"

#include "NativeFodDimming.h"

#include <memory>
#include <sys/socket.h>

#include <android-base/unique_fd.h>
#include <binder/ProcessState.h>
#include <cutils/sockets.h>
#include <log/log.h>

using namespace aidl::android::hardware::biometrics::fingerprint;
using ::android::base::unique_fd;

int main() {
    ::android::ProcessState::self()->setThreadPoolMaxThreadCount(1);
    ::android::ProcessState::self()->startThreadPool();

    const int server = android_get_control_socket("lge-fod");
    if (server < 0 || listen(server, 1) < 0) {
        ALOGE("Unable to listen on fingerprint dimming socket: %m");
        return 1;
    }
    for (;;) {
        unique_fd client(accept4(server, nullptr, nullptr, SOCK_CLOEXEC));
        if (client.get() < 0) continue;
        std::unique_ptr<NativeFodDimming> dimming;
        FodRequest request;
        while (recv(client.get(), &request, sizeof(request), MSG_TRUNC) == sizeof(request)) {
            if (request.reference <= 0 || request.reference > 2047 ||
                request.location.sensorRadius <= 0 || request.location.sensorRadius > 2048 ||
                request.location.sensorLocationX < 0 || request.location.sensorLocationX > 8192 ||
                request.location.sensorLocationY < 0 || request.location.sensorLocationY > 8192) {
                break;
            }
            if (!dimming) {
                dimming = std::make_unique<NativeFodDimming>(request.reference, request.location);
            }
            int32_t result = 0;
            switch (request.command) {
                case FodCommand::PREPARE:
                    result = dimming->prepare();
                    break;
                case FodCommand::SHOW:
                    if (request.brightness >= 0 && request.maximum > 0 &&
                        request.maximum <= 65535) {
                        result = dimming->show(request.brightness, request.maximum);
                    }
                    break;
                case FodCommand::HIDE:
                    dimming->hide();
                    result = 1;
                    break;
            }
            if (send(client.get(), &result, sizeof(result), MSG_NOSIGNAL) != sizeof(result)) break;
        }
        // Closing the HAL session or losing its process must remove the mask.
        if (dimming) dimming->hide();
    }
}
