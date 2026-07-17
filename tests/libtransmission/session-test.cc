// This file Copyright (C) 2013-2022 Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <array>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <libtransmission/transmission.h>

#include <libtransmission/crypto-utils.h>
#include <libtransmission/quark.h>
#include <libtransmission/session-id.h>
#include <libtransmission/session.h>
#include <libtransmission/variant.h>
#include <libtransmission/version.h>

#include "test-fixtures.h"

using namespace std::literals;

namespace tr::test
{

TEST_F(SessionTest, propertiesApi)
{
    // Note, this test is just for confirming that the getters/setters
    // in both the tr_session class and in the C API bindings all work,
    // e.g. you can get back the same value you set in.
    //
    // Confirming that each of these settings _does_ something in the session
    // is a much broader scope and left to other tests :)

    auto* const session = session_;

    // download dir

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        session->setDownloadDir(value);
        EXPECT_EQ(value, session->downloadDir());
        EXPECT_EQ(value, tr_sessionGetDownloadDir(session));

        tr_sessionSetDownloadDir(session, value);
        EXPECT_EQ(value, session->downloadDir());
        EXPECT_EQ(value, tr_sessionGetDownloadDir(session));
    }

    // incomplete dir

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        session->setIncompleteDir(value);
        EXPECT_EQ(value, session->incompleteDir());
        EXPECT_EQ(value, tr_sessionGetIncompleteDir(session));

        tr_sessionSetIncompleteDir(session, value);
        EXPECT_EQ(value, session->incompleteDir());
        EXPECT_EQ(value, tr_sessionGetIncompleteDir(session));
    }

    // script

    for (auto const& type : { TR_SCRIPT_ON_TORRENT_ADDED, TR_SCRIPT_ON_TORRENT_DONE })
    {
        for (auto const& value : { "foo"sv, "bar"sv, ""sv })
        {
            session->setScript(type, value);
            EXPECT_EQ(value, session->script(type));
            EXPECT_EQ(value, tr_sessionGetScript(session, type));

            tr_sessionSetScript(session, type, value);
            EXPECT_EQ(value, session->script(type));
            EXPECT_EQ(value, tr_sessionGetScript(session, type));
        }

        for (auto const value : { true, false })
        {
            session->useScript(type, value);
            EXPECT_EQ(value, session->useScript(type));
            EXPECT_EQ(value, tr_sessionIsScriptEnabled(session, type));

            tr_sessionSetScriptEnabled(session, type, value);
            EXPECT_EQ(value, session->useScript(type));
            EXPECT_EQ(value, tr_sessionIsScriptEnabled(session, type));
        }
    }

    // incomplete dir enabled

    for (auto const value : { true, false })
    {
        session->useIncompleteDir(value);
        EXPECT_EQ(value, session->useIncompleteDir());
        EXPECT_EQ(value, tr_sessionIsIncompleteDirEnabled(session));

        tr_sessionSetIncompleteDirEnabled(session, value);
        EXPECT_EQ(value, session->useIncompleteDir());
        EXPECT_EQ(value, tr_sessionIsIncompleteDirEnabled(session));
    }

    // blocklist url

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        session->setBlocklistUrl(value);
        EXPECT_EQ(value, session->blocklistUrl());
        EXPECT_EQ(value, tr_blocklistGetURL(session));

        tr_blocklistSetURL(session, value);
        EXPECT_EQ(value, session->blocklistUrl());
        EXPECT_EQ(value, tr_blocklistGetURL(session));
    }

    // rpc username

    for (auto const& value : { "foo"sv, "bar"sv, ""sv })
    {
        tr_sessionSetRPCUsername(session, value);
        EXPECT_EQ(value, tr_sessionGetRPCUsername(session));
    }

    // rpc password (unsalted)

    {
        auto const value = "foo"sv;
        tr_sessionSetRPCPassword(session, value);
        EXPECT_NE(value, tr_sessionGetRPCPassword(session));
        EXPECT_EQ('{', tr_sessionGetRPCPassword(session)[0]);
    }

    // rpc password (salted)

    {
        auto const plaintext = "foo"sv;
        auto const salted = tr_ssha1(plaintext);
        tr_sessionSetRPCPassword(session, salted);
        EXPECT_EQ(salted, tr_sessionGetRPCPassword(session));
    }

    // blocklist enabled

    for (auto const value : { true, false })
    {
        session->set_blocklist_enabled(value);
        EXPECT_EQ(value, session->blocklist_enabled());
        EXPECT_EQ(value, tr_blocklistIsEnabled(session));

        tr_sessionSetIncompleteDirEnabled(session, value);
        EXPECT_EQ(value, session->blocklist_enabled());
        EXPECT_EQ(value, tr_blocklistIsEnabled(session));
    }
}

