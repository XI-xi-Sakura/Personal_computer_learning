# I/O 多路转接之 epoll
## epoll 初识
按照 man 手册的说法：是**为处理大批量句柄而作了改进的 poll**。
它是在 2.5.44 内核中被引进的（epoll(4) is a new API introduced in Linux kernel 2.5.44）
它几乎具备了之前所说的一切优点，被公认为 Linux2.6 下性能最好的多路 I/O 就绪通知方法。

## epoll 的相关系统调用
epoll 有 3 个相关的系统调用。

### epoll_create
C
```c
int epoll_create(int size);
```
创建一个 epoll 的句柄。
- 自从 linux2.6.8 之后，size 参数是被忽略的。
- 返回值：
	- 成功时返回指向新创建 epoll 实例的文件描述符。
	- 失败时返回 -1，并设置 errno
- 用完之后，必须调用 close()关闭。

### epoll_ctl

```c
int epoll_ctl( int epfd, 
			   int op, 
               int fd, 
               struct epoll_event *event
              );
```
epoll 的事件注册函数。
- 它不同于 select()是在监听事件时告诉内核要监听什么类型的事件，而是在这里先注册要监听的事件类型。
- 第一个参数是 epoll_create()的返回值(epoll 的句柄)。
- 第二个参数表示动作，用三个宏来表示。
- 第三个参数是需要监听的 fd。
- 第四个参数是告诉内核需要监听什么事。

第二个参数的取值：
- `EPOLL_CTL_ADD`：注册新的 fd 到 epfd 中；
- `EPOLL_CTL_MOD`：修改已经注册的 fd 的监听事件；
- `EPOLL_CTL_DEL`：从 epfd 中删除一个 fd；

`struct epoll_event` 结构如下：
```c
#include <sys/epoll.h>

typedef union epoll_data {
    void        *ptr;    // 指向用户自定义数据的指针
    int          fd;     // 注册的文件描述符
    uint32_t     u32;    // 32位整数
    uint64_t     u64;    // 64位整数
} epoll_data_t;

struct epoll_event {
    uint32_t     events;      // 事件掩码，指定监听的事件类型
    epoll_data_t data;        // 用户数据，传递给事件处理函数
} __EPOLL_PACKED;
```
events 可以是以下几个宏的集合：
- **EPOLLIN**：表示对应的文件描述符可以读(包括对端 SOCKET 正常关闭)；
- **EPOLLOUT**：表示对应的文件描述符可以写；
- **EPOLLPRI**：表示对应的文件描述符有紧急的数据可读(这里应该表示有带外数据到来)； 
- **EPOLLERR**：表示对应的文件描述符发生错误；
- **EPOLLHUP**：表示对应的文件描述符被挂断；
- **EPOLLET**：将 EPOLL 设为边缘触发(Edge Triggered)模式，这是相对于水平触发(Level Triggered)来说的。
- **EPOLLONESHOT**：只监听一次事件，当监听完这次事件之后，如果还需要继续监听这个 socket 的话，需要再次把这个 socket 加入到 EPOLL 队列里。

### epoll_wait
C
```c
int epoll_wait(	int epfd, 
			   	struct epoll_event * events, 
				int maxevents, 
				int timeout
				);
```
收集在 epoll 监控的事件中已经发送的事件。
- 参数：
	- `events` 是分配好的 epoll_event 结构体数组。
	- `maxevents` 告之内核这个 events 有多大，这个 maxevents 的值不能大于创建 epoll_create()时的 size。 
	- 参数 `timeout` 是超时时间(毫秒，0 会立即返回，-1 是永久阻塞)。 
- epoll 将会把发生的事件赋值到 events 数组中(events 不可以是空指针，内核只负责把数据复制到这个 events 数组中，不会去帮助我们在用户态中分配内存)
- 如果函数调用成功，返回**对应 I/O 上已准备好的文件描述符数目**，如返回 0 表示已超时，返回小于 0 表示函数失败。




