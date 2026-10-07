/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "FodDimming"

#include "FodDimming.h"

#include <fstream>
#include <poll.h>
#include <sys/socket.h>

#include <cutils/sockets.h>
#include <log/log.h>

namespace aidl::android::hardware::biometrics::fingerprint {

FodDimming::FodDimming(int reference, const SensorLocation& location)
    : mReference(reference),
      mLocation{location.sensorLocationX, location.sensorLocationY, location.sensorRadius} {}

bool FodDimming::send(FodCommand command, int brightness, int maximum) {
    if (mSocket.get() < 0) {
        mSocket.reset(socket_local_client("lge-fod", ANDROID_SOCKET_NAMESPACE_RESERVED,
                                         SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK));
        if (mSocket.get() < 0) {
            ALOGE("Unable to connect to fingerprint dimming service: %m");
            return false;
        }
    }
    const FodRequest request{command, mReference, mLocation, brightness, maximum};
    if (::send(mSocket.get(), &request, sizeof(request), MSG_NOSIGNAL) != sizeof(request)) {
        ALOGE("Unable to send fingerprint dimming request: %m");
        mSocket.reset();
        return false;
    }
    pollfd fd{mSocket.get(), POLLIN, 0};
    int32_t result = 0;
    // A missing or unresponsive compositor must never stall fingerprint enrollment.
    if (poll(&fd, 1, 300) <= 0 || !(fd.revents & POLLIN) ||
        recv(mSocket.get(), &result, sizeof(result), 0) != sizeof(result)) {
        ALOGE("Fingerprint dimming service did not respond");
        mSocket.reset();
        return false;
    }
    return result != 0;
}

bool FodDimming::prepare() {
    return send(FodCommand::PREPARE);
}

void FodDimming::show() {
    int brightness = 0;
    int maximum = 0;
    std::ifstream("/sys/class/backlight/panel0-backlight/brightness") >> brightness;
    std::ifstream("/sys/class/backlight/panel0-backlight/max_brightness") >> maximum;
    if (!send(FodCommand::SHOW, brightness, maximum)) {
        ALOGE("Unable to show fingerprint dimming");
    }
}

void FodDimming::hide() {
    if (mSocket.get() >= 0) send(FodCommand::HIDE);
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
