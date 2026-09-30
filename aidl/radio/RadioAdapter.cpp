/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include "RadioAdapter.h"
#include <android-base/logging.h>
#include <algorithm>
#include <chrono>
#include <limits>
#include "Helpers.h"

namespace lge::radio {
namespace {
int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
}
QosTracker::Call identity(const data::SetupDataCallResult& call) {
    std::string key = call.ifname;
    std::vector<std::string> addresses;
    for (const auto& address : call.addresses) addresses.push_back(address.address);
    std::sort(addresses.begin(), addresses.end());
    for (const auto& address : addresses) key += "|" + address;
    return {call.cid, key, call.active != 0};
}
data::QosSession toAidl(const Session& session) {
    data::QosSession result;
    result.qosSessionId = session.id;
    data::EpsQos eps;
    eps.qci = session.qci;
    eps.uplink.maxBitrateKbps = session.uplink.maximumKbps;
    eps.uplink.guaranteedBitrateKbps = session.uplink.guaranteedKbps;
    eps.downlink.maxBitrateKbps = session.downlink.maximumKbps;
    eps.downlink.guaranteedBitrateKbps = session.downlink.guaranteedKbps;
    result.qos.set<data::Qos::Tag::eps>(eps);
    for (const auto& f : session.filters) {
        data::QosFilter out;
        out.localAddresses = f.localAddresses;
        out.remoteAddresses = f.remoteAddresses;
        if (f.localPort) out.localPort = data::PortRange{f.localPort->start, f.localPort->end};
        if (f.remotePort) out.remotePort = data::PortRange{f.remotePort->start, f.remotePort->end};
        out.protocol = f.protocol;
        out.direction = f.direction;
        out.precedence = f.precedence;
        if (f.tos) out.tos.set<data::QosFilterTypeOfService::Tag::value>(*f.tos);
        if (f.flowLabel) out.flowLabel.set<data::QosFilterIpv6FlowLabel::Tag::value>(*f.flowLabel);
        if (f.spi) out.spi.set<data::QosFilterIpsecSpi::Tag::value>(*f.spi);
        result.qosFilters.push_back(std::move(out));
    }
    return result;
}
// HIDL 1.5 has no bearer fields. Convert only its data-call contract locally.
data::SetupDataCallResult toAidl(const hidl::V1_5::SetupDataCallResult& call) {
    data::SetupDataCallResult out;
    out.cause = static_cast<data::DataCallFailCause>(call.cause);
    out.suggestedRetryTime = call.suggestedRetryTime;
    out.cid = call.cid;
    out.active = static_cast<int32_t>(call.active);
    out.type = static_cast<data::PdpProtocolType>(call.type);
    out.ifname = call.ifname;
    for (const auto& address : call.addresses) {
        data::LinkAddress converted;
        converted.address = address.address;
        converted.addressProperties = address.properties;
        converted.deprecationTime = static_cast<int64_t>(address.deprecationTime);
        converted.expirationTime = static_cast<int64_t>(address.expirationTime);
        out.addresses.push_back(std::move(converted));
    }
    for (const auto& address : call.dnses) out.dnses.push_back(address);
    for (const auto& address : call.gateways) out.gateways.push_back(address);
    for (const auto& address : call.pcscf) out.pcscf.push_back(address);
    out.mtuV4 = call.mtuV4;
    out.mtuV6 = call.mtuV6;
    return out;
}
hidl::V1_5::SetupDataCallResult upgrade(const hidl::V1_4::SetupDataCallResult& call) {
    hidl::V1_5::SetupDataCallResult out{};
    out.cause = call.cause;
    out.suggestedRetryTime = call.suggestedRetryTime;
    out.cid = call.cid;
    out.active = call.active;
    out.type = call.type;
    out.ifname = call.ifname;
    out.addresses.resize(call.addresses.size());
    for (size_t i = 0; i < call.addresses.size(); ++i) {
        out.addresses[i].address = call.addresses[i];
        out.addresses[i].deprecationTime = std::numeric_limits<uint64_t>::max();
        out.addresses[i].expirationTime = std::numeric_limits<uint64_t>::max();
    }
    out.dnses = call.dnses;
    out.gateways = call.gateways;
    out.pcscf = call.pcscf;
    out.mtuV4 = call.mtu;
    out.mtuV6 = call.mtu;
    return out;
}
::aidl::android::hardware::radio::RadioResponseInfo toAidl(
        const hidl::V1_0::RadioResponseInfo& info) {
    ::aidl::android::hardware::radio::RadioResponseInfo out;
    out.serial = info.serial;
    out.error = static_cast<::aidl::android::hardware::radio::RadioError>(info.error);
    out.type = static_cast<::aidl::android::hardware::radio::RadioResponseType>(info.type);
    return out;
}
}  // namespace
RadioAdapter::RadioAdapter()
    : mWorker([this] {
          for (;;) {
              std::function<void()> work;
              {
                  std::unique_lock lock(mMutex);
                  mCv.wait(lock, [this] { return mStopping || !mWork.empty(); });
                  if (mStopping && mWork.empty()) return;
                  work = std::move(mWork.front());
                  mWork.pop_front();
              }
              work();  // No state mutex held over binder callbacks.
          }
      }) {}
