#include "QRCodeForStream.h"

#include <string>
#include <string_view>

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
                                           const std::string_view mid)
{
    this->uid = uid;
    this->stoken = stoken;
    this->mid = mid;
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
                qrScanners.decodeSingle(img, str);

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
                const std::string passportQrUrl = PandaScanQRCode(scanUrl, ticket, gameType);
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
                qrScanners.decodeSingle(img, str);
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
                if (ret = scanCheck(ticket); ret == ScanRet::SUCCESS)
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
    if (pAVCodecContext->width < pAVCodecContext->height ||
        pAVCodecContext->height == 480 ||
        pAVCodecContext->height == 720)
    {
        videoStreamWidth = pAVCodecContext->width;
        videoStreamHeight = pAVCodecContext->height;
    }
    else
    {
        videoStreamWidth = pAVCodecContext->width / 1.5;
        videoStreamHeight = pAVCodecContext->height / 1.5;
    }
}

void QRCodeForStream::stop()
{
    m_stop.store(false);
}

void QRCodeForStream::setUrl(const std::string& url, const std::map<std::string, std::string> heard)
{
    streamUrl = url;
    for (const auto& it : heard)
    {
        av_dict_set(&pAvdictionary, it.first.c_str(), it.second.c_str(), 0);
    }
    av_dict_set(&pAvdictionary, "max_delay", "0", 0);
    av_dict_set(&pAvdictionary, "probesize", "1024", 0);
    av_dict_set(&pAvdictionary, "packetsize", "128", 0);
    av_dict_set(&pAvdictionary, "rtbufsize", "0", 0);
    av_dict_set(&pAvdictionary, "delay", "0", 0);
    av_dict_set(&pAvdictionary, "buffer_size", "1000", 0);
    av_dict_set(&pAvdictionary, "fflags", "nobuffer", 0);
    av_dict_set(&pAvdictionary, "flags", "low_delay", 0);
    av_dict_set(&pAvdictionary, "avioflags", "direct", 0);
    av_dict_set(&pAvdictionary, "analyzeduration", "0", 0);
}

auto QRCodeForStream::init() -> bool
{
    pAVFormatContext = avformat_alloc_context();
    if (pAVFormatContext == nullptr)
    {
        std::cerr << "Error allocating format context" << std::endl;
        return false;
    }
    pAVFormatContext->flags |= AVFMT_FLAG_NOBUFFER;
    if (avformat_open_input(&pAVFormatContext, streamUrl.c_str(), NULL, &pAvdictionary) != 0)
    {
        std::cerr << "Error opening input file" << std::endl;
        return false;
    }
    if (avformat_find_stream_info(pAVFormatContext, NULL) < 0)
    {
        std::cerr << "Error finding stream information" << std::endl;
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
        std::cerr << "No video stream found" << std::endl;
        return false;
    }
    videoStreamIndex = videoStream->index;
    const AVCodec* decoder{ avcodec_find_decoder(videoStream->codecpar->codec_id) };
    if (decoder == nullptr)
    {
        std::cerr << "Codec not found" << std::endl;
        return false;
    }
    pAVCodecContext = avcodec_alloc_context3(decoder);
    if (pAVCodecContext == nullptr)
    {
        std::cerr << "Error allocating codec context" << std::endl;
        return false;
    }
    avcodec_parameters_to_context(pAVCodecContext, videoStream->codecpar);
    pAVCodecContext->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(pAVCodecContext, decoder, NULL) < 0)
    {
        std::cerr << "Error opening codec" << std::endl;
        return false;
    }
    setStreamHW();
    pSwsContext = sws_getContext(
        pAVCodecContext->width, pAVCodecContext->height, pAVCodecContext->pix_fmt,
        videoStreamWidth, videoStreamHeight, AV_PIX_FMT_BGR24, SWS_BILINEAR, NULL, NULL, NULL);
    pAVPacket = av_packet_alloc();
    pAVFrame = av_frame_alloc();
    return true;
}

void QRCodeForStream::continueLastLogin()
{
    switch (servertype)
    {
        using enum ServerType;
    case Official:
    {
        const bool b = ScanPassportQRLogin(lastQrCode, stoken, mid, uid) &&
                       ConfirmPassportQRLogin(lastQrCode, stoken, mid, uid);
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
    m_stop.store(true);
    ret = ScanRet::UNKNOW;
    lastTicket.clear();
    lastQrCode.clear();
    lastAttemptTicket.clear();
    lastAttemptAt = {};
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
