// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "libtransmission/usenet-service.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#ifdef WITH_OPENSSL
#include <openssl/ssl.h>
#endif

#include <fmt/format.h>

#include "libtransmission/error.h"
#include "libtransmission/file.h"
#include "libtransmission/quark.h"
#include "libtransmission/subprocess.h"
#include "libtransmission/tr-strbuf.h"
#include "libtransmission/variant.h"

using namespace std::literals;

namespace
{
auto constexpr DefaultGroup = "alt.binaries.test"sv;

struct UsenetConfig
{
    std::string host;
    std::string port = "563";
    bool tls = true;
    std::string username;
    std::string password;
    std::string from = "nashawk@localhost";
    std::string group = std::string{ DefaultGroup };
};

struct TempPathGuard
{
    explicit TempPathGuard(std::string path_in)
        : path{ std::move(path_in) }
    {
    }

    TempPathGuard(TempPathGuard const&) = delete;
    TempPathGuard& operator=(TempPathGuard const&) = delete;

    ~TempPathGuard()
    {
        if (!std::empty(path))
        {
            tr_sys_path_remove(path);
        }
    }

    std::string path;
};

[[nodiscard]] std::string trim(std::string_view value)
{
    while (!std::empty(value) && std::isspace(static_cast<unsigned char>(value.front())) != 0)
    {
        value.remove_prefix(1);
    }

    while (!std::empty(value) && std::isspace(static_cast<unsigned char>(value.back())) != 0)
    {
        value.remove_suffix(1);
    }

    return std::string{ value };
}

[[nodiscard]] std::string unquote(std::string value)
{
    if (std::size(value) < 2U)
    {
        return value;
    }

    auto const quote = value.front();
    if ((quote != '"' && quote != '\'') || value.back() != quote)
    {
        return value;
    }

    auto out = std::string{};
    out.reserve(std::size(value) - 2U);

    for (size_t i = 1U; i + 1U < std::size(value); ++i)
    {
        if (quote == '"' && value[i] == '\\' && i + 2U < std::size(value))
        {
            ++i;
            switch (value[i])
            {
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            default:
                out += value[i];
                break;
            }
        }
        else
        {
            out += value[i];
        }
    }

    return out;
}

void read_dotenv_file(std::map<std::string, std::string>& values, std::string const& filename)
{
    auto file = std::ifstream{ filename };
    if (!file)
    {
        return;
    }

    auto line = std::string{};
    while (std::getline(file, line))
    {
        auto text = std::string_view{ line };
        if (!std::empty(text) && text.back() == '\r')
        {
            text.remove_suffix(1);
        }

        auto trimmed = trim(text);
        text = std::string_view{ trimmed };
        if (std::empty(text) || text.front() == '#')
        {
            continue;
        }

        if (text.starts_with("export "sv))
        {
            text.remove_prefix(std::size("export "sv));
        }

        auto const eq = text.find('=');
        if (eq == std::string_view::npos)
        {
            continue;
        }

        auto key = trim(text.substr(0, eq));
        auto value = trim(text.substr(eq + 1U));
        if (!std::empty(key))
        {
            values.insert_or_assign(std::move(key), unquote(std::move(value)));
        }
    }
}

[[nodiscard]] std::string config_path(std::string_view dir, std::string_view base)
{
    if (std::empty(dir))
    {
        return std::string{ base };
    }

    auto out = std::string{ dir };
    if (out.back() != '/')
    {
        out += '/';
    }

    out += base;
    return out;
}

[[nodiscard]] std::optional<bool> parse_bool(std::string_view value)
{
    auto lower = trim(value);
    std::transform(std::begin(lower), std::end(lower), std::begin(lower), [](unsigned char ch) { return std::tolower(ch); });

    if (lower == "1" || lower == "true" || lower == "yes" || lower == "on")
    {
        return true;
    }

    if (lower == "0" || lower == "false" || lower == "no" || lower == "off")
    {
        return false;
    }

    return {};
}

[[nodiscard]] std::optional<std::string> getenv_string(char const* name)
{
    if (auto const* value = std::getenv(name); value != nullptr && *value != '\0')
    {
        return value;
    }

    return {};
}

[[nodiscard]] std::map<std::string, std::string> read_usenet_env(std::string_view config_dir)
{
    auto values = std::map<std::string, std::string>{};
    read_dotenv_file(values, ".env");
    read_dotenv_file(values, config_path(config_dir, ".env"));

    for (auto const* key : { "USENET_HOST", "USENET_PORT", "USENET_TLS", "USENET_USERNAME", "USENET_PASSWORD", "USENET_FROM",
             "USENET_GROUP" })
    {
        if (auto value = getenv_string(key); value)
        {
            values.insert_or_assign(key, std::move(*value));
        }
    }

    return values;
}

[[nodiscard]] std::optional<UsenetConfig> load_usenet_config(std::string_view config_dir, std::string& error)
{
    auto const values = read_usenet_env(config_dir);

    auto get = [&values](std::string_view key) -> std::optional<std::string> {
        if (auto const it = values.find(std::string{ key }); it != std::end(values) && !std::empty(it->second))
        {
            return it->second;
        }

        return {};
    };

    auto config = UsenetConfig{};

    if (auto value = get("USENET_HOST"sv); value)
    {
        config.host = std::move(*value);
    }
    if (auto value = get("USENET_PORT"sv); value)
    {
        config.port = std::move(*value);
    }
    if (auto value = get("USENET_TLS"sv); value)
    {
        if (auto const parsed = parse_bool(*value); parsed)
        {
            config.tls = *parsed;
        }
        else
        {
            error = "Invalid USENET_TLS value";
            return {};
        }
    }
    if (auto value = get("USENET_USERNAME"sv); value)
    {
        config.username = std::move(*value);
    }
    if (auto value = get("USENET_PASSWORD"sv); value)
    {
        config.password = std::move(*value);
    }
    if (auto value = get("USENET_FROM"sv); value)
    {
        config.from = std::move(*value);
    }
    if (auto value = get("USENET_GROUP"sv); value)
    {
        config.group = std::move(*value);
    }

    auto missing = std::vector<std::string_view>{};
    if (std::empty(config.host))
    {
        missing.emplace_back("USENET_HOST"sv);
    }
    if (std::empty(config.username))
    {
        missing.emplace_back("USENET_USERNAME"sv);
    }
    if (std::empty(config.password))
    {
        missing.emplace_back("USENET_PASSWORD"sv);
    }

    if (!std::empty(missing))
    {
        error = "Missing Usenet configuration: ";
        for (size_t i = 0; i < std::size(missing); ++i)
        {
            if (i != 0U)
            {
                error += ", ";
            }
            error += missing[i];
        }
        return {};
    }

    return config;
}

[[nodiscard]] bool executable_exists(std::string const& path)
{
#ifdef _WIN32
    auto st = struct stat{};
    return stat(path.c_str(), &st) == 0 && (st.st_mode & S_IFREG) != 0;
#else
    return access(path.c_str(), X_OK) == 0;
#endif
}

[[nodiscard]] bool path_has_separator(std::string_view path)
{
    return path.find('/') != std::string_view::npos || path.find('\\') != std::string_view::npos;
}

[[nodiscard]] bool find_executable(std::string_view name)
{
    if (path_has_separator(name))
    {
        return executable_exists(std::string{ name });
    }

    auto const* path_env = std::getenv("PATH");
    if (path_env == nullptr)
    {
        return false;
    }

    auto path = std::string_view{ path_env };
    while (true)
    {
        auto const sep = path.find(':');
        auto const dir = sep == std::string_view::npos ? path : path.substr(0, sep);
        auto candidate = std::string{ std::empty(dir) ? "."sv : dir };
        if (!std::empty(candidate) && candidate.back() != '/')
        {
            candidate += '/';
        }
        candidate += name;
        if (executable_exists(candidate))
        {
            return true;
        }

        if (sep == std::string_view::npos)
        {
            break;
        }
        path.remove_prefix(sep + 1U);
    }

    return false;
}

[[nodiscard]] std::string yenc_encode(std::string_view name, std::string_view payload)
{
    auto out = std::string{};
    out.reserve(std::size(payload) + std::size(payload) / 64U + 128U);
    out += fmt::format("=ybegin line=128 size={} name={}\r\n", std::size(payload), name);

    auto column = size_t{ 0U };
    for (auto const raw : payload)
    {
        auto ch = static_cast<unsigned char>(raw);
        ch = static_cast<unsigned char>(ch + 42U);
        auto const escape = ch == 0U || ch == 9U || ch == 10U || ch == 13U || ch == 61U;
        if (escape)
        {
            if (column + 2U > 128U)
            {
                out += "\r\n";
                column = 0U;
            }
            out += '=';
            out += static_cast<char>(ch + 64U);
            column += 2U;
        }
        else
        {
            if (column + 1U > 128U)
            {
                out += "\r\n";
                column = 0U;
            }
            out += static_cast<char>(ch);
            ++column;
        }
    }

    out += fmt::format("\r\n=yend size={}\r\n", std::size(payload));
    return out;
}

[[nodiscard]] std::string json_escape(std::string_view text)
{
    auto out = std::string{};
    out.reserve(std::size(text) + 8U);

    for (auto const ch : text)
    {
        switch (ch)
        {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20U)
            {
                out += fmt::format("\\u{:04x}", static_cast<unsigned char>(ch));
            }
            else
            {
                out += ch;
            }
            break;
        }
    }

