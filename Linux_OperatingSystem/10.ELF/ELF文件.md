
##  1. 目标文件
编译和链接这两个步骤，在Windows下被我们的IDE封装得很完美，我们一般都是一键构建非常方便，但一旦遇到错误的时候呢，尤其是链接相关的错误，很多人就束手无策了。在Linux下，我们之前也学习过如何通过gcc编译器来完成这一系列操作。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/e860f6c38f874505bb851074754e99f5.png)


接下来我们深入探讨一下编译和链接的整个过程，来更好的理解动静态库的使用原理。

先来回顾下什么是编译呢？编译的过程其实就是将我们程序的源代码翻译成CPU能够直接运行的机器代码。

比如：在一个源文件hello.c里要简单输出"hello world!"，并且调用一个run函数，而这个函数被定义在另一个原文件code.c中。这里我们就可以调用gcc -c来分别编译这两个原文件。

```c
// hello.c
#include<stdio.h>
void run();
int main() {
    printf("hello world!\n");
    run();
    return 0;
}

// code.c
#include<stdio.h>
void run() {
    printf("running...\n");
}

// 编译两个源文件
$ gcc -c hello.c
$ gcc -c code.c
$ ls
code.c  code.o  hello.c  hello.o
```

可以看到，在编译之后会生成两个扩展名为.o的文件，它们被称作目标文件。要注意的是如果我们修改了一个原文件，那么只需要单独编译它这一个，而不需要浪费时间重新编译整个工程。

目标文件是一个二进制的文件，文件的格式是ELF，是对二进制代码的一种封装。

```bash
$ file hello.o
hello.o: ELF 64-bit LSB relocatable, x86-64, version 1 (SYSV), not stripped
### file命令用于辨识文件类型。
```

## 2. ELF文件
要理解编译链接的细节，我们不得不了解一下ELF文件。其实有以下四种文件其实都是ELF文件：
- 可重定位文件（Relocatable File）：即xxx.o文件。包含适合于与其他目标文件链接来创建可执行文件或者共享目标文件的代码和数据。
- 可执行文件（Executable File）：即可执行程序。 
- 共享目标文件（Shared Object File）：即xxx.so文件。 
- 内核转储（core dumps），存放当前进程的执行上下文，用于dump信号触发。

一个ELF文件由以下四部分组成：
- ELF头（ELF header）：**描述文件的主要特性**。其位于文件的开始位置，它的主要目的是定位文件的其他部分。 
- 程序头表（Program header table）：列举了所有有效的段（segments）和他们的属性。表里记着每个段的开始的位置和位移（offset）、长度，毕竟这些段，都是紧密的放在二进制文件中，需要段表的描述信息，才能把他们每个段分割开。 **描述文件中各个段在内存中的布局和加载方式，当文件加载到内存时，程序头表中的信息会指导系统如何将文件中的端映射到内存地址**
- 节头表（Section header table）：包含对节（sections）的描述。 
- 节（Section）：**ELF文件中的基本组成单位，包含了特定类型的数据**。ELF文件的各种信息和数据都存储在不同的节中，如代码节存储了可执行代码，数据节存储了全局变量和静态数据等。（而端是从内存角度的划分，一个端可能会包括多个节）

最常见的节：
- 代码节（.text）：用于保存机器指令，是程序的主要执行部分。 
- 数据节（.data）：保存已初始化的全局变量和局部静态变量。 
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/b64286add5db4d8c85f6a2dbf020b828.png)

## 3. ELF从形成到加载轮廓

### 3-1 ELF形成可执行

- step-1：将多份C/C++ 源代码，翻译成为目标`.o`文件
- step-2：将多份`.o` 文件section进行合并

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/e1e7113441cd4fb2957f9bcc2ec5e45c.png)
### 3-2 ELF可执行文件加载
- **一个ELF会有多种不同的Section，在加载到内存的时候，也会进行`Section`合并，形成`Segment`**（链接本质）
- 合并原则：相同属性，比如：可读，可写，可执行，需要加载时申请空间等.
- 这样，即便是不同的Section，在加载到内存中，可能会以segment的形式，加载到一起
- 很显然，这个合并工作也已经在形成ELF的时候，合并方式已经确定了，**具体合并原则被记录在了ELF的程序头表（Program header table）中**

