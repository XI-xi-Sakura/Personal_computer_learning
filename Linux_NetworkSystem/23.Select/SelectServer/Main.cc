#include "SelectServer.hpp"

// ./select_server 8080
int main(int argc, char *argv[])
{
    if(argc != 2)
    {
        std::cout << "Usage: " << argv[0] << " port" << std::endl;
        return 1;
    }
    ENABLE_CONSOLE_LOG();
    uint16_t local_port = std::stoi(argv[1]);

    std::unique_ptr<SelectServer> ssvr = std::make_unique<SelectServer>(local_port);
    ssvr->Init();
    ssvr->Loop();

    return 0;
}


































// int main()
// {
//     fd_set fds;
//     std::cout << "fds: " << sizeof(fds)*8 << std::endl;
// }