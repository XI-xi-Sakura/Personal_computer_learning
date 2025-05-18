#include <iostream>
#include <cstdio>
#include <string>
#include <cerrno>
#include <unistd.h>
#include <fcntl.h>

void SetNonBlock(int fd)
{
    int fl = fcntl(fd, F_GETFL);
    if (fl < 0)
    {
        perror("fcntl");
        return;
    }
    fcntl(fd, F_SETFL, fl | O_NONBLOCK); // O_NONBLOCK：让该fd，以非阻塞方式进行工作
}

int main()
{
    std::string tips = "Please Enter# ";
    char buffer[1024];
    SetNonBlock(0);
    while (true)
    {
        write(0, tips.c_str(), tips.size());
        // 非阻塞，如果我们不做输入，数据不就绪，以出错形式返回！！
        // read不是有读取失败(-1)吗？失败vs底层数据没就绪 -> 底层数据没就绪，不算失败
        // 如果是-1， 失败vs底层数据没就绪我们后续的做法是不同的！
        // read->-1,失败vs底层数据没就绪->需要区分的必要性的！
        // errno表示：更详细的出错原因,最近一次调用，出错的时候的出错码
        int n = read(0, buffer, sizeof(buffer));
        if (n > 0)
        {
            buffer[n] = 0;
            std::cout << "echo# " << buffer << std::endl;
        }
        else if (n == 0)
        {
            std::cout << "read file end!" << std::endl;
            break;
        }
        else
        {
            // EAGAIN		11	/* Try again */
            // EWOULDBLOCK	EAGAIN	/* Operation would block */
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // 做其他事情呢？

                std::cout << "底层数据，没有就绪" << std::endl;
                sleep(1);

                continue;
            }
            else if (errno == EINTR)
            {
                std::cout << "被中断,从新来" << std::endl;
                sleep(1);

                continue;
            }
            else
            {
                std::cout << "read error: " << n << ", errno: " << errno << std::endl;
            }
        }
    }
}