    return out;
}

[[nodiscard]] std::string make_nyuu_config(UsenetConfig const& config, tr_usenet_upload_request const& request)
{
    auto const subject = !std::empty(request.subject) ? request.subject : request.message_id;
    auto const yenc_name = !std::empty(request.yenc_name) ? request.yenc_name : request.message_id;

    auto json = std::string{};
    json += "{\n";
    json += fmt::format("\"host\":\"{}\",\n", json_escape(config.host));
    json += fmt::format("\"port\":\"{}\",\n", json_escape(config.port));
    json += fmt::format("\"ssl\":{},\n", config.tls ? "true" : "false");
    json += fmt::format("\"user\":\"{}\",\n", json_escape(config.username));
    json += fmt::format("\"password\":\"{}\",\n", json_escape(config.password));
    json += "\"connections\":1,\n";
    json += fmt::format("\"article-size\":\"{}\",\n", request.article_size);
    json += fmt::format("\"from\":\"{}\",\n", json_escape(config.from));
    json += fmt::format("\"groups\":\"{}\",\n", json_escape(config.group));
    json += fmt::format("\"subject\":\"{}\",\n", json_escape(subject));
    json += fmt::format("\"message-id\":\"{}\",\n", json_escape(request.message_id));
    json += "\"keep-message-id\":true,\n";
    json += fmt::format("\"yenc-name\":\"{}\",\n", json_escape(yenc_name));
    json += "\"check-connections\":1,\n";
    json += "\"check-tries\":2,\n";
    json += "\"check-delay\":\"5s\",\n";
    json += "\"check-post-tries\":0,\n";
    json += "\"out\":null,\n";
    json += "\"overwrite\":true,\n";
    json += "\"quiet\":true,\n";
    json += "\"progress\":\"none\"\n";
    json += "}\n";
    return json;
}