```
# 查看可执行程序的section
$ readelf -S a.out
There are 31 section headers, starting at offset 0x19d8:

Section Headers:
[Nr] Name              Type            Address          Offset
     Size              EntSize          Flags  Link  Info  Align

[ 0]                   NULL            0000000000000000 00000000
     0000000000000000 0000000000000000      0     0     0     0
[ 1].interp           PROGBITS        0000000000400238 00000238
     000000000000001c 0000000000000000   A       0     0     1
[ 2].note.ABI-tag     NOTE            0000000000400254 00000254
     0000000000000020 0000000000000000   A       0     0     4
[ 3].note.gnu.build-i NOTE            0000000000400274 00000274
     0000000000000024 0000000000000000   A       0     0     4
[ 4].gnu.hash         GNU_HASH        0000000000400298 00000298
     000000000000001c 0000000000000000   A       5     0     8
[ 5].dynsym           DYNSYM          00000000004002b8 000002b8
     0000000000000048 0000000000000018   A       6     1     8
[ 6].dynstr           STRTAB          0000000000400300 00000300
     0000000000000038 0000000000000000   A       0     0     1
[ 7].gnu.version      VERSYM          0000000000400338 00000338
     0000000000000006 0000000000000002   A       5     0     2
[ 8].gnu.version_r    VERNEED         0000000000400340 00000340
     0000000000000010 0000000000000000   A       6     1     8
[ 9].rela.dyn         RELA            0000000000400360 00000360
     0000000000000018 0000000000000018   A       5     0     8
[10].rela.plt         RELA            0000000000400378 00000378
     0000000000000018 0000000000000018  AI     5    24     8
[11].init             PROGBITS        0000000000400390 00000390
```

```
# 查看section合并的segment
$ readelf -l a.out
Elf file type is EXEC (Executable file)
Entry point 0x4003e0
There are 9 program headers, starting at offset 64

Program Headers:
Type           Offset             VirtAddr           PhysAddr
               FileSiz            MemSiz              Flags  Align
PHDR           0x0000000000000040 0x0000000000400040 0x0000000000400040
               0x00000000000001f8 0x00000000000001f8  R E     8
INTERP         0x0000000000000238 0x0000000000400238 0x0000000000400238
               0x000000000000001c 0x000000000000001c  R       1
    [Requesting program interpreter: /lib64/ld-linux-x86-64.so.2]
LOAD           0x0000000000000000 0x0000000000400000 0x0000000000400000
               0x0000000000000744 0x0000000000000744  R E   200000
LOAD           0x0000000000000e10 0x0000000000600e10 0x0000000000600e10
               0x0000000000000218 0x0000000000000228  RW   200000
DYNAMIC        0x0000000000000e28 0x0000000000600e28 0x0000000000600e28
               0x00000000000001d0 0x00000000000001d0  RW       8
NOTE           0x0000000000000254 0x0000000000400254 0x0000000000400254
               0x0000000000000044 0x0000000000000044  R       4
GNU_EH_FRAME   0x00000000000005a0 0x00000000004005a0 0x00000000004005a0
               0x000000000000004c 0x000000000000004c  R       4
GNU_STACK      0x0000000000000000 0x0000000000000000 0x0000000000000000
               0x0000000000000000 0x0000000000000000  RW      10
GNU_RELRO      0x0000000000000e10 0x0000000000600e10 0x0000000000600e10
               0x00000000000001f0 0x00000000000001f0  R       1

Section to Segment mapping:
Segment Sections...
  00
  01     .interp
  02     .interp .note.ABI-tag .note.gnu.build-id .gnu.hash .dynsym .dynstr
.gnu.version .gnu.version_r .rela.dyn .rela.plt .init .plt .plt.got .text
.fini .rodata .eh_frame_hdr .eh_frame
  03     .init_array .fini_array .jcr .dynamic .got .got.plt .data .bss
  04     .dynamic
  05     .note.ABI-tag .note.gnu.build-id
  06     .eh_frame_hdr
  07
  08     .init_array .fini_array .jcr .dynamic .got
```

>为什么要将section合并成为segment
>- Section合并的主要原因是**为了减少页面碎片**，提高内存使用效率。如果不进行合并，假设页面大小为4096字节（内存块基本大小，加载，管理的基本单位），如果.text部分为4097字节，.init部分为512字节，那么它们将占用3个页面，而合并后，它们只需2个页面。
>- 此外，操作系统在加载程序时，会将具有相同属性的section合并成一个大的segment，这样就可以实现不同的访问权限，从而优化内存管理和权限访问控制。

对于程序头表和节头表，又有什么用呢，其实ELF文件提供2个不同的视图/视角来让我们理解这两个部分：
- 链接视图（Linking view）- 对应节头表Section header table
  - 文件结构的粒度更细，将文件按功能模块的差异进行划分，静态链接分析的时候一般关注的是链接视图，能够理解ELF文件中包含的各个部分的信息。
  - 为了空间布局上的效率，将来在链接目标文件时，链接器会把很多节（section）合并，规整成可执行的段（segment）、可读写的段、只读段等。合并了后，空间利用率就高了，否则，很小的很小的一段，未来物理内存浪费太大（物理内存分配一般都是整数倍一块给你，比如4k），所以，链接器趁着链接就把小块们都合并了。
