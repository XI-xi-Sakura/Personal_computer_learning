#pragma once

#include <iostream>
#include <string>
#include <queue>
#include <vector>
#include <memory>
#include "Log.hpp"
#include "Mutex.hpp"
#include "Cond.hpp"
#include "Thread.hpp"

namespace ThreadPoolModule
{
    using namespace LogMudule;
    using namespace ThreadModule;
    using namespace LockModule;
    using namespace CondModule;

    // 用来做测试的线程方法
    void DefaultTest()
    {
        while (true)
        {
            LOG(LogLevel::DEBUG) << "我是一个测试方法";
            sleep(1);
        }
    }

    using thread_t = std::shared_ptr<Thread>;

    const static int defaultnum = 5;

    template <typename T>
    class ThreadPool
    {
    private:
        bool IsEmpty() { return _taskq.empty(); }

        void HandlerTask(std::string name)
        {
            LOG(LogLevel::INFO) << "线程: " << name << ", 进入HandlerTask的逻辑";
            while (true)
            {
                // 1. 拿任务
                T t;
                {
                    LockGuard lockguard(_lock);
                    while (IsEmpty() && _isrunning)
                    {
                        _wait_num++;
                        _cond.Wait(_lock);
                        _wait_num--;
                    }
                    // 2. 任务队列为空 && 线程池退出了
                    if (IsEmpty() && !_isrunning)
                        break;

                    t = _taskq.front();
                    _taskq.pop();
                }

                // 2. 处理任务
                t(name); // 规定，未来所有的任务处理，全部都是必须提供()方法！
            }
            LOG(LogLevel::INFO) << "线程: " << name << " 退出";
        }

    public:
        ThreadPool(int num = defaultnum) : _num(num), _wait_num(0), _isrunning(false)
        {
            for (int i = 0; i < _num; i++)
            {
                _threads.push_back(std::make_shared<Thread>(std::bind(&ThreadPool::HandlerTask, this, std::placeholders::_1)));
                LOG(LogLevel::INFO) << "构建线程" << _threads.back()->Name() << "对象 ... 成功";
            }
        }
        void Equeue(T &&in)
        {
            LockGuard lockguard(_lock);
            if (!_isrunning)
                return;
            _taskq.push(std::move(in));
            if (_wait_num > 0)
                _cond.Notify();
        }
        void Start()
        {
            if (_isrunning)
                return;
            _isrunning = true; // bug fix??
            for (auto &thread_ptr : _threads)
            {
                LOG(LogLevel::INFO) << "启动线程" << thread_ptr->Name() << " ... 成功";
                thread_ptr->Start();
            }
        }
        void Wait()
        {
            for (auto &thread_ptr : _threads)
            {
                thread_ptr->Join();
                LOG(LogLevel::INFO) << "回收线程" << thread_ptr->Name() << " ... 成功";
            }
        }
        void Stop()
        {
            LockGuard lockguard(_lock);
            if (_isrunning)
            {
                // 3. 不能在入任务了
                _isrunning = false; // 不工作
                // 1. 让线程自己退出(要唤醒) && // 2. 历史的任务被处理完了
                if (_wait_num > 0)
                    _cond.NotifyAll();
            }
        }
        ~ThreadPool()
        {
        }

    private:
        std::vector<thread_t> _threads;
        int _num;
        int _wait_num;
        std::queue<T> _taskq; // 临界资源

        Mutex _lock;
        Cond _cond;

        bool _isrunning;
    };
}

// using namespace std;
// using namespace ThreadModule;
// using namespace LockModule;
// using namespace LogModule;

// const static int gdefaultthreadnum = 10;

// template <typename T>
// class ThreadPool
// {
// private:
//     void HandlerTask() // 类的成员方法，也可以成为另一个类的回调方法，方便我们继续类级别的互相调用！
//     {
//         std::string name = GetThreadNameFromNptl();
//         LOG(LogLevel::INFO) << name << " is running...";
//         while (true)
//         {
//             // 1. 保证队列安全
//             _mutex.Lock();
//             // 2. 队中不一定有数据
//             while (_task_queue.empty() && _isrunning)
//             {
//                 _cond.Wait(_mutex);
//                 _waitnum++;
//                 _waitnum--;
//             }
//             // 2.1 如果线程池已经退出了 && 任务队列是空的
//             if (_task_queue.empty() &&!_isrunning)
//             {
//                 _mutex.Unlock();
//                 break;
//             }
//             // 2.3 如果线程池没退出 && 任务队列不是空的
//             // 2.3 如果线程池已经退出 && 任务队列不是空的 --- 处理完所有的任务，然后在退出
//             // 3. 一定有任务，处理任务
//             T task = _task_queue.front();
//             _task_queue.pop();
//             _mutex.Unlock();

//             LOG(LogLevel::DEBUG) << name << " get a task";
//             // 4. 处理任务，这个任务属于线程独占的任务
//             task();
//         }
//     }
// public:
//     // 是要有的，必须是私有的
//     ThreadPool(int threadnum = gdefaultthreadnum) : _threadnum(threadnum), _waitnum(0), _isrunning(false)
//     {
//         LOG(LogLevel::INFO) << "ThreadPool Construct()";
//     }
//     void InitThreadPool()
//     {
//         // 指向构造出所有的线程，并不启动
//         for (int num = 0; num < _threadnum; num++)
//         {
//             _threads.emplace_back(std::bind(&ThreadPool::HandlerTask, this));
//             LOG(LogLevel::INFO) << "init thread " << _threads.back().Name() << " done";
//         }
//     }
//     void Start()
//     {
//         _isrunning = true;
//         for (auto &thread : _threads)
//         {
//             thread.Start();
//             LOG(LogLevel::INFO) << "start thread " << thread.Name() << " done";
//         }
//     }
//     void Stop()
//     {
//         _mutex.Lock();
//         _isrunning = false;
//         _cond.NotifyAll();
//         _mutex.Unlock();
//         LOG(LogLevel::DEBUG) << "线程池退出中...";
//     }
//     void Wait()
//     {
//         for (auto &thread : _threads)
//         {
//             thread.Join();
//             LOG(LogLevel::INFO) << thread.Name() << " 退出...";
//         }
//     }
//     bool Enqueue(const T &t)
//     {
//         bool ret = false;
//         _mutex.Lock();
//         if (_isrunning)
//         {
//             _task_queue.push(t);
//             if (_waitnum > 0)
//             {
//                 _cond.Notify();
//                 LOG(LogLevel::DEBUG) << "任务入队列成功";
//             }
//             ret = true;
//         }
//         _mutex.Unlock();
//         return ret;
//     }
// private:
//     std::vector<Thread> _threads; // for fix, int temp
//     std::queue<T> _task_queue;
//     Cond _cond;
//     Mutex _mutex;
//     int _waitnum;
//     bool _isrunning;
//     int _threadnum;
// };