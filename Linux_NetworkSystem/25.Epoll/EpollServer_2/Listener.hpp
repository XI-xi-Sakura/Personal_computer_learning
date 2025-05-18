#pragma once

#include <iostream>
#include <memory>
#include "Socket.hpp"
#include "Log.hpp"

using namespace SocketModule;
using namespace LogModule;
// 专门负责获取链接的模块
// 连接管理器
class Listener
{
public:
    Listener(int port)
        : _listensock(std::make_unique<TcpSocket>()),
          _port(port)
    {
        _listensock->BuildTcpSocketMethod(_port);
    }
    void Accepter()
    {
        LOG(LogLevel::DEBUG) << "hahahha Accepter";
    }
    int Sockfd() { return _listensock->Fd(); }
    ~Listener()
    {
        _listensock->Close();
    }

private:
    std::unique_ptr<Socket> _listensock;
    int _port;
};