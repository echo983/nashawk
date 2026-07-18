// This file Copyright (C) 2026 Nashawk contributors.
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only

#pragma once

#include <cstddef>
#include <ctime>

namespace tr_peer_recovery
{
inline constexpr auto MaxChecksumFailures = size_t{ 12 };
inline constexpr auto InactivityTimeout = time_t{ 60 };
inline constexpr auto Cooldown = time_t{ 30 * 60 };

[[nodiscard]] constexpr bool should_pause(size_t const failure_count) noexcept
{
    return failure_count >= MaxChecksumFailures;
}

[[nodiscard]] constexpr bool is_inactive(time_t const last_progress_at, time_t const now) noexcept
{
    return last_progress_at != 0 && now - last_progress_at >= InactivityTimeout;
}

[[nodiscard]] constexpr bool is_cooling_down(time_t const resume_at, time_t const now) noexcept
{
    return resume_at > now;
}
} // namespace tr_peer_recovery
