// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include <libtransmission/crypto-utils.h>
#include <libtransmission/usenet-piece-store.h>
#include <libtransmission/usenet-service.h>

using namespace std::literals;

namespace tr::test
{
namespace
{
auto const BaseMessageId = std::string(40U, 'a') + "@nashawk.local";

struct ChainFixture
{
    std::map<std::string, std::vector<uint8_t>> articles;
    std::vector<std::string> requested;

    [[nodiscard]] tr_usenet_decoded_article_fetch fetch()
    {
        return [this](std::string_view const message_id, std::vector<uint8_t>& setme) -> std::optional<std::string>
        {
            requested.emplace_back(message_id);
            auto const iter = articles.find(std::string{ message_id });
            if (iter == std::end(articles))
            {
                return "missing";
            }
            setme = iter->second;
            return {};
        };
    }
};
} // namespace

TEST(UsenetServiceTest, assemblesSingleAndMultipartChains)
{
    auto fixture = ChainFixture{};
    fixture.articles[BaseMessageId] = { 1U, 2U };
    fixture.articles[std::string(40U, 'a') + ".1@nashawk.local"] = { 3U, 4U };
    fixture.articles[std::string(40U, 'a') + ".2@nashawk.local"] = { 5U };

    auto result = tr_usenet_download_result{};
    auto const expected = std::vector<uint8_t>{ 1U, 2U, 3U, 4U, 5U };
    EXPECT_FALSE(
        tr_usenet_assemble_piece_chain(BaseMessageId, std::size(expected), tr_sha1::digest(expected), fixture.fetch(), result));
    EXPECT_EQ(expected, result.data);
    EXPECT_EQ(3U, result.article_count);
    EXPECT_EQ(
        (std::vector<std::string>{
            BaseMessageId,
            std::string(40U, 'a') + ".1@nashawk.local",
            std::string(40U, 'a') + ".2@nashawk.local",
        }),
        fixture.requested);

    fixture = {};
    fixture.articles[BaseMessageId] = expected;
    result = {};
    EXPECT_FALSE(
        tr_usenet_assemble_piece_chain(BaseMessageId, std::size(expected), tr_sha1::digest(expected), fixture.fetch(), result));
    EXPECT_EQ(1U, result.article_count);
    EXPECT_EQ((std::vector<std::string>{ BaseMessageId }), fixture.requested);
}

TEST(UsenetServiceTest, rejectsMissingEmptyOverflowAndHashMismatch)
{
    auto fixture = ChainFixture{};
    fixture.articles[BaseMessageId] = { 1U, 2U };
    auto result = tr_usenet_download_result{};

    auto error = tr_usenet_assemble_piece_chain(BaseMessageId, 3U, {}, fixture.fetch(), result);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("article 1"));
    EXPECT_TRUE(std::empty(result.data));

    fixture = {};
    fixture.articles[BaseMessageId] = {};
    error = tr_usenet_assemble_piece_chain(BaseMessageId, 1U, {}, fixture.fetch(), result);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("empty"));
    EXPECT_TRUE(std::empty(result.data));

    fixture = {};
    fixture.articles[BaseMessageId] = { 1U, 2U };
    error = tr_usenet_assemble_piece_chain(BaseMessageId, 1U, {}, fixture.fetch(), result);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("exceeds"));
    EXPECT_TRUE(std::empty(result.data));

    fixture = {};
    fixture.articles[BaseMessageId] = { 1U };
    error = tr_usenet_assemble_piece_chain(BaseMessageId, 1U, tr_sha1::digest("different"sv), fixture.fetch(), result);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("hash"));
    EXPECT_TRUE(std::empty(result.data));
}