- 执行视图（execution view）- 对应程序头表Program header table
  - 告诉操作系统，如何加载可执行文件，完成进程内存的初始化。一个可执行程序的格式中，一定有program header table。
- 说白了就是：一个在链接时作用，一个在运行加载时作用。 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/035c05f1152e409f8b54d2999b6d66ba.png)
#### 从链接视图来看：
- 命令`readelf -S hello.o`可以帮助查看ELF文件的节头表。
- `.text节`：是保存了程序代码指令的代码节。
- `.data节`：保存了初始化的全局变量和局部静态变量等数据。
- `.rodata节`：保存了只读的数据，如一行C语言代码中的字符串。由于.rodata节是只读的，所以只能存在于一个可执行文件的只读段中。因此，只能是在text段（不是data段）中找到.rodata节。 
- `.BSS节`：为未初始化的全局变量和局部静态变量预留位置。
- `.symtab节`（Symbol Table 符号表）：.symtab里面保存了函数名、变量名和代码的对应关系。 
- `.got.plt节`（全局偏移表 - 过程链接表）：.got节保存了全局偏移表。.got节和.plt节一起提供了对导入的共享库函数的访问入口，由动态链接器在运行时进行修改。对于GOT的理解，我们后面会说。
- 使用`readelf`命令查看.so文件可以看到该节。

#### 从执行视图来看：
- 告诉操作系统哪些模块可以被加载进内存。
- 加载进内存之后哪些分段是可读可写，哪些分段是只读，哪些分段是可执行的。

我们可以在ELF头中找到文件的基本信息，以及可以看到ELF头是如何定位程序头表和节头表的。例如我们查看hello.o这个可重定位文件的主要信息：
```bash
# 查看目标文件
$ readelf -h hello.o
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00 
  Class:                             ELF64  # 文件类型
  Data:                              2's complement, little endian  # 指定的编码方式
  Version:                           1 (current)
  OS/ABI:                            UNIX - System V
  ABI Version:                       0
  Type:                              REL (Relocatable file)  # 指出ELF文件的类型
  Machine:                           Advanced Micro Devices X86-64  # 该程序需要的体系结构
  Version:                           0x0
  Entry point address:               0x0  # 系统第一个传输控制的虚拟地址，在那启动进程。假如文件没有如何关联的入口点，该成员就保持为0。
  Start of program headers:          0 (bytes into file)
  Start of section headers:          728 (bytes into file)
  Flags:                             0x0
  Size of this header:               64 (bytes)  # 保存着ELF头大小(以字节计数)
  Size of program headers:           64 (bytes)  # 保存着在文件的程序头表(program header table)中一个入口的大小
  Number of program headers:         0  # 保存着在程序头表中入口的个数。因此，e_phentsize和e_phnum的乘机就是表的大小(以字节计数).假如没有程序头表，变量为0。
  Size of section headers:           64 (bytes)  # 保存着section头的大小(以字节计数)。一个section头是在section头表的一个入口
  Number of section headers:         13  # 保存着在section header table的入口数目。因此，e_shentsize和e_shnum的乘积就是section头表的大小(以字节计数)。假如文件没有section头表，值为0。
  Section header string table index: 12  # 保存着跟section名字符表相关入口的section头(section header table)索引。
```
```bash
# 查看可执行程序
$ gcc -o main.c
$ readelf -h a.out
ELF Header:
  Magic:   7f 45 4c 46 02 01 00 00 00 00 00 00 00 00 00 00 
  Class:                             ELF64
  Data:                              2's complement, little endian
  Version:                           1 (current)
  OS/ABI:                            UNIX - System V
  ABI Version:                       0
  Type:                              DYN (Shared object file)
  Machine:                           Advanced Micro Devices X86-64
  Version:                           0x1
  Entry point address:               0x1060
  Start of program headers:          64 (bytes into file)
  Start of section headers:          14768 (bytes into file)
  Flags:                             0x0
  Size of this header:               64 (bytes)
  Size of program headers:           56 (bytes)
  Number of program headers:         13
  Size of section headers:           64 (bytes)
  Number of section headers:         31
  Section header string table index: 30
```
对于ELF HEADER这部分来说，我们只用知道其作用即可，它的主要目的是定位文件的其他部分。



## 4. 理解连接与加载
### 4-1静态链接
- 无论是自己的.o，还是静态库中的.o，本质都是把.o文件进行连接的过程
- 所以：研究静态链接，本质就是研究.o是如何链接的

