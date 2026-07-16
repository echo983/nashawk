// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm>
#include <array>
#include <fstream>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <libtransmission/crypto-utils.h>
#include <libtransmission/torrent-metainfo.h>
#include <libtransmission/usenet-piece-store.h>

#include "test-fixtures.h"

using namespace std::literals;

namespace
{
[[nodiscard]] tr_torrent_metainfo load_metainfo(std::string_view const filename)
{
    auto metainfo = tr_torrent_metainfo{};
    EXPECT_TRUE(metainfo.parse_torrent_file(tr_pathbuf{ LIBTRANSMISSION_TEST_ASSETS_DIR, '/', filename }));
    return metainfo;
}
} // namespace

namespace tr::test
{

using UsenetPieceStoreTest = SandboxedTest;

TEST_F(UsenetPieceStoreTest, stateNamesRoundtrip)
{
    for (auto const state : { tr_usenet_piece_state::Unknown,
                              tr_usenet_piece_state::Uploading,
                              tr_usenet_piece_state::Available,
                              tr_usenet_piece_state::Failed })
    {
        EXPECT_EQ(state, tr_usenet_piece_state_from_name(tr_usenet_piece_state_name(state)));
    }

    EXPECT_FALSE(tr_usenet_piece_state_from_name("not-a-state"sv));

    for (auto const state : { tr_usenet_discovery_state::NotChecked,
                              tr_usenet_discovery_state::Checking,
                              tr_usenet_discovery_state::Available,
                              tr_usenet_discovery_state::Missing,
                              tr_usenet_discovery_state::Error })
    {
        EXPECT_EQ(state, tr_usenet_discovery_state_from_name(tr_usenet_discovery_state_name(state)));
    }

    EXPECT_FALSE(tr_usenet_discovery_state_from_name("not-a-discovery-state"sv));
}

TEST_F(UsenetPieceStoreTest, multipartMessageIdsAreDeterministicAndBounded)
{
    auto const metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto const base = tr_usenet_piece_base_message_id(metainfo.piece_hash(0));
    auto const hash = tr_sha1_to_string(metainfo.piece_hash(0));

    EXPECT_EQ(fmt::format("{}@nashawk.local", hash), base);
    EXPECT_EQ(base, tr_usenet_piece_article_message_id(base, 0U));
    EXPECT_EQ(fmt::format("{}.1@nashawk.local", hash), tr_usenet_piece_article_message_id(base, 1U));
    EXPECT_EQ(fmt::format("{}.1023@nashawk.local", hash), tr_usenet_piece_article_message_id(base, 1023U));
    EXPECT_FALSE(tr_usenet_piece_article_message_id(base, TrUsenetMaxArticlesPerPiece));
    EXPECT_FALSE(tr_usenet_piece_article_message_id("not-a-piece@nashawk.local"sv, 0U));
    EXPECT_FALSE(tr_usenet_piece_article_message_id(fmt::format("{}@example.com", hash), 0U));
}

TEST_F(UsenetPieceStoreTest, multipartPartPlanCoversPieceExactly)
{
    static auto constexpr MiB = uint64_t{ 1024U * 1024U };

    auto plan = tr_usenet_piece_part_plan(2U * MiB, 2U * MiB);
    ASSERT_TRUE(plan);
    EXPECT_EQ((std::vector<tr_usenet_piece_part>{ { 0U, 0U, 2U * MiB } }), *plan);

    plan = tr_usenet_piece_part_plan(4U * MiB, 2U * MiB);
    ASSERT_TRUE(plan);
    EXPECT_EQ((std::vector<tr_usenet_piece_part>{ { 0U, 0U, 2U * MiB }, { 1U, 2U * MiB, 2U * MiB } }), *plan);

    plan = tr_usenet_piece_part_plan(4U * MiB + 1U, 2U * MiB);
    ASSERT_TRUE(plan);
    EXPECT_EQ(
        (std::vector<tr_usenet_piece_part>{
            { 0U, 0U, 2U * MiB },
            { 1U, 2U * MiB, 2U * MiB },
            { 2U, 4U * MiB, 1U },
        }),
        *plan);

    EXPECT_FALSE(tr_usenet_piece_part_plan(2U * MiB, 0U));
    EXPECT_FALSE(tr_usenet_piece_part_plan(0U, 2U * MiB));
    EXPECT_EQ(TrUsenetMaxArticlesPerPiece, tr_usenet_piece_article_count(TrUsenetMaxArticlesPerPiece, 1U));
    EXPECT_FALSE(tr_usenet_piece_article_count(TrUsenetMaxArticlesPerPiece + 1U, 1U));
}

TEST_F(UsenetPieceStoreTest, evictionEligibilityRequiresAvailableLocalOldPiece)
{
    auto entry = tr_usenet_piece_entry{
        .state = tr_usenet_piece_state::Available,
        .available_at = 100U,
        .verified_at = 100U,
        .last_local_at = 100U,
        .message_id = "piece@nashawk.local",
    };

    EXPECT_TRUE(tr_usenet_piece_is_eviction_eligible(entry, true, 200U, 60U));
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, false, 200U, 60U));
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 120U, 60U));
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 99U, 60U));

    entry.verified_at = 0U;
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 200U, 60U));
    entry.verified_at = 100U;

    entry.last_local_at = 180U;
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 200U, 60U));
    EXPECT_TRUE(tr_usenet_piece_is_eviction_eligible(entry, true, 240U, 60U));

    entry.last_local_at = 100U;
    entry.available_at = 0U;
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 200U, 60U));

    entry.available_at = 100U;
    entry.state = tr_usenet_piece_state::Uploading;
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 200U, 60U));
}

