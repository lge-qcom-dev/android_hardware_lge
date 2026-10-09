// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <aidl/vendor/lge/hardware/dualscreen/BnDualScreen.h>
#include <android-base/unique_fd.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace aidl::vendor::lge::hardware::dualscreen {

class DualScreen final : public BnDualScreen {
   public:
    DualScreen();
    ~DualScreen() override;
    ndk::ScopedAStatus setCallback(const std::shared_ptr<ICaseCallback>& callback) override;
    ndk::ScopedAStatus setDisplayState(bool interactive, int32_t brightness) override;
    ndk::ScopedAStatus drawCover(int32_t width, int32_t height,
                                 const std::vector<uint8_t>& pixels) override;

   private:
    struct ClientDeath {
        std::weak_ptr<DualScreen> owner;
        std::shared_ptr<ICaseCallback> callback;
    };
    static void clientDied(void* cookie);
    void wake();
    void run();
    void discover();
    void update();
    void notify();
    void disconnect();
    bool command(const std::string& command, std::string* response = nullptr);
    bool powerInner(bool enabled);
    bool powerCover(bool enabled);
    bool sendFrame(const std::vector<uint8_t>& pixels);

    std::mutex mMutex;
    bool mStopping = false;
    bool mInteractive = false;
    int mBrightness = 128;
    bool mNotify = true;
    CaseInfo mInfo;
    std::shared_ptr<ICaseCallback> mCallback;
    ndk::ScopedAIBinder_DeathRecipient mDeath;
    ClientDeath* mClientDeath = nullptr;
    std::vector<uint8_t> mFrame;
    android::base::unique_fd mWake;
    std::thread mThread;

    // Accessed only by the worker. Binder calls never wait for USB transactions.
    android::base::unique_fd mSerial;
    android::base::unique_fd mHid;
    std::string mUsbPath;
    bool mReady = false;
    bool mInnerOn = false;
    bool mCoverOn = false;
    int mAppliedBrightness = -1;
    int mFailures = 0;
    bool mRecoveryPending = false;
    std::chrono::steady_clock::time_point mNextAttempt{};
    std::chrono::steady_clock::time_point mNextRecovery{};
};

}  // namespace aidl::vendor::lge::hardware::dualscreen
