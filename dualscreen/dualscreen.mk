# SPDX-FileCopyrightText: 2026 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
PRODUCT_PACKAGES += \
    vendor.lge.hardware.dualscreen-service \
    LgeDualScreen \
    FrameworksResOverlayLgeDualScreen

PRODUCT_COPY_FILES += \
    hardware/lge/dualscreen/display_settings.xml:$(TARGET_COPY_OUT_VENDOR)/etc/display_settings.xml \
    hardware/lge/dualscreen/Vendor_1004_Product_637a.idc:$(TARGET_COPY_OUT_VENDOR)/usr/idc/Vendor_1004_Product_637a.idc \
    hardware/lge/dualscreen/Vendor_1004_Product_637a.kl:$(TARGET_COPY_OUT_VENDOR)/usr/keylayout/Vendor_1004_Product_637a.kl \
    hardware/lge/dualscreen/ueventd.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd/dualscreen.rc