TEST_F(SessionTest, peerId)
{
    auto const check_peer_id_prefix = [](std::string_view peer_id_prefix)
    {
        for (int i = 0; i < 100000; ++i)
        {
            // get a new peer-id
            auto const buf = tr_peerIdInit();

            // confirm that it begins with peer_id_prefix
            auto const peer_id = std::string_view{ reinterpret_cast<char const*>(buf.data()), std::size(buf) };
            EXPECT_EQ(peer_id_prefix, peer_id.substr(0, peer_id_prefix.size()));

            // confirm that its total is evenly divisible by 36
            int val = 0;
            auto const suffix = peer_id.substr(peer_id_prefix.size());
            for (char const ch : suffix)
            {
                auto const tmp = std::array<char, 2>{ ch, '\0' };
                val += strtoul(tmp.data(), nullptr, 36);
            }

            EXPECT_EQ(0, val % 36);
        }
    };

    tr_set_version_compat_enabled(true);
    check_peer_id_prefix(COMPAT_PEERID_PREFIX);

    tr_set_version_compat_enabled(false);
    check_peer_id_prefix(PEERID_PREFIX);

    tr_set_version_compat_enabled(true);
}

TEST_F(SessionTest, versionCompatibilityIdentity)
{
    tr_set_version_compat_enabled(true);
    EXPECT_STREQ("4.1.2", tr_display_short_version_string());
    EXPECT_STREQ("4.1.2 (f234716f3e)", tr_display_long_version_string());
    EXPECT_STREQ("Transmission", tr_display_client_name());
    EXPECT_STREQ("4.1.2", tr_display_user_agent_prefix());
    EXPECT_STREQ("-TR4120-", tr_display_peer_id_prefix());

    tr_set_version_compat_enabled(false);
    EXPECT_STREQ(LONG_VERSION_STRING, tr_display_long_version_string());

    tr_set_version_compat_enabled(true);
}

namespace current_time_mock
{
namespace
{

auto value = time_t{};

time_t get()
{
    return value;
}

void set(time_t now)
{
    value = now;
}

} // unnamed namespace
} // namespace current_time_mock

TEST_F(SessionTest, sessionId)
{
#ifdef __sun
    // FIXME: File locking doesn't work as expected
    GTEST_SKIP();
#endif

    EXPECT_FALSE(tr_session_id::is_local(""));
    EXPECT_FALSE(tr_session_id::is_local("test"));

    current_time_mock::set(0U);
    auto session_id = std::make_unique<tr_session_id>(current_time_mock::get);

    EXPECT_NE(""sv, session_id->sv());
    EXPECT_EQ(session_id->sv(), session_id->c_str()) << session_id->sv() << ", " << session_id->c_str();
    EXPECT_EQ(48U, strlen(session_id->c_str()));
    auto session_id_str_1 = std::string{ session_id->sv() };
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));

    current_time_mock::set(current_time_mock::get() + (3600U - 1U));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));
    auto session_id_str_2 = std::string{ session_id->sv() };
    EXPECT_EQ(session_id_str_1, session_id_str_2);

    current_time_mock::set(3600U);
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));
    session_id_str_2 = std::string{ session_id->sv() };
    EXPECT_NE(session_id_str_1, session_id_str_2);
    EXPECT_EQ(session_id_str_2, session_id->c_str());
    EXPECT_EQ(48U, strlen(session_id->c_str()));

    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));
    current_time_mock::set(7200U);
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_1));

    auto const session_id_str_3 = std::string{ session_id->sv() };
    EXPECT_EQ(48U, std::size(session_id_str_3));
    EXPECT_NE(session_id_str_2, session_id_str_3);
    EXPECT_NE(session_id_str_1, session_id_str_3);

    EXPECT_TRUE(tr_session_id::is_local(session_id_str_3));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_1));

    current_time_mock::set(36000U);
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_3));
    EXPECT_TRUE(tr_session_id::is_local(session_id_str_2));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_1));

    session_id.reset();
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_3));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_2));
    EXPECT_FALSE(tr_session_id::is_local(session_id_str_1));
}

