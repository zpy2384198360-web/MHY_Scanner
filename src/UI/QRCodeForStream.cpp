#include "QRCodeForStream.h"

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "QRScanner.h"
#include "MhyApi.hpp"

QRCodeForStream::QRCodeForStream(QObject* parent) :
    QThread(parent),
    pAvdictionary(nullptr),
    pAVFormatContext(nullptr),
    pSwsContext(nullptr),
    pAVFrame(nullptr),
    pAVPacket(nullptr),
    pAVCodecContext(nullptr),
    m_stop(false),
    servertype(ServerType::Official)

{
    av_log_set_level(AV_LOG_FATAL);
    m_gameQrSession = std::make_unique<cpr::Session>();
}

QRCodeForStream::~QRCodeForStream()
{
    if (!this->isInterruptionRequested())
    {
        m_stop.store(false);
    }
    this->requestInterruption();
    this->wait();
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken)
{
    this->uid = uid;
    this->gameToken = gameToken;
    this->stoken.clear();
    this->mid.clear();
}

void QRCodeForStream::setPassportLoginInfo(const std::string_view uid, const std::string_view stoken,
                                           const std::string_view mid, const std::string_view gameToken)
{
    this->uid = uid;
    this->stoken = stoken;
    this->mid = mid;
    this->gameToken = gameToken;
}

void QRCodeForStream::setLoginInfo(const std::string_view uid, const std::string_view gameToken, const std::string& name)
{
    this->uid = uid;
    this->gameToken = gameToken;
    this->m_name = name;
    this->stoken.clear();
    this->mid.clear();
}

void QRCodeForStream::setServerType(const ServerType servertype)
{
    this->servertype = servertype;
}

void QRCodeForStream::setAutoLogin(const bool enabled)
{
    m_autoLogin.store(enabled);
}

void QRCodeForStream::setContinuousScan(const bool enabled)
{
    m_continuousScan.store(enabled);
}

bool QRCodeForStream::isContinuousScan() const
{
    return m_continuousScan.load();
}

std::string QRCodeForStream::lastInitError()
{
    const std::scoped_lock lock(mtx);
    return m_lastInitError;
}

std::string QRCodeForStream::lastLatencySummary()
{
    const std::scoped_lock lock(timingMtx);
    return m_lastLatencySummary;
}

void QRCodeForStream::recordClaimTiming(const long long decodeMs, const long long claimMs)
{
    const std::scoped_lock lock(timingMtx);
    m_attemptDecodeMs = decodeMs;
    m_attemptClaimMs = claimMs;
    m_lastLatencySummary = "识别 " + std::to_string(decodeMs) + "ms，抢码接口 " +
                           std::to_string(claimMs) + "ms";
}

void QRCodeForStream::finishTiming(const long long confirmMs, const std::string& mode)
{
    const std::scoped_lock lock(timingMtx);
    const long long totalMs = m_attemptDecodeMs + m_attemptClaimMs + confirmMs;
    m_lastLatencySummary = "识别 " + std::to_string(m_attemptDecodeMs) + "ms，抢码接口 " +
                           std::to_string(m_attemptClaimMs) + "ms，确认 " +
                           std::to_string(confirmMs) + "ms，总计 " + std::to_string(totalMs) +
                           "ms（" + mode + "）";
}

void QRCodeForStream::setInitError(const std::string& stage, const int errorCode)
{
    std::string detail = stage;
    if (errorCode < 0)
    {
        std::array<char, AV_ERROR_MAX_STRING_SIZE> errorText{};
        if (av_strerror(errorCode, errorText.data(), errorText.size()) == 0)
        {
            detail += "（" + std::string(errorText.data()) + "）";
        }
        detail += "，错误码 " + std::to_string(errorCode);
    }
    const std::scoped_lock lock(mtx);
    m_lastInitError = std::move(detail);
}

