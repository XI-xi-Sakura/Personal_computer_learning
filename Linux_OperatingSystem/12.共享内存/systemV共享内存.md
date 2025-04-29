# system V共享内存
在Linux系统中，共享内存是一种高效的进程间通信（IPC）机制，它允许两个或者多个进程共享同一块**物理内存区域**，这些进程可以将这块区域映射到自己的虚拟地址空间中。

共享内存区是最快的IPC形式。一旦这样的**内存映射到共享它的进程的地址空间**，这些进程间数据传递不再涉及到内核，换句话说进程不再通过执行进入内核的系统调用来传递彼此的数据。

## 1.共享内存示意图
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/7c565e076e744b70900c19e5eb9860e5.png)

## 2.共享内存数据结构

```c
struct shmid_ds {
    struct ipc_perm       shm_perm;      /* operation perms */
    int                   shm_segsz;     /* size of segment (bytes) */
    __kernel_time_t       shm_atime;     /* last attach time */
    __kernel_time_t       shm_dtime;     /* last detach time */
    __kernel_time_t       shm_ctime;     /* last change time */
    __kernel_ipc_pid_t    shm_cpid;      /* pid of creator */
    __kernel_ipc_pid_t    shm_lpid;      /* pid of last operator */
    unsigned short        shm_nattch;    /* no. of current attaches */
    unsigned short        shm_nattch;    /* no. of current attaches */
    unsigned short        shm_unused;    /* compatibility */
    void                  shm_unused2;   /* ditto - used by DIPC */
    void                  shm_unused3;   /* unused */
};
```

## 3.共享内存函数

### shmget函数

1. 功能: 用来创建共享内存，在共享内存中起着关键的初始作用，负责在系统中分配一块共享内存区域或者获取已有的共享内存的表示符
2. 原型
```c
#include <sys/ipc.h>
#include <sys/shm.h>

int shmget(key_t key, size_t size, int shmflg);
```
3. 参数
    - key: 共享内存段名字（**要用户传入**），`key`是一个键值，用于唯一标识一块共享内存段。（这个键值可以用`ftok`函数生成），若key值为`IPC_PRIVATE`,则创建一个新的私有共享内存段，这个段只能通过子进程继承的方式被其他进程访问，通常用于父子进程之间的通信
    - size: 共享内存大小
    - shmflg: 由九个权限标志构成，还包括权限位（用法和创建文件时使用的mode模式标志是一样的）
        - 取值为`IPC_CREAT`: 共享内存不存在，创建并返回；共享内存已存在，获取并返回。
        - 取值为`IPC_CREAT | IPC_EXCL`: 共享内存不存在，创建并返回；共享内存已存在，出错返回一个非负整数，即该共享内存段的标识码；失败返回-1（只要成功，所创建的共享内存一定是新的）
4. 返回值: 成功返回一个非负整数，即该共享内存段的标识码；失败返回-1
### ftok函数
`ftok` 函数是在Linux系统里用于生成System V IPC（Inter-Process Communication，进程间通信）键值的函数。这个函数能够把一个路径名与一个项目ID转换为一个系统V IPC键值，该键值可用于标识共享内存段、消息队列和信号量集等。

1. 函数原型
```c
#include <sys/types.h>
#include <sys/ipc.h>

key_t ftok(const char *pathname, int proj_id);
```

2. 参数说明
- `pathname`：一个存在且可访问的文件或目录的路径名。该文件或目录必须真实存在，因为 `ftok` 会使用它的索引节点号（inode number）。
- `proj_id`：一个项目ID，它是一个1 - 255之间的非零整数。这个ID会和 `pathname` 的索引节点号结合起来生成IPC键值。

3. 返回值
- 若成功，返回一个有效的IPC键值。
- 若失败，返回 -1，并设置 `errno` 以指示错误类型。


4. 示例代码
以下是一个使用 `ftok` 函数的简单示例：
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/ipc.h>

