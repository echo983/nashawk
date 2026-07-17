// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <algorithm> // std::partial_sort(), std::min(), std::max()
#include <condition_variable>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstddef> // size_t
#include <cstdint>
#include <ctime>
#include <fstream>
#include <future>
#include <iterator> // for std::back_inserter
#include <limits> // std::numeric_limits
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h> /* umask() */
#include <unistd.h>
#endif

#include <event2/event.h>

#include <fmt/format.h> // fmt::ptr

#include "libtransmission/transmission.h"

#include "libtransmission/api-compat.h"
#include "libtransmission/bandwidth.h"
#include "libtransmission/blocklist.h"
#include "libtransmission/crypto-utils.h"
#include "libtransmission/file-utils.h"
#include "libtransmission/file.h"
#include "libtransmission/inout.h"
#include "libtransmission/ip-cache.h"
#include "libtransmission/interned-string.h"
#include "libtransmission/log.h"
#include "libtransmission/net.h"
#include "libtransmission/peer-mgr.h"
#include "libtransmission/peer-socket-tcp.h"
#include "libtransmission/peer-socket.h"
#include "libtransmission/port-forwarding.h"
#include "libtransmission/quark.h"
#include "libtransmission/rpc-server.h"
#include "libtransmission/session-alt-speeds.h"
#include "libtransmission/session.h"
#include "libtransmission/string-utils.h"
#include "libtransmission/timer-ev.h"
#include "libtransmission/torrent.h"
#include "libtransmission/torrent-ctor.h"
#include "libtransmission/usenet-piece-store.h"
#include "libtransmission/usenet-service.h"
#include "libtransmission/tr-assert.h"
#include "libtransmission/tr-dht.h"
#include "libtransmission/tr-lpd.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/tr-utp.h"
#include "libtransmission/types.h"
#include "libtransmission/variant.h"
#include "libtransmission/version.h"
#include "libtransmission/web.h"

struct tr_ctor;

using namespace std::literals;
using namespace tr::Values;

namespace
{
auto constexpr UsenetEvictionInterval = 5min;
auto constexpr MaxConcurrentNyuuUploads = size_t{ 2U };
auto constexpr MaxNyuuBatchFiles = size_t{ 40U };
auto constexpr MaxNyuuBatchConnections = size_t{ 8U };
auto constexpr NyuuBatchCheckConnections = size_t{ 1U };

[[nodiscard]] bool punch_file_hole(std::string_view const path, uint64_t const offset, uint64_t const length, tr_error& error)
{
    if (length == 0U)
    {
        return true;
    }

#if defined(HAVE_FALLOCATE64) && defined(FALLOC_FL_PUNCH_HOLE) && defined(FALLOC_FL_KEEP_SIZE)
    auto fd = tr_sys_file_open(path, TR_SYS_FILE_WRITE, 0, &error);
    if (fd == TR_BAD_SYS_FILE)
    {
        return false;
    }

    auto const ret = fallocate64(
        fd,
        FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
        static_cast<off64_t>(offset),
        static_cast<off64_t>(length));
    auto const saved_errno = errno;
    tr_sys_file_close(fd);

    if (ret == 0)
    {
        return true;
    }

    error.set_from_errno(saved_errno);
    return false;
#else
    error.set(ENOTSUP, "Filesystem hole punching is not supported by this build"sv);
    return false;
#endif
}

[[nodiscard]] bool punch_torrent_piece_holes(tr_torrent const& tor, tr_piece_index_t const piece, tr_error& error)
{
    auto [file_index, file_offset] = tor.file_offset(tor.piece_loc(piece));
    auto const bytes_left = tor.piece_size(piece);
    auto const bytes_in_file = tor.file_size(file_index) - file_offset;
    if (bytes_left > bytes_in_file)
    {
        error.set(ENOTSUP, "Cross-file Usenet piece eviction is not supported yet"sv);
        return false;
    }

    auto const found = tor.find_file(file_index);
    if (!found)
    {
        error.set(ENOENT, fmt::format("Could not find local file '{}'", tor.file_subpath(file_index)));
        return false;
    }

    return punch_file_hole(found->filename(), file_offset, bytes_left, error);
}

struct TempFileGuard
{
    explicit TempFileGuard(std::string path_in)
        : path{ std::move(path_in) }
    {
    }

    TempFileGuard(TempFileGuard const&) = delete;
    TempFileGuard& operator=(TempFileGuard const&) = delete;

    ~TempFileGuard()
    {
        if (!std::empty(path))
        {
            tr_sys_path_remove(path);
        }
    }

    [[nodiscard]] std::string release()
    {
        return std::exchange(path, {});
    }

    std::string path;
};

struct TempDirGuard
{
    explicit TempDirGuard(std::string path_in)
        : path{ std::move(path_in) }
    {
    }

    TempDirGuard(TempDirGuard const&) = delete;
    TempDirGuard& operator=(TempDirGuard const&) = delete;

    ~TempDirGuard()
    {
        for (auto const& file : files)
        {
            tr_sys_path_remove(file);
        }

        if (!std::empty(path))
        {
            tr_sys_path_remove(path);
        }
    }

    [[nodiscard]] std::string release()
    {
        files.clear();
        return std::exchange(path, {});
    }

    std::string path;
    std::vector<std::string> files;
};

[[nodiscard]] std::string usenet_temp_dir(std::string_view config_dir)
{
    return std::string{ tr_pathbuf{ config_dir, "/usenet-upload-temp"sv }.sv() };
}

[[nodiscard]] std::optional<std::string> make_usenet_batch_temp_dir(std::string_view const config_dir, std::string& dirname)
{
    auto const parent = usenet_temp_dir(config_dir);
    if (auto error = tr_error{}; !tr_sys_dir_create(parent, TR_SYS_DIR_CREATE_PARENTS, 0700, &error))
    {
        return fmt::format("Could not create Usenet upload temp dir: {}", error.message());
    }

    auto path = std::string{ tr_pathbuf{ parent, "/batch.XXXXXX"sv }.sv() };
    auto error = tr_error{};
    if (!tr_sys_dir_create_temp(std::data(path), &error))
    {
        return fmt::format("Could not create Usenet batch temp dir: {}", error.message());
    }

    dirname = std::move(path);
    return {};
}

[[nodiscard]] std::string message_id_local_part(std::string_view message_id)
{
    if (!std::empty(message_id) && message_id.front() == '<')
    {
        message_id.remove_prefix(1U);
    }

    if (!std::empty(message_id) && message_id.back() == '>')
    {
        message_id.remove_suffix(1U);
    }

    auto const at = message_id.find('@');
    return std::string{ message_id.substr(0U, at) };
}

[[nodiscard]] std::optional<std::string> write_piece_to_temp_file(
    tr_torrent const& tor,
    tr_piece_index_t const piece,
    std::string_view const config_dir,
    std::string& filename)
{
    auto const dir = usenet_temp_dir(config_dir);
    if (auto error = tr_error{}; !tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0700, &error))
    {
        return fmt::format("Could not create Usenet upload temp dir: {}", error.message());
    }

    auto path = std::string{ tr_pathbuf{ dir, "/piece.XXXXXX"sv }.sv() };
    auto error = tr_error{};
    auto fd = tr_sys_file_open_temp(std::data(path), &error);
    if (fd == TR_BAD_SYS_FILE)
    {
        return fmt::format("Could not create Usenet upload temp file: {}", error.message());
    }

    auto guard = TempFileGuard{ path };
    auto buffer = std::array<uint8_t, tr_block_info::BlockSize>{};
    auto& open_files = tor.session->openFiles();

    auto const [begin_byte, end_byte] = tor.block_info().byte_span_for_piece(piece);
    auto const [begin_block, end_block] = tor.block_span_for_piece(piece);
    auto n_bytes_written = uint64_t{};

    for (auto block = begin_block; block < end_block; ++block)
    {
        auto const block_loc = tor.block_loc(block);
        auto const block_len = tor.block_size(block);
        auto contents = std::span{ std::data(buffer), block_len };
        if (tr_ioRead(tor, open_files, block_loc, contents) != 0)
        {
            tr_sys_file_close(fd);
            return "Could not read completed piece for Usenet upload";
        }

        auto const start = std::max(begin_byte, block_loc.byte);
        auto const end = std::min(end_byte, block_loc.byte + block_len);
        auto const piece_data = contents.subspan(start - block_loc.byte, static_cast<size_t>(end - start));

        auto bytes_written = uint64_t{};
        if (!tr_sys_file_write(fd, std::data(piece_data), std::size(piece_data), &bytes_written, &error) ||
            bytes_written != std::size(piece_data))
        {
            tr_sys_file_close(fd);
            return fmt::format("Could not write Usenet upload temp file: {}", error.message());
        }

        n_bytes_written += bytes_written;
    }

    tr_sys_file_close(fd);

    if (n_bytes_written != tor.piece_size(piece))
    {
        return fmt::format("Usenet upload temp file has wrong size: {}", n_bytes_written);
    }

    filename = guard.release();
    return {};
}

[[nodiscard]] std::optional<std::string> read_file_bytes(std::string_view const filename, std::vector<uint8_t>& setme)
{
    auto file = std::ifstream{ std::string{ filename }, std::ios::binary };
    if (!file)
    {
        return "Could not read staged Usenet upload file";
    }

    file.seekg(0, std::ios::end);
    auto const size = file.tellg();
    if (size < 0)
    {
        return "Could not determine staged Usenet upload file size";
    }

    file.seekg(0, std::ios::beg);
    setme.resize(static_cast<size_t>(size));
    if (!std::empty(setme) &&
        !file.read(reinterpret_cast<char*>(std::data(setme)), static_cast<std::streamsize>(std::size(setme))))
    {
        return "Could not read staged Usenet upload file contents";
    }

    return {};
}

[[nodiscard]] std::optional<std::string> verify_usenet_upload_after_error(
    std::string_view const config_dir,
    std::string_view const message_id,
    std::string_view const temp_file,
    uint64_t const expected_size)
{
    auto local = std::vector<uint8_t>{};
    if (auto error = read_file_bytes(temp_file, local); error)
    {
        return error;
    }

    auto remote = std::vector<uint8_t>{};
    if (auto error = tr_usenet_download_piece(
            {
                .config_dir = config_dir,
                .message_id = message_id,
                .expected_size = expected_size,
                .expected_hash = {},
            },
            remote);
        error)
    {
        return error;
    }

    if (local != remote)
    {
        return "Usenet readback did not match staged upload file";
    }

    return {};
}