TEST_F(UsenetPieceStoreTest, ensureTorrentCreatesManifest)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };

    EXPECT_FALSE(store.ensure_torrent(metainfo));

    auto manifest = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(manifest);
    EXPECT_EQ(TrUsenetPieceManifestVersion, manifest->version);
    EXPECT_EQ(metainfo.info_hash_string(), manifest->info_hash_string);
    EXPECT_EQ(metainfo.piece_size(), manifest->piece_size);
    EXPECT_EQ(metainfo.piece_count(), manifest->piece_count());
    ASSERT_FALSE(std::empty(manifest->pieces));
    EXPECT_EQ(tr_usenet_piece_state::Unknown, manifest->pieces.front().state);
    EXPECT_EQ(fmt::format("{}@nashawk.local", tr_sha1_to_string(metainfo.piece_hash(0))), manifest->pieces.front().message_id);
}

TEST_F(UsenetPieceStoreTest, savesPieceState)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    auto manifest = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(manifest);
    manifest->set_piece_state(0, tr_usenet_piece_state::Available);
    ASSERT_TRUE(store.save(*manifest));

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(TrUsenetPieceManifestVersion, loaded->version);
    EXPECT_TRUE(loaded->is_available(0));
    EXPECT_GT(loaded->pieces[0].available_at, 0U);
    EXPECT_GT(loaded->pieces[0].last_local_at, 0U);
    EXPECT_EQ(0U, loaded->pieces[0].verified_at);
}

TEST_F(UsenetPieceStoreTest, verifiedCredentialRoundtripsAndAppliesToDuplicateMessageIds)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    auto manifest = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(manifest);
    ASSERT_GE(manifest->piece_count(), 2U);
    manifest->pieces[1].message_id = manifest->pieces[0].message_id;
    manifest->set_message_id_state(manifest->pieces[0].message_id, tr_usenet_piece_state::Available);
    manifest->mark_message_id_verified(manifest->pieces[0].message_id, 12345U);
    ASSERT_TRUE(store.save(*manifest));

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(12345U, loaded->pieces[0].verified_at);
    EXPECT_EQ(12345U, loaded->pieces[1].verified_at);
}