TEST_F(SessionTest, getDefaultSettingsIncludesSubmodules)
{
    auto settings = tr_sessionGetDefaultSettings();
    auto* settings_map = settings.get_if<tr_variant::Map>();
    ASSERT_NE(settings_map, nullptr);

    // Choose a setting from each of [tr_session, tr_session_alt_speeds, tr_rpc_server] to test all of them.
    // These are all `false` by default
    for (auto const& key : { TR_KEY_peer_port_random_on_start, TR_KEY_alt_speed_time_enabled, TR_KEY_rpc_enabled })
    {
        auto flag = settings_map->value_if<bool>(key);
        ASSERT_TRUE(flag);
        EXPECT_FALSE(*flag);
    }
}

TEST_F(SessionTest, honorsSettings)
{
    // Baseline: confirm that these settings are disabled by default
    EXPECT_FALSE(session_->isPortRandom());
    EXPECT_FALSE(tr_sessionUsesAltSpeedTime(session_));
    EXPECT_FALSE(tr_sessionIsRPCEnabled(session_));

    // Choose a setting from each of [tr_session, tr_session_alt_speeds, tr_rpc_server] to test all of them.
    // These are all `false` by default
    auto settings = tr_sessionGetDefaultSettings();
    auto* settings_map = settings.get_if<tr_variant::Map>();
    ASSERT_NE(settings_map, nullptr);
    for (auto const& key : { TR_KEY_peer_port_random_on_start, TR_KEY_alt_speed_time_enabled, TR_KEY_rpc_enabled })
    {
        settings_map->insert_or_assign(key, true);
    }
    auto* session = tr_sessionInit(sandboxDir(), false, settings);

    // confirm that these settings were enabled
    EXPECT_TRUE(session->isPortRandom());
    EXPECT_TRUE(tr_sessionUsesAltSpeedTime(session));
    EXPECT_TRUE(tr_sessionIsRPCEnabled(session));

    tr_sessionClose(session, 0.5);
}

TEST_F(SessionTest, usenetInitializationFailureKeepsNewTorrentStopped)
{
    auto const config_dir = tr_pathbuf{ sandboxDir(), "/usenet-init-failure"sv };
    ASSERT_TRUE(tr_sys_dir_create(config_dir, TR_SYS_DIR_CREATE_PARENTS, 0700));

    auto settings = tr_sessionGetDefaultSettings();
    auto* const settings_map = settings.get_if<tr_variant::Map>();
    ASSERT_NE(settings_map, nullptr);
    settings_map->insert_or_assign(TR_KEY_download_dir, tr_pathbuf{ config_dir, "/downloads"sv }.sv());
    settings_map->insert_or_assign(TR_KEY_usenet_enabled, true);
    settings_map->insert_or_assign(TR_KEY_usenet_check_article_size, int64_t{ 1 });
    settings_map->insert_or_assign(TR_KEY_usenet_discovery_enabled, false);
    settings_map->insert_or_assign(TR_KEY_peer_port_random_on_start, true);
    settings_map->insert_or_assign(TR_KEY_port_forwarding_enabled, false);

    auto* const session = tr_sessionInit(config_dir, false, settings);
    ASSERT_NE(session, nullptr);

    auto* const ctor = tr_ctorNew(session);
    ASSERT_TRUE(ctor->set_metainfo_from_file(
        tr_pathbuf{ LIBTRANSMISSION_TEST_ASSETS_DIR, "/archlinux-2025.05.01-x86_64.iso.torrent"sv }));
    tr_ctorSetPaused(ctor, TR_FORCE, false);

    auto* const tor = tr_torrentNew(ctor, nullptr);
    ASSERT_NE(tor, nullptr);
    tr_ctorFree(ctor);

    auto const stats = tr_torrentStat(tor);
    EXPECT_EQ(TR_STATUS_STOPPED, stats.activity);
    EXPECT_EQ(tr_stat::Error::LocalError, stats.error);
    EXPECT_NE(std::string::npos, stats.error_string.find("1024 article safety limit"));

    tr_sessionClose(session, 0.5);
}

