#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
#include <libswscale/swscale.h>
};

#include <QThread>
#include <QMutex>
#include <QtConcurrent/QtConcurrent>
#include <QFuture>
#include <QThreadPool>

#include "ApiDefs.hpp"
#include "ScannerBase.hpp"

class QRCodeForStream final :
    public QThread,
    public ScannerBase
{
    Q_OBJECT
public:
    QRCodeForStream(QObject* parent = nullptr);
    ~QRCodeForStream();
    Q_DISABLE_COPY_MOVE(QRCodeForStream)

    void setLoginInfo(const std::string_view uid, const std::string_view gameToken);
    void setPassportLoginInfo(const std::string_view uid, const std::string_view stoken,
                              const std::string_view mid);
    void setLoginInfo(const std::string_view uid, const std::string_view gameToken, const std::string& name);
    void setServerType(const ServerType servertype);
    void setAutoLogin(bool enabled);
    void setContinuousScan(bool enabled);
    [[nodiscard]] bool isContinuousScan() const;
    [[nodiscard]] std::string lastInitError();
    void setUrl(const std::string& url, const std::map<std::string, std::string> heard = {});
    auto init() -> bool;
    void run();
    void stop();
    void continueLastLogin();

Q_SIGNALS:
    void loginResults(const ScanRet ret);
    void loginConfirm(const GameType gameType, bool b);

private:
    std::mutex mtx;
    void LoginOfficial();
    void LoginBH3BiliBili();
    void setStreamHW();
    void setInitError(const std::string& stage, int errorCode = 0);
    std::string streamUrl{};
    std::map<std::string, std::string> streamHeaders;
    std::string m_lastInitError;
    std::string m_name;
    std::string stoken;
    std::string mid;
    std::string lastAttemptTicket;
    std::chrono::steady_clock::time_point lastAttemptAt{};
    ServerType servertype;
    ScanRet ret = ScanRet::UNKNOW;
    AVDictionary* pAvdictionary;
    AVFormatContext* pAVFormatContext;
    AVCodecContext* pAVCodecContext;
    SwsContext* pSwsContext;
    AVFrame* pAVFrame;
    AVPacket* pAVPacket;
    int videoStreamIndex{ 0 };
    int videoStreamWidth{};
    int videoStreamHeight{};
    const int threadNumber{ 2 };
    QThreadPool threadPool;
    std::atomic<bool> m_stop;
    std::atomic<bool> m_autoLogin{ false };
    std::atomic<bool> m_continuousScan{ false };
};
