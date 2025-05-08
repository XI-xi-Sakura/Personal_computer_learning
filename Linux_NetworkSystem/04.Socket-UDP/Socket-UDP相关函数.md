

## socket() 函数
这个函数的作用是创建一个 Socket 文件描述符，在客户端和服务器都可以使用。
```c
#include <sys/socket.h>
int socket(int domain, int type, int protocol);
```
- **参数**：
    - `domain`：指定协议族，例如 `AF_INET` 代表 IPv4 协议，`AF_INET6` 代表 IPv6 协议。
    - `type`：指定 Socket 类型，像 `SOCK_STREAM` 是面向连接的 TCP 协议，`SOCK_DGRAM` 是无连接的 UDP 协议。
    - `protocol`：通常设为 0，由系统依据 `domain` 和 `type` 来选择合适的协议。
- **返回值**：成功时返回一个非负的 Socket 文件描述符，失败则返回 -1。

## bind() 函数
此函数用于把一个本地地址和端口号绑定到 Socket 上，一般在服务器端使用。
```c
#include <sys/socket.h>
int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
```
- **参数**：
    - `sockfd`：由 `socket()` 函数返回的 Socket 文件描述符。
    - `addr`：指向一个 `struct sockaddr` 类型的结构体指针，该结构体包含了要绑定的地址和端口信息。
    - `addrlen`：`addr` 结构体的长度。
- **返回值**：成功时返回 0，失败返回 -1。


`struct sockaddr_in` 是在网络编程中用于表示 IPv4 地址信息的结构体，它在 `netinet/in.h` 头文件中被定义。这个结构体是网络编程的基础组件，在使用 `bind`、`connect`、`sendto`、`recvfrom` 等函数时，需要使用它来指定或获取网络地址信息。

## sockaddr_in 结构体
```c
#include <netinet/in.h>

struct sockaddr_in {
    sa_family_t    sin_family; /* 地址族，必须为 AF_INET */
    in_port_t      sin_port;   /* 端口号，使用网络字节序 */
    struct in_addr sin_addr;   /* IPv4 地址结构体 */
    char           sin_zero[8];/* 填充字节，使其与 struct sockaddr 大小相同 */
};

struct in_addr {
    in_addr_t s_addr; /* 32 位 IPv4 地址，使用网络字节序 */
};
```

### 成员说明
1. **`sin_family`**：
    - 该成员指定地址族，对于 IPv4 地址，其值必须设置为 `AF_INET`。地址族用于区分不同的网络协议，例如 `AF_INET` 表示 IPv4，`AF_INET6` 表示 IPv6。
2. **`sin_port`**：
    - 此成员表示端口号，类型为 `in_port_t`。在设置端口号时，需要使用 `htons` 函数将主机字节序转换为网络字节序，以确保不同字节序的系统之间能够正确通信。例如，若要设置端口号为 8080，可以这样做：
```c
struct sockaddr_in addr;
addr.sin_port = htons(8080);
```
3. **`sin_addr`**：
    - 这是一个 `struct in_addr` 类型的结构体，用于存储 IPv4 地址。`struct in_addr` 结构体只有一个成员 `s_addr`，它是一个 32 位的整数，同样需要使用网络字节序。可以使用 `inet_addr` 或 `inet_pton` 函数将点分十进制的 IP 地址转换为网络字节序的 32 位整数。例如：
```c
struct sockaddr_in addr;
addr.sin_addr.s_addr = inet_addr("127.0.0.1");
```
4. **`sin_zero`**：
    - 这是一个长度为 8 的字符数组，用于填充结构体，使其大小与 `struct sockaddr` 相同。在使用时，通常将其初始化为 0。可以使用 `memset` 函数来完成初始化：
```c
struct sockaddr_in addr;
memset(&addr, 0, sizeof(addr));
```

 与 `struct sockaddr` 的关系
 
