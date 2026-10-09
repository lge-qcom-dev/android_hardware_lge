// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
package vendor.lge.hardware.dualscreen;
import vendor.lge.hardware.dualscreen.CaseInfo;

@VintfStability
oneway interface ICaseCallback {
    void onCaseChanged(in CaseInfo info);
}