void QRCodeForStream::LoginOfficial()
{
    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            av_packet_unref(pAVPacket);
            continue;
        }
        avcodec_send_packet(pAVCodecContext, pAVPacket);
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            break;
        }
        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            cv::Mat img(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img.data };
            const int dstLinesize[1] = { static_cast<int>(img.step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                const auto decodeStarted = std::chrono::steady_clock::now();
                qrScanners.decodeSingle(img, str);
                const auto decodedAt = std::chrono::steady_clock::now();

                std::unique_lock lock(mtx, std::try_to_lock);
                if (!lock.owns_lock() || !m_stop.load())
                    return;

                std::string ticket;
                if (!parseOfficialQRCode(str, ticket))
                    return;

                if (lastTicket == ticket)
                    return;

                const auto now = std::chrono::steady_clock::now();
                if (lastAttemptTicket == ticket && now - lastAttemptAt < std::chrono::seconds(1))
                    return;
                lastAttemptTicket = ticket;
                lastAttemptAt = now;

                const bool continuousScan = m_continuousScan.load();
                const auto claimStarted = std::chrono::steady_clock::now();
                const std::string passportQrUrl =
                    PandaScanQRCode(*m_gameQrSession, scanUrl, ticket, gameType);
                const auto claimFinished = std::chrono::steady_clock::now();
                recordClaimTiming(
                    std::chrono::duration_cast<std::chrono::milliseconds>(decodedAt - decodeStarted).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(claimFinished - claimStarted).count());
                if (!passportQrUrl.empty())
                {
                    lastTicket = ticket;
                    lastQrCode = passportQrUrl;
                    if (m_autoLogin.load() || continuousScan)
                    {
                        continueLastLogin();
                    }
                    else
                    {
                        Q_EMIT loginConfirm(gameType, false);
                    }
                }
                else
                {
                    Q_EMIT loginResults(ScanRet::FAILURE_1);
                }
                if (!continuousScan)
                    stop();
            });
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::LoginBH3BiliBili()
{
    while (m_stop.load())
    {
        if (av_read_frame(pAVFormatContext, pAVPacket) < 0)
        {
            ret = ScanRet::LIVESTOP;
            break;
        }
        if (pAVPacket->stream_index != videoStreamIndex)
        {
            av_packet_unref(pAVPacket);
            continue;
        }
        avcodec_send_packet(pAVCodecContext, pAVPacket);
        if (pAVFrame == nullptr)
        {
            std::cerr << "Error allocating frame" << std::endl;
            ret = ScanRet::LIVESTOP;
            break;
        }

        while (avcodec_receive_frame(pAVCodecContext, pAVFrame) == 0)
        {
            cv::Mat img(videoStreamHeight, videoStreamWidth, CV_8UC3);
            uint8_t* dstData[1] = { img.data };
            const int dstLinesize[1] = { static_cast<int>(img.step) };
            sws_scale(pSwsContext, pAVFrame->data, pAVFrame->linesize, 0, pAVFrame->height,
                      dstData, dstLinesize);
#ifndef SHOW
            cv::imshow("Video_Stream", img);
            cv::waitKey(1);
#endif
            threadPool.tryStart([&, img = std::move(img)]() {
                thread_local QRScanner qrScanners;
                std::string str;
                const auto decodeStarted = std::chrono::steady_clock::now();
                qrScanners.decodeSingle(img, str);
                const auto decodedAt = std::chrono::steady_clock::now();
                if (str.size() < 85)
                {
                    return;
                }
                if (std::string_view view(str.c_str() + 79, 3); view != "8F3")
                {
                    return;
                }
                const std::string ticket = str.substr(str.length() - 24);
                std::unique_lock lock(mtx, std::try_to_lock);
                if (!lock.owns_lock() || !m_stop.load())
                    return;

                if (lastTicket == ticket)
                    return;

                const auto now = std::chrono::steady_clock::now();
                if (lastAttemptTicket == ticket && now - lastAttemptAt < std::chrono::seconds(1))
                    return;
                lastAttemptTicket = ticket;
                lastAttemptAt = now;

                const bool continuousScan = m_continuousScan.load();
                const auto claimStarted = std::chrono::steady_clock::now();
                ret = scanCheck(ticket);
                const auto claimFinished = std::chrono::steady_clock::now();
                recordClaimTiming(
                    std::chrono::duration_cast<std::chrono::milliseconds>(decodedAt - decodeStarted).count(),
                    std::chrono::duration_cast<std::chrono::milliseconds>(claimFinished - claimStarted).count());
                if (ret == ScanRet::SUCCESS)
                {
                    lastTicket = ticket;
                    if (m_autoLogin.load() || continuousScan)
                    {
                        continueLastLogin();
                    }
                    else
                    {
                        Q_EMIT loginConfirm(GameType::Honkai3_BiliBili, false);
                    }
                }
                else
                {
                    Q_EMIT loginResults(ret);
                }
                if (!continuousScan)
                    stop();
            });
        }
        av_frame_unref(pAVFrame);
        av_packet_unref(pAVPacket);
    }
}

void QRCodeForStream::setStreamHW()
{
    const int sourceWidth = pAVCodecContext->width;
    const int sourceHeight = pAVCodecContext->height;
    if (sourceWidth <= 0 || sourceHeight <= 0)
    {
        videoStreamWidth = 1;
        videoStreamHeight = 1;
        return;
    }
    const bool portrait = sourceHeight > sourceWidth;
    const double widthScale = static_cast<double>(portrait ? 720 : 1280) / sourceWidth;
    const double heightScale = static_cast<double>(portrait ? 1280 : 720) / sourceHeight;
    const double scale = std::min({ 1.0, widthScale, heightScale });
    videoStreamWidth = std::max(1, static_cast<int>(sourceWidth * scale));
    videoStreamHeight = std::max(1, static_cast<int>(sourceHeight * scale));
}

