// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "Illumination.h"

#include <chrono>
#include <string>

namespace lge::fingerprint {

struct Config {
    int sensorId = 0;
    int x = 0;
    int y = 0;
    int radius = 0;
    int maxEnrollments = 5;
    PanelSequence sequence = PanelSequence::Legacy;
    IlluminationMode mode = IlluminationMode::Global;
    std::chrono::milliseconds settleTime{50};
    std::string panelPath;
    std::string touchResetPath;

    static Config get();
};

}  // namespace lge::fingerprint
