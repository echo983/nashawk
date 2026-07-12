// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "libtransmission/types.h"

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

struct tr_usenet_upload_batch_request
{
    std::string_view config_dir;
    std::vector<std::string> file_paths;
    uint64_t article_size = 0U;
    size_t connections = 1U;
};

struct tr_usenet_download_request
{
    std::string_view config_dir;
    std::string_view message_id;
    uint64_t expected_size = 0U;
    std::optional<tr_sha1_digest_t> expected_hash;
};

struct tr_usenet_download_result
{
    std::vector<uint8_t> data;
    size_t article_count = 0U;
};

using tr_usenet_decoded_article_fetch = std::function<
    std::optional<std::string>(std::string_view message_id, std::vector<uint8_t>& setme)>;

struct tr_usenet_article_exists_request
{
    std::string_view config_dir;
    std::string_view message_id;
};

enum class tr_usenet_article_exists_result : uint8_t
{
    Exists,
    Missing,
};

[[nodiscard]] std::optional<std::string> tr_usenet_startup_check(std::string_view config_dir, tr_variant const& settings);
[[nodiscard]] std::optional<std::string> tr_usenet_upload_file(tr_usenet_upload_request const& request);
[[nodiscard]] std::optional<std::string> tr_usenet_upload_files(tr_usenet_upload_batch_request const& request);
[[nodiscard]] std::variant<tr_usenet_article_exists_result, std::string> tr_usenet_article_exists(
    tr_usenet_article_exists_request const& request);
[[nodiscard]] std::optional<std::string> tr_usenet_download_piece(
    tr_usenet_download_request const& request,
    std::vector<uint8_t>& setme);
[[nodiscard]] std::optional<std::string> tr_usenet_assemble_piece_chain(
    std::string_view base_message_id,
    uint64_t expected_size,
    std::optional<tr_sha1_digest_t> const& expected_hash,
    tr_usenet_decoded_article_fetch const& fetch,
    tr_usenet_download_result& setme);
[[nodiscard]] std::optional<std::string> tr_usenet_download_piece_chain(
    tr_usenet_download_request const& request,
    tr_usenet_download_result& setme);