RadioAdapter::~RadioAdapter() {
    {
        std::lock_guard lock(mMutex);
        mStopping = true;
    }
    mCv.notify_one();
    mWorker.join();
}
void RadioAdapter::post(std::function<void()> work) {
    {
        std::lock_guard lock(mMutex);
        CHECK_LT(mWork.size(), 256u) << "LG radio callback queue overflow";
        mWork.push_back(std::move(work));
    }
    mCv.notify_one();
}
void RadioAdapter::enrich(data::SetupDataCallResult& call) const {
    call.qosSessions.clear();
    for (const auto& session : mTracker.sessions(call.cid))
        call.qosSessions.push_back(toAidl(session));
}
void RadioAdapter::list(std::vector<data::SetupDataCallResult> calls) {
    std::vector<QosTracker::Call> identities;
    for (const auto& call : calls) identities.push_back(identity(call));
    mTracker.updateCalls(identities, nowMs());
    for (const auto& old : mCalls) {
        if (std::none_of(calls.begin(), calls.end(), [&](const auto& call) {
                return call.active != 0 && call.ifname == old.ifname;
            }))
            mRetiredInterfaces.insert(old.ifname);
    }
    mCalls = std::move(calls);
    std::vector<vendor::lge::hardware::lgdata::V1_0::LgDataQosResponse> early;
    for (auto it = mEarly.begin(); it != mEarly.end();) {
        bool found = std::any_of(mCalls.begin(), mCalls.end(), [&](const auto& call) {
            return call.active != 0 && call.ifname == it->first.first;
        });
        if (found || it->second.second <= nowMs()) {
            if (found && !mRetiredInterfaces.count(it->first.first))
                early.push_back(it->second.first);
            it = mEarly.erase(it);
        } else
            ++it;
    }
    for (const auto& call : mCalls)
        if (call.active != 0) mRetiredInterfaces.erase(call.ifname);
    for (const auto& event : early) qos(event);
    for (auto& call : mCalls) enrich(call);
}
void RadioAdapter::setup(data::SetupDataCallResult call) {
    mTracker.setup(identity(call), nowMs());
    mCalls.erase(std::remove_if(mCalls.begin(), mCalls.end(),
                                [&](const auto& existing) { return existing.cid == call.cid; }),
                 mCalls.end());
    enrich(call);
    mCalls.push_back(std::move(call));
}
void RadioAdapter::reset() {
    mEps = false;
    mEarly.clear();
    mRetiredInterfaces.clear();
    mTracker.clear();
    emit();
    mCalls.clear();
}
void RadioAdapter::setEps(bool eps) {
    if (mEps && !eps) {
        mTracker.clear();
        mEarly.clear();
        emit();
    }
    mEps = eps;
}
void RadioAdapter::qos(const vendor::lge::hardware::lgdata::V1_0::LgDataQosResponse& event) {
    LOG(DEBUG) << "LG data QoS interface=" << event.dev_name << " qid=" << event.qid
               << " status=" << event.status;
    auto call = std::find_if(mCalls.begin(), mCalls.end(), [&](const auto& call) {
        return call.active != 0 && call.ifname == event.dev_name.c_str();
    });
    if (call == mCalls.end()) {
        if (mEarly.size() < 32 && !mRetiredInterfaces.count(event.dev_name.c_str()))
            mEarly[{event.dev_name.c_str(), event.qid}] = {event, nowMs() + 5000};
        return;
    }
    applyQos(call->cid, event);
}
void RadioAdapter::emit() {
    for (auto& call : mCalls) enrich(call);
    if (!mCalls.empty() && onList) onList(mCalls);
}
void RadioAdapter::applyQos(int32_t cid,
                            const vendor::lge::hardware::lgdata::V1_0::LgDataQosResponse& event) {
    // The stock serializer can encode LTE QCI or 5G 5QI with no discriminator.
    // Only expose EPS sessions while the standard radio reports an LTE anchor.
    if (!mEps) {
        LOG(WARNING) << "Ignoring ambiguous LG QoS outside LTE registration";
        return;
    }
    if (event.status < 1 || event.status > 6) {
        LOG(WARNING) << "Unknown LG QoS status " << event.status;
        return;
    }
    bool payload = !event.tx_flow_desc.empty() || !event.rx_flow_desc.empty() ||
                   !event.tx_tft.empty() || !event.rx_tft.empty();
    auto session = payload ? parseQos(event.qid, event.tx_flow_desc, event.rx_flow_desc,
                                      event.tx_tft, event.rx_tft)
                           : std::nullopt;
    if (payload && !session)
        LOG(WARNING) << "Rejected LG QoS payload cid=" << cid << " qid=" << event.qid;
    mTracker.event(cid, event.qid, static_cast<QosEvent>(event.status), std::move(session), payload,
                   nowMs());
    LOG(INFO) << "LG bearer update cid=" << cid << " qid=" << event.qid
              << " status=" << event.status << " sessions=" << mTracker.sessions(cid).size();
    emit();
}
DataIndication::DataIndication(std::shared_ptr<RadioAdapter> adapter)
    : mAdapter(std::move(adapter)) {
    mAdapter->onList = [this](const auto& calls) {
        dataCb()->dataCallListChanged(
                ::aidl::android::hardware::radio::RadioIndicationType::UNSOLICITED, calls);
    };
}
void DataIndication::setCallbackManager(std::weak_ptr<compat::CallbackManager> callbacks) {
    mCallbacks = std::move(callbacks);
}
std::shared_ptr<data::IRadioDataIndication> DataIndication::dataCb() {
    auto callbacks = mCallbacks.lock();
    CHECK(callbacks);
    return callbacks->indication().dataCb();
}
Return<void> DataIndication::dataCallListChanged(
        hidl::V1_0::RadioIndicationType type,
        const hidl_vec<hidl::V1_0::SetupDataCallResult>& calls) {
    hidl_vec<hidl::V1_4::SetupDataCallResult> converted;
    converted.resize(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) converted[i] = Create1_4SetupDataCallResult(calls[i]);
    return dataCallListChanged_1_4(type, converted);
}
Return<void> DataIndication::dataCallListChanged_1_4(
        hidl::V1_0::RadioIndicationType type,
        const hidl_vec<hidl::V1_4::SetupDataCallResult>& calls) {
    hidl_vec<hidl::V1_5::SetupDataCallResult> converted;
    converted.resize(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) converted[i] = upgrade(calls[i]);
    return dataCallListChanged_1_5(type, converted);
}
Return<void> DataIndication::dataCallListChanged_1_5(
        hidl::V1_0::RadioIndicationType type,
        const hidl_vec<hidl::V1_5::SetupDataCallResult>& calls) {
    std::vector<data::SetupDataCallResult> converted;
    for (const auto& call : calls) converted.push_back(toAidl(call));
    mAdapter->post([this, type, calls = std::move(converted)]() mutable {
        mAdapter->list(std::move(calls));
        dataCb()->dataCallListChanged(
                static_cast<::aidl::android::hardware::radio::RadioIndicationType>(type),
                mAdapter->calls());
    });
    return {};
}
Return<void> DataIndication::radioStateChanged(hidl::V1_0::RadioIndicationType type,
                                               hidl::V1_0::RadioState state) {
    if (state != hidl::V1_0::RadioState::ON) mAdapter->post([this] { mAdapter->reset(); });
    return hidl::implementation::RadioIndication::radioStateChanged(type, state);
}
Return<void> DataIndication::simStatusChanged(hidl::V1_0::RadioIndicationType type) {
    mAdapter->post([this] { mAdapter->reset(); });
    return hidl::implementation::RadioIndication::simStatusChanged(type);
}
DataResponse::DataResponse(std::shared_ptr<RadioAdapter> adapter) : mAdapter(std::move(adapter)) {}
void DataResponse::setCallbackManager(std::weak_ptr<compat::CallbackManager> callbacks) {
    mCallbacks = std::move(callbacks);
}
std::shared_ptr<data::IRadioDataResponse> DataResponse::dataCb() {
    auto callbacks = mCallbacks.lock();
    CHECK(callbacks);
    return callbacks->response().dataCb();
}
Return<void> DataResponse::getDataCallListResponse(
        const hidl::V1_0::RadioResponseInfo& info,
        const hidl_vec<hidl::V1_0::SetupDataCallResult>& calls) {
    hidl_vec<hidl::V1_4::SetupDataCallResult> converted;
    converted.resize(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) converted[i] = Create1_4SetupDataCallResult(calls[i]);
    return getDataCallListResponse_1_4(info, converted);
}
Return<void> DataResponse::setupDataCallResponse(const hidl::V1_0::RadioResponseInfo& info,
                                                 const hidl::V1_0::SetupDataCallResult& call) {
    return setupDataCallResponse_1_4(info, Create1_4SetupDataCallResult(call));
}
Return<void> DataResponse::getDataCallListResponse_1_4(
        const hidl::V1_0::RadioResponseInfo& info,
        const hidl_vec<hidl::V1_4::SetupDataCallResult>& calls) {
    hidl_vec<hidl::V1_5::SetupDataCallResult> converted;
    converted.resize(calls.size());
    for (size_t i = 0; i < calls.size(); ++i) converted[i] = upgrade(calls[i]);
    return getDataCallListResponse_1_5(info, converted);
}
Return<void> DataResponse::getDataCallListResponse_1_5(
        const hidl::V1_0::RadioResponseInfo& info,
        const hidl_vec<hidl::V1_5::SetupDataCallResult>& calls) {
    std::vector<data::SetupDataCallResult> converted;
    for (const auto& call : calls) converted.push_back(toAidl(call));
    mAdapter->post([this, info, calls = std::move(converted)]() mutable {
        if (info.error == hidl::V1_0::RadioError::NONE) mAdapter->list(calls);
        for (auto& call : calls) mAdapter->enrich(call);
        dataCb()->getDataCallListResponse(toAidl(info), calls);
    });
    return {};
}
Return<void> DataResponse::setupDataCallResponse_1_4(const hidl::V1_0::RadioResponseInfo& info,
                                                     const hidl::V1_4::SetupDataCallResult& call) {
    return setupDataCallResponse_1_5(info, upgrade(call));
}
Return<void> DataResponse::setupDataCallResponse_1_5(const hidl::V1_0::RadioResponseInfo& info,
                                                     const hidl::V1_5::SetupDataCallResult& call) {
    mAdapter->post([this, info, call = toAidl(call)]() mutable {
        if (info.error == hidl::V1_0::RadioError::NONE &&
            call.cause == data::DataCallFailCause::NONE && call.active != 0)
            mAdapter->setup(call);
        mAdapter->enrich(call);
        dataCb()->setupDataCallResponse(toAidl(info), call);
    });
    return {};
}
Return<void> DataResponse::getDataRegistrationStateResponse_1_5(
        const hidl::V1_0::RadioResponseInfo& info, const hidl::V1_5::RegStateResult& result) {
    mAdapter->post([this, info, result] {
        if (info.error == hidl::V1_0::RadioError::NONE)
            mAdapter->setEps(result.rat == hidl::V1_4::RadioTechnology::LTE ||
                             result.rat == hidl::V1_4::RadioTechnology::LTE_CA);
        hidl::implementation::RadioResponse::getDataRegistrationStateResponse_1_5(info, result);
    });
    return {};
}
}  // namespace lge::radio