**查看编译后的.o目标文件**
```bash
$ objdump -d code.o

code.o:	file format elf64-x86-64

Disassembly of section .text:

0000000000000000 <run>:
   0:	 f3 0f 1e fa          	endbr64
   4:	 55                   	push   %rbp
   5:	 48 89 e5             	mov    %rsp,%rbp
   8:	 48 8d 3d 00 00 00 00 	lea    0x0(%rip),%rdi        # f <run+0xf>
   f:	 e8 00 00 00 00       	callq  0x14 <run+0x14>
  14:	 b8 00 00 00 00       	mov    $0x0,%eax
  15:	 5d                   	pop    %rbp
  16:	 c3                   	retq   

$ objdump -d hello.o

hello.o:	file format elf64-x86-64

Disassembly of section .text:

0000000000000000 <main>:
   0:	 f3 0f 1e fa          	endbr64
   4:	 55                   	push   %rbp
   5:	 48 89 e5             	mov    %rsp,%rbp
   8:	 48 8d 3d 00 00 00 00 	lea    0x0(%rip),%rdi        # f <main+0xf>
   f:	 e8 00 00 00 00       	callq  0x14 <main+0x14>
  14:	 b8 00 00 00 00       	mov    $0x0,%eax
  19:	 e8 00 00 00 00       	callq  0x1e <main+0x1e>
  1e:	 b8 00 00 00 00       	mov    $0x0,%eax
  23:	 5d                   	pop    %rbp
  24:	 c3                   	retq   
```
- `objdump -d`命令：将代码段（.text）进行反汇编查看
```c
$ cat hello.c
#include<stdio.h>
void run();
int main()
{
    printf("hello world!\n");
    run();
    return 0;
}
```
- `code.o不认识printf函数`
```c
$ cat code.c
#include<stdio.h>
void run()
{
    printf("running...\n");
}
```
我们可以看到这里的call指令，它们分别对应之前调用的printf和run函数，但是你会发现他们的跳转地址都被设成了0。这是为什么呢？
其实就是在**编译hello.c的时候，编译器是完全不知道printf和run函数的存在的，比如他们位于内存的哪个区块，代码长什么样都是不知道的**。因此，编辑器只能将这两个函数的跳转地址先暂设为0。 

这个地址会在哪个时候被修正？链接的时候！为了让链接器将来在链接时能够正确定位到这些被修正的地址，在**代码块（.data）中还存在一个重定位表**，这张表将来在链接的时候，就会根据表里记录的地址将其修正。

**注意**：
- printf涉及到动态库，这里暂不做说明 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/b6550b4e18cb4bfca076a7bede9c4fb3.png)
静态链接就是把库中的.o进行合并，和上述过程一样。

所以**链接其实就是将编译之后的所有目标文件连同用到的一些静态库运行时库组合，拼装成一个独立的可执行文件**。

其中就包括我们之前提到的地址修正，当所有模块组合在一起之后，链接器会**根据我们的.o文件或者静态库中的重定位表找到那些需要被重定位的函数全局变量，从而修正它们的地址**。这其实就是静态链接的过程。

所以，**链接过程中会涉及到对.o中外部符号进行地址重定位**。

### 4-2 ELF加载与进程地址空间
#### 4-2-1虚拟地址/逻辑地址
**问题**：
- 一个ELF程序，在没有被加载到内存的时候，有没有地址呢？
- 进程mm_struct、vm_area_struct在进程刚刚创建的时候，初始化数据从哪里来的？

**答案**：
- **一个ELF程序，在没有被加载到内存的时候，本来就有地址**，当代计算机工作的时候，都采用“平坦模式”进行工作。所以也要求ELF对自己的代码和数据进行统一编址，下面是`objdump -S`反汇编之后的代码
```
0000000000001149 <run>:
    1149:       f3 0f 1e fa           endbr64
    114d:       55                    push   %rbp
    114e:       48 89 e5              mov    %rsp,%rbp
    1151:       48 8d 3d ae ff ff 00  lea    -0x52(%rip),%rdi        # 2084 <_IO_stdin_used+0x4>
    1158:       e8 f3 fe ff ff        callq  1050 <puts@plt>
    115d:       b8 00 00 00 00        mov    $0x0,%eax
    1162:       5d                    pop    %rbp
    1163:       c3                    retq   

0000000000001166 <main>:
    1166:       f3 0f 1e fa           endbr64
    116a:       55                    push   %rbp
    116b:       48 89 e5              mov    %rsp,%rbp
    116e:       48 8d 3d 8d ff ff 00  lea    -0x73(%rip),%rdi        # 2082 <_IO_stdin_used+0xf>
    1175:       e8 d6 fe ff ff        callq  1050 <puts@plt>
    117a:       e8 cb ff ff ff        callq  114a <run>
    117f:       b8 00 00 00 00        mov    $0x0,%eax
    1184:       5d                    pop    %rbp
    1185:       c3                    retq   
    1186:       66 2e 0f 1f 84 00 00  nop    %cs:0x0(%rax,%rax,1)
    118d:       00 00 00
```
最左侧的就是ELF的虚拟地址，其实，严格意义上应该叫做**逻辑地址(起始地址+偏移量)**,但是我们认为起始地址是0.也就是说，其实**虚拟地址在我们的程序还没有加载到内存的时候，就已经把可执行程序进行统一编址了**。
- 进程mm_struct、vm_area_struct在进程刚刚创建的时候，初始化数据从哪里来的？从ELF各个segment来，每个segment有自己的起始地址和自己的长度，用来初始化内核结构中的[start, end]等范围数据，另外，用详细地址，填充页表.

