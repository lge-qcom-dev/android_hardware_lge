// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "Session.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <endian.h>
#include <sys/stat.h>
#include <algorithm>
#include <cerrno>
#include <format>

namespace lge::fingerprint {
namespace {
using DeathCookie = std::weak_ptr<Session>;
constexpr int kTimedThreshold = 5;
constexpr int kPermanentThreshold = 20;
constexpr auto kLockoutDuration = std::chrono::seconds(30);

fp::Error errorFromLegacy(int error, int32_t& vendorCode) {
    vendorCode = 0;
    switch (error) {
        case FINGERPRINT_ERROR_HW_UNAVAILABLE:
            return fp::Error::HW_UNAVAILABLE;
        case FINGERPRINT_ERROR_UNABLE_TO_PROCESS:
            return fp::Error::UNABLE_TO_PROCESS;
        case FINGERPRINT_ERROR_TIMEOUT:
            return fp::Error::TIMEOUT;
        case FINGERPRINT_ERROR_NO_SPACE:
            return fp::Error::NO_SPACE;
        case FINGERPRINT_ERROR_CANCELED:
            return fp::Error::CANCELED;
        case FINGERPRINT_ERROR_UNABLE_TO_REMOVE:
            return fp::Error::UNABLE_TO_REMOVE;
        default:
            if (error >= FINGERPRINT_ERROR_VENDOR_BASE) {
                vendorCode = error - FINGERPRINT_ERROR_VENDOR_BASE;
                return fp::Error::VENDOR;
            }
            return fp::Error::UNABLE_TO_PROCESS;
    }
}

fp::AcquiredInfo acquiredFromLegacy(int info, int32_t& vendorCode) {
    vendorCode = 0;
    switch (info) {
        case FINGERPRINT_ACQUIRED_GOOD:
            return fp::AcquiredInfo::GOOD;
        case FINGERPRINT_ACQUIRED_PARTIAL:
            return fp::AcquiredInfo::PARTIAL;
        case FINGERPRINT_ACQUIRED_INSUFFICIENT:
            return fp::AcquiredInfo::INSUFFICIENT;
        case FINGERPRINT_ACQUIRED_IMAGER_DIRTY:
            return fp::AcquiredInfo::SENSOR_DIRTY;
        case FINGERPRINT_ACQUIRED_TOO_SLOW:
            return fp::AcquiredInfo::TOO_SLOW;
        case FINGERPRINT_ACQUIRED_TOO_FAST:
            return fp::AcquiredInfo::TOO_FAST;
        default:
            if (info >= FINGERPRINT_ACQUIRED_VENDOR_BASE) {
                vendorCode = info - FINGERPRINT_ACQUIRED_VENDOR_BASE;
                return fp::AcquiredInfo::VENDOR;
            }
            return fp::AcquiredInfo::UNKNOWN;
    }
}

hw_auth_token_t toLegacy(const HardwareAuthToken& token) {
    hw_auth_token_t hat{};
    hat.challenge = token.challenge;
    hat.user_id = token.userId;
    hat.authenticator_id = token.authenticatorId;
    hat.authenticator_type = htobe32(static_cast<uint32_t>(token.authenticatorType));
    hat.timestamp = htobe64(token.timestamp.milliSeconds);
    std::copy(token.mac.begin(), token.mac.end(), hat.hmac);
    return hat;
}

HardwareAuthToken fromLegacy(const hw_auth_token_t& hat) {
    HardwareAuthToken token;
    token.challenge = hat.challenge;
    token.userId = hat.user_id;
    token.authenticatorId = hat.authenticator_id;
    token.authenticatorType =
            static_cast<::aidl::android::hardware::keymaster::HardwareAuthenticatorType>(
                    be32toh(hat.authenticator_type));
    token.timestamp.milliSeconds = be64toh(hat.timestamp);
    token.mac.assign(std::begin(hat.hmac), std::end(hat.hmac));
    return token;
}
}  // namespace

Session::Session(std::shared_ptr<LegacyHal> hal, std::shared_ptr<Worker> worker,
                 std::shared_ptr<Lockouts> lockouts, Config config, int32_t userId,
                 std::shared_ptr<fp::ISessionCallback> callback)
    : mHal(std::move(hal)),
      mWorker(std::move(worker)),
      mLockouts(std::move(lockouts)),
      mConfig(std::move(config)),
      mUserId(userId),
      mCallback(std::move(callback)),
      mIllumination(
              mConfig.sequence, mConfig.mode, [this](int state) { return writePanel(state); },
              [this](bool enabled) { return mHal->scan(enabled); },
              [this] { return resetTouch(); }),
      mDeathRecipient(AIBinder_DeathRecipient_new([](void* cookie) {
          if (auto session = static_cast<DeathCookie*>(cookie)->lock()) session->close();
      })) {
    AIBinder_DeathRecipient_setOnUnlinked(
            mDeathRecipient.get(), [](void* cookie) { delete static_cast<DeathCookie*>(cookie); });
}

Session::~Session() {
    // The unlink callback owns the cookie, including in the binder-death race.
    if (mDeathCookie) {
        AIBinder_unlinkToDeath(mCallback->asBinder().get(), mDeathRecipient.get(), mDeathCookie);
    }
}

binder_status_t Session::initialize() {
    mDeathCookie = new DeathCookie(ref<Session>());
    const auto result =
            AIBinder_linkToDeath(mCallback->asBinder().get(), mDeathRecipient.get(), mDeathCookie);
    if (result != STATUS_OK) {
        mDeathCookie = nullptr;  // onUnlinked frees the cookie even when linking fails.
        return result;
    }
    post([](Session& self) {
        const auto path = std::format("/data/vendor_de/{}/fpdata", self.mUserId);
        if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
            PLOG(ERROR) << "Cannot create fingerprint storage";
            self.fail(fp::Error::HW_UNAVAILABLE);
            return;
        }
        auto* device = self.mHal->device();
        if (device->set_active_group(device, self.mUserId, path.c_str()) != 0 ||
            !self.mIllumination.recover()) {
            self.fail(fp::Error::HW_UNAVAILABLE);
            return;
        }
        self.mInitialized = true;
    });
    return STATUS_OK;
}