在网络编程的函数（如 `bind`、`connect` 等）中，通常需要传递一个 `struct sockaddr` 类型的指针作为参数。但实际使用时，我们更多地使用 `struct sockaddr_in` 来存储 IPv4 地址信息。因此，在传递参数时，需要进行强制类型转换：
```c
struct sockaddr_in server_addr;
// 初始化 server_addr...

// 绑定套接字
bind(sockfd, (struct sockaddr *)&server_addr, sizeof(server_addr));
```

## recvfrom
`recvfrom` 是一个用于接收数据报（datagram）的系统调用函数，在网络编程里应用广泛，特别是在使用 UDP 协议通信时。下面为你详细介绍它的用法、参数和返回值。

### 函数原型
```c
#include <sys/socket.h>

ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags,
                 struct sockaddr *src_addr, socklen_t *addrlen);
```

### 参数说明
- **`sockfd`**：这是一个整数类型的参数，代表已创建的套接字描述符。此描述符是通过调用 `socket` 函数得到的，它标识了用于接收数据的套接字。
- **`buf`**：是一个指向缓冲区的指针，接收到的数据会被存于该缓冲区。
- **`len`**：这是一个 `size_t` 类型的参数，指定了 `buf` 缓冲区的大小，也就是能够接收的最大字节数。
- **`flags`**：此参数是一个整数，用于指定接收数据的方式。一般设为 0 即可，表示使用默认的接收方式。也可以使用一些标志位，例如 `MSG_DONTWAIT` 实现非阻塞接收。
- **`src_addr`**：这是一个指向 `struct sockaddr` 类型的指针，用于存储发送方的地址信息。在接收数据时，该参数会被填充为发送方的地址。
- **`addrlen`**：这是一个指向 `socklen_t` 类型的指针，在调用 `recvfrom` 之前，它应包含 `src_addr` 所指向的结构体的长度；调用完成后，它会被更新为实际存储的地址信息的长度。

### 返回值
- 若调用成功，`recvfrom` 会返回接收到的字节数。
- 若遇到错误，会返回 -1，并设置 `errno` 来指示具体的错误类型。
- 若对端关闭连接（仅适用于面向连接的套接字），会返回 0。

 
## sendto
`sendto` 是网络编程里用于发送数据报（datagram）的系统调用函数，主要用于 UDP 协议通信。与 TCP 不同，UDP 是无连接的协议，`sendto` 可以在不建立连接的情况下向指定的目标地址和端口发送数据。

### 函数原型
```c
#include <sys/socket.h>

ssize_t sendto(int sockfd, const void *buf, size_t len, int flags,
               const struct sockaddr *dest_addr, socklen_t addrlen);
```

### 参数说明
- **`sockfd`**：这是一个整数类型的参数，代表已创建的套接字描述符。该描述符是通过调用 `socket` 函数得到的，它标识了用于发送数据的套接字。
- **`buf`**：是一个指向要发送数据的缓冲区的指针。
- **`len`**：这是一个 `size_t` 类型的参数，指定了要发送的数据的字节数。
- **`flags`**：此参数是一个整数，用于指定发送数据的方式。一般设为 0 即可，表示使用默认的发送方式。也可以使用一些标志位，例如 `MSG_DONTWAIT` 实现非阻塞发送。
- **`dest_addr`**：这是一个指向 `struct sockaddr` 类型的指针，用于指定数据的目标地址和端口信息。对于 IPv4 地址，通常使用 `struct sockaddr_in` 结构体来填充该信息，并进行强制类型转换。
- **`addrlen`**：这是一个 `socklen_t` 类型的参数，指定了 `dest_addr` 所指向的结构体的长度。

### 返回值
- 若调用成功，`sendto` 会返回实际发送的字节数。这个值可能小于 `len`，特别是在非阻塞模式下。
- 若遇到错误，会返回 -1，并设置 `errno` 来指示具体的错误类型。


    






    
    



