#include <atomic>

#include "libtransmission/version.h"

namespace
{

auto version_compat_enabled = std::atomic_bool{ true };

} // namespace

bool tr_version_compat_enabled() noexcept
{
    return version_compat_enabled.load(std::memory_order_relaxed);
}

void tr_set_version_compat_enabled(bool const enabled) noexcept
{
    version_compat_enabled.store(enabled, std::memory_order_relaxed);
}

char const* tr_display_short_version_string() noexcept
{
    return tr_version_compat_enabled() ? COMPAT_VERSION_STRING : SHORT_VERSION_STRING;
}

char const* tr_display_long_version_string() noexcept
{
    return tr_version_compat_enabled() ? COMPAT_LONG_VERSION_STRING : LONG_VERSION_STRING;
}

char const* tr_display_client_name() noexcept
{
    return tr_version_compat_enabled() ? COMPAT_CLIENT_NAME : CLIENT_NAME;
}

char const* tr_display_user_agent_prefix() noexcept
{
    return tr_version_compat_enabled() ? COMPAT_USERAGENT_PREFIX : USERAGENT_PREFIX;
}

char const* tr_display_peer_id_prefix() noexcept
{
    return tr_version_compat_enabled() ? COMPAT_PEERID_PREFIX : PEERID_PREFIX;
}