[[nodiscard]] bool upload_diagnostics_include_piece(
    tr_usenet_upload_diagnostics const& diagnostics,
    std::string_view const base_message_id,
    size_t const article_count)
{
    for (size_t article = 0U; article < article_count; ++article)
    {
        auto const message_id = tr_usenet_piece_article_message_id(base_message_id, article);
        if (message_id &&
            std::ranges::find(diagnostics.duplicate_message_ids, *message_id) != std::end(diagnostics.duplicate_message_ids))
        {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::optional<std::string> stage_usenet_piece_parts(
    std::string_view const source,
    std::string_view const batch_dir,
    std::string_view const base_message_id,
    uint64_t const piece_size,
    uint64_t const article_payload_size,
    std::vector<std::string>& setme)
{
    auto const plan = tr_usenet_piece_part_plan(piece_size, article_payload_size);
    if (!plan)
    {
        return "Could not plan multipart Usenet upload";
    }

    auto input = std::ifstream{ std::string{ source }, std::ios::binary };
    if (!input)
    {
        return "Could not open staged Usenet piece";
    }

    auto const fail = [&setme](std::string error)
    {
        for (auto const& path : setme)
        {
            tr_sys_path_remove(path);
        }
        setme.clear();
        return std::optional<std::string>{ std::move(error) };
    };

    for (auto const& part : *plan)
    {
        auto const message_id = tr_usenet_piece_article_message_id(base_message_id, part.index);
        if (!message_id)
        {
            return fail("Could not derive multipart Usenet Message-ID");
        }

        auto const path = std::string{ tr_pathbuf{ batch_dir, '/', message_id_local_part(*message_id) }.sv() };
        auto output = std::ofstream{ path, std::ios::binary | std::ios::trunc };
        if (!output)
        {
            return fail("Could not create multipart Usenet staging file");
        }

        auto remaining = part.size;
        auto buffer = std::array<char, 64U * 1024U>{};
        while (remaining != 0U)
        {
            auto const count = static_cast<std::streamsize>(std::min<uint64_t>(remaining, std::size(buffer)));
            if (!input.read(std::data(buffer), count) || !output.write(std::data(buffer), count))
            {
                output.close();
                tr_sys_path_remove(path);
                return fail("Could not split multipart Usenet staging file");
            }
            remaining -= static_cast<uint64_t>(count);
        }
        setme.push_back(path);
    }

    return {};
}

namespace bandwidth_group_helpers
{
auto constexpr BandwidthGroupsFilename = "bandwidth-groups.json"sv;

void bandwidthGroupRead(tr_session* session, std::string_view config_dir)
{
    auto const filename = tr_pathbuf{ config_dir, '/', BandwidthGroupsFilename };
    if (!tr_sys_path_exists(filename))
    {
        return;
    }

    auto groups_var = tr_variant_serde::json().parse_file(filename);
    if (!groups_var)
    {
        return;
    }
    tr::api_compat::convert_incoming_data(*groups_var);

    auto const* const groups_map = groups_var->get_if<tr_variant::Map>();
    if (groups_map == nullptr)
    {
        return;
    }

    for (auto const& [key, group_var] : *groups_map)
    {
        auto const* const group_map = group_var.get_if<tr_variant::Map>();
        if (group_map == nullptr)
        {
            continue;
        }

        auto& group = session->getBandwidthGroup(tr_interned_string{ key });
        auto limits = tr_bandwidth_limits{};

        if (auto const val = group_map->value_if<bool>(TR_KEY_upload_limited); val)
        {
            limits.up_limited = *val;
        }

        if (auto const val = group_map->value_if<bool>(TR_KEY_download_limited); val)
        {
            limits.down_limited = *val;
        }

        if (auto const val = group_map->value_if<int64_t>(TR_KEY_upload_limit); val)
        {
            limits.up_limit = Speed{ *val, Speed::Units::KByps };
        }

        if (auto const val = group_map->value_if<int64_t>(TR_KEY_download_limit); val)
        {
            limits.down_limit = Speed{ *val, Speed::Units::KByps };
        }

        group.set_limits(limits);

        if (auto const val = group_map->value_if<bool>(TR_KEY_honors_session_limits); val)
        {
            group.honor_parent_limits(tr_direction::Up, *val);
            group.honor_parent_limits(tr_direction::Down, *val);
        }
    }
}

void bandwidthGroupWrite(tr_session const* session, std::string_view const config_dir)
{
    auto const& groups = session->bandwidthGroups();
    auto groups_map = tr_variant::Map{ std::size(groups) };
    for (auto const& [name, group] : groups)
    {
        auto const limits = group->get_limits();
        auto group_map = tr_variant::Map{ 6U };
        group_map.try_emplace(TR_KEY_download_limit, limits.down_limit.count(Speed::Units::KByps));
        group_map.try_emplace(TR_KEY_download_limited, limits.down_limited);
        group_map.try_emplace(TR_KEY_honors_session_limits, group->are_parent_limits_honored(tr_direction::Up));
        group_map.try_emplace(TR_KEY_name, name.sv());
        group_map.try_emplace(TR_KEY_upload_limit, limits.up_limit.count(Speed::Units::KByps));
        group_map.try_emplace(TR_KEY_upload_limited, limits.up_limited);
        groups_map.try_emplace(name.quark(), std::move(group_map));
    }

    auto out = tr_variant{ std::move(groups_map) };
    tr::api_compat::convert_outgoing_data(out);
    tr_variant_serde::json().to_file(out, tr_pathbuf{ config_dir, '/', BandwidthGroupsFilename });
}
} // namespace bandwidth_group_helpers
} // namespace

void tr_session::update_bandwidth(tr_direction const dir)
{
    if (auto const limit = active_speed_limit(dir); limit)
    {
        top_bandwidth_.set_limited(dir, limit->base_quantity() > 0U);
        top_bandwidth_.set_desired_speed(dir, *limit);
    }
    else
    {
        top_bandwidth_.set_limited(dir, false);
    }
}

tr_port tr_session::randomPort() const
{
    auto const lower = std::min(settings_.peer_port_random_low.host(), settings_.peer_port_random_high.host());
    auto const upper = std::max(settings_.peer_port_random_low.host(), settings_.peer_port_random_high.host());
    auto const range = upper - lower;
    return tr_port::from_host(lower + tr_rand_int(range + 1U));
}

/* Generate a peer id : "-TRxyzb-" + 12 random alphanumeric
   characters, where x is the major version number, y is the
   minor version number, z is the maintenance number, and b
   designates beta (Azureus-style) */
tr_peer_id_t tr_peerIdInit()
{
    auto peer_id = tr_peer_id_t{};
    auto* it = std::data(peer_id);

    // starts with -TRXXXX-
    auto const Prefix = std::string_view{ tr_display_peer_id_prefix() };
    auto const* const end = it + std::size(peer_id);
    it = std::copy_n(std::data(Prefix), std::size(Prefix), it);

    // remainder is randomly-generated characters
    auto constexpr Pool = std::string_view{ "0123456789abcdefghijklmnopqrstuvwxyz" };
    auto total = size_t{};
    tr_rand_buffer(it, end - it);
    while (it + 1 < end)
    {
        auto const val = *it % std::size(Pool);
        total += val;
        *it++ = Pool[val];
    }
    auto const val = total % std::size(Pool) != 0 ? std::size(Pool) - (total % std::size(Pool)) : 0;
    *it = Pool[val];

    return peer_id;
}

// ---

std::vector<tr_torrent_id_t> tr_session::DhtMediator::torrents_allowing_dht() const
{
    auto ids = std::vector<tr_torrent_id_t>{};
    auto const& torrents = session_.torrents();

    ids.reserve(std::size(torrents));
    for (auto const* const tor : torrents)
    {
        if (tor->is_running() && tor->allows_dht())
        {
            ids.push_back(tor->id());
        }
    }

    return ids;
}

tr_sha1_digest_t tr_session::DhtMediator::torrent_info_hash(tr_torrent_id_t id) const
{
    if (auto const* const tor = session_.torrents().get(id); tor != nullptr)
    {
        return tor->info_hash();
    }

    return {};
}

void tr_session::DhtMediator::add_pex(tr_sha1_digest_t const& info_hash, tr_pex const* pex, size_t n_pex)
{
    if (auto* const tor = session_.torrents().get(info_hash); tor != nullptr)
    {
        tr_peerMgrAddPex(tor, TR_PEER_FROM_DHT, pex, n_pex);
    }
}

// ---

std::string tr_session::QueueMediator::store_filename(tr_torrent_id_t id) const
{
    auto const* const tor = session_.torrents().get(id);
    return tor != nullptr ? tor->store_filename() : std::string{};
}

// ---

bool tr_session::LpdMediator::onPeerFound(std::string_view info_hash_str, tr_address address, tr_port port)
{
    auto const digest = tr_sha1_from_string(info_hash_str);
    if (!digest)
    {
        return false;
    }

    tr_torrent* const tor = session_.torrents_.get(*digest);
    if (!tr_isTorrent(tor) || !tor->allows_lpd())
    {
        return false;
    }

    // we found a suitable peer, add it to the torrent
    auto const socket_address = tr_socket_address{ address, port };
    auto const pex = tr_pex{ socket_address };
    tr_peerMgrAddPex(tor, TR_PEER_FROM_LPD, &pex, 1U);
    tr_logAddDebugTor(tor, fmt::format("Found a local peer from LPD ({:s})", socket_address.display_name()));
    return true;
}

std::vector<tr_lpd::Mediator::TorrentInfo> tr_session::LpdMediator::torrents() const
{
    auto ret = std::vector<tr_lpd::Mediator::TorrentInfo>{};
    ret.reserve(std::size(session_.torrents()));
    for (auto const* const tor : session_.torrents())
    {
        auto info = tr_lpd::Mediator::TorrentInfo{};
        info.info_hash_str = tor->info_hash_string();
        info.activity = tor->activity();
        info.allows_lpd = tor->allows_lpd();
        info.announce_after = tor->lpdAnnounceAt;
        ret.emplace_back(info);
    }
    return ret;
}

void tr_session::LpdMediator::setNextAnnounceTime(std::string_view info_hash_str, time_t announce_after)
{
    if (auto digest = tr_sha1_from_string(info_hash_str); digest)
    {
        if (tr_torrent* const tor = session_.torrents_.get(*digest); tr_isTorrent(tor))
        {
            tor->lpdAnnounceAt = announce_after;
        }
    }
}

// ---

std::optional<std::string> tr_session::WebMediator::cookieFile() const
{
    auto const path = tr_pathbuf{ session_->configDir(), "/cookies.txt"sv };

    if (!tr_sys_path_exists(path))
    {
        return {};
    }

    return std::string{ path };
}

std::optional<std::string_view> tr_session::WebMediator::userAgent() const
{
    static thread_local auto user_agent = std::string{};
    user_agent = fmt::format("{:s}/{:s}", tr_display_client_name(), tr_display_short_version_string());
    return std::string_view{ user_agent };
}

std::optional<std::string> tr_session::WebMediator::bind_address_V4() const
{
    if (auto const addr = session_->bind_address(TR_AF_INET); !addr.is_any())
    {
        return addr.display_name();
    }

    return std::nullopt;
}

std::optional<std::string> tr_session::WebMediator::bind_address_V6() const
{
    if (auto const addr = session_->bind_address(TR_AF_INET6); !addr.is_any())
    {
        return addr.display_name();
    }

    return std::nullopt;
}

size_t tr_session::WebMediator::clamp(int torrent_id, size_t byte_count) const
{
    auto const lock = session_->unique_lock();

    auto const* const tor = session_->torrents().get(torrent_id);
    return tor == nullptr ? 0U : tor->bandwidth().clamp(tr_direction::Down, byte_count);
}

std::optional<std::string> tr_session::WebMediator::proxyUrl() const
{
    return session_->settings().proxy_url;
}

void tr_session::WebMediator::run(tr_web::FetchDoneFunc&& func, tr_web::FetchResponse&& response) const
{
    session_->run_in_session_thread(std::move(func), std::move(response));
}

std::chrono::steady_clock::time_point tr_session::WebMediator::now() const
{
    return std::chrono::steady_clock::now();
}

void tr_sessionFetch(tr_session* session, tr_web::FetchOptions&& options)
{
    session->fetch(std::move(options));
}

// ---

tr_encryption_mode tr_sessionGetEncryption(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->encryptionMode();
}

void tr_sessionSetEncryption(tr_session* session, tr_encryption_mode mode)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(mode == TR_ENCRYPTION_PREFERRED || mode == TR_ENCRYPTION_REQUIRED || mode == TR_CLEAR_PREFERRED);

    session->settings_.encryption_mode = mode;
}

// ---

void tr_session::onIncomingPeerConnection(tr_socket_t fd, void* vsession)
{
    auto* session = static_cast<tr_session*>(vsession);

    if (auto const incoming_info = tr_netAccept(session, fd); incoming_info)
    {
        auto const& [socket_address, sock] = *incoming_info;
        tr_logAddTrace(fmt::format("new incoming connection {} ({})", sock, socket_address.display_name()));
        session->addIncoming(tr_peer_socket_tcp::create(*session, socket_address, sock));
    }
}

tr_session::BoundSocket::BoundSocket(
    struct event_base* evbase,
    tr_address const& addr,
    tr_port port,
    IncomingCallback cb,
    void* cb_data)
    : cb_{ cb }
    , cb_data_{ cb_data }
    , socket_{ tr_netBindTCP(addr, port, false) }
    , ev_{ tr::evhelpers::event_new_pri2(
          evbase,
          static_cast<evutil_socket_t>(socket_),
          EV_READ | EV_PERSIST,
          &BoundSocket::onCanRead,
          this) }
{
    if (!is_valid_socket(socket_))
    {
        return;
    }

    tr_logAddInfo(
        fmt::format(
            fmt::runtime(_("Listening to incoming peer connections on {hostport}")),
            fmt::arg("hostport", tr_socket_address::display_name(addr, port))));
    event_add(ev_.get(), nullptr);
}

tr_session::BoundSocket::~BoundSocket()
{
    ev_.reset();

    if (is_valid_socket(socket_))
    {
        tr_net_close_socket(socket_);
        socket_ = TR_BAD_SOCKET;
    }
}

tr_address tr_session::bind_address(tr_address_type type) const noexcept
{
    if (type == TR_AF_INET)
    {
        // if user provided an address, use it.
        // otherwise, use any_ipv4 (0.0.0.0).
        return ip_cache_->bind_addr(type);
    }

    if (type == TR_AF_INET6)
    {
        // if user provided an address, use it.
        // otherwise, if we can determine which one to use via global_source_address(ipv6) magic, use it.
        // otherwise, use any_ipv6 (::).
        auto const source_addr = source_address(type);
        auto const default_addr = source_addr && source_addr->is_global_unicast() ? *source_addr : tr_address::any(TR_AF_INET6);
        return tr_address::from_string(settings_.bind_address_ipv6).value_or(default_addr);
    }

    TR_ASSERT_MSG(false, "invalid type");
    return {};
}

// ---

tr_variant tr_sessionGetDefaultSettings()
{
    auto ret = tr_variant::make_map();
    ret.merge(tr::SessionSettingsSnapshot{}.save());

    // TODO(5.0.0): remove this if block
    // N.B. Because `tr::SessionSettings::load()` calls
    // `tr::SessionSettings::fixup_to_preferred_transports()`,
    // the defaults of `preferred_transports` essentially
    // just repeats `utp_enabled` + `tcp_enabled`.
    //
    // Erase `preferred_transports` from the defaults to avoid
    // overwriting `utp_enabled` and `tcp_enabled` that is set
    // by the user.
    if (auto* const map = ret.get_if<tr_variant::Map>())
    {
        map->erase(TR_KEY_preferred_transports);
    }

    return ret;
}

tr_variant tr_sessionGetSettings(tr_session const* session)
{
    auto snapshot = tr::SessionSettingsSnapshot{};
    snapshot.session = session->settings_;
    snapshot.alt_speeds = session->alt_speeds_.settings();
    snapshot.rpc_server = session->rpc_server_->settings();

    auto settings = tr_variant{ snapshot.save() };
    tr_variantDictAddInt(&settings, TR_KEY_message_level, tr_logGetLevel());
    return settings;
}

tr_variant tr_sessionLoadSettings(std::string_view const config_dir, tr_variant const* const app_defaults)
{
    // merge in order of precedence:
    // 1. the previous session's settings from `settings.json`
    // 2. app_defaults, if provided
    // 3. lastly, `tr_sessionGetDefaultSettings()` to fill in any blanks

    auto settings = tr_variant::make_map();

    // settings.json (if available) has highest precedence
    if (auto const filename = fmt::format("{:s}/settings.json", config_dir); tr_sys_path_exists(filename))
    {
        if (auto file_settings = tr_variant_serde::json().parse_file(filename))
        {
            tr::api_compat::convert_incoming_data(*file_settings);
            settings.merge(std::move(*file_settings));
        }
    }

    if (app_defaults != nullptr && app_defaults->holds_alternative<tr_variant::Map>())
    {
        settings.merge(*app_defaults);
    }

    settings.merge(tr_sessionGetDefaultSettings());

    return settings;
}

void tr_sessionSaveSettings(tr_session* session, std::string_view const config_dir, tr_variant const& client_settings)
{
    using namespace bandwidth_group_helpers;

    TR_ASSERT(client_settings.holds_alternative<tr_variant::Map>());

    auto const filename = tr_pathbuf{ config_dir, "/settings.json"sv };

    // from highest to lowest precedence:
    // - actual values
    // - client settings
    // - previous session's settings stored in settings.json
    // - built-in defaults
    auto settings = tr_sessionGetSettings(session);
    settings.merge(client_settings);
    if (auto file_settings = tr_variant_serde::json().parse_file(filename); file_settings)
    {
        tr::api_compat::convert_incoming_data(*file_settings);
        settings.merge(*file_settings);
    }
    settings.merge(tr_sessionGetDefaultSettings());

    // save 'em
    tr::api_compat::convert_outgoing_data(settings);
    tr_variant_serde::json().to_file(settings, filename);

    // write bandwidth groups limits to file
    bandwidthGroupWrite(session, config_dir);
}

// ---

struct tr_session::init_data
{
    init_data(bool message_queuing_enabled_in, std::string_view config_dir_in, tr_variant const& settings_in)
        : message_queuing_enabled{ message_queuing_enabled_in }
        , config_dir{ config_dir_in }
        , settings{ settings_in }
    {
    }

    bool message_queuing_enabled;
    std::string_view config_dir;
    tr_variant const& settings;

    std::condition_variable_any done_cv;
};

tr_session* tr_sessionInit(std::string_view const config_dir, bool message_queueing_enabled, tr_variant const& client_settings)
{
    using namespace bandwidth_group_helpers;

    TR_ASSERT(client_settings.holds_alternative<tr_variant::Map>());

    tr_timeUpdate(time(nullptr));

    // settings order of precedence from highest to lowest:
    // - client settings
    // - previous session's values in settings.json
    // - hardcoded defaults
    auto settings = client_settings.clone();
    settings.merge(tr_sessionLoadSettings(config_dir));

    // if logging is desired, start it now before doing more work
    if (auto const* settings_map = settings.get_if<tr_variant::Map>(); settings_map != nullptr)
    {
        if (auto const val = settings_map->value_if<bool>(TR_KEY_message_level))
        {
            tr_logSetLevel(static_cast<tr_log_level>(*val));
        }
    }

    // initialize the bare skeleton of the session object
    auto* const session = new tr_session{ config_dir, tr_variant::make_map() };
    bandwidthGroupRead(session, config_dir);

    // run initImpl() in the libtransmission thread
    auto data = tr_session::init_data{ message_queueing_enabled, config_dir, settings };
    auto lock = session->unique_lock();
    session->run_in_session_thread([&session, &data]() { session->initImpl(data); });
    data.done_cv.wait(lock); // wait for the session to be ready

    return session;
}

void tr_session::on_now_timer()
{
    TR_ASSERT(now_timer_);
    auto const now = std::chrono::system_clock::now();

    // tr_session upkeep tasks to perform once per second
    tr_timeUpdate(std::chrono::system_clock::to_time_t(now));
    alt_speeds_.check_scheduler();

    // set the timer to kick again right after (10ms after) the next second
    auto const target_time = std::chrono::time_point_cast<std::chrono::seconds>(now) + 1s + 10ms;
    auto target_interval = target_time - now;
    if (target_interval < 100ms)
    {
        target_interval += 1s;
    }
    now_timer_->set_interval(std::chrono::duration_cast<std::chrono::milliseconds>(target_interval));
}

namespace
{
namespace queue_helpers
{
std::vector<tr_torrent*> get_next_queued_torrents(tr_torrents const& torrents, tr_direction dir, size_t num_wanted)
{
    auto candidates = torrents.get_matching([dir](auto const* const tor) { return tor->is_queued(dir); });

    // find the best n candidates
    num_wanted = std::min(num_wanted, std::size(candidates));
    if (num_wanted < candidates.size())
    {
        std::ranges::partial_sort(
            candidates,
            std::begin(candidates) + static_cast<decltype(candidates)::difference_type>(num_wanted),
            tr_torrent::CompareQueuePosition);
        candidates.resize(num_wanted);
    }

    return candidates;
}
} // namespace queue_helpers
} // namespace

size_t tr_session::count_queue_free_slots(tr_direction dir) const noexcept
{
    if (!queueEnabled(dir))
    {
        return std::numeric_limits<size_t>::max();
    }

    auto const max = queueSize(dir);
    auto const activity = dir == tr_direction::Up ? TR_STATUS_SEED : TR_STATUS_DOWNLOAD;

    // count how many torrents are active
    auto active_count = size_t{};
    auto const stalled_enabled = queueStalledEnabled();
    auto const stalled_if_idle_for_n_seconds = static_cast<time_t>(queueStalledMinutes() * 60);
    auto const now = tr_time();
    for (auto const* const tor : torrents())
    {
        // is it the right activity?
        if (activity != tor->activity())
        {
            continue;
        }

        // is it stalled?
        if (stalled_enabled)
        {
            auto const idle_seconds = tor->idle_seconds(now);
            if (idle_seconds && *idle_seconds >= stalled_if_idle_for_n_seconds)
            {
                continue;
            }
        }

        ++active_count;

        /* if we've reached the limit, no need to keep counting */
        if (active_count >= max)
        {
            return 0;
        }
    }

    return max - active_count;
}

void tr_session::on_queue_timer()
{
    using namespace queue_helpers;

    for (auto const dir : { tr_direction::Up, tr_direction::Down })
    {
        if (!queueEnabled(dir))
        {
            continue;
        }

        auto const n_wanted = count_queue_free_slots(dir);

        for (auto* tor : get_next_queued_torrents(torrents(), dir, n_wanted))
        {
            tr_torrentStartNow(tor);

            if (queue_start_callback_)
            {
                queue_start_callback_(tor->id());
            }
        }
    }
}

// Periodically save the .resume files of any torrents whose
// status has recently changed. This prevents loss of metadata
// in the case of a crash, unclean shutdown, clumsy user, etc.
void tr_session::on_save_timer()
{
    for (auto* const tor : torrents())
    {
        tor->save_resume_file();
    }

    stats().save_if_dirty();
    torrent_queue().to_file();
}

void tr_session::initImpl(init_data& data)
{
    auto lock = unique_lock();
    TR_ASSERT(am_in_session_thread());

    auto const& settings = data.settings;
    TR_ASSERT(settings.holds_alternative<tr_variant::Map>());

    tr_logAddTrace(fmt::format("tr_sessionInit: the session's top-level bandwidth object is {}", fmt::ptr(&top_bandwidth_)));

#ifndef _WIN32
    /* Don't exit when writing on a broken socket */
    (void)signal(SIGPIPE, SIG_IGN);
#endif

    tr_logSetQueueEnabled(data.message_queuing_enabled);

    blocklists_.load(blocklist_dir_, blocklist_enabled());

    tr_logAddInfo(
        fmt::format(fmt::runtime(_("Transmission version {version} starting")), fmt::arg("version", LONG_VERSION_STRING)));

    setSettings(settings, true);

    if (settings_.usenet_enabled)
    {
        usenet_piece_store_ = std::make_unique<tr_usenet_piece_store>(config_dir_, settings_.usenet_check_article_size);
        startUsenetIoLimiter();
        startUsenetDiscoveryWorker();
        startUsenetIntegrityWorker();
        startUsenetUploadWorker();
        startUsenetDownloadWorker();
        startUsenetEvictionTimer();
    }

    tr_utp_init(this);

    /* cleanup */
    data.done_cv.notify_one();
}

void tr_session::setSettings(tr_variant const& settings, bool force)
{
    TR_ASSERT(am_in_session_thread());
    TR_ASSERT(settings.holds_alternative<tr_variant::Map>());

    setSettings(tr_session::Settings{ settings }, force);

    // delegate loading out the other settings
    alt_speeds_.load(tr_session_alt_speeds::Settings{ settings });
    rpc_server_->load(tr_rpc_server::Settings{ settings });
}

// NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved): `std::swap()` also move from the parameter
void tr_session::setSettings(tr_session::Settings&& settings_in, bool force)
{
    auto const lock = unique_lock();

    std::swap(settings_, settings_in);
    auto const& new_settings = settings_;
    auto const& old_settings = settings_in;

    // the rest of the func is session_ responding to settings changes

    if (auto const& val = new_settings.log_level; force || val != old_settings.log_level)
    {
        tr_logSetLevel(val);
    }

#ifndef _WIN32
    if (auto const& val = new_settings.umask; force || val != old_settings.umask)
    {
        ::umask(val);
    }
#endif

    if (auto const& val = new_settings.bind_address_ipv4; force || val != old_settings.bind_address_ipv4)
    {
        ip_cache_->update_addr(TR_AF_INET);
    }
    if (auto const& val = new_settings.bind_address_ipv6; force || val != old_settings.bind_address_ipv6)
    {
        ip_cache_->update_addr(TR_AF_INET6);
    }

    if (auto const& val = new_settings.default_trackers_str; force || val != old_settings.default_trackers_str)
    {
        setDefaultTrackers(val);
    }

    bool const utp_changed = new_settings.utp_enabled != old_settings.utp_enabled;

    set_blocklist_enabled(new_settings.blocklist_enabled);

    auto local_peer_port = force && settings_.peer_port_random_on_start ? randomPort() : new_settings.peer_port;
    bool port_changed = false;
    if (force || local_peer_port_ != local_peer_port)
    {
        local_peer_port_ = local_peer_port;
        advertised_peer_port_ = local_peer_port;
        port_changed = true;
    }

    bool addr_changed = false;
    if (new_settings.tcp_enabled)
    {
        if (auto const& val = new_settings.bind_address_ipv4; force || port_changed || val != old_settings.bind_address_ipv4)
        {
            auto const addr = bind_address(TR_AF_INET);
            bound_ipv4_.emplace(event_base(), addr, local_peer_port_, &tr_session::onIncomingPeerConnection, this);
            addr_changed = true;
        }

        if (auto const& val = new_settings.bind_address_ipv6; force || port_changed || val != old_settings.bind_address_ipv6)
        {
            auto const addr = bind_address(TR_AF_INET6);
            bound_ipv6_.emplace(event_base(), addr, local_peer_port_, &tr_session::onIncomingPeerConnection, this);
            addr_changed = true;
        }
    }
    else
    {
        bound_ipv4_.reset();
        bound_ipv6_.reset();
        addr_changed = true;
    }

    if (auto const& val = new_settings.port_forwarding_enabled; force || val != old_settings.port_forwarding_enabled)
    {
        tr_sessionSetPortForwardingEnabled(this, val);
    }

    if (port_changed)
    {
        port_forwarding_->local_port_changed();
    }

    if (!udp_core_ || force || addr_changed || port_changed || utp_changed)
    {
        udp_core_ = std::make_unique<tr_session::tr_udp_core>(*this, udpPort());
    }

    // Sends out announce messages with advertisedPeerPort(), so this
    // section needs to happen here after the peer port settings changes
    if (auto const& val = new_settings.lpd_enabled; force || val != old_settings.lpd_enabled)
    {
        if (val)
        {
            lpd_ = tr_lpd::create(lpd_mediator_, event_base());
        }
        else
        {
            lpd_.reset();
        }
    }

    if (!new_settings.dht_enabled)
    {
        dht_.reset();
    }
    else if (force || !dht_ || port_changed || addr_changed || new_settings.dht_enabled != old_settings.dht_enabled)
    {
        dht_ = tr_dht::create(dht_mediator_, advertisedPeerPort(), udp_core_->socket4(), udp_core_->socket6());
    }

    if (auto const& val = new_settings.sleep_per_seconds_during_verify;
        force || val != old_settings.sleep_per_seconds_during_verify)
    {
        verifier_->set_sleep_per_seconds_during_verify(val);
    }

    // We need to update bandwidth if speed settings changed.
    // It's a harmless call, so just call it instead of checking for settings changes
    update_bandwidth(tr_direction::Up);
    update_bandwidth(tr_direction::Down);
}

void tr_sessionSet(tr_session* session, tr_variant const& settings)
{
    // do the work in the session thread
    auto done_promise = std::promise<void>{};
    auto done_future = done_promise.get_future();
    session->run_in_session_thread(
        [&session, &settings, &done_promise]()
        {
            session->setSettings(settings, false);
            done_promise.set_value();
        });
    done_future.wait();
}

// ---

std::string tr::SessionSettings::get_default_download_dir()
{
    return tr_getDefaultDownloadDir();
}

void tr::SessionSettings::fixup_from_preferred_transports()
{
    utp_enabled = false;
    tcp_enabled = false;
    for (auto const& transport : preferred_transports)
    {
        switch (transport)
        {
        case tr_preferred_transport::UTP:
            utp_enabled = true;
            break;
        case tr_preferred_transport::TCP:
            tcp_enabled = true;
            break;
        default:
            break;
        }
    }
}

void tr::SessionSettings::fixup_to_preferred_transports()
{
    if (!utp_enabled)
    {
        auto const [first, last] = std::ranges::remove(preferred_transports, tr_preferred_transport::UTP);
        preferred_transports.erase(first, last);
    }
    else if (std::ranges::find(preferred_transports, tr_preferred_transport::UTP) == std::ranges::end(preferred_transports))
    {
        TR_ASSERT(std::size(preferred_transports) < preferred_transports.max_size());
        preferred_transports.emplace(std::begin(preferred_transports), tr_preferred_transport::UTP);
    }

    if (!tcp_enabled)
    {
        auto const [first, last] = std::ranges::remove(preferred_transports, tr_preferred_transport::TCP);
        preferred_transports.erase(first, last);
    }
    else if (std::ranges::find(preferred_transports, tr_preferred_transport::TCP) == std::ranges::end(preferred_transports))
    {
        TR_ASSERT(std::size(preferred_transports) < preferred_transports.max_size());
        preferred_transports.emplace_back(tr_preferred_transport::TCP);
    }
}

// ---

void tr_sessionSetDownloadDir(tr_session* session, std::string_view const dir)
{
    TR_ASSERT(session != nullptr);

    session->setDownloadDir(dir);
}

std::string tr_sessionGetDownloadDir(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->downloadDir();
}

std::string tr_sessionGetConfigDir(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->configDir();
}

// ---

void tr_sessionSetIncompleteFileNamingEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->settings_.is_incomplete_file_naming_enabled = enabled;
}

bool tr_sessionIsIncompleteFileNamingEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isIncompleteFileNamingEnabled();
}

// ---

void tr_sessionSetIncompleteDir(tr_session* session, std::string_view const dir)
{
    TR_ASSERT(session != nullptr);

    session->setIncompleteDir(dir);
}

std::string tr_sessionGetIncompleteDir(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->incompleteDir();
}

void tr_sessionSetIncompleteDirEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->useIncompleteDir(enabled);
}

bool tr_sessionIsIncompleteDirEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->useIncompleteDir();
}

// --- Peer Port

void tr_sessionSetPeerPort(tr_session* session, uint16_t hport)
{
    TR_ASSERT(session != nullptr);

    if (auto const port = tr_port::from_host(hport); port != session->localPeerPort())
    {
        session->run_in_session_thread(
            [session, port]()
            {
                auto settings = session->settings_;
                settings.peer_port = port;
                session->setSettings(std::move(settings), false);
            });
    }
}

uint16_t tr_sessionGetPeerPort(tr_session const* session)
{
    return session != nullptr ? session->localPeerPort().host() : 0U;
}

uint16_t tr_sessionSetPeerPortRandom(tr_session* session)
{
    auto const p = session->randomPort();
    tr_sessionSetPeerPort(session, p.host());
    return p.host();
}

void tr_sessionSetPeerPortRandomOnStart(tr_session* session, bool random)
{
    TR_ASSERT(session != nullptr);

    session->settings_.peer_port_random_on_start = random;
}

bool tr_sessionGetPeerPortRandomOnStart(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isPortRandom();
}

tr_port_forwarding_state tr_sessionGetPortForwarding(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->port_forwarding_->state();
}

void tr_session::onAdvertisedPeerPortChanged()
{
    for (auto* const tor : torrents())
    {
        tr_torrentChangeMyPort(tor);
    }
}

// ---

void tr_sessionSetRatioLimited(tr_session* session, bool is_limited)
{
    TR_ASSERT(session != nullptr);

    session->settings_.ratio_limit_enabled = is_limited;
}

void tr_sessionSetRatioLimit(tr_session* session, double desired_ratio)
{
    TR_ASSERT(session != nullptr);

    session->settings_.ratio_limit = desired_ratio;
}

bool tr_sessionIsRatioLimited(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isRatioLimited();
}

double tr_sessionGetRatioLimit(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->desiredRatio();
}

// ---

void tr_sessionSetIdleLimited(tr_session* session, bool is_limited)
{
    TR_ASSERT(session != nullptr);

    session->settings_.idle_seeding_limit_enabled = is_limited;
}

void tr_sessionSetIdleLimit(tr_session* session, uint16_t idle_minutes)
{
    TR_ASSERT(session != nullptr);

    session->settings_.idle_seeding_limit_minutes = idle_minutes;
}

bool tr_sessionIsIdleLimited(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->isIdleLimited();
}

uint16_t tr_sessionGetIdleLimit(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->idleLimitMinutes();
}

// --- Speed limits

std::optional<Speed> tr_session::active_speed_limit(tr_direction dir) const noexcept
{
    if (tr_sessionUsesAltSpeed(this))
    {
        return alt_speeds_.speed_limit(dir);
    }

    if (is_speed_limited(dir))
    {
        return speed_limit(dir);
    }

    return {};
}

time_t tr_session::AltSpeedMediator::time()
{
    return tr_time();
}

void tr_session::AltSpeedMediator::is_active_changed(bool is_active, tr_session_alt_speeds::ChangeReason reason)
{
    auto const in_session_thread = [session = &session_, is_active, reason]()
    {
        session->update_bandwidth(tr_direction::Up);
        session->update_bandwidth(tr_direction::Down);

        if (session->alt_speed_active_changed_func_)
        {
            session->alt_speed_active_changed_func_(is_active, reason == tr_session_alt_speeds::ChangeReason::User);
        }
    };

    session_.run_in_session_thread(in_session_thread);
}

// --- Session primary speed limits

void tr_sessionSetSpeedLimit_KBps(tr_session* const session, tr_direction const dir, size_t const limit_kbyps)
{
    TR_ASSERT(session != nullptr);
    session->set_speed_limit(dir, Speed{ limit_kbyps, Speed::Units::KByps });
}

size_t tr_sessionGetSpeedLimit_KBps(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    return static_cast<size_t>(session->speed_limit(dir).count(Speed::Units::KByps));
}

void tr_sessionLimitSpeed(tr_session* session, tr_direction const dir, bool limited)
{
    TR_ASSERT(session != nullptr);

    if (dir == tr_direction::Down)
    {
        session->settings_.speed_limit_down_enabled = limited;
    }
    else
    {
        session->settings_.speed_limit_up_enabled = limited;
    }

    session->update_bandwidth(dir);
}

bool tr_sessionIsSpeedLimited(tr_session const* session, tr_direction const dir)
{
    TR_ASSERT(session != nullptr);
    return session->is_speed_limited(dir);
}

// --- Session alt speed limits

void tr_sessionSetAltSpeed_KBps(tr_session* const session, tr_direction const dir, size_t const limit_kbyps)
{
    TR_ASSERT(session != nullptr);
    session->alt_speeds_.set_speed_limit(dir, Speed{ limit_kbyps, Speed::Units::KByps });
    session->update_bandwidth(dir);
}

size_t tr_sessionGetAltSpeed_KBps(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    return static_cast<size_t>(session->alt_speeds_.speed_limit(dir).count(Speed::Units::KByps));
}

void tr_sessionUseAltSpeedTime(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_scheduler_enabled(enabled);
}

bool tr_sessionUsesAltSpeedTime(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.is_scheduler_enabled();
}

void tr_sessionSetAltSpeedBegin(tr_session* session, size_t minutes_since_midnight)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_start_minute(minutes_since_midnight);
}

size_t tr_sessionGetAltSpeedBegin(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.start_minute();
}
void tr_sessionSetAltSpeedEnd(tr_session* session, size_t minutes_since_midnight)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_end_minute(minutes_since_midnight);
}

size_t tr_sessionGetAltSpeedEnd(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.end_minute();
}

void tr_sessionSetAltSpeedDay(tr_session* session, tr_sched_day days)
{
    TR_ASSERT(session != nullptr);

    session->alt_speeds_.set_weekdays(days);
}

tr_sched_day tr_sessionGetAltSpeedDay(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.weekdays();
}

void tr_sessionUseAltSpeed(tr_session* session, bool enabled)
{
    session->alt_speeds_.set_active(enabled, tr_session_alt_speeds::ChangeReason::User);
}

bool tr_sessionUsesAltSpeed(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->alt_speeds_.is_active();
}

void tr_sessionSetAltSpeedFunc(tr_session* session, tr_altSpeedFunc func)
{
    TR_ASSERT(session != nullptr);

    session->alt_speed_active_changed_func_ = std::move(func);
}

// ---

void tr_sessionSetPeerLimit(tr_session* session, uint16_t max_global_peers)
{
    TR_ASSERT(session != nullptr);

    session->settings_.peer_limit_global = max_global_peers;
}

uint16_t tr_sessionGetPeerLimit(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->peerLimit();
}

void tr_sessionSetPeerLimitPerTorrent(tr_session* session, uint16_t max_peers)
{
    TR_ASSERT(session != nullptr);

    session->settings_.peer_limit_per_torrent = max_peers;
}

uint16_t tr_sessionGetPeerLimitPerTorrent(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->peerLimitPerTorrent();
}

// ---

void tr_sessionSetPaused(tr_session* session, bool is_paused)
{
    TR_ASSERT(session != nullptr);

    session->settings_.should_start_added_torrents = !is_paused;
}

bool tr_sessionGetPaused(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->shouldPauseAddedTorrents();
}

void tr_sessionSetDeleteSource(tr_session* session, bool delete_source)
{
    TR_ASSERT(session != nullptr);

    session->settings_.should_delete_source_torrents = delete_source;
}

// ---

double tr_sessionGetRawSpeed_KBps(tr_session const* session, tr_direction dir)
{
    if (session != nullptr)
    {
        return session->top_bandwidth_.get_raw_speed(0, dir).count(Speed::Units::KByps);
    }

    return {};
}

void tr_session::closeImplPart1(std::promise<void>* closed_promise, std::chrono::time_point<std::chrono::steady_clock> deadline)
{
    is_closing_ = true;
    stopUsenetDiscoveryWorker();
    stopUsenetIntegrityWorker();
    stopUsenetDownloadWorker();
    stopUsenetUploadWorker();
    stopUsenetIoLimiter();

    // close the low-hanging fruit that can be closed immediately w/o consequences
    utp_timer.reset();
    verifier_.reset();
    save_timer_.reset();
    queue_timer_.reset();
    now_timer_.reset();
    rpc_server_.reset();
    dht_.reset();
    lpd_.reset();

    port_forwarding_.reset();
    bound_ipv6_.reset();
    bound_ipv4_.reset();

    torrent_queue().to_file();

    // Close the torrents in order of most active to least active
    // so that the most important announce=stopped events are
    // fired out first...
    auto torrents = torrents_.get_all();
    std::ranges::sort(
        torrents,
        [](auto const* a, auto const* b)
        {
            auto const a_cur = a->bytes_downloaded_.ever();
            auto const b_cur = b->bytes_downloaded_.ever();
            return a_cur > b_cur; // larger xfers go first
        });
    for (auto* tor : torrents)
    {
        tr_torrentFreeInSessionThread(tor);
    }
    torrents.clear();
    // ...now that all the torrents have been closed, any remaining
    // `&event=stopped` announce messages are queued in the announcer.
    // Tell the announcer to start shutdown, which sends out the stop
    // events and stops scraping.
    this->announcer_->startShutdown();
    // ...since global_ip_cache_ relies on web_ to update global addresses,
    // we tell it to stop updating before web_ starts to refuse new requests.
    // But we keep it intact for now, so that udp_core_ can continue.
    this->ip_cache_->try_shutdown();
    // ...and now that those are done, tell web_ that we're shutting
    // down soon. This leaves the `event=stopped` going but refuses any
    // new tasks.
    auto const now = std::chrono::steady_clock::now();
    auto const remaining_ms = now < deadline ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now) : 0ms;
    this->web_->startShutdown(remaining_ms);

    // recycle the now-unused save_timer_ here to wait for UDP shutdown
    TR_ASSERT(!save_timer_);
    save_timer_ = timerMaker().create([this, closed_promise, deadline]() { closeImplPart2(closed_promise, deadline); });
    save_timer_->start_repeating(50ms);
}