TEST_F(SessionTest, usenetMultipartPieceSizeIsAdmittedWithoutUnpromptedDiscovery)
{
    auto const config_dir = tr_pathbuf{ sandboxDir(), "/usenet-multipart-admission"sv };
    ASSERT_TRUE(tr_sys_dir_create(config_dir, TR_SYS_DIR_CREATE_PARENTS, 0700));

    auto settings = tr_sessionGetDefaultSettings();
    auto* const settings_map = settings.get_if<tr_variant::Map>();
    ASSERT_NE(settings_map, nullptr);
    settings_map->insert_or_assign(TR_KEY_download_dir, tr_pathbuf{ config_dir, "/downloads"sv }.sv());
    settings_map->insert_or_assign(TR_KEY_usenet_enabled, true);
    settings_map->insert_or_assign(TR_KEY_usenet_check_article_size, int64_t{ 256U * 1024U });
    settings_map->insert_or_assign(TR_KEY_usenet_discovery_enabled, true);
    settings_map->insert_or_assign(TR_KEY_peer_port_random_on_start, true);
    settings_map->insert_or_assign(TR_KEY_port_forwarding_enabled, false);

    auto* const session = tr_sessionInit(config_dir, false, settings);
    ASSERT_NE(session, nullptr);

    auto* const ctor = tr_ctorNew(session);
    ASSERT_TRUE(ctor->set_metainfo_from_file(
        tr_pathbuf{ LIBTRANSMISSION_TEST_ASSETS_DIR, "/archlinux-2025.05.01-x86_64.iso.torrent"sv }));
    tr_ctorSetPaused(ctor, TR_FORCE, true);

    auto* const tor = tr_torrentNew(ctor, nullptr);
    ASSERT_NE(tor, nullptr);
    tr_ctorFree(ctor);

    auto const stats = tr_torrentStat(tor);
    EXPECT_EQ(tr_stat::Error::Ok, stats.error);

    auto const summary = session->usenetPieceSummary(*tor);
    EXPECT_TRUE(summary.eligible);
    EXPECT_TRUE(summary.manifest_present);
    EXPECT_EQ(tr_usenet_discovery_state::NotChecked, summary.discovery.state);
    EXPECT_EQ(tr_usenet_discovery_trigger::None, summary.discovery.trigger);
    EXPECT_TRUE(summary.discovery.attempted_pieces.empty());
    EXPECT_TRUE(summary.discovery.duplicate_verified_pieces.empty());

    auto store = tr_usenet_piece_store{ config_dir, 256U * 1024U };
    auto manifest = store.load(tor->info_hash_string());
    ASSERT_TRUE(manifest);
    manifest->discovery.state = tr_usenet_discovery_state::Checking;
    ASSERT_TRUE(store.save(*manifest));
    EXPECT_EQ(
        (std::optional<std::string>{ "Usenet discovery is already running" }),
        session->queueUsenetIntegrityAudit(*tor, true));

    tr_sessionClose(session, 0.5);
}

