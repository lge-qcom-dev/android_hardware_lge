/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>
#include "Qos.h"

namespace lge::radio {
namespace {
const std::string flow = "qci=1,max_rate=128000,guaranteed_rate=64000";
const std::string tx =
        "filter_id=1,precedence=10,ipv4_dest_addr=192.0.2.5,"
        "ipv4_dest_subnet_mask=0xffffffff,protocol=0x11,"
        "udp_src_port_start=4000,udp_src_port_range=1,"
        "udp_dest_port_start=5000,udp_dest_port_range=1";

TEST(LgeQos, TranslatesDirectionAndBandwidth) {
    auto session = parseQos(7, flow, flow, tx,
                            "protocol=0x11,udp_dest_port_start=4000,udp_dest_port_range=1");
    ASSERT_TRUE(session);
    EXPECT_EQ(1, session->qci);
    EXPECT_EQ(128, session->uplink.maximumKbps);
    EXPECT_EQ(64, session->downlink.guaranteedKbps);
    ASSERT_EQ(2u, session->filters.size());
    const auto& up = session->filters[0];
    EXPECT_EQ(1, up.direction);
    EXPECT_EQ("192.0.2.5/32", up.remoteAddresses.at(0));
    EXPECT_EQ(4000, up.localPort->start);
    EXPECT_EQ(4001, up.localPort->end);
    EXPECT_EQ(5000, up.remotePort->start);
    EXPECT_EQ(0, session->filters[1].direction);
    EXPECT_EQ(4000, session->filters[1].localPort->start);
}

TEST(LgeQos, MultipleIpv6FiltersAndVendorSpelling) {
    auto session =
            parseQos(9, flow, "",
                     "ipv6_dest_addr=2001:db8::1,"
                     "ipv6_dest_filter_prefix_len=0x80,protocol=0x6,tcp_src_port_start=42,tcp_src_"
                     "ports_range=0/"
                     "protocol=0x11,transport_dest_port_start =4000,transport_dest_port_range=3",
                     "");
    ASSERT_TRUE(session);
    ASSERT_EQ(2u, session->filters.size());
    EXPECT_EQ("2001:db8::1/128", session->filters[0].remoteAddresses.at(0));
    EXPECT_EQ(42, session->filters[0].localPort->end);
    EXPECT_EQ(4003, session->filters[1].remotePort->end);
}

// Sanitized Wing capture: LG omits filter_id and precedence, and supplies
// separate RTP/RTCP filters in each direction. Rates changed after activation.
TEST(LgeQos, WingCapturedRtpAndRtcpBearer) {
    const std::string up =
            "ipv4_dest_addr=192.0.2.16,ipv4_dest_subnet_mask=0xffffffff,"
            "protocol=0x11,udp_src_port_start=50023,udp_src_port_range=0,"
            "udp_dest_port_start=32655,udp_dest_port_range=0/"
            "ipv4_dest_addr=192.0.2.16,ipv4_dest_subnet_mask=0xffffffff,"
            "protocol=0x11,udp_src_port_start=50022,udp_src_port_range=0,"
            "udp_dest_port_start=32654,udp_dest_port_range=0";
    const std::string down =
            "ipv4_src_addr=192.0.2.16,ipv4_src_subnet_mask=0xffffffff,"
            "protocol=0x11,udp_src_port_start=32654,udp_src_port_range=0,"
            "udp_dest_port_start=50022,udp_dest_port_range=0/"
            "ipv4_src_addr=192.0.2.16,ipv4_src_subnet_mask=0xffffffff,"
            "protocol=0x11,udp_src_port_start=32655,udp_src_port_range=0,"
            "udp_dest_port_start=50023,udp_dest_port_range=0";
    for (const int rate : {45000, 31000}) {
        const std::string description = "qci=1,max_rate=" + std::to_string(rate) +
                                        ",guaranteed_rate=" + std::to_string(rate);
        auto session = parseQos(275, description, description, up, down);
        ASSERT_TRUE(session);
        EXPECT_EQ(275, session->id);
        EXPECT_EQ(1, session->qci);
        EXPECT_EQ(rate / 1000, session->uplink.maximumKbps);
        EXPECT_EQ(rate / 1000, session->downlink.guaranteedKbps);
        ASSERT_EQ(4u, session->filters.size());
        for (const auto& filter : session->filters) {
            EXPECT_EQ(17, filter.protocol);
            EXPECT_EQ(-1, filter.precedence);
            ASSERT_TRUE(filter.localPort);
            ASSERT_TRUE(filter.remotePort);
            EXPECT_EQ(filter.localPort->start, filter.localPort->end);
            EXPECT_EQ(filter.remotePort->start, filter.remotePort->end);
            EXPECT_EQ("192.0.2.16/32", filter.remoteAddresses.at(0));
        }
        EXPECT_EQ(50023, session->filters[0].localPort->start);
        EXPECT_EQ(32655, session->filters[0].remotePort->start);
        EXPECT_EQ(50022, session->filters[2].localPort->start);
        EXPECT_EQ(32654, session->filters[2].remotePort->start);
        EXPECT_EQ(1, session->filters[0].direction);
        EXPECT_EQ(0, session->filters[2].direction);
    }
}

TEST(LgeQos, RejectsAndroidInvalidPortsAndConflictingSelectors) {
    EXPECT_FALSE(
            parseQos(1, flow, flow, "protocol=17,udp_src_port_start=19,udp_src_port_range=1", ""));
    EXPECT_TRUE(
            parseQos(1, flow, flow, "protocol=17,udp_src_port_start=20,udp_src_port_range=0", ""));
    EXPECT_FALSE(parseQos(1, "qci=255,max_rate=1,guaranteed_rate=0", "", tx, ""));
    EXPECT_FALSE(parseQos(1, flow, flow,
                          "protocol=17,tos_value=0x20,mask=255,"
                          "ipv6_traffic_class_value=0x40,ipv6_traffic_class_mask=255",
                          ""));
}

TEST(LgeQos, RejectsInvalidOrBroadenedFilters) {
    for (const auto& bad :
         {"", "filter_id=1", "protocol=garbage", "protocol=0x11,protocol=0x6", "protocol=0x11/",
          "protocol=0x11,", "protocol=0x11,unknown=1",
          "protocol=0x11,udp_dest_port_start=65535,udp_dest_port_range=1",
          "protocol=0x11,tos_value=0x20,mask=0x30",
          "protocol=0x11,ipv4_dest_addr=192.0.2.5,ipv4_dest_subnet_mask=0xff00ff00",
          "protocol=0x11,ipv6_dest_addr=2001:db8::1,ipv6_dest_filter_prefix_len=129",
          "protocol=0x11,tcp_src_port_start=42,tcp_src_ports_range=0"}) {
        EXPECT_FALSE(parseQos(1, flow, flow, bad, "")) << bad;
    }
    EXPECT_FALSE(parseQos(0, flow, flow, tx, ""));
    EXPECT_FALSE(parseQos(1, flow, "qci=2,max_rate=128000,guaranteed_rate=64000", tx, ""));
    EXPECT_FALSE(parseQos(1, "qci=1,max_rate=-1,guaranteed_rate=0", "", tx, ""));
}
}  // namespace
}  // namespace lge::radio
