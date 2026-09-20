#include "LiveStreamLink.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <future>
#include <limits>
#include <string_view>
#include <utility>

#include <cpr/cpr.h>
#include <nlohmann/json.hpp>

namespace
{
constexpr std::string_view BrowserUserAgent =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/139.0.0.0 Safari/537.36";

void AppendUnique(std::vector<std::string>& links, const std::string& link)
{
    if (!link.empty() && std::find(links.begin(), links.end(), link) == links.end())
        links.push_back(link);
}

void AppendFlvRoute(const nlohmann::json& route, std::vector<std::string>& links)
{
    if (route.is_array())
    {
        for (const auto& item : route)
            AppendFlvRoute(item, links);
        return;
    }
    if (route.is_object() && route.contains("flv") && route["flv"].is_string())
        AppendUnique(links, route["flv"].get<std::string>());
}

bool AppendQualityRoutes(
    const nlohmann::json& qualities,
    const std::string_view quality,
    std::vector<std::string>& links)
{
    const std::string qualityKey{ quality };
    if (!qualities.contains(qualityKey) || !qualities[qualityKey].is_object())
        return false;

    const std::size_t before = links.size();
    const auto& qualityData = qualities[qualityKey];
    if (qualityData.contains("main"))
        AppendFlvRoute(qualityData["main"], links);
    if (qualityData.contains("backup"))
        AppendFlvRoute(qualityData["backup"], links);
    return links.size() > before;
}

void AppendStreamDataLinks(const nlohmann::json& streamData, std::vector<std::string>& links)
{
    if (!streamData.contains("data") || !streamData["data"].is_object())
        return;

    const auto& qualities = streamData["data"];
    if (AppendQualityRoutes(qualities, "origin", links))
        return;
    for (const std::string_view quality : { "uhd", "hd", "sd", "ld" })
    {
        if (AppendQualityRoutes(qualities, quality, links))
            return;
    }
}

void AppendEncodedStreamData(const nlohmann::json& node, std::vector<std::string>& links)
{
    if (!node.is_string())
        return;
    const auto streamData = nlohmann::json::parse(node.get<std::string>(), nullptr, false);
    if (!streamData.is_discarded())
        AppendStreamDataLinks(streamData, links);
}
}

LiveBili::LiveBili(const std::string& roomID) :
    roomID(roomID)
{
}

LiveStreamInfo LiveBili::GetLiveStreamInfo()
{
    const auto response = cpr::Get(
        cpr::Url{ std::format("{}?id={}", api::live::bili::room_init.c_str(), roomID) },
        cpr::ConnectTimeout{ 1500 },
        cpr::Timeout{ 3000 });
    if (response.error || response.status_code != 200 || response.text.empty())
        return { LiveStreamStatus::Error, "" };

    try
    {
        const auto roomInfo = nlohmann::json::parse(response.text, nullptr, false);
        if (roomInfo.is_discarded())
            return { LiveStreamStatus::Error, "" };

        const int code = roomInfo.value("code", -1);
        if (code == 60004)
            return { LiveStreamStatus::Absent, "" };
        if (code != 0 || !roomInfo.contains("data"))
            return { LiveStreamStatus::Error, "" };

        const auto& data = roomInfo["data"];
        if (data.value("live_status", 0) != 1)
            return { LiveStreamStatus::NotLive, "" };

        realRoomID = data.contains("room_id") ? std::to_string(data["room_id"].get<int>()) : roomID;
        auto links = GetLinkByRealRoomID(realRoomID);
        if (links.empty())
            return { LiveStreamStatus::Error, "" };
        return { LiveStreamStatus::Normal, links.front(), std::move(links) };
    }
    catch (const nlohmann::json::exception&)
    {
        return { LiveStreamStatus::Error, "" };
    }
}

std::vector<std::string> LiveBili::GetLinkByRealRoomID(const std::string& realRoomID)
{
    const cpr::Parameters params = {
        { "codec", "0" },
        { "format", "0,2" },
        { "only_audio", "0" },
        { "only_video", "0" },
        { "protocol", "0,1" },
        { "qn", "10000" },
        { "room_id", realRoomID },
    };
    return GetStreamUrls(params);
}