void tr_session::closeImplPart2(std::promise<void>* closed_promise, std::chrono::time_point<std::chrono::steady_clock> deadline)
{
    // try to keep web_ and the UDP announcer alive long enough to send out
    // all the &event=stopped tracker announces.
    // also wait for all ip cache updates to finish so that web_ can
    // safely destruct.
    if ((!web_->is_idle() || !announcer_udp_->is_idle() || !ip_cache_->try_shutdown()) &&
        std::chrono::steady_clock::now() < deadline)
    {
        announcer_->upkeep();
        return;
    }

    save_timer_.reset();

    this->announcer_.reset();
    this->announcer_udp_.reset();

    stats().save();
    peer_mgr_.reset();
    openFiles().close_all();
    tr_utp_close(this);
    this->udp_core_.reset();

    // tada we are done!
    closed_promise->set_value();
}

void tr_sessionClose(tr_session* session, double const timeout_secs)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(!session->am_in_session_thread());

    tr_logAddInfo(
        fmt::format(fmt::runtime(_("Transmission version {version} shutting down")), fmt::arg("version", LONG_VERSION_STRING)));

    auto closed_promise = std::promise<void>{};
    auto closed_future = closed_promise.get_future();
    auto const deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds{ static_cast<int64_t>(timeout_secs * 1000.0) };
    session->run_in_session_thread([&closed_promise, deadline, session]()
                                   { session->closeImplPart1(&closed_promise, deadline); });
    closed_future.wait();

    delete session;
}

namespace
{
namespace load_torrents_helpers
{
auto get_remaining_files(std::string_view folder, std::vector<std::string>& queue_order)
{
    auto files = tr_sys_dir_get_files(folder);
    auto ret = std::vector<std::string>{};
    ret.reserve(std::size(files));
    std::ranges::sort(queue_order);
    std::ranges::sort(files);

    std::ranges::set_difference(files, queue_order, std::back_inserter(ret));

    // Read .torrent first if somehow a .magnet of the same hash exists
    // Example of possible cause: https://github.com/transmission/transmission/issues/5007
    std::ranges::stable_partition(ret, [](std::string_view name) { return tr_strv_ends_with(name, ".torrent"sv); });

    return ret;
}

void session_load_torrents(tr_session* session, tr_ctor* ctor, std::promise<size_t>* loaded_promise)
{
    auto n_torrents = size_t{};
    auto const& folder = session->torrentDir();

    auto load_func = [&folder, &n_torrents, ctor, buf = std::vector<char>{}](std::string_view name) mutable
    {
        if (tr_strv_ends_with(name, ".torrent"sv))
        {
            auto const path = tr_pathbuf{ folder, '/', name };
            if (ctor->set_metainfo_from_file(path.sv()) && tr_torrentNew(ctor, nullptr) != nullptr)
            {
                ++n_torrents;
            }
        }
        else if (tr_strv_ends_with(name, ".magnet"sv))
        {
            auto const path = tr_pathbuf{ folder, '/', name };
            if (tr_file_read(path, buf) &&
                ctor->set_metainfo_from_magnet_link(std::string_view{ std::data(buf), std::size(buf) }, nullptr) &&
                tr_torrentNew(ctor, nullptr) != nullptr)
            {
                ++n_torrents;
            }
        }
    };

    auto queue_order = session->torrent_queue().from_file();
    for (auto const& filename : queue_order)
    {
        load_func(filename);
    }
    for (auto const& filename : get_remaining_files(folder, queue_order))
    {
        load_func(filename);
    }

    if (n_torrents != 0U)
    {
        tr_logAddInfo(
            fmt::format(
                fmt::runtime(tr_ngettext("Loaded {count} torrent", "Loaded {count} torrents", n_torrents)),
                fmt::arg("count", n_torrents)));
    }

    session->setTorrentsLoadedTime();
    loaded_promise->set_value(n_torrents);
}
} // namespace load_torrents_helpers
} // namespace

size_t tr_sessionLoadTorrents(tr_session* session, tr_ctor* ctor)
{
    using namespace load_torrents_helpers;

    auto loaded_promise = std::promise<size_t>{};
    auto loaded_future = loaded_promise.get_future();

    session->run_in_session_thread(session_load_torrents, session, ctor, &loaded_promise);
    loaded_future.wait();
    auto const n_torrents = loaded_future.get();
    return n_torrents;
}

size_t tr_sessionGetAllTorrents(tr_session* session, tr_torrent** buf, size_t buflen)
{
    auto& torrents = session->torrents();
    auto const n = std::size(torrents);

    if (buflen >= n)
    {
        std::copy_n(std::begin(torrents), n, buf);
    }

    return n;
}

// ---

void tr_sessionSetPexEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->settings_.pex_enabled = enabled;
}

bool tr_sessionIsPexEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allows_pex();
}

bool tr_sessionIsDHTEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allowsDHT();
}

void tr_sessionSetDHTEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    if (enabled != session->allowsDHT())
    {
        session->run_in_session_thread(
            [session, enabled]()
            {
                auto settings = session->settings_;
                settings.dht_enabled = enabled;
                session->setSettings(std::move(settings), false);
            });
    }
}

// ---

bool tr_session::allowsUTP() const noexcept
{
#ifdef WITH_UTP
    return settings_.utp_enabled;
#else
    return false;
#endif
}

bool tr_sessionIsUTPEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allowsUTP();
}

void tr_sessionSetUTPEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    if (enabled == session->allowsUTP())
    {
        return;
    }

    session->run_in_session_thread(
        [session, enabled]()
        {
            auto settings = session->settings_;
            settings.utp_enabled = enabled;
            settings.fixup_to_preferred_transports();
            session->setSettings(std::move(settings), false);
        });
}

void tr_sessionSetLPDEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    if (enabled != session->allowsLPD())
    {
        session->run_in_session_thread(
            [session, enabled]()
            {
                auto settings = session->settings_;
                settings.lpd_enabled = enabled;
                session->setSettings(std::move(settings), false);
            });
    }
}

bool tr_sessionIsLPDEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->allowsLPD();
}

// ---

void tr_sessionSetCompleteVerifyEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->settings_.torrent_complete_verify_enabled = enabled;
}

// ---

void tr_session::setDefaultTrackers(std::string_view trackers)
{
    auto const oldval = default_trackers_;

    settings_.default_trackers_str = trackers;
    default_trackers_.parse(trackers);

    // if the list changed, update all the public torrents
    if (default_trackers_ != oldval)
    {
        for (auto* const tor : torrents())
        {
            if (tor->is_public())
            {
                announcer_->resetTorrent(tor);
            }
        }
    }
}

void tr_sessionSetDefaultTrackers(tr_session* session, std::string_view const trackers)
{
    TR_ASSERT(session != nullptr);

    session->setDefaultTrackers(trackers);
}

// ---

tr_bandwidth& tr_session::getBandwidthGroup(std::string_view name)
{
    auto& groups = this->bandwidth_groups_;

    for (auto const& [group_name, group] : groups)
    {
        if (group_name == name)
        {
            return *group;
        }
    }

    auto& [group_name, group] = groups.emplace_back(name, std::make_unique<tr_bandwidth>(&top_bandwidth_, true));
    return *group;
}

// ---

void tr_sessionSetPortForwardingEnabled(tr_session* session, bool enabled)
{
    session->run_in_session_thread(
        [session, enabled]()
        {
            session->settings_.port_forwarding_enabled = enabled;
            session->port_forwarding_->set_enabled(enabled);
        });
}

bool tr_sessionIsPortForwardingEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->port_forwarding_->is_enabled();
}

// ---

void tr_sessionReloadBlocklists(tr_session* session)
{
    session->blocklists_.load(session->blocklist_dir_, session->blocklist_enabled());
}

size_t tr_blocklistGetRuleCount(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->blocklists_.num_rules();
}

bool tr_blocklistIsEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->blocklist_enabled();
}

void tr_blocklistSetEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->set_blocklist_enabled(enabled);
}

bool tr_blocklistExists(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->blocklists_.num_lists() > 0U;
}

std::optional<size_t> tr_blocklistSetContent(tr_session* session, std::string_view const content_filename)
{
    auto const lock = session->unique_lock();
    return session->blocklists_.update_primary_blocklist(content_filename, session->blocklist_enabled());
}

void tr_blocklistSetURL(tr_session* session, std::string_view const url)
{
    session->setBlocklistUrl(url);
}

std::string tr_blocklistGetURL(tr_session const* session)
{
    return session->blocklistUrl();
}

// ---

void tr_session::setRpcWhitelist(std::string_view whitelist) const
{
    this->rpc_server_->set_whitelist(whitelist);
}

void tr_session::useRpcWhitelist(bool enabled) const
{
    this->rpc_server_->set_whitelist_enabled(enabled);
}

bool tr_session::useRpcWhitelist() const
{
    return this->rpc_server_->is_whitelist_enabled();
}

void tr_sessionSetRPCEnabled(tr_session* session, bool is_enabled)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_enabled(is_enabled);
}

bool tr_sessionIsRPCEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->is_enabled();
}

void tr_sessionSetRPCPort(tr_session* session, uint16_t hport)
{
    TR_ASSERT(session != nullptr);

    if (session->rpc_server_)
    {
        session->rpc_server_->set_port(tr_port::from_host(hport));
    }
}

uint16_t tr_sessionGetRPCPort(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_ ? session->rpc_server_->port().host() : uint16_t{};
}

void tr_sessionSetRPCCallback(tr_session* session, tr_rpc_func func)
{
    TR_ASSERT(session != nullptr);

    session->rpc_func_ = std::move(func);
}

void tr_sessionSetRPCWhitelist(tr_session* session, std::string_view const whitelist)
{
    TR_ASSERT(session != nullptr);

    session->setRpcWhitelist(whitelist);
}

std::string tr_sessionGetRPCWhitelist(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->whitelist();
}

void tr_sessionSetRPCWhitelistEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->useRpcWhitelist(enabled);
}

bool tr_sessionGetRPCWhitelistEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->useRpcWhitelist();
}

void tr_sessionSetRPCPassword(tr_session* session, std::string_view const password)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_password(password);
}

std::string tr_sessionGetRPCPassword(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->get_salted_password();
}

void tr_sessionSetRPCUsername(tr_session* session, std::string_view const username)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_username(username);
}

std::string tr_sessionGetRPCUsername(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->username();
}

void tr_sessionSetRPCPasswordEnabled(tr_session* session, bool enabled)
{
    TR_ASSERT(session != nullptr);

    session->rpc_server_->set_password_enabled(enabled);
}

bool tr_sessionIsRPCPasswordEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->rpc_server_->is_password_enabled();
}

// ---

void tr_sessionSetScriptEnabled(tr_session* session, TrScript type, bool enabled)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    session->useScript(type, enabled);
}

bool tr_sessionIsScriptEnabled(tr_session const* session, TrScript type)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    return session->useScript(type);
}

void tr_sessionSetScript(tr_session* session, TrScript type, std::string_view const script)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    session->setScript(type, script);
}

std::string tr_sessionGetScript(tr_session const* const session, TrScript const type)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(type < TR_SCRIPT_N_TYPES);

    return session->script(type);
}

// ---

void tr_sessionSetQueueSize(tr_session* session, tr_direction dir, size_t max_simultaneous_torrents)
{
    TR_ASSERT(session != nullptr);

    if (dir == tr_direction::Down)
    {
        session->settings_.download_queue_size = max_simultaneous_torrents;
    }
    else
    {
        session->settings_.seed_queue_size = max_simultaneous_torrents;
    }
}

size_t tr_sessionGetQueueSize(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);

    return session->queueSize(dir);
}

void tr_sessionSetQueueEnabled(tr_session* session, tr_direction dir, bool do_limit_simultaneous_torrents)
{
    TR_ASSERT(session != nullptr);

    if (dir == tr_direction::Down)
    {
        session->settings_.download_queue_enabled = do_limit_simultaneous_torrents;
    }
    else
    {
        session->settings_.seed_queue_enabled = do_limit_simultaneous_torrents;
    }
}

bool tr_sessionGetQueueEnabled(tr_session const* session, tr_direction dir)
{
    TR_ASSERT(session != nullptr);
    return session->queueEnabled(dir);
}

void tr_sessionSetQueueStalledMinutes(tr_session* session, size_t minutes)
{
    TR_ASSERT(session != nullptr);
    TR_ASSERT(minutes > 0);

    session->settings_.queue_stalled_minutes = minutes;
}

void tr_sessionSetQueueStalledEnabled(tr_session* session, bool is_enabled)
{
    TR_ASSERT(session != nullptr);

    session->settings_.queue_stalled_enabled = is_enabled;
}

bool tr_sessionGetQueueStalledEnabled(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->queueStalledEnabled();
}

size_t tr_sessionGetQueueStalledMinutes(tr_session const* session)
{
    TR_ASSERT(session != nullptr);

    return session->queueStalledMinutes();
}

// ---

void tr_session::verify_remove(tr_torrent const* const tor)
{
    if (verifier_)
    {
        verifier_->remove(tor->info_hash());
    }
}

void tr_session::verify_add(tr_torrent* const tor)
{
    if (verifier_)
    {
        verifier_->add(std::make_unique<tr_torrent::VerifyMediator>(tor), tor->get_priority());
    }
}

// ---

void tr_session::close_torrent_files(tr_torrent_id_t const tor_id) noexcept
{
    openFiles().close_torrent(tor_id);
}

void tr_session::close_torrent_file(tr_torrent const& tor, tr_file_index_t file_num) noexcept
{
    openFiles().close_file(tor.id(), file_num);
}

// ---

void tr_sessionSetQueueStartCallback(tr_session* session, tr_session_queue_start_func callback)
{
    session->setQueueStartCallback(std::move(callback));
}

void tr_sessionSetRatioLimitHitCallback(tr_session* session, tr_session_ratio_limit_hit_func callback)
{
    session->setRatioLimitHitCallback(std::move(callback));
}

void tr_sessionSetIdleLimitHitCallback(tr_session* session, tr_session_idle_limit_hit_func callback)
{
    session->setIdleLimitHitCallback(std::move(callback));
}

void tr_sessionSetMetadataCallback(tr_session* session, tr_session_metadata_func callback)
{
    session->setMetadataCallback(std::move(callback));
}

void tr_sessionSetCompletenessCallback(tr_session* session, tr_torrent_completeness_func callback)
{
    session->setTorrentCompletenessCallback(std::move(callback));
}

tr_session_stats tr_sessionGetStats(tr_session const* session)
{
    return session->stats().current();
}

tr_session_stats tr_sessionGetCumulativeStats(tr_session const* session)
{
    return session->stats().cumulative();
}

void tr_sessionClearStats(tr_session* session)
{
    session->stats().clear();
}

// ---

namespace
{
auto constexpr QueueInterval = 1s;
auto constexpr SaveInterval = 360s;

[[nodiscard]] auto makeSubdir(std::string_view const parent, std::string_view const child)
{
    auto dir = fmt::format("{:s}/{:s}"sv, parent, child);
    tr_sys_dir_create(dir, TR_SYS_DIR_CREATE_PARENTS, 0777);
    return dir;
}

auto makeResumeDir(std::string_view config_dir)
{
#if defined(__APPLE__) || defined(_WIN32)
    return makeSubdir(config_dir, "Resume"sv);
#else
    return makeSubdir(config_dir, "resume"sv);
#endif
}

auto makeTorrentDir(std::string_view config_dir)
{
#if defined(__APPLE__) || defined(_WIN32)
    return makeSubdir(config_dir, "Torrents"sv);
#else
    return makeSubdir(config_dir, "torrents"sv);
#endif
}

auto makeBlocklistDir(std::string_view config_dir)
{
    return makeSubdir(config_dir, "blocklists"sv);
}
} // namespace

