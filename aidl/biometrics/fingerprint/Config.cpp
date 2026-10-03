// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "Config.h"

#include <string_view>

namespace lge::fingerprint {

static_assert(LGE_UDFPS_SENSOR_X >= 0, "Configure lge_udfps.sensor_x in device.mk");
static_assert(LGE_UDFPS_SENSOR_Y >= 0, "Configure lge_udfps.sensor_y in device.mk");
static_assert(LGE_UDFPS_SENSOR_RADIUS > 0, "Configure lge_udfps.sensor_radius in device.mk");
static_assert(!LGE_UDFPS_LOCAL_ILLUMINATION || LGE_UDFPS_MANAGED_SEQUENCE,
              "Local illumination requires the managed panel sequence");
static_assert(std::string_view(LGE_UDFPS_PANEL_PATH).starts_with("/sys/"), "Invalid panel path");
static_assert(std::string_view(LGE_UDFPS_TOUCH_RESET_PATH).empty() ||
                      std::string_view(LGE_UDFPS_TOUCH_RESET_PATH).starts_with("/sys/"),
              "Invalid touch reset path");

Config Config::get() {
    return {
            .x = LGE_UDFPS_SENSOR_X,
            .y = LGE_UDFPS_SENSOR_Y,
            .radius = LGE_UDFPS_SENSOR_RADIUS,
            .sequence = LGE_UDFPS_MANAGED_SEQUENCE ? PanelSequence::Managed : PanelSequence::Legacy,
            .mode = LGE_UDFPS_LOCAL_ILLUMINATION ? IlluminationMode::Local
                                                 : IlluminationMode::Global,
            .panelPath = LGE_UDFPS_PANEL_PATH,
            .touchResetPath = LGE_UDFPS_TOUCH_RESET_PATH,
    };
}

}  // namespace lge::fingerprint
