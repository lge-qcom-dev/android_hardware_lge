// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
package vendor.lge.hardware.dualscreen;
import vendor.lge.hardware.dualscreen.CaseInfo;
import vendor.lge.hardware.dualscreen.ICaseCallback;

@VintfStability
interface IDualScreen {
    void setCallback(in @nullable ICaseCallback callback);
    // Brightness is 0..255. The HAL clamps lit panels to the stock minimum of 10.
    void setDisplayState(boolean interactive, int brightness);
    // Row-major 8-bit grayscale; dimensions must match the current CaseInfo.
    void drawCover(int width, int height, in byte[] pixels);
}