TEST_F(UsenetPieceStoreTest, discoveryMetadataRoundtrips)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    auto manifest = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(manifest);
    manifest->discovery.state = tr_usenet_discovery_state::Available;
    manifest->discovery.checked_at = 12345U;
    manifest->discovery.sample_size = 4U;
    manifest->discovery.sampled_pieces = { 0U, 1U, 2U, 3U };
    manifest->discovery.error = "ignored after success";
    manifest->set_all_piece_states(tr_usenet_piece_state::Available);
    ASSERT_TRUE(store.save(*manifest));

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(tr_usenet_discovery_state::Available, loaded->discovery.state);
    EXPECT_EQ(12345U, loaded->discovery.checked_at);
    EXPECT_EQ(4U, loaded->discovery.sample_size);
    EXPECT_EQ((std::vector<tr_piece_index_t>{ 0U, 1U, 2U, 3U }), loaded->discovery.sampled_pieces);
    EXPECT_EQ("ignored after success"sv, loaded->discovery.error);
    EXPECT_TRUE(loaded->has_meaningful_state());
    ASSERT_FALSE(std::empty(loaded->pieces));
    EXPECT_EQ(tr_usenet_piece_state::Available, loaded->pieces.front().state);
}

TEST_F(UsenetPieceStoreTest, discoverySamplePiecesAreDeterministicBoundedAndUseful)
{
    auto const samples = tr_usenet_discovery_sample_pieces("0123456789012345678901234567890123456789"sv, 100U, 16U);
    auto const again = tr_usenet_discovery_sample_pieces("0123456789012345678901234567890123456789"sv, 100U, 16U);
    EXPECT_EQ(samples, again);
    EXPECT_EQ(16U, std::size(samples));
    EXPECT_TRUE(std::ranges::is_sorted(samples));
    EXPECT_EQ(std::end(samples), std::adjacent_find(std::begin(samples), std::end(samples)));
    EXPECT_NE(std::end(samples), std::ranges::find(samples, 0U));
    EXPECT_NE(std::end(samples), std::ranges::find(samples, 50U));
    EXPECT_NE(std::end(samples), std::ranges::find(samples, 99U));

    EXPECT_EQ((std::vector<tr_piece_index_t>{ 0U }), tr_usenet_discovery_sample_pieces("hash"sv, 1U, 16U));
    EXPECT_TRUE(std::empty(tr_usenet_discovery_sample_pieces("hash"sv, 100U, 0U)));
    EXPECT_TRUE(std::empty(tr_usenet_discovery_sample_pieces("hash"sv, 0U, 16U)));
}

TEST_F(UsenetPieceStoreTest, notePieceLocalActivityUpdatesTimestamp)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(0U, loaded->pieces[0].last_local_at);

    EXPECT_FALSE(store.note_piece_local_activity(metainfo.info_hash_string(), 0));

    loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_GT(loaded->pieces[0].last_local_at, 0U);
}

TEST_F(UsenetPieceStoreTest, loadsManifestWithoutTimestamps)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };

    auto file = std::ofstream{ store.manifest_path(metainfo.info_hash_string()) };
    file << fmt::format(
        R"({{"version":1,"hash_string":"{}","piece_count":1,"piece_size":{},"max_article_size":{},"pieces":[{{"status":"available","message_id":"{}@nashawk.local"}}]}})",
        metainfo.info_hash_string(),
        metainfo.piece_size(),
        metainfo.piece_size(),
        tr_sha1_to_string(metainfo.piece_hash(0)));
    file.close();

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(1U, loaded->version);
    ASSERT_EQ(1U, loaded->piece_count());
    EXPECT_TRUE(loaded->is_available(0));
    EXPECT_EQ(0U, loaded->pieces[0].available_at);
    EXPECT_EQ(0U, loaded->pieces[0].last_local_at);

    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 0U, tr_usenet_piece_state::Available));
    loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(TrUsenetPieceManifestVersion, loaded->version);
}

TEST_F(UsenetPieceStoreTest, setPieceStateUpdatesManifest)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    EXPECT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 1, tr_usenet_piece_state::Uploading));

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    ASSERT_LT(1U, loaded->piece_count());
    EXPECT_EQ(tr_usenet_piece_state::Uploading, loaded->pieces[1].state);
}

