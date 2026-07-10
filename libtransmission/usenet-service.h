// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

class tr_variant;

struct tr_usenet_upload_request
{
    std::string_view config_dir;
    std::string_view file_path;
    std::string_view message_id;
    std::string_view subject;
    std::string_view yenc_name;
    uint64_t article_size = 0U;
};

[[nodiscard]] std::optional<std::string> tr_usenet_startup_check(std::string_view config_dir, tr_variant const& settings);
[[nodiscard]] std::optional<std::string> tr_usenet_upload_file(tr_usenet_upload_request const& request);
