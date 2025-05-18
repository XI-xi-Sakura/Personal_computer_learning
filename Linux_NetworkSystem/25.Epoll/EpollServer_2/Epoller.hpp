#pragma once

#include <iostream>
#include <sys/epoll.h>
#include "Log.hpp"
#include "Common.hpp"

using namespace LogModule;

namespace EpollModule
{
    class Epoller
    {
    public:
        Epoller() : _epfd(-1)
        {
        }
        void Init()
        {
            int n = epoll_create(256); // bug
            if (n < 0)
            {
                LOG(LogLevel::ERROR) << "epoll_create error";
                exit(EPOLL_CREATE_ERR);
            }
            LOG(LogLevel::INFO) << "epoll_create success, epfd: " << _epfd;
        }
        int Wait(struct epoll_event revs[], int num) // 输出就绪的fd和events
        {
            int n = epoll_wait(_epfd, revs, num, -1);
            if (n < 0)
            {
                LOG(LogLevel::WARNING) << "epoll_wait error";
            }
            return n;
        }
        void Ctrl(int sockfd, uint32_t events)
        {
            struct epoll_event ev;
            ev.events = events;
            ev.data.fd = sockfd;
            int n = epoll_ctl(_epfd, EPOLL_CTL_ADD, sockfd, &ev);
            if (n < 0)
            {
                LOG(LogLevel::WARNING) << "epoll_ctl error";
            }
        }
        ~Epoller()
        {
        }

    private:
        int _epfd;
    };
}