## epoll 工作原理 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/ed94d9a6a8204642bee61d52bed4184a.png)
- 当某一进程调用epoll_create方法时，Linux内核会创建一个`eventpoll`结构体，这个结构体中有两个成员与epoll的使用方式密切相关。
```c
struct eventpoll{
   ...
    /*红黑树的根节点，这颗树中存储着所有添加到epoll中的需要监控的事件*/
    struct rb_root rbr;
    /*双链表中则存放着将要通过epoll_wait返回给用户的满足条件的事件*/
    struct list_head rdlist;
   ...
};
```
- 每一个epoll对象都有一个独立的`eventpoll`结构体，用于存放通过`epoll_ctl`方法向epoll对象中添加进来的事件。
	- 红黑树（`struct rb_root rbr`）：存储所有被监控的文件描述符（FD）及其事件掩码。插入、删除和查找操作的时间复杂度为 O (log n)，适合处理大量 FD。
	- 就绪链表（`struct list_head rdlist`）：当 FD 就绪时，内核将其从红黑树移至就绪链表。epoll_wait () 直接从该链表获取就绪 FD，无需遍历整个监控列表。
- 这些事件都会挂载在红黑树中，如此，重复添加的事件就可以通过红黑树而高效的识别出来(红黑树的插入时间效率是$lgn$，其中n为树的高度)。
- 而所有添加到epoll中的事件都会与设备(网卡)驱动程序建立回调关系，也就是说，当响应的事件发生时会调用这个回调方法。
- 这个回调方法在内核中叫`ep_poll_callback`，它会将发生的事件添加到rdlist双链表中。
- 在epoll中，对于每一个事件，都会建立一个`epitem`结构体。
```c
struct epitem{
    struct rb_node rbn;				//红黑树节点
    struct list_head  rdlLink;		//双向链表节点
    struct epoll_filefd ffd; 		//与事件句柄信息
    struct eventpoll *ep;   		//指向其所属的eventpoll对象
    struct epoll_event event; 		//期待发生的事件类型
};
```
- 当调用epoll_wait检查是否有事件发生时，只需要检查eventpoll对象中的`rdlist`双链表中是否有`epitem`元素即可。
- 如果`rdlist`不为空，则把发生的事件复制到用户态，同时将事件数量返回给用户. 这个操作的时间复杂度是O(1)。

总结一下，epoll的使用过程就是三部曲：
- 调用`epoll_create`创建一个epoll句柄；
- 调用`epoll_ctl`，将要监控的文件描述符进行注册；
- 调用`epoll_wait`，等待文件描述符就绪

**细节**：
- epoll中红黑树相当于select和poll中的辅助数组
- 具体是如何将一个红黑树节点迁移到就绪队列中的？
	- 内核中，一个数据结构也可以同时是其他数据结构节点
- 红黑树中的key值就是注册的fd。
- 为什么epoll_create 返回的是一个fd？
	- `struct file` 中 `void *private_data`指向 `struct evevtpoll`
	- Linux下一切皆文件



### epoll的优点(和select的缺点对应)
- 接口使用方便: 虽然拆分成了三个函数，但是反而使用起来更方便高效. 不需要每次循环都设置关注的文件描述符，也做到了输入输出参数分离开
- 数据拷贝轻量: **只在合适的时候调用**`EPOLL_CTL_ADD`**将文件描述符结构拷贝到内核中**，这个操作并不频繁(而select/poll都是每次循环都要进行拷贝)
- 事件回调机制: 避免使用遍历，而是使用回调函数的方式，将就绪的文件描述符结构加入到就绪队列中，epoll_wait返回直接访问就绪队列就知道哪些文件描述符就绪. 这个操作时间复杂度O(1). 即使文件描述符数目很多，效率也不会受到影响.
- 没有数量限制: 文件描述符数目无上限.

## 注意!!
网上有些博客说，epoll中使用了内存映射机制
- 内存映射机制: 内核直接将就绪队列通过mmap的方式映射到用户态. 避免了拷贝内存这样的额外性能开销.

