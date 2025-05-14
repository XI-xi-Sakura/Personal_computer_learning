#include <iostream>
#include <string>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        std::cerr << "\nUsage: " << argv[0] << " serverip serverport\n"
                  << std::endl;
        return 1;
    }
    std::string serverip = argv[1];
    uint16_t serverport = std::stoi(argv[2]);

    int clientSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (clientSocket < 0)
    {
        std::cerr << "socket failed" << std::endl;
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(serverport);                  // 替换为服务器端口
    serverAddr.sin_addr.s_addr = inet_addr(serverip.c_str()); // 替换为服务器IP地址

    int result = connect(clientSocket, (struct sockaddr *)&serverAddr, sizeof(serverAddr));
    if (result < 0)
    {
        std::cerr << "connect failed" << std::endl;
        ::close(clientSocket);
        return 1;
    }
    while (true)
    {
        std::string message;
        std::cout << "Please Enter@ ";
        std::getline(std::cin, message);
        if (message.empty())
            continue;
        send(clientSocket, message.c_str(), message.size(), 0);

        char buffer[1024] = {0};
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived > 0)
        {
            buffer[bytesReceived] = '\0'; // 确保字符串以 null 结尾
            std::cout << "Received from server: " << buffer << std::endl;
        }
        else
        {
            std::cerr << "recv failed" << std::endl;
        }
    }
    ::close(clientSocket);
    return 0;
}