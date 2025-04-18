﻿## Linux系统初步介绍
Linux是一种自由和开放源代码的类UNIX操作系统,该操作系统的内核由林纳斯托瓦兹在1991年首次发布,之后,在加上用户空间的应用程序之后,就成为了Linux操作系统｡

严格来讲,Linux 只是操作系统内核本身,但通常采用“Linux内核”来表达该意思｡而Linux则常用来指基于 Linux内核的完整操作系统,它包括GUI组件和许多其他实用工具｡​

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/bb5288a906c24c4faecc5eeb6030a9ab.png)
使计算机更好用! 这是操作系统的根本要义!!​

## Linux下基本指令​

### ls 指令

语法：`ls [选项] [目录或文件]`

功能：对于目录，列出该目录下的所有子目录与文件；对于文件，列出文件名及其他信息。

常用选项：
- `-a` 列出所有文件，包括隐含文件；
- `-d` 将目录像文件一样显示；
- `-i` 输出文件的 i 节点索引信息；
- `-k` 以 k 字节表示文件大小；
- `-l` 列出文件详细信息；
- `-n` 用数字的 UID、GID 代替名称；
- `-F` 在文件名后附上字符说明文件类型；`*`表示可执行的普通文件;`/`表示 目录;`@`表示符号链接;“`|`表示FIFOs（进程间通信）;`=`表示sockets（套接字）｡(目录类型识别)​
- `-r` 对目录反向排序；
- `-t` 以时间排序；
- `-s` 在文件名后输出文件大小；
- `-R` 列出所有子目录下的文件；
- `-1` 一行只输出一个文件。

### pwd
语法：`pwd`
功能：显示用户当前所在的目录

### cd
**Linux 理论知识：路径的认识**：
- Linux 系统中文件和目录组成目录树，普通文件是叶子节点，目录可能是叶子或路上节点。

- 路径分为绝对路径（从 / 开始，不依赖其他目录定位文件）和相对路径（相对于当前用户所处目录定位文件），绝对路径常用于特定服务配置文件，相对路径在命令行中使用较多。
-  理解路径存在的意义:树状组织方式，都是为了保证快速定位查找到指定的文件，而定位文件就需要具有唯一性的方案来进行定位文件。其中任何一个节点，都只有一个父节点，所以，从根目录开始，定位指定文件，路径具有唯一性。

语法：`cd 目录名`
功能：改变工作目录
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/972b6966d68d499599de664892771d27.png)


### touch 指令
Linux 理论知识：文件类型的认识
语法：touch [选项]... 文件...
功能：更改文档或目录的日期时间（包括获取时间和更改时间），或新建一个不存在的文件。

常用选项：
- `-a` 仅更改存取时间；
- `-c` 仅更改修改时间。

### mkdir 指令


语法：`mkdir [选项] dirname...`

功能：在当前目录下创建名为 “dirname” 的目录

常用选项：-p/--parents 可自动建立路径中不存在的目录，可一次创建多个目录。



### rmdir 指令 && rm 指令
`rmdir` 指令与mkdir相对应的命令

语法：`rmdir [-p] [dirName]`

适用对象：具有当前目录操作权限的所有使用者

功能：删除空目录

常用选项：-p 当子目录删除后父目录为空时，连带父目录一起删除。

`rm` 命令

语法：rm [-f -i -r -v] [dirName/dir]

适用对象：所有使用者

功能：删除文件或目录

常用选项：
- `-f` 直接删除，即使文件只读；
- `-i` 删除前询问确认；
- `-r` 删除目录及其下所有文件。

### man 指令

语法：`man [选项] 命令`

常用选项：
- -k 根据关键字搜索联机帮助；
- num 只在第 num 章节查找；
- -a 显示所有章节。

解释一下：man 手册分为 9 章（不同系统可能有差别），1 是普通命令，2 是系统调用，3 是库函数，4 是特殊文件，5 是文件格式，6 是游戏相关，7 是附件和全局变量，8 是系统管理用的命令（只能由 root 使用，例如`ifconfig`），9 略。