这种说法是不准确的. 我们定义的struct epoll_event是我们在用户空间中分配好的内存. 势必还是需要将内核的数据拷贝到这个用户空间的内存中的.

| 机制 | 优点 | 缺点 |
| ---- | ---- | ---- |
| select | <li>跨平台性好，几乎支持所有主流操作系统</li><li>接口简单，易于理解和使用</li> | <li>文件描述符集合操作繁琐，每次调用需重新设置</li><li>线性扫描检查就绪状态，大量文件描述符时性能差，时间复杂度O(n)</li><li>需将文件描述符数组在用户空间和内核空间拷贝，开销大</li><li>仅返回可读文件描述符个数，需遍历确定具体就绪描述符</li><li>文件描述符数量受限，一般为1024</li> |
| poll | <li>跨平台性好，被多数操作系统支持</li><li>无文件描述符集合大小限制，比select灵活</li><li>使用链表管理文件描述符，遍历开销相对select较小</li> | <li>大量文件描述符时，仍需在用户空间和内核空间复制所有文件描述符，性能有限</li><li>通过轮询检查文件描述符就绪状态，大量未就绪描述符时效率低</li> |
| epoll | <li>高性能，适用于大量并发连接场景，使用红黑树管理文件描述符，`epoll_wait`获取就绪事件时间复杂度O(1)</li><li>事件通知机制优，文件描述符就绪时内核主动唤醒，无需轮询</li><li>操作便捷，内核维护文件描述符集合，用户通过`epoll_ctl`操作</li><li>触发模式灵活，支持水平触发（LT）和边缘触发（ET）</li><li>返回信息精准，内核仅返回有I/O事件的文件描述符</li><li>理论上文件描述符集合大小无上限</li> | <li>仅Linux平台可用，不具备跨平台性</li><li>接口更底层，涉及概念多，学习和开发成本高</li><li>文件描述符存储在内核空间，状态变化和调试相对麻烦；频繁修改描述符状态时，系统调用`epoll_ctl`可能降低效率</li> | 

## epoll工作方式
### 例子
你正在吃鸡，眼看进入了决赛圈，你妈饭做好了，喊你吃饭的时候有两种方式：
1. 如果你妈喊你一次，你没动，那么你妈会继续喊你第二次，第三次…(亲妈，水平触发)
2. 如果你妈喊你一次，你没动，你妈就不管你了(后妈，边缘触发)

epoll有2种工作方式-水平触发(`LT`)和边缘触发(`ET`)

假如有这样一个例子:
- 我们已经把一个tcp socket添加到epoll描述符
- 这个时候socket的另一端被写入了2KB的数据
- 调用epoll_wait，并且它会返回. 说明它已经准备好读取操作
- 然后调用read，只读了1KB的数据
- 继续调用epoll_wait……

### 水平触发Level Triggered工作模式
epoll默认状态下就是LT工作模式.
- 当epoll检测到socket上事件就绪的时候，可以不立刻进行处理. 或者只处理一部分.
- 如上面的例子，由于只读了1K数据，缓冲区中还剩1K数据，在第二次调用epoll_wait时，epoll_wait仍然会立刻返回并通知socket读事件就绪.
- 直到缓冲区上所有的数据都被处理完，epoll_wait才不会立刻返回.
- 支持阻塞读写和非阻塞读写

### 边缘触发Edge Triggered工作模式
如果我们在第1步将socket添加到epoll描述符的时候使用了EPOLLET标志，epoll入ET工作模式.
- 当epoll检测到socket上事件就绪时，必须立刻处理.
- 如上面的例子，虽然只读了1K的数据，缓冲区还剩1K的数据，在第二次调用epoll_wait时，ET模式下文件描述符上的事件就绪后，只有一次处理机会.
- 也就是说，**ET模式下，文件描述符上的事件就绪后，只有一次处理机会**.
- ET的性能比LT性能更高(epoll_wait返回的次数少了很多). Nginx默认采用ET模式使用epoll.
- 只支持非阻塞的读写

