# 多路转接select

## 多路转接/多路复用
- select
- poll
- epoll

多路转接的核心作用：
- 对多个文件描述符进行等待，通知上层有哪些fd已就绪
- 本质上就是一种对IO事件就绪的通知机制
## I/O多路转接之select

### **初识select**

系统提供select函数来实现多路复用输入/输出模型。
- select系统调用是用来让我们的程序监视多个文件描述符的状态变化的；
- 程序会停在select这里等待，直到被监视的文件描述符有一个或多个发生了状态改变。

### **select函数原型**

select的函数原型如下：
```c
#include <sys/select.h>
int select( int nfds, 
			fd_set *readfds, 
			fd_set *writefds, 
			fd_set *exceptfds, 
			struct timeval *timeout
		   );
```
**参数解释**：
- 参数nfds是需要监视的**最大的文件描述符值+1**；
- `rdset`、`wrset`、`exset`分别对应于需要检测的**可读文件描述符**的集合，可**写文件**描述符的集合及**异常文件**描述符的集合；
- 参数`timeout`为结构`timeval`，用来设置`select`()的等待时间。

### **struct timeval**

`struct timeval` 是C语言中用于表示时间的结构体 ，常被用于实现计时器、超时等待等时间相关功能，在头文件 `<sys/time.h>` 中定义，定义形式如下：
```c
struct timeval {
    time_t tv_sec;   /* seconds */
    suseconds_t tv_usec;  /* microseconds */
};
```
- **成员说明**：
    - `tv_sec`：类型为`time_t` ，是一个整数，存储自1970年1月1日00:00:00（UNIX时间戳起始，也叫Epoch时间 ）起经过的秒数 ，取值范围一般是 -2147483648 到 2147483647 。
    - `tv_usec`：类型为`suseconds_t` ，也是整数，记录自上一个整秒以来经过的微秒数 ，取值范围是 0 到 999999 。 
- **时间表示**：该结构体以1970 - 01 - 01 00:00:00 +0000 (UTC) 为起始点，之后的时间通过相对于此起始点流逝的秒数（`tv_sec` ）和微秒数（`tv_usec` ）来表示 ，时间精度为微秒 。 

**参数timeout取值**：
- NULL：则表示select（）没有timeout，select将一直被阻塞，直到某个文件描述符上发生了事件；
- 0：仅检测描述符集合的状态，然后立即返回，并不等待外部事件的发生；
- 特定的时间值：如果在指定的时间段里没有事件发生，select将超时返回。

### **关于fd_set结构**

```c
/* fd_set for select and pselect. */
typedef struct
{
    /* XPG4.2 requires this member name. Otherwise avoid the name
       from the global namespace. */
#if defined(__USE_XOPEN)
    __fd_mask fds_bits[__FD_SETSIZE / __NFDBITS];
#else
    __fd_mask __fds_bits[__FD_SETSIZE / __NFDBITS];
#endif
} fd_set;
/* The fd_set member is required to be an array of longs. */
typedef long int __fd_mask;
```
其实这个结构就是一个整数数组，更严格的说，是一个“**位图**”。使用位图中对应的位来表示要监视的文件描述符。
提供了一组操作fd_set的接口，来比较方便的操作位图。
```c
void FD_CLR(int fd, fd_set *set); // 用来清除描述词组set中相关fd的位
int  FD_ISSET(int fd, fd_set *set); // 用来测试描述词组set中相关fd的位是否为真
void FD_SET(int fd, fd_set *set); // 用来设置描述词组set中相关fd的位
void FD_ZERO(fd_set *set); // 用来清除描述词组set的全部位
```

### **函数返回值**：

 执行成功则返回文件描述符已就绪的个数；
- 如果返回0代表在描述词状态改变前已超过timeout时间，没有返回；
- 当有错误发生时则返回-1，错误原因存于errno，此时参数readfds，writefds，exceptfds和timeout的值变成不可预测。

### **错误值可能为**：

 EBADF：文件描述词为无效的或该文件已关闭；
