#pragma once

#include <iostream>
#include <string>
#include <functional>
#include "InetAddr.hpp"

class EpollServer;

using func_t = std::function<void()>; // TODO

// 普通的fd, Listensockfd;
// 让对fd的处理方式采用同一种方式
// 描述一个连接
class Connection
{
public:
    Connection(int sockfd) : _sockfd(sockfd), _events(0)
    {
    }
    void InitCB(func_t recver, func_t sender, func_t excepter)
    {
        _recver = recver;
        _sender = sender;
        _excepter = excepter;
    }
    void SetPeerInfo(const InetAddr &peer_addr)
    {
        _peer_addr = peer_addr;
    }
    int Sockfd() { return _sockfd; }
    void SetEvents(uint32_t events) { _events = events; }
    uint32_t GetEvents() { return _events; }
    void CallRecv()
    {
        if(_recver != nullptr)
            _recver();
    }
    void CallSend()
    {
        if(_sender != nullptr)
            _sender();
    }
    ~Connection()
    {
    }

private:
    int _sockfd;
    std::string _inbuffer;
    std::string _outbuffer;
    InetAddr _peer_addr; // 对应哪一个客户端

    // 回调方法
    func_t _recver;
    func_t _sender;
    func_t _excepter;

    // 添加一个指针
    EpollServer *_owner;

    // 我关心的事件
    uint32_t _events; // 我这个connection关心的事件
};