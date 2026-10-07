/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <aidl/android/hardware/biometrics/fingerprint/SensorLocation.h>
#include <android-base/unique_fd.h>

#include "FodDimmingProtocol.h"

namespace aidl::android::hardware::biometrics::fingerprint {

// Owned by the session and used under its FOD mutex.
class FodDimming {
  public:
    FodDimming(int reference, const SensorLocation& location);
    bool prepare();
    void show();
    void hide();

  private:
    bool send(FodCommand command, int brightness = 0, int maximum = 0);
    const int mReference;
    const FodGeometry mLocation;
    ::android::base::unique_fd mSocket;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
