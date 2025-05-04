#include <iostream>
#include <unistd.h>
#include <pthread.h>

thread_local int a = 100;

// 新线程
void *run(void *args)
{
    while (true)
    {
        pthread_t tid = pthread_self();
        std::cout << "new thread, tid: " << tid << ", a: " << a << std::endl;
        sleep(5);
    }
    return nullptr;
}

int main()
{

    std::cout << "我是一个进程: " << getpid() << std::endl;
    pthread_t tid;
    pthread_create(&tid, nullptr, run, (void *)"thread-1");

    pthread_t ptid = pthread_self();
    // 主线程
    while (true)
    {
        a += 100;
        std::cout << "main thread, tid: " << ptid << ", a: " << a << std::endl;
        sleep(5);
    }

    return 0;
}