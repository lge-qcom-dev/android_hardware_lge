// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
package vendor.lge.hardware.dualscreen;

@VintfStability
parcelable CaseInfo {
    boolean connected;
    // Kernel smartcover states: 0 open, 1 closed, 5 folded behind the phone.
    int coverState;
    int width;
    int height;
}
