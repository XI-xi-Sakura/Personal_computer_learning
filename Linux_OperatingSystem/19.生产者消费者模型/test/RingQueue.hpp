template <typename T>
class RingQueue
{
private:
    void Lock(pthread_mutex_t &mutex)
    {
        pthread_mutex_lock(&mutex);
    }
    void Unlock(pthread_mutex_t &mutex)
    {
        pthread_mutex_unlock(&mutex);
    }

public:
    RingQueue(int cap)
        : _cap(cap),
          _room_sem(cap),
          _data_sem(0),
          _producer_step(0),
          _consumer_step(0)
    {
        pthread_mutex_init(&_producer_mutex, nullptr);
        pthread_mutex_init(&_consumer_mutex, nullptr);
    }
    void Enqueue(const T &in)
    {
        // 生产行为
        _room_sem.P();
        Lock(_producer_mutex);
        // 一定有空间！！！
        _ring_queue[_producer_step++] = in; // 生产
        _producer_step %= _cap;
        Unlock(_producer_mutex);
        _data_sem.V();
    }
    void Pop(T *out)
    {
        // 消费行为
        _data_sem.P();
        Lock(_consumer_mutex);
        *out = _ring_queue[_consumer_step++];
        _consumer_step %= _cap;
        Unlock(_consumer_mutex);
        _room_sem.V();
    }
    ~RingQueue()
    {
        pthread_mutex_destroy(&_producer_mutex);
        pthread_mutex_destroy(&_consumer_mutex);
    }

private:
    // 1. 环形队列
    std::vector<T> _ring_queue;
    int _cap; // 环形队列的容量上限
    // 2. 生产和消费的下标
    int _producer_step;
    int _consumer_step;
    // 3. 定义信号量
    Sem _room_sem; // 生产者关心
    Sem _data_sem; // 消费者关心
    // 4. 定义锁，维护多生产多消费之间的互斥关系
    pthread_mutex_t _producer_mutex;
    pthread_mutex_t _consumer_mutex;
};