### cp 指令

语法：`cp [选项] 源文件或目录 目标文件或目录`

功能：复制文件或目录

说明：可将多个文件或目录复制到指定目录。

常用选项：
- -f 强行复制；
- -i 覆盖文件前询问用户；
- -r 递归处理目录。

### mv 指令

语法：`mv [选项] 源文件或目录 目标文件或目录`

功能： 
- 视mv命令中第二个参数类型的不同（是目标文件还是目标目录），mv命令将文件重命名或将其移至一个新的目录中。
- 当第二个参数类型是文件时，mv命令完成文件重命名，此时，源文件只能有一个（也可以是源目录名），它将所给的源文件或目录重命名为给定的目标文件名。
- 当第二个参数是已存在的目录名称时，源文件或目录参数可以有多个，mv命令将各参数指定的源文件均移至目标目录中

常用选项：
- -f 强制覆盖；
- -i 目标文件存在时询问是否覆盖。

### cat 指令

语法：cat [选项] [文件]
功能：查看目标文件的内容
常用选项：
- -b 对非空输出行编号；
- -n 对所有输出行编号；
- -s 不输出多行空行。


### more 指令

语法：more [选项]
功能：类似 cat 命令
常用选项：-n 指定输出行数；q 退出 more。


### less 指令

语法：less [参数] 文件
功能：对文件或输出进行分页显示，使用less可以任意浏览文件，而more只能向前移动却不能向前移动
选项：
- -i 忽略搜索大小写；
- -N 显示行号；
- / 字符串向下搜索；
- ? 字符串向上搜索；
- n 重复前一个搜索；
- N 反向重复前一个搜索；
- q 退出。



### head 指令

语法：`head [参数]... [文件]...`

功能：显示档案开头至标准输出，默认打印开头 10 行。

选项：-n <行数> 显示指定行数。

### tail 指令

语法：`tail 必要参数 [文件]`

功能：显示指定文件末尾内容，常用于查看日志文件，-f 选项可循环读取。

选项：-f 循环读取；-n <行数> 显示指定行数。




### date指令
指定格式显示时间：`date +%Y:%m:%d`
用法：`date [OPTION]... [+FORMAT]`
1. 在显示方面，使用者可以设定欲显示的格式，格式设定为一个加号后接数个标记，其中常用的标记有：
	- %H（小时，00..23）、
	- %M（分钟，00..59） 、
	- %S（秒，00..61） 、
	- %X（相当于 %H:%M:%S）、
	- %d（日，01..31） 、
	- %m（月份，01..12） 、
	- %Y（完整年份，0000..9999） 、
	- %F（相当于 %Y-%m-%d）。
2. 在设定时间方面：`date -s`用于设置当前时间，只有root权限才能设置，其他用户只能查看。可以通过`date -s`加上具体时间或日期时间组合来设置时间，如设置成20080523会把具体时间设置成空00:00:00 ；设置具体时间如01:01:01不会对日期做更改；设置全部时间有多种格式，如“01:01:01 2008-05-23”“01:01:01 20080523”“2008-05-23 01:01:01”“20080523 01:01:01”等。
3. 时间戳相关：时间转时间戳使用date +%s；时间戳转时间使用date -d@加上时间戳数值。Unix时间戳是从1970年1月1日（UTC/GMT的午夜）开始所经过的秒数，不考虑闰秒。

### cal指令
cal命令用于显示公历（阳历）日历。公历是现在国际通用的历法，又称格列历，通称阳历，“阳历”又名“太阳历”，系以地球绕行太阳一周为一年，为西方各国所通用，故又名“西历”。

命令格式：`cal 参数 [年份]`
功能是用于查看日历等时间信息，如只有一个参数，则表示年份（1 - 9999），如有两个参数，则表示月份和年份。
常用选项：
 - -3：显示系统前一个月，当前月，下一个月的月历。
 - -j：显示在当年中的第几天（一年日期按天算，从1月1号算起，默认显示当前月在一年中的天数）。
 - -y：显示当前年份的日历 。