int main() {
    const char *pathname = "/tmp";
    int proj_id = 'A';

    key_t key = ftok(pathname, proj_id);
    if (key == -1) {
        perror("ftok");
        return 1;
    }

    printf("Generated key: %d\n", (int)key);
    return 0;
}
```
在这个示例中，使用 `/tmp` 作为 `pathname`，`'A'` 作为 `proj_id` 来生成一个IPC键值。若生成成功，就会把该键值打印出来；若失败，则会输出错误信息。

5. 注意事项
- 要保证 `pathname` 对应的文件或目录在使用期间不会被删除或替换，不然生成的键值可能会改变。
- 不同的 `pathname` 和 `proj_id` 组合应该能生成不同的键值，但也不能完全排除生成相同键值的可能性。
- `proj_id` 的取值范围是1 - 255，若超出这个范围，可能会导致未定义行为。 

### shmat函数

1. 功能: 将共享内存段连接到进程地址空间
2. 原型
```c
void *shmat(int shmid, const void *shmaddr, int shmflg);
```
3. 参数
    - `shmid`: 共享内存标识
    - `shmaddr`: 指定共享内存连接到进程空间的起始地址，通常设置为NULL，让系统自动选择一个合适地址自行链接
    - `shmflg`: 用于控制共享内存与进程地址空间之间的联系方式和访问权限等
    	- `SHM_RDONLY`:表示一只读方式连接共享内存段，若不指定该标志，则默认是读写方式连接
    	- `SHM_REMAP`:若设置该标志，当`shmaddr`部位`NULL`时，系统会将指定地址处已经存在的映射替换新的共享内存映射
4. 返回值: 成功返回一个指针，指向共享内存所连接的进程地址空间的起始位置；失败返回-1

说明:
1. `shmaddr`为`NULL`，核心自动选择一个地址
2. `shmaddr`不为`NULL`且shmflg无`SHM_RND`标记，则以shmaddr为连接地址。
3. `shmaddr`不为`NULL`且shmflg设置了`SHM_RND`标记，则连接的地址会自动向下调整为SHMLBA（指定共享内存段在进程地址空间中的最低地址边界）的整数倍。公式: s`hmaddr - (shmaddr % SHMLBA)`
4. `shmflg`=`SHM_RDONLY`，表示连接操作用来只读共享内存

### shmdt函数

1. 功能: 将共享内存段与当前进程脱离，**但不删除共享内存段**
2. 原型
```c
int shmdt(const void *shmaddr);
```
3. 参数
    - shmaddr: 由`shmat`所返回的指针
4. 返回值: 成功返回0；失败返回-1
5. 注意: 将共享内存段与当前进程脱离不等于删除共享内存段

### shmctl函数

1. 功能: 用于控制共享内存
2. 原型
```c
int shmctl(int shmid, int cmd, struct shmid_ds *buf);
```
3. 参数
    - shmid: 由shmget返回的共享内存标识码
    - cmd: 将要采取的动作 (有三个可取值)
    - buf: 指向一个保存着共享内存的模式状态和访问权限的数据结构
4. 返回值: 成功返回0；失败返回-1

| 命令 | 说明 |
| ---- | ---- |
| IPC_STAT | 把`shmid_ds`结构中的数据设置为共享内存的当前关联值 |
| IPC_SET | 在进程有足够权限的前提下，把共享内存的当前关联值设置为shmid_ds数据结构中给出的值 |
| IPC_RMID | 删除共享内存段 |

## 4.共享内存的生命周期
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/786eca6e503348a78c25941f9a347b43.jpeg)

### 测试代码结构

#### 示例1

server.c
```c
#include <iostream>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

const char* PATH = ".";
const int PROJ_ID = 'a';
const int SHM_SIZE = 1024;

int main() {
    // 生成唯一键值
    key_t key = ftok(PATH, PROJ_ID);
    if (key == -1) {
        perror("ftok");
        return 1;
    }

    // 获取共享内存
    int shmid = shmget(key, SHM_SIZE, 0666);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }

    // 将共享内存附加到进程地址空间
    char* shm_addr = static_cast<char*>(shmat(shmid, nullptr, 0));
    if (shm_addr == reinterpret_cast<char*>(-1)) {
        perror("shmat");
        return 1;
    }

    // 从共享内存读取数据
    std::cout << "Reader: Data read from shared memory: " << shm_addr << std::endl;

    // 将共享内存从进程地址空间分离
    if (shmdt(shm_addr) == -1) {
        perror("shmdt");
        return 1;
    }

    // 删除共享内存
    if (shmctl(shmid, IPC_RMID, nullptr) == -1) {
        perror("shmctl");
        return 1;
    }

    return 0;
}
```

client.c
```c
#include <iostream>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <cstring>

