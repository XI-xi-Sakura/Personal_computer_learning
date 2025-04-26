## 1. 理解"文件"

### 1-1狭义理解

 磁盘在磁盘里
- 磁盘是永久性存储介质，因此文件在磁盘上的存储是永久性的
- 磁盘是外设（即是输出设备也是输入设备）
- 磁盘上的文件（本质是对文件的所有操作），都是对外设的输入和输出 **简称IO**

### 1-2广义理解

 **Linux下一切皆文件**（

### 1-3文件操作的归类认知

 首先明确一点：即使是0KB的空文件，是占据磁盘空间的
- 文件是文件属性和文件内容的集合（**文件 = 属性+内容**）
- 所有的文件操作本质是文件内容操作和文件属性操作

### 1-4系统角度

 对文件的操作本质是**进程对文件的操作**
- 磁盘的管理者是操作系统
- 文件的读写本质不是通过C语言/C++的库函数来操作的（这些库函数只是为用户提供方便），而是通过文件相关的系统调用接口来实现的

## 2. 回顾C文件接口

### 2-1 hello.c打开文件

```c
#include <stdio.h>
int main()
{
    FILE *fp = fopen("myfile", "w");
    if(!fp){
        printf("fopen error!\n");
    }
    while(1);
    fclose(fp);
    return 0;
}
```
- 打开的myfile文件在哪个路径下?
- 在程序的当前路径下，那系统怎么知道程序的当前路径在哪里呢?
- 可以使用`ls /proc/[进程id] -l` 命令查看当前正在运行进程的信息:

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/f249443b3a3e4b15b76ae5d0092fccc3.png)

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/3a73fba09420476ab88b80e752cb4460.png)

其中:
- cwd: 指向当前进程运行目录的一个符号链接。
- exe: 指向启动当前进程的可执行文件（完整路径）的符号链接。

**打开文件，本质是进程打开**，所以，进程知道自己在哪里，即便文件不带路径，进程也知道。由此OS就能知道要创建的文件放在哪里。

### 2-2 hello.c写文件

```c
#include <stdio.h>
#include <string.h>
int main()
{
    FILE *fp = fopen("myfile", "w");
    if(!fp){
        printf("fopen error!\n");
    }
    const char *msg = "hello bit!\n";
    int count = 5;
    while(count--){
        fwrite(msg, strlen(msg), 1, fp);
    }
    fclose(fp);
    return 0;
}
```

### 2-3 hello.c读文件

```c
#include <stdio.h>
#include <string.h>
int main()
{
    FILE *fp = fopen("myfile", "r");
    if(!fp){
        printf("fopen error!\n");
        return 1;
    }
    char buf[1024];
    const char *msg = "hello bit!\n";

    while(1){
        //注意返回值和参数，此处有坑，仔细查看man手册关于该函数的说明
        size_t s = fread(buf, 1, strlen(msg), fp);
        if(s > 0){
            buf[s] = 0;
            printf("%s", buf);
        }
        if(feof(fp)){
            break;
        }
    }
    fclose(fp);
    return 0;
}
```
稍作修改，实现简单cat命令:
```c
#include <stdio.h>
#include <string.h>
int main(int argc, char* argv[])
{
    if (argc!= 2)
    {
        printf("argc error!\n");
        return 1;
    }
    FILE *fp = fopen(argv[1], "r");
    if(!fp){
        printf("fopen error!\n");
        return 2;
    }
    char buf[1024];
    while(1){
        size_t s = fread(buf, 1, sizeof(buf), fp);
        if(s > 0){
            buf[s] = 0;
            printf("%s", buf);
        }
        if(feof(fp)){
            break;
        }
    }
    fclose(fp);
    return 0;
}
```

### 2-4输出信息到显示器

```c
#include <stdio.h>
#include <string.h>
int main()
{
    const char *msg = "hello fwrite\n";
    fwrite(msg, strlen(msg), 1, stdout);
    printf("hello, printf\n");
    fprintf(stdout, "hello fprintf\n");
    return 0;
}
```