select和poll其实是工作在LT模式下. epoll既可以支持LT，也可以支持ET.

### 对比LT和ET
LT是epoll的默认行为.

使用ET能够减少epoll触发的次数. 但是代价就是**强逼着程序猿一次响应就绪过程中把所有的数据都处理就绪**.

相当于一个文件描述符就绪之后，不会反复被提示就绪，看起来就比LT更高效一些.

提示在LT情况下如果也能做到每次就绪的文件描述符都立刻处理，不让这个就绪被重复提示的话，其实性能也是一样的.

另一方面，ET的代码复杂程度更高了.

## 理解ET模式和非阻塞文件描述符
使用ET模式的epoll，需要将文件描述设置为非阻塞. 这个不是接口上的要求，而是”实践”上的要求.

假设这样的场景: 服务器接收到一个10k请求，会向客户端返回一个应答数据. 如果接收不到客户端的数据就会阻塞在read调用上. 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/563bdd267199446c92a4b8cfd921fcc9.png)
如果服务端写的代码是阻塞式的read,并且一次只read1k数据的话(read不能保证一次就把所有的数据都读出来,参考man手册的说明,可能被信号打断),剩下的9k数据就会待在缓冲区中.

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/73ea87fe36d64a30acbfed7eba11b911.png)
此时由于epoll是ET模式,并不会认为文件描述符读就绪.epoll_wait就不会再次返回. 剩下的9k数据会一直在缓冲区中.直到下一次客户端再给服务器写数据. epoll_wait 才能返回
但是问题来了.
- 服务器只读到1k个数据,要10k读完才会给客户端返回响应数据.
- 客户端要读到服务器的响应,才会发送下一个请求
- 客户端发送了下一个请求,epoll_wait才会返回,才能去读缓冲区中剩余的数据.

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/6f6eeb4df58b4af99c5aa32ad9328026.png)
所以,为了解决上述问题(阻塞read不一定能一下把完整的请求读完),于是就可以使用非阻塞轮训的方式来读缓冲区,保证一定能把完整的请求都读出来.

而如果是LT没这个问题.只要缓冲区中的数据没读完,就能够让epoll_wait返回文件描述符读就绪。

## epoll示例: epoll服务器(LT模式)
**tcp_epoll_server.hpp**
```cpp
/*************************************************************************
// 封装一个Epoll服务器，只考虑读就绪的情况
*************************************************************************/
#pragma once
#include <vector>
#include <functional>
#include <sys/epoll.h>
#include "tcp_socket.hpp"

typedef std::function<void(const std::string&, std::string* resp)> Handler;

class Epoll {
public:
    Epoll() {
        epoll_fd_ = epoll_create(10);
    }
    ~Epoll() {
        close(epoll_fd_);
    }
    bool Add(const TcpSocket& sock) const {
        int fd = sock.GetFd();
        printf("Epoll Add fd = %d\n", fd);
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        if (ret < 0) {
            perror("epoll_ctl ADD");
            return false;
        }
        return true;
    }
    bool Del(const TcpSocket& sock) const {
        int fd = sock.GetFd();
        printf("Epoll Del fd = %d\n", fd);
        int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL);
        if (ret < 0) {
            perror("epoll_ctl DEL");
            return false;
        }
        return true;
    }
    bool Wait(std::vector<TcpSocket*>* output) const {
        output->clear();
        epoll_event events[1000];
        int nfds = epoll_wait(epoll_fd_, events, sizeof(events) / sizeof(events[0]), -1);
        if (nfds < 0) {
            perror("epoll_wait");
            return false;
        }
        // [注意！]此处必须循环到 nfds，不能多循环
        for (int i = 0; i < nfds; ++i) {
            TcpSocket sock(events[i].data.fd);
            output->push_back(&sock);
        }
        return true;
    }
private:
    int epoll_fd_;
};

class TcpEpollServer {
public:
    TcpEpollServer(const std::string& ip, uint16_t port) : ip_(ip), port_(port) {}
    bool Start(Handler handler) {
        // 1. 创建listen_sock
        TcpSocket listen_sock;
        CHECK_RET(listen_sock.Socket());
        // 2. 绑定
        CHECK_RET(listen_sock.Bind(ip_, port_));
        // 3. 监听
        CHECK_RET(listen_sock.Listen(5));
        // 4. 创建Epoll对象，并将listen_sock加入进去
        Epoll epoll;
        epoll.Add(listen_sock);
        // 5. 进入事件循环
        for (;;) {
            std::vector<TcpSocket*> output;
            if (!epoll.Wait(&output)) {
                continue;
            }
            // 7. 根据就绪的文件描述符的种类决定如何处理
            for (size_t i = 0; i < output.size(); ++i) {
                if (output[i]->GetFd() == listen_sock.GetFd()) {
                    // 如果是listen_sock，就调用accept
                    TcpSocket new_sock;
                    listen_sock.Accept(&new_sock);
                    epoll.Add(new_sock);
                } else {
                    // 如果是new_sock，就进行一次读写
                    std::string req, resp;
                    bool ret = output[i]->Recv(&req);
                    if (!ret) {
                        // [注意！]需要把不用的socket关闭
                        // 先后顺序别搞反。不过在epoll删除的时候其实就已经关闭socket了
                        epoll.Del(output[i]);
                        output[i]->Close();
                        continue;
                    }
                    handler(req, &resp);
                    output[i]->Send(resp);
                }
            }
        }
        return true;
    }
private:
    std::string ip_;
    uint16_t port_;
};
```