void Session::post(std::function<void(Session&)> task) {
    mWorker->post([self = ref<Session>(), task = std::move(task)] {
        if (!self->closed()) task(*self);
    });
}

bool Session::writePanel(int state) {
    if (::android::base::WriteStringToFile(std::to_string(state), mConfig.panelPath)) return true;
    PLOG(ERROR) << "Cannot set fingerprint panel state " << state;
    return false;
}

bool Session::resetTouch() {
    if (mConfig.touchResetPath.empty()) return true;
    if (::android::base::WriteStringToFile("4", mConfig.touchResetPath)) return true;
    PLOG(ERROR) << "Cannot restore fingerprint touch state";
    return false;
}

bool Session::begin(Operation operation, uint64_t generation) {
    if (!mInitialized || mOperation != Operation::Idle) {
        mCallback->onError(fp::Error::UNABLE_TO_PROCESS, 0);
        return false;
    }
    mGeneration.store(generation);
    mOperation = operation;
    mEnrollments.clear();
    if (operation == Operation::Authenticate || operation == Operation::Enroll) {
        if (!mIllumination.prepare()) {
            fail(fp::Error::HW_UNAVAILABLE);
            return false;
        }
    }
    return true;
}

void Session::release() {
    ++mTouchGeneration;
    mFingerDown = false;
    mUiReady = false;
    mPointerId = -1;
    if (!mIllumination.release()) LOG(ERROR) << "Fingerprint illumination release failed";
}

void Session::finish() {
    release();
    if (!mIllumination.finish()) LOG(ERROR) << "Fingerprint panel restoration failed";
    mAcquiredGood = false;
    mOperation = Operation::Idle;
    mGeneration.store(0);
}

void Session::reportAcquiredGood() {
    if (!mAcquiredGood) return;
    mAcquiredGood = false;
    mCallback->onAcquired(fp::AcquiredInfo::GOOD, 0);
}

void Session::fail(fp::Error error) {
    const bool active = mOperation != Operation::Idle;
    finish();
    if (active) mHal->device()->cancel(mHal->device());
    mCallback->onError(error, 0);
}