### 2-5 stdin & stdout & stderr

 C默认会打开三个输入/输出流，分别是`stdin`,`stdout`,`stderr`
- 仔细观察发现，这三个流的类型都是`FILE*`，fopen返回值类型，文件指针
```c
#include <stdio.h>
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
```

### 2-6打开文件的方式

```
r   Open text file for reading.
    The stream is positioned at the beginning of the file.
r+  Open for reading and writing.
    The stream is positioned at the beginning of the file.
w   Truncate(缩短) file to zero length or create text file for writing.
    The stream is positioned at the beginning of the file.
w+  Open for reading and writing.
    The file is created if it does not exist, otherwise it is truncated.
    The stream is positioned at the beginning of the file.
a   Open for appending (writing at end of file).
    The file is created if it does not exist.
    The stream is positioned at the end of the file.
a+  Open for reading and appending (writing at end of file).
    The file is created if it does not exist. The initial file position
    for reading is at the beginning of the file,
    but output is always appended to the end of the file.
```

## 3. 系统文件I/O
打开文件的方式不仅仅是`fopen`，`ifstream`等流式，语言层的方案，其实系统才是打开文件最底层的方案。不过，在学习系统文件IO之前，先要了解下如何给函数传递标志位，该方法在系统文件IO接口中会使用到:

### 3-1 一种传递标志位的方法
```c
#include <stdio.h>

#define ONE  0001 //0000 0001
#define TWO  0002 //0000 0010
#define THREE 0004 //0000 0100

void func(int flags) {
    if (flags & ONE) printf("flags has ONE!");
    if (flags & TWO) printf("flags has TWO!");
    if (flags & THREE) printf("flags has THREE!");
    printf("\n");
}

int main() {
    func(ONE);
    func(THREE);
    func(ONE | TWO);
    func(ONE | THREE | TWO);
    return 0;
}
```

操作文件，除了上小节的C接口（当然，C++也有接口，其他语言也有），我们还可以采用系统接口来进行文件访问，先来直接以系统代码的形式，实现和上面一模一样的代码:

### 3-2 hello.c写文件:
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

int main()
{
    umask(0);
    int fd = open("myfile", O_WRONLY|O_CREAT, 0644);
    if(fd < 0){
        perror("open");
        return 1;
    }

    int count = 5;
    const char *msg = "hello bit!\n";
    int len = strlen(msg);

    while(count--){
        write(fd, msg, len);//fd: 后面讲，msg: 缓冲区首地址，len: 本次读取，期望写入多少个字节的数据。返回值: 实际写了多少字节数据
    }

    close(fd);
    return 0;
}
```

### 3-3 hello.c读文件
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <unistd.h>
#include <string.h>

int main()
{
    int fd = open("myfile", O_RDONLY);
    if(fd < 0){
        perror("open");
        return 1;
    }

    const char *msg = "hello bit!\n";
    char buf[1024];
    while(1){
        ssize_t s = read(fd, buf, strlen(msg));//类比write
        if(s > 0){
            printf("%s", buf);
        }else{
            break;
        }
    }

    close(fd);
    return 0;
}
```

### 3-4 接口介绍
**open** `man open`
```c
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int open(const char *pathname, int flags);
int open(const char *pathname, int flags, mode_t mode);
```
- `pathname`: 要打开或创建的目标文件
- `flags`: 打开文件时，可以传入多个参数选项，用下面的一个或者多个常量进行“或”运算，构成flags。
  - `O_RDONLY`: 只读打开
  - `O_WRONLY`: 只写打开
  - `O_RDWR`: 读，写打开
    - 这三个常量，必须指定一个且只能指定一个
  - `O_CREAT`: 若文件不存在，则创建它。需要使用`mode`选项，来指明新文件的访问权限
  - `O_APPEND`: 追加写
- **返回值**:
  - 成功: 新打开的文件描述符
  - 失败: -1

`mode_t`理解: 当使用 `O_CREAT` 标志时，需要指定此参数，用于设置新创建文件的权限。

`open`函数具体使用哪个，和具体应用场景相关，如目标文件不存在，需要`open`创建，则第三个参数表示创建文件的默认权限，否则，使用两个参数的`open`。

