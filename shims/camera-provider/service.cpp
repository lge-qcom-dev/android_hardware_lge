/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "LgeCameraProviderShim"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>

#include <set>
#include <string>
#include <utility>

#include <binder/ProcessState.h>
#include <cutils/properties.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

#include <CameraProvider_2_4.h>
#include <LegacyCameraProviderImpl_2_4.h>

namespace android::hardware::camera::provider::V2_4::implementation {

namespace {

constexpr char kBlacklistRangeProperty[] =
        "ro.vendor.camera.provider.blacklist_range";
constexpr char kBlacklistIdsProperty[] =
        "ro.vendor.camera.provider.blacklist_ids";

std::string getProperty(const char* name) {
    char value[PROPERTY_VALUE_MAX] = {};
    property_get(name, value, "");
    return value;
}

bool parseCameraId(const std::string& value, int* cameraId) {
    char* end = nullptr;
    errno = 0;
    const long parsedId = strtol(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0' || parsedId < 0 ||
            parsedId > INT_MAX) {
        return false;
    }

    *cameraId = parsedId;
    return true;
}

class CameraIdBlacklist {
  public:
    CameraIdBlacklist() {
        const std::string ids = getProperty(kBlacklistIdsProperty);
        const std::string range = getProperty(kBlacklistRangeProperty);

        if (!ids.empty()) {
            if (!range.empty()) {
                ALOGW("Both blacklist properties are set; using %s", kBlacklistIdsProperty);
            }
            parseIds(ids);
        } else if (!range.empty()) {
            parseRange(range);
        }
    }

    bool contains(int cameraId) const {
        return mCameraIds.count(cameraId) != 0;
    }

    const std::set<int>& ids() const {
        return mCameraIds;
    }

  private:
    void parseIds(const std::string& value) {
        std::set<int> ids;
        size_t start = 0;
        while (start <= value.size()) {
            const size_t end = value.find(',', start);
            const std::string id = value.substr(start, end - start);
            int parsedId;
            if (!parseCameraId(id, &parsedId)) {
                ALOGE("Invalid camera ID list in %s: %s", kBlacklistIdsProperty,
                        value.c_str());
                return;
            }
            ids.insert(parsedId);
            if (end == std::string::npos) {
                mCameraIds = std::move(ids);
                ALOGI("Filtering camera IDs from %s: %s", kBlacklistIdsProperty,
                        value.c_str());
                return;
            }
            start = end + 1;
        }
    }

    void parseRange(const std::string& value) {
        const size_t separator = value.find('-');
        if (separator == std::string::npos || separator != value.rfind('-')) {
            ALOGE("Invalid camera ID range in %s: %s", kBlacklistRangeProperty,
                    value.c_str());
            return;
        }

        int firstId;
        int lastId;
        if (!parseCameraId(value.substr(0, separator), &firstId) ||
                !parseCameraId(value.substr(separator + 1), &lastId) || firstId > lastId) {
            ALOGE("Invalid camera ID range in %s: %s", kBlacklistRangeProperty,
                    value.c_str());
            return;
        }

        for (int cameraId = firstId;; cameraId++) {
            mCameraIds.insert(cameraId);
            if (cameraId == lastId) {
                break;
            }
        }
        ALOGI("Filtering camera IDs from %s: %s", kBlacklistRangeProperty,
                value.c_str());
    }

    std::set<int> mCameraIds;
};

const CameraIdBlacklist& cameraIdBlacklist() {
    static const CameraIdBlacklist blacklist;
    return blacklist;
}

struct LgeCameraProviderImpl : public LegacyCameraProviderImpl_2_4 {
    LgeCameraProviderImpl() : LegacyCameraProviderImpl_2_4() {
        camera_device_status_change = cameraDeviceStatusChange;
        torch_mode_status_change = torchModeStatusChange;

        for (int cameraId : cameraIdBlacklist().ids()) {
            mCameraStatusMap.erase(std::to_string(cameraId));
        }
    }

    static void cameraDeviceStatusChange(const camera_module_callbacks_t* callbacks,
            int cameraId, int newStatus) {
        if (cameraIdBlacklist().contains(cameraId)) {
            ALOGI("Suppressing camera device status for ID %d", cameraId);
            return;
        }

        sCameraDeviceStatusChange(callbacks, cameraId, newStatus);
    }

    static void torchModeStatusChange(const camera_module_callbacks_t* callbacks,
            const char* cameraId, int newStatus) {
        int parsedId;
        if (cameraId != nullptr && parseCameraId(cameraId, &parsedId) &&
                cameraIdBlacklist().contains(parsedId)) {
            ALOGI("Suppressing torch status for ID %s", cameraId);
            return;
        }

        sTorchModeStatusChange(callbacks, cameraId, newStatus);
    }
};

}  // namespace

}  // namespace android::hardware::camera::provider::V2_4::implementation

int main() {
    using android::OK;
    using android::ProcessState;
    using android::sp;
    using android::hardware::configureRpcThreadpool;
    using android::hardware::joinRpcThreadpool;
    using android::hardware::camera::provider::V2_4::ICameraProvider;
    using android::hardware::camera::provider::V2_4::implementation::CameraProvider;
    using android::hardware::camera::provider::V2_4::implementation::LgeCameraProviderImpl;

    ProcessState::initWithDriver("/dev/vndbinder");
    configureRpcThreadpool(6, true);

    sp<ICameraProvider> provider = new CameraProvider<LgeCameraProviderImpl>();
    if (provider->registerAsService("legacy/0") != OK) {
        ALOGE("Failed to register the filtered camera provider");
        return EXIT_FAILURE;
    }

    joinRpcThreadpool();
    return EXIT_FAILURE;
}