std::vector<std::string> LiveBili::GetStreamUrls(const cpr::Parameters param)
{
    const auto response = cpr::Get(
        cpr::Url{ api::live::bili::v2_play_info },
        param,
        cpr::ConnectTimeout{ 1500 },
        cpr::Timeout{ 3000 });
    if (response.error || response.status_code != 200 || response.text.empty())
        return {};

    try
    {
        const auto playInfo = nlohmann::json::parse(response.text, nullptr, false);
        if (playInfo.is_discarded())
            return {};

        const auto& streams = playInfo.at("data").at("playurl_info").at("playurl").at("stream");
        std::vector<std::string> links;
        for (const auto& stream : streams)
        {
            if (!stream.contains("format") || !stream["format"].is_array())
                continue;
            for (const auto& format : stream["format"])
            {
                const std::string formatName = format.value("format_name", std::string{});
                if (!formatName.empty() && formatName != "flv")
                    continue;
                if (!format.contains("codec") || !format["codec"].is_array())
                    continue;
                for (const auto& codec : format["codec"])
                {
                    const std::string baseUrl = codec.value("base_url", std::string{});
                    if (baseUrl.empty() || !codec.contains("url_info") || !codec["url_info"].is_array())
                        continue;
                    for (const auto& urlInfo : codec["url_info"])
                    {
                        AppendUnique(
                            links,
                            urlInfo.value("host", std::string{}) + baseUrl +
                                urlInfo.value("extra", std::string{}));
                        if (links.size() >= 8)
                            return links;
                    }
                }
            }
        }
        return links;
    }
    catch (const nlohmann::json::exception&)
    {
        return {};
    }
}

LiveDouyin::LiveDouyin(const std::string& roomID) :
    m_roomID(roomID)
{
}

LiveStreamInfo LiveDouyin::GetLiveStreamInfo()
{
    try
    {
        const std::string roomPage = "https://live.douyin.com/" + m_roomID;
        const cpr::Header headers = {
            { "User-Agent", std::string(BrowserUserAgent) },
            { "Referer", roomPage },
            { "Accept", "application/json, text/plain, */*" },
        };

        cpr::Session session;
        session.SetHeader(headers);
        session.SetConnectTimeout(cpr::ConnectTimeout{ 2000 });
        session.SetTimeout(cpr::Timeout{ 5000 });
        session.SetRedirect(cpr::Redirect{ true });

        session.SetUrl(cpr::Url{ roomPage });
        const auto landing = session.Get();
        cpr::Cookies cookies = landing.cookies;
        cookies.emplace_back(cpr::Cookie{ "enter_pc_once", "1", ".douyin.com", true, "/", true });
        session.SetCookies(cookies);

        const std::string params =
            "aid=6383&app_name=douyin_web&live_id=1&device_platform=web&"
            "browser_language=zh-CN&browser_platform=Win32&browser_name=Edge&"
            "browser_version=139.0.0.0&is_need_double_stream=false&web_rid=" +
            m_roomID;
        session.SetUrl(cpr::Url{ std::string(api::live::douyin::room) + params });
        const auto response = session.Get();
        if (response.error || response.status_code != 200 || response.text.empty())
            return { LiveStreamStatus::Error, "" };

        const auto streamInfo = nlohmann::json::parse(response.text, nullptr, false);
        if (streamInfo.is_discarded() || streamInfo.value("status_code", -1) != 0)
            return { LiveStreamStatus::Absent, "" };

        const auto& roomData = streamInfo.at("data").at("data");
        if (!roomData.is_array() || roomData.empty())
            return { LiveStreamStatus::Absent, "" };
        const auto& data = roomData[0];
        const int status = data.value("status", 0);
        if (status == 4)
            return { LiveStreamStatus::NotLive, "" };
        if (status != 2)
            return { LiveStreamStatus::Error, "" };

        auto links = GetStreamLinksFromResponse(data);
        if (links.empty())
            return { LiveStreamStatus::Error, "" };
        return { LiveStreamStatus::Normal, links.front(), std::move(links) };
    }
    catch (const nlohmann::json::exception&)
    {
        return { LiveStreamStatus::Error, "" };
    }
}

