# system V 消息队列
在Linux系统中，System V消息队列是一种进程间通信（IPC）机制。

它允许不同的进程通过发送和接收消息来进行通信，这种通信方式是**基于消息**的，消息是一个有类型的数据块，进程可以**根据消息类型来区分不同的消息**。

消息队列提供了一种异步通信的方式，发送进程将消息发送到消息队列后可继续执行，而接收进程可以在合适的时候从消息队列中获取消息。

## 相关系统调用

### 创建或打开消息队列


```c
int msgget(key_t key, int msgflg);
```
返回 `msqid`（消息队列标识符）

### 发送消息到消息队列

```c
int msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
```
   - **参数介绍**：
        - `const void *msgp`：指向发送的消息
        - `size_t msgsz`：消息数据部分的长度
        - `int msgflg`：用于控制消息发送的标志位
            - `0`：表示如果消息队列满了，`msgsnd`将阻塞，直到有足够空间发送
            - `IPC_NOWAIT`：若消息队列满了，不阻塞，立即返回并设置`errno`
3. **从消息队列接收消息**
```c
ssize_t msgrcv(int msqid, void *msgp, size_t msgsz, long msgtyp, int msgflg);
```
   - **参数介绍**：
        - `size_t msgsz`：接收消息数据部分的最大字节数
        - `msgtyp`：用于指定要接收的消息类型
            - `>0`：接收消息队列中消息类型为`msgtyp`的第一个消息
            - `=0`：接收消息队列中消息类型最小的第一个消息
            - `<0`：接收消息队列中消息类型小于等于`-msgtyp`绝对值的最小消息类型的第一个消息
        - `msgflg`：用于控制消息接收行为的标志位
            - `0`：表示如果没有符合条件的消息，`msgrcv`将阻塞，直到有符合条件的消息到达
            - `IPC_NOWAIT`：若无消息，函数不阻塞，直接返回并设置`errno`
            - `MSG_NOERROR`：当接收消息长度大于`msgsz`，不会截断信息，而直接接收完整消息并重新设置`msgsz`

## 控制消息队列属性

```c
int msgctl(int msqid, int cmd, struct msqid_ds *buf);
```
   - **参数介绍**：
        - `cmd`：执行命令
            - `IPC_RMID`：删除
            - `IPC_SET`：设置属性
            - `IPC_STAT`：获取属性
        - `buf`：指向`msqid_ds`结构的指针，用于存储和设置消息队列的属性信息

#### 消息队列的结构和属性
每个消息队列在内核中有一个对应的`msqid_ds`结构来描述它的属性。

消息本身在消息队列中按照消息类型（mtype）和发送时间等因素进行排序存储的，消息类型是由发送进程定义的一个长整型的值，接收进程可以根据消息类型来选择接收消息。

## 使用ipcs -q查询消息队列

```c
struct msgbuf {
    long mtype;
    char mtext[100];
};
```


