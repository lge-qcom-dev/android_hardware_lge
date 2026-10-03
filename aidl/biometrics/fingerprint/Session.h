// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <aidl/android/hardware/biometrics/common/BnCancellationSignal.h>
#include <aidl/android/hardware/biometrics/fingerprint/BnSession.h>
#include <aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h>
#include <android/binder_auto_utils.h>
#include <atomic>
#include <map>

#include "Config.h"
#include "LegacyHal.h"
#include "Worker.h"

namespace lge::fingerprint {
namespace fp = ::aidl::android::hardware::biometrics::fingerprint;
namespace common = ::aidl::android::hardware::biometrics::common;
using ::aidl::android::hardware::keymaster::HardwareAuthToken;
using common::ICancellationSignal;
using common::OperationContext;
using fp::PointerContext;
using ndk::ScopedAStatus;

struct Lockout {
    int failures = 0;
    std::chrono::steady_clock::time_point until{};
};
using Lockouts = std::map<int32_t, Lockout>;

class Session : public fp::BnSession {
  public:
    Session(std::shared_ptr<LegacyHal> hal, std::shared_ptr<Worker> worker,
            std::shared_ptr<Lockouts> lockouts, Config config, int32_t userId,
            std::shared_ptr<fp::ISessionCallback> callback);
    ~Session() override;
    binder_status_t initialize();
    bool closed() const { return mClosed.load(); }
    void notify(fingerprint_msg_t message);

    ScopedAStatus generateChallenge() override;
    ScopedAStatus revokeChallenge(int64_t challenge) override;
    ScopedAStatus enroll(const HardwareAuthToken& hat,
                         std::shared_ptr<ICancellationSignal>* out) override;
    ScopedAStatus authenticate(int64_t operationId,
                               std::shared_ptr<ICancellationSignal>* out) override;
    ScopedAStatus detectInteraction(std::shared_ptr<ICancellationSignal>* out) override;
    ScopedAStatus enumerateEnrollments() override;
    ScopedAStatus removeEnrollments(const std::vector<int32_t>& ids) override;
    ScopedAStatus getAuthenticatorId() override;
    ScopedAStatus invalidateAuthenticatorId() override;
    ScopedAStatus resetLockout(const HardwareAuthToken& hat) override;
    ScopedAStatus close() override;
    ScopedAStatus onPointerDown(int32_t pointerId, int32_t x, int32_t y, float minor,
                                float major) override;
    ScopedAStatus onPointerUp(int32_t pointerId) override;
    ScopedAStatus onUiReady() override;
    ScopedAStatus authenticateWithContext(int64_t operationId, const OperationContext& context,
                                          std::shared_ptr<ICancellationSignal>* out) override;
    ScopedAStatus enrollWithContext(const HardwareAuthToken& hat, const OperationContext& context,
                                    std::shared_ptr<ICancellationSignal>* out) override;
    ScopedAStatus detectInteractionWithContext(const OperationContext& context,
                                               std::shared_ptr<ICancellationSignal>* out) override;
    ScopedAStatus onPointerDownWithContext(const PointerContext& context) override;
    ScopedAStatus onPointerUpWithContext(const PointerContext& context) override;
    ScopedAStatus onContextChanged(const OperationContext& context) override;
    ScopedAStatus onPointerCancelWithContext(const PointerContext& context) override;
    ScopedAStatus setIgnoreDisplayTouches(bool ignore) override;
    void cancel(uint64_t generation);

  private:
    enum class Operation { Idle, Authenticate, Enroll, Enumerate, Remove };
    void post(std::function<void(Session&)> task);
    bool begin(Operation operation, uint64_t generation);
    void finish();
    void release();
    void reportAcquiredGood();
    void updateContext(const OperationContext& context);
    void fail(fp::Error error);
    void handle(const fingerprint_msg_t& message);
    bool checkLockout();
    void scheduleLockoutExpiry();
    void removeNext();
    bool writePanel(int state);
    bool resetTouch();

    std::shared_ptr<LegacyHal> mHal;
    std::shared_ptr<Worker> mWorker;
    std::shared_ptr<Lockouts> mLockouts;
    const Config mConfig;
    const int32_t mUserId;
    std::shared_ptr<fp::ISessionCallback> mCallback;
    Illumination mIllumination;
    std::atomic<bool> mClosed{false};
    std::atomic<uint64_t> mNextGeneration{1};
    std::atomic<uint64_t> mGeneration{0};
    ndk::ScopedAIBinder_DeathRecipient mDeathRecipient;
    void* mDeathCookie = nullptr;

    // Below: owned exclusively by mWorker.
    Operation mOperation = Operation::Idle;
    uint64_t mTouchGeneration = 0;
    int32_t mPointerId = -1;
    bool mFingerDown = false;
    bool mUiReady = false;
    bool mAcquiredGood = false;
    bool mIgnoreTouches = false;
    bool mInitialized = false;
    std::vector<int32_t> mEnrollments;
    std::vector<int32_t> mRemovalQueue;
    size_t mRemovalIndex = 0;
};

class CancellationSignal : public common::BnCancellationSignal {
  public:
    CancellationSignal(std::weak_ptr<Session> session, uint64_t generation)
        : mSession(std::move(session)), mGeneration(generation) {}
    ScopedAStatus cancel() override;

  private:
    std::weak_ptr<Session> mSession;
    const uint64_t mGeneration;
};

}  // namespace lge::fingerprint