所以：虚拟地址机制，不光光OS要支持，编译器也要支持.

#### 4-2-2重新理解进程虚拟地址空间
ELF在被编译好之后，会把自己未来程序的入口地址记录在ELF header的Entry字段中：
```bash
$ gcc *.o
$ readelf -h a.out
ELF Header:
  Magic:   7f 45 4c 46 02 01 01 00 00 00 00 00 00 00 00 00 
  Class:                             ELF64
  Data:                              2's complement, little endian
  Version:                           1 (current)
  OS/ABI:                            UNIX - System V
  ABI Version:                       0
  Type:                              DYN (Shared object file)
  Machine:                           Advanced Micro Devices X86-64
  Version:                           0x1
  Entry point address:               0x1060
  Start of program headers:          64 (bytes into file)
  Start of section headers:          14768 (bytes into file)
  Flags:                             0x0
  Size of this header:               64 (bytes)
  Size of program headers:           56 (bytes)
  Number of program headers:         13
  Size of section headers:           64 (bytes)
  Number of section headers:         31
  Section header string table index: 30
```
- 与虚拟地址的关系
在ELF文件被加载到内存后．逻辑地址会根据加载的映射关系．转换为虚拟地址。
这个转换是**基于ELF文件的程序头表来完成的**。在加载时、系统会根据程序头表将代码段映射到进程虚拟地址空间的特定位置，此时、代码段在ELF文件中的逻辑地址加上其在虚拟空间中的起始地址，就得到了虚拟地址。
- 与物理地址的关系．
物理地址是内存芯片中实际存储单元地址
逻辑地址不直接对应物理地址．需通过虚拟地扯这个中间桥梁、再经过，内存管理单元（MMU）的转换才能得到物理地址．
MMU根据页表等机制，将虚拟地址转换为物理地址

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/8bd93be962de4425899ceb4c5d68b8d2.jpeg)

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/2c32db2c66eb44fda16a117ae0e4b1de.png)
#### 4-2-3 重谈区域划分vm_area_struct
`vm_area_struct` 是Linux内核中用于描述进程虚拟内存区域的重要结构体，简称vma ，也被称为进程地址空间或进程线性区,是操作系统对虚拟内存空间进行管理的基本单位。
##### 基本概念
它定义在 `include/linux/mm_types.h` 文件中 ，用来**描述一段连续的、具有相同访问属性的虚拟内存空间**，且大小为物理内存页面的整数倍。由于进程使用的虚拟内存空间常不连续且各部分访问属性可能不同，一个进程的虚拟内存空间通常需多个 `vm_area_struct` 结构来描述。
##### 结构体字段及功能
1. **地址相关**
    - **`vm_start`**：保存虚拟内存空间首地址，以字节为单位，标识该区域在进程虚拟地址空间起始位置。 
    - **`vm_end`**：保存虚拟内存空间末地址后第一个字节的地址，同样以字节为单位，虚拟内存空间范围用 `(vm_start, vm_end)` 表示，`vm_end` 通常不包含在该区域内 。
2. **链表指针**
    - **`vm_next`**：指向链表中下一个 `vm_area_struct` 结构，用于将同一进程的虚拟内存区域以链表形式组织起来，方便遍历。 
    - **`vm_prev`**：指向链表中上一个 `vm_area_struct` 结构 。当 `vm_area_struct` 结构数目较少时，各结构按升序排序以单链表形式组织数据。

4. **所属进程及权限相关**
    - **`vm_mm`**：指向该虚拟内存区域所属进程的 `struct mm_struct` 结构，`mm_struct` 涵盖了与进程相关的所有内存区域信息 。
    - **`vm_page_prot`**：描述 `vma` 访问权限，用于创建区域中各页目录、页表项和存取控制标志，如r/w（读/写）、u/s（用户/超级用户）、a（访问过）、d（脏页）、g（全局）位等 。
    - **`vm_flags`**：保存 `vma` 标志位，指示该区域特性，
      
5. **文件映射相关**
    - **`vm_file`**：若 `vma` 描述的是文件映射的虚存空间，该指针指向被映射文件的 `file` 结构 。
    - **`vm_pgoff`**：指定文件映射的偏移量，单位是页面，即该虚存空间起始地址在 `vm_file` 文件里的文件偏移 。
    - **`vm_ops`**：`vma` 操作函数合集，常用于文件映射，比如当产生缺页异常时，通过 `vm_ops->nopage` 指向的函数将对应文件数据读取出来 。 

 
