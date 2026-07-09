// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <string_view>

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
    for (auto const state : { tr_usenet_piece_state::Unknown, tr_usenet_piece_state::Uploading, tr_usenet_piece_state::Available,
             tr_usenet_piece_state::Failed })
    {
        EXPECT_EQ(state, tr_usenet_piece_state_from_name(tr_usenet_piece_state_name(state)));
    }

    EXPECT_FALSE(tr_usenet_piece_state_from_name("not-a-state"sv));
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
