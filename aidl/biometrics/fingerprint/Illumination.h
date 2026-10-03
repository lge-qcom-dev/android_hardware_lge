// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>

namespace lge::fingerprint {

enum class PanelSequence { Legacy, Managed };
enum class IlluminationMode { Global, Local };

// Panel and scan transitions are owned exclusively by the session worker.
class Illumination {
  public:
    using PanelWrite = std::function<bool(int)>;
    using ScanControl = std::function<bool(bool)>;
    Illumination(PanelSequence sequence, IlluminationMode mode, PanelWrite panel, ScanControl scan,
                 std::function<bool()> resetTouch);
    bool prepare();
    bool setDisplayActive(bool active);
    bool press();
    bool startScan();
    bool release();
    bool finish();
    bool recover();

  private:
    bool preparePanel();
    bool restore();
    const PanelSequence mSequence;
    const IlluminationMode mMode;
    PanelWrite mPanel;
    ScanControl mScan;
    std::function<bool()> mResetTouch;
    bool mRequested = false;
    bool mDisplayActive = true;
    bool mPrepared = false;
    bool mPressed = false;
    bool mScanning = false;
};

}  // namespace lge::fingerprint