void QRCodeForStream::stop()
{
    m_stop.store(false);
}

void QRCodeForStream::setUrl(const std::string& url, const std::map<std::string, std::string> heard)
{
    streamUrl = url;
    streamHeaders = heard;
}

auto QRCodeForStream::init() -> bool
{
    // Some CDNs do not include codec metadata in the first few packets. Try a
    // low-latency profile first, then reopen with a larger probe window.
    bool streamReady = false;
    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (pAVFormatContext != nullptr)
        {
            avformat_close_input(&pAVFormatContext);
        }
        av_dict_free(&pAvdictionary);

        for (const auto& [key, value] : streamHeaders)
        {
            av_dict_set(&pAvdictionary, key.c_str(), value.c_str(), 0);
        }
        av_dict_set(&pAvdictionary, "rw_timeout", "8000000", 0);
        av_dict_set(&pAvdictionary, "reconnect", "1", 0);
        av_dict_set(&pAvdictionary, "reconnect_streamed", "1", 0);
        av_dict_set(&pAvdictionary, "reconnect_delay_max", "2", 0);

        if (attempt == 0)
        {
            av_dict_set(&pAvdictionary, "max_delay", "0", 0);
            av_dict_set(&pAvdictionary, "probesize", "32768", 0);
            av_dict_set(&pAvdictionary, "analyzeduration", "500000", 0);
            av_dict_set(&pAvdictionary, "buffer_size", "65536", 0);
            av_dict_set(&pAvdictionary, "fflags", "nobuffer", 0);
            av_dict_set(&pAvdictionary, "flags", "low_delay", 0);
        }
        else
        {
            av_dict_set(&pAvdictionary, "probesize", "1048576", 0);
            av_dict_set(&pAvdictionary, "analyzeduration", "2000000", 0);
            av_dict_set(&pAvdictionary, "buffer_size", "262144", 0);
        }

        pAVFormatContext = avformat_alloc_context();
        if (pAVFormatContext == nullptr)
        {
            setInitError("无法分配直播流解析器");
            return false;
        }
        if (attempt == 0)
        {
            pAVFormatContext->flags |= AVFMT_FLAG_NOBUFFER;
        }

        const int openResult =
            avformat_open_input(&pAVFormatContext, streamUrl.c_str(), nullptr, &pAvdictionary);
        if (openResult < 0)
        {
            setInitError(attempt == 0 ? "无法连接直播流，正在尝试兼容模式" : "无法连接直播流",
                         openResult);
            continue;
        }

        const int infoResult = avformat_find_stream_info(pAVFormatContext, nullptr);
        if (infoResult < 0)
        {
            setInitError(attempt == 0 ? "无法解析直播流，正在尝试兼容模式" : "无法解析直播流",
                         infoResult);
            continue;
        }
        streamReady = true;
        break;
    }

    if (!streamReady)
    {
        return false;
    }
    if (pAVFormatContext == nullptr || pAVFormatContext->nb_streams == 0)
    {
        if (lastInitError().empty())
        {
            setInitError("直播流中没有可解析的数据");
        }
        return false;
    }

    AVStream* videoStream = nullptr;
    for (int i = 0; i < pAVFormatContext->nb_streams; i++)
    {
        if (pAVFormatContext->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
        {
            videoStream = pAVFormatContext->streams[i];
            break;
        }
    }
    if (videoStream == nullptr)
    {
        setInitError("直播地址已连接，但没有检测到视频画面");
        return false;
    }
    videoStreamIndex = videoStream->index;
    const AVCodec* decoder{ avcodec_find_decoder(videoStream->codecpar->codec_id) };
    if (decoder == nullptr)
    {
        setInitError("当前版本不支持该直播视频编码");
        return false;
    }
    pAVCodecContext = avcodec_alloc_context3(decoder);
    if (pAVCodecContext == nullptr)
    {
        setInitError("无法分配视频解码器");
        return false;
    }
    const int parametersResult = avcodec_parameters_to_context(pAVCodecContext, videoStream->codecpar);
    if (parametersResult < 0)
    {
        setInitError("无法读取视频编码参数", parametersResult);
        return false;
    }
    pAVCodecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
    pAVCodecContext->flags2 |= AV_CODEC_FLAG2_FAST;
    if ((decoder->capabilities & AV_CODEC_CAP_SLICE_THREADS) != 0)
    {
        pAVCodecContext->thread_type = FF_THREAD_SLICE;
    }
    else
    {
        // Frame threading buffers complete frames and increases live latency.
        pAVCodecContext->thread_count = 1;
    }
    const int decoderResult = avcodec_open2(pAVCodecContext, decoder, nullptr);
    if (decoderResult < 0)
    {
        setInitError("无法启动视频解码器", decoderResult);
        return false;
    }
    setStreamHW();
    pSwsContext = sws_getContext(
        pAVCodecContext->width, pAVCodecContext->height, pAVCodecContext->pix_fmt,
        videoStreamWidth, videoStreamHeight, AV_PIX_FMT_BGR24, SWS_BILINEAR, nullptr, nullptr, nullptr);
    pAVPacket = av_packet_alloc();
    pAVFrame = av_frame_alloc();
    if (pSwsContext == nullptr || pAVPacket == nullptr || pAVFrame == nullptr)
    {
        setInitError("无法分配视频画面转换缓冲区");
        return false;
    }
    return true;
}