`write` `read` `close` `lseek`，类比C文件相关接口。

### 3-5 open函数返回值
在认识返回值之前，先来认识一下两个概念: **系统调用** 和 **库函数**
- 上面的`fopen` `fclose` `fread` `fwrite` 都是C标准库当中的函数，我们称之为库函数（`libc`）。
- 而`open` `close` `read` `write` `lseek` 都属于系统提供的接口，称之为系统调用接口
- 回忆一下我们讲操作系统概念时，画的一张图 
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/99bad243ad154300a20f30a7ad9fa474.png)
系统调用接口和库函数的关系，一目了然。
所以，可以认为，`f#`系列的函数，都是对系统调用的封装，方便二次开发。

## 4 文件描述符fd
- 通过对open函数的学习，我们知道了文件描述符就是一个小整数

### 4-1 0 & 1 & 2
- Linux进程默认情况下会有3个缺省打开的文件描述符，分别是标准输入0，标准输出1，标准错误2。
- 0,1,2对应的物理设备一般是：键盘，显示器，显示器

所以输入输出还可以采用如下方式:
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>

int main()
{
    char buf[1024];
    ssize_t s = read(0, buf, sizeof(buf));
    if(s > 0){
        buf[s] = 0;
        write(1, buf, strlen(buf));
        write(2, buf, strlen(buf));
    }
    return 0;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/eee1c2b9f9a24fdaac408f3f566cb7e9.png)


而现在知道，文件描述符就是从0开始的小整数。

当我们打开文件时，操作系统在内存中要创建相应的数据结构来描述目标文件。于是就有了file结构体，表示一个已经打开的文件对象。

而进程执行open系统调用，所以必须让进程和文件关联起来。每个进程都有一个指针`*files`,指向一张表`files_struct`,该表最重要的部分就是包含一个指针数组，每个元素都是一个指向打开文件的指针！

所以，本质上**文件描述符就是该数组的下标**。所以，只要拿着文件描述符，就可以找到对应的文件。

### 4-2 文件描述符的分配规则
直接看代码:
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main()
{
    int fd = open("myfile", O_RDONLY);
    if(fd < 0){
        perror("open");
        return 1;
    }
    printf("fd: %d\n", fd);

    close(fd);
    return 0;
}
```
输出发现是 fd: 3
关闭0或者2，在看
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

int main()
{
    close(0);
    //close(2);
    int fd = open("myfile", O_RDONLY);
    if(fd < 0){
        perror("open");
        return 1;
    }
    printf("fd: %d\n", fd);

    close(fd);
    return 0;
}
```
发现是结果是: fd: 0 或者 fd 2，可见，文件描述符的分配规则: **在files_struct数组当中，找到当前没有被使用的最小的一个下标，作为新的文件描述符**。 

## 5 重定向
那如果关闭1呢？看代码:
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdlib.h>