tr_session::tr_session(std::string_view config_dir, tr_variant const& settings_dict)
    : config_dir_{ config_dir }
    , resume_dir_{ makeResumeDir(config_dir) }
    , torrent_dir_{ makeTorrentDir(config_dir) }
    , blocklist_dir_{ makeBlocklistDir(config_dir) }
    , session_thread_{ tr_session_thread::create() }
    , timer_maker_{ std::make_unique<tr::EvTimerMaker>(event_base()) }
    , settings_{ settings_dict }
    , session_id_{ tr_time }
    , peer_mgr_{ tr_peerMgrNew(this), &tr_peerMgrFree }
    , rpc_server_{ std::make_unique<tr_rpc_server>(this, tr_rpc_server::Settings{ settings_dict }) }
    , now_timer_{ timer_maker_->create([this]() { on_now_timer(); }) }
    , queue_timer_{ timer_maker_->create([this]() { on_queue_timer(); }) }
    , save_timer_{ timer_maker_->create([this]() { on_save_timer(); }) }
    , usenet_eviction_timer_{ timer_maker_->create([this]() { scanUsenetEvictionCandidates(); }) }
    , usenet_eviction_trigger_timer_{ timer_maker_->create(
          [this]()
          {
              usenet_eviction_scan_pending_ = false;
              scanUsenetEvictionCandidates();
          }) }
{
    now_timer_->start_repeating(1s);
    queue_timer_->start_repeating(QueueInterval);
    save_timer_->start_repeating(SaveInterval);
}

void tr_session::addIncoming(std::shared_ptr<tr_peer_socket> socket)
{
    tr_peerMgrAddIncoming(peer_mgr_.get(), std::move(socket));
}

std::optional<std::string> tr_session::addTorrent(tr_torrent* tor)
{
    tor->init_id(torrents().add(tor));
    torrent_queue_.add(tor->id());

    tr_peerMgrAddTorrent(peer_mgr_.get(), tor);

    return ensureUsenetTorrent(tor);
}

std::optional<std::string> tr_session::ensureUsenetTorrent(tr_torrent* const tor)
{
    if (usenet_piece_store_ == nullptr || tor == nullptr || !tor->has_metainfo())
    {
        return {};
    }

    auto error = std::optional<std::string>{};
    auto interrupted_uploads = std::vector<tr_piece_index_t>{};
    auto interrupted_repairs = std::vector<tr_piece_index_t>{};
    auto interrupted_discovery = false;
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        error = usenet_piece_store_->ensure_torrent(tor->metainfo());
        if (!error)
        {
            error = usenet_piece_store_->reset_interrupted_uploads(tor->info_hash_string(), interrupted_uploads);
        }
        if (!error)
        {
            auto manifest = usenet_piece_store_->load(tor->info_hash_string());
            auto manifest_changed = false;
            if (manifest && manifest->reset_interrupted_discovery(static_cast<uint64_t>(tr_time())))
            {
                interrupted_discovery = true;
                manifest_changed = true;
            }
            if (manifest && manifest->integrity.state == tr_usenet_integrity_state::Checking)
            {
                manifest->integrity.state = tr_usenet_integrity_state::Error;
                manifest->integrity.finished_at = static_cast<uint64_t>(tr_time());
                manifest->integrity.error = "Previous Usenet integrity audit was interrupted";
                manifest_changed = true;
            }
            if (manifest && manifest->integrity.state == tr_usenet_integrity_state::Repairing)
            {
                for (tr_piece_index_t piece = 0; piece < tor->piece_count(); ++piece)
                {
                    if (manifest->pieces[piece].state == tr_usenet_piece_state::Failed && tor->has_piece(piece))
                    {
                        interrupted_repairs.push_back(piece);
                    }
                }
            }
            if (manifest && manifest_changed && !usenet_piece_store_->save(*manifest))
            {
                error = "Could not save recovered Usenet manifest state";
            }
        }
    }

    if (error)
    {
        auto const message = *error;
        tr_logAddErrorTor(tor, std::move(*error));
        tor->error().set_local_error(message);
        return message;
    }

    for (auto const piece : interrupted_uploads)
    {
        if (tor->has_piece(piece))
        {
            onUsenetPieceCompleted(*tor, piece);
            tr_logAddTraceTor(tor, fmt::format("Requeued interrupted Usenet upload for piece {}", piece));
        }
    }

    for (auto const piece : interrupted_repairs)
    {
        if (std::find(std::begin(interrupted_uploads), std::end(interrupted_uploads), piece) == std::end(interrupted_uploads))
        {
            onUsenetPieceCompleted(*tor, piece);
            tr_logAddTraceTor(tor, fmt::format("Requeued interrupted Usenet repair for piece {}", piece));
        }
    }

    if (interrupted_discovery)
    {
        queueUsenetUploadsForLocalPieces(*tor);
        tr_logAddInfoTor(tor, "Recovered interrupted Usenet discovery and resumed local uploads");
    }

    (void)queueUsenetIntegrityAudit(*tor, false);
    auto resume_queued_integrity_audit = false;
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto const manifest = usenet_piece_store_->load(tor->info_hash_string());
        resume_queued_integrity_audit = manifest && manifest->integrity.state == tr_usenet_integrity_state::Queued;
    }
    if (resume_queued_integrity_audit)
    {
        (void)queueUsenetIntegrityAudit(*tor, true);
    }
    return {};
}

void tr_session::maybeQueueUsenetDiscovery(tr_torrent const& tor)
{
    if (auto const error = queueUsenetDiscovery(tor, false); error)
    {
        tr_logAddTraceTor(&tor, fmt::format("Automatic Usenet discovery not queued: {}", *error));
    }
}

std::optional<std::string> tr_session::queueUsenetDiscovery(tr_torrent const& tor, bool const manual)
{
    static constexpr auto AutomaticDiscoveryRetryCooldown = uint64_t{ 30U * 60U };
    if (usenet_piece_store_ == nullptr || !settings_.usenet_enabled)
    {
        return "Usenet backend is disabled";
    }
    if (!settings_.usenet_discovery_enabled)
    {
        return "Usenet discovery is disabled";
    }
    if (!tor.has_metainfo())
    {
        return "Torrent metadata is not available";
    }

    auto samples = std::vector<UsenetDiscoverySample>{};
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto manifest = usenet_piece_store_->load(tor.info_hash_string());
        if (!manifest)
        {
            return "Usenet manifest is missing or incomplete";
        }

        if (manifest->discovery.state == tr_usenet_discovery_state::Checking)
        {
            return "Usenet discovery is already running";
        }
        if (tr_usenet_discovery_is_blocked_by_integrity(manifest->integrity.state))
        {
            return "Usenet integrity audit or repair is already running";
        }
        if (!manual && manifest->discovery.state == tr_usenet_discovery_state::Available)
        {
            return "Torrent is already discovered on Usenet";
        }
        auto const now = static_cast<uint64_t>(tr_time());
        if (!manual && manifest->discovery.retry_after > now)
        {
            return fmt::format("Automatic Usenet discovery is cooling down until {}", manifest->discovery.retry_after);
        }
        if (!manual && !tr_usenet_discovery_evidence_ready(manifest->discovery, manifest->piece_count()))
        {
            return "Verified duplicate evidence threshold has not been reached";
        }

        auto const candidate_count = std::min<size_t>(
            manifest->piece_count(),
            settings_.usenet_discovery_sample_size + std::size(manifest->discovery.duplicate_verified_pieces));
        auto sample_pieces = tr_usenet_discovery_sample_pieces(
            tor.info_hash_string(),
            manifest->piece_count(),
            candidate_count);
        sample_pieces = tr_usenet_prioritize_discovery_samples(
            std::move(sample_pieces),
            manifest->discovery.duplicate_verified_pieces,
            settings_.usenet_discovery_sample_size);
        if (std::empty(sample_pieces))
        {
            return "No Usenet discovery samples are available";
        }

        samples.reserve(std::size(sample_pieces));
        for (auto const piece : sample_pieces)
        {
            samples.push_back(
                {
                    .piece = piece,
                    .message_id = manifest->pieces[piece].message_id,
                    .expected_size = tor.piece_size(piece),
                    .expected_hash = tor.piece_hash(piece),
                });
        }

        manifest->discovery.state = tr_usenet_discovery_state::Checking;
        manifest->discovery.trigger = manual ? tr_usenet_discovery_trigger::Manual :
                                               tr_usenet_discovery_trigger::DuplicateEvidence;
        manifest->discovery.checked_at = 0U;
        manifest->discovery.sample_size = settings_.usenet_discovery_sample_size;
        manifest->discovery.sampled_pieces = sample_pieces;
        manifest->discovery.error.clear();
        if (!manual)
        {
            manifest->discovery.retry_after = now + AutomaticDiscoveryRetryCooldown;
        }
        if (!usenet_piece_store_->save(*manifest))
        {
            return "Could not save Usenet discovery state";
        }
    }

    cancelPendingUsenetUploadsForDiscovery(tor.info_hash_string());

    auto const sample_count = std::size(samples);
    enqueueUsenetDiscoveryTask(
        {
            .torrent_id = tor.id(),
            .info_hash_string = std::string{ tor.info_hash_string() },
            .samples = std::move(samples),
            .requested_sample_size = settings_.usenet_discovery_sample_size,
        });

    tr_logAddInfoTor(
        &tor,
        fmt::format("Queued {} Usenet discovery with {} sample piece(s)", manual ? "manual" : "automatic", sample_count));
    return {};
}

void tr_session::queueUsenetUploadsForLocalPieces(tr_torrent const& tor)
{
    if (usenet_piece_store_ == nullptr || !tor.has_metainfo())
    {
        return;
    }

    auto pieces = std::vector<tr_piece_index_t>{};
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto const manifest = usenet_piece_store_->load(tor.info_hash_string());
        if (!manifest)
        {
            return;
        }

        auto const piece_count = std::min<tr_piece_index_t>(tor.piece_count(), manifest->piece_count());
        for (tr_piece_index_t piece = 0; piece < piece_count; ++piece)
        {
            if (manifest->pieces[piece].state == tr_usenet_piece_state::Unknown && tor.has_piece(piece))
            {
                pieces.push_back(piece);
            }
        }
    }

    for (auto const piece : pieces)
    {
        onUsenetPieceCompleted(tor, piece);
    }

    if (!std::empty(pieces))
    {
        tr_logAddTraceTor(&tor, fmt::format("Queued {} local piece(s) for Usenet upload", std::size(pieces)));
    }
}

bool tr_session::hasUsenetPiece(tr_torrent const& tor, tr_piece_index_t const piece)
{
    if (usenet_piece_store_ == nullptr || !tor.has_metainfo())
    {
        return false;
    }

    auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
    auto const entry = usenet_piece_store_->piece_entry(tor.info_hash_string(), piece);
    return entry && entry->state == tr_usenet_piece_state::Available;
}

bool tr_session::isUsenetServableComplete(tr_torrent const& tor)
{
    if (usenet_piece_store_ == nullptr || !settings_.usenet_enabled || !tor.has_metainfo() || tor.piece_count() == 0U)
    {
        return false;
    }

    if (!usenet_piece_store_->is_piece_size_eligible(tor.piece_size()))
    {
        return false;
    }

    auto const available = usenetPieceAvailability(tor);
    for (tr_piece_index_t piece = 0; piece < tor.piece_count(); ++piece)
    {
        if (!tor.has_piece(piece) && !available.test(piece))
        {
            return false;
        }
    }

    return true;
}

size_t tr_session::usenetIoLimit() const
{
    auto constexpr MaxUsenetIoConcurrency = size_t{ 64U };
    return std::clamp(settings_.usenet_upload_concurrency, size_t{ 1U }, MaxUsenetIoConcurrency);
}

tr_usenet_runtime_snapshot tr_session::usenetRuntimeSnapshot()
{
    auto snapshot = tr_usenet_runtime_snapshot{
        .enabled = settings_.usenet_enabled,
        .auto_integrity_audit_enabled = settings_.usenet_auto_integrity_audit_enabled,
        .evict_after_readback = settings_.usenet_evict_after_readback,
        .eviction_enabled = settings_.usenet_eviction_enabled,
        .discovery_enabled = settings_.usenet_discovery_enabled,
        .io_limit = usenetIoLimit(),
        .io_active = 0U,
        .upload_queue_size = 0U,
        .download_queue_size = 0U,
        .download_in_flight = 0U,
        .upload_concurrency = std::min(usenetIoLimit(), MaxConcurrentNyuuUploads),
        .eviction_min_age_minutes = settings_.usenet_eviction_min_age_minutes,
        .cache_size_mib = settings_.usenet_cache_size_mib,
        .discovery_sample_size = settings_.usenet_discovery_sample_size,
    };

    {
        auto lock = std::lock_guard{ usenet_io_mutex_ };
        snapshot.io_limit = usenet_io_limit_;
        snapshot.io_active = usenet_io_active_;
    }

    {
        auto lock = std::lock_guard{ usenet_upload_mutex_ };
        snapshot.upload_queue_size = std::size(usenet_upload_queue_);
    }

    {
        auto lock = std::lock_guard{ usenet_download_mutex_ };
        snapshot.download_queue_size = std::size(usenet_download_queue_);
        snapshot.download_in_flight = std::size(usenet_download_in_flight_);
    }

    if (!snapshot.enabled)
    {
        snapshot.io_limit = 0U;
        snapshot.io_active = 0U;
        snapshot.upload_queue_size = 0U;
        snapshot.download_queue_size = 0U;
        snapshot.download_in_flight = 0U;
        snapshot.upload_concurrency = 0U;
    }

    return snapshot;
}

tr_usenet_piece_summary tr_session::usenetPieceSummary(tr_torrent const& tor)
{
    auto summary = tr_usenet_piece_summary{};
    if (!tor.has_metainfo())
    {
        return summary;
    }

    summary.piece_count = tor.piece_count();

    for (tr_piece_index_t piece = 0; piece < tor.piece_count(); ++piece)
    {
        if (tor.has_piece(piece))
        {
            ++summary.local_piece_count;
        }
    }

    auto manifest = std::optional<tr_usenet_piece_manifest>{};
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        if (usenet_piece_store_ == nullptr)
        {
            summary.servable = summary.local_piece_count;
            return summary;
        }

        summary.eligible = usenet_piece_store_->is_piece_size_eligible(tor.piece_size());
        manifest = usenet_piece_store_->load(tor.info_hash_string());
    }

    if (!manifest)
    {
        summary.servable = summary.local_piece_count;
        return summary;
    }

    summary.manifest_present = true;
    summary.discovery = manifest->discovery;
    summary.integrity = manifest->integrity;

    for (tr_piece_index_t piece = 0; piece < tor.piece_count(); ++piece)
    {
        auto const state = piece < std::size(manifest->pieces) ? manifest->pieces[piece].state : tr_usenet_piece_state::Unknown;

        switch (state)
        {
        case tr_usenet_piece_state::Unknown:
            ++summary.unknown;
            break;

        case tr_usenet_piece_state::Uploading:
            ++summary.uploading;
            break;

        case tr_usenet_piece_state::Available:
            ++summary.available;
            if (manifest->pieces[piece].verified_at != 0U)
            {
                ++summary.verified;
            }
            break;

        case tr_usenet_piece_state::Failed:
            ++summary.failed;
            break;
        }

        if (tor.has_piece(piece) || state == tr_usenet_piece_state::Available)
        {
            ++summary.servable;
        }
    }

    return summary;
}

void tr_session::startUsenetEvictionTimer()
{
    if (usenet_piece_store_ == nullptr || !settings_.usenet_eviction_enabled)
    {
        return;
    }

    usenet_eviction_timer_->start_repeating(std::chrono::duration_cast<std::chrono::milliseconds>(UsenetEvictionInterval));
    scanUsenetEvictionCandidates();
}

void tr_session::scheduleUsenetEvictionScan()
{
    if (usenet_eviction_scan_pending_ || usenet_piece_store_ == nullptr || !settings_.usenet_eviction_enabled)
    {
        return;
    }

    usenet_eviction_scan_pending_ = true;
    usenet_eviction_trigger_timer_->start_single_shot(1s);
}