## epoll示例: epoll服务器(ET模式)
基于LT版本稍作修改即可
1. 修改 `tcp_socket.hpp`, 新增非阻塞读和非阻塞写接口
2. 对于 `accept` 返回的 `new_sock` 加上 `EPOLLET` 这样的选项

**注意**：此代码暂时未考虑 `listen_sock` ET的情况。如果将 `listen_sock` 设为ET, 则需要非阻塞轮询的方式 `accept`。否则会导致同一时刻大量的客户端同时连接的时候，只能 `accept` 一次的问题。

**tcp_socket.hpp**
```cpp
// 以下代码添加在TcpSocket类中
// 非阻塞IO接口
bool SetNoBlock() {
    int fl = fcntl(fd_, F_GETFL);
    if (fl < 0) {
        perror("fcntl F_GETFL");
        return false;
    }
    int ret = fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
    if (ret < 0) {
        perror("fcntl F_SETFL");
        return false;
    }
    return true;
}
bool RecvNoBlock(std::string* buf) const {
    // 对于非阻塞IO读数据，如果TCP接收缓冲区为空，就会返回错误需要重试
    // 错误码为EAGAIN或者EWOULDBLOCK
    // 这种写法其实不算特别严谨(没有考虑缓冲区问题)，读到当前不算很长的数据长度，就退出循环
    buf->clear();
    char tmp[1024 * 10] = {0};
    for (;;) {
        ssize_t read_size = recv(fd_, tmp, sizeof(tmp) - 1, 0);
        if (read_size < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("recv");
            return false;
        } else if (read_size == 0) {
            return false;
        }
        tmp[read_size] = '\0';
        if (read_size < (ssize_t)sizeof(tmp) - 1) {
            break;
        }
        buf->append(tmp);
    }
    return true;
}
bool SendNoBlock(const std::string& buf) const {
    // 对于非阻塞IO的写入，如果TCP的发送缓冲区已经满了，就会出现出错的情况
    // 此时的错误码是EAGAIN或者EWOULDBLOCK，这种情况下不应放弃治疗
    // 而要进行重试
    ssize_t cur_pos = 0; // 记录当前写到的位置
    ssize_t left_size = buf.size();
    for (;;) {
        ssize_t write_size = send(fd_, buf.data() + cur_pos, left_size, 0);
        if (write_size < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            perror("send");
            return false;
        }
        cur_pos += write_size;
        left_size -= write_size;
        if (left_size <= 0) {
            break;
        }
    }
    return true;
}
```