TEST_F(UsenetPieceStoreTest, setMessageIdStateUpdatesDuplicatePieces)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    auto manifest = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(manifest);
    ASSERT_LT(2U, manifest->piece_count());

    auto const message_id = manifest->pieces[0].message_id;
    manifest->pieces[1].message_id = message_id;
    manifest->pieces[2].message_id = "other@nashawk.local";
    ASSERT_TRUE(store.save(*manifest));

    EXPECT_FALSE(store.set_message_id_state(
        metainfo.info_hash_string(),
        message_id,
        tr_usenet_piece_state::Available,
        3U,
        2U * 1024U * 1024U));

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(tr_usenet_piece_state::Available, loaded->pieces[0].state);
    EXPECT_EQ(tr_usenet_piece_state::Available, loaded->pieces[1].state);
    EXPECT_EQ(3U, loaded->pieces[0].article_count);
    EXPECT_EQ(3U, loaded->pieces[1].article_count);
    EXPECT_EQ(2U * 1024U * 1024U, loaded->pieces[0].article_payload_size);
    EXPECT_EQ(2U * 1024U * 1024U, loaded->pieces[1].article_payload_size);
    EXPECT_EQ(tr_usenet_piece_state::Unknown, loaded->pieces[2].state);
    EXPECT_TRUE(loaded->has_message_id_state(message_id, tr_usenet_piece_state::Available));

    EXPECT_FALSE(store.set_message_id_state(metainfo.info_hash_string(), message_id, tr_usenet_piece_state::Failed));
    loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(0U, loaded->pieces[0].article_count);
    EXPECT_EQ(0U, loaded->pieces[1].article_count);
    EXPECT_EQ(0U, loaded->pieces[0].article_payload_size);
    EXPECT_EQ(0U, loaded->pieces[1].article_payload_size);
}

TEST_F(UsenetPieceStoreTest, manifestVersionTwoNormalizesUnknownChainMetadata)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };

    auto file = std::ofstream{ store.manifest_path(metainfo.info_hash_string()) };
    file << fmt::format(
        R"({{"version":2,"hash_string":"{}","piece_count":1,"piece_size":{},"max_article_size":{},"pieces":[{{"status":"available","message_id":"{}@nashawk.local","article_count":0,"article_payload_size":123}}]}})",
        metainfo.info_hash_string(),
        metainfo.piece_size(),
        metainfo.piece_size(),
        tr_sha1_to_string(metainfo.piece_hash(0)));
    file.close();

    auto const loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    ASSERT_EQ(1U, loaded->piece_count());
    EXPECT_EQ(0U, loaded->pieces[0].article_count);
    EXPECT_EQ(0U, loaded->pieces[0].article_payload_size);
    EXPECT_EQ(0U, loaded->pieces[0].verified_at);
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(loaded->pieces[0], true, 1000U, 0U));
}

TEST_F(UsenetPieceStoreTest, rejectsUnsupportedManifestVersionAndArticleCount)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    auto const path = store.manifest_path(metainfo.info_hash_string());
    auto const message_id = fmt::format("{}@nashawk.local", tr_sha1_to_string(metainfo.piece_hash(0)));

    {
        auto file = std::ofstream{ path };
        file << fmt::format(
            R"({{"version":{},"hash_string":"{}","piece_count":1,"piece_size":{},"pieces":[{{"status":"available","message_id":"{}"}}]}})",
            TrUsenetPieceManifestVersion + 1U,
            metainfo.info_hash_string(),
            metainfo.piece_size(),
            message_id);
    }
    EXPECT_FALSE(store.load(metainfo.info_hash_string()));

    {
        auto file = std::ofstream{ path };
        file << fmt::format(
            R"({{"version":2,"hash_string":"{}","piece_count":1,"piece_size":{},"pieces":[{{"status":"available","message_id":"{}","article_count":1025}}]}})",
            metainfo.info_hash_string(),
            metainfo.piece_size(),
            message_id);
    }
    EXPECT_FALSE(store.load(metainfo.info_hash_string()));

    {
        auto file = std::ofstream{ path };
        file << fmt::format(
            R"({{"version":2,"hash_string":"{}","piece_count":1,"piece_size":{},"pieces":[{{"status":"available","message_id":"{}","article_count":-1}}]}})",
            metainfo.info_hash_string(),
            metainfo.piece_size(),
            message_id);
    }
    EXPECT_FALSE(store.load(metainfo.info_hash_string()));

    {
        auto file = std::ofstream{ path };
        file << fmt::format(
            R"({{"version":2,"hash_string":"{}","piece_count":1,"piece_size":{},"pieces":[{{"status":"available","message_id":"{}","article_count":1,"article_payload_size":-1}}]}})",
            metainfo.info_hash_string(),
            metainfo.piece_size(),
            message_id);
    }
    EXPECT_FALSE(store.load(metainfo.info_hash_string()));
}

