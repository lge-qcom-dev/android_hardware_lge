/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once
#include <libradiocompat/CallbackManager.h>
#include <vendor/lge/hardware/lgdata/1.0/types.h>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include "QosTracker.h"
#include "RadioIndication.h"
#include "RadioResponse.h"

namespace lge::radio {
namespace compat = ::android::hardware::radio::compat;
namespace hidl = ::android::hardware::radio;
namespace data = ::aidl::android::hardware::radio::data;
using ::android::hardware::hidl_vec;
using ::android::hardware::Return;

class RadioAdapter {
  public:
    RadioAdapter();
    ~RadioAdapter();
    void post(std::function<void()> work);
    void qos(const vendor::lge::hardware::lgdata::V1_0::LgDataQosResponse& event);
    void setEps(bool eps);
    void list(std::vector<data::SetupDataCallResult> calls);
    void setup(data::SetupDataCallResult call);
    void reset();
    void enrich(data::SetupDataCallResult& call) const;
    void emit();
    const std::vector<data::SetupDataCallResult>& calls() const { return mCalls; }
    std::function<void(const std::vector<data::SetupDataCallResult>&)> onList;

  private:
    void applyQos(int32_t cid, const vendor::lge::hardware::lgdata::V1_0::LgDataQosResponse& event);
    std::mutex mMutex;
    std::condition_variable mCv;
    bool mStopping = false;
    std::deque<std::function<void()>> mWork;
    std::thread mWorker;
    bool mEps = false;
    std::map<std::pair<std::string, int32_t>,
             std::pair<vendor::lge::hardware::lgdata::V1_0::LgDataQosResponse, int64_t>>
            mEarly;
    std::set<std::string> mRetiredInterfaces;
    QosTracker mTracker;
    std::vector<data::SetupDataCallResult> mCalls;
};

class DataIndication : public hidl::implementation::RadioIndication {
  public:
    explicit DataIndication(std::shared_ptr<RadioAdapter> adapter);
    void setCallbackManager(std::weak_ptr<compat::CallbackManager> callbacks);
    Return<void> dataCallListChanged(
            hidl::V1_0::RadioIndicationType type,
            const hidl_vec<hidl::V1_0::SetupDataCallResult>& calls) override;
    Return<void> dataCallListChanged_1_4(
            hidl::V1_0::RadioIndicationType type,
            const hidl_vec<hidl::V1_4::SetupDataCallResult>& calls) override;
    Return<void> dataCallListChanged_1_5(
            hidl::V1_0::RadioIndicationType type,
            const hidl_vec<hidl::V1_5::SetupDataCallResult>& calls) override;
    Return<void> radioStateChanged(hidl::V1_0::RadioIndicationType type,
                                   hidl::V1_0::RadioState state) override;
    Return<void> simStatusChanged(hidl::V1_0::RadioIndicationType type) override;

  private:
    std::shared_ptr<data::IRadioDataIndication> dataCb();
    std::weak_ptr<compat::CallbackManager> mCallbacks;
    std::shared_ptr<RadioAdapter> mAdapter;
};
class DataResponse : public hidl::implementation::RadioResponse {
  public:
    explicit DataResponse(std::shared_ptr<RadioAdapter> adapter);
    void setCallbackManager(std::weak_ptr<compat::CallbackManager> callbacks);
    Return<void> getDataCallListResponse(
            const hidl::V1_0::RadioResponseInfo& info,
            const hidl_vec<hidl::V1_0::SetupDataCallResult>& calls) override;
    Return<void> setupDataCallResponse(const hidl::V1_0::RadioResponseInfo& info,
                                       const hidl::V1_0::SetupDataCallResult& call) override;
    Return<void> getDataCallListResponse_1_4(
            const hidl::V1_0::RadioResponseInfo& info,
            const hidl_vec<hidl::V1_4::SetupDataCallResult>& calls) override;
    Return<void> getDataCallListResponse_1_5(
            const hidl::V1_0::RadioResponseInfo& info,
            const hidl_vec<hidl::V1_5::SetupDataCallResult>& calls) override;
    Return<void> getDataRegistrationStateResponse_1_5(
            const hidl::V1_0::RadioResponseInfo& info,
            const hidl::V1_5::RegStateResult& result) override;
    Return<void> setupDataCallResponse_1_4(const hidl::V1_0::RadioResponseInfo& info,
                                           const hidl::V1_4::SetupDataCallResult& call) override;
    Return<void> setupDataCallResponse_1_5(const hidl::V1_0::RadioResponseInfo& info,
                                           const hidl::V1_5::SetupDataCallResult& call) override;

  private:
    std::shared_ptr<data::IRadioDataResponse> dataCb();
    std::weak_ptr<compat::CallbackManager> mCallbacks;
    std::shared_ptr<RadioAdapter> mAdapter;
};
}  // namespace lge::radio
