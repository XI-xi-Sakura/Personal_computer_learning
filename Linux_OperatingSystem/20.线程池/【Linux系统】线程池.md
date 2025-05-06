## 线程池设计
**线程池**：
一种线程使用模式。线程过多会带来调度开销，进而影响缓存局部性和整体性能。而线程池维护着多个线程，等待着监督管理者分配可并发执行的任务。这避免了在处理短时间任务时创建与销毁线程的代价。线程池不仅能够保证内核的充分利用，还能防止过分调度。可用线程数量应该取决于可用的并发处理器、处理器内核、内存、网络sockets等的数量。

**线程池的应用场景**：
- 需要大量的线程来完成任务，且完成任务的时间比较短。比如WEB服务器完成网页请求这样的任务，使用线程池技术是非常合适的。因为单个任务小，而任务数量巨大，你可以想象一个热门网站的点击次数。但对于长时间的任务，比如一个Telnet连接请求，线程池的优点就不明显了。因为Telnet会话时间比线程的创建时间大多了。
- 对性能要求苛刻的应用，比如要求服务器迅速响应客户请求。
- 接受突发性的大量请求，但不至于使服务器因此产生大量线程的应用。突发性大量客户请求，在没有线程池情况下，将产生大量线程，虽然理论上大部分操作系统线程数目最大值不是问题，短时间内产生大量线程可能使内存到达极限，出现错误。

**线程池的种类**：
a. 创建固定数量线程池，循环从**任务队列**中获取任务对象，获取到任务对象后，执行任务对象中的任务接口
b. 浮动线程池，其他同上
此处，我们选择固定线程个数的线程池。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/9f2674a557f5499d9e9c4bef65dea917.png)

## ThreadPool.hpp
```cpp
#pragma once
#include <iostream>
#include <vector>
#include <queue>
#include <memory>

#include <pthread.h>
#include "Log.hpp"     // 引入自己的日志
#include "Thread.hpp"  // 引入自己的线程
#include "Lock.hpp"    // 引入自己的锁
#include "Cond.hpp"    // 引入自己的条件变量

using namespace std;
using namespace ThreadModule;
using namespace LockModule;
using namespace LogModule;

const static int gdefaultthreadnum = 10;

template <typename T>
class ThreadPool
{
private:
    void HandlerTask() // 类的成员方法，也可以成为另一个类的回调方法，方便我们继续类级别的互相调用！
    {
        std::string name = GetThreadNameFromNptl();
        LOG(LogLevel::INFO) << name << " is running...";
        while (true)
        {
            // 1. 保证队列安全
            _mutex.Lock();
            // 2. 队中不一定有数据
            while (_task_queue.empty() && _isrunning)
            {
            	_waitnum++;
                _cond.Wait(_mutex);
                _waitnum--;
            }
            // 2.1 如果线程池已经退出了 && 任务队列是空的
            if (_task_queue.empty() &&!_isrunning)
            {
                _mutex.Unlock();
                break;
            }
            // 2.3 如果线程池没退出 && 任务队列不是空的
            // 2.3 如果线程池已经退出 && 任务队列不是空的 --- 处理完所有的任务，然后在退出
            // 3. 一定有任务，处理任务
            T task = _task_queue.front();
            _task_queue.pop();
            _mutex.Unlock();

            LOG(LogLevel::DEBUG) << name << " get a task";
            // 4. 处理任务，这个任务属于线程独占的任务
            task();
        }
    }
public:
    // 是要有的，必须是私有的
    ThreadPool(int threadnum = gdefaultthreadnum) : _threadnum(threadnum), _waitnum(0), _isrunning(false)
    {
        LOG(LogLevel::INFO) << "ThreadPool Construct()";
    }
    void InitThreadPool()
    {
        // 指向构造出所有的线程，并不启动
        for (int num = 0; num < _threadnum; num++)
        {
            _threads.emplace_back(std::bind(&ThreadPool::HandlerTask, this));
            LOG(LogLevel::INFO) << "init thread " << _threads.back().Name() << " done";
        }
    }
    void Start()
    {
        _isrunning = true;
        for (auto &thread : _threads)
        {
            thread.Start();
            LOG(LogLevel::INFO) << "start thread " << thread.Name() << " done";
        }
    }
    void Stop()
    {
        _mutex.Lock();
        _isrunning = false;
        _cond.NotifyAll();
        _mutex.Unlock();
        LOG(LogLevel::DEBUG) << "线程池退出中...";
    }
    void Wait()
    {
        for (auto &thread : _threads)
        {
            thread.Join();
            LOG(LogLevel::INFO) << thread.Name() << " 退出...";
        }
    }
    bool Enqueue(const T &t)
    {
        bool ret = false;
        _mutex.Lock();
        if (_isrunning)
        {
            _task_queue.push(t);
            if (_waitnum > 0)
            {
                _cond.Notify();
                LOG(LogLevel::DEBUG) << "任务入队列成功";
            }
            ret = true;
        }
        _mutex.Unlock();
        return ret;
    }
private:
    std::vector<Thread> _threads; // for fix, int temp
    std::queue<T> _task_queue;
    Cond _cond;
    Mutex _mutex;
    int _waitnum;
    bool _isrunning;
    int _threadnum;
};
```

