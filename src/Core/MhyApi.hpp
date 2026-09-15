#pragma once

#include <string>
#include <string_view>
#include <format>
#include <random>
#include <sstream>
#include <optional>
#include <iostream>
#include <tuple>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>

#include "ApiDefs.hpp"
#include "CreateUUID.hpp"
#include "CryptoKit.h"
#include "UtilString.hpp"
#include "TimeStamp.hpp"
#include "UrlQuery.hpp"

static const std::string device_id{ CreateUUID::CreateUUID4() };
static const std::string device_fp{ Md5(device_id).substr(0, 13) };
static std::string loginQrcodeTicket{};

[[nodiscard]] inline std::string DataSignAlgorithmVersionGen1()
{
    return "";
}

[[nodiscard]] inline std::string DataSignAlgorithmVersionGen2(const std::string_view body, const std::string_view query)
{
    const std::string time_now{ std::to_string(GetUnixTimeStampSeconds()) };
    std::random_device rd{};
    std::mt19937 gen{ rd() };
    int lower_bound{ 100001 };
    int upper_bound{ 200000 };
    std::uniform_int_distribution<int> dist(lower_bound, upper_bound);
    const std::string rand{ std::to_string(dist(gen)) };
    std::string m{ "salt=" + std::string(mihoyobbs_salt_x6) + "&t=" + time_now + "&r=" + rand + "&b=" + std::string(body) + "&q=" + std::string(query) };
    return time_now + "," + rand + "," + Md5(m);
}

inline std::string Encrypt(const std::string_view source)
{
    static constinit const char* PublicKey{
        "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDDvekdPMHN3AYhm/vktJT+YJr7"
        "cI5DcsNKqdsx5DZX0gDuWFuIjzdwButrIYPNmRJ1G8ybDIF7oDW2eEpm5sMbL9zs"
        "9ExXCdvqrn51qELbqj0XxtMTIpaCHFSI50PfPpTFV9Xt/hmyVwokoOXFlAEgCn+Q"
        "CgGs52bFoYMtyi+xEQIDAQAB\n"
        "-----END PUBLIC KEY-----"
    };
    return rsaEncrypt(source.data(), PublicKey);
}

inline cpr::Header GetRequestHeader()
{
    static cpr::Header headers{
        { "User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) miHoYoBBS/2.76.1" },
        { "Accept", "application/json" },
        { "x-rpc-app_id", "bll8iq97cem8" },
        { "x-rpc-app_version", "2.76.1" },
        { "x-rpc-client_type", "2" },
        { "x-rpc-device_id", device_id },
        { "x-rpc-device_name", "" },
        { "x-rpc-game_biz", "bbs_cn" },
        { "x-rpc-sdk_version", "2.16.0" }
    };
    return headers;
}

inline cpr::Header GetPassportQRCodeHeader(const std::string_view appId = "dw9y09jqjpxc")
{
    return cpr::Header{
        { "Content-Type", "application/json" },
        { "x-rpc-app_id", std::string(appId) },
        { "x-rpc-device_id", device_id },
        { "x-rpc-device_fp", device_fp }
    };
}

inline std::string GetLoginQrcodeUrl()
{
    loginQrcodeTicket.clear();
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::create_qr_login },
        cpr::Body{ "" },
        GetPassportQRCodeHeader());

    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
        return {};

    try
    {
        const auto data = nlohmann::json::parse(response.text);
        if (data.value("retcode", -1) != 0 || !data.contains("data"))
            return {};

        loginQrcodeTicket = data["data"].value("ticket", std::string{});
        return data["data"].value("url", std::string{});
    }
    catch (const nlohmann::json::exception&)
    {
        loginQrcodeTicket.clear();
        return {};
    }
}

inline const std::string& GetLoginQrcodeTicket()
{
    return loginQrcodeTicket;
}