void QRCodeForStream::continueLastLogin()
{
    const auto confirmStarted = std::chrono::steady_clock::now();
    switch (servertype)
    {
        using enum ServerType;
    case Official:
    {
        bool b = false;
        std::string mode = "兼容确认";
        if (!gameToken.empty())
        {
            b = ConfirmQRLogin(*m_gameQrSession, confirmUrl, uid, gameToken, lastTicket, gameType);
            mode = b ? "快速确认" : "快速失败后兼容确认";
        }
        if (!b)
        {
            b = ScanPassportQRLogin(lastQrCode, stoken, mid, uid) &&
                ConfirmPassportQRLogin(lastQrCode, stoken, mid, uid);
        }
        const auto confirmFinished = std::chrono::steady_clock::now();
        finishTiming(
            std::chrono::duration_cast<std::chrono::milliseconds>(confirmFinished - confirmStarted).count(),
            mode);
        if (b)
        {
            Q_EMIT loginResults(ScanRet::SUCCESS);
        }
        else
        {
            Q_EMIT loginResults(ScanRet::FAILURE_2);
        }
    }
    break;
    case BH3_BiliBili:
    {
        ret = scanConfirm(lastTicket, uid, gameToken, m_name);
        const auto confirmFinished = std::chrono::steady_clock::now();
        finishTiming(
            std::chrono::duration_cast<std::chrono::milliseconds>(confirmFinished - confirmStarted).count(),
            "B服确认");
        Q_EMIT loginResults(ret);
    }
    break;
    default:
        break;
    }
}

void QRCodeForStream::run()
{
    threadPool.setMaxThreadCount(threadNumber);
    threadPool.setThreadPriority(QThread::HighPriority);
    m_stop.store(true);
    ret = ScanRet::UNKNOW;
    lastTicket.clear();
    lastQrCode.clear();
    lastAttemptTicket.clear();
    lastAttemptAt = {};
    {
        const std::scoped_lock lock(mtx);
        m_lastInitError.clear();
    }
    {
        const std::scoped_lock lock(timingMtx);
        m_lastLatencySummary.clear();
        m_attemptDecodeMs = 0;
        m_attemptClaimMs = 0;
    }
    //TODO 获取直播流地址放在这里
    if (init())
    {
#ifndef SHOW
        cv::namedWindow("Video_Stream", cv::WINDOW_AUTOSIZE);
        cv::resizeWindow("Video_Stream", videoStreamWidth / 2, videoStreamHeight / 2);
#endif
        switch (servertype)
        {
            using enum ServerType;
        case Official:
            LoginOfficial();
            break;
        case BH3_BiliBili:
            LoginBH3BiliBili();
            break;
        default:
            break;
        }
    }
    else
    {
        ret = ScanRet::STREAMERROR;
    }
    threadPool.waitForDone();
    if (ret == ScanRet::LIVESTOP || ret == ScanRet::STREAMERROR)
    {
        emit loginResults(ret);
    }
#ifndef SHOW
    cv::destroyWindow("Video_Stream");
#endif
    avformat_close_input(&pAVFormatContext);
    avcodec_free_context(&pAVCodecContext);
    sws_freeContext(pSwsContext);
    av_dict_free(&pAvdictionary);
    av_frame_free(&pAVFrame);
    av_packet_free(&pAVPacket);
    pAVFormatContext = nullptr;
    pAVCodecContext = nullptr;
    pSwsContext = nullptr;
    pAvdictionary = nullptr;
    pAVFrame = nullptr;
    pAVPacket = nullptr;
}
