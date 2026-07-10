// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "libtransmission/usenet-piece-store.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

#include <fmt/format.h>

#include "libtransmission/crypto-utils.h"
#include "libtransmission/file.h"
#include "libtransmission/quark.h"
#include "libtransmission/torrent-metainfo.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/variant.h"

using namespace std::literals;

namespace
{
auto constexpr Domain = "nashawk.local"sv;

[[nodiscard]] tr_quark key_available_at()
{
    return tr_quark_new("available_at"sv);
}

[[nodiscard]] tr_quark key_last_local_at()
{
    return tr_quark_new("last_local_at"sv);
}

[[nodiscard]] tr_quark key_max_article_size()
{
    return tr_quark_new("max_article_size"sv);
}

[[nodiscard]] tr_quark key_message_id()
{
    return tr_quark_new("message_id"sv);
}

[[nodiscard]] std::string make_dir(std::string_view config_dir)
{
    auto dir = tr_pathbuf{ config_dir, "/usenet-pieces"sv };
    tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0700);
    return std::string{ dir };
}

[[nodiscard]] std::string make_message_id(tr_sha1_digest_t const& piece_hash)
{
    return fmt::format("{}@{}", tr_sha1_to_string(piece_hash), Domain);
}

[[nodiscard]] uint64_t now_seconds()
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
}

[[nodiscard]] tr_variant manifest_to_variant(tr_usenet_piece_manifest const& manifest)
{
    auto top = tr_variant::Map{ 6U };
    top.try_emplace(TR_KEY_version, int64_t{ manifest.version });
    top.try_emplace(TR_KEY_hash_string, manifest.info_hash_string);
    top.try_emplace(TR_KEY_piece_count, static_cast<int64_t>(std::size(manifest.pieces)));
    top.try_emplace(TR_KEY_piece_size, static_cast<int64_t>(manifest.piece_size));
    top.try_emplace(key_max_article_size(), static_cast<int64_t>(manifest.max_article_size));

    auto pieces = tr_variant::Vector{};
    pieces.reserve(std::size(manifest.pieces));
    for (auto const& piece : manifest.pieces)
    {
        auto entry = tr_variant::Map{ 4U };
        entry.try_emplace(TR_KEY_status, tr_variant::unmanaged_string(tr_usenet_piece_state_name(piece.state)));
        entry.try_emplace(key_message_id(), piece.message_id);
        if (piece.available_at != 0U)
        {
            entry.try_emplace(key_available_at(), static_cast<int64_t>(piece.available_at));
        }
        if (piece.last_local_at != 0U)
        {
            entry.try_emplace(key_last_local_at(), static_cast<int64_t>(piece.last_local_at));
        }
        pieces.emplace_back(std::move(entry));
    }
    top.try_emplace(TR_KEY_pieces, std::move(pieces));

    return tr_variant{ std::move(top) };
}

[[nodiscard]] std::optional<tr_usenet_piece_manifest> manifest_from_variant(tr_variant const& variant)
{
    auto const* top = variant.get_if<tr_variant::Map>();
    if (top == nullptr)
    {
        return {};
    }

    auto manifest = tr_usenet_piece_manifest{};

    if (auto const version = top->value_if<int64_t>(TR_KEY_version); version && *version > 0)
    {
        manifest.version = static_cast<uint32_t>(*version);
    }

    if (auto const info_hash = top->value_if<std::string_view>(TR_KEY_hash_string); info_hash)
    {
        manifest.info_hash_string = *info_hash;
    }

    if (auto const piece_size = top->value_if<int64_t>(TR_KEY_piece_size); piece_size && *piece_size >= 0)
    {
        manifest.piece_size = static_cast<uint64_t>(*piece_size);
    }

    if (auto const max_article_size = top->value_if<int64_t>(key_max_article_size());
        max_article_size && *max_article_size >= 0)
    {
        manifest.max_article_size = static_cast<uint64_t>(*max_article_size);
    }

    auto const* pieces = top->find_if<tr_variant::Vector>(TR_KEY_pieces);
    if (std::empty(manifest.info_hash_string) || pieces == nullptr)
    {
        return {};
    }

    manifest.pieces.reserve(std::size(*pieces));
    for (auto const& var : *pieces)
    {
        auto const* entry = var.get_if<tr_variant::Map>();
        if (entry == nullptr)
        {
            return {};
        }

        auto piece = tr_usenet_piece_entry{};
        if (auto const status = entry->value_if<std::string_view>(TR_KEY_status); status)
        {
            if (auto const state = tr_usenet_piece_state_from_name(*status); state)
            {
                piece.state = *state;
            }
            else
            {
                return {};
            }
        }

        if (auto const message_id = entry->value_if<std::string_view>(key_message_id()); message_id)
        {
            piece.message_id = *message_id;
        }

        if (auto const available_at = entry->value_if<int64_t>(key_available_at()); available_at && *available_at > 0)
        {
            piece.available_at = static_cast<uint64_t>(*available_at);
        }

        if (auto const last_local_at = entry->value_if<int64_t>(key_last_local_at()); last_local_at && *last_local_at > 0)
        {
            piece.last_local_at = static_cast<uint64_t>(*last_local_at);
        }

        if (std::empty(piece.message_id))
        {
            return {};
        }

        manifest.pieces.emplace_back(std::move(piece));
    }

    if (auto const piece_count = top->value_if<int64_t>(TR_KEY_piece_count);
        piece_count && *piece_count >= 0 && static_cast<uint64_t>(*piece_count) != std::size(manifest.pieces))
    {
        return {};
    }

    return manifest;
}
} // namespace

