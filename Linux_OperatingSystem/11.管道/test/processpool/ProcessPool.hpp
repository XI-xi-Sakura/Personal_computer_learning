#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <functional>
#include "Task.hpp"
#include "Channel.hpp"

// typedef std::function<void()> work_t;
using work_t = std::function<void()>;

enum
{
    OK = 0,
    UsageError,
    PipeError,
    ForkError
};

class ProcessPool
{
public:
    ProcessPool(int n, work_t w)
        : processnum(n), work(w)
    {
    }
    // channels : 输出型参数
    // work_t work: 回调
    int InitProcessPool()
    {
        // 2. 创建指定个数个进程
        for (int i = 0; i < processnum; i++)
        {
            // 1. 先有管道
            int pipefd[2] = {0};
            int n = pipe(pipefd);
            if (n < 0)
                return PipeError;
            // 2. 创建进程
            pid_t id = fork();
            if (id < 0)
                return ForkError;

            // 3. 建立通信信道
            if (id == 0)
            {
                // 关闭历史wfd
                std::cout << getpid() << ", child close history fd: ";
                for (auto &c : channels)
                {
                    std::cout << c.wFd() << " ";
                    c.Close();
                }
                std::cout << " over" << std::endl;

                ::close(pipefd[1]); // read
                // child
                std::cout << "debug: " << pipefd[0] << std::endl;
                dup2(pipefd[0], 0);
                work();
                ::exit(0);
            }

            // 父进程执行
            ::close(pipefd[0]); // write
            channels.emplace_back(pipefd[1], id);
            // Channel ch(pipefd[1], id);
            // channels.push_back(ch);
        }

        return OK;
    }

    void DispatchTask()
    {
        int who = 0;
        // 2. 派发任务
        int num = 20;
        while (num--)
        {
            // a. 选择一个任务， 整数
            int task = tm.SelectTask();
            // b. 选择一个子进程channel
            Channel &curr = channels[who++];
            who %= channels.size();

            std::cout << "######################" << std::endl;
            std::cout << "send " << task << " to " << curr.Name() << ", 任务还剩: " << num << std::endl;
            std::cout << "######################" << std::endl;

            // c. 派发任务
            curr.Send(task);

            sleep(1);
        }
    }

    void CleanProcessPool()
    {
        // version 3
        for (auto &c : channels)
        {
            c.Close();
            pid_t rid = ::waitpid(c.Id(), nullptr, 0);
            if (rid > 0)
            {
                std::cout << "child " << rid << " wait ... success" << std::endl;
            }
        }

        // version 2
        // for (auto &c : channels)
        // for(int i = channels.size()-1; i >= 0; i--)
        // {
        //     channels[i].Close();
        //     pid_t rid = ::waitpid(channels[i].Id(), nullptr, 0); // 阻塞了！
        //     if (rid > 0)
        //     {
        //         std::cout << "child " << rid << " wait ... success" << std::endl;
        //     }
        // }

        // version 1
        // for (auto &c : channels)
        // {
        //     c.Close();
        // }
        //?
        // for (auto &c : channels)
        // {
        //     pid_t rid = ::waitpid(c.Id(), nullptr, 0);
        //     if (rid > 0)
        //     {
        //         std::cout << "child " << rid << " wait ... success" << std::endl;
        //     }
        // }
    }

    void DebugPrint()
    {
        for (auto &c : channels)
        {
            std::cout << c.Name() << std::endl;
        }
    }

private:
    std::vector<Channel> channels;
    int processnum;
    work_t work;
};
