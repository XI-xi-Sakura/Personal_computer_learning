// 操作共享变量会有问题的售票系统代码
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

int ticket = 1000;
pthread_spinlock_t lock;

void *route(void *arg)
{
    char *id = (char *)arg;
    while (1)
    {
        while (1)
        {
            pthread_spin_lock(&lock);
            if (ticket > 0)
            {
                usleep(1000);
                printf("%s sells ticket:%d\n", id, ticket);
                ticket--;
                pthread_spin_unlock(&lock);
            }
            else
            {
                pthread_spin_unlock(&lock);
                break;
            }
        }
        break;
    }
    return nullptr;
}

int main(void)
{
    pthread_spin_init(&lock, PTHREAD_PROCESS_PRIVATE);
    pthread_t t1, t2, t3, t4;
    pthread_create(&t1, NULL, route, (void *)"thread 1");
    pthread_create(&t2, NULL, route, (void *)"thread 2");
    pthread_create(&t3, NULL, route, (void *)"thread 3");
    pthread_create(&t4, NULL, route, (void *)"thread 4");

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);

    pthread_spin_destroy(&lock);
    return 0;
}