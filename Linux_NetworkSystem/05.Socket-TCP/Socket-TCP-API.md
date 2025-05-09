

### 创建套接字`socket()`
```cpp
#include <sys/socket.h>

int socket(int domain, int type, int protocol);
```
- **参数**
    - `domain`：指定协议族，如`AF_INET`（IPv4）、`AF_INET6`（IPv6）。
    - `type`：指定套接字类型，`SOCK_STREAM`表示面向连接的字节流套接字（TCP）。
    - `protocol`：通常设为0，由系统根据`domain`和`type`选择默认协议。
- **返回值**：成功时返回一个非负整数描述符，失败时返回 -1。

### 绑定地址`bind()`
```cpp
#include <sys/socket.h>

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```
- **参数**
    - `sockfd`：由`socket()`函数返回的套接字描述符。
    - `addr`：指向包含服务器地址和端口信息的结构体指针，对于IPv4是`struct sockaddr_in`，对于IPv6是`struct sockaddr_in6`。
    - `addrlen`：地址结构体的长度。
- **返回值**：成功返回0，失败返回 -1。

### 监听连接`listen()`
```cpp
#include <sys/socket.h>

int listen(int sockfd, int backlog);
```
- **参数**
    - `sockfd`：要监听的套接字描述符。
    - `backlog`：指定等待连接队列的最大长度。
- **返回值**：成功返回0，失败返回 -1。

### 接受连接`accept()`
```cpp
#include <sys/socket.h>

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
```
- **参数**
    - `sockfd`：处于监听状态的套接字描述符。
    - `addr`：用于存储客户端地址信息的结构体指针。
    - `addrlen`：是一个指向`addr`结构体长度的指针，传入时需初始化为`addr`结构体的大小，函数返回时会更新为实际存储的客户端地址长度。
- **返回值**：成功返回一个新的套接字描述符，用于与客户端通信，失败返回 -1。

### 连接服务器`connect()`
```cpp
#include <sys/socket.h>

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```
- **参数**
    - `sockfd`：客户端套接字描述符。
    - `addr`：服务器的地址结构体指针。
    - `addrlen`：服务器地址结构体的长度。
- **返回值**：成功返回0，失败返回 -1。

### 发送数据`send()`
```cpp
#include <sys/socket.h>

ssize_t send(int sockfd, const void *buf, size_t len, int flags);
```
- **参数**
    - `sockfd`：要发送数据的套接字描述符。
    - `buf`：指向要发送数据的缓冲区指针。
    - `len`：要发送数据的长度。
    - `flags`：通常设为0，用于指定一些发送选项。
- **返回值**：成功返回实际发送的字节数，失败返回 -1。

### 接收数据`recv()`
```cpp
#include <sys/socket.h>

ssize_t recv(int sockfd, void *buf, size_t len, int flags);
```
- **参数**
    - `sockfd`：接收数据的套接字描述符。
    - `buf`：用于存储接收数据的缓冲区指针。
    - `len`：缓冲区的长度。
    - `flags`：通常设为0，用于指定一些接收选项。
- **返回值**：成功返回实际接收的字节数，连接关闭时返回0，失败返回 -1。

### 关闭套接字`close()`
```cpp
#include <unistd.h>

int close(int fd);
```
- **参数**：`fd`是要关闭的套接字描述符。
- **返回值**：成功返回0，失败返回 -1。

这些函数是C++ 中实现Socket - TCP编程的基础，通过组合使用它们，可以创建可靠的网络应用程序，实现客户端与服务器之间的通信。
