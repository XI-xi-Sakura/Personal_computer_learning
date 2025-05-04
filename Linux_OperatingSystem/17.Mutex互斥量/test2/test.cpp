#include <iostream>
#include <vector>
#include "Thread.hpp"
#include <mutex>

using namespace ThreadModule;

#define NUM 4

// 1. 锁本身是全局的，那么锁也是共享资源！谁保证锁的安全？？
// pthread_mutex:加锁和解锁被设计成为原子的了 --- TODO??
// 2. 如何看待锁呢？二元信号量就是锁！
//    2.1 加锁本质就是对资源展开预订！
//    2.2 整体使用资源！！
// 3. 如果申请锁的时候，锁被别人已经拿走了，怎么办？其他线程要进行阻塞等待
// 4. 线程在访问临界区代码的时候，可以不可以切换？？可以切换！！
//    4.1 我被切走的时候，别人能进来吗？？不能！我是抱着锁，被切换的！！不就是串行吗！效率低的原因！原子性！
// 5. 不遵守这个约定？？bug！




// pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
int ticketnum = 10000; // 共享资源,临界资源
std::mutex gmtx;

class ThreadData
{
public:
    std::string name;
    pthread_mutex_t *lock_ptr;
};

void Ticket(ThreadData &td)
{
    while (true)
    {
        gmtx.lock();
        // pthread_mutex_lock(td.lock_ptr); // 加锁
        if (ticketnum > 0)
        {
            usleep(1000);

            // 1. 抢票
            printf("get a new ticket, who get it: %s, id: %d\n", td.name.c_str(), ticketnum--);
            gmtx.unlock();
            // pthread_mutex_unlock(td.lock_ptr); // ??
            // 2. 入库模拟
            usleep(50);
        }
        else
        {
            // pthread_mutex_unlock(td.lock_ptr); // ??
            gmtx.unlock();
            break;
        }
    }
}

int main()
{
    pthread_mutex_t lock;
    pthread_mutex_init(&lock, nullptr);

    // 1. 构建线程对象
    std::vector<Thread<ThreadData>> threads;

    for (int i = 0; i < NUM; i++)
    {
        ThreadData *td = new ThreadData();
        td->lock_ptr = &lock;
        threads.emplace_back(Ticket, *td);
        td->name = threads.back().Name();
    }

    // 2. 启动线程
    for (auto &thread : threads)
    {
        thread.Start();
    }

    // 3. 等待线程
    for (auto &thread : threads)
    {
        thread.Join();
    }

    pthread_mutex_destroy(&lock);

    return 0;
}