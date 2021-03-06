/**
 *
 *  ListenerManager.cc
 *  An Tao
 *
 *  Copyright 2018, An Tao.  All rights reserved.
 *  https://github.com/an-tao/drogon
 *  Use of this source code is governed by a MIT license
 *  that can be found in the License file.
 *
 *  Drogon
 *
 */

#include "ListenerManager.h"
#include "HttpServer.h"
#include "HttpAppFrameworkImpl.h"
#include <drogon/config.h>
#include <trantor/utils/Logger.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace drogon
{
class DrogonFileLocker : public trantor::NonCopyable
{
  public:
    DrogonFileLocker()
    {
        fd_ = open("/tmp/drogon.lock", O_TRUNC | O_CREAT, 0755);
        flock(fd_, LOCK_EX);
    }
    ~DrogonFileLocker()
    {
        close(fd_);
    }

  private:
    int fd_{0};
};

}  // namespace drogon

using namespace trantor;
using namespace drogon;

void ListenerManager::addListener(const std::string &ip,
                                  uint16_t port,
                                  bool useSSL,
                                  const std::string &certFile,
                                  const std::string &keyFile)
{
#ifndef OpenSSL_FOUND
    if (useSSL)
    {
        LOG_ERROR << "Can't use SSL without OpenSSL found in your system";
    }
#endif
    listeners_.emplace_back(ip, port, useSSL, certFile, keyFile);
}

std::vector<trantor::EventLoop *> ListenerManager::createListeners(
    const HttpAsyncCallback &httpCallback,
    const WebSocketNewAsyncCallback &webSocketCallback,
    const ConnectionCallback &connectionCallback,
    size_t connectionTimeout,
    const std::string &globalCertFile,
    const std::string &globalKeyFile,
    size_t threadNum,
    const std::vector<std::function<HttpResponsePtr(const HttpRequestPtr &)>>
        &syncAdvices)
{
#ifdef __linux__
    std::vector<trantor::EventLoop *> ioLoops;
    for (size_t i = 0; i < threadNum; ++i)
    {
        LOG_TRACE << "thread num=" << threadNum;
        auto loopThreadPtr = std::make_shared<EventLoopThread>("DrogonIoLoop");
        listeningloopThreads_.push_back(loopThreadPtr);
        ioLoops.push_back(loopThreadPtr->getLoop());
        for (auto const &listener : listeners_)
        {
            auto const &ip = listener.ip_;
            bool isIpv6 = ip.find(':') == std::string::npos ? false : true;
            std::shared_ptr<HttpServer> serverPtr;
            if (i == 0)
            {
                DrogonFileLocker lock;
                // Check whether the port is in use.
                TcpServer server(HttpAppFrameworkImpl::instance().getLoop(),
                                 InetAddress(ip, listener.port_, isIpv6),
                                 "drogonPortTest",
                                 true,
                                 false);
                serverPtr = std::make_shared<HttpServer>(
                    loopThreadPtr->getLoop(),
                    InetAddress(ip, listener.port_, isIpv6),
                    "drogon",
                    syncAdvices);
            }
            else
            {
                serverPtr = std::make_shared<HttpServer>(
                    loopThreadPtr->getLoop(),
                    InetAddress(ip, listener.port_, isIpv6),
                    "drogon",
                    syncAdvices);
            }

            if (listener.useSSL_)
            {
#ifdef OpenSSL_FOUND
                auto cert = listener.certFile_;
                auto key = listener.keyFile_;
                if (cert == "")
                    cert = globalCertFile;
                if (key == "")
                    key = globalKeyFile;
                if (cert == "" || key == "")
                {
                    std::cerr
                        << "You can't use https without cert file or key file"
                        << std::endl;
                    exit(1);
                }
                serverPtr->enableSSL(cert, key);
#endif
            }
            serverPtr->setHttpAsyncCallback(httpCallback);
            serverPtr->setNewWebsocketCallback(webSocketCallback);
            serverPtr->setConnectionCallback(connectionCallback);
            serverPtr->kickoffIdleConnections(connectionTimeout);
            serverPtr->start();
            servers_.push_back(serverPtr);
        }
    }
#else
    auto loopThreadPtr =
        std::make_shared<EventLoopThread>("DrogonListeningLoop");
    listeningloopThreads_.push_back(loopThreadPtr);
    ioLoopThreadPoolPtr_ = std::make_shared<EventLoopThreadPool>(threadNum);
    for (auto const &listener : listeners_)
    {
        LOG_TRACE << "thread num=" << threadNum;
        auto ip = listener.ip_;
        bool isIpv6 = ip.find(':') == std::string::npos ? false : true;
        auto serverPtr = std::make_shared<HttpServer>(
            loopThreadPtr->getLoop(),
            InetAddress(ip, listener.port_, isIpv6),
            "drogon",
            syncAdvices);
        if (listener.useSSL_)
        {
#ifdef OpenSSL_FOUND
            auto cert = listener.certFile_;
            auto key = listener.keyFile_;
            if (cert == "")
                cert = globalCertFile;
            if (key == "")
                key = globalKeyFile;
            if (cert == "" || key == "")
            {
                std::cerr << "You can't use https without cert file or key file"
                          << std::endl;
                exit(1);
            }
            serverPtr->enableSSL(cert, key);
#endif
        }
        serverPtr->setIoLoopThreadPool(ioLoopThreadPoolPtr_);
        serverPtr->setHttpAsyncCallback(httpCallback);
        serverPtr->setNewWebsocketCallback(webSocketCallback);
        serverPtr->setConnectionCallback(connectionCallback);
        serverPtr->kickoffIdleConnections(connectionTimeout);
        serverPtr->start();
        servers_.push_back(serverPtr);
    }
    auto ioLoops = ioLoopThreadPoolPtr_->getLoops();
#endif
    return ioLoops;
}

void ListenerManager::startListening()
{
    if (listeners_.size() == 0)
        return;
    for (auto &loopThread : listeningloopThreads_)
    {
        loopThread->run();
    }
}

ListenerManager::~ListenerManager()
{
    for (size_t i = 0; i < servers_.size(); ++i)
    {
        std::promise<int> pro;
        auto f = pro.get_future();
        servers_[i]->getLoop()->runInLoop([&pro, this, i] {
            servers_[i].reset();
            pro.set_value(1);
        });
        (void)f.get();
    }
}
