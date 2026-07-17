// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

struct tr_error;

bool tr_spawn_async(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    std::string_view work_dir,
    tr_error* error);

bool tr_spawn_sync(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    std::string_view work_dir,
    tr_error* error);

struct tr_spawn_stderr_capture
{
    std::string text;
    bool truncated = false;
};

bool tr_spawn_sync_capture_stderr(
    char const* const* cmd,
    std::map<std::string_view, std::string_view> const& env,
    std::string_view work_dir,
    size_t max_stderr_size,
    tr_spawn_stderr_capture& capture,
    tr_error* error);
