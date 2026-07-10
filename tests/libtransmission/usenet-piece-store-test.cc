// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

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
}

TEST_F(UsenetPieceStoreTest, evictionEligibilityRequiresAvailableLocalOldPiece)
{
    auto entry = tr_usenet_piece_entry{
        .state = tr_usenet_piece_state::Available,
        .available_at = 100U,
        .last_local_at = 100U,
        .message_id = "piece@nashawk.local",
    };

    EXPECT_TRUE(tr_usenet_piece_is_eviction_eligible(entry, true, 200U, 60U));
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, false, 200U, 60U));
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 120U, 60U));
    EXPECT_FALSE(tr_usenet_piece_is_eviction_eligible(entry, true, 99U, 60U));

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
    EXPECT_TRUE(loaded->is_available(0));
    EXPECT_GT(loaded->pieces[0].available_at, 0U);
    EXPECT_GT(loaded->pieces[0].last_local_at, 0U);
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
    ASSERT_EQ(1U, loaded->piece_count());
    EXPECT_TRUE(loaded->is_available(0));
    EXPECT_EQ(0U, loaded->pieces[0].available_at);
    EXPECT_EQ(0U, loaded->pieces[0].last_local_at);
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

    EXPECT_FALSE(store.set_message_id_state(metainfo.info_hash_string(), message_id, tr_usenet_piece_state::Available));

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(tr_usenet_piece_state::Available, loaded->pieces[0].state);
    EXPECT_EQ(tr_usenet_piece_state::Available, loaded->pieces[1].state);
    EXPECT_EQ(tr_usenet_piece_state::Unknown, loaded->pieces[2].state);
    EXPECT_TRUE(loaded->has_message_id_state(message_id, tr_usenet_piece_state::Available));
}

TEST_F(UsenetPieceStoreTest, resetInterruptedUploads)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() };
    ASSERT_FALSE(store.ensure_torrent(metainfo));

    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 0, tr_usenet_piece_state::Uploading));
    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 1, tr_usenet_piece_state::Available));
    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 2, tr_usenet_piece_state::Failed));
    ASSERT_FALSE(store.set_piece_state(metainfo.info_hash_string(), 3, tr_usenet_piece_state::Uploading));

    auto pieces = std::vector<tr_piece_index_t>{};
    EXPECT_FALSE(store.reset_interrupted_uploads(metainfo.info_hash_string(), pieces));
    EXPECT_EQ((std::vector<tr_piece_index_t>{ 0U, 3U }), pieces);

    auto loaded = store.load(metainfo.info_hash_string());
    ASSERT_TRUE(loaded);
    EXPECT_EQ(tr_usenet_piece_state::Unknown, loaded->pieces[0].state);
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

TEST_F(UsenetPieceStoreTest, rejectsOversizedPiece)
{
    auto metainfo = load_metainfo("archlinux-2025.05.01-x86_64.iso.torrent"sv);
    auto store = tr_usenet_piece_store{ sandboxDir(), metainfo.piece_size() - 1U };

    auto error = store.ensure_torrent(metainfo);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("exceeds Usenet article size limit"));
    EXPECT_FALSE(store.load(metainfo.info_hash_string()));
}

} // namespace tr::test
