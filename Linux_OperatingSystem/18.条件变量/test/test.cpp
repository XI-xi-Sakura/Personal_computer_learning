#include <iostream>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

void *active(void *arg)
{
    std::string name = static_cast<const char *>(arg);
    while (true)
    {
        pthread_mutex_lock(&mutex);
        pthread_cond_wait(&cond, &mutex);
        std::cout << name << " 活动..." << std::endl;
        pthread_mutex_unlock(&mutex);
    }
}

int main(void)
{
    pthread_t t1, t2;
    pthread_create(&t1, NULL, active, (void *)"thread-1");
    pthread_create(&t2, NULL, active, (void *)"thread-2");
    sleep(3); // 可有可无，这里确保两个线程已经在运行
    while (true)
    {
        // 对比测试
        // pthread_cond_signal(&cond); // 唤醒一个线程
        pthread_cond_broadcast(&cond); // 唤醒所有线程
        sleep(1);
    }
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
}