TEST_F(UsenetPieceStoreTest, resetInterruptedUploads)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    ASSERT_FALSE(
        store.set_piece_state(metainfo.info_hash_string(), 0, tr_usenet_piece_state::Uploading, 2U, 2U * 1024U * 1024U));
    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 1, tr_usenet_piece_state::Available));
    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 2, tr_usenet_piece_state::Failed));
    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 3, tr_usenet_piece_state::Uploading));

    auto pieces = std::vector<tr_piece_index_t>{};
    EXPECT_FALSE(store.reset_interrupted_uploads(metainfo.info_hash_string(), pieces));
    EXPECT_EQ((std::vector<tr_piece_index_t>{ 0U, 3U }), pieces);

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(tr_usenet_piece_state::Unknown, loaded->pieces[0].state);
    EXPECT_EQ(0U, loaded->pieces[0].article_count);
    EXPECT_EQ(0U, loaded->pieces[0].article_payload_size);
    EXPECT_EQ(tr_usenet_piece_state::Available, loaded->pieces[1].state);
    EXPECT_EQ(tr_usenet_piece_state::Failed, loaded->pieces[2].state);
    EXPECT_EQ(tr_usenet_piece_state::Unknown, loaded->pieces[3].state);

    EXPECT_FALSE(store.reset_interrupted_uploads(metainfo.info_hash_string(), pieces));
    EXPECT_TRUE(std::empty(pieces));
}

TEST_F(UsenetPieceStoreTest, pieceEntryLoadsSinglePiece)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    auto entry = store.piece_entry(metainfo.info_hash_string(), 2);
    ASSERT_TRUE(entry);
    EXPECT_EQ(tr_usenet_piece_state::Unknown, entry->state);
    EXPECT_EQ(fmt::format("{}@nashawk.local", tr_sha1_to_string(metainfo.piece_hash(2))), entry->message_id);

    EXPECT_FALSE(store.piece_entry(metainfo.info_hash_string(), metainfo.piece_count()));
    EXPECT_FALSE(store.piece_entry("0123456789012345678901234567890123456789"sv, 0));
}

TEST_F(UsenetPieceStoreTest, setPieceStateRejectsMissingManifest)
{
    auto store = tr_usenet_piece_store{ sandboxDir(), 1U };

    auto error = store.set_piece_state("0123456789012345678901234567890123456789"sv, 0, tr_usenet_piece_state::Uploading);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("missing"));
}

TEST_F(UsenetPieceStoreTest, setPieceStateRejectsOutOfRangePiece)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    auto error = store.set_piece_state(metainfo.info_hash_string(), metainfo.piece_count(), tr_usenet_piece_state::Uploading);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("out of range"));
}

TEST_F(UsenetPieceStoreTest, acceptsPieceSpanningMultipleArticles)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() - 1U };

    EXPECT_FALSE(store.ensure_torrent(metainfo));
    EXPECT_TRUE(store.load(metainfo.info_hash_string()));
}

TEST_F(UsenetPieceStoreTest, rejectsPieceExceedingArticleCountSafetyBound)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), 1U };

    auto const error = store.ensure_torrent(metainfo);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("1024 article safety limit"));
    EXPECT_FALSE(store.load(metainfo.info_hash_string()));
}

TEST_F(UsenetPieceStoreTest, rejectsZeroArticlePayloadLimit)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), 0U };

    auto const error = store.ensure_torrent(metainfo);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("must be positive"));
    EXPECT_FALSE(store.load(metainfo.info_hash_string()));
}

} // namespace tr::test