[[nodiscard]] std::optional<std::string> write_temp_file(
    std::string_view const config_dir,
    std::string_view const prefix,
    std::string_view const contents,
    std::string& filename)
{
    auto path = std::string{ tr_pathbuf{ std::empty(config_dir) ? "."sv : config_dir, '/', prefix, ".XXXXXX"sv }.sv() };
    auto error = tr_error{};
    auto fd = tr_sys_file_open_temp(std::data(path), &error);
    if (fd == TR_BAD_SYS_FILE)
    {
        return fmt::format("Could not create temporary Usenet file: {}", error.message());
    }

    auto bytes_written = uint64_t{};
    if (!tr_sys_file_write(fd, std::data(contents), std::size(contents), &bytes_written, &error) ||
        bytes_written != std::size(contents))
    {
        tr_sys_file_close(fd);
        tr_sys_path_remove(path);
        return fmt::format("Could not write temporary Usenet file: {}", error.message());
    }

    tr_sys_file_close(fd);
    filename = std::move(path);
    return {};
}

#ifndef _WIN32
class NntpConnection
{
public:
    NntpConnection() = default;
    NntpConnection(NntpConnection const&) = delete;
    NntpConnection& operator=(NntpConnection const&) = delete;

    ~NntpConnection()
    {
#ifdef WITH_OPENSSL
        if (ssl_ != nullptr)
        {
            SSL_shutdown(ssl_);
            SSL_free(ssl_);
        }

        if (ssl_ctx_ != nullptr)
        {
            SSL_CTX_free(ssl_ctx_);
        }
#endif

        if (socket_ >= 0)
        {
            close(socket_);
        }
    }

