#ifndef __BLOCK_QUEUE_HPP__
#define __BLOCK_QUEUE_HPP__

#include <iostream>
#include <string>
#include <queue>
#include <pthread.h>

template <typename T>
class BlockQueue
{
private:
    bool IsFull()
    {
        return _block_queue.size() == _cap;
    }
    bool IsEmpty()
    {
        return _block_queue.empty();
    }

public:
    BlockQueue(int cap) : _cap(cap)
    {
        _producer_wait_num = 0;
        _consumer_wait_num = 0;
        pthread_mutex_init(&_mutex, nullptr);
        pthread_cond_init(&_product_cond, nullptr);
        pthread_cond_init(&_consum_cond, nullptr);
    }
    void Enqueue(T &in)
    { // 生产者用的接口
        pthread_mutex_lock(&_mutex);
        while (IsFull())
        { // 保证代码的健壮性
            // 生产线程去等待，是在临界区中休眠的！你现在还持有锁呢！！！
            // 1. pthread_cond_wait调用是：
            // a. 让调用线程等待；
            // b. 自动释放曾经持有的_mutex锁；
            // c. 当条件满足，线程唤醒，pthread_cond_wait要求线程：必须重新竞争mutex锁，竞争成功，方可返回！！！
            _producer_wait_num++;
            pthread_cond_wait(&_product_cond, &_mutex); // 只要等待，必定会有唤醒，唤醒的时候，就要继续从这个位置往下走！！
            _producer_wait_num--;
            // 之后：安全
        }
        // 进行生产
        _block_queue.push(std::move(in));
        // 通知消费者来消费
        if (_consumer_wait_num > 0)
            pthread_cond_signal(&_consum_cond);
        pthread_mutex_unlock(&_mutex);
    }
    void Pop(T *out)
    { // 消费者用的接口
        pthread_mutex_lock(&_mutex);
        while (IsEmpty())
        { // 保证代码的健壮性
            // 消费线程去等待，是在临界区中休眠的！你现在还持有锁呢！！！
            // 1. pthread_cond_wait调用是：
            // a. 让调用线程等待；
            // b. 自动释放曾经持有的_mutex锁；
            // c. 当条件满足，线程唤醒，pthread_cond_wait要求线程：必须重新竞争mutex锁，竞争成功，方可返回！！！
            _consumer_wait_num++;
            pthread_cond_wait(&_consum_cond, &_mutex); // 伪唤醒
            _consumer_wait_num--;
        }
        // 进行消费
        *out = _block_queue.front();
        _block_queue.pop();
        // 通知生产者来生产
        if (_producer_wait_num > 0)
            pthread_cond_signal(&_product_cond);
        pthread_mutex_unlock(&_mutex);
    }
    ~BlockQueue()
    {
        pthread_mutex_destroy(&_mutex);
        pthread_cond_destroy(&_product_cond);
        pthread_cond_destroy(&_consum_cond);
    }

private:
    std::queue<T> _block_queue; // 阻塞队列，是被整体使用的！！！
    int _cap;                   // 上限

    pthread_mutex_t _mutex; // 保护_block_queue的锁

    pthread_cond_t _product_cond; // 专门给生产者提供的条件变量
    pthread_cond_t _consum_cond;  // 专门给消费者提供的条件变量

    int _producer_wait_num;
    int _consumer_wait_num;
};

#endif