ScopedAStatus Session::generateChallenge() {
    post([](Session& self) {
        if (!self.mInitialized) {
            self.fail(fp::Error::HW_UNAVAILABLE);
            return;
        }
        self.mCallback->onChallengeGenerated(self.mHal->device()->pre_enroll(self.mHal->device()));
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::revokeChallenge(int64_t challenge) {
    post([challenge](Session& self) {
        if (self.mHal->device()->post_enroll(self.mHal->device()) != 0) {
            self.fail(fp::Error::UNABLE_TO_PROCESS);
        } else {
            self.mCallback->onChallengeRevoked(challenge);
        }
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::enroll(const HardwareAuthToken& hat,
                              std::shared_ptr<ICancellationSignal>* out) {
    if (hat.mac.size() != sizeof(hw_auth_token_t{}.hmac)) {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const uint64_t generation = mNextGeneration.fetch_add(1);
    *out = ndk::SharedRefBase::make<CancellationSignal>(ref<Session>(), generation);
    post([generation, token = toLegacy(hat)](Session& self) {
        if (!self.begin(Operation::Enroll, generation)) return;
        auto* device = self.mHal->device();
        if (device->enroll(device, &token, self.mUserId, 60) != 0) {
            self.fail(fp::Error::UNABLE_TO_PROCESS);
        }
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::authenticate(int64_t operationId,
                                    std::shared_ptr<ICancellationSignal>* out) {
    const uint64_t generation = mNextGeneration.fetch_add(1);
    *out = ndk::SharedRefBase::make<CancellationSignal>(ref<Session>(), generation);
    post([generation, operationId](Session& self) {
        if (self.checkLockout() || !self.begin(Operation::Authenticate, generation)) return;
        auto* device = self.mHal->device();
        if (device->authenticate(device, operationId, self.mUserId) != 0) {
            self.fail(fp::Error::UNABLE_TO_PROCESS);
        }
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::detectInteraction(std::shared_ptr<ICancellationSignal>* out) {
    *out = ndk::SharedRefBase::make<CancellationSignal>(ref<Session>(), 0);
    post([](Session& self) { self.mCallback->onError(fp::Error::UNABLE_TO_PROCESS, 0); });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::enumerateEnrollments() {
    const auto generation = mNextGeneration.fetch_add(1);
    post([generation](Session& self) {
        if (!self.begin(Operation::Enumerate, generation)) return;
        if (self.mHal->device()->enumerate(self.mHal->device()) != 0) {
            self.fail(fp::Error::UNABLE_TO_PROCESS);
        }
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::removeEnrollments(const std::vector<int32_t>& ids) {
    if (ids.empty() || std::any_of(ids.begin(), ids.end(), [](int32_t id) { return id <= 0; })) {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const auto generation = mNextGeneration.fetch_add(1);
    post([ids, generation](Session& self) {
        if (!self.begin(Operation::Remove, generation)) return;
        self.mRemovalQueue = ids;
        self.mRemovalIndex = 0;
        self.removeNext();
    });
    return ScopedAStatus::ok();
}

void Session::removeNext() {
    auto* device = mHal->device();
    if (device->remove(device, mUserId, mRemovalQueue[mRemovalIndex]) != 0) {
        fail(fp::Error::UNABLE_TO_REMOVE);
    }
}

ScopedAStatus Session::getAuthenticatorId() {
    post([](Session& self) {
        self.mCallback->onAuthenticatorIdRetrieved(
                self.mHal->device()->get_authenticator_id(self.mHal->device()));
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::invalidateAuthenticatorId() {
    // The legacy ABI cannot regenerate an authenticator ID independently of enrollments.
    // Do not report the unchanged ID as a successful invalidation.
    post([](Session& self) { self.mCallback->onError(fp::Error::UNABLE_TO_PROCESS, 0); });
    return ScopedAStatus::ok();
}

bool Session::checkLockout() {
    auto& lockout = (*mLockouts)[mUserId];
    if (lockout.failures >= kPermanentThreshold) {
        mCallback->onLockoutPermanent();
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (lockout.until > now) {
        mCallback->onLockoutTimed(
                std::chrono::duration_cast<std::chrono::milliseconds>(lockout.until - now).count());
        scheduleLockoutExpiry();
        return true;
    }
    return false;
}

void Session::scheduleLockoutExpiry() {
    const auto until = (*mLockouts)[mUserId].until;
    const auto delay =
            std::chrono::ceil<std::chrono::milliseconds>(until - std::chrono::steady_clock::now());
    mWorker->post(
            [weak = std::weak_ptr<Session>(ref<Session>()), until] {
                if (auto self = weak.lock(); self && !self->closed()) {
                    auto& lockout = (*self->mLockouts)[self->mUserId];
                    if (lockout.until == until) {
                        lockout.until = {};
                        self->mCallback->onLockoutCleared();
                    }
                }
            },
            delay);
}

ScopedAStatus Session::resetLockout(const HardwareAuthToken& hat) {
    if (hat.mac.size() != sizeof(hw_auth_token_t{}.hmac)) {
        return ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    // Like the HIDL adapter, software lockout trusts the credential-verified framework reset.
    // Enrollment and authentication tokens are still verified/generated by the vendor TEE.
    post([](Session& self) {
        (*self.mLockouts)[self.mUserId] = {};
        self.mCallback->onLockoutCleared();
    });
    return ScopedAStatus::ok();
}

void Session::cancel(uint64_t generation) {
    post([generation](Session& self) {
        if (generation == 0 || self.mGeneration.load() != generation) return;
        self.fail(fp::Error::CANCELED);
    });
}

ScopedAStatus CancellationSignal::cancel() {
    if (auto session = mSession.lock()) session->cancel(mGeneration);
    return ScopedAStatus::ok();
}

ScopedAStatus Session::close() {
    if (mClosed.exchange(true)) return ScopedAStatus::ok();
    mWorker->post([self = ref<Session>()] {
        self->finish();
        self->mHal->device()->cancel(self->mHal->device());
        self->mCallback->onSessionClosed();
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::onPointerDown(int32_t pointerId, int32_t, int32_t, float, float) {
    post([pointerId](Session& self) {
        if (self.mIgnoreTouches || self.mFingerDown ||
            (self.mOperation != Operation::Authenticate && self.mOperation != Operation::Enroll))
            return;
        self.mPointerId = pointerId;
        self.mFingerDown = true;
        self.mUiReady = false;
        self.mAcquiredGood = false;
        ++self.mTouchGeneration;
        if (!self.mIllumination.press()) self.fail(fp::Error::HW_UNAVAILABLE);
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::onPointerUp(int32_t pointerId) {
    post([pointerId](Session& self) {
        if (self.mFingerDown && self.mPointerId == pointerId) self.release();
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::onUiReady() {
    post([](Session& self) {
        if (!self.mFingerDown || self.mUiReady || self.mIgnoreTouches) return;
        self.mUiReady = true;
        const auto touch = self.mTouchGeneration;
        const auto operation = self.mGeneration.load();
        self.mWorker->post(
                [weak = std::weak_ptr<Session>(self.ref<Session>()), touch, operation] {
                    if (auto session = weak.lock();
                        session && !session->closed() && session->mGeneration.load() == operation &&
                        session->mTouchGeneration == touch && session->mFingerDown &&
                        !session->mIgnoreTouches) {
                        if (!session->mIllumination.startScan())
                            session->fail(fp::Error::HW_UNAVAILABLE);
                    }
                },
                self.mConfig.settleTime);
    });
    return ScopedAStatus::ok();
}

ScopedAStatus Session::authenticateWithContext(int64_t operationId, const OperationContext& context,
                                               std::shared_ptr<ICancellationSignal>* out) {
    onContextChanged(context);
    return authenticate(operationId, out);
}
ScopedAStatus Session::enrollWithContext(const HardwareAuthToken& hat,
                                         const OperationContext& context,
                                         std::shared_ptr<ICancellationSignal>* out) {
    onContextChanged(context);
    return enroll(hat, out);
}
ScopedAStatus Session::detectInteractionWithContext(const OperationContext&,
                                                    std::shared_ptr<ICancellationSignal>* out) {
    return detectInteraction(out);
}
ScopedAStatus Session::onPointerDownWithContext(const PointerContext& context) {
    return onPointerDown(context.pointerId, context.x, context.y, context.minor, context.major);
}
ScopedAStatus Session::onPointerUpWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}
ScopedAStatus Session::onPointerCancelWithContext(const PointerContext& context) {
    return onPointerUp(context.pointerId);
}
void Session::updateContext(const OperationContext& context) {
    const bool active = !context.isAod && context.displayState != common::DisplayState::AOD &&
                        context.displayState != common::DisplayState::NO_UI;
    if (!active) release();  // Invalidate any capture waiting for UI settling.
    if (!mIllumination.setDisplayActive(active)) fail(fp::Error::HW_UNAVAILABLE);
}

ScopedAStatus Session::onContextChanged(const OperationContext& context) {
    post([context](Session& self) { self.updateContext(context); });
    return ScopedAStatus::ok();
}
ScopedAStatus Session::setIgnoreDisplayTouches(bool ignore) {
    post([ignore](Session& self) {
        self.mIgnoreTouches = ignore;
        if (ignore) self.release();
    });
    return ScopedAStatus::ok();
}

void Session::notify(fingerprint_msg_t message) {
    const auto generation = mGeneration.load();
    post([generation, message](Session& self) {
        if (generation == 0 || self.mGeneration.load() != generation) return;
        self.handle(message);
    });
}

void Session::handle(const fingerprint_msg_t& message) {
    int32_t vendorCode = 0;
    switch (message.type) {
        case FINGERPRINT_ERROR: {
            const auto error = errorFromLegacy(message.data.error, vendorCode);
            finish();
            if (message.data.error == FINGERPRINT_ERROR_LOCKOUT) {
                (*mLockouts)[mUserId].failures = kPermanentThreshold;
                mCallback->onLockoutPermanent();
            } else {
                mCallback->onError(error, vendorCode);
            }
            break;
        }
        case FINGERPRINT_ACQUIRED: {
            if (mOperation != Operation::Authenticate && mOperation != Operation::Enroll) return;
            // Egis reports GOOD for each image and may capture more before returning a match.
            // Defer it: SystemUI also disables illumination when GOOD is forwarded.
            if (message.data.acquired.acquired_info == FINGERPRINT_ACQUIRED_GOOD) {
                mAcquiredGood = true;
                return;
            }
            const auto acquired =
                    acquiredFromLegacy(message.data.acquired.acquired_info, vendorCode);
            mCallback->onAcquired(acquired, vendorCode);
            break;
        }
        case FINGERPRINT_AUTHENTICATED: {
            if (mOperation != Operation::Authenticate ||
                message.data.authenticated.finger.gid != static_cast<uint32_t>(mUserId))
                return;
            release();
            reportAcquiredGood();
            const auto id = message.data.authenticated.finger.fid;
            if (id != 0) {
                finish();
                (*mLockouts)[mUserId] = {};
                mCallback->onAuthenticationSucceeded(id,
                                                     fromLegacy(message.data.authenticated.hat));
            } else {
                mCallback->onAuthenticationFailed();
                auto& lockout = (*mLockouts)[mUserId];
                ++lockout.failures;
                if (lockout.failures % kTimedThreshold == 0) {
                    lockout.until = std::chrono::steady_clock::now() + kLockoutDuration;
                }
                if (checkLockout()) {
                    finish();
                    mHal->device()->cancel(mHal->device());
                }
            }
            break;
        }
        case FINGERPRINT_TEMPLATE_ENROLLING:
            if (mOperation != Operation::Enroll ||
                message.data.enroll.finger.gid != static_cast<uint32_t>(mUserId))
                return;
            release();
            reportAcquiredGood();
            if (message.data.enroll.samples_remaining == 0) finish();
            mCallback->onEnrollmentProgress(message.data.enroll.finger.fid,
                                            message.data.enroll.samples_remaining);
            break;
        case FINGERPRINT_TEMPLATE_ENUMERATING:
            if (mOperation != Operation::Enumerate) return;
            if (message.data.enumerated.finger.fid != 0) {
                mEnrollments.push_back(message.data.enumerated.finger.fid);
            }
            if (message.data.enumerated.remaining_templates == 0) {
                finish();
                mCallback->onEnrollmentsEnumerated(mEnrollments);
            }
            break;
        case FINGERPRINT_TEMPLATE_REMOVED:
            if (mOperation != Operation::Remove) return;
            if (message.data.removed.finger.fid != 0) {
                mEnrollments.push_back(message.data.removed.finger.fid);
            }
            if (message.data.removed.remaining_templates == 0) {
                if (++mRemovalIndex < mRemovalQueue.size()) {
                    removeNext();
                } else {
                    finish();
                    mCallback->onEnrollmentsRemoved(mEnrollments);
                }
            }
            break;
    }
}

}  // namespace lge::fingerprint