inline std::tuple<LoginQRCodeState, std::string, std::string, std::string> GetQRCodeState(
    const std::string_view ticket)
{
    if (ticket.empty())
        return { LoginQRCodeState::Expired, {}, {}, {} };

    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::query_qr_login_status },
        cpr::Body{ nlohmann::json{ { "ticket", std::string(ticket) } }.dump() },
        GetPassportQRCodeHeader());

    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
        return { LoginQRCodeState::Expired, {}, {}, {} };

    try
    {
        const auto data = nlohmann::json::parse(response.text);
        if (data.value("retcode", -1) != 0 || !data.contains("data"))
            return { LoginQRCodeState::Expired, {}, {}, {} };

        const std::string status = data["data"].value("status", std::string{});
        if (status == "Created" || status == "Init")
            return { LoginQRCodeState::Init, {}, {}, {} };
        if (status == "Scanned")
            return { LoginQRCodeState::Scanned, {}, {}, {} };
        if (status != "Confirmed")
            return { LoginQRCodeState::Expired, {}, {}, {} };

        const auto& payload = data["data"];
        if (!payload.contains("user_info") || !payload.contains("tokens") ||
            !payload["tokens"].is_array() || payload["tokens"].empty())
            return { LoginQRCodeState::Expired, {}, {}, {} };

        const nlohmann::json* token = &payload["tokens"][0];
        for (const auto& candidate : payload["tokens"])
        {
            const std::string name = candidate.value("name", std::string{});
            if (name == "stoken" || name == "stoken_v2")
            {
                token = &candidate;
                break;
            }
        }
        return { LoginQRCodeState::Confirmed,
                 payload["user_info"].value("aid", std::string{}),
                 payload["user_info"].value("mid", std::string{}),
                 token->value("token", std::string{}) };
    }
    catch (const nlohmann::json::exception&)
    {
        return { LoginQRCodeState::Expired, {}, {}, {} };
    }
}

inline std::string getMysUserName(const std::string_view uid)
{
    static constexpr std::string_view url = api::mhy::mys::userinfo;
    const auto response = cpr::Get(
        cpr::Url{ std::format("{}?uid={}", url, uid) });

    const auto data = nlohmann::json::parse(response.text);
    return data["data"]["user_info"]["nickname"].get<std::string>();
}

inline std::tuple<int, std::string, std::string> GetStokenByGameToken(
    const std::string_view uid,
    const std::string_view game_token)
{
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::takumi::game_token_stoken },
        cpr::Body{ nlohmann::json{ { "account_id", std::stoi(uid.data()) }, { "game_token", game_token } }.dump() },
        GetRequestHeader());

    const auto j = nlohmann::json::parse(response.text);
    const int retcode = j.value("retcode", -1);

    if (retcode != 0)
        return { retcode, {}, {} };

    return { 0,
             j["data"]["user_info"]["mid"].get<std::string>(),
             j["data"]["token"]["token"].get<std::string>() };
}

inline std::tuple<int, std::string> GetGameTokenByStoken(
    const std::string_view stoken,
    const std::string_view mid)
{
    if (stoken.empty() || mid.empty())
        return { -1, {} };

    const auto requestByCookie = [stoken, mid](const std::string_view tokenName) -> std::tuple<int, std::string> {
        const auto response = cpr::Get(
            cpr::Url{ api::mhy::takumi::game_token },
            cpr::Header{
                { "Accept", "application/json" },
                { "Cookie", std::string(tokenName) + "=" + std::string(stoken) + "; mid=" + std::string(mid) }
            });

        if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
            return { -1, {} };

        try
        {
            const auto j = nlohmann::json::parse(response.text);
            const int retcode = j.value("retcode", -1);
            if (retcode != 0 || !j.contains("data"))
                return { retcode, {} };
            return { 0, j["data"].value("game_token", std::string{}) };
        }
        catch (const nlohmann::json::exception&)
        {
            return { -1, {} };
        }
    };

    if (stoken.starts_with("v2_"))
    {
        auto result = requestByCookie("stoken_v2");
        if (std::get<0>(result) == 0 && !std::get<1>(result).empty())
            return result;
    }

    {
        auto result = requestByCookie("stoken");
        if (std::get<0>(result) == 0 && !std::get<1>(result).empty())
            return result;
    }

    // Older accounts may still require the legacy query-parameter form.
    const auto response = cpr::Get(
        cpr::Url{ api::mhy::takumi::game_token },
        cpr::Parameters{
            { "stoken", std::string(stoken) },
            { "mid", std::string(mid) } });

    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
        return { -1, {} };

    try
    {
        const auto j = nlohmann::json::parse(response.text);
        const int retcode = j.value("retcode", -1);
        if (retcode != 0 || !j.contains("data"))
            return { retcode, {} };

        const std::string gameToken = j["data"].value("game_token", std::string{});
        if (gameToken.empty())
            return { -1, {} };
        return { 0, gameToken };
    }
    catch (const nlohmann::json::exception&)
    {
        return { -1, {} };
    }
}

