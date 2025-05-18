# 多路转接poll


在Linux网络编程中，`poll` 是一种I/O多路复用机制，允许程序在单个线程内同时监视多个文件描述符（如socket描述符）的状态变化，高效处理多个并发连接 。

以下是详细介绍：
## 1. 与select的关联及改进
`poll` 是对 `select` 模型的优化。`select` 存在一些局限性，如能监视的文件描述符数量受 `fd_set` 大小限制，且每次调用需手动设置输入输出参数并重置关注的事件 。

`poll` 解决了这些问题，理论上能描述的文件描述符个数无上限，还将输入输出型参数分离，使用时无需重新设置。 
## 2. 函数接口
`poll` 函数原型为：`int poll(struct pollfd *fds, nfds_t nfds, int timeout);`
- **参数 `fds`**：是一个指向 `pollfd` 结构体数组的指针。`pollfd` 结构体用于指定需要监视的文件描述符及其事件，定义如下：
```
struct pollfd {
    int fd;         /* 文件描述符 */
    short events;   /* 用户感兴趣的事件集合，如POLLIN（可读）、POLLOUT（可写）、POLLERR(错误)等 ，通过位或运算组合多个事件 */
    short revents;  /* 内核在调用返回时设置的实际发生的事件集合 */
};
```
- **参数 `nfds`**：表示 `fds` 数组中元素的数量，即需要监视的文件描述符的数量 。
- **参数 `timeout`**：指定 `poll` 函数等待事件发生前的超时时间（单位为毫秒） 。若 `timeout` 为 -1 ，`poll` 将无限期等待；若为 0 ，`poll` 将立即返回，不阻塞；若为正数，`poll` 将等待指定毫秒数 。
- 以下是以表格形式呈现的 `poll` 事件选项说明：



| 事件宏         | 值   | 描述                                                                 |
|----------------|------|----------------------------------------------------------------------|
| `POLLIN`       | 0x001 | 数据可读（包括普通数据、优先级带数据等）。                           |
| `POLLRDNORM`   | 0x001 | 普通数据可读（Linux 下与 `POLLIN` 等效）。                           |
| `POLLRDBAND`   | 0x004 | 优先级带数据可读（Linux 支持有限）。                                 |
| `POLLOUT`      | 0x004 | 数据可写（发送缓冲区可用空间充足）。                                 |
| `POLLWRNORM`   | 0x004 | 普通数据可写（Linux 下与 `POLLOUT` 等效）。                          |
| `POLLWRBAND`   | 0x008 | 优先级带数据可写（Linux 支持有限）。                                 |
| `POLLRDHUP`    | 0x2000 | 对端关闭连接（TCP FIN 包或半关闭状态）。                            |
| `POLLERR`      | 0x008 | 发生错误（如连接重置、超时等），需通过 `getsockopt(SO_ERROR)` 读取。 |
| `POLLHUP`      | 0x010 | 挂起（如管道写端关闭，或套接字连接断开）。                          |
| `POLLNVAL`     | 0x020 | 文件描述符无效（如未打开或已关闭的 FD）。                           |






## 3. 工作原理
调用 `poll` 函数后，程序会阻塞等待，直到以下情况之一发生：
- 有文件描述符上发生了用户感兴趣的事件（如可读、可写、异常等 ，对应 `revents` 字段被内核设置）。
- 等待超时（达到 `timeout` 设置的时间 ）。
- 收到信号中断 。

当 `poll` 函数返回时，通过检查 `fds` 数组中每个元素的 `revents` 域，可确定哪些文件描述符上发生了事件，并据此进行相应处理 。例如，若 `revents` 被设置为 `POLLIN | POLLERR` ，表示同时发生了可读事件和错误事件 。 
### 4. 特点
- **优点**：
    - **灵活性高**：允许用户指定多个感兴趣的事件，可根据实际发生的事件灵活处理 。
    - **突破数量限制**：使用数组存储文件描述符，理论上能监视的文件描述符个数无上限，解决了 `select` 中文件描述符数量受限问题 。
    - **参数易用**：将输入输出型参数分离，无需像 `select` 那样每次调用都重新设置参数 。
- **缺点**：
    - **拷贝开销大**：作为系统调用，每次调用都要将文件描述符数组从用户态拷贝到内核态，存在较大开销 。
    - **遍历效率低**：每次调用 `poll` 都需遍历所有要监视的文件描述符来检测事件，当文件描述符数量很多时，开销剧增，性能和效率会降低 。 
### 5. 使用示例
以下是一个简单示例，使用 `poll` 函数监视标准输入（文件描述符0）和普通文件读取事件 ：
```c
#include <stdio.h>
#include <stdlib.h>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>

#define MAX_EVENTS 2

int main() {
    struct pollfd fds[MAX_EVENTS];
    int fd;
    char buf[1024];

    // 监视标准输入（文件描述符0）的可读事件
    fds[0].fd = 0;
    fds[0].events = POLLIN;

    // 打开一个普通文件用于监视
    fd = open("test.txt", O_RDONLY);
    if (fd < 0) {
        perror("open");
        return EXIT_FAILURE;
    }
    fds[1].fd = fd;
    fds[1].events = POLLIN;

    // 调用poll函数，等待事件发生，设置超时时间为5000毫秒
    int n = poll(fds, MAX_EVENTS, 5000);
    if (n < 0) {
        perror("poll");
        close(fd);
        return EXIT_FAILURE;
    } else if (n == 0) {
        printf("Poll timed out.\n");
    } else {
        // 检查标准输入是否有事件发生
        if (fds[0].revents & POLLIN) {
            ssize_t bytes_read = read(0, buf, sizeof(buf) - 1);
            if (bytes_read > 0) {
                buf[bytes_read] = '\0';
                printf("Standard input: %s", buf);
            }
        }
        // 检查文件描述符对应的文件是否有事件发生
        if (fds[1].revents & POLLIN) {
            ssize_t bytes_read = read(fd, buf, sizeof(buf) - 1);
            if (bytes_read > 0) {
                buf[bytes_read] = '\0';
                printf("File: %s", buf);
            }
        }
    }
    close(fd);
    return EXIT_SUCCESS;
}
```
在使用 `poll` 函数时，需确保 `fds` 数组中的所有文件描述符在调用前有效 。

`poll` 函数返回后，`fds` 数组中的 `revents` 字段会被内核设置以反映实际发生的事件，程序需检查这些字段来确定事件发生的文件描述符 。

此外，`poll` 函数返回时不会清空 `fds` 数组，可连续多次调用，无需重新初始化（除非要监视不同文件描述符或事件 ）。 尽管 `poll` 在灵活性和超时控制上有优势，但处理大规模连接时可能出现性能瓶颈，此时可考虑使用更高效的 `epoll` 机制 。 

## poll 的优点
不同于select使用三个位图来表示三个fdset的方式，poll使用一个pollfd的指针实现.
 - pollfd结构包含了要监视的event和发生的event，不再使用select“参数-值”传
递的方式.接口使用比select更方便.
 - poll并没有最大数量限制(但是数量过大后性能也是会下降).
## poll 的缺点
- poll 中监听的文件描述符数目增多时和select函数一样，poll返回后，需要轮询pollfd来获取就绪的描述符.
- 每次调用poll都需要把大量的pollfd结构从用户态拷贝到内核中.
- 同时连接的大量客户端在一时刻可能只有很少的处于就绪状态,因此随着监视
的描述符数量的增长,其效率也会线性下降