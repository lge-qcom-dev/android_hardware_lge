/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <cstdint>

namespace aidl::android::hardware::biometrics::fingerprint {

enum class FodCommand : int32_t { PREPARE, SHOW, HIDE };

struct FodGeometry {
    int32_t sensorLocationX;
    int32_t sensorLocationY;
    int32_t sensorRadius;
};

struct FodRequest {
    FodCommand command;
    int32_t reference;
    FodGeometry location;
    int32_t brightness;
    int32_t maximum;
};

static_assert(sizeof(FodRequest) == 28);

}  // namespace aidl::android::hardware::biometrics::fingerprint
