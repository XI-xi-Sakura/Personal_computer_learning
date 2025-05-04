
## 相关背景概念
- 临界资源：多线程执行流共享的资源就叫做临界资源
- 临界区：每个线程内部，访问临界资源的代码，就叫做临界区
- 互斥：任何时刻，互斥保证有且只有一个执行流进入临界区，访问临界资源，通常对临界资源起保护作用
- 原子性（后面讨论如何实现）：不会被任何调度机制打断的操作，该操作只有两态，要么完成，要么未完成

## 互斥量mutex
- 大部分情况，线程使用的数据都是**局部变量**，变量的地址空间在线程栈空间内，这种情况，变量归属单个线程，其他线程无法获得这种变量。
- 但有时候，很多变量都需要在线程间共享，这样的变量称为**共享变量**，可以通过数据的共享，完成线程之间的交互。
- 多个线程并发的操作共享变量，会带来一些问题。

```c
// 操作共享变量会有问题的售票系统代码
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

int ticket = 100;

void *route(void *arg) {
    char *id = (char*)arg;
    while (1) {
        if (ticket > 0) {
            usleep(1000);
            printf("%s sells ticket:%d\n", id, ticket);
            ticket--;
        } else {
            break;
        }
    }
}

int main( void ) {
    pthread_t t1, t2, t3, t4;

    pthread_create(&t1, NULL, route, "thread 1");
    pthread_create(&t2, NULL, route, "thread 2");
    pthread_create(&t3, NULL, route, "thread 3");
    pthread_create(&t4, NULL, route, "thread 4");

    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);
}
```
执行结果：

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/781ceb49d3af44039eb3ff72b640fc93.png)


### 为什么可能无法获得争取结果?
- `if` 语句判断条件为真以后，代码可以并发的切换到其他线程
- `usleep` 这个模拟漫长业务的过程，在这个漫长的业务过程中，可能有很多个线程会进入该代码段
- `ticket` 操作本身就不是一个原子操作

```
// 取出ticket--部分的汇编代码
// objdump -d a.out > test.objdump
// 152  40064b:   8b 05 e3 04 20 00       mov    0x2004e3(%rip),%eax        # 600b34 <ticket>
// 153  400651:   83 e8 01                sub    $0x1,%eax
// 154  400654:   89 05 da 04 20 00       mov    %eax,0x2004da(%rip)        # 600b34 <ticket>
```
- 操作并不是原子操作，而是对应三条汇编指令:
- load：将共享变量ticket从内存加载到寄存器中
- update：更新寄存器里面的值，执行-1操作
- store：将新值，从寄存器写回共享变量ticket的内存地址

要解决以上问题，需要做到三点:
- 代码必须要有互斥行为：当代码进入临界区执行时，**不允许其他线程进入该临界区**。
- 如果多个线程同时要求执行临界区的代码，并且临界区没有线程在执行，那么只能允许一个线程进入该临界区。
- 如果线程不在临界区中执行，那么该线程不能阻止其他线程进入临界区。

要做到这三点，本质上就是需要一把锁。Linux上提供的这把锁叫**互斥量**。 
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/82780e4a827649ea92198c1c24c0dc8a.png)

## 互斥量的接口
### 初始化互斥量
初始化互斥量有两种方法：
- **方法1，静态分配**：
```c
pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
```
- **方法2，动态分配**：
```c
int pthread_mutex_init(pthread_mutex_t *restrict mutex, 
					   const pthread_mutexattr_t *restrict attr);
					   
// 参数:
// mutex: 要初始化的互斥量
// attr: NULL
```
### 销毁互斥量
销毁互斥量需要注意：
- 使用`PTHREAD_MUTEX_INITIALIZER`初始化的互斥量不需要销毁
- 不要销毁一个已经加锁的互斥量
- 已经销毁的互斥量，要确保后面不会有线程再尝试加锁
```c
int pthread_mutex_destroy(pthread_mutex_t *mutex);
```
#### 互斥量加锁和解锁
```c
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);
// 返回值:成功返回0,失败返回错误号
```
调用`pthread_lock`时，可能会遇到以下情况：
- 互斥量处于未锁状态，该函数会将互斥量锁定，同时返回成功
- 发起函数调用时，其他线程已经锁定互斥量，或者存在其他线程同时申请互斥量，但没有竞争到互斥量，那么`pthread_lock`调用会陷入阻塞(执行流被挂起)，等待互斥量解锁。