    [[nodiscard]] std::optional<std::string> connect_to(UsenetConfig const& config)
    {
        auto hints = addrinfo{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        auto* addresses = static_cast<addrinfo*>(nullptr);
        if (auto const rc = getaddrinfo(config.host.c_str(), config.port.c_str(), &hints, &addresses); rc != 0)
        {
            return fmt::format("Usenet DNS lookup failed: {}", gai_strerror(rc));
        }

        auto address_holder = std::unique_ptr<addrinfo, decltype(&freeaddrinfo)>{ addresses, freeaddrinfo };

        for (auto const* addr = addresses; addr != nullptr; addr = addr->ai_next)
        {
            socket_ = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);
            if (socket_ < 0)
            {
                continue;
            }

            if (::connect(socket_, addr->ai_addr, addr->ai_addrlen) == 0)
            {
                break;
            }

            close(socket_);
            socket_ = -1;
        }

        if (socket_ < 0)
        {
            return fmt::format("Usenet TCP connect failed: {}", std::strerror(errno));
        }

        if (config.tls)
        {
#ifdef WITH_OPENSSL
            ssl_ctx_ = SSL_CTX_new(TLS_client_method());
            if (ssl_ctx_ == nullptr)
            {
                return "Usenet TLS context creation failed";
            }

            SSL_CTX_set_default_verify_paths(ssl_ctx_);
            ssl_ = SSL_new(ssl_ctx_);
            if (ssl_ == nullptr)
            {
                return "Usenet TLS session creation failed";
            }

            SSL_set_tlsext_host_name(ssl_, config.host.c_str());
            SSL_set_fd(ssl_, socket_);
            if (SSL_connect(ssl_) != 1)
            {
                return "Usenet TLS handshake failed";
            }
#else
            return "Usenet TLS requires an OpenSSL build";
#endif
        }

        auto greeting = std::string{};
        if (auto error = read_line(greeting); error)
        {
            return error;
        }

        if (!greeting.starts_with("200 "sv) && !greeting.starts_with("201 "sv))
        {
            return "Usenet server rejected connection";
        }

        return {};
    }

    [[nodiscard]] std::optional<std::string> auth(UsenetConfig const& config)
    {
        if (auto error = command("AUTHINFO USER " + config.username, { 381, 281 }); error)
        {
            return error;
        }

        if (last_code_ == 381)
        {
            if (auto error = command("AUTHINFO PASS " + config.password, { 281 }); error)
            {
                return error;
            }
        }

        return {};
    }

    [[nodiscard]] std::optional<std::string> group(std::string const& name)
    {
        return command("GROUP " + name, { 211 });
    }

    [[nodiscard]] std::optional<std::string> post(std::string const& article)
    {
        if (auto error = command("POST", { 340 }); error)
        {
            return error;
        }

        if (auto error = write_all(dot_stuff(article) + ".\r\n"); error)
        {
            return error;
        }

        auto line = std::string{};
        if (auto error = read_line(line); error)
        {
            return error;
        }

        if (parse_code(line) != 240)
        {
            return "Usenet POST failed";
        }

        return {};
    }

    [[nodiscard]] std::optional<std::string> body(std::string const& message_id, std::string& out)
    {
        if (auto error = command("BODY <" + message_id + ">", { 222 }); error)
        {
            return error;
        }

        return read_multiline(out);
    }

private:
    [[nodiscard]] std::optional<std::string> command(std::string const& command, std::initializer_list<int> expected)
    {
        if (auto error = write_all(command + "\r\n"); error)
        {
            return error;
        }

        auto line = std::string{};
        if (auto error = read_line(line); error)
        {
            return error;
        }

        last_code_ = parse_code(line);
        if (std::find(std::begin(expected), std::end(expected), last_code_) == std::end(expected))
        {
            return fmt::format("Usenet command failed: {}", command.substr(0, command.find(' ')));
        }

        return {};
    }