## 线程安全的单例模式
### 什么是单例模式
单例模式是一种设计模式，**确保一个类在整个应用程序中只有一个实例**，并提供全局访问点。在很多场景下，比如服务器开发中管理大量加载到内存的数据时，只希望存在一个实例来进行统一管理 。
### 单例模式的特点
某些类，只应该具有一个对象(实例)，就称之为单例。

例如一个男人只能有一个媳妇。
在很多服务器开发场景中，经常需要让服务器加载很多的数据(上百G)到内存中。此时往往要用一个单例类来管理这些数据。
### 饿汉实现方式和懒汉实现方式
[洗碗的例子]
1. 吃完饭，立刻洗碗，这种就是饿汉方式。因为一顿吃的时候可以立刻拿着碗就能吃饭。
2. 吃完饭，先把碗放下，然后下一顿饭用到这个碗了再洗碗，就是懒汉方式。

懒汉方式最核心的思想是“**延时加载**”，从而能够优化服务器的启动速度。
### 饿汉方式实现单例模式
```cpp
template <typename T>
class Singleton {
    static T data;
public:
    static T* GetInstance() {
        return &data;
    }
};
```
只要通过 Singleton 这个包装类来使用 T 对象，则一个进程中只有一个 T 对象的实例。
### 懒汉方式实现单例模式
```cpp
template <typename T>
class Singleton {
    static T* inst;
public:
    static T* GetInstance() {
        if (inst == NULL) {
            inst = new T();
        }
        return inst;
    }
};
```
存在一个严重的问题，线程不安全。
第一次调用 GetInstance 的时候，如果两个线程同时调用，可能会创建出两份 T 对象的实例。
但是后续再次调用，就没有问题了。
### 懒汉方式实现单例模式(线程安全版本)
```cpp
// 懒汉模式，线程安全
template <typename T>
class Singleton {
    volatile static T* inst;  // 需要设置 volatile 关键字，否则可能被编译器优化。
    static std::mutex lock;
public:
    static T* GetInstance() {
        if (inst == NULL) {    // 双重判定空指针，降低锁冲突的概率，提高性能。
            lock.lock();      // 使用互斥锁，保证多线程情况下也只调用一次 new。
            if (inst == NULL) {
                inst = new T();
            }
            lock.unlock();
        }
        return inst;
    }
};
```
注意事项:
1. 加锁解锁的位置
2. 双重 if 判定，**避免不必要的锁竞争**
3. volatile 关键字防止过度优化
## 单例式线程池
### ThreadPool.hpp
```cpp
#pragma once

#include <iostream>
#include <vector>
#include <queue>
#include <memory>
#include <pthread.h>
#include "Log.hpp"      // 引入自己的日志
#include "Thread.hpp"   // 引入自己的线程
#include "Lock.hpp"     // 引入自己的锁
#include "Cond.hpp"     // 引入自己的条件变量

using namespace ThreadModule;
using namespace CondModule;
using namespace LockModule;
using namespace LogModule;

const static int gdefaultthreadnum = 10;

// 日志
template <typename T>
class ThreadPool
{
private:
    // 是要有的，必须是私有的
    ThreadPool(int threadnum = gdefaultthreadnum) : _threadnum(threadnum), _waitnum(0), _isrunning(false)
    {
        LOG(LogLevel::INFO) << "ThreadPool Construct()";
    }
    void InitThreadPool()
    {
        // 指向构建所有的线程，并不启动
        for (int num = 0; num < _threadnum; num++)
        {
            _threads.emplace_back(std::bind(&ThreadPool::HandlerTask, this));
            LOG(LogLevel::INFO) << "init thread " << _threads.back().Name() << " done";
        }
    }
    void Start()
    {
        _isrunning = true;
        for (auto &thread : _threads)
        {
            thread.Start();
            LOG(LogLevel::INFO) << "start thread " << thread.Name() << " done";
        }
    }
    void HandlerTask() // 类的成员方法，也可以成为另一个类的回调方法，方便我们继续类别的互相调用！
    {
        std::string name = GetThreadNameFromMptl();
        LOG(LogLevel::INFO) << name << " is running...";
        while (true)
        {
            // 1. 保证队列安全
            _mutex.Lock();
            // 2. 队列中不一定有数据
            while (_task_queue.empty() && _isrunning)
            {
                _waitnum++;
                _cond.Wait(_mutex);
                _waitnum--;
            }
            // 2.1 如果线程池已经退出了 && 任务队列是空的
            if (_task_queue.empty() &&!_isrunning)
            {
                _mutex.Unlock();
                break;
            }
            // 2.2 如果线程池没退出 && 任务队列不是空的
            // 2.3 一定有任务，处理任务
            T t = _task_queue.front();
            _task_queue.pop();
            _mutex.Unlock();
            LOG(LogLevel::DEBUG) << name << " get a task";
            // 4. 处理任务，这个任务属于线程独占的任务
            t();
        }
    }
    // 单例模式
    ThreadPool<T>& operator=(const ThreadPool<T>&) = delete;
    ThreadPool(const ThreadPool<T>&) = delete;
public:
    static ThreadPool<T>* GetInstance()
    {
        // 只有第一次会创建对象，后续都是这块代码
        // 双判断的方式，可以有效减少获取单例的加锁成本，而且保证线程安全
        if (nullptr == _instance)  // 保证第一次之后，所有线程，不用在加锁，直接返回单例对象
        {
            LockGuard lockguard(_lock);
            if (nullptr == _instance)
            {
                _instance = new ThreadPool<T>();
                _instance->InitThreadPool();
                _instance->Start();
                LOG(LogLevel::DEBUG) << "创建线程池单例";
            }
            return _instance;
        }
        LOG(LogLevel::DEBUG) << "获取线程池单例";
        return _instance;
    }
    void Stop()
    {
        _mutex.Lock();
        _isrunning = false;
        _cond.NotifyAll();
        _mutex.Unlock();
        LOG(LogLevel::DEBUG) << "线程池退出中...";
    }
    void Wait()
    {
        for (auto &thread : _threads)
        {
            thread.Join();
            LOG(LogLevel::INFO) << thread.Name() << " 退出...";
        }
    }
    bool Enqueue(const T&t)
    {
        bool ret = false;
        _mutex.Lock();
        if (!_isrunning)
        {
            _mutex.Unlock();
            return ret;
        }
        _task_queue.push(t);
        if (_waitnum > 0)
        {
            _cond.Notify();
        }
        LOG(LogLevel::DEBUG) << "任务入队成功";
        ret = true;
        _mutex.Unlock();
        return ret;
    }
private:
    std::vector<Thread> _threads; 
    int _threadnum;
    std::queue<T> _task_queue;
    Cond _cond;
    int _waitnum;
    Mutex _mutex;
    bool _isrunning;
    // 添加单例模式
    static ThreadPool<T>* _instance;
    static Mutex _lock;
};

template <typename T>
ThreadPool<T>* ThreadPool<T>::_instance = nullptr;
template <typename T>
Mutex ThreadPool<T>::_lock;
```