int main()
{
    close(1);
    int fd = open("myfile", O_WRONLY|O_CREAT, 0644);
    if(fd < 0){
        perror("open");
        return 1;
    }
    printf("fd: %d\n", fd);
    fflush(stdout);

    close(fd);
    exit(0);
}
```
此时，我们发现，本来应该输出到显示器上的内容，输出到了文件myfile当中，其中，fd = 1。这种现象叫做输出重定向。常见的重定向有: >，>>，< 。

### 重定向概念
重定向是指改变输入或输出的默认流向 。在文件I/O操作中，常见的有输入重定向、输出重定向和追加重定向。在Linux系统下，标准输入（stdin，文件描述符为0 ）默认关联键盘，标准输出（stdout，文件描述符为1 ）和标准错误输出（stderr，文件描述符为2 ）默认关联显示器。重定向就是改变这些默认关联，让输入从文件读取，或者让输出写入到文件等其他目标。

### 5-1 重定向类型
- **输出重定向（>）**：使用`>`符号 ，会将命令的输出结果覆盖写入到指定文件中。如果文件不存在则创建文件；如果文件已存在，则会清空文件原有内容后再写入。例如在命令行中执行`ls > file.txt`，会把`ls`命令的输出结果写入到`file.txt`文件中，`file.txt`原内容被覆盖 。
- **追加重定向（>>）**：使用`>>`符号 ，会将命令的输出结果追加到指定文件末尾。文件不存在时会创建文件；存在时不会清空原有内容，而是在文件最后添加新的输出内容。比如`echo "new line" >> file.txt` ，会在`file.txt`文件末尾添加“new line”这一行内容。 
- **输入重定向（<）**：利用`<`符号 ，让命令从指定文件中读取输入数据，而不是从标准输入（如键盘）获取。例如`sort < numbers.txt` ，`sort`命令会从`numbers.txt`文件中读取内容进行排序，而不是等待从键盘输入数据。 

### 5-2 代码层面原理（以输出重定向为例）
在程序代码中，如之前示例：
```c
close(1);
int fd = open("myfile", O_WRONLY|O_CREAT, 0644);
// 后续输出操作
```
关闭标准输出（文件描述符1 ）后，再打开一个文件（这里是`myfile` ），此时新打开文件的文件描述符会被分配为1（根据文件描述符分配规则，找未使用的最小下标 ）。
后续原本向标准输出（显示器）输出内容的操作，因为底层通过文件描述符1来定位输出目标，现在就会将内容输出到`myfile`文件中，实现了输出重定向。 


**那重定向的本质是什么呢?**

**重定向的本质是对文件描述符的重新分配与关联** 。

### 5-3 使用dup2系统调用
函数原型如下:
```c
#include <unistd.h>

int dup2(int oldfd, int newfd);
```
示例代码
```c
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