### 4-3 动态链接与动态库加载
#### 4-3-1 进程如何看到动态库
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/39add8a95cb24cad824e1e3502f4b1bf.png)


#### 4-3-2 进程间如何共享库的
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/1a30c97d6bdf43c68522f43ca5c42aac.png)


#### 4-3-3 动态链接
**概要**
动态链接其实远比静态链接要常用得多。比如我们查看下hello这个可执行程序依赖的动态库，会发现它就用到了一个c动态链接库：
```bash
$ ldd hello
        linux-vdso.so.1 (0x00007fffeb1ab000)
        libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f776af5000)
        /lib64/ld-linux-x86-64.so.2 (0x00007f776ec3000)
# ldd命令用于打印程序或者库文件所依赖的共享库列表。
```
这里的libc.so是C语言的运行时库，里面提供了常用的标准输入输出文件字符串处理等等这些功能。

那为什么编译器默认不使用静态链接呢？静态链接会将编译产生的所有目标文件，连同用到的各种库，合并形成一个独立的可执行文件，它不需要额外的依赖就可以运行。照理来说应该更加方便才对是吧？

**静态链接最大的问题在于生成的文件体积大，并且相当耗费内存资源**。随着软件复杂度的提升，我们的操作系统也越来越臃肿，不同的软件就有可能都包含了相同的功能和代码，显然会浪费大量的硬盘空间。

这个时候，动态链接的优势就体现出来了，我们可以将需要共享的代码单独提取出来，保存成一个独立的动态链接库，等到程序运行的时候再将它们加载到内存，这样不但可以节省空间，因为同一个模块在内存中只需要保留一份副本，可以被不同的进程所共享。

动态链接到底是如何工作的？？

首先要交代一个结论，**动态链接实际上将链接的整个过程推迟到了程序加载的时候**。比如我们去运行一个程序，操作系统会首先将程序的数据代码连同它用到的一系列动态库先加载到内存，其中每个动态库的加载地址都是不固定的，操作系统会根据当前地址空间的使用情况为它们动态分配一段内存。**当动态库被加载到内存以后，一旦它的内存地址被确定，我们就可以去修正动态库中的那些函数跳转地址了。**

##### **4-3-3-2 我们的可执行程序被编译器动了手脚**
```bash
$ ldd /usr/bin/ls
        linux-vdso.so.1 (0x00007fffd85f000)
        libselinux.so.1 => /lib/x86_64-linux-gnu/libselinux.so.1 (0x00007f42c025a000)
        libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f42c0068000)
        libpcre2-8.so.0 => /lib/x86_64-linux-gnu/libpcre2-8.so.0 (0x00007f42bffd7000)
        libdl.so.2 => /lib/x86_64-linux-gnu/libdl.so.2 (0x00007f42bffd1000)
        /lib64/ld-linux-x86-64.so.2 (0x00007f42c02b6000)
        libpthread.so.0 => /lib/x86_64-linux-gnu/libpthread.so.0 (0x00007f42bffae000)
$ ldd main.exe
        linux-vdso.so.1 (0x00007fff231d6000)
        libc.so.6 => /lib/x86_64-linux-gnu/libc.so.6 (0x00007f197ec3b000)
        /lib64/ld-linux-x86-64.so.2 (0x00007f197ee3e000)
```
在C/C++程序中，当程序开始执行时，它首先并不会直接跳转到main函数。实际上，程序的入口点是`_start`，这是一个由运行时库（通常是glibc）或链接器（如ld）提供的特殊函数。

在`_start`函数中，会执行一系列初始化操作，这些操作包括：
1. **设置堆栈**：为程序创建一个初始的堆栈环境。
2. **初始化数据段**：将程序的数据段（如全局变量和静态变量）从初始化数据段复制到相应的内存位置，并清零未初始化的数据段。
3. **动态链接**：这是关键的一步，`_start`函数会调用动态链接器的代码来解析和加载程序所依赖的动态库（shared libraries）。动态链接器会处理所有的符号解析和重定位，确保程序中的函数调用和变量访问能够正确地映射到动态库中的实际地址。
    - **动态链接器**：动态链接器（如ld-linux.so）负责在程序运行时加载动态库。当程序启动时，动态链接器会解析程序中的动态库依赖，并加载这些库到内存中。
    - **环境变量和配置文件**：Linux系统通过环境变量（如LD_LIBRARY_PATH）和配置文件（如/etc/ld.so.conf及其子配置文件）来指定动态库的搜索路径。这些路径会被动态链接器在加载动态库时搜索。
    - **缓存文件**：为了提高动态库的加载效率，Linux系统会维护一个名为/etc/ld.so.cache的缓存文件。该文件包含了系统中所有已知动态库的路径和相关信息，动态链接器在加载动态库时会首先搜索这个缓存文件。
