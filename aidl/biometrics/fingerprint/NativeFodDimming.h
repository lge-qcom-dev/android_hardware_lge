/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "FodDimmingProtocol.h"
#include <gui/SurfaceComposerClient.h>
#include <ui/GraphicBuffer.h>

namespace aidl::android::hardware::biometrics::fingerprint {

// Owned by a connection to the system helper. The circular opening also
// keeps illumination intact when SurfaceFlinger falls back to GPU composition.
class NativeFodDimming {
  public:
    NativeFodDimming(int reference, const FodGeometry& location);
    bool prepare();
    bool show(int brightness, int maximum);
    void hide();

  private:
    const int mReference;
    const FodGeometry mLocation;
    ::android::sp<::android::SurfaceComposerClient> mClient;
    ::android::sp<::android::SurfaceControl> mSurface;
    ::android::sp<::android::GraphicBuffer> mMask;
    ::android::ui::Rotation mRotation = ::android::ui::ROTATION_0;
    bool mVisible = false;
};

}  // namespace aidl::android::hardware::biometrics::fingerprint