    [[nodiscard]] static int parse_code(std::string_view line)
    {
        auto code = 0;
        auto const first = std::data(line);
        auto const last = first + std::min<size_t>(3U, std::size(line));
        auto const result = std::from_chars(first, last, code);
        return result.ec == std::errc{} ? code : 0;
    }

    [[nodiscard]] std::optional<std::string> write_all(std::string const& data)
    {
        auto sent = size_t{ 0U };
        while (sent < std::size(data))
        {
#ifdef WITH_OPENSSL
            auto const n = ssl_ != nullptr ? SSL_write(ssl_, std::data(data) + sent, static_cast<int>(std::size(data) - sent)) :
                                             static_cast<int>(send(socket_, std::data(data) + sent, std::size(data) - sent, 0));
#else
            auto const n = static_cast<int>(send(socket_, std::data(data) + sent, std::size(data) - sent, 0));
#endif
            if (n <= 0)
            {
                return "Usenet socket write failed";
            }
            sent += static_cast<size_t>(n);
        }

        return {};
    }

    [[nodiscard]] std::optional<std::string> read_byte(char& ch)
    {
#ifdef WITH_OPENSSL
        auto const n = ssl_ != nullptr ? SSL_read(ssl_, &ch, 1) : static_cast<int>(recv(socket_, &ch, 1, 0));
#else
        auto const n = static_cast<int>(recv(socket_, &ch, 1, 0));
#endif
        if (n <= 0)
        {
            return "Usenet socket read failed";
        }

        return {};
    }

    [[nodiscard]] std::optional<std::string> read_line(std::string& out)
    {
        out.clear();
        auto ch = char{};
        while (true)
        {
            if (auto error = read_byte(ch); error)
            {
                return error;
            }

            if (ch == '\n')
            {
                if (!std::empty(out) && out.back() == '\r')
                {
                    out.pop_back();
                }
                return {};
            }

            out += ch;
            if (std::size(out) > 8192U)
            {
                return "Usenet line too long";
            }
        }
    }

    [[nodiscard]] std::optional<std::string> read_multiline(std::string& out)
    {
        out.clear();
        auto line = std::string{};
        while (true)
        {
            if (auto error = read_line(line); error)
            {
                return error;
            }

            if (line == "."sv)
            {
                return {};
            }

            if (line.starts_with(".."sv))
            {
                line.erase(std::begin(line));
            }

            out += line;
            out += "\r\n";
        }
    }

    [[nodiscard]] static std::string dot_stuff(std::string_view text)
    {
        auto out = std::string{};
        out.reserve(std::size(text) + 16U);
        auto at_line_start = true;
        for (auto const ch : text)
        {
            if (at_line_start && ch == '.')
            {
                out += '.';
            }

            out += ch;
            at_line_start = ch == '\n';
        }

        return out;
    }

    int socket_ = -1;
    int last_code_ = 0;
#ifdef WITH_OPENSSL
    SSL_CTX* ssl_ctx_ = nullptr;
    SSL* ssl_ = nullptr;
#endif
};
#endif

[[nodiscard]] std::string make_payload(size_t const size)
{
    auto payload = std::string(size, '\0');
    for (size_t i = 0; i < size; ++i)
    {
        payload[i] = static_cast<char>((i * 31U + 17U) & 0xFFU);
    }

    return payload;
}

[[nodiscard]] std::string make_message_id(size_t const payload_size, std::string_view suffix)
{
    auto const now = std::chrono::steady_clock::now().time_since_epoch().count();
    return fmt::format("nashawk-startup-{}-{}-{}@nashawk.local", payload_size, now, suffix);
}

[[nodiscard]] std::string make_article(UsenetConfig const& config, std::string const& message_id, std::string const& payload)
{
    auto const name = fmt::format("{}.piece", message_id.substr(0, message_id.find('@')));
    auto article = std::string{};
    article += fmt::format("From: {}\r\n", config.from);
    article += fmt::format("Newsgroups: {}\r\n", config.group);
    article += fmt::format("Subject: Nashawk startup check {}\r\n", message_id);
    article += fmt::format("Message-ID: <{}>\r\n", message_id);
    article += "Date: Fri, 10 Jul 2026 00:00:00 +0000\r\n";
    article += "Content-Type: application/octet-stream\r\n";
    article += "\r\n";
    article += yenc_encode(name, payload);
    return article;
}

