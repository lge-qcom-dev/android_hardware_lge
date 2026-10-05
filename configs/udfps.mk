# SPDX-FileCopyrightText: 2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0

# Fingerprint services and HALs
PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint-service.lge \
    sensors.lge

$(call soong_config_set,surfaceflinger,udfps_lib,//hardware/lge:libudfps_extension.lge)

# Fingerprint resource overlays
PRODUCT_PACKAGES += \
    FrameworkResOverlayLgeUdfps \
    SystemUIOverlayLgeUdfps
