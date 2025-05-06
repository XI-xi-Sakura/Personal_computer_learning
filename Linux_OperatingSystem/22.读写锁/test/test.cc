#include <iostream>
#include <pthread.h>
#include <unistd.h>
#include <vector>
#include <cstdlib>
#include <ctime>

// 共享资源
int shared_data = 0;

// 读写锁
pthread_rwlock_t rwlock;

// 读者线程函数
void *Reader(void *arg)
{
    // sleep(1); //读者优先，一旦读者进入&&读者很多，写者基本就很难进入了
    int number = *(int *)arg;
    while (true)
    {
        pthread_rwlock_rdlock(&rwlock); // 读者加锁
        std::cout << "读者-" << number << " 正在读取数据, 数据是: " << shared_data << std::endl;
        sleep(1);                       // 模拟读取操作
        pthread_rwlock_unlock(&rwlock); // 解锁
    }
    delete (int *)arg;
    return NULL;
}

// 写者线程函数
void *Writer(void *arg)
{
    int number = *(int *)arg;
    while (true)
    {
        pthread_rwlock_wrlock(&rwlock); // 写者加锁
        shared_data = rand() % 100;     // 修改共享数据
        std::cout << "写者- " << number << " 正在写入. 新的数据是: " << shared_data << std::endl;
        sleep(2);                       // 模拟写入操作
        pthread_rwlock_unlock(&rwlock); // 解锁
    }
    delete (int *)arg;
    return NULL;
}

int main()
{
    srand(time(nullptr) ^ getpid());
    pthread_rwlockattr_t attr;
    // 初始化读写锁属性对象
    pthread_rwlockattr_init(&attr);
    // 设置写者优先模式
    pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NP);

    // 使用属性对象初始化读写锁
    pthread_rwlock_init(&rwlock, &attr);

    // 销毁读写锁属性对象
    pthread_rwlockattr_destroy(&attr);

    // 可以更高读写数量配比，观察现象
    const int reader_num = 2;
    const int writer_num = 2;
    const int total = reader_num + writer_num;
    pthread_t threads[total]; // 假设读者和写者数量相等

    // 创建读者线程
    for (int i = 0; i < reader_num; ++i)
    {
        int *id = new int(i);
        pthread_create(&threads[i], NULL, Reader, id);
    }

    // 创建写者线程
    for (int i = reader_num; i < total; ++i)
    {
        int *id = new int(i - reader_num);
        pthread_create(&threads[i], NULL, Writer, id);
    }

    // 等待所有线程完成
    for (int i = 0; i < total; ++i)
    {
        pthread_join(threads[i], NULL);
    }

    pthread_rwlock_destroy(&rwlock); // 销毁读写锁

    return 0;
}