TEST(UsenetServiceTest, rejectsMissingBaseAndCorruptMiddleArticle)
{
    auto fixture = ChainFixture{};
    auto result = tr_usenet_download_result{};

    auto error = tr_usenet_assemble_piece_chain(BaseMessageId, 1U, {}, fixture.fetch(), result);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("article 0"));
    EXPECT_TRUE(std::empty(result.data));
    EXPECT_EQ((std::vector<std::string>{ BaseMessageId }), fixture.requested);

    auto const expected = std::vector<uint8_t>{ 1U, 2U, 3U, 4U, 5U, 6U };
    fixture = {};
    fixture.articles[BaseMessageId] = { 1U, 2U };
    fixture.articles[std::string(40U, 'a') + ".1@nashawk.local"] = { 9U, 9U };
    fixture.articles[std::string(40U, 'a') + ".2@nashawk.local"] = { 5U, 6U };

    error = tr_usenet_assemble_piece_chain(
        BaseMessageId,
        std::size(expected),
        tr_sha1::digest(expected),
        fixture.fetch(),
        result);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("hash"));
    EXPECT_TRUE(std::empty(result.data));
    EXPECT_EQ(0U, result.article_count);
}

TEST(UsenetServiceTest, enforcesArticleSafetyBound)
{
    auto fixture = ChainFixture{};
    auto const fetch = [&fixture](std::string_view const message_id, std::vector<uint8_t>& setme) -> std::optional<std::string>
    {
        fixture.requested.emplace_back(message_id);
        setme = { 1U };
        return {};
    };
    auto result = tr_usenet_download_result{};
    auto const error = tr_usenet_assemble_piece_chain(BaseMessageId, TrUsenetMaxArticlesPerPiece + 1U, {}, fetch, result);
    ASSERT_TRUE(error);
    EXPECT_NE(std::string::npos, error->find("safety limit"));
    EXPECT_EQ(TrUsenetMaxArticlesPerPiece, std::size(fixture.requested));
    EXPECT_TRUE(std::empty(result.data));
}

TEST(UsenetServiceTest, parsesOnlyUniqueDuplicateMessageIdErrors)
{
    auto const first = std::string(40U, 'a') + "@nashawk.local";
    auto const continuation = std::string(40U, 'b') + ".1@nashawk.local";
    auto const diagnostics = fmt::format(
        "[ERR ] NNTPError: Server could not accept post {}, returned: 441 Posting Failed. Message-ID is not unique E1\n"
        "[WARN] unrelated 441 Message-ID is not unique\n"
        "[ERR ] NNTPError: Server could not accept post <{}>, returned: 441 Posting Failed. Message-ID is not unique E1\n"
        "[ERR ] NNTPError: Server could not accept post {}, returned: 441 Posting Failed. Message-ID is not unique E1\n"
        "[ERR ] NNTPError: Server could not accept post bad id, returned: 441 Posting Failed. Message-ID is not unique E1\n"
        "[ERR ] NNTPError: Server could not accept post ignored@nashawk.local, returned: 430 No Such Article\n",
        first,
        continuation,
        first);

    EXPECT_EQ((std::vector<std::string>{ first, continuation }), tr_usenet_parse_duplicate_message_ids(diagnostics));
}

TEST(UsenetServiceTest, missingArticleScanRejectsInvalidRequestsBeforeConnecting)
{
    auto result = tr_usenet_missing_piece_articles({ .config_dir = {}, .base_message_id = {}, .article_count = 1U });
    ASSERT_TRUE(std::holds_alternative<std::string>(result));

    result = tr_usenet_missing_piece_articles(
        {
            .config_dir = {},
            .base_message_id = "0123456789abcdef0123456789abcdef01234567@nashawk.local",
            .article_count = 0U,
        });
    ASSERT_TRUE(std::holds_alternative<std::string>(result));

    result = tr_usenet_missing_piece_articles(
        {
            .config_dir = {},
            .base_message_id = "0123456789abcdef0123456789abcdef01234567@nashawk.local",
            .article_count = TrUsenetMaxArticlesPerPiece + 1U,
        });
    ASSERT_TRUE(std::holds_alternative<std::string>(result));
}

} // namespace tr::test