inline std::tuple<int, GeetestData> CreateLoginCaptcha(
    const std::string_view mobile,
    const std::string_view aigis = "")
{
    const std::string body{ nlohmann::json{
        { "area_code", Encrypt("+86") },
        { "mobile", Encrypt(mobile) } }
                                .dump() };
    cpr::Header reqHeaders{ GetRequestHeader() };
    reqHeaders["DS"] = DataSignAlgorithmVersionGen2(body, "");
    if (!aigis.empty())
        reqHeaders["X-Rpc-Aigis"] = aigis;
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::create_captcha },
        cpr::Body{ body },
        cpr::Header{ reqHeaders });

    GeetestData result{};
    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
        return { -1, result };

    nlohmann::json j;
    try
    {
        j = nlohmann::json::parse(response.text);
    }
    catch (const nlohmann::json::exception&)
    {
        return { -1, result };
    }

    const int retcode = j.value("retcode", -1);
    if (retcode == 0)
    {
        result.action_type = j["data"]["action_type"].get<std::string>();
        return { retcode, result };
    }
    if (retcode == -3101)
    {
        const auto it = response.header.find("X-Rpc-Aigis");
        if (it != response.header.end())
        {
            const auto aigisJson = nlohmann::json::parse(it->second);
            const auto captchaJson = nlohmann::json::parse(aigisJson["data"].get<std::string>());

            result.session_id = aigisJson["session_id"].get<std::string>();
            result.mmt_type = aigisJson["mmt_type"].get<int>();
            result.gt = captchaJson["gt"].get<std::string>();
            result.challenge = captchaJson["challenge"].get<std::string>();
            result.GeeTestType = ServerType::Official;
        }
    }
    return { retcode, result };
}

inline auto LoginByMobileCaptcha(const std::string_view actionType, const std::string_view mobile, const std::string_view captcha, const std::string_view aigis = "")
{
    struct
    {
        int retcode{};
        struct
        {
            std::string V2Token{};
            std::string aid{};
            std::string mid{};
        } data;
    } result;
    const std::string requestBody{ nlohmann::json{
        { "area_code", Encrypt("+86") },
        { "action_type", std::string(actionType) },
        { "captcha", std::string(captcha) },
        { "mobile", Encrypt(mobile) } }
                                           .dump() };
    cpr::Header headers{ GetRequestHeader() };
    headers["DS"] = DataSignAlgorithmVersionGen2(requestBody, "");
    if (!aigis.empty())
        headers["X-Rpc-Aigis"] = aigis;

    const auto response = cpr::Post(
        cpr::Url{ api::mhy::passport::login_by_mobile_captcha },
        cpr::Body{ requestBody },
        cpr::Header{ headers });

    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
    {
        result.retcode = -1;
        return result;
    }

    try
    {
        const auto j = nlohmann::json::parse(response.text);
        result.retcode = j.value("retcode", -1);
        if (result.retcode == 0)
        {
            result.data.V2Token = j["data"]["token"].value("token", std::string{});
            result.data.aid = j["data"]["user_info"].value("aid", std::string{});
            result.data.mid = j["data"]["user_info"].value("mid", std::string{});
        }
    }
    catch (const nlohmann::json::exception&)
    {
        result.retcode = -1;
    }
    return result;
}

