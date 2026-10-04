# SPDX-FileCopyrightText: 2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0

PRODUCT_PACKAGES += \
    android.hardware.biometrics.fingerprint-service.lge \
    LgeUdfpsFrameworkOverlay \
    LgeUdfpsSystemUIOverlay

$(call soong_config_set,surfaceflinger,udfps_lib,//hardware/lge:libudfps_extension.lge)
