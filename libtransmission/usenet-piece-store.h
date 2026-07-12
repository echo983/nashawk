// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "libtransmission/types.h"

struct tr_torrent_metainfo;

enum class tr_usenet_piece_state : uint8_t
{
    Unknown,
    Uploading,
    Available,
    Failed,
};

enum class tr_usenet_discovery_state : uint8_t
{
    NotChecked,
    Checking,
    Available,
    Missing,
    Error,
};

struct tr_usenet_piece_entry
{
    tr_usenet_piece_state state = tr_usenet_piece_state::Unknown;
    uint64_t available_at = 0U;
    uint64_t last_local_at = 0U;
    std::string message_id;
};

struct tr_usenet_discovery_info
{
    tr_usenet_discovery_state state = tr_usenet_discovery_state::NotChecked;
    uint64_t checked_at = 0U;
    size_t sample_size = 0U;
    std::vector<tr_piece_index_t> sampled_pieces;
    std::string error;
};

struct tr_usenet_piece_manifest
{
    uint32_t version = 1U;
    std::string info_hash_string;
    uint64_t piece_size = 0U;
    uint64_t max_article_size = 0U;
    tr_usenet_discovery_info discovery;
    std::vector<tr_usenet_piece_entry> pieces;

    [[nodiscard]] size_t piece_count() const noexcept;
    [[nodiscard]] bool has_piece(tr_piece_index_t piece) const noexcept;
    [[nodiscard]] bool is_available(tr_piece_index_t piece) const noexcept;
    [[nodiscard]] bool has_message_id_state(std::string_view message_id, tr_usenet_piece_state state) const noexcept;
    [[nodiscard]] bool has_meaningful_state() const noexcept;

    void set_piece_state(tr_piece_index_t piece, tr_usenet_piece_state state);
    void set_message_id_state(std::string_view message_id, tr_usenet_piece_state state);
    void set_all_piece_states(tr_usenet_piece_state state);
};

class tr_usenet_piece_store
{
public:
    tr_usenet_piece_store(std::string_view config_dir, uint64_t max_article_size);

    [[nodiscard]] constexpr uint64_t max_article_size() const noexcept
    {
        return max_article_size_;
    }

    [[nodiscard]] bool is_piece_size_eligible(uint64_t piece_size) const noexcept;
    [[nodiscard]] std::optional<std::string> ensure_torrent(tr_torrent_metainfo const& metainfo);
    [[nodiscard]] std::optional<std::string> set_piece_state(
        std::string_view info_hash_string,
        tr_piece_index_t piece,
        tr_usenet_piece_state state) const;
    [[nodiscard]] std::optional<std::string> set_message_id_state(
        std::string_view info_hash_string,
        std::string_view message_id,
        tr_usenet_piece_state state) const;
    [[nodiscard]] std::optional<std::string> note_piece_local_activity(
        std::string_view info_hash_string,
        tr_piece_index_t piece) const;
    [[nodiscard]] std::optional<std::string> reset_interrupted_uploads(
        std::string_view info_hash_string,
        std::vector<tr_piece_index_t>& pieces) const;
    [[nodiscard]] std::optional<tr_usenet_piece_entry> piece_entry(std::string_view info_hash_string, tr_piece_index_t piece)
        const;
    [[nodiscard]] std::optional<tr_usenet_piece_manifest> load(std::string_view info_hash_string) const;
    [[nodiscard]] bool save(tr_usenet_piece_manifest const& manifest) const;
    [[nodiscard]] std::string manifest_path(std::string_view info_hash_string) const;

private:
    [[nodiscard]] tr_usenet_piece_manifest make_manifest(tr_torrent_metainfo const& metainfo) const;

    std::string dir_;
    uint64_t max_article_size_ = 0U;
};

[[nodiscard]] std::string_view tr_usenet_piece_state_name(tr_usenet_piece_state state) noexcept;
[[nodiscard]] std::optional<tr_usenet_piece_state> tr_usenet_piece_state_from_name(std::string_view name) noexcept;
[[nodiscard]] std::string_view tr_usenet_discovery_state_name(tr_usenet_discovery_state state) noexcept;
[[nodiscard]] std::optional<tr_usenet_discovery_state> tr_usenet_discovery_state_from_name(std::string_view name) noexcept;
[[nodiscard]] std::vector<tr_piece_index_t> tr_usenet_discovery_sample_pieces(
    std::string_view info_hash_string,
    tr_piece_index_t piece_count,
    size_t sample_size);
[[nodiscard]] bool tr_usenet_piece_is_eviction_eligible(
    tr_usenet_piece_entry const& entry,
    bool has_local_piece,
    uint64_t now_seconds,
    uint64_t min_age_seconds) noexcept;