4. **调用`__libc_start_main`**：一旦动态链接完成，`_start`函数会调用`__libc_start_main`（这是glibc提供的一个函数）。`__libc_start_main`函数负责执行一些额外的初始化工作，比如设置信号处理函数、初始化线程库（如果使用了线程）等。
5. **调用main函数**：最后，`__libc_start_main`函数会调用程序的main函数，此时程序的执行控制权才正式交给用户编写的代码。
6. **处理main函数的返回值**：当main函数返回时，`__libc_start_main`会负责处理这个返回值，并最终调用`_exit`函数来终止程序。

上述过程描述了C/C++程序在main函数之前执行的一系列操作，但这些操作对于大多数程序员来说是透明的。程序员通常只需要关注main函数中的代码，而不需要关心底层的初始化过程。然而，了解这些底层细节有助于更好地理解程序的执行流程和调试问题。

##### **4-3-3-3 动态库中的相对地址**
动态库为了随时进行加载，为了支持并映射到任意进程的任意位置，对动态库中的方法，统一编址，采用相对编址的方案进行编制的(其实可执行程序也一样，都要遵守平坦模式，只不过exe是直接加载的)。
```bash
# ubuntu下查看任意一个库的反汇编
objdump -S /lib/x86_64-linux-gnu/libc-2.31.so | less
objdump -S /lib/x86_64-linux-gnu/libc-2.31.so | less
objdump -S /lib/x86_64-linux-gnu/libc-2.31.so | less
# Centos下查看任意一个库的反汇编
objdump -S /lib64/libc-2.17.so | less
```

##### **8-3-3-4 我们的程序，怎么和库具体映射起来的**
**注意**：
- 动态库也是一个文件，要访问也是要被先加载，要加载也是要被打开的
- 让我们的进程找到动态库的本质：也是文件操作，不过我们访问库函数，通过虚拟地址进行跳转访问的，所以需要把动态库映射到进程的地址空间中 
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/500e891a7f6047e6adcd852d4a4312cd.png)


**8-3-3-5 我们的程序，怎么进行库函数调用**

**注意**：
- 库已经被我们映射到了当前进程的地址空间中
- 库的虚拟起始地址我们也已经知道了
- 库中每一个方法的偏移量地址我们也知道
- 所有：访问库中方法，只需要知道**库的起始虚拟地址+方法偏移量**即可定位库中的方法
- 而且：整个调用过程，是从代码区跳转到共享区，调用完毕在返回到代码区，整个过程完全在进程地址空间中进行的 

##### 4-3-3-6 全局偏移量表GOT（global offset table）
**注意**：
- 也就是说，我们的程序运行之前，先把所有库加载并映射，所有库的起始虚拟地址都应该提前知道
- 然后对我们加载到内存中的程序的库函数调用进行地址修改，在内存中二次完成地址设置（这个叫做加载地址重定位）
- 等等，修改的是代码区？不是说代码区在进程中是只读的吗？怎么修改？能修改吗？

所以：动态链接采用的做法是在.data（可执行程序中或者库自己）中**专门预留一片区域用来存放函数的跳转地址，它也被叫做全局偏移量表GOT**，表中每一项都是本运行模块要引用的一个全局变量或函数的地址。
- 因为.data区域是可读写的，所以可以支持动态进行修改
```bash
$ readelf -S a.out
...
[24].got             PROGBITS        0000000000003fb8  00002fb8
  0000000000000048  0000000000000008  WA       0     0     8
...
$ readelf -l a.out #.got在加载的时候，会和.data合并成为一个segment，然后加载在一起
...
  03     .init_array .fini_array .dynamic .got .data .bss
...
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/5c3eb6d0ac014fa4ada7b13af7e2b47d.png)

1. 由于代码段只读，我们不能直接修改代码段。但有了GOT表，代码便可以被所有进程共享。但在不同进程的地址空间中，各动态库的绝对地址不同，相对位置也不同。反映到GOT表上，就是每个进程的GOT表与代码段之间的相对位置可能不同，所以进程间不能共享GOT表。
2. 每个动态库都有独立的GOT表，由于GOT表与.text的相对位置是固定的，我们完全可以利用CPU的相对寻址来找到GOT表。
3. 在调用函数的时候会首先查表，然后根据表中的地址来进行跳转，这些地址在动态库加载的时候会被修改为真正的地址。
4. 这种方式实现的动态链接就被叫做PIC（Position - Independent Code，地址无关代码）。换句话说，我们的动态库不需要做任何修改，被加载到任意内存地址都能够正常运行，并且能够被所有进程共享，这也是为什么之前我们给编译器指定-fPIC参数的原因，PIC=相对编址+GOT。
```bash
$ objdump -S a.out
...
0000000000001050 <puts@plt>:
    1050:       f3 0f 1e fa           endbr64
    1054:       f2 ff 25 75 2f 00 00  bnd jmpq *0x2f75(%rip)        # 3fd0 <puts@GLIBC_2.2.5>