std::string_view tr_usenet_piece_state_name(tr_usenet_piece_state const state) noexcept
{
    switch (state)
    {
    case tr_usenet_piece_state::Unknown:
        return "unknown"sv;
    case tr_usenet_piece_state::Uploading:
        return "uploading"sv;
    case tr_usenet_piece_state::Available:
        return "available"sv;
    case tr_usenet_piece_state::Failed:
        return "failed"sv;
    }

    return "unknown"sv;
}

std::optional<tr_usenet_piece_state> tr_usenet_piece_state_from_name(std::string_view const name) noexcept
{
    if (name == "unknown"sv)
    {
        return tr_usenet_piece_state::Unknown;
    }
    if (name == "uploading"sv)
    {
        return tr_usenet_piece_state::Uploading;
    }
    if (name == "available"sv)
    {
        return tr_usenet_piece_state::Available;
    }
    if (name == "failed"sv)
    {
        return tr_usenet_piece_state::Failed;
    }

    return {};
}

bool tr_usenet_piece_is_eviction_eligible(
    tr_usenet_piece_entry const& entry,
    bool const has_local_piece,
    uint64_t const now_seconds,
    uint64_t const min_age_seconds) noexcept
{
    if (!has_local_piece || entry.state != tr_usenet_piece_state::Available || entry.available_at == 0U)
    {
        return false;
    }

    auto const local_reference_time = std::max(entry.available_at, entry.last_local_at);
    return now_seconds >= local_reference_time && now_seconds - local_reference_time >= min_age_seconds;
}

size_t tr_usenet_piece_manifest::piece_count() const noexcept
{
    return std::size(pieces);
}

bool tr_usenet_piece_manifest::has_piece(tr_piece_index_t const piece) const noexcept
{
    return piece < std::size(pieces) && pieces[piece].state == tr_usenet_piece_state::Available;
}

bool tr_usenet_piece_manifest::is_available(tr_piece_index_t const piece) const noexcept
{
    return has_piece(piece);
}

bool tr_usenet_piece_manifest::has_message_id_state(std::string_view const message_id, tr_usenet_piece_state const state)
    const noexcept
{
    return std::any_of(
        std::begin(pieces),
        std::end(pieces),
        [message_id, state](auto const& entry) { return entry.message_id == message_id && entry.state == state; });
}

void tr_usenet_piece_manifest::set_piece_state(tr_piece_index_t const piece, tr_usenet_piece_state const state)
{
    if (piece < std::size(pieces))
    {
        auto& entry = pieces[piece];
        auto const previous_state = entry.state;
        auto const timestamp = now_seconds();

        if (state == tr_usenet_piece_state::Available)
        {
            if (previous_state != tr_usenet_piece_state::Available || entry.available_at == 0U)
            {
                entry.available_at = timestamp;
            }

            entry.last_local_at = timestamp;
        }
        else if (previous_state == tr_usenet_piece_state::Available)
        {
            entry.available_at = 0U;
        }

        pieces[piece].state = state;
    }
}

void tr_usenet_piece_manifest::set_message_id_state(std::string_view const message_id, tr_usenet_piece_state const state)
{
    for (tr_piece_index_t piece = 0U; piece < std::size(pieces); ++piece)
    {
        if (pieces[piece].message_id == message_id)
        {
            set_piece_state(piece, state);
        }
    }
}

tr_usenet_piece_store::tr_usenet_piece_store(std::string_view const config_dir, uint64_t const max_article_size)
    : dir_{ make_dir(config_dir) }
    , max_article_size_{ max_article_size }
{
}

bool tr_usenet_piece_store::is_piece_size_eligible(uint64_t const piece_size) const noexcept
{
    return piece_size <= max_article_size_;
}

std::optional<std::string> tr_usenet_piece_store::ensure_torrent(tr_torrent_metainfo const& metainfo)
{
    if (!is_piece_size_eligible(metainfo.piece_size()))
    {
        return fmt::format(
            "Torrent piece size {} exceeds Usenet article size limit {}",
            metainfo.piece_size(),
            max_article_size_);
    }

    if (auto existing = load(metainfo.info_hash_string()); existing)
    {
        if (existing->piece_size != metainfo.piece_size() || existing->piece_count() != metainfo.piece_count())
        {
            return "Usenet manifest does not match torrent metainfo";
        }

        return {};
    }

    if (!save(make_manifest(metainfo)))
    {
        return "Could not save Usenet piece manifest";
    }

    return {};
}