inline std::string PandaScanQRCode(const std::string_view url, const std::string_view ticket, GameType gameType)
{
    if (url.empty() || ticket.empty())
        return {};

    const auto response = cpr::Post(
        cpr::Url{ url },
        cpr::Body{ nlohmann::json{
            { "passport_app_id", "bll8iq97cem8" },
            { "app_id", static_cast<int>(gameType) },
            { "device", device_id },
            { "ticket", std::string(ticket) },
            { "ts", GetUnixTimeStampSeconds() } }
                       .dump() },
        GetPassportQRCodeHeader("bll8iq97cem8"));

    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
        return {};

    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    if (j.is_discarded() || j.value("retcode", -1) != 0 || !j.contains("data"))
        return {};
    return j["data"].value("passport_qr_url", std::string{});
}

inline bool PassportQRCodeLogin(
    const std::string_view qrCode,
    const std::string_view stoken,
    const std::string_view mid,
    const std::string_view uid,
    const bool confirm)
{
    const std::string ticket = GetUrlQueryParam(qrCode, "tk");
    const std::string tokenType = GetUrlQueryParam(qrCode, "token_types");
    if (ticket.empty() || tokenType.empty() || stoken.empty() || mid.empty() || uid.empty())
        return false;

    auto headers = GetPassportQRCodeHeader("bll8iq97cem8");
    headers["Cookie"] = "stoken=" + std::string(stoken) +
                        "; stuid=" + std::string(uid) +
                        "; mid=" + std::string(mid);
    const std::string endpoint = confirm
                                     ? static_cast<std::string>(api::mhy::passport::confirm_qr_login)
                                     : static_cast<std::string>(api::mhy::passport::scan_qr_login);
    const auto response = cpr::Post(
        cpr::Url{ endpoint },
        cpr::Body{ nlohmann::json{
            { "ticket", ticket },
            { "token_types", nlohmann::json::array({ tokenType }) } }
                       .dump() },
        headers);

    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
        return false;

    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    return !j.is_discarded() && j.value("retcode", -1) == 0;
}

inline bool ScanPassportQRLogin(
    const std::string_view qrCode,
    const std::string_view stoken,
    const std::string_view mid,
    const std::string_view uid)
{
    return PassportQRCodeLogin(qrCode, stoken, mid, uid, false);
}

inline bool ConfirmPassportQRLogin(
    const std::string_view qrCode,
    const std::string_view stoken,
    const std::string_view mid,
    const std::string_view uid)
{
    return PassportQRCodeLogin(qrCode, stoken, mid, uid, true);
}

// Legacy helpers retained for BBS/game integrations that still use game_token.
inline bool ScanQRLogin(const std::string_view url, const std::string_view ticket, GameType gameType)
{
    return !PandaScanQRCode(url, ticket, gameType).empty();
}

inline bool ConfirmQRLogin(const std::string_view url, const std::string_view uid, const std::string_view gameToken, const std::string_view ticket, GameType gameType)
{
    if (url.empty() || uid.empty() || gameToken.empty() || ticket.empty())
        return false;

    const auto response = cpr::Post(
        cpr::Url{ url },
        cpr::Body{ nlohmann::json{
            { "app_id", static_cast<int>(gameType) },
            { "device", device_id },
            { "ticket", ticket },
            { "payload", { { "proto", "Account" }, { "raw", nlohmann::json{ { "uid", uid }, { "token", gameToken } }.dump() } } } }
                       .dump() },
        cpr::Header{ { "Content-Type", "application/json" } });

    if (response.error || response.status_code < 200 || response.status_code >= 300 || response.text.empty())
        return false;

    const auto j = nlohmann::json::parse(response.text, nullptr, false);
    return !j.is_discarded() && j.value("retcode", -1) == 0;
}

inline std::string makeSign(const nlohmann::json& data)
{
    std::string param;
    for (auto& [key, value] : data.items())
    {
        if (key == "sign")
            continue;
        const std::string strVal = value.is_string() ? value.get<std::string>() : value.dump();

        param += key + "=" + strVal + "&";
    }
    if (!param.empty())
        param.pop_back();
#ifdef _DEBUG
    std::cout << "make_param = " << param << std::endl;
#endif
    constexpr std::string_view key = "0ebc517adb1b62c6b408df153331f9aa";
    return HmacSha256(param, std::string(key));
}

