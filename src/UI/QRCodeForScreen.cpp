#include "QRCodeForScreen.h"

#include <QFuture>
#include <QtConcurrent/QtConcurrent>
#include <QThreadPool>

#include "QRScanner.h"
#include "ScreenScan.h"
#include "ScreenShotDXGI.hpp"

QRCodeForScreen::QRCodeForScreen(QObject* parent) :
    QThread(parent),
    m_stop(false)
{
    m_config = &ConfigDate::getInstance();
}

QRCodeForScreen::~QRCodeForScreen()
{
    if (!this->isInterruptionRequested())
    {
        m_stop.store(false);
    }
    this->requestInterruption();
    this->wait();
}

void QRCodeForScreen::setLoginInfo(const std::string& uid, const std::string& token)
{
    this->uid = uid;
    this->gameToken = token;
    this->stoken.clear();
    this->mid.clear();
}

void QRCodeForScreen::setLoginInfo(const std::string& uid, const std::string& token, const std::string& name)
{
    this->uid = uid;
    this->gameToken = token;
    this->m_name = name;
    this->stoken.clear();
    this->mid.clear();
}

void QRCodeForScreen::setPassportLoginInfo(const std::string& uid, const std::string& stoken, const std::string& mid)
{
    this->uid = uid;
    this->stoken = stoken;
    this->mid = mid;
}

void QRCodeForScreen::LoginOfficial()
{
    QThreadPool threadPool;
    threadPool.setMaxThreadCount(threadNumber);
    std::mutex mtx;
    ScreenShotDXGI screenshotdxgi;
    int w{ 0 };
    int h{ 0 };
    if (!screenshotdxgi.InitDevice() || !screenshotdxgi.InitDupl(0, w, h))
        return;
    long mBufferSize = w * h * 4;
    uint8_t* mBuffer = new UCHAR[mBufferSize];
    while (m_stop.load())
    {
        const int frameResult = screenshotdxgi.getFrame(50);
        if (frameResult == 2)
            continue;
        if (frameResult != 0)
            break;
        if (!screenshotdxgi.copyFrameToBuffer(&mBuffer, mBufferSize))
        {
            screenshotdxgi.doneWithFrame();
            continue;
        }
        cv::Mat img;
        cv::resize(cv::Mat(h, w, CV_8UC4, mBuffer), img, { 1280, 720 });
        screenshotdxgi.doneWithFrame();
#ifndef SHOW
        cv::imshow("Video_Stream", img);
        cv::waitKey(1);
#endif
        threadPool.tryStart([&, img = std::move(img)]() {
            thread_local QRScanner qrScanners;
            std::string str;
            qrScanners.decodeSingle(img, str);
            std::string ticket;
            if (!parseOfficialQRCode(str, ticket))
                return;

            if (lastTicket == ticket)
            {
                return;
            }
            if (mtx.try_lock())
            {
                if (!m_stop.load())
                {
                    mtx.unlock();
                    return;
                }
                const std::string passportQrUrl = PandaScanQRCode(scanUrl, ticket, gameType);
                if (!passportQrUrl.empty())
                {
                    lastTicket = ticket;
                    lastQrCode = passportQrUrl;
                    nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                    if (config["auto_login"])
                    {
                        continueLastLogin();
                    }
                    else
                    {
                        emit loginConfirm(gameType, true);
                    }
                }
                else
                {
                    emit loginResults(ScanRet::FAILURE_1);
                }
                stop();
                mtx.unlock();
            }
        });
    }
    threadPool.waitForDone();
    delete[] mBuffer;
}

void QRCodeForScreen::LoginBH3BiliBili()
{
    QThreadPool threadPool;
    threadPool.setMaxThreadCount(threadNumber);
    std::mutex mtx;
    ScreenShotDXGI screenshotdxgi;
    int w{ 0 };
    int h{ 0 };
    if (!screenshotdxgi.InitDevice() || !screenshotdxgi.InitDupl(0, w, h))
        return;
    long mBufferSize = w * h * 4;
    uint8_t* mBuffer = new UCHAR[mBufferSize];
    while (m_stop.load())
    {
        const int frameResult = screenshotdxgi.getFrame(50);
        if (frameResult == 2)
            continue;
        if (frameResult != 0)
            break;
        if (!screenshotdxgi.copyFrameToBuffer(&mBuffer, mBufferSize))
        {
            screenshotdxgi.doneWithFrame();
            continue;
        }
        cv::Mat img;
        cv::resize(cv::Mat(h, w, CV_8UC4, mBuffer), img, { 1280, 720 });
        screenshotdxgi.doneWithFrame();
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
            const std::string& ticket = str.substr(str.length() - 24);
            if (lastTicket == ticket)
            {
                return;
            }
            if (mtx.try_lock())
            {
                if (!m_stop.load())
                {
                    mtx.unlock();
                    return;
                }
                if (ret = scanCheck(ticket); ret == ScanRet::SUCCESS)
                {
                    lastTicket = ticket;
                    nlohmann::json config = nlohmann::json::parse(m_config->getConfig());
                    if (config["auto_login"])
                    {
                        continueLastLogin();
                    }
                    else
                    {
                        emit loginConfirm(GameType::Honkai3_BiliBili, true);
                    }
                }
                else
                {
                    emit loginResults(ScanRet::FAILURE_1);
                }
                stop();
                mtx.unlock();
            }
        });
    }
    threadPool.waitForDone();
    delete[] mBuffer;
}

void QRCodeForScreen::continueLastLogin()
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

void QRCodeForScreen::run()
{
    ret = ScanRet::UNKNOW;
    m_stop.store(true);
    lastTicket.clear();
    lastQrCode.clear();
#ifndef SHOW
    cv::namedWindow("Video_Stream", cv::WINDOW_AUTOSIZE);
#endif
    switch (servertype)
    {
    case ServerType::Official:
        LoginOfficial();
        break;
    case ServerType::BH3_BiliBili:
        LoginBH3BiliBili();
        break;
    default:
        break;
    }
#ifndef SHOW
    cv::destroyWindow("Video_Stream");
#endif
}

void QRCodeForScreen::stop()
{
    m_stop.store(false);
}

void QRCodeForScreen::setServerType(const ServerType servertype)
{
    this->servertype = servertype;
}