void tr_session::scanUsenetEvictionCandidates()
{
    if (usenet_piece_store_ == nullptr || !settings_.usenet_eviction_enabled)
    {
        return;
    }

    struct Candidate
    {
        tr_torrent* tor = nullptr;
        tr_piece_index_t piece = 0U;
        uint64_t size = 0U;
    };

    auto const now = static_cast<uint64_t>(tr_time());
    auto const min_age_seconds = static_cast<uint64_t>(settings_.usenet_eviction_min_age_minutes) * 60U;
    auto candidates = std::vector<Candidate>{};
    auto eligible_byte_count = uint64_t{};

    for (auto* const tor : torrents())
    {
        if (tor == nullptr || !tor->has_metainfo())
        {
            continue;
        }

        auto manifest = std::optional<tr_usenet_piece_manifest>{};
        {
            auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
            manifest = usenet_piece_store_->load(tor->info_hash_string());
        }

        if (!manifest)
        {
            continue;
        }
        cacheUsenetPieceAvailability(*tor, *manifest);
        if (!tr_usenet_manifest_allows_eviction(manifest->integrity.state, settings_.usenet_evict_after_readback))
        {
            continue;
        }

        auto const piece_count = std::min<tr_piece_index_t>(tor->piece_count(), manifest->piece_count());
        for (tr_piece_index_t piece = 0; piece < piece_count; ++piece)
        {
            if (tr_usenet_piece_is_eviction_eligible(manifest->pieces[piece], tor->has_piece(piece), now, min_age_seconds))
            {
                auto const size = tor->piece_size(piece);
                candidates.push_back({ .tor = tor, .piece = piece, .size = size });
                eligible_byte_count += size;
            }
        }
    }

    if (std::empty(candidates))
    {
        return;
    }

    tr_logAddTrace(
        fmt::format(
            "Usenet local piece eviction scan found {} eligible piece(s), {} byte(s)",
            std::size(candidates),
            eligible_byte_count));

    auto evicted_piece_count = size_t{};
    auto evicted_byte_count = uint64_t{};
    for (auto const& candidate : candidates)
    {
        if (candidate.tor == nullptr || !candidate.tor->has_piece(candidate.piece))
        {
            continue;
        }

        auto error = tr_error{};
        if (!punch_torrent_piece_holes(*candidate.tor, candidate.piece, error))
        {
            tr_logAddTraceTor(
                candidate.tor,
                fmt::format("Skipped Usenet local piece eviction for piece {}: {}", candidate.piece, error.message()));
            continue;
        }

        candidate.tor->mark_piece_evicted(candidate.piece);
        ++evicted_piece_count;
        evicted_byte_count += candidate.size;
        tr_logAddTraceTor(candidate.tor, fmt::format("Evicted local Usenet-backed piece {}", candidate.piece));
    }

    if (evicted_piece_count != 0U)
    {
        tr_logAddInfo(
            fmt::format("Evicted {} Usenet-backed local piece(s), {} byte(s)", evicted_piece_count, evicted_byte_count));
    }
}

void tr_session::startUsenetIoLimiter()
{
    auto lock = std::lock_guard{ usenet_io_mutex_ };
    usenet_io_limit_ = usenetIoLimit();
    usenet_io_active_ = 0U;
    usenet_io_stopping_ = false;
}

void tr_session::stopUsenetIoLimiter()
{
    {
        auto lock = std::lock_guard{ usenet_io_mutex_ };
        usenet_io_stopping_ = true;
    }

    usenet_io_cv_.notify_all();
}

bool tr_session::acquireUsenetIoSlot()
{
    return acquireUsenetIoSlots(1U);
}

bool tr_session::acquireUsenetIoSlots(size_t const count)
{
    auto lock = std::unique_lock{ usenet_io_mutex_ };
    auto const slots = std::clamp(count, size_t{ 1U }, usenet_io_limit_);
    usenet_io_cv_.wait(lock, [this, slots]() { return usenet_io_stopping_ || usenet_io_active_ + slots <= usenet_io_limit_; });

    if (usenet_io_stopping_)
    {
        return false;
    }

    usenet_io_active_ += slots;
    return true;
}

void tr_session::releaseUsenetIoSlot()
{
    releaseUsenetIoSlots(1U);
}

void tr_session::releaseUsenetIoSlots(size_t const count)
{
    {
        auto lock = std::lock_guard{ usenet_io_mutex_ };
        auto const slots = std::max<size_t>(1U, count);
        TR_ASSERT(usenet_io_active_ >= slots);
        usenet_io_active_ -= slots;
    }

    usenet_io_cv_.notify_all();
}

void tr_session::addUsenetPiecesToBitfield(tr_torrent const& tor, std::vector<uint8_t>& bitfield)
{
    auto const available = usenetPieceAvailability(tor);
    auto const piece_count = std::min<size_t>(tor.piece_count(), available.size());
    for (size_t piece = 0; piece < piece_count; ++piece)
    {
        if (available.test(piece))
        {
            bitfield[piece / 8U] |= static_cast<uint8_t>(uint8_t{ 0x80U } >> (piece % 8U));
        }
    }
}

tr_bitfield tr_session::usenetPieceAvailability(tr_torrent const& tor)
{
    auto available = tr_bitfield{ tor.has_metainfo() ? tor.piece_count() : 0U };
    if (usenet_piece_store_ == nullptr || !tor.has_metainfo())
    {
        return available;
    }

    auto const info_hash_string = std::string{ tor.info_hash_string() };
    auto const now = std::chrono::steady_clock::now();
    {
        auto lock = std::lock_guard{ usenet_piece_availability_mutex_ };
        auto const iter = std::ranges::find(
            usenet_piece_availability_cache_,
            info_hash_string,
            &UsenetPieceAvailabilityCache::info_hash_string);
        if (iter != std::end(usenet_piece_availability_cache_) && iter->piece_count == tor.piece_count() &&
            now - iter->updated_at < 2s)
        {
            available.set_raw(std::data(iter->raw), std::size(iter->raw));
            return available;
        }
    }

    auto manifest = std::optional<tr_usenet_piece_manifest>{};
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        manifest = usenet_piece_store_->load(info_hash_string);
    }
    if (!manifest)
    {
        return available;
    }

    cacheUsenetPieceAvailability(tor, *manifest);

    auto lock = std::lock_guard{ usenet_piece_availability_mutex_ };
    auto const iter = std::ranges::find(
        usenet_piece_availability_cache_,
        info_hash_string,
        &UsenetPieceAvailabilityCache::info_hash_string);
    if (iter != std::end(usenet_piece_availability_cache_))
    {
        available.set_raw(std::data(iter->raw), std::size(iter->raw));
    }

    return available;
}

void tr_session::cacheUsenetPieceAvailability(tr_torrent const& tor, tr_usenet_piece_manifest const& manifest)
{
    auto available = tr_bitfield{ tor.piece_count() };

    auto const piece_count = std::min<tr_piece_index_t>(tor.piece_count(), manifest.piece_count());
    for (tr_piece_index_t piece = 0; piece < piece_count; ++piece)
    {
        if (manifest.is_available(piece))
        {
            available.set(piece);
        }
    }

    auto entry = UsenetPieceAvailabilityCache{
        .info_hash_string = std::string{ tor.info_hash_string() },
        .raw = available.raw(),
        .piece_count = tor.piece_count(),
        .updated_at = std::chrono::steady_clock::now(),
    };
    auto lock = std::lock_guard{ usenet_piece_availability_mutex_ };
    auto const iter = std::ranges::find(
        usenet_piece_availability_cache_,
        entry.info_hash_string,
        &UsenetPieceAvailabilityCache::info_hash_string);
    if (iter == std::end(usenet_piece_availability_cache_))
    {
        usenet_piece_availability_cache_.push_back(std::move(entry));
    }
    else
    {
        *iter = std::move(entry);
    }
}

bool tr_session::isUsenetDownloadInFlight(std::string_view const info_hash_string, tr_piece_index_t const piece) const
{
    return std::ranges::any_of(
        usenet_download_in_flight_,
        [info_hash_string, piece](auto const& item) { return item.first == info_hash_string && item.second == piece; });
}

void tr_session::removeUsenetDownloadInFlight(std::string_view const info_hash_string, tr_piece_index_t const piece)
{
    std::erase_if(
        usenet_download_in_flight_,
        [info_hash_string, piece](auto const& item) { return item.first == info_hash_string && item.second == piece; });
}

void tr_session::fetchUsenetPiece(tr_torrent const& tor, tr_piece_index_t const piece)
{
    if (usenet_piece_store_ == nullptr || !tor.has_metainfo() || tor.has_piece(piece))
    {
        return;
    }

    auto const info_hash_string = std::string{ tor.info_hash_string() };
    auto entry = std::optional<tr_usenet_piece_entry>{};
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        entry = usenet_piece_store_->piece_entry(info_hash_string, piece);
    }

    if (!entry || entry->state != tr_usenet_piece_state::Available)
    {
        return;
    }

    {
        auto lock = std::lock_guard{ usenet_download_mutex_ };
        if (usenet_download_stopping_ || isUsenetDownloadInFlight(info_hash_string, piece))
        {
            return;
        }

        usenet_download_in_flight_.emplace_back(info_hash_string, piece);
    }

    enqueueUsenetDownloadTask(
        {
            .torrent_id = tor.id(),
            .info_hash_string = std::move(info_hash_string),
            .piece = piece,
            .message_id = entry->message_id,
            .expected_size = tor.piece_size(piece),
            .expected_hash = tor.piece_hash(piece),
        });

    tr_logAddTraceTor(&tor, fmt::format("Queued piece {} for Usenet download", piece));
}

void tr_session::startUsenetDiscoveryWorker()
{
    if (usenet_discovery_thread_ != nullptr)
    {
        return;
    }

    usenet_discovery_stopping_ = false;
    usenet_discovery_thread_ = std::make_unique<std::thread>(&tr_session::usenetDiscoveryWorker, this);
}

void tr_session::stopUsenetDiscoveryWorker()
{
    {
        auto lock = std::lock_guard{ usenet_discovery_mutex_ };
        usenet_discovery_stopping_ = true;
    }
    usenet_discovery_cv_.notify_one();

    if (usenet_discovery_thread_ != nullptr && usenet_discovery_thread_->joinable())
    {
        usenet_discovery_thread_->join();
    }

    usenet_discovery_thread_.reset();

    {
        auto lock = std::lock_guard{ usenet_discovery_mutex_ };
        usenet_discovery_queue_.clear();
    }
}

void tr_session::enqueueUsenetDiscoveryTask(UsenetDiscoveryTask task)
{
    {
        auto lock = std::lock_guard{ usenet_discovery_mutex_ };
        if (usenet_discovery_stopping_)
        {
            return;
        }

        usenet_discovery_queue_.push_back(std::move(task));
    }

    usenet_discovery_cv_.notify_one();
}

void tr_session::usenetDiscoveryWorker()
{
    for (;;)
    {
        auto task = UsenetDiscoveryTask{};
        {
            auto lock = std::unique_lock{ usenet_discovery_mutex_ };
            usenet_discovery_cv_.wait(
                lock,
                [this]() { return usenet_discovery_stopping_ || !std::empty(usenet_discovery_queue_); });

            if (usenet_discovery_stopping_)
            {
                return;
            }

            task = std::move(usenet_discovery_queue_.front());
            usenet_discovery_queue_.pop_front();
        }

        auto result = UsenetDiscoveryResult{
            .task = std::move(task),
            .state = tr_usenet_discovery_state::Available,
            .error = {},
        };

        for (auto const& sample : result.task.samples)
        {
            if (!acquireUsenetIoSlot())
            {
                result.state = tr_usenet_discovery_state::Error;
                result.error = "Usenet IO is stopping";
                break;
            }

            auto chain = tr_usenet_download_result{};
            auto error = tr_usenet_download_piece_chain(
                {
                    .config_dir = config_dir_,
                    .message_id = sample.message_id,
                    .expected_size = sample.expected_size,
                    .expected_hash = sample.expected_hash,
                },
                chain);
            releaseUsenetIoSlot();

            if (!error)
            {
                continue;
            }

            result.state = tr_usenet_discovery_state::Missing;
            result.error = fmt::format("Sample piece {} failed Usenet chain validation: {}", sample.piece, *error);
            break;
        }

        {
            auto lock = std::lock_guard{ usenet_discovery_mutex_ };
            if (usenet_discovery_stopping_)
            {
                return;
            }
        }

        queue_session_thread([this, result = std::move(result)]() mutable { onUsenetDiscoveryFinished(std::move(result)); });
    }
}

void tr_session::onUsenetDiscoveryFinished(UsenetDiscoveryResult result)
{
    auto* const tor = torrents_.get(result.task.torrent_id);

    auto store_error = std::optional<std::string>{};
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        if (usenet_piece_store_ == nullptr)
        {
            return;
        }

        auto manifest = usenet_piece_store_->load(result.task.info_hash_string);
        if (!manifest || manifest->discovery.state != tr_usenet_discovery_state::Checking)
        {
            return;
        }

        manifest->discovery.state = result.state;
        manifest->discovery.checked_at = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());
        manifest->discovery.sample_size = result.task.requested_sample_size;
        manifest->discovery.error = result.error;

        if (result.state == tr_usenet_discovery_state::Available)
        {
            for (tr_piece_index_t piece_index = 0; piece_index < manifest->piece_count(); ++piece_index)
            {
                auto& piece = manifest->pieces[piece_index];
                if (piece.state == tr_usenet_piece_state::Unknown || piece.state == tr_usenet_piece_state::Uploading)
                {
                    manifest->set_piece_state(piece_index, tr_usenet_piece_state::Available);
                    piece.verified_at = 0U;
                }
            }
            auto const verified_at = static_cast<uint64_t>(tr_time());
            for (auto const& sample : result.task.samples)
            {
                manifest->mark_message_id_verified(sample.message_id, verified_at);
            }
            manifest->discovery.retry_after = 0U;
        }
        else if (manifest->discovery.trigger == tr_usenet_discovery_trigger::DuplicateEvidence)
        {
            // A failed run consumes its evidence window. Fresh verified duplicates
            // are required before automatic discovery can run again.
            manifest->discovery.attempted_pieces.clear();
            manifest->discovery.duplicate_verified_pieces.clear();
            manifest->discovery.evidence_window_started_at = 0U;
        }

        if (!usenet_piece_store_->save(*manifest))
        {
            store_error = "Could not save Usenet discovery result";
        }
    }

    if (store_error)
    {
        tr_logAddWarn(std::move(*store_error));
        return;
    }

    if (tor != nullptr)
    {
        if (result.state == tr_usenet_discovery_state::Available)
        {
            tr_logAddInfoTor(
                tor,
                fmt::format(
                    "Usenet discovery passed with {} sample piece(s); torrent marked Usenet-available",
                    std::size(result.task.samples)));
        }
        else
        {
            tr_logAddInfoTor(
                tor,
                fmt::format("Usenet discovery ended as {}: {}", tr_usenet_discovery_state_name(result.state), result.error));
        }

        if (result.state == tr_usenet_discovery_state::Available)
        {
            (void)queueUsenetIntegrityAudit(*tor, false);
        }
        else
        {
            queueUsenetUploadsForLocalPieces(*tor);
        }
    }
}

std::optional<std::string> tr_session::queueUsenetIntegrityAudit(tr_torrent const& tor, bool const manual)
{
    if (usenet_piece_store_ == nullptr || !settings_.usenet_enabled || !tor.has_metainfo())
    {
        return "Usenet integrity audit is unavailable";
    }

    if (!manual && !settings_.usenet_auto_integrity_audit_enabled)
    {
        return {};
    }

    if (!manual && !isUsenetServableComplete(tor))
    {
        return {};
    }

    auto task = UsenetIntegrityTask{
        .torrent_id = tor.id(),
        .info_hash_string = std::string{ tor.info_hash_string() },
        .pieces = {},
    };
    auto waiting_for_uploads = false;
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto manifest = usenet_piece_store_->load(tor.info_hash_string());
        if (!manifest || manifest->piece_count() < tor.piece_count())
        {
            return "Usenet manifest is missing or incomplete";
        }
        if (tr_usenet_integrity_is_blocked_by_discovery(manifest->discovery.state))
        {
            return manual ? std::optional<std::string>{ "Usenet discovery is already running" } : std::nullopt;
        }
        if (!manual && manifest->discovery.state != tr_usenet_discovery_state::Available &&
            std::ranges::any_of(manifest->pieces, [](auto const& piece) { return piece.verified_at == 0U; }))
        {
            return {};
        }
        if (manifest->integrity.state == tr_usenet_integrity_state::Checking)
        {
            return manual ? std::optional<std::string>{ "Usenet integrity audit is already running" } : std::nullopt;
        }
        auto const backend_ready = std::ranges::all_of(
            manifest->pieces,
            [](auto const& piece) { return piece.state == tr_usenet_piece_state::Available && piece.verified_at != 0U; });
        if (manual && !backend_ready)
        {
            if (manifest->integrity.state != tr_usenet_integrity_state::Queued)
            {
                manifest->integrity = {
                    .state = tr_usenet_integrity_state::Queued,
                    .error = "Waiting for pending Usenet uploads and readbacks",
                };
                if (!usenet_piece_store_->save(*manifest))
                {
                    return "Could not save queued Usenet integrity audit state";
                }
            }
            waiting_for_uploads = true;
        }
        if (!manual && manifest->integrity.state != tr_usenet_integrity_state::NotChecked &&
            manifest->integrity.state != tr_usenet_integrity_state::Error)
        {
            return {};
        }

        if (waiting_for_uploads)
        {
            task.pieces.clear();
        }
        else
        {
            task.pieces.reserve(tor.piece_count());
            for (tr_piece_index_t piece = 0U; piece < tor.piece_count(); ++piece)
            {
                task.pieces.push_back(
                    {
                        .piece = piece,
                        .message_id = manifest->pieces[piece].message_id,
                        .expected_size = tor.piece_size(piece),
                        .expected_hash = tor.piece_hash(piece),
                    });
            }

            manifest->integrity = {
                .state = tr_usenet_integrity_state::Checking,
                .started_at = static_cast<uint64_t>(tr_time()),
                .error = {},
            };
            if (!usenet_piece_store_->save(*manifest))
            {
                return "Could not save Usenet integrity audit state";
            }
        }
    }

    if (waiting_for_uploads)
    {
        tr_logAddInfoTor(&tor, "Queued full Usenet integrity audit until pending uploads finish");
        return {};
    }

    {
        auto lock = std::lock_guard{ usenet_integrity_mutex_ };
        if (usenet_integrity_stopping_)
        {
            return "Usenet integrity worker is stopping";
        }
        usenet_integrity_queue_.push_back(std::move(task));
    }
    usenet_integrity_cv_.notify_one();
    tr_logAddInfoTor(&tor, fmt::format("Queued full Usenet integrity audit for {} piece(s)", tor.piece_count()));
    return {};
}