### find指令
Linux下find命令在目录结构中搜索文件，并执行指定的操作。

该命令提供了相当多的查找条件，功能强大，选项也很多。即使系统中含有网络文件系统（NFS），在具有相应权限时，find命令在该文件系统中同样有效。由于遍历大文件系统可能耗时较长（30G字节以上的文件系统），运行消耗资源大的find命令时，很多人倾向于放在后台执行。

语法：`find pathname -options`。

功能是用于在文件树中查找文件，并作出相应的处理（可能访问磁盘）。常用选项中，-name用于按照文件名查找文件，其他选项较为复杂需另行查阅 。

### which指令
功能：搜索系统指定的命令。

### whereis指令
功能：用于找到程序的源、二进制文件或手册。

### alias指令
功能：设置命令的别名。

### grep指令
语法：grep [选项] 搜寻字符串 文件。功能是在文件中搜索字符串，将找到的行打印出来。
常用选项：
 - -i：忽略大小写的不同，即大小写视为相同。
 - -n：顺便输出行号。
 - -v：反向选择，亦即显示出没有 '搜寻字符串' 内容的那一行。

### zip/unzip指令
语法：zip 压缩文件.zip 目录或文件。功能是将目录或文件压缩成zip格式。常用选项 -r表示递归处理，将指定目录下的所有文件和子目录一并处理。解压时使用unzip命令，如解压到tmp目录：unzip test2.zip -d /tmp。

### 关于rzsz
这个工具用于windows机器和远端的Linux机器通过XShell传输文件。安装完毕之后可以通过拖拽的方式将文件上传过去。

安装命令为：



```
sudo yum/apt install -y lrzlz。
```

### tar指令（重要）
用于打包/解包，不打开它，直接看内容。

语法：tar [-c x t z j v f] 文件与目录 ....

参数：
 - -c：建立一个压缩文件的参数指令（create的意思）。
 - -x：解开一个压缩文件的参数指令。
 - -t：查看tarfile里面的文件。
 - -z：是否同时具有gzip的属性，即是否需要用gzip压缩。
 - -j：是否同时具有bzip2的属性，即是否需要用bzip2压缩。
 - -v：压缩的过程中显示文件，常用但不建议用在背景执行过程。
 - -f：使用档名，注意在f之后要立即接档名，不要再加参数。
 - -C：解压到指定目录。

### bc指令
bc命令可以很方便的进行浮点运算。

### uname –r指令
语法：uname [选项] 。功能是uname用来获取电脑和操作系统的相关信息，可显示linux主机所用的操作系统的版本、硬件的名称等基本信息。
常用选项：-a或–all详细输出所有信息，依次为内核名称，主机名，内核版本号，内核版本，硬件名，处理器类型，硬件平台类型，操作系统名称 。

### 重要的几个热键 [Tab], [ctrl]-c, [ctrl]-d
[Tab]按键具有“命令补全”和“档案补齐”的功能；[Ctrl]-c按键用于让当前的程序“停掉”；[Ctrl]-d按键通常代表着“键盘输入结束（End Of File，EOF或End OfInput）”的意思，也可以用来取代exit。

### 关机
语法：shutdown [选项]
常见选项：
 - -h：将系统的服务停掉后，立即关机。
 - -r：在将系统的服务停掉之后就重新启动。
 - -t sec：-t后面加秒数，即“过几秒后关机”的意思。

## shell命令以及运行原理
Linux严格意义上是一个操作系统核心（kernel），一般用户不能直接使用kernel，而是通过shell（命令行解释器）与kernel沟通。

Shell主要功能是将使用者的命令翻译给核心处理，同时将核心的处理结果翻译给使用者，类似Windows的图形接口，在用户和内核之间起到交互作用。

