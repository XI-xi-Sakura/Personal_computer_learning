#pragma once
#include <iostream>
#include <memory>
#include <unordered_map>
#include "Epoller.hpp"
#include "Connection.hpp"

using namespace EpollModule;

using connection_t = std::shared_ptr<Connection>;

class EpollServer
{
    const static int event_num = 64;
public:
    EpollServer():_isrunning(false)
    {}
    void Init()
    {
        _epoller->Init();
    }
    void InsertConnection(connection_t &conn)
    {
        auto iter = _connections.find(conn->Sockfd());
        if(iter == _connections.end())
        {
            // 1. 把连接，放到unordered_map中进行管理
            _connections.insert(std::make_pair(conn->Sockfd(), conn));
            // 2. 把新插入进来的连接，写透到内核的epoll中
            _epoller->Ctrl(conn->Sockfd(), conn->GetEvents());
        }
    }
    bool IsConnectionExists(int sockfd)
    {
        return _connections.find(sockfd) != _connections.end();
    }
    void Loop()
    {
        _isrunning = true;
        while(_isrunning)
        {
            int n = _epoller->Wait(_revs, event_num);
            for(int i = 0 ;i < n; i++)
            {
                // 开始进行派发, 派发给指定的模块
                int sockfd = _revs[i].data.fd;
                uint32_t revents = _revs[i].events;
                if((revents & EPOLLERR) || (revents & EPOLLHUP))
                    revents = (EPOLLIN | EPOLLOUT); // 异常事件，转换成为读写事件
                if((revents&EPOLLIN) && IsConnectionExists(sockfd))
                {
                    _connections[sockfd]->CallRecv();
                }
                if((revents & EPOLLOUT) && IsConnectionExists(sockfd))
                {
                    _connections[sockfd]->CallSend();
                }
            }
        }
        _isrunning = false;
    }
    void Stop()
    {
        _isrunning = false;
    }
    ~EpollServer()
    {}
private:
    std::unique_ptr<Epoller> _epoller; // 不要忘记初始化
    std::unordered_map<int, connection_t> _connections; // fd: Connection， 服务器内部所有的连接
    bool _isrunning;
    struct epoll_event _revs[event_num];
};