std::vector<std::string> LiveDouyin::GetStreamLinksFromResponse(const nlohmann::json& data)
{
    std::vector<std::string> links;
    try
    {
        const auto& streamUrl = data.at("stream_url");
        if (streamUrl.contains("pull_datas") && streamUrl["pull_datas"].is_object())
        {
            for (const auto& [_, pullData] : streamUrl["pull_datas"].items())
            {
                if (pullData.contains("stream_data"))
                    AppendEncodedStreamData(pullData["stream_data"], links);
            }
        }
        if (streamUrl.contains("live_core_sdk_data"))
        {
            const auto& sdkData = streamUrl["live_core_sdk_data"];
            if (sdkData.contains("pull_data") && sdkData["pull_data"].contains("stream_data"))
                AppendEncodedStreamData(sdkData["pull_data"]["stream_data"], links);
        }
        if (links.empty() && streamUrl.contains("flv_pull_url") && streamUrl["flv_pull_url"].is_object())
        {
            const auto& pullUrls = streamUrl["flv_pull_url"];
            for (const std::string_view quality : { "FULL_HD1", "HD1", "SD1", "SD2" })
            {
                const std::string qualityKey{ quality };
                if (pullUrls.contains(qualityKey) && pullUrls[qualityKey].is_string())
                {
                    AppendUnique(links, pullUrls[qualityKey].get<std::string>());
                    break;
                }
            }
        }
    }
    catch (const nlohmann::json::exception&)
    {
        return {};
    }
    return links;
}

std::string SelectLowestLatencyStream(
    const std::vector<std::string>& links,
    const std::string& userAgent,
    const std::string& referer)
{
    if (links.empty())
        return {};
    if (links.size() == 1)
        return links.front();

    struct ProbeResult
    {
        std::string link;
        long long elapsedMs{};
        bool usable{};
    };

    const std::size_t probeCount = std::min<std::size_t>(links.size(), 6);
    std::vector<std::future<ProbeResult>> probes;
    probes.reserve(probeCount);
    for (std::size_t i = 0; i < probeCount; ++i)
    {
        probes.emplace_back(std::async(std::launch::async, [&, i]() {
            cpr::Session session;
            session.SetUrl(cpr::Url{ links[i] });
            session.SetHeader(cpr::Header{
                { "User-Agent", userAgent },
                { "Referer", referer },
            });
            session.SetConnectTimeout(cpr::ConnectTimeout{ 1000 });
            session.SetTimeout(cpr::Timeout{ 1800 });
            session.SetRedirect(cpr::Redirect{ true });
            const auto started = std::chrono::steady_clock::now();
            const auto response = session.Head();
            const auto finished = std::chrono::steady_clock::now();
            const bool usable = !response.error &&
                                ((response.status_code >= 200 && response.status_code < 400) ||
                                 response.status_code == 405);
            return ProbeResult{
                links[i],
                std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count(),
                usable,
            };
        }));
    }

    ProbeResult fastest{ links.front(), (std::numeric_limits<long long>::max)(), false };
    for (auto& probe : probes)
    {
        const ProbeResult result = probe.get();
        if (result.usable && (!fastest.usable || result.elapsedMs < fastest.elapsedMs))
            fastest = result;
    }
    return fastest.usable ? fastest.link : links.front();
}

LiveStreamInfo GetLiveInfo(const LivePlatform platform, const std::string& roomID)
{
    switch (platform)
    {
    case LivePlatform::Douyin:
        return GetLiveInfo<LiveDouyin>(roomID);
    case LivePlatform::BiliBili:
        return GetLiveInfo<LiveBili>(roomID);
    default:
        return LiveStreamInfo{ LiveStreamStatus::Error };
    }
}
