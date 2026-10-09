// SPDX-FileCopyrightText: 2026 The LineageOS Project
// SPDX-License-Identifier: Apache-2.0
#include "DualScreen.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/strings.h>
#include <android/binder_ibinder.h>
#include <cutils/uevent.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/hidraw.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>

#include "Protocol.h"

using android::base::ReadFileToString;
using android::base::Trim;
using android::base::unique_fd;
using android::base::WriteStringToFile;
using namespace std::chrono_literals;
namespace protocol = ::lge::dualscreen;

namespace aidl::vendor::lge::hardware::dualscreen {
namespace {
constexpr char kPd[] = "/sys/class/dualscreen/ds2/ds2_pd";
constexpr char kReady[] = "/sys/class/dualscreen/ds2/ds2_hal_ready";
constexpr char kHall[] = "/sys/class/smartcover/smartcover/state";

std::string readNode(const std::string& path) {
    std::string value;
    ReadFileToString(path, &value);
    return Trim(value);
}

// Resolve the USB parent rather than assuming hidraw0 or ttyACM0 belongs to the case.
std::string usbParent(const std::string& path) {
    std::array<char, PATH_MAX> resolved{};
    if (!realpath(path.c_str(), resolved.data())) return {};
    std::filesystem::path parent(resolved.data());
    while (parent != parent.root_path()) {
        if (readNode(parent.string() + "/idVendor") == "1004" &&
            readNode(parent.string() + "/idProduct") == "637a") {
            const auto product = readNode(parent.string() + "/product");
            if (product == "LMG905N" || product == "LMV600N" || product == "LMV515N" ||
                product == "DS3" || product == "DS2")
                return parent.string();
        }
        parent = parent.parent_path();
    }
    return {};
}

ndk::ScopedAStatus checkCaller() {
    return AIBinder_getCallingUid() == 1000 ? ndk::ScopedAStatus::ok()
                                            : ndk::ScopedAStatus::fromExceptionCode(EX_SECURITY);
}

bool writeReport(int fd, const std::vector<uint8_t>& report) {
    // HID writes are atomic reports. A short write must not become a second report.
    return TEMP_FAILURE_RETRY(write(fd, report.data(), report.size())) ==
           static_cast<ssize_t>(report.size());
}
}  // namespace

DualScreen::DualScreen() : mWake(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    CHECK(mWake.ok());
    mDeath = ndk::ScopedAIBinder_DeathRecipient(AIBinder_DeathRecipient_new(clientDied));
    AIBinder_DeathRecipient_setOnUnlinked(
        mDeath.get(), [](void* cookie) { delete static_cast<ClientDeath*>(cookie); });
    mThread = std::thread(&DualScreen::run, this);
}

DualScreen::~DualScreen() {
    {
        std::lock_guard lock(mMutex);
        mStopping = true;
    }
    wake();
    mThread.join();
    mDeath.set(nullptr);
}

void DualScreen::clientDied(void* cookie) {
    const auto* client = static_cast<ClientDeath*>(cookie);
    if (auto owner = client->owner.lock()) {
        {
            std::lock_guard lock(owner->mMutex);
            if (owner->mCallback != client->callback) return;
            owner->mCallback.reset();
            owner->mClientDeath = nullptr;
            owner->mInteractive = false;
        }
        owner->wake();
    }
}

void DualScreen::wake() {
    // Cover alarms can return immediately after queuing a frame. Keep the queued
    // work awake until the worker drains it, with a timeout if the process dies.
    WriteStringToFile("lge-dualscreen 30000000000", "/sys/power/wake_lock");
    uint64_t value = 1;
    if (write(mWake.get(), &value, sizeof(value)) < 0 && errno != EAGAIN) {
        PLOG(ERROR) << "Cannot wake dual-screen worker";
    }
}

ndk::ScopedAStatus DualScreen::setCallback(const std::shared_ptr<ICaseCallback>& callback) {
    auto status = checkCaller();
    if (!status.isOk()) return status;
    {
        std::lock_guard lock(mMutex);
        if (mCallback && mClientDeath) {
            AIBinder_unlinkToDeath(mCallback->asBinder().get(), mDeath.get(), mClientDeath);
            mClientDeath = nullptr;
        }
        mCallback = callback;
        if (callback) {
            auto* client = new ClientDeath{ref<DualScreen>(), callback};
            const auto result =
                AIBinder_linkToDeath(callback->asBinder().get(), mDeath.get(), client);
            if (result != STATUS_OK) {
                // The unlink callback owns client even if linkToDeath fails.
                mCallback.reset();
                mInteractive = false;
                return ndk::ScopedAStatus::fromStatus(result);
            }
            mClientDeath = client;
        }
        mNotify = true;
        if (!callback) mInteractive = false;
    }
    wake();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DualScreen::setDisplayState(bool interactive, int32_t brightness) {
    auto status = checkCaller();
    if (!status.isOk()) return status;
    if (brightness < 0 || brightness > 255) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    {
        std::lock_guard lock(mMutex);
        brightness = std::max(10, brightness);
        if (mInteractive == interactive && mBrightness == brightness)
            return ndk::ScopedAStatus::ok();
        mInteractive = interactive;
        mBrightness = brightness;
    }
    wake();
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus DualScreen::drawCover(int32_t width, int32_t height,
                                         const std::vector<uint8_t>& pixels) {
    auto status = checkCaller();
    if (!status.isOk()) return status;
    {
        std::lock_guard lock(mMutex);
        if (!protocol::validDimensions(width, height) || width != mInfo.width ||
            height != mInfo.height || pixels.size() != size_t(width) * height) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        mFrame = protocol::packPixels(pixels);
    }
    wake();
    return ndk::ScopedAStatus::ok();
}

void DualScreen::disconnect() {
    mSerial.reset();
    mHid.reset();
    mUsbPath.clear();
    mReady = mInnerOn = mCoverOn = false;
    mAppliedBrightness = -1;
    mFailures = 0;
    mRecoveryPending = false;
    mNextAttempt = {};
    mNextRecovery = {};
    std::lock_guard lock(mMutex);
    if (mInfo.connected || mInfo.width) mNotify = true;
    mInfo = {};
    mFrame.clear();
}

void DualScreen::discover() {
    if (!mUsbPath.empty() && readNode(mUsbPath + "/idProduct") != "637a") disconnect();
    if (!mSerial.ok()) {
        std::unique_ptr<DIR, decltype(&closedir)> dir(opendir("/sys/class/tty"), closedir);
        if (dir) {
            while (auto* entry = readdir(dir.get())) {
                std::string name(entry->d_name);
                if (!android::base::StartsWith(name, "ttyACM")) continue;
                auto parent = usbParent("/sys/class/tty/" + name + "/device");
                if (parent.empty()) continue;
                unique_fd serial(
                    open(("/dev/" + name).c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC));
                if (!serial.ok()) continue;
                termios settings{};
                if (tcgetattr(serial.get(), &settings) != 0) continue;
                cfmakeraw(&settings);
                cfsetspeed(&settings, B115200);
                settings.c_cflag &= ~HUPCL;
                settings.c_cflag |= CLOCAL | CREAD;
                if (tcsetattr(serial.get(), TCSANOW, &settings) != 0) continue;
                mSerial = std::move(serial);
                mUsbPath = parent;
                if (!command("ATE0\r") || !command("AT%LOGSAVE=3\r") ||
                    !WriteStringToFile("0 1", kPd) || !command("AT%DS=0\r")) {
                    disconnect();
                    break;
                }
                std::this_thread::sleep_for(10ms);
                LOG(INFO) << "Dual-screen case attached at " << parent;
                break;
            }
        }
    }
    if (!mSerial.ok()) return;
    if (!mReady) {
        mReady = WriteStringToFile("1", kReady);
        if (!mReady) PLOG(WARNING) << "Dual-screen kernel handshake failed";
    }
    if (!mHid.ok()) {
        std::unique_ptr<DIR, decltype(&closedir)> dir(opendir("/sys/class/hidraw"), closedir);
        if (dir) {
            while (auto* entry = readdir(dir.get())) {
                std::string name(entry->d_name);
                if (!android::base::StartsWith(name, "hidraw")) continue;
                if (usbParent("/sys/class/hidraw/" + name + "/device") != mUsbPath) continue;
                unique_fd hid(open(("/dev/" + name).c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
                if (!hid.ok()) continue;
                hidraw_devinfo identity{};
                if (ioctl(hid.get(), HIDIOCGRAWINFO, &identity) < 0 || identity.vendor != 0x1004 ||
                    identity.product != 0x637a)
                    continue;
                std::array<uint8_t, 7> info{2};
                if (ioctl(hid.get(), HIDIOCGFEATURE(info.size()), info.data()) !=
                    int(info.size())) {
                    continue;
                }
                const int width = protocol::read16(info.data() + 1);
                const int height = protocol::read16(info.data() + 3);
                if (!protocol::validDimensions(width, height)) continue;
                mHid = std::move(hid);
                if (!powerCover(false)) {
                    mHid.reset();
                    continue;
                }
                std::lock_guard lock(mMutex);
                mInfo.width = width;
                mInfo.height = height;
                mNotify = true;
                LOG(INFO) << "Cover display: " << width << 'x' << height << ", format "
                          << int(info[5]);
                break;
            }
        }
    }
    const auto hall = readNode(kHall);
    const int coverState = hall == "1" ? 1 : hall == "5" ? 5 : 0;
    std::lock_guard lock(mMutex);
    if (mInfo.connected != mReady || mInfo.coverState != coverState) mNotify = true;
    mInfo.connected = mReady;
    mInfo.coverState = coverState;
}

bool DualScreen::command(const std::string& command, std::string* response) {
    if (!mSerial.ok()) return false;
    tcflush(mSerial.get(), TCIFLUSH);
    if (TEMP_FAILURE_RETRY(write(mSerial.get(), command.data(), command.size())) !=
        static_cast<ssize_t>(command.size()))
        return false;
    std::string result;
    const auto deadline = std::chrono::steady_clock::now() + 1500ms;
    while (result.size() < 4096) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                             deadline - std::chrono::steady_clock::now())
                             .count();
        if (remaining <= 0) break;
        pollfd fd{mSerial.get(), POLLIN, 0};
        if (TEMP_FAILURE_RETRY(poll(&fd, 1, int(remaining))) <= 0 ||
            (fd.revents & (POLLERR | POLLHUP | POLLNVAL)))
            break;
        std::array<char, 256> buffer;
        ssize_t count = TEMP_FAILURE_RETRY(read(mSerial.get(), buffer.data(), buffer.size()));
        if (count <= 0) break;
        result.append(buffer.data(), count);
        // The stock parser treats OK/ERROR as complete lines, not substrings.
        const auto lines = android::base::Split(result, "\n");
        for (size_t i = 0; i + 1 < lines.size(); ++i) {
            const auto line = Trim(lines[i]);
            if (line == "ERROR") return false;
            if (line == "OK") {
                if (response) *response = result;
                return true;
            }
        }
    }
    LOG(WARNING) << "Case command timed out: " << Trim(command);
    return false;
}

bool DualScreen::powerInner(bool enabled) {
    // Keep the bridge alive until composer has disconnected its display.
    if (!enabled && !WriteStringToFile("0 1", kPd)) return false;
    if (!command(enabled ? "AT%DS=1\r" : "AT%DS=0\r")) return false;
    if (enabled) {
        bool hpd = false;
        for (int attempt = 0; attempt < 10 && !hpd; ++attempt) {
            std::this_thread::sleep_for(50ms);
            std::string response;
            if (!command("AT%HPD\r", &response)) break;
            for (const auto& line : android::base::Split(response, "\n")) {
                if (Trim(line) == "On") hpd = true;
            }
        }
        if (!hpd) {
            LOG(WARNING) << "Case panel did not assert HPD after power-on";
            command("AT%DS=0\r");
            return false;
        }
    } else {
        std::this_thread::sleep_for(10ms);
    }
    if (enabled && !WriteStringToFile("1 1", kPd)) {
        PLOG(WARNING) << "Cannot set case DisplayPort HPD";
        return false;
    }
    // ds2_pd accepts writes before DP configuration but silently ignores them.
    // Confirm HPD actually changed before caching the panel as enabled.
    const auto hpdState = readNode(kPd);
    if (hpdState != (enabled ? "1" : "0")) {
        LOG(WARNING) << "Case DisplayPort HPD readback: " << hpdState;
        if (enabled) command("AT%DS=0\r");
        return false;
    }
    // Stock LpwgStatus is {lpwgMode, screenStatus}; normal input needs screenStatus=1.
    // TCPERF changes performance only and does not wake the touch controller.
    if (!command(enabled ? "AT%TCHNOTIFY=LPWG_NOTIFY,0,1\r" : "AT%TCHNOTIFY=LPWG_NOTIFY,0,0\r"))
        return false;
    mInnerOn = enabled;
    mAppliedBrightness = -1;
    command(enabled ? "AT%TCPERF=1\r" : "AT%TCPERF=0\r");
    return true;
}

bool DualScreen::powerCover(bool enabled) {
    std::array<uint8_t, 3> config{3, uint8_t(enabled ? 2 : 0), uint8_t(enabled ? 255 : 0)};
    if (ioctl(mHid.get(), HIDIOCSFEATURE(config.size()), config.data()) < 0) return false;
    mCoverOn = enabled;
    return true;
}

bool DualScreen::sendFrame(const std::vector<uint8_t>& pixels) {
    for (size_t offset = 0; offset < pixels.size(); offset += protocol::kPayloadSize) {
        const auto size = std::min(protocol::kPayloadSize, pixels.size() - offset);
        if (!writeReport(mHid.get(), protocol::pixelReport(offset, pixels.data() + offset, size))) {
            return false;
        }
    }
    // A zero-length report commits the completed frame; never commit a partial transfer.
    return writeReport(mHid.get(), protocol::pixelReport(0, nullptr, 0));
}

void DualScreen::update() {
    if (!mReady) return;
    bool interactive;
    int brightness;
    CaseInfo info;
    {
        std::lock_guard lock(mMutex);
        interactive = mInteractive;
        brightness = mBrightness;
        info = mInfo;
    }
    const bool inner = interactive && info.coverState == 0;
    bool success = true;
    if (inner != mInnerOn) {
        if (std::chrono::steady_clock::now() < mNextAttempt) return;
        success = powerInner(inner);
        mNextAttempt = success ? std::chrono::steady_clock::time_point{}
                               : std::chrono::steady_clock::now() + 1s;
    }
    if (success && mInnerOn && brightness != mAppliedBrightness) {
        success = command("AT%DB=" + std::to_string(brightness) + "\r");
        if (success) mAppliedBrightness = brightness;
    }
    const bool cover = info.coverState == 1;
    if (mHid.ok()) {
        if (cover != mCoverOn && !powerCover(cover)) {
            mHid.reset();
            mCoverOn = false;
        }
        if (mCoverOn) {
            std::vector<uint8_t> frame;
            {
                std::lock_guard lock(mMutex);
                frame.swap(mFrame);
            }
            if (!frame.empty() && !sendFrame(frame)) {
                std::lock_guard lock(mMutex);
                if (mFrame.empty()) mFrame = std::move(frame);
                mHid.reset();
                mCoverOn = false;
            }
        }
    }
    if (success) {
        mFailures = 0;
    } else if (++mFailures >= 3) {
        // Reopen control interfaces after bounded retries. Do not trigger the kernel's
        // ds2_recovery attribute: its stock driver contains a configurable BUG_ON.
        LOG(WARNING) << "Reopening unresponsive case control interfaces";
        disconnect();
    }
}

void DualScreen::notify() {
    std::shared_ptr<ICaseCallback> callback;
    CaseInfo info;
    {
        std::lock_guard lock(mMutex);
        if (!mNotify || !mCallback) return;
        mNotify = false;
        callback = mCallback;
        info = mInfo;
    }
    if (!callback->onCaseChanged(info).isOk()) {
        std::lock_guard lock(mMutex);
        if (mCallback == callback) {
            if (mClientDeath) {
                AIBinder_unlinkToDeath(callback->asBinder().get(), mDeath.get(), mClientDeath);
                mClientDeath = nullptr;
            }
            mCallback.reset();
            mInteractive = false;
        }
        wake();
    }
}

void DualScreen::run() {
    unique_fd uevents(uevent_open_socket(64 * 1024, true));
    if (uevents.ok())
        fcntl(uevents.get(), F_SETFL, O_NONBLOCK);
    else
        PLOG(ERROR) << "Cannot listen for case hotplug events";
    bool rescan = true;
    while (true) {
        {
            std::lock_guard lock(mMutex);
            if (mStopping) break;
        }
        if (rescan || (mRecoveryPending && std::chrono::steady_clock::now() >= mNextRecovery)) {
            // Hold suspend only for the bounded USB transactions, including alarm-driven frames.
            WriteStringToFile("lge-dualscreen 30000000000", "/sys/power/wake_lock");
            if (mRecoveryPending && std::chrono::steady_clock::now() >= mNextRecovery) {
                // Disconnect composer before retraining; repeated HPD-high resets leave stale
                // frames.
                if (mSerial.ok() && powerInner(false)) {
                    mReady = false;
                    mRecoveryPending = false;
                    mNextRecovery = std::chrono::steady_clock::now() + 5s;
                    mNextAttempt = {};
                } else {
                    mNextRecovery = std::chrono::steady_clock::now() + 1s;
                }
            }
            discover();
            update();
            notify();
            WriteStringToFile("lge-dualscreen", "/sys/power/wake_unlock");
        }
        rescan = false;
        pollfd fds[] = {{mWake.get(), POLLIN, 0}, {uevents.get(), POLLIN, 0}};
        // A fallback rescan also handles a case already attached when the HAL starts.
        const int result = TEMP_FAILURE_RETRY(poll(
            fds, 2,
            mRecoveryPending || ((!mReady || !mHid.ok() || mFailures) && mSerial.ok()) ? 1000
                                                                                       : 30000));
        if (result < 0) {
            PLOG(ERROR) << "Case event poll failed";
            break;
        }
        if (result == 0) rescan = true;
        if (fds[0].revents & POLLIN) {
            rescan = true;
            uint64_t value;
            TEMP_FAILURE_RETRY(read(mWake.get(), &value, sizeof(value)));
        }
        if (fds[1].revents & POLLIN) {
            std::array<char, 4096> event;
            ssize_t count;
            while ((count = uevent_kernel_multicast_recv(uevents.get(), event.data(),
                                                         event.size())) > 0) {
                std::string message(event.data(), count);
                if (message.find("SWITCH_NAME=cover_recovery") != std::string::npos &&
                    message.find("SWITCH_STATE=1") != std::string::npos) {
                    mRecoveryPending = true;
                }
                // Ignore unrelated battery, graphics and wake-lock uevents.
                if (message.find("SUBSYSTEM=usb") != std::string::npos ||
                    message.find("SUBSYSTEM=tty") != std::string::npos ||
                    message.find("SUBSYSTEM=hidraw") != std::string::npos ||
                    message.find("SWITCH_NAME=smartcover") != std::string::npos) {
                    rescan = true;
                }
            }
        }
    }
    if (mInnerOn) powerInner(false);
    if (mCoverOn) powerCover(false);
    WriteStringToFile("lge-dualscreen", "/sys/power/wake_unlock");
}

}  // namespace aidl::vendor::lge::hardware::dualscreen