...
...
0000000000001149 <main>:
    1149:       f3 0f 1e fa           endbr64
    114d:       55                    push   %rbp
    114e:       48 89 e5              mov    %rsp,%rbp
    1151:       48 8d 3d ac 0e 00 00  lea    0xeac(%rip),%rdi        # 2004 <_IO_stdin_used+0x4>
    1158:       e8 f3 fe ff ff        callq  1050 <puts@plt>
...
```
总结一下：
- 文件打开与读取头部信息
- 内存映射
- 符号解析
将可执行程序中的符号引用与动态库中的实际符号进行链接（符号解析与重定位的过程）
	- 符号解析：
加载器遍分可执行程序文件和动态库的符号表。对于可执行程序中引用的动态库符旁加载器需要在动态库找到相关定义。
	- GOT:（全局偏移表）
位于进程的虚拟内有空间的数据段中，是ELF文件格式的一部分。
当程序使用动态库时，GOT就像是一个地址索引表、用于在运行时确走动态库中的函数和变量的实际内存地址。GOT是一个表结项、其中多含多个表项。每个表项对应一个需要在运行时进行地址解析的动态库符号（正数名／变量），存储运行时地址
- 重定位：
	- 生成占位符引用
在编译使用动态库的程序时、编译器会在目标文件（`.o`文件）或可执行文件中的每个引用的动态库出数和变量生成一个占位符。这些占位符会指向GOT相应表项。(此时GOT 还没有填入实际的符号地址）
	- 构建重定位信息
重定位表记录了需要重定位的位置和重定位的类型
	- 加载动态库到内存
	- 根据加载位置确定符号地址
加载器会根据动态库在内有的实际位置查找每个符号（函数和变量）的真实地址。
	- 填充GOT表项：
一旦我到了符号的实际地址，加载器就会将这些地址填充到GOT的相应表项中。
	- 正数和变量访问的地址查找机制。
当程序调用一个动态库正数时，调用指令会间接跳到GOT中相应表项所存储地址。全局变量访问也是类似情况。

##### 4-3-3-7 库间依赖
**注意**：
- 不仅仅有可执行程序调用库
- 库也会调用其他库！！库之间是有依赖的，如何做到库和库之间互相调用也是与地址无关的呢？？
- 库中也有GOT,和可执行一样！这也就是为什么大家为什么都是ELF的格式！
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/1e0ef101301f4e54b4fa08f5bec70ee7.png)




由于动态链接在程序加载的时候需要对大量函数进行重定位，这一步显然是非常耗时的。为了进一步降低开销，我们的操作系统还做了一些其他的优化，比如延迟绑定，或者也叫PLT（过程连接表，Procedure Linkage Table ）。

与其在程序一开始就对所有函数进行重定位，不如将这个过程推迟到函数第一次被调用的时候，因为绝大多数动态库中的函数可能在程序运行期间一次都不会被使用到。

思路是：GOT中的跳转地址默认会指向一段辅助代码，它也被叫做桩代码/stub。在我们第一次调用函数的时候，这段代码会负责查询真正函数的跳转地址，并且去更新GOT表。于是我们再次调用函数的时候，就会直接跳转到动态库中真正的函数实现。 

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/b508b01c007f4698a59228da96803e70.png)


总而言之，动态链接实际上将链接的整个过程，比如符号查询、地址的重定位从编译时推迟到了程序的运行时，它虽然牺牲了一定的性能和程序加载时间，但绝对是物有所值的。因为动态链接能够更有效的利用磁盘空间和内存资源，以极大方便了代码的更新和维护，更关键的是，它实现了二进制级别的代码复用。

**解析依赖关系的时候，就是加载并完善互相之间的GOT表的过程.**

#### 4-3-4 总结
- 静态链接的出现，提高了程序的模块化水平。对于一个大的项目，不同的人可以独立地测试和开发自己的模块。通过静态链接，生成最终的可执行文件。
- 我们知道静态链接会将编译产生的所有目标文件，和用到的各种库合并成一个独立的可执行文件，其中我们会去修正模块间函数的跳转地址，也被叫做编译重定位(也叫做静态重定位)。
- 而动态链接实际上将链接的整个过程推迟到了程序加载的时候。比如我们去运行一个程序，操作系统会首先将程序的数据代码连同它用到的一系列动态库先加载到内存，其中每个动态库的加载地址都是不固定的，但是无论加载到什么地方，都要映射到进程对应的地址空间，然后通过.GOT方式进行调用(运行重定位，也叫做动态地址重定位)。 