TEST_F(SessionTest, usenetManifestWriteFailureKeepsNewTorrentStopped)
{
    auto const config_dir = tr_pathbuf{ sandboxDir(), "/usenet-manifest-write-failure"sv };
    ASSERT_TRUE(tr_sys_dir_create(config_dir, TR_SYS_DIR_CREATE_PARENTS, 0700));
    createFileWithContents(tr_pathbuf{ config_dir, "/usenet-pieces"sv }, "not a directory", 15U);

    auto settings = tr_sessionGetDefaultSettings();
    auto* const settings_map = settings.get_if<tr_variant::Map>();
    ASSERT_NE(settings_map, nullptr);
    settings_map->insert_or_assign(TR_KEY_download_dir, tr_pathbuf{ config_dir, "/downloads"sv }.sv());
    settings_map->insert_or_assign(TR_KEY_usenet_enabled, true);
    settings_map->insert_or_assign(TR_KEY_usenet_check_article_size, int64_t{ 1024U * 1024U });
    settings_map->insert_or_assign(TR_KEY_usenet_discovery_enabled, false);
    settings_map->insert_or_assign(TR_KEY_peer_port_random_on_start, true);
    settings_map->insert_or_assign(TR_KEY_port_forwarding_enabled, false);

    auto* const session = tr_sessionInit(config_dir, false, settings);
    ASSERT_NE(session, nullptr);

    auto* const ctor = tr_ctorNew(session);
    ASSERT_TRUE(ctor->set_metainfo_from_file(
        tr_pathbuf{ LIBTRANSMISSION_TEST_ASSETS_DIR, "/archlinux-2025.05.01-x86_64.iso.torrent"sv }));
    tr_ctorSetPaused(ctor, TR_FORCE, false);

    auto* const tor = tr_torrentNew(ctor, nullptr);
    ASSERT_NE(tor, nullptr);
    tr_ctorFree(ctor);

    auto const stats = tr_torrentStat(tor);
    EXPECT_EQ(TR_STATUS_STOPPED, stats.activity);
    EXPECT_EQ(tr_stat::Error::LocalError, stats.error);
    EXPECT_EQ("Could not save Usenet piece manifest", stats.error_string);

    tr_sessionClose(session, 0.5);
}

TEST_F(SessionTest, savesSettings)
{
    // Baseline: confirm that these settings are disabled by default
    EXPECT_FALSE(session_->isPortRandom());
    EXPECT_FALSE(tr_sessionUsesAltSpeedTime(session_));
    EXPECT_FALSE(tr_sessionIsRPCEnabled(session_));

    tr_sessionSetPeerPortRandomOnStart(session_, true);
    tr_sessionUseAltSpeedTime(session_, true);
    tr_sessionSetRPCEnabled(session_, true);

    // Choose a setting from each of [tr_session, tr_session_alt_speeds, tr_rpc_server] to test all of them.
    auto settings = tr_sessionGetSettings(session_);
    auto* settings_map = settings.get_if<tr_variant::Map>();
    ASSERT_NE(settings_map, nullptr);
    for (auto const& key : { TR_KEY_peer_port_random_on_start, TR_KEY_alt_speed_time_enabled, TR_KEY_rpc_enabled })
    {
        auto flag = settings_map->value_if<bool>(key);
        ASSERT_TRUE(flag);
        EXPECT_TRUE(*flag);
    }
}

TEST_F(SessionTest, loadTorrentsThenMagnets)
{
    static auto constexpr TorrentFile = LIBTRANSMISSION_TEST_ASSETS_DIR "/archlinux-2025.05.01-x86_64.iso.torrent";
    static auto constexpr MagnetFile = LIBTRANSMISSION_TEST_ASSETS_DIR "/archlinux-2025.05.01-x86_64.iso.magnet";

    if (auto error = tr_error{};
        !tr_sys_path_copy(
            TorrentFile,
            tr_pathbuf{ session_->torrentDir(), "/2e34989b1c60df821b2d046c884d8f4d1858b97a.torrent"sv },
            &error) ||
        !tr_sys_path_copy(
            MagnetFile,
            tr_pathbuf{ session_->torrentDir(), "/2e34989b1c60df821b2d046c884d8f4d1858b97a.magnet"sv },
            &error))
    {
        GTEST_SKIP() << fmt::format("Failed to setup torrents dir: {} ({})", error.message(), error.code());
    }

    auto* const ctor = tr_ctorNew(session_);
    ctor->set_paused(TR_FORCE, false);
    EXPECT_EQ(tr_sessionLoadTorrents(session_, ctor), 1U);
    tr_ctorFree(ctor);

    auto* const tor = session_->torrents().get(1U);
    ASSERT_NE(tor, nullptr);

    EXPECT_TRUE(tor->has_metainfo());
}

} // namespace tr::test