[[nodiscard]] std::optional<std::string> check_post_read(UsenetConfig const& config, size_t const payload_size, std::string_view suffix)
{
#ifdef _WIN32
    return "Usenet startup checks are not implemented on Windows yet";
#else
    auto connection = NntpConnection{};
    if (auto error = connection.connect_to(config); error)
    {
        return error;
    }
    if (auto error = connection.auth(config); error)
    {
        return error;
    }
    if (auto error = connection.group(config.group); error)
    {
        return error;
    }

    auto const payload = make_payload(payload_size);
    auto const message_id = make_message_id(payload_size, suffix);
    auto const article = make_article(config, message_id, payload);
    if (auto error = connection.post(article); error)
    {
        return error;
    }

    auto body = std::string{};
    if (auto error = connection.body(message_id, body); error)
    {
        return error;
    }

    auto const encoded = yenc_encode(fmt::format("{}.piece", message_id.substr(0, message_id.find('@'))), payload);
    if (body.find(encoded) == std::string::npos)
    {
        return "Usenet startup readback did not match uploaded yEnc body";
    }

    return {};
#endif
}

[[nodiscard]] bool settings_bool(tr_variant const& settings, tr_quark const key, bool const fallback)
{
    if (auto const* map = settings.get_if<tr_variant::Map>(); map != nullptr)
    {
        if (auto const value = map->value_if<bool>(key); value)
        {
            return *value;
        }
    }

    return fallback;
}

[[nodiscard]] size_t settings_size(tr_variant const& settings, tr_quark const key, size_t const fallback)
{
    if (auto const* map = settings.get_if<tr_variant::Map>(); map != nullptr)
    {
        if (auto const value = map->value_if<int64_t>(key); value && *value >= 0)
        {
            return static_cast<size_t>(*value);
        }
    }

    return fallback;
}
} // namespace

std::optional<std::string> tr_usenet_startup_check(std::string_view const config_dir, tr_variant const& settings)
{
    if (!settings_bool(settings, TR_KEY_usenet_enabled, false))
    {
        return {};
    }

    if (!find_executable("nyuu"sv))
    {
        return "Usenet mode requires nyuu in PATH";
    }

    auto error = std::string{};
    auto const config = load_usenet_config(config_dir, error);
    if (!config)
    {
        return error;
    }

    if (auto post_error = check_post_read(*config, 64U, "small"sv); post_error)
    {
        return post_error;
    }

    auto const check_size = settings_size(settings, TR_KEY_usenet_check_article_size, 1024U * 1024U);
    if (check_size > 64U)
    {
        if (auto post_error = check_post_read(*config, check_size, "configured"sv); post_error)
        {
            return post_error;
        }
    }

    return {};
}

std::optional<std::string> tr_usenet_upload_file(tr_usenet_upload_request const& request)
{
    if (std::empty(request.file_path))
    {
        return "Usenet upload requires a file path";
    }

    if (std::empty(request.message_id))
    {
        return "Usenet upload requires a message id";
    }

    if (request.article_size == 0U)
    {
        return "Usenet upload requires a positive article size";
    }

    auto error = std::string{};
    auto const config = load_usenet_config(request.config_dir, error);
    if (!config)
    {
        return error;
    }

    auto config_path = std::string{};
    if (auto write_error = write_temp_file(request.config_dir, "nyuu-config"sv, make_nyuu_config(*config, request), config_path);
        write_error)
    {
        return write_error;
    }

    auto config_guard = TempPathGuard{ config_path };

    auto args = std::vector<std::string>{
        "nyuu",
        "--config",
        config_path,
        std::string{ request.file_path },
    };

    auto c_args = std::vector<char const*>{};
    c_args.reserve(std::size(args) + 1U);
    for (auto const& arg : args)
    {
        c_args.push_back(arg.c_str());
    }
    c_args.push_back(nullptr);

    auto spawn_error = tr_error{};
    if (!tr_spawn_sync(std::data(c_args), {}, {}, &spawn_error))
    {
        return fmt::format("nyuu upload failed: {}", spawn_error.message());
    }

    return {};
}
