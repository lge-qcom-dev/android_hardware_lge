/*
 * Copyright (C) 2024 The LineageOS Project
 *               2024 Paranoid Android
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "Fingerprint.h"

#include <android-base/properties.h>
#include <fingerprint.sysprop.h>
#include <util/Util.h>

#include <android-base/logging.h>
#include <android-base/strings.h>

namespace aidl::android::hardware::biometrics::fingerprint {

namespace {
constexpr int MAX_ENROLLMENTS_PER_USER = 5;
constexpr char HW_COMPONENT_ID[] = "fingerprintSensor";
constexpr char HW_VERSION[] = "vendor/model/revision";
constexpr char FW_VERSION[] = "1.01";
constexpr char SERIAL_NUMBER[] = "00000001";
constexpr char SW_COMPONENT_ID[] = "matchingAlgorithm";
constexpr char SW_VERSION[] = "vendor/version/revision";
}  // namespace

static Fingerprint* sInstance;

Fingerprint::Fingerprint(std::shared_ptr<FingerprintConfig> config)
    : mConfig(std::move(config)), mDevice(openHal()) {
    sInstance = this;  // keep track of the most recent instance

    std::string sensorTypeProp = mConfig->get<std::string>("type");
    if (sensorTypeProp == "side") {
        mSensorType = FingerprintSensorType::POWER_BUTTON;
    } else if (sensorTypeProp == "home") {
        mSensorType = FingerprintSensorType::HOME_BUTTON;
    } else if (sensorTypeProp == "rear") {
        mSensorType = FingerprintSensorType::REAR;
    } else {
        mSensorType = FingerprintSensorType::UNKNOWN;
        UNIMPLEMENTED(FATAL) << "unrecognized or unimplemented fingerprint behavior: "
                             << sensorTypeProp;
    }
    ALOGI("sensorTypeProp: %s", sensorTypeProp.c_str());
}

Fingerprint::~Fingerprint() {
    ALOGV("~Fingerprint()");
    if (mDevice == nullptr) {
        ALOGE("No valid device");
        return;
    }
    int err;
    if (0 != (err = mDevice->rbs_uninitialize())) {
        ALOGE("Can't close fingerprint module, error: %d", err);
        return;
    }
    free(mDevice);
}

rbs_fingerprint_device_t* Fingerprint::openHal() {
    int err = 0;
    rbs_fingerprint_device_t* fp_device = RBS_LoadLibrary();
    if (0 != (err = fp_device->rbs_set_on_callback_proc((void*)Fingerprint::notify))) {
        ALOGE("Can't register fingerprint module callback, error: %d", err);
        return nullptr;
    }

    return fp_device;
}

SensorLocation Fingerprint::getSensorLocation() {
    SensorLocation location;

    auto loc = mConfig->get<std::string>("sensor_location");
    auto isValidStr = false;
    auto dim = ::android::base::Split(loc, "|");

    if (dim.size() != 3 and dim.size() != 4) {
        if (!loc.empty()) {
            ALOGE("Invalid sensor location input (x|y|radius) or (x|y|radius|display): %s",
                  loc.c_str());
        }
    } else {
        int32_t x, y, r;
        std::string d;
        isValidStr = ParseInt(dim[0], &x) && ParseInt(dim[1], &y) && ParseInt(dim[2], &r);
        if (dim.size() == 4) {
            d = dim[3];
            isValidStr = isValidStr && !d.empty();
        }
        if (isValidStr)
            location = {
                    .sensorLocationX = x, .sensorLocationY = y, .sensorRadius = r, .display = d};
    }

    return location;
}

void Fingerprint::notify(uint32_t eventId, uint32_t value1, uint32_t value2, void* buffer,
                                   uint32_t buffer_size) {
    Fingerprint* thisPtr = sInstance;
    if (thisPtr == nullptr || thisPtr->mSession == nullptr || thisPtr->mSession->isClosed()) {
        ALOGE("Receiving callbacks before a session is opened.");
        return;
    }
    thisPtr->mSession->notify(eventId, value1, value2, buffer, buffer_size);
}

ndk::ScopedAStatus Fingerprint::getSensorProps(std::vector<SensorProps>* out) {
    std::vector<common::ComponentInfo> componentInfo = {
            {HW_COMPONENT_ID, HW_VERSION, FW_VERSION, SERIAL_NUMBER, "" /* softwareVersion */},
            {SW_COMPONENT_ID, "" /* hardwareVersion */, "" /* firmwareVersion */,
             "" /* serialNumber */, SW_VERSION}};
    auto sensorId = mConfig->get<std::int32_t>("sensor_id");
    auto sensorStrength = mConfig->get<std::int32_t>("sensor_strength");
    auto navigationGuesture = mConfig->get<bool>("navigation_gesture");
    auto detectInteraction = mConfig->get<bool>("detect_interaction");

    common::CommonProps commonProps = {sensorId, (common::SensorStrength)sensorStrength,
                                       MAX_ENROLLMENTS_PER_USER, componentInfo};

    SensorLocation sensorLocation = getSensorLocation();

    ALOGI("sensor type: %s, location: %s", ::android::internal::ToString(mSensorType).c_str(),
          sensorLocation.toString().c_str());

    *out = {{commonProps,
             mSensorType,
             {sensorLocation},
             navigationGuesture,
             detectInteraction,
             false,
             false,
             std::nullopt}};
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Fingerprint::createSession(int32_t /*sensorId*/, int32_t userId,
                                              const std::shared_ptr<ISessionCallback>& cb,
                                              std::shared_ptr<ISession>* out) {
    CHECK(mSession == nullptr || mSession->isClosed()) << "Open session already exists!";

    mSession = SharedRefBase::make<Session>(mDevice, userId, cb, mLockoutTracker);
    *out = mSession;

    mSession->linkToDeath(cb->asBinder().get());

    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::biometrics::fingerprint
