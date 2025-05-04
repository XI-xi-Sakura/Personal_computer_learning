#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "Mutex.hpp"

using namespace LockModule;

int ticket = 100;
Mutex mutex;

void *route(void *arg)
{
    char *id = (char *)arg;
    while (1)
    {
        LockGuard lockguard(mutex); // 使用RAII风格的锁
        if (ticket > 0)
        {
            usleep(1000);
            printf("%s sells ticket:%d\n", id, ticket);
            ticket--;
        }
        else
        {
            break;
        }
    }
    return nullptr;
}

int main(void)
{
    pthread_t t1, t2, t3, t4;
    pthread_create(&t1, NULL, route, (void *)"thread 1");
    pthread_create(&t2, NULL, route, (void *)"thread 2");
    pthread_create(&t3, NULL, route, (void *)"thread 3");
    pthread_create(&t4, NULL, route, (void *)"thread 4");
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);
    return 0;
}