void tr_session::startUsenetIntegrityWorker()
{
    if (usenet_integrity_thread_ != nullptr)
    {
        return;
    }
    usenet_integrity_stopping_ = false;
    usenet_integrity_thread_ = std::make_unique<std::thread>(&tr_session::usenetIntegrityWorker, this);
}

void tr_session::stopUsenetIntegrityWorker()
{
    {
        auto lock = std::lock_guard{ usenet_integrity_mutex_ };
        usenet_integrity_stopping_ = true;
    }
    usenet_integrity_cv_.notify_one();
    if (usenet_integrity_thread_ != nullptr && usenet_integrity_thread_->joinable())
    {
        usenet_integrity_thread_->join();
    }
    usenet_integrity_thread_.reset();
    auto lock = std::lock_guard{ usenet_integrity_mutex_ };
    usenet_integrity_queue_.clear();
}

void tr_session::usenetIntegrityWorker()
{
    static constexpr auto ProgressBatchSize = size_t{ 16U };
    for (;;)
    {
        auto task = UsenetIntegrityTask{};
        {
            auto lock = std::unique_lock{ usenet_integrity_mutex_ };
            usenet_integrity_cv_.wait(
                lock,
                [this]() { return usenet_integrity_stopping_ || !std::empty(usenet_integrity_queue_); });
            if (usenet_integrity_stopping_)
            {
                return;
            }
            task = std::move(usenet_integrity_queue_.front());
            usenet_integrity_queue_.pop_front();
        }

        auto result = UsenetIntegrityResult{ .task = std::move(task), .pieces = {}, .stopped = false };
        result.pieces.reserve(std::size(result.task.pieces));
        for (auto const& piece : result.task.pieces)
        {
            {
                auto lock = std::lock_guard{ usenet_integrity_mutex_ };
                if (usenet_integrity_stopping_)
                {
                    result.stopped = true;
                    break;
                }
            }
            auto item = UsenetIntegrityPieceResult{
                .piece = piece.piece,
                .message_id = piece.message_id,
                .article_count = 0U,
                .error = {},
            };
            if (!acquireUsenetIoSlot())
            {
                result.stopped = true;
                break;
            }
            auto chain = tr_usenet_download_result{};
            if (auto error = tr_usenet_download_piece_chain(
                    {
                        .config_dir = config_dir_,
                        .message_id = piece.message_id,
                        .expected_size = piece.expected_size,
                        .expected_hash = piece.expected_hash,
                    },
                    chain);
                error)
            {
                item.error = std::move(*error);
            }
            else
            {
                item.article_count = chain.article_count;
            }
            releaseUsenetIoSlot();
            result.pieces.push_back(std::move(item));
            if (std::size(result.pieces) % ProgressBatchSize == 0U || std::size(result.pieces) == std::size(result.task.pieces))
            {
                auto const checked = std::size(result.pieces);
                auto const verified = static_cast<size_t>(std::ranges::count_if(
                    result.pieces,
                    [](auto const& checked_piece) { return std::empty(checked_piece.error); }));
                queue_session_thread(
                    [this, info_hash_string = result.task.info_hash_string, checked, verified, missing = checked - verified]()
                    { onUsenetIntegrityProgress(info_hash_string, checked, verified, missing); });
            }
        }

        queue_session_thread([this, result = std::move(result)]() mutable { onUsenetIntegrityFinished(std::move(result)); });
    }
}

void tr_session::onUsenetIntegrityProgress(
    std::string const& info_hash_string,
    size_t const checked,
    size_t const verified,
    size_t const missing)
{
    if (usenet_piece_store_ == nullptr)
    {
        return;
    }

    auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
    auto manifest = usenet_piece_store_->load(info_hash_string);
    if (!manifest || manifest->integrity.state != tr_usenet_integrity_state::Checking || checked <= manifest->integrity.checked)
    {
        return;
    }

    manifest->integrity.checked = checked;
    manifest->integrity.verified = verified;
    manifest->integrity.missing = missing;
    if (!usenet_piece_store_->save(*manifest))
    {
        tr_logAddWarn(fmt::format("Could not save Usenet integrity audit progress at {} piece(s)", checked));
    }
}

void tr_session::onUsenetIntegrityFinished(UsenetIntegrityResult result)
{
    auto* const tor = torrents_.get(result.task.torrent_id);
    if (tor == nullptr || !tor->has_metainfo() || usenet_piece_store_ == nullptr)
    {
        return;
    }

    auto repair_pieces = std::vector<tr_piece_index_t>{};
    auto outcome = tr_usenet_integrity_info{};
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto manifest = usenet_piece_store_->load(result.task.info_hash_string);
        if (!manifest || manifest->integrity.state != tr_usenet_integrity_state::Checking)
        {
            return;
        }

        auto& integrity = manifest->integrity;
        integrity.checked = 0U;
        integrity.verified = 0U;
        integrity.missing = 0U;
        integrity.repairing = 0U;
        integrity.waiting_for_peers = 0U;
        integrity.checked = std::size(result.pieces);
        integrity.finished_at = static_cast<uint64_t>(tr_time());
        integrity.error.clear();
        auto const verified_at = integrity.finished_at;
        for (auto const& item : result.pieces)
        {
            if (std::empty(item.error))
            {
                manifest->set_message_id_state(item.message_id, tr_usenet_piece_state::Available, item.article_count, 0U);
                manifest->mark_message_id_verified(item.message_id, verified_at);
                ++integrity.verified;
            }
            else
            {
                manifest->set_message_id_state(item.message_id, tr_usenet_piece_state::Failed);
                ++integrity.missing;
                if (tor->has_piece(item.piece))
                {
                    repair_pieces.push_back(item.piece);
                    ++integrity.repairing;
                }
                else
                {
                    ++integrity.waiting_for_peers;
                }
                if (std::empty(integrity.error))
                {
                    integrity.error = fmt::format("Piece {} failed: {}", item.piece, item.error);
                }
            }
        }

        if (result.stopped)
        {
            integrity.state = tr_usenet_integrity_state::Error;
            integrity.error = "Usenet integrity audit stopped before completion";
        }
        else if (integrity.missing == 0U && integrity.checked == tor->piece_count())
        {
            integrity.state = tr_usenet_integrity_state::Ready;
        }
        else if (integrity.repairing != 0U)
        {
            integrity.state = tr_usenet_integrity_state::Repairing;
        }
        else
        {
            integrity.state = tr_usenet_integrity_state::Incomplete;
        }

        if (!usenet_piece_store_->save(*manifest))
        {
            tr_logAddWarnTor(tor, "Could not save Usenet integrity audit result");
            return;
        }
        outcome = integrity;
    }

    for (auto const piece : repair_pieces)
    {
        onUsenetPieceCompleted(*tor, piece);
    }
    tr_logAddInfoTor(
        tor,
        fmt::format(
            "Usenet integrity audit finished: {} verified, {} missing, {} repairing, {} waiting for peers",
            outcome.verified,
            outcome.missing,
            outcome.repairing,
            outcome.waiting_for_peers));
}

void tr_session::startUsenetDownloadWorker()
{
    if (usenet_download_thread_ != nullptr)
    {
        return;
    }

    usenet_download_stopping_ = false;
    usenet_download_thread_ = std::make_unique<std::thread>(&tr_session::usenetDownloadWorker, this);
}

void tr_session::stopUsenetDownloadWorker()
{
    {
        auto lock = std::lock_guard{ usenet_download_mutex_ };
        usenet_download_stopping_ = true;
    }
    usenet_download_cv_.notify_one();

    if (usenet_download_thread_ != nullptr && usenet_download_thread_->joinable())
    {
        usenet_download_thread_->join();
    }

    usenet_download_thread_.reset();

    {
        auto lock = std::lock_guard{ usenet_download_mutex_ };
        usenet_download_queue_.clear();
        usenet_download_in_flight_.clear();
    }
}

void tr_session::enqueueUsenetDownloadTask(UsenetDownloadTask task)
{
    {
        auto lock = std::lock_guard{ usenet_download_mutex_ };
        if (usenet_download_stopping_)
        {
            removeUsenetDownloadInFlight(task.info_hash_string, task.piece);
            return;
        }

        usenet_download_queue_.push_back(std::move(task));
    }

    usenet_download_cv_.notify_one();
}

void tr_session::usenetDownloadWorker()
{
    for (;;)
    {
        auto task = UsenetDownloadTask{};
        {
            auto lock = std::unique_lock{ usenet_download_mutex_ };
            usenet_download_cv_.wait(
                lock,
                [this]() { return usenet_download_stopping_ || !std::empty(usenet_download_queue_); });

            if (usenet_download_stopping_)
            {
                return;
            }

            task = std::move(usenet_download_queue_.front());
            usenet_download_queue_.pop_front();
        }

        if (!acquireUsenetIoSlot())
        {
            auto lock = std::lock_guard{ usenet_download_mutex_ };
            removeUsenetDownloadInFlight(task.info_hash_string, task.piece);
            return;
        }

        auto result = UsenetDownloadResult{ .task = std::move(task), .data = {}, .article_count = 0U, .error = {} };
        auto chain = tr_usenet_download_result{};
        result.error = tr_usenet_download_piece_chain(
            {
                .config_dir = config_dir_,
                .message_id = result.task.message_id,
                .expected_size = result.task.expected_size,
                .expected_hash = result.task.expected_hash,
            },
            chain);
        result.data = std::move(chain.data);
        result.article_count = chain.article_count;
        releaseUsenetIoSlot();

        {
            auto lock = std::lock_guard{ usenet_download_mutex_ };
            if (usenet_download_stopping_)
            {
                return;
            }
        }

        queue_session_thread([this, result = std::move(result)]() mutable { onUsenetPieceDownloaded(std::move(result)); });
    }
}

void tr_session::onUsenetPieceDownloaded(UsenetDownloadResult result)
{
    {
        auto lock = std::lock_guard{ usenet_download_mutex_ };
        removeUsenetDownloadInFlight(result.task.info_hash_string, result.task.piece);
    }

    auto* const tor = torrents_.get(result.task.torrent_id);
    if (tor == nullptr || !tor->has_metainfo())
    {
        return;
    }

    if (result.error)
    {
        tr_logAddWarnTor(tor, fmt::format("Could not download piece {} from Usenet: {}", result.task.piece, *result.error));
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        (void)usenet_piece_store_->set_message_id_state(
            result.task.info_hash_string,
            result.task.message_id,
            tr_usenet_piece_state::Failed);
        return;
    }

    if (std::size(result.data) != tor->piece_size(result.task.piece))
    {
        tr_logAddWarnTor(tor, fmt::format("Usenet piece {} had an unexpected size", result.task.piece));
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        (void)usenet_piece_store_->set_message_id_state(
            result.task.info_hash_string,
            result.task.message_id,
            tr_usenet_piece_state::Failed);
        return;
    }

    if (auto const err = tr_ioWrite(
            *tor,
            openFiles(),
            tor->piece_loc(result.task.piece),
            std::span<uint8_t const>{ std::data(result.data), std::size(result.data) });
        err != 0)
    {
        tr_logAddWarnTor(
            tor,
            fmt::format("Could not write Usenet piece {} to local data: {}", result.task.piece, tr_strerror(err)));
        return;
    }

    if (!tor->install_recovered_piece(result.task.piece))
    {
        tr_logAddWarnTor(tor, fmt::format("Usenet piece {} failed its checksum test", result.task.piece));
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        (void)usenet_piece_store_->set_message_id_state(
            result.task.info_hash_string,
            result.task.message_id,
            tr_usenet_piece_state::Failed);
        return;
    }

    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        (void)usenet_piece_store_->set_message_id_state(
            result.task.info_hash_string,
            result.task.message_id,
            tr_usenet_piece_state::Available,
            result.article_count,
            0U);
        (void)usenet_piece_store_->mark_message_id_verified(
            result.task.info_hash_string,
            result.task.message_id,
            static_cast<uint64_t>(tr_time()));
    }

    tr_logAddTraceTor(tor, fmt::format("Usenet piece {} restored to local data", result.task.piece));
}

void tr_session::startUsenetUploadWorker()
{
    if (!std::empty(usenet_upload_threads_))
    {
        return;
    }

    auto const upload_concurrency = std::min(usenetIoLimit(), MaxConcurrentNyuuUploads);

    usenet_upload_stopping_ = false;
    usenet_upload_threads_.reserve(upload_concurrency);
    for (size_t i = 0; i < upload_concurrency; ++i)
    {
        usenet_upload_threads_.emplace_back(&tr_session::usenetUploadWorker, this);
    }

    tr_logAddInfo(
        fmt::format(
            "Started {} Usenet upload worker(s) with a shared Usenet IO limit of {}",
            upload_concurrency,
            usenet_io_limit_));
}

void tr_session::stopUsenetUploadWorker()
{
    {
        auto lock = std::lock_guard{ usenet_upload_mutex_ };
        usenet_upload_stopping_ = true;
    }
    usenet_upload_cv_.notify_all();

    for (auto& thread : usenet_upload_threads_)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }

    usenet_upload_threads_.clear();

    auto queue = std::deque<UsenetUploadTask>{};
    {
        auto lock = std::lock_guard{ usenet_upload_mutex_ };
        queue.swap(usenet_upload_queue_);
    }

    for (auto const& task : queue)
    {
        tr_sys_path_remove(task.temp_file);
    }
}

void tr_session::enqueueUsenetUploadTask(UsenetUploadTask task)
{
    {
        auto lock = std::lock_guard{ usenet_upload_mutex_ };
        if (usenet_upload_stopping_)
        {
            tr_sys_path_remove(task.temp_file);
            return;
        }

        usenet_upload_queue_.push_back(std::move(task));
    }

    usenet_upload_cv_.notify_one();
}

void tr_session::cancelPendingUsenetUploadsForDiscovery(std::string_view const info_hash_string)
{
    auto cancelled = std::vector<UsenetUploadTask>{};
    {
        auto lock = std::lock_guard{ usenet_upload_mutex_ };
        for (auto iter = std::begin(usenet_upload_queue_); iter != std::end(usenet_upload_queue_);)
        {
            if (iter->info_hash_string == info_hash_string)
            {
                cancelled.push_back(std::move(*iter));
                iter = usenet_upload_queue_.erase(iter);
            }
            else
            {
                ++iter;
            }
        }
    }

    auto reset_saved = std::empty(cancelled);
    if (usenet_piece_store_ != nullptr && !std::empty(cancelled))
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto manifest = usenet_piece_store_->load(info_hash_string);
        if (manifest)
        {
            for (auto const& task : cancelled)
            {
                if (task.piece < manifest->piece_count() &&
                    manifest->pieces[task.piece].state == tr_usenet_piece_state::Uploading)
                {
                    manifest->set_message_id_state(task.message_id, tr_usenet_piece_state::Unknown);
                }
            }
            if (!usenet_piece_store_->save(*manifest))
            {
                tr_logAddWarn(fmt::format("Could not reset cancelled Usenet uploads for torrent {}", info_hash_string));
            }
            else
            {
                reset_saved = true;
            }
        }
    }

    if (!reset_saved)
    {
        {
            auto lock = std::lock_guard{ usenet_upload_mutex_ };
            for (auto iter = std::rbegin(cancelled); iter != std::rend(cancelled); ++iter)
            {
                usenet_upload_queue_.push_front(std::move(*iter));
            }
        }
        usenet_upload_cv_.notify_all();
        tr_logAddWarn(fmt::format("Restored {} Usenet upload(s) after discovery hold failed", std::size(cancelled)));
        return;
    }

    for (auto const& task : cancelled)
    {
        tr_sys_path_remove(task.temp_file);
    }
    if (!std::empty(cancelled))
    {
        tr_logAddInfo(fmt::format("Held {} pending Usenet upload(s) for discovery", std::size(cancelled)));
    }
}

bool tr_session::holdUsenetUploadBatchForDiscovery(std::vector<UsenetUploadTask> const& batch)
{
    if (usenet_piece_store_ == nullptr || std::empty(batch))
    {
        return false;
    }

    auto const& info_hash_string = batch.front().info_hash_string;
    auto held = false;
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto manifest = usenet_piece_store_->load(info_hash_string);
        if (!manifest || manifest->discovery.state != tr_usenet_discovery_state::Checking)
        {
            return false;
        }

        for (auto const& task : batch)
        {
            if (task.info_hash_string == info_hash_string && task.piece < manifest->piece_count() &&
                manifest->pieces[task.piece].state == tr_usenet_piece_state::Uploading)
            {
                manifest->set_message_id_state(task.message_id, tr_usenet_piece_state::Unknown);
                held = true;
            }
        }

        if (held && !usenet_piece_store_->save(*manifest))
        {
            tr_logAddWarn(fmt::format("Could not reset held Usenet upload batch for torrent {}", info_hash_string));
            held = false;
        }
    }

    if (held)
    {
        for (auto const& task : batch)
        {
            tr_sys_path_remove(task.temp_file);
        }
        tr_logAddInfo(fmt::format("Held {} staged Usenet upload(s) for discovery", std::size(batch)));
    }
    return held;
}