inline std::string& getOAString()
{
    static std::string value = []() {
        const auto response = cpr::Get(cpr::Url{ "https://api.v6qbb.cloud/get_bh3_bilibili_oa" });
        if (response.text.empty())
            throw std::runtime_error("");
        return response.text;
    }();
    return value;
}

inline std::tuple<int, std::string, std::string, std::string> GetBH3ExternalLoginInfo(const std::string& uid, const std::string& access_key)
{
    const std::string bodyData = std::format(R"({{"access_key":"{}","uid":{}}})", access_key, uid);

    nlohmann::json body{
        { "device", "0000000000000000" },
        { "app_id", 1 },
        { "channel_id", 14 },
        { "data", bodyData }
    };
    body["sign"] = makeSign(body);
    const auto response = cpr::Post(
        cpr::Url{ api::mhy::bh3::v2_login },
        cpr::Header{ { "Content-Type", "application/json" } },
        cpr::Body{ body.dump() });

    const auto j = nlohmann::json::parse(response.text);
    const int retcode = j.value("retcode", -1);

#ifdef _DEBUG
    std::cout << "崩坏3验证完成 : " << response.text << std::endl;
#endif

    if (retcode != 0)
    {
        return { retcode, {}, {}, {} };
    }

    return { 0,
             j["data"]["open_id"].get<std::string>(),
             j["data"]["combo_token"].get<std::string>(),
             j["data"]["combo_id"].get<std::string>() };
}

inline ScanRet scanCheck(const std::string& ticket)
{
    const std::string body = nlohmann::json{
        { "app_id", "1" },
        { "device", "0000000000000000" },
        { "ticket", ticket },
        { "ts", GetUnixTimeStampSeconds() }
    }.dump();

    const auto response = cpr::Post(
        cpr::Url{ api::mhy::bh3::qrcode_scan },
        cpr::Body{ body },
        cpr::Header{ { "Content-Type", "application/json" } });

    const auto j = nlohmann::json::parse(response.text);
    return j.value("retcode", -1) == 0 ? ScanRet::SUCCESS : ScanRet::FAILURE_1;
}

inline ScanRet scanConfirm(const std::string& ticket, const std::string& uid, const std::string& access_key, const std::string& name)
{
    auto [code, open_id, combo_token, combo_id] = GetBH3ExternalLoginInfo(uid, access_key);
    if (code != 0)
        return ScanRet::FAILURE_2;

    const auto raw =
        nlohmann::json{
            { "heartbeat", false },
            { "open_id", open_id },
            { "device_id", "0000000000000000" },
            { "app_id", "1" },
            { "channel_id", "14" },
            { "combo_token", combo_token },
            { "asterisk_name", name },
            { "combo_id", combo_id },
            { "account_type", "2" }
        };

    const auto ext =
        nlohmann::json{
            { "data", nlohmann::json{
                          { "accountType", "2" },
                          { "accountID", "" },
                          { "c", open_id },
                          { "accountToken", combo_token },
                          { "dispatch", getOAString() } } }
        };

    const nlohmann::json postBody{
        { "device", "0000000000000000" },
        { "app_id", 1 },
        { "ts", GetUnixTimeStampSeconds() },
        { "ticket", ticket },
        { "payload", nlohmann::json{
                         { "proto", "Combo" },
                         { "raw", raw.dump() },
                         { "ext", ext.dump() } } }
    };

#ifdef _DEBUG
    std::cout << postBody.dump() << std::endl;
#endif

    const auto response = cpr::Post(
        cpr::Url{ api::mhy::bh3::qrcode_confirm },
        cpr::Header{ { "Content-Type", "application/json" } },
        cpr::Body{ postBody.dump() });

    const auto j = nlohmann::json::parse(response.text);
    return j.value("retcode", -1) == 0 ? ScanRet::SUCCESS : ScanRet::FAILURE_2;
}