- EINTR：此调用被信号所中断；
- EINVAL：参数n为负值；
- ENOMEM：核心内存不足。

### **常见的程序片段如下**：

```c
fd_set readset;
FD_SET(fd,&readset);
select(fd+1,&readset,NULL,NULL,NULL);
if(FD_ISSET(fd,readset)){...}
```

### **理解select执行过程**

理解select模型的关键在于理解fd_set，为说明方便，取fd_set长度为1字节，fd_set中的每一bit可以对应一个文件描述符fd。则1字节长的fd_set最大可以对应8个fd。
- （1）执行fd_set set; FD_ZERO(&set);则set用位表示是0000,0000。
- （2）若fd = 5,执行FD_SET(fd,&set);后set变为0001,0000(第5位置为1)
- （3）若再加入fd = 2，fd = 1,则set变为0001,0011
- （4）执行select(6,&set,0,0,0)阻塞等待
- （5）若fd = 1,fd = 2上都发生可读事件，则select返回，此时set变为0000,0011。注意：没有事件发生的fd = 5被清空。

## **Socket就绪条件**

**读就绪**
- Socket内核中，接收缓冲区中的字节数，大于等于低水位标记SO_RCVLOWAT，此时可以无阻塞的读，并且返回值大于0；
- socket TCP通信中，对端关闭连接，此时对该socket读，则返回0；
- 监听的socket上有新的连接请求；
- socket上有未处理的错误；

**写就绪**
- socket内核中，发送缓冲区中的可用字节数(发送缓冲区的空闲位置大小)，大于等于低水位标记SO_SNDLOWAT，此时可以无阻塞的写，并且返回值大于0；
- socket的写操作被关闭（close或者shutdown），对一个写操作被关闭的socket进行写操作，会触发SIGPIPE信号；
- socket使用非阻塞的connect连接成功或失败之后；
- socket上有未读取的错误；
## **select的特点**
每次调用select,都需要对输入输出参数进行重新设置。

 可监控的文件描述符个数取决于sizeof(fd_set)的值。这边服务器上sizeof(fd_set)=512，每bit表示一个文件描述符，则服务器上支持的最大文件描述符512*8=4096
 将fd加入select监控集的同时，还要**再使用一个数据结构array保存放到select监控集中的fd**。
- 一是用于select返回后，array作为数据源和fd_set进行FD_ISSET判断。
- 二是select返回后会把以前加入的但无事件发生的fd清空，则每次开始select前都要重新从array取得fd逐一加入（FD_ZERO最先），扫描array的同时取得fd最大值maxfd，用于select的第一个参数。

**备注**：
fd_set的大小可以调整，可能涉及到重新编译内核，感兴趣的同学可以自己去收集相关资料。

## **select缺点**

 - 每次调用select，都需要手动设置fd集合，从接口使用角度来说也非常不便；
- 每次调用select，都需要把fd集合从用户态拷贝到内核态，这个开销在fd很多时也很大；
- 同时每次调用select都需要在内核遍历传递进来的所有fd，这个开销在fd很多时也很大；
- select支持的文件描述符数量太小。 
## select使用示例: 检测标准输入输出
**只检测标准输入**:
```c
#include <stdio.h>
#include <unistd.h>
#include <sys/select.h>

int main() {
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(0, &read_fds);

    for (;;) {
        printf("> ");
        fflush(stdout);
        int ret = select(1, &read_fds, NULL, NULL, NULL);
        if (ret < 0) {
            perror("select");
            continue;
        }
        if (FD_ISSET(0, &read_fds)) {
            char buf[1024] = {0};
            read(0, buf, sizeof(buf) - 1);
            printf("input: %s", buf);
        } else {
            printf("error! invaild fd\n");
            continue;
        }
        FD_ZERO(&read_fds);
        FD_SET(0, &read_fds);
    }
    return 0;
}
```
**说明**:
- 当只检测文件描述符0（标准输入）时，因为输入条件只有在你有输入信息的时候，才成立，所以如果一直不输入，就会产生超时信息。 