int main() {
    int fd = open("./log", O_CREAT | O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    close(1);
    dup2(fd, 1);
    for (;;) {
        char buf[1024] = {0};
        ssize_t read_size = read(0, buf, sizeof(buf) - 1);
        if (read_size < 0) {
            perror("read");
            break;
        }
        printf("%s", buf);
        fflush(stdout);
    }
    return 0;
}
```
其作用是将一个现有的文件描述符（oldfd）复制到另一个文件描述符（newfd），并且复制成功后 ， oldfd与newfd将指向同一个文文件表项

printf是C库当中的IO函数，一般往stdout中输出，但是stdout底层访问文件的时候，找的还是fd:1，但此时，fd:1下标所表示内容，已经变成了myfile的地址，不再是显示器文件的地址，所以，输出的任何消息都会往文件中写入，进而完成输出重定向。

## 6. 理解“一切皆文件”
- 首先，在windows中是文件的东西，它们在linux中也是文件；
- 其次一些在windows中不是文件的东西，比如进程、磁盘、显示器、键盘这样硬件设备也被抽象成了文件，你可以使用访问文件的方法访问它们获得信息；
- 甚至管道，也是文件；
- 网络编程中的socket（套接字）这样的东西，使用的接口跟文件接口也是一致的。

这样做最明显的好处是，开发者仅需要使用一套API和开发工具，即可调取Linux系统中绝大部分的资源。

举个简单的例子，Linux中几乎所有读（读文件，读系统状态，读PIPE）的操作都可以用read函数来进行；几乎所有更改（更改文件，更改系统参数，写PIPE）的操作都可以用write函数来进行。

之前我们讲过，当打开一个文件时，操作系统为了管理所打开的文件，都会为这个文件创建一个file结构体，该结构体定义在`/usr/src/kernels/3.10.0-1160.71.1.el7.x86_64/include/linux/fs.h`下，以下展示了该结构部分我们关系的内容:

```c
struct file {
   ...
    struct inode  *f_inode;  /* cached value */

    const struct file_operations  *f_op;
   ...
    atomic_long_t      f_count;   // 表示打开文件的引用计数，如果有多个文件指针指向它，就会增加f_count的值。
    unsigned int       f_flags;   // 表示打开文件的权限
    fmode_t            f_mode;    // 设置对文件的访问模式，例如：只读，只写等。所有的标志在头文件<fcntl.h>中定义
    loff_t             f_pos;     // 表示当前读写文件的位置
   ...
} __attribute__((aligned(4)));  /* lest something weird decides that 2 is OK */
```
值得关注的是`struct file`中的`f_op`指针指向了一个`file_operations`结构体，这个结构体中的成员除了`struct module* owner`其余都是函数指针。该结构和struct file都在fs.h下。
```c
struct file_operations {
    struct module *owner;
    //指向拥有该模块的指针;
    loff_t (*llseek) (struct file *, loff_t, int);
    //llseek 方法用作改变文件中的当前读/写位置，并且新位置作为(正的)返回值.
    ssize_t (*read) (struct file *, char __user *, size_t, loff_t *);
    //用来从设备中获取数据. 在这个位置的一个空指针导致 read 系统调用以 -EINVAL("Invalid argument")失败. 一个非负返回值代表了成功读取的字节数( 返回值是一个 "signed size" 类型，常常是目标平台本地的整数类型).
    ssize_t (*write) (struct file *, const char __user *, size_t, loff_t *);
    //发送数据到设备. 如果 NULL，-EINVAL 返回给调用 write 系统调用的程序. 如果非负，返回值代表成功写的字节数.
    ssize_t (*aio_read) (struct kiocb *, const struct iovec *, unsigned long, loff_t);
    //初始化一个异步读 -- 可能在函数返回前不结束的读操作.
    ssize_t (*aio_write) (struct kiocb *, const struct iovec *, unsigned long, loff_t);
    //初始化设备上的一个异步写.
    int (*readdir) (struct file *, void *, filldir_t);
    //对于设备文件这个成员应当为 NULL; 它用来读取目录，并且仅对**文件系统**有用.
    unsigned int (*poll) (struct file *, struct poll_table_struct *);
    int (*ioctl) (struct inode *, struct file *, unsigned int, unsigned long);
    long (*unlocked_ioctl) (struct file *, unsigned int, unsigned long);
    long (*compat_ioctl) (struct file *, unsigned int, unsigned long);
    int (*mmap) (struct file *, struct vm_area_struct *);
    //mmap 用来请求将设备内存映射到进程的地址空间. 如果这个方法是 NULL，mmap 系统调用返回 -ENODEV.
    int (*open) (struct inode *, struct file *);
    //打开一个文件
    int (*flush) (struct file *, fl_owner_t id);
    //flush 操作在进程关闭它的设备文件描述符的拷贝时调用;
    int (*release) (struct inode *, struct file *);
    //在文件结构被释放时引用这个操作. 如同 open, release 可以为 NULL.
    int (*fsync) (struct file *, struct dentry *, int datasync);
    //发送调用刷新任何挂着的数据.
    int (*aio_fsync) (struct kiocb *, int datasync);
    int (*fasync) (int, struct file *, int);
    int (*lock) (struct file *, int, struct file_lock *);
    //lock 方法用来实现文件加锁；加锁对常规文件是必不可少的特性，但是设备驱动几乎从不实现它.
    ssize_t (*sendpage) (struct file *, struct page *, int, size_t, loff_t *, int);
    unsigned long (*get_unmapped_area)(struct file *, unsigned long, unsigned long, unsigned long, unsigned long);
    int (*check_flags)(int);
    int (*flock) (struct file *, int, struct file_lock *);
    ssize_t (*splice_write)(struct pipe_inode_info *, struct file *, loff_t *, size_t, unsigned int);
    ssize_t (*splice_read)(struct file *, loff_t *, struct pipe_inode_info *, size_t, unsigned int);
    int (*setlease)(struct file *, long, struct file_lock **);
};
```
`file_operation`就是**把系统调用和驱动程序关联起来的关键数据结构**，这个结构的每一个成员都对应着一个系统调用。读取file_operation中相应的函数指针，接着把控制权转交给函数，从而完成了Linux设备驱动程序的工作。

介绍完相关代码，一张图总结: 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/402d5218f49a48dead6d7ed456e5dd3d.png)


## 7. 缓冲区
### 7-1 什么是缓冲区
缓冲区是内存空间的一部分。

也就是说，在内存空间中预留了一定的存储空间，这些**存储空间用来缓冲输入或输出的数据**，这部分预留的空间就叫做缓冲区。

缓冲区根据其对应的是输入设备还是输出设备，**分为输入缓冲区和输出缓冲区**。

### 5-2 为什么要引入缓冲区机制
读写文件时，如果不会开辟对文件操作的缓冲区，直接通过系统调用对磁盘进行操作（读、写等），那么每次对文件进行一次读写操作时，都需要使用读写系统调用来处理此操作，即需要执行一次系统调用，执行一次系统调用将涉及到CPU状态的切换，即从用户空间切换到内核空间，实现进程上下文的切换，这将损耗一定的CPU时间，频繁的磁盘访问对程序的执行效率造成很大的影响。

**为了减少使用系统调用的次数，提高效率，我们就可以采用缓冲机制**。比如我们从磁盘里取信息，可以在磁盘文件进行操作时，一次从文件中读出大量的数据到缓冲区中，以后对这部分的访问就不需要再使用系统调用了，等缓冲区的数据取完后再去磁盘中读取，这样就可以**减少磁盘的读写次数，再加上计算机对缓冲区的操作大大快于对磁盘的操作，故应用缓冲区可大大提高计算机的运行速度**。

又比如，我们使用打印机打印文档，由于打印机的打印速度相对较慢，我们先把文档输出到打印机相应的缓冲区，打印机再自行逐步打印，这时我们的CPU可以处理别的事情。可以看出，缓冲区就是一块内存区域，它用在输入输出设备和CPU之间，用来缓存数据。它使得低速的输入输出设备和高速的CPU能够协调工作，避免低速的输入输出设备占用CPU，解放出CPU，使其能够高效率工作。

### 7-3 缓冲类型
标准I/O提供了3种类型的缓冲区。
- **全缓冲区**：这种缓冲方式要求填满整个缓冲区后才进行I/O系统调用操作。对于磁盘文件的操作通常使用全缓冲的方式访问。
- **行缓冲区**：在操作的缓冲情况下，当在输入和输出中遇到换行符时，标准I/O库函数将会执行系统调用操作。当所操作的流涉及一个终端（例如标准输入和标准输出），使用行缓冲方式。因为标准I/O库每行的缓冲区长度是固定的，所以只要填满了缓冲区，即使还没有遇到换行符，也会执行I/O系统调用操作，默认行缓冲区的大小为1024。 
- **无缓冲区**：无缓冲区是指标准I/O库不进行缓存，直接调用系统调用。标准出错流stderr通常是不带缓冲区的，这使得出错信息能够尽快地显示出来。

除了上述列举的默认刷新方式，下列特殊情况也会引发缓冲区的刷新：
1. 缓冲区满时；
2. 执行flush语句；

示例如下：
```c
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    close(1);
    int fd = open("log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return 0;
    }
    printf("hello world: %d\n", fd);
    close(fd);
    return 0;
}
```
我们本来想使用重定向思维，让本应该打印在显示器上的内容写到“log.txt”文件中，但我们发现，程序运行结束后，文件中并没有被写入内容：
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/39d6eedd0a3e4d8aaf312a5bbf3dbaae.png)

这是由于我们将1号描述符重定向到磁盘文件后，缓冲区的刷新方式成为了全缓冲。而我们写入的内容并没有填满整个缓冲区，导致并不会将缓冲区的内容刷新到磁盘文件中。怎么办呢？可以使用fflush强制刷新下缓冲区。
```c
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    close(1);
    int fd = open("log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return 0;
    }
    printf("hello world: %d\n", fd);
    fflush(stdout);
    close(fd);
    return 0;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/365143fc27d34d7c8cb660c327762feb.png)

