#include <iostream>
#include <string>
#include "Log.hpp"
#include "Listener.hpp"
#include "Connection.hpp"
#include "EpollServer.hpp"

using namespace LogModule;

int main(int argc, char *argv[])
{
    if (argc != 2)
    {
        std::cout << "Usage: " << argv[0] << " port" << std::endl;
        return 1;
    }
    ENABLE_CONSOLE_LOG();
    uint16_t local_port = std::stoi(argv[1]);
    
    Listener listen(local_port); // 完成具体工作的模块, 也有一个listensockfd.
    // 我们要把listensockfd，封装成为一个Connection，Connection托管给EpollServer
    auto conn = std::make_shared<Connection>(listen.Sockfd());
    conn->InitCB(
        [&listen](){
            listen.Accepter();
        }, 
        nullptr,
        nullptr
    );
    conn->SetEvents(EPOLLIN|EPOLLET);

    EpollServer epoll_svr;
    epoll_svr.Init();
    epoll_svr.InsertConnection(conn);
    epoll_svr.Loop();

    return 0;
}