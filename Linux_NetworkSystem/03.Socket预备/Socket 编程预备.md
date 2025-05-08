# Socket编程预备
## 1. 源IP地址和目的IP地址
- IP在网络中，用来**标识主机的唯一性**
- 注意：后面我们会讲IP的分类，后面会详细阐述IP的特点

但是这里要思考一个问题：数据传输到主机是目的吗？不是的。因为数据是给人用的。比如：聊天是人在聊天，下载是人在下载，浏览网页是人在浏览。

但是人是怎么看到聊天信息的呢？怎么执行下载任务呢？怎么浏览网页信息呢？通过启动的qq，迅雷，浏览器。

而启动的qq，迅雷，浏览器都是进程。换句话说，进程是人在系统中的代表，只要把数据给进程，人就相当于拿到了数据。

所以：**数据传输到主机不是目的**，而是手段。**到达主机内部，在交给主机内的进程，才是目的**。

但是系统中，同时会存在非常多的进程，当数据到达目标主机之后，怎么转发给目标进程？这就要在网络的背景下，在系统中，标识主机的唯一性。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/0a96b87db7c849839543bcb16c705c3a.png)

## 2. 认识端口号
端口号(port)是传输层协议的内容。
- 端口号是一个2字节16位的整数；
- 端口号用来标识一个进程，告诉操作系统，当前的这个数据要交给哪一个进程来处理；
- I**P地址 + 端口号能够标识网络上的某一台主机的某一个进程**；
- **一个端口号只能被一个进程占用**。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/45046f977ebf48c0bbe1b3c61a432040.png)

### 端口号范围划分
- 0 - 1023：知名端口号，HTTP，FTP，SSH等这些广为使用的应用层协议，他们的端口号都是固定的。
- 1024 - 65535：操作系统动态分配的端口号，客户端程序的端口号，就是由操作系统从这个范围分配的。

### 理解“端口号”和“进程ID”
pid表示唯一一个进程；此处我们的端口号也是唯一表示一个进程。那么这两者之间是怎样的关系？

一个进程可以绑定多个端口号；但是一个端口号不能被多个进程绑定；

- 进程ID属于系统概念，技术上也具有唯一性，确实可以用来标识唯一的一个进程，但是这样做，会让系统进程管理和网络强耦合，实际设计的时候，并没有选择这样做。


传输层协议(TCP和UDP)的数据段中有两个端口号，分别叫做**源端口号**和**目的端口号**。就是在描述“数据是谁发的，要发给谁”；

### 理解socket
- 综上，`IP`地址用来标识互联网中唯一的一台主机，`port`用来标识该主机上唯一的一个网络进程
- `IP+Port`就能表示**互联网中唯一的一个进程**
- 所以，通信的时候，本质是两个互联网进程代表人来进行通信，{srcIp, srcPort, dstIp, dstPort}这样的4元组就能标识互联网中唯二的两个进程
- 所以，**网络通信的本质，也是进程间通信**
- 我们把ip+port叫做套接字`socket`



## 3. 传输层的典型代表
- 如果我们了解了系统，也了解了网络协议栈，我们就会清楚，传输层是属于内核的，那么我们要通过网络协议栈进行通信，必定调用的是传输层提供的系统调用，来进行的网络通信。 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/92ccd710e0dd4e6e9998ab5697a21479.png)
### 认识TCP协议
此处我们先对TCP(Transmission Control Protocol 传输控制协议)有一个直观的认识；后面我们再详细讨论TCP的一些细节问题。
- 传输层协议
- 有连接
- 可靠传输
- 面向字节流

### 认识UDP协议
此处我们也是对UDP(User Datagram Protocol 用户数据报协议)有一个直观的认识；后面再详细讨论。
- 传输层协议
- 无连接
- 不可靠传输
- 面向数据报

因为我们暂时还没有深入了解tcp、udp协议，此处只做了解即可

## 4. 网络字节序
我们已经知道，内存中的多字节数据相对于内存地址有大端和小端之分，磁盘文件中的多字节数据相对于文件中的偏移地址也有大端小端之分，网络数据流同样有大端小端之分。那么如何定义网络数据流的地址呢？
- 发送主机通常将发送缓冲区中的数据按内存地址从低到高的顺序发出；
- 接收主机把从网络上接到的字节依次保存在接收缓冲区中，也是按内存地址从低到高的顺序保存；
- 因此，**网络数据流的地址应这样规定：先发出的数据是低地址，后发出的数据是高地址**。
- TCP/IP协议规定，网络数据流应采用**大端字节序**，即低地址高字节。
- 不管这台主机是大端机还是小端机，都会按照这个TCP/IP规定的网络字节序来发送/接收数据；
- 如果当前发送主机是小端，就需要先将数据转成大端；否则就忽略，直接发送即可；

