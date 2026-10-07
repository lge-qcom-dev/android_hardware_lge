/*
 * Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "NativeFodDimming"

#include "NativeFodDimming.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>

#include <hardware/gralloc.h>
#include <log/log.h>
#include <ui/DisplayState.h>

namespace aidl::android::hardware::biometrics::fingerprint {

using ::android::GraphicBuffer;
using ::android::SurfaceComposerClient;
using ::android::gui::ISurfaceComposerClient;
using ::android::sp;

NativeFodDimming::NativeFodDimming(int reference, const FodGeometry& location)
    : mReference(reference), mLocation(location) {}

bool NativeFodDimming::prepare() {
    const auto displays = SurfaceComposerClient::getPhysicalDisplayIds();
    if (displays.empty()) return false;
    const auto display = SurfaceComposerClient::getPhysicalDisplayToken(displays.front());
    ::android::ui::DisplayState state;
    if (SurfaceComposerClient::getDisplayState(display, &state) != ::android::OK) return false;
    const int width = state.layerStackSpaceRect.getWidth();
    const int height = state.layerStackSpaceRect.getHeight();
    if (width <= 0 || height <= 0) return false;

    if (mMask && mMask->getWidth() == static_cast<uint32_t>(width) &&
        mMask->getHeight() == static_cast<uint32_t>(height) && mRotation == state.orientation) {
        return true;
    }

    int x = mLocation.sensorLocationX;
    int y = mLocation.sensorLocationY;
    switch (state.orientation) {
        case ::android::ui::ROTATION_90:
            x = mLocation.sensorLocationY;
            y = height - mLocation.sensorLocationX;
            break;
        case ::android::ui::ROTATION_180:
            x = width - x;
            y = height - y;
            break;
        case ::android::ui::ROTATION_270:
            x = width - mLocation.sensorLocationY;
            y = mLocation.sensorLocationX;
            break;
        default:
            break;
    }

    const auto mask = sp<GraphicBuffer>::make(width, height, ::android::PIXEL_FORMAT_RGBA_8888,
                                             1, GRALLOC_USAGE_SW_WRITE_RARELY |
                                                        GRALLOC_USAGE_HW_COMPOSER |
                                                        GRALLOC_USAGE_HW_TEXTURE,
                                             "LGE fingerprint dim mask");
    void* data = nullptr;
    if (mask->initCheck() != ::android::OK ||
        mask->lock(GRALLOC_USAGE_SW_WRITE_RARELY, &data) != ::android::OK) {
        ALOGE("Unable to allocate fingerprint dim mask");
        return false;
    }
    auto* pixels = static_cast<uint32_t*>(data);
    const int radius = mLocation.sensorRadius;
    for (int row = 0; row < height; ++row) {
        auto* line = pixels + row * mask->getStride();
        std::fill_n(line, width, 0xff000000);
        if (std::abs(row - y) > radius) continue;
        for (int col = std::max(0, x - radius); col < std::min(width, x + radius + 1); ++col) {
            if ((col - x) * (col - x) + (row - y) * (row - y) <= radius * radius) {
                line[col] = 0;
            }
        }
    }
    if (mask->unlock() != ::android::OK) return false;

    if (!mClient) mClient = sp<SurfaceComposerClient>::make();
    if (mClient->initCheck() != ::android::OK) return false;
    if (!mSurface) {
        mSurface = mClient->createSurface(::android::String8("LGE fingerprint dim"), width,
                                         height, ::android::PIXEL_FORMAT_RGBA_8888,
                                         ISurfaceComposerClient::eFXSurfaceBufferState |
                                                 ISurfaceComposerClient::eHidden);
    }
    if (!mSurface) return false;
    // This root layer affects only the primary display, including during AOD.
    const auto status = SurfaceComposerClient::Transaction()
                                .setLayerStack(mSurface, state.layerStack)
                                .setLayer(mSurface, std::numeric_limits<int32_t>::max() - 1)
                                .setTrustedOverlay(mSurface, true)
                                .setBuffer(mSurface, mask)
                                .apply();
    if (status != ::android::OK) return false;
    mMask = mask;
    mRotation = state.orientation;
    return true;
}

bool NativeFodDimming::show(int brightness, int maximum) {
    // The DTS maps the backlight index linearly to 0..2047. Black compositing
    // scales SDR pixel values, so compensate for the approximate SDR gamma too.
    if (maximum <= 0 || !prepare()) {
        ALOGE("Unable to prepare fingerprint dimming");
        return false;
    }
    const float ratio = std::clamp(brightness * 2047.0f / (maximum * mReference), 0.0f, 1.0f);
    const float alpha = 1.0f - std::pow(ratio, 1.0f / 2.2f);
    auto committed = std::make_shared<std::promise<void>>();
    auto ready = committed->get_future();
    const auto status = SurfaceComposerClient::Transaction()
                                .setAlpha(mSurface, alpha)
                                .show(mSurface)
                                .addTransactionCommittedCallback(
                                        [committed](void*, auto, const auto&, const auto&) {
                                            committed->set_value();
                                        },
                                        nullptr)
                                .apply();
    mVisible = status == ::android::OK;
    if (!mVisible) ALOGE("Unable to show fingerprint dimming: %d", status);
    // Avoid the four-second synchronous transaction timeout when the display
    // is transitioning out of AOD. Normally the mask commits in the next frame.
    if (mVisible && ready.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) {
        ALOGW("Fingerprint dim transaction has not committed yet");
    }
    return mVisible;
}

void NativeFodDimming::hide() {
    if (!mVisible) return;
    const auto status = SurfaceComposerClient::Transaction().hide(mSurface).apply();
    if (status == ::android::OK) mVisible = false;
    else ALOGE("Unable to hide fingerprint dimming: %d", status);
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
