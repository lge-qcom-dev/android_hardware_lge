/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
#include <gtest/gtest.h>
#include "QosTracker.h"
namespace lge::radio {
namespace {
Session bearer(int id) {
    Session s;
    s.id = id;
    s.qci = 1;
    return s;
}
TEST(LgeQosTracker, LifecycleAndMalformedModification) {
    QosTracker t;
    t.updateCalls({{1, "ims", true}, {0, "internet", true}}, 0);
    t.event(1, 7, QosEvent::ACTIVATED, bearer(7), true, 1);
    EXPECT_EQ(1u, t.sessions(1).size());
    EXPECT_TRUE(t.sessions(0).empty());
    t.event(1, 7, QosEvent::SUSPENDED, {}, false, 2);
    EXPECT_TRUE(t.sessions(1).empty());
    t.event(1, 7, QosEvent::ENABLED, {}, false, 3);
    EXPECT_EQ(1u, t.sessions(1).size());
    t.event(1, 7, QosEvent::MODIFIED, {}, true, 4);
    t.event(1, 7, QosEvent::ENABLED, {}, false, 5);
    EXPECT_TRUE(t.sessions(1).empty());
    t.event(1, 8, QosEvent::ACTIVATED, bearer(8), true, 6);
    t.event(1, 8, QosEvent::DELETED, {}, false, 7);
    EXPECT_TRUE(t.sessions(1).empty());
}
TEST(LgeQosTracker, EarlyEventExpiresAndDoesNotCrossCidReuse) {
    QosTracker t;
    t.event(1, 1, QosEvent::ACTIVATED, bearer(1), true, 0);
    t.updateCalls({{1, "first", true}}, 1);
    ASSERT_EQ(1u, t.sessions(1).size());
    t.updateCalls({}, 2);
    t.event(1, 1, QosEvent::ACTIVATED, bearer(1), true, 3);
    t.updateCalls({{1, "second", true}}, 4);
    EXPECT_TRUE(t.sessions(1).empty());
    t.event(2, 1, QosEvent::ACTIVATED, bearer(1), true, 5);
    t.updateCalls({{1, "second", true}, {2, "late", true}}, 6000);
    EXPECT_TRUE(t.sessions(2).empty());
}
TEST(LgeQosTracker, IdentitySetupAndResetClearBearers) {
    QosTracker t;
    t.updateCalls({{1, "first", true}}, 0);
    t.event(1, 1, QosEvent::ACTIVATED, bearer(1), true, 1);
    t.updateCalls({{1, "second", true}}, 2);
    EXPECT_TRUE(t.sessions(1).empty());
    t.event(1, 1, QosEvent::ACTIVATED, bearer(1), true, 3);
    t.setup({1, "second", true}, 4);
    EXPECT_TRUE(t.sessions(1).empty());
    t.event(1, 1, QosEvent::ACTIVATED, bearer(1), true, 5);
    t.clear();
    EXPECT_TRUE(t.sessions(1).empty());
}
}  // namespace
}  // namespace lge::radio