将0x1234abcd写入到以0x0000开始的内存中，则结果为
|        | big - endian | little - endian |
| ------ | ------------ | --------------- |
| 0x0000 | 0x12         | 0xcd            |
| 0x0001 | 0x23         | 0xab            |
| 0x0002 | 0xab         | 0x34            |
| 0x0003 | 0xcd         | 0x12            |

为使网络程序具有可移植性，使同样的C代码在大端和小端计算机上编译后都能正常运行，可以调用以下库函数做网络字节序和主机字节序的转换。
```c
#include <arpa/inet.h>
uint32_t htonl(uint32_t hostlong);
uint16_t htons(uint16_t hostshort);
uint32_t ntohl(uint32_t netlong);
uint16_t ntohs(uint16_t netshort);
```
- 这些函数名很好记，h表示host，n表示network，l表示32位长整数，s表示16位短整数。
- 例如htonl表示将32位的长整数从主机字节序转换为网络字节序，例如将IP地址转换后准备发送。
- 如果主机是小端字节序，这些函数将参数做相应的大小端转换然后返回；
- 如果主机是大端字节序，这些函数不做转换，将参数原封不动地返回。

## 5. socket编程接口

### socket常见API
```c
// 创建socket文件描述符（TCP/UDP，客户端 + 服务器）
int socket(int domain, int type, int protocol);

// 绑定端口号（TCP/UDP，服务器）
int bind(int socket, const struct sockaddr *address, socklen_t address_len);

// 开始监听socket（TCP，服务器）
int listen(int socket, int backlog);

// 接收请求（TCP，服务器）
int accept(int socket, struct sockaddr* address, socklen_t* address_len);

// 建立连接（TCP，客户端）
int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```

### sockaddr结构
socket API是一层抽象的网络编程接口，适用于各种底层网络协议，如IPv4、IPv6，以及后面要讲的UNIX Domain Socket。然而，各种网络协议的地址格式并不相同。 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/dc642d3c68c14e21b48da1af1da129af.png)
- IPv4和IPv6的地址格式定义在netinet/in.h中，IPv4地址用sockaddr_in结构体表示，包括16位地址类型，16位端口号和32位IP地址。
- IPv4、IPv6地址类型分别定义为常数`AF_INET`、`AF_INET6`。这样，只要取得某种`sockaddr`结构体的首地址，不需要知道具体是哪种类型的sockaddr结构体，就可以根据地址类型字段确定结构体中的内容。
- socket API可以都用struct sockaddr *类型表示，在使用的时候需要强制转化成`sockaddr_in`；这样的好处是程序的通用性，可以接收IPv4，IPv6，以及UNIX Domain Socket各种类型的sockaddr结构体指针做为参数；

### sockaddr结构
```c
148 struct sockaddr
149 {
150     __SOCKADDR_COMMON (sa_); /* Common data: address family and length. */
151     char sa_data[14]; /* Address data. */
152 };
```

### sockaddr_in结构
```c
237 /* Structure describing an Internet socket address. */
238 struct sockaddr_in
239 {
240     __SOCKADDR_COMMON (sin_);
241     in_port_t sin_port;   /* Port number. */
242     struct in_addr sin_addr; /* Internet address. */
243 
244     /* Pad to size of `struct sockaddr'. */
245     unsigned char sin_zero[sizeof (struct sockaddr) -
246                            __SOCKADDR_COMMON_SIZE -
247                            sizeof (in_port_t) -
248                            sizeof (struct in_addr)];
249 };
```
虽然socket api的接口是sockaddr，但是我们真正在基于IPv4编程时，使用的数据结构是sockaddr_in；这个结构里主要有三部分信息：地址类型，端口号，IP地址。

### in_addr结构
```c
30  /* Internet address. */
31  typedef uint32_t in_addr_t;
32  struct in_addr
33  {
34      in_addr_t s_addr;
35  };
```
in_addr用来表示一个IPv4的IP地址. 其实就是一个32位的整数; 
