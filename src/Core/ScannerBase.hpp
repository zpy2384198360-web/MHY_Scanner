#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>

#include "ApiDefs.hpp"
#include "UrlQuery.hpp"

class ScannerBase
{
public:
    GameType gameType{ GameType::UNKNOW };
    std::string_view scanUrl{};
    std::string_view confirmUrl{};
    std::string lastTicket;
    std::string lastQrCode;
    std::string uid;
    std::string gameToken{};
    std::map<std::string_view, std::function<void()>> setGameTypeByBizKey{
        { "bh3_cn", [this]() {
             gameType = GameType::Honkai3;
             scanUrl = api::mhy::bh3::qrcode_scan;
             confirmUrl = api::mhy::bh3::qrcode_confirm;
         } },
        { "hk4e_cn", [this]() {
             gameType = GameType::Genshin;
             scanUrl = api::mhy::hk4e::qrcode_scan;
             confirmUrl = api::mhy::hk4e::qrcode_confirm;
         } },
        { "hkrpg_cn", [this]() {
             gameType = GameType::HonkaiStarRail;
             scanUrl = api::mhy::hkrpg::qrcode_scan;
             confirmUrl = api::mhy::hkrpg::qrcode_confirm;
         } },
        { "nap_cn", [this]() {
             gameType = GameType::ZenlessZoneZero;
             scanUrl = api::mhy::nap::qrcode_scan;
             confirmUrl = api::mhy::nap::qrcode_confirm;
         } },
    };

    [[nodiscard]] bool setGameTypeByAppId(const std::string_view appId)
    {
        if (appId == "1")
            setGameTypeByBizKey["bh3_cn"]();
        else if (appId == "4" || appId == "7")
            setGameTypeByBizKey["hk4e_cn"]();
        else if (appId == "8")
            setGameTypeByBizKey["hkrpg_cn"]();
        else if (appId == "12")
            setGameTypeByBizKey["nap_cn"]();
        else
            return false;
        return true;
    }

    [[nodiscard]] bool parseOfficialQRCode(const std::string_view qrCode, std::string& ticket)
    {
        ticket = GetUrlQueryParam(qrCode, "ticket");
        if (ticket.empty())
            ticket = GetUrlQueryParam(qrCode, "tk");
        if (ticket.empty())
            return false;

        std::string bizKey = GetUrlQueryParam(qrCode, "biz_key");
        if (bizKey.empty())
            bizKey = GetUrlQueryParam(qrCode, "game_biz");
        if (auto it = setGameTypeByBizKey.find(bizKey); it != setGameTypeByBizKey.end())
        {
            it->second();
            return true;
        }
        return setGameTypeByAppId(GetUrlQueryParam(qrCode, "app_id"));
    }
};