void tr_session::usenetUploadWorker()
{
    for (;;)
    {
        auto batch = std::vector<UsenetUploadTask>{};
        {
            auto lock = std::unique_lock{ usenet_upload_mutex_ };
            usenet_upload_cv_.wait(lock, [this]() { return usenet_upload_stopping_ || !std::empty(usenet_upload_queue_); });

            if (usenet_upload_stopping_)
            {
                return;
            }

            batch.push_back(std::move(usenet_upload_queue_.front()));
            usenet_upload_queue_.pop_front();
            auto article_count = batch.front().article_count;
            while (!std::empty(usenet_upload_queue_) &&
                   usenet_upload_queue_.front().info_hash_string == batch.front().info_hash_string &&
                   article_count + usenet_upload_queue_.front().article_count <= MaxNyuuBatchFiles)
            {
                article_count += usenet_upload_queue_.front().article_count;
                batch.push_back(std::move(usenet_upload_queue_.front()));
                usenet_upload_queue_.pop_front();
            }
        }

        if (holdUsenetUploadBatchForDiscovery(batch))
        {
            continue;
        }

        auto finish_task = [this](
                               UsenetUploadTask task,
                               bool const upload_attempted,
                               bool const duplicate_verified,
                               bool const success,
                               std::string error)
        {
            queue_session_thread(
                [this,
                 task = std::move(task),
                 upload_attempted,
                 duplicate_verified,
                 success,
                 error = std::move(error)]() mutable
                {
                    onUsenetPieceUploadFinished(
                        std::move(task.info_hash_string),
                        task.piece,
                        std::move(task.message_id),
                        std::move(task.temp_file),
                        task.article_count,
                        task.article_payload_size,
                        upload_attempted,
                        duplicate_verified,
                        success,
                        std::move(error));
                });
        };
        auto verify_task = [this](UsenetUploadTask const& task) -> std::optional<std::string>
        {
            if (!acquireUsenetIoSlot())
            {
                return "upload readback skipped because Usenet IO is stopping";
            }

            auto error = verify_usenet_upload_after_error(config_dir_, task.message_id, task.temp_file, task.piece_size);
            releaseUsenetIoSlot();
            return error;
        };

        auto batch_dir = std::string{};
        if (auto dir_error = make_usenet_batch_temp_dir(config_dir_, batch_dir); dir_error)
        {
            for (auto& task : batch)
            {
                finish_task(std::move(task), false, false, false, *dir_error);
            }
            continue;
        }

        auto batch_guard = TempDirGuard{ batch_dir };
        auto staged_paths_by_task = std::vector<std::vector<std::string>>(std::size(batch));
        auto article_payload_size = uint64_t{};
        auto staging_error = std::optional<std::string>{};

        for (size_t i = 0U; i < std::size(batch); ++i)
        {
            auto const& task = batch[i];
            if (auto error = stage_usenet_piece_parts(
                    task.temp_file,
                    batch_dir,
                    task.message_id,
                    task.piece_size,
                    task.article_payload_size,
                    staged_paths_by_task[i]);
                error)
            {
                staging_error = std::move(error);
                break;
            }

            batch_guard.files.insert(
                std::end(batch_guard.files),
                std::begin(staged_paths_by_task[i]),
                std::end(staged_paths_by_task[i]));
            article_payload_size = std::max(article_payload_size, task.article_payload_size);
        }

        if (staging_error)
        {
            for (auto& task : batch)
            {
                finish_task(std::move(task), false, false, false, *staging_error);
            }
            continue;
        }

        if (holdUsenetUploadBatchForDiscovery(batch))
        {
            continue;
        }

        auto const io_limit = usenetIoLimit();
        auto const check_connection_budget = io_limit > NyuuBatchCheckConnections ? NyuuBatchCheckConnections : 0U;
        auto staged_paths = std::vector<std::string>{};
        for (auto const& paths : staged_paths_by_task)
        {
            staged_paths.insert(std::end(staged_paths), std::begin(paths), std::end(paths));
        }
        auto const connection_count = std::clamp(
            std::min({ std::size(staged_paths), MaxNyuuBatchConnections, io_limit - check_connection_budget }),
            size_t{ 1U },
            io_limit);
        auto const reserved_io_slots = std::min(io_limit, connection_count + check_connection_budget);
        if (!acquireUsenetIoSlots(reserved_io_slots))
        {
            for (auto const& task : batch)
            {
                tr_sys_path_remove(task.temp_file);
            }
            return;
        }

        auto upload_diagnostics = tr_usenet_upload_diagnostics{};
        auto const upload_error = tr_usenet_upload_files(
            {
                .config_dir = config_dir_,
                .file_paths = std::move(staged_paths),
                .article_size = article_payload_size,
                .connections = connection_count,
            },
            &upload_diagnostics);
        releaseUsenetIoSlots(reserved_io_slots);

        if (!upload_error)
        {
            tr_logAddTrace(
                fmt::format(
                    "Usenet batch uploaded {} piece(s) using {} nyuu upload connection(s)",
                    std::size(batch),
                    connection_count));
            for (auto& task : batch)
            {
                if (auto readback_error = verify_task(task); readback_error)
                {
                    finish_task(
                        std::move(task),
                        true,
                        false,
                        false,
                        fmt::format("upload completed but mandatory readback failed: {}", *readback_error));
                }
                else
                {
                    finish_task(std::move(task), true, false, true, {});
                }
            }
            continue;
        }

        auto readback_success_count = size_t{};
        auto single_retry_success_count = size_t{};
        auto failed_count = size_t{};

        for (size_t i = 0U; i < std::size(batch); ++i)
        {
            auto& task = batch[i];
            auto success = false;
            auto duplicate_verified = false;
            auto error = *upload_error;

            if (acquireUsenetIoSlot())
            {
                if (auto readback_error = verify_usenet_upload_after_error(
                        config_dir_,
                        task.message_id,
                        task.temp_file,
                        task.piece_size);
                    readback_error)
                {
                    error += fmt::format("; batch readback check failed: {}", *readback_error);
                }
                else
                {
                    ++readback_success_count;
                    duplicate_verified = upload_diagnostics_include_piece(
                        upload_diagnostics,
                        task.message_id,
                        task.article_count);
                    finish_task(std::move(task), true, duplicate_verified, true, {});
                    releaseUsenetIoSlot();
                    continue;
                }
                releaseUsenetIoSlot();
            }
            else
            {
                error += "; batch readback check skipped because Usenet IO is stopping";
            }

            auto single_upload_error = std::optional<std::string>{ "single-piece retry skipped because Usenet IO is stopping" };
            auto single_diagnostics = tr_usenet_upload_diagnostics{};
            auto const single_retry_slots = std::min(usenetIoLimit(), size_t{ 2U });
            if (acquireUsenetIoSlots(single_retry_slots))
            {
                single_upload_error = tr_usenet_upload_files(
                    {
                        .config_dir = config_dir_,
                        .file_paths = staged_paths_by_task[i],
                        .article_size = task.article_payload_size,
                        .connections = 1U,
                    },
                    &single_diagnostics);
                releaseUsenetIoSlots(single_retry_slots);

                if (!single_upload_error)
                {
                    if (auto readback_error = verify_task(task); readback_error)
                    {
                        single_upload_error = fmt::format("mandatory readback failed: {}", *readback_error);
                    }
                    else
                    {
                        ++single_retry_success_count;
                        finish_task(std::move(task), true, false, true, {});
                        continue;
                    }
                }
            }

            error += fmt::format("; single-piece retry failed: {}", *single_upload_error);
            if (acquireUsenetIoSlot())
            {
                if (auto retry_readback_error = verify_usenet_upload_after_error(
                        config_dir_,
                        task.message_id,
                        task.temp_file,
                        task.piece_size);
                    retry_readback_error)
                {
                    error += fmt::format("; retry readback check failed: {}", *retry_readback_error);
                }
                else
                {
                    ++readback_success_count;
                    success = true;
                    duplicate_verified = upload_diagnostics_include_piece(
                                             upload_diagnostics,
                                             task.message_id,
                                             task.article_count) ||
                        upload_diagnostics_include_piece(single_diagnostics, task.message_id, task.article_count);
                    error.clear();
                }

                releaseUsenetIoSlot();
            }
            else
            {
                error += "; retry readback check skipped because Usenet IO is stopping";
            }

            if (!success)
            {
                ++failed_count;
            }
            finish_task(std::move(task), true, duplicate_verified, success, std::move(error));
        }

        tr_logAddInfo(
            fmt::format(
                "Usenet batch upload reported an error; recovered {} by readback, {} by single-piece retry, {} failed",
                readback_success_count,
                single_retry_success_count,
                failed_count));
    }
}

void tr_session::onUsenetPieceCompleted(tr_torrent const& tor, tr_piece_index_t const piece)
{
    if (usenet_piece_store_ == nullptr || !tor.has_metainfo())
    {
        return;
    }

    auto error = std::optional<std::string>{};
    auto entry = std::optional<tr_usenet_piece_entry>{};
    auto should_upload = false;
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        auto manifest = usenet_piece_store_->load(tor.info_hash_string());
        if (!manifest || piece >= manifest->piece_count())
        {
            entry = {};
        }
        else
        {
            if (manifest->discovery.state == tr_usenet_discovery_state::Checking)
            {
                return;
            }
            entry = manifest->pieces[piece];
            if (manifest->has_message_id_state(entry->message_id, tr_usenet_piece_state::Available))
            {
                error = usenet_piece_store_->set_message_id_state(
                    tor.info_hash_string(),
                    entry->message_id,
                    tr_usenet_piece_state::Available);
            }
            else if (manifest->has_message_id_state(entry->message_id, tr_usenet_piece_state::Uploading))
            {
                error = usenet_piece_store_->set_message_id_state(
                    tor.info_hash_string(),
                    entry->message_id,
                    tr_usenet_piece_state::Uploading);
            }
            else if (entry->state != tr_usenet_piece_state::Available && entry->state != tr_usenet_piece_state::Uploading)
            {
                error = usenet_piece_store_->set_message_id_state(
                    tor.info_hash_string(),
                    entry->message_id,
                    tr_usenet_piece_state::Uploading);
                should_upload = !error;
            }
            if (!error)
            {
                entry = usenet_piece_store_->piece_entry(tor.info_hash_string(), piece);
            }
        }
    }

    if (error)
    {
        tr_logAddWarnTor(&tor, fmt::format("Could not queue piece {} for Usenet upload: {}", piece, *error));
        return;
    }

    if (!entry)
    {
        tr_logAddWarnTor(&tor, fmt::format("Could not find Usenet manifest entry for piece {}", piece));
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        (void)usenet_piece_store_->set_piece_state(tor.info_hash_string(), piece, tr_usenet_piece_state::Failed);
        return;
    }

    if (!should_upload)
    {
        return;
    }

    auto temp_file = std::string{};
    if (auto write_error = write_piece_to_temp_file(tor, piece, config_dir_, temp_file); write_error)
    {
        tr_logAddWarnTor(&tor, fmt::format("Could not stage piece {} for Usenet upload: {}", piece, *write_error));
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        (void)usenet_piece_store_->set_piece_state(tor.info_hash_string(), piece, tr_usenet_piece_state::Failed);
        return;
    }

    enqueueUsenetUploadTask(
        {
            .info_hash_string = std::string{ tor.info_hash_string() },
            .piece = piece,
            .message_id = entry->message_id,
            .temp_file = std::move(temp_file),
            .piece_size = tor.piece_size(piece),
            .article_payload_size = usenet_piece_store_->max_article_size(),
            .article_count = tr_usenet_piece_article_count(tor.piece_size(piece), usenet_piece_store_->max_article_size())
                                 .value_or(0U),
        });

    tr_logAddTraceTor(&tor, fmt::format("Queued piece {} for Usenet upload", piece));
}

void tr_session::onUsenetPieceUploadFinished(
    std::string info_hash_string,
    tr_piece_index_t const piece,
    std::string message_id,
    std::string temp_file,
    size_t const article_count,
    uint64_t const article_payload_size,
    bool const upload_attempted,
    bool const duplicate_verified,
    bool const success,
    std::string error)
{
    tr_sys_path_remove(temp_file);

    if (usenet_piece_store_ == nullptr)
    {
        return;
    }

    auto const state = success ? tr_usenet_piece_state::Available : tr_usenet_piece_state::Failed;
    auto store_error = std::optional<std::string>{};
    auto evidence_ready = false;
    auto queued_integrity_audit_ready = false;
    {
        auto lock = std::lock_guard{ usenet_piece_store_mutex_ };
        store_error = usenet_piece_store_->set_message_id_state(
            info_hash_string,
            message_id,
            state,
            success ? std::optional<size_t>{ article_count } : std::nullopt,
            success ? std::optional<uint64_t>{ article_payload_size } : std::nullopt);
        if (!store_error && success)
        {
            store_error = usenet_piece_store_->mark_message_id_verified(
                info_hash_string,
                message_id,
                static_cast<uint64_t>(tr_time()));
            auto manifest = usenet_piece_store_->load(info_hash_string);
            if (!store_error && manifest &&
                std::ranges::all_of(
                    manifest->pieces,
                    [](auto const& entry)
                    { return entry.state == tr_usenet_piece_state::Available && entry.verified_at != 0U; }))
            {
                queued_integrity_audit_ready = manifest->integrity.state == tr_usenet_integrity_state::Queued;
                if (!queued_integrity_audit_ready && manifest->integrity.state != tr_usenet_integrity_state::Checking)
                {
                    manifest->integrity.state = tr_usenet_integrity_state::Ready;
                    manifest->integrity.finished_at = static_cast<uint64_t>(tr_time());
                    manifest->integrity.checked = manifest->piece_count();
                    manifest->integrity.verified = manifest->piece_count();
                    manifest->integrity.missing = 0U;
                    manifest->integrity.repairing = 0U;
                    manifest->integrity.waiting_for_peers = 0U;
                    manifest->integrity.error.clear();
                    if (!usenet_piece_store_->save(*manifest))
                    {
                        store_error = "Could not save repaired Usenet integrity state";
                    }
                }
            }
        }
        else if (!store_error)
        {
            auto manifest = usenet_piece_store_->load(info_hash_string);
            if (manifest && manifest->integrity.state == tr_usenet_integrity_state::Queued)
            {
                manifest->integrity.state = tr_usenet_integrity_state::Incomplete;
                manifest->integrity.finished_at = static_cast<uint64_t>(tr_time());
                manifest->integrity.error = fmt::format("Piece {} upload failed while integrity audit was queued", piece);
                if (!usenet_piece_store_->save(*manifest))
                {
                    store_error = "Could not save failed queued Usenet integrity audit state";
                }
            }
        }
        if (!store_error && upload_attempted)
        {
            auto manifest = usenet_piece_store_->load(info_hash_string);
            if (manifest && manifest->discovery.state != tr_usenet_discovery_state::Checking &&
                manifest->discovery.state != tr_usenet_discovery_state::Available)
            {
                evidence_ready = manifest->record_discovery_upload_attempt(
                    piece,
                    success && duplicate_verified,
                    static_cast<uint64_t>(tr_time()));
                if (!usenet_piece_store_->save(*manifest))
                {
                    store_error = "Could not save Usenet discovery upload evidence";
                }
            }
        }
    }

    if (store_error)
    {
        tr_logAddWarn(fmt::format("Could not update Usenet upload state for piece {}: {}", piece, *store_error));
        return;
    }

    if (success)
    {
        if (duplicate_verified)
        {
            tr_logAddInfo(fmt::format("Verified duplicate Usenet upload evidence for piece {}", piece));
        }
        if (evidence_ready)
        {
            tr_logAddInfo(fmt::format("Usenet duplicate evidence threshold reached for torrent {}", info_hash_string));
            if (auto const digest = tr_sha1_from_string(info_hash_string); digest)
            {
                if (auto* const tor = torrents_.get(*digest); tor != nullptr)
                {
                    maybeQueueUsenetDiscovery(*tor);
                }
            }
        }
        tr_logAddTrace(fmt::format("Usenet upload completed for piece {}", piece));
        scheduleUsenetEvictionScan();
        if (auto const digest = tr_sha1_from_string(info_hash_string); digest)
        {
            if (auto* const tor = torrents_.get(*digest); tor != nullptr)
            {
                if (queued_integrity_audit_ready)
                {
                    (void)queueUsenetIntegrityAudit(*tor, true);
                }
                (void)queueUsenetIntegrityAudit(*tor, false);
            }
        }
    }
    else
    {
        tr_logAddWarn(fmt::format("Usenet upload failed for piece {}: {}", piece, error));
    }
}