**tcp_epoll_server.hpp**
```cpp
/*************************************************************************
// 修改点：Epoll_ET服务器
// 1. 对于new_sock，加上EPOLLET标记
// 2. 修改listen_sock如果设置成ET，就需要非阻塞调用accept了
// 稍微麻烦一点，此处暂时不实现
*************************************************************************/
#pragma once
#include <vector>
#include <functional>
#include <sys/epoll.h>
#include "tcp_socket.hpp"

typedef std::function<void(const std::string&, std::string* resp)> Handler;

class Epoll {
public:
    Epoll() {
        epoll_fd_ = epoll_create(10);
    }
    ~Epoll() {
        close(epoll_fd_);
    }
    bool Add(const TcpSocket& sock, bool epoll_et = false) const {
        int fd = sock.GetFd();
        printf("Epoll Add fd = %d\n", fd);
        epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = fd;
        if (epoll_et) {
            ev.events |= EPOLLET;
        }
        int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev);
        if (ret < 0) {
            perror("epoll_ctl ADD");
            return false;
        }
        return true;
    }
    bool Del(const TcpSocket& sock) const {
        int fd = sock.GetFd();
        printf("Epoll Del fd = %d\n", fd);
        int ret = epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, NULL);
        if (ret < 0) {
            perror("epoll_ctl DEL");
            return false;
        }
        return true;
    }
    bool Wait(std::vector<TcpSocket*>* output) const {
        output->clear();
        epoll_event events[1000];
        int nfds = epoll_wait(epoll_fd_, events, sizeof(events) / sizeof(events[0]), -1);
        if (nfds < 0) {
            perror("epoll_wait");
            return false;
        }
        // [注意！]此处必须循环到 nfds，不能多循环
        for (int i = 0; i < nfds; ++i) {
            TcpSocket sock(events[i].data.fd);
            output->push_back(&sock);
        }
        return true;
    }
private:
    int epoll_fd_;
};

class TcpEpollServer {
public:
    TcpEpollServer(const std::string& ip, uint16_t port) : ip_(ip), port_(port) {}
    bool Start(Handler handler) {
        // 1. 创建listen_sock
        TcpSocket listen_sock;
        CHECK_RET(listen_sock.Socket());
        // 2. 绑定
        CHECK_RET(listen_sock.Bind(ip_, port_));
        // 3. 监听
        CHECK_RET(listen_sock.Listen(5));
        // 4. 创建Epoll对象，并将listen_sock加入进去
        Epoll epoll;
        epoll.Add(listen_sock);
        // 5. 进入事件循环
        for (;;) {
            std::vector<TcpSocket*> output;
            if (!epoll.Wait(&output)) {
                continue;
            }
            // 7. 根据就绪的文件描述符的种类决定如何处理
            for (size_t i = 0; i < output.size(); ++i) {
                if (output[i]->GetFd() == listen_sock.GetFd()) {
                    // 如果是listen_sock，就调用accept
                    TcpSocket new_sock;
                    listen_sock.Accept(&new_sock);
                    epoll.Add(new_sock, true);
                } else {
                    // 如果是new_sock，就进行一次读写
                    std::string req, resp;
                    bool ret = output[i]->RecvNoBlock(&req);
                    if (!ret) {
                        // [注意！]需要把不用的socket关闭
                        // 先后顺序别搞反。不过在epoll删除的时候其实就已经关闭socket了
                        epoll.Del(output[i]);
                        output[i]->Close();
                        continue;
                    }
                    handler(req, &resp);
                    output[i]->SendNoBlock(resp);
                    printf("client %d req: %s, resp: %s\n", output[i]->GetFd(), req.c_str(), resp.c_str());
                }
            }
        }
        return true;
    }
private:
    std::string ip_;
    uint16_t port_;
};
```