还有一种解决方法，刚好可以验证一下stderr是不带缓冲区的，代码如下：
```c
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

int main() {
    close(2);
    int fd = open("log.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        perror("open");
        return 0;
    }
    perror("hello world");
    close(fd);
    return 0;
}
```
这种方式便可以将2号文件描述符重定向至文件，由于stderr没有缓冲区，“hello world”不用fflash就可以写入文件：
```bash
[hyb@VM-8-12-centos buffer]$./myfile
[hyb@VM-8-12-centos buffer]$ cat log.txt
hello world: Success
```

### 7-4 FILE
- 因为IO相关函数与系统调用接口对应，并且库函数封装系统调用，所以本质上，访问文件都是通过fd访问的。所以C库当中的FILE结构体内，必定封装了fd。

来段代码研究一下：
```c
#include <stdio.h>
#include <string.h>

int main()
{
    const char *msg0="hello printf\n";
    const char *msg1="hello fwriteln";
    const char *msg2="hello write\n";

    printf("%s",msg0);
    fwrite(msg1,strlen(msg1),stdout);
    write(msg2,strlen(msg2),1,stdout);


    fork();
    return 0;
}
```
运行结果：
```
hello printf
hello fwrite
hello fwrite
```
但如果对进程实现输出重定向呢？`./hello > file` ，我们发现结果变成了：
```
hello write
hello printf
hello fwrite
hello printf
hello fwrite
```
我们发现printf和fwriteln（库函数）都输出了2次，而write只输出了一次（系统调用）。为什么呢？肯定和fork有关！
- 一般库函数写入文件时是全缓冲的，而写入显示器是行缓冲。
- printf、fwriteln库函数会自带缓冲区，当发生重定向到普通文件时，数据的缓冲方式由行缓冲变成了全缓冲。
- 而我们放在缓冲区中的数据，就不会被立即刷新，甚至fork之后但是进程退出之后，会统一刷新，写入文件当中。
- 但是fork的时候，父子进程数据会发生写时拷贝，所以当父进程准备刷新的时候，子进程也有了同样的一份数据，随即产生两份数据。
- write没有变化，说明没有所谓的缓冲区，而write系统调用没有带缓冲区。