std::optional<std::string> tr_usenet_piece_store::set_piece_state(
    std::string_view const info_hash_string,
    tr_piece_index_t const piece,
    tr_usenet_piece_state const state) const
{
    auto manifest = load(info_hash_string);
    if (!manifest)
    {
        return "Usenet manifest is missing";
    }

    if (piece >= manifest->piece_count())
    {
        return fmt::format("Usenet piece index {} is out of range", piece);
    }

    manifest->set_piece_state(piece, state);
    if (!save(*manifest))
    {
        return "Could not save Usenet piece manifest";
    }

    return {};
}

std::optional<std::string> tr_usenet_piece_store::set_message_id_state(
    std::string_view const info_hash_string,
    std::string_view const message_id,
    tr_usenet_piece_state const state) const
{
    auto manifest = load(info_hash_string);
    if (!manifest)
    {
        return "Usenet manifest is missing";
    }

    if (std::empty(message_id))
    {
        return "Usenet message id is empty";
    }

    manifest->set_message_id_state(message_id, state);
    if (!save(*manifest))
    {
        return "Could not save Usenet piece manifest";
    }

    return {};
}

std::optional<std::string> tr_usenet_piece_store::note_piece_local_activity(
    std::string_view const info_hash_string,
    tr_piece_index_t const piece) const
{
    auto manifest = load(info_hash_string);
    if (!manifest)
    {
        return "Usenet manifest is missing";
    }

    if (piece >= manifest->piece_count())
    {
        return fmt::format("Usenet piece index {} is out of range", piece);
    }

    manifest->pieces[piece].last_local_at = now_seconds();
    if (!save(*manifest))
    {
        return "Could not save Usenet piece manifest";
    }

    return {};
}

std::optional<std::string> tr_usenet_piece_store::reset_interrupted_uploads(
    std::string_view const info_hash_string,
    std::vector<tr_piece_index_t>& pieces) const
{
    pieces.clear();

    auto manifest = load(info_hash_string);
    if (!manifest)
    {
        return "Usenet manifest is missing";
    }

    for (tr_piece_index_t piece = 0; piece < manifest->piece_count(); ++piece)
    {
        if (manifest->pieces[piece].state == tr_usenet_piece_state::Uploading)
        {
            manifest->pieces[piece].state = tr_usenet_piece_state::Unknown;
            manifest->pieces[piece].available_at = 0U;
            pieces.push_back(piece);
        }
    }

    if (std::empty(pieces))
    {
        return {};
    }

    if (!save(*manifest))
    {
        pieces.clear();
        return "Could not save Usenet piece manifest";
    }

    return {};
}

std::optional<tr_usenet_piece_entry> tr_usenet_piece_store::piece_entry(
    std::string_view const info_hash_string,
    tr_piece_index_t const piece) const
{
    auto const manifest = load(info_hash_string);
    if (!manifest || piece >= manifest->piece_count())
    {
        return {};
    }

    return manifest->pieces[piece];
}

std::optional<tr_usenet_piece_manifest> tr_usenet_piece_store::load(std::string_view const info_hash_string) const
{
    auto const filename = manifest_path(info_hash_string);
    if (!tr_sys_path_exists(filename))
    {
        return {};
    }

    auto const variant = tr_variant_serde::json().parse_file(filename);
    if (!variant)
    {
        return {};
    }

    return manifest_from_variant(*variant);
}

bool tr_usenet_piece_store::save(tr_usenet_piece_manifest const& manifest) const
{
    tr_sys_dir_create(dir_, TR_SYS_DIR_CREATE_PARENTS, 0700);
    return tr_variant_serde::json().to_file(manifest_to_variant(manifest), manifest_path(manifest.info_hash_string));
}

std::string tr_usenet_piece_store::manifest_path(std::string_view const info_hash_string) const
{
    return std::string{ tr_pathbuf{ dir_, '/', info_hash_string, ".json"sv }.sv() };
}

tr_usenet_piece_manifest tr_usenet_piece_store::make_manifest(tr_torrent_metainfo const& metainfo) const
{
    auto manifest = tr_usenet_piece_manifest{};
    manifest.info_hash_string = metainfo.info_hash_string();
    manifest.piece_size = metainfo.piece_size();
    manifest.max_article_size = max_article_size_;
    manifest.pieces.reserve(metainfo.piece_count());

    for (tr_piece_index_t piece = 0; piece < metainfo.piece_count(); ++piece)
    {
        manifest.pieces.push_back(
            {
                .state = tr_usenet_piece_state::Unknown,
                .message_id = make_message_id(metainfo.piece_hash(piece)),
            });
    }

    return manifest;
}