**改进上面的售票系统**：
```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sched.h>

int ticket = 100;
pthread_mutex_t mutex;

void *route(void *arg) {
    char *id = (char*)arg;
    while (1) {
        pthread_mutex_lock(&mutex);
        if (ticket > 0) {
            usleep(1000);
            printf("%s sells ticket:%d\n", id, ticket);
            ticket--;
            pthread_mutex_unlock(&mutex);
            sched_yield();//放弃CPU
        } else {
            pthread_mutex_unlock(&mutex);
            break;
        }
    }
    return NULL;
}

int main( void ) {
    pthread_t t1, t2, t3, t4;
    pthread_mutex_init(&mutex, NULL);
    pthread_create(&t1, NULL, route, "thread 1");
    pthread_create(&t2, NULL, route, "thread 2");
    pthread_create(&t3, NULL, route, "thread 3");
    pthread_create(&t4, NULL, route, "thread 4");
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);
    pthread_mutex_destroy(&mutex);
    return 0;
}
```
## 互斥量实现原理
- 经过上面的例子，大家已经意识到单纯的`i++`或者`++i`都不是原子的，有可能会有数据一致性问题。
- 为了实现互斥锁操作，大多数体系结构都提供了`swap`或`exchange`指令，该指令的作用是把寄存器和内存单元的数据相交换，由于只有一条指令，保证了原子性，即使是多处理器平台，访问内存的总线周期也有先后，一个处理器上的交换指令执行时另一个处理器的交换指令只能等待总线周期。

现在我们把`lock`和`unlock`的伪代码改一下
```
lock:
    movb $0, %al
    xchgb %al, mutex
    if(al寄存器内容 > 0){
        return 0;
    } else {
        挂起等待;
        goto lock;
    }
unlock:
    movb $1, mutex
    唤醒等待Mutex的线程;
    return 0;
```
- 把数据从内存到寄存器，本质上就是从进程间线程共享到线程私有。
- 寄存器内容：执行流（线程）的硬件上下文。
- 一行汇编，就可以视为原子性的。

但是仍有几个问题需要说明：
- 锁本身就是一个全局变量，那谁来确保锁的安全？
	- 加锁和解锁被设计成原子性操作

- 所以如何看待锁呢？
	- 二元信号量，加锁本身就是对资源展开预定。
- 若申请锁的时候，都被别的线程拿走，怎么办？
	- 阻塞等待
- 线程在访问临界区代码时，可以切换至其他线程吗？
	- 可以，但是该线程被切走后，其他线程仍不能进入临界区（抱着锁切换的）；串行（效率低的原因） 
## 1-4 互斥量的封装
**Lock.hpp**
```cpp
#pragma once
#include <iostream>
#include <string>
#include <pthread.h>

namespace LockModule {
    // 对锁进行封装，可以独立使用
    class Mutex {
    public:
        // 删除不要的拷贝和赋值
        Mutex(const Mutex &) = delete;
        const Mutex &operator =(const Mutex &) = delete;
        Mutex() {
            int n = pthread_mutex_init(&_mutex, nullptr);
            (void)n;
        }
        void Lock() {
            int n = pthread_mutex_lock(&_mutex);
            (void)n;
        }
        void Unlock() {
            int n = pthread_mutex_unlock(&_mutex);
            (void)n;
        }
        pthread_mutex_t *GetMutexOriginal() { // 获取原始指针
            return &_mutex;
        }
        ~Mutex() {
            int n = pthread_mutex_destroy(&_mutex);
            (void)n;
        }
    private:
        pthread_mutex_t _mutex;
    };

    // 采用RAII风格，进行锁管理
    class LockGuard {
    public:
        LockGuard(Mutex &mutex):_mutex(mutex) {
            _mutex.Lock();
        }
        ~LockGuard() {
            _mutex.Unlock();
        }
    private:
        Mutex &_mutex;
    };
}
```
**抢票的代码就可以更新成为**
```cpp
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "Lock.hpp"

using namespace LockModule;

int ticket = 100;
Mutex mutex;

void *route(void *arg) {
    char *id = (char*)arg;
    while (1) {
        LockGuard lockguard(mutex); // 使用RAII风格的锁
        if (ticket > 0) {
            usleep(1000);
            printf("%s sells ticket:%d\n", id, ticket);
            ticket--;
        } else {
            break;
        }
    }
    return nullptr;
}

int main(void) {
    pthread_t t1, t2, t3, t4;
    pthread_create(&t1, NULL, route, (void*)"thread 1");
    pthread_create(&t2, NULL, route, (void*)"thread 2");
    pthread_create(&t3, NULL, route, (void*)"thread 3");
    pthread_create(&t4, NULL, route, (void*)"thread 4");
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    pthread_join(t3, NULL);
    pthread_join(t4, NULL);
    return 0;
}
```
**RAII风格的互斥锁，C++也有，比如**：
```cpp
std::mutex mtx;
std::lock_guard<std::mutex> guard(mtx);
```