综上：`printf`、`fwrite`库函数会自带缓冲区，而write系统调用没有带缓冲区。另外，我们这里所说的缓冲区，都是用户级缓冲区。其实为了提升整机性能，OS也会提供相关内核级缓冲区，不过不再我们讨论范围之内。

那这层缓冲区对系统调用的printf、fwriteln有，但是`write`没有，而`printf`、`fwrite`是库函数，也就是说，**该缓冲区是二次加上的**，又因为是C，所以由标准库提供。如果有兴趣，可以看一下FILE的结构体：

```c
struct _IO_FILE {
    int _flags;           /* High-order word is _IO_MAGIC; rest is flags. */
    #define _IO_file_flags _flags

    //缓冲区相关
    /* The following pointers correspond to the C++ streambuf protocol. */
    /* Note: Tk uses the _IO_read_ptr and _IO_read_end fields directly. */
    char* _IO_read_ptr;       /* Current read pointer */
    char* _IO_read_end;       /* End of get area. */
    char* _IO_read_base;      /* Start of putback+get area. */
    char* _IO_write_base;     /* Start of put area. */
    char* _IO_write_ptr;      /* Current put pointer. */
    char* _IO_write_end;      /* End of put area. */
    char* _IO_buf_base;       /* Start of reserve area. */
    char* _IO_buf_end;        /* End of reserve area. */
    /* The following fields are used to support backing up and undo. */
    char *_IO_save_base; /* Pointer to start of non-current get area. */
    char *_IO_backup_base; /* Pointer to first valid character of backup area */
    char *_IO_save_end; /* Pointer to end of non-current get area. */

    struct _IO_marker *_markers;
    struct _IO_FILE *_chain;

    int _fileno; //封装的文件描述符
    #if 0
    int _blksize;
    #else
    int _flags2;
    #endif
    _IO_off_t _old_offset; /* This used to be _offset but it's too small. */

    #define __HAVE_COLUMN /* temporary */
    /* 1+column number of pbase(); 0 is unknown. */
    unsigned short _cur_column;
    signed char _vtable_offset;
    char _shortbuf[1];

    /* char* _save_gptr;  char* _save_egptr; */
    _IO_lock_t *_lock;
    #ifdef _IO_USE_OLD_IO_FILE
};
```