从技术角度：Shell的最简单定义：命令行解释器

## Linux权限的概念
- Linux下有超级用户（root）和普通用户。

- 超级用户可以在linux系统下做任何事情，不受限制；

- 普通用户在linux下做有限的事情。超级用户的命令提示符是“#”，普通用户的命令提示符是“$”。


- 命令su [用户名]用于切换用户，如从root用户切换到普通用户user使用su user；从普通用户切换到root用户使用su root（root可省略），此时系统会提示输入root用户的口令。

## Linux权限管理

### 文件访问者的分类
文件和文件目录的所有者（u，User）、文件和文件目录的所有者所在的组的用户（g，Group）、其它用户（o，Others）。

### 文件类型和访问权限*

   - 文件类型：
    	- d表示文件夹、
    	- -表示普通文件、
    	- l表示软链接（类似Windows的快捷方式）、
    	- b表示块设备文件（例如硬盘、光驱等）、
    	- p表示管道文件、
    	- c表示字符设备文件（例如屏幕等串口设备）、
    	- s表示套接口文件。
   - 基本权限：
    	- 读（r/4），对文件而言具有读取文件内容的权限，对目录来说具有浏览该目录信息的权限；
    	- 写（w/2），对文件而言具有修改文件内容的权限，对目录来说具有删除移动目录内文件的权限；
    	- 执行（x/1），对文件而言具有执行文件的权限，对目录来说具有进入目录的权限，“-”表示不具有该项权限 。

### 文件权限值的表示方法

字符表示方法和8进制数值表示方法。

### 文件访问权限的相关设置方法

   - chmod：功能是设置文件的访问权限。格式为`chmod [参数] 权限 文件名`，常用选项R表示递归修改目录文件的权限。权限值格式有用户表示符+/-=权限字符，如u+w表示给拥有者增加写权限，o-x表示取消其他用户的执行权限，a=x表示给所有用户赋予执行权限；也可以用三位8进制数字表示，如664、640等 。示例：`chmod 664 /home/test.txt`

   - chown：功能是修改文件的拥有者，格式为`chown [参数] 用户名 文件名`，例如chown user1 f1 、chown -R user1 filegroup1。

   - chgrp：功能是修改文件或目录的所属组，格式为`chgrp [参数] 用户组名 文件名`，常用选项： -R递归修改文件或目录的所属组，如chgrp users /abc/f2。

   - umask：功能是查看或修改文件掩码。新建文件夹默认权限是0666，新建目录默认权限是0777，但实际创建的文件和目录权限受umask影响，实际创建的出来的文件权限是mask & ~umask 。格式为umask 权限值，超级用户默认掩码值为0022，普通用户默认为0002。

### file指令
功能说明：辨识文件类型。

语法：`file [选项] 文件或目录...`。


常用选项：
- -c详细显示指令执行过程，便于排错或分析程序执行的情形；
- -z尝试去解读压缩文件的内容。

### 使用sudo分配权限
1. 修改/etc/sudoers文件分配文件，如chmod 740 /etc/sudoers，然后vi /etc/sudoer进行编辑，格式为接受权限的用户登陆的主机 =(执行命令的用户) 命令。
2. 使用sudo调用授权的命令，格式为$ `sudo –u用户名 命令`，例如$ sudo -u root /usr/sbin/useradd u2。

### 目录的权限
可执行权限决定能否cd到目录中；可读权限决定能否用ls等命令查看目录中的文件内容；可写权限决定能否在目录中创建或删除文件。但只要用户具有目录的写权限，就可以删除目录中的文件，而不论这个用户是否有这个文件的写权限。

### 关于权限的总结
目录的可执行权限表示能否在目录下执行命令，如果目录没有 -x权限，则无法对目录执行任何命令，甚至无法cd进入目录，即使目录有 -r读权限；如果目录具有 -x权限，但没有 -r权限，则用户可以执行命令，可以cd进入目录，但无法读出目录下的文档。