const char* PATH = ".";
const int PROJ_ID = 'a';
const int SHM_SIZE = 1024;

int main() {
    // 生成唯一键值
    key_t key = ftok(PATH, PROJ_ID);
    if (key == -1) {
        perror("ftok");
        return 1;
    }

    // 创建共享内存
    int shmid = shmget(key, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid == -1) {
        perror("shmget");
        return 1;
    }

    // 将共享内存附加到进程地址空间
    char* shm_addr = static_cast<char*>(shmat(shmid, nullptr, 0));
    if (shm_addr == reinterpret_cast<char*>(-1)) {
        perror("shmat");
        return 1;
    }

    // 向共享内存写入数据
    const char* message = "Hello, shared memory!";
    strcpy(shm_addr, message);
    std::cout << "Writer: Data written to shared memory." << std::endl;

    // 将共享内存从进程地址空间分离
    if (shmdt(shm_addr) == -1) {
        perror("shmdt");
        return 1;
    }

    return 0;
}
```



**注意: 共享内存没有进行同步与互斥! 共享内存缺乏访问控制! 会带来并发问题。**

#### 实例2.借助管道实现访问控制版的共享内存
Comm.hpp
```cpp
#pragma once
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <iostream>

using namespace std;

#define Debug 0
#define Notice 1
#define Warning 2
#define Error 3
const std::string msg[] = {
    "Debug",
    "Notice",
    "Warning",
    "Error"
};
std::ostream &Log(std::string message, int level) {
    std::cout << " | " << (unsigned)time(nullptr) << " | " << msg[level] << " | " << message;
    return std::cout;
}
#define PATH_NAME "/home/hyb"
#define PROJ_ID 0x66
#define SHM_SIZE 4096 // 共享内存的大小，最好是页(PAGE: 4096)的整数倍
#define FIFO_NAME "./fifo"
class Init {
public:
    Init() {
        int n = mkfifo(FIFO_NAME, 0666);
        assert(n == 0);
        (void)n;
        Log("create fifo success", Notice) << "\n";
    }
    ~Init() {
        unlink(FIFO_NAME);
        Log("remove fifo success", Notice) << "\n";
    }
};
#define READ O_RDONLY
#define WRITE O_WRONLY
int OpenFIFO(std::string pathname, int flags) {
    int fd = open(pathname.c_str(), flags);
    assert(fd >= 0);
    return fd;
}
void CloseFifo(int fd) {
    close(fd);
}
void Wait(int fd) {
    Log("等待中...", Notice) << "\n";
    uint32_t temp = 0;
    ssize_t s = read(fd, &temp, sizeof(uint32_t));
    assert(s == sizeof(uint32_t));
    (void)s;
}
void Signal(int fd) {
    uint32_t temp = 1;
    ssize_t s = write(fd, &temp, sizeof(uint32_t));
    assert(s == sizeof(uint32_t));
    (void)s;
    Log("唤醒中...", Notice) << "\n";
}
string TransToHex(key_t k) {
    char buffer[32];
    snprintf(buffer, sizeof buffer, "0x%x", k);
    return buffer;
}
```

ShmServer.cc
```cpp
#include "Comm.hpp"
Init init;
int main() {
    // 1. 创建公共的Key值
    key_t k = ftok(PATH_NAME, PROJ_ID);
    assert(k != -1);
    Log("create key done", Debug) << " server key : " << TransToHex(k) << endl;
    // 2. 创建共享内存 -- 建议要创建一个全新的共享内存 -- 通信的发起者
    int shmid = shmget(k, SHM_SIZE, IPC_CREAT | IPC_EXCL | 0666);
    if (shmid == -1) {
        perror("shmget");
        exit(1);
    }
    Log("create shm done", Debug) << " shmid : " << shmid << endl;
    // 3. 将指定的共享内存，挂接到自己的地址空间
    char* shmaddr = (char*)shmat(shmid, nullptr, 0);
    Log("attach shm done", Debug) << " shmid : " << shmid << endl;
    // 4. 访问控制
    int fd = OpenFIFO(FIFO_NAME, O_RDONLY);
    while (true) {
        // 阻塞
        Wait(fd);
        // 临界区
        printf("%s\n", shmaddr);
        if (strcmp(shmaddr, "quit") == 0)
            break;
    }
    CloseFifo(fd);
    // 5. 将指定的共享内存，从自己的地址空间中去关联
    int n = shmdt(shmaddr);
    assert(n != -1);
    (void)n;
    Log("detach shm done", Debug) << " shmid : " << shmid << endl;
    // 6. 删除共享内存，IPC_RMID, 即便是有进程和当下的shm挂接，依旧删除共享内存
    n = shmctl(shmid, IPC_RMID, nullptr);
    assert(n != -1);
    (void)n;
    Log("delete shm done", Debug) << " shmid : " << shmid << endl;
    return 0;
}
```

ShmClient.cc
```cpp
#include "Comm.hpp"
int main() {
    // 1. 创建公共的Key值
    key_t k = ftok(PATH_NAME, PROJ_ID);
    if (k < 0) {
        Log("create key failed", Error) << " client key : " << TransToHex(k) << endl;
        exit(1);
    }
    Log("create key done", Debug) << " client key : " << TransToHex(k) << endl;
    // 2. 获取共享内存
    int shmid = shmget(k, SHM_SIZE, 0);
    if (shmid < 0) {
        Log("create key failed", Error) << " client key : " << TransToHex(k) << endl;
        exit(2);
    }
    Log("create shm success", Error) << " client key : " << TransToHex(k) << endl;
    // 3. 挂接共享内存
    char* shmaddr = (char*)shmat(shmid, nullptr, 0);
    if (shmaddr == (char*)-1) {
        Log("attach shm failed", Error) << " client key : " << TransToHex(k) << endl;
        exit(3);
    }
    Log("attach shm success", Error) << " client key : " << TransToHex(k) << endl;
    // 4. 写
    int fd = OpenFIFO(FIFO_NAME, O_WRONLY);
    while (true) {
        ssize_t s = read(0, shmaddr, SHM_SIZE - 1);
        if (s > 0) {
            shmaddr[s - 1] = 0;
            Signal(fd);
            if (strcmp(shmaddr, "quit") == 0)
                break;
        }
    }
    CloseFifo(fd);
    // 5. 去关联
    int n = shmdt(shmaddr);
    assert(n != -1);
    Log("detach shm success", Error) << " client key : " << TransToHex(k) << endl;
    return 0;
}
```

## 5.共享内存系统级管理指令

### 1. ipcs
该指令用于显示系统中的进程间通信（IPC）资源信息，其中就包含共享内存段。
- **显示所有共享内存段**
```bash
ipcs -m
```
- **显示指定用户的共享内存段**
```bash
ipcs -m -u username
```

### 2. ipcrm
此指令用于删除系统中的IPC资源，能够删除指定的共享内存段。
- **通过ID删除共享内存段**
```bash
ipcrm -m shmid
```
其中`shmid`是共享内存段的ID，可以通过`ipcs -m`命令获取。

### 3. df -h
虽然它主要用于显示文件系统磁盘使用情况，但也能查看挂载的共享内存文件系统（如`tmpfs`）的使用情况。
```bash
df -h
```

### 4. mount
该指令可用于挂载共享内存文件系统。比如，要将`tmpfs`文件系统挂载到`/mnt/ramdisk`目录：
```bash
mount -t tmpfs -o size=512M tmpfs /mnt/ramdisk
```
这里的`size=512M`指定了共享内存的大小为512MB。

### 5. umount
当你不再需要使用共享内存文件系统时，可使用该指令进行卸载。
```bash
umount /mnt/ramdisk
```


