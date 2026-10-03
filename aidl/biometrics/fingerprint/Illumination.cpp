// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "Illumination.h"

#include <utility>

namespace lge::fingerprint {
namespace {
constexpr int kReady = 2;
constexpr int kExit = 3;
constexpr int kOn = 1;
constexpr int kOff = 0;
constexpr int kManagedOn = 11;
constexpr int kManagedOff = 10;
}  // namespace

Illumination::Illumination(PanelSequence sequence, IlluminationMode mode, PanelWrite panel,
                           ScanControl scan, std::function<bool()> resetTouch)
    : mSequence(sequence),
      mMode(mode),
      mPanel(std::move(panel)),
      mScan(std::move(scan)),
      mResetTouch(std::move(resetTouch)) {}

bool Illumination::prepare() {
    mRequested = true;
    return !mDisplayActive || preparePanel();
}

bool Illumination::setDisplayActive(bool active) {
    if (mDisplayActive == active) return true;
    mDisplayActive = active;
    // Authentication can outlive the visible lockscreen. READY suppresses the
    // kernel's normal backlight writes, so it must never span display sleep.
    if (!active) return restore();
    return !mRequested || preparePanel();
}

bool Illumination::preparePanel() {
    if (mPrepared) return true;
    // Mark before writing: a failing write can still have partially changed panel state.
    mPrepared = true;
    if (mMode == IlluminationMode::Local || mPanel(kReady)) return true;
    finish();
    return false;
}

bool Illumination::press() {
    if (!mRequested || !preparePanel()) return false;
    if (mPressed) return true;
    mPressed = true;
    if (mPanel(mSequence == PanelSequence::Managed ? kManagedOn : kOn)) return true;
    finish();
    return false;
}

bool Illumination::startScan() {
    if (!mPressed) return false;
    if (mScanning) return true;
    mScanning = true;
    if (mScan(true)) return true;
    finish();
    return false;
}

bool Illumination::release() {
    bool ok = true;
    if (mScanning) {
        ok = mScan(false);
        mScanning = false;
    }
    if (mPressed) {
        ok = mPanel(mSequence == PanelSequence::Managed ? kManagedOff : kOff) && ok;
        ok = mResetTouch() && ok;
        mPressed = false;
    }
    // AOD capture may temporarily prepare an otherwise sleeping display.
    if (!mDisplayActive && mPrepared) {
        ok = mPanel(kExit) && ok;
        mPrepared = false;
    }
    return ok;
}

bool Illumination::finish() {
    mRequested = false;
    return restore();
}

bool Illumination::restore() {
    bool ok = release();
    if (mPrepared) {
        ok = mPanel(kExit) && ok;
        mPrepared = false;
    }
    return ok;
}

bool Illumination::recover() {
    // Service restart can leave the panel prepared without any in-memory state.
    bool ok = mScan(false);
    return mPanel(kExit) && ok;
}

}  // namespace lge::fingerprint
