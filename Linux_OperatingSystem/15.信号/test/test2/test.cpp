#include <iostream>
#include <unistd.h>
#include <signal.h>
#include <vector>
#include <functional>

using func_t = std::function<void()>;

int gcount = 0;
std::vector<func_t> gfuncs;

void handler(int signo)
{
    for (auto &f : gfuncs)
    {
        f();
    }
    std::cout << "gcount : " << gcount << std::endl;
    int n = alarm(1); // 重设闹钟，会返回上一次闹钟的剩余时间
    std::cout << "剩余时间 : " << n << std::endl;
}

int main()
{
    gfuncs.push_back([]()
                     { std::cout << "我是一个内核刷新操作" << std::endl; });
    gfuncs.push_back([]()
                     { std::cout << "我是一个检测进程时间片的操作，如果时间片到了，我会切换进程" << std::endl; });
    gfuncs.push_back([]()
                     { std::cout << "我是一个内存管理操作，定期清理操作系统内部的内存碎片" << std::endl; });

    alarm(1); // 一次性的闹钟，超时alarm会自动被取消
    signal(SIGALRM, handler);

    while (true)
    {
        pause();
        std::cout << "我醒来了..." << std::endl;
        gcount++;
    }
}