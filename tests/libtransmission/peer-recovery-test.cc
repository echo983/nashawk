// This file Copyright (C) 2026 Nashawk contributors.
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only

#include <gtest/gtest.h>

#include <libtransmission/peer-recovery.h>

TEST(PeerRecoveryTest, pausesAtFailureLimit)
{
    EXPECT_FALSE(tr_peer_recovery::should_pause(tr_peer_recovery::MaxChecksumFailures - 1U));
    EXPECT_TRUE(tr_peer_recovery::should_pause(tr_peer_recovery::MaxChecksumFailures));
    EXPECT_TRUE(tr_peer_recovery::should_pause(tr_peer_recovery::MaxChecksumFailures + 1U));
}

TEST(PeerRecoveryTest, timeoutTracksInactivity)
{
    auto constexpr Now = time_t{ 1000 };
    EXPECT_FALSE(tr_peer_recovery::is_inactive(0, Now));
    EXPECT_FALSE(tr_peer_recovery::is_inactive(Now - tr_peer_recovery::InactivityTimeout + 1, Now));
    EXPECT_TRUE(tr_peer_recovery::is_inactive(Now - tr_peer_recovery::InactivityTimeout, Now));
}

TEST(PeerRecoveryTest, recentProgressExtendsTimeout)
{
    auto constexpr Now = time_t{ 1000 };
    auto constexpr SelectedAt = Now - tr_peer_recovery::InactivityTimeout;
    auto constexpr LastProgressAt = Now - 1;
    EXPECT_TRUE(tr_peer_recovery::is_inactive(SelectedAt, Now));
    EXPECT_FALSE(tr_peer_recovery::is_inactive(LastProgressAt, Now));
}

TEST(PeerRecoveryTest, cooldownEndsAtResumeTime)
{
    auto constexpr Now = time_t{ 1000 };
    EXPECT_TRUE(tr_peer_recovery::is_cooling_down(Now + 1, Now));
    EXPECT_FALSE(tr_peer_recovery::is_cooling_down(Now, Now));
    EXPECT_FALSE(tr_peer_recovery::is_cooling_down(0, Now));
}
