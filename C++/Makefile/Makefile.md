## 语法规则

```
目标 ... : 依赖 ...
	命令1
	命令2
	. . .
```
- `目标`即要生成的文件。如果目标文件的更新时间晚于依赖文件更新时间，则说明依赖文件没有改动，目标文件不需要重新编译。否则会进行重新编译并更新目标文件。
- `依赖`：即目标文件由哪些文件生成。
- `命令`：即通过执行命令由依赖文件生成目标文件。注意每条命令之前必须有一个tab保持缩进，这是语法要求
- `all`：Makefile文件默认只生成第一个目标文件即完成编译，但是我们可以通过all 指定所需要生成的目标文件。

```
all: target1 target2 target3
target1:
# 编译规则1
target2:
# 编译规则2
target3:
# 编译规则3

```

>all被设置为第一个目标，并且target1、target2和target3被列为all的依赖。当你在命令行中运行make时，make命令会寻找并执行all目标规则，这将依次执行target1、target2和target3的编译规则。
因此，通过在Makefile中设置all作为默认目标规则，你可以简化构建过程，只需运行make命令即可执行整个编译过程，无需显式指定目标
## 变量
`$`符号表示**取变量的值**，当变量名多于一个字符时，使用"( )"

`$`符的其他用法
- $^ 表示所有的依赖文件
- $@ 表示生成的目标文件
- $< 代表第一个依赖文件

```
SRC = $(wildcard *.c)
OBJ = $(patsubst %.c, %.o, $(SRC))
 
ALL: hello.out
 
hello.out: $(OBJ)
        gcc $< -o $@
 
%.o: %.c
        gcc -c $< -o $@

```

## 变量赋值
1、"`=`"是最普通的等号，在Makefile中容易搞错赋值等号，使用 “=”进行赋值，变量的值是整个Makefile中最后被指定的值。

```
VIR_A = A
VIR_B = $(VIR_A) B
VIR_A = AA
```

经过上面的赋值后，最后VIR_B的值是**AA B**，而不是**A B**，在make时，会把整个Makefile展开，来决定变量的值。

2、“`:=`” 表示直接赋值，赋予当前位置的值。

```
VIR_A := A
VIR_B := $(VIR_A) B
VIR_A := AA
```
最后BIR_B的值是A B，即根据当前位置进行赋值。因此相当于“`=`”，“`：=`”才是真正意义上的直接赋值。

3、“?=” 表示如果该变量没有被赋值，赋值予等号后面的值。

```
VIR ?= new_value
```

## 预定义变量

CC：c编译器的名称，默认值为cc。.cpp .c预编译器的名称默认值为$(CC) -E

```
CC = gcc
```

回显问题，Makefile中的命令都会被打印出来。如果不想打印命令部分 可以使用`@`去除回显

```
@echo "clean done!"
```
## 函数
- 通配符

```
SRC = $(wildcard ./*.c)
```
>$(wildcard)函数来获取当前目录下所有.c源文件，并且把这些文件路径赋值给变量SRC。
- 匹配目录下所有.c 文件，并将其赋值给SRC变量。

```
OBJ = $(patsubst %.c, %.o, $(SRC))

```
>`patsubst` 是 Makefile 里的一个函数，其作用是对字符串进行模式替换。具体到这行代码，它把 $(SRC) 变量里所有以 .c 结尾的文件名替换成以 .o 结尾的文件名，再把结果赋值给 OBJ 变量。

示例：如果目录下有很多个.c 源文件，就不需要写很多条规则语句了，而是可以像下面这样写.

```
SRC = $(wildcard *.c)
OBJ = $(patsubst %.c, %.o, $(SRC))
 
ALL: hello.out
 
hello.out: $(OBJ)
        gcc $(OBJ) -o hello.out
 
%.o: %.c
        gcc -c $< -o $@

```

- `ALL: hello.out`：定义了默认目标 ALL，它依赖于 hello.out，这意味着执行 make 命令时，会尝试构建 hello.out。
- `hello.out: $(OBJ)`：`hello.out` 依赖于所有的目标文件 `$(OBJ)`。当目标文件有更新时，会执行下面的命令来链接生成 `hello.out`。
- `gcc $(OBJ) -o hello.out`：将所有目标文件链接成可执行文件 `hello.out`。
- `%.o: %.c`：这是一个模式规则，用于将 `.c 文件`编译成 `.o 文件`。`$<` 表示第一个依赖文件（即 .c 文件），$@ 表示目标文件（即 .o 文件）。
- `gcc -c $< -o $@`：使用 gcc 编译器对 .c 文件进行编译，生成对应的 .o 文件。


## 伪目标 .PHONY
伪目标只是一个标签，clean是个伪目标没有依赖文件，只有用make来调用时才会执行.

当目录下有与make 命令 同名的文件时 执行make 命令就会出现错误。
解决办法就是使用伪目标.

```
SRC = $(wildcard *.c)
OBJ = $(patsubst %.c, %.o, $(SRC))
 
ALL: hello.out
 
hello.out: $(OBJ)
        gcc $< -o $@
 
$(OBJ): $(SRC)
        gcc -c $< -o $@
 
clean:
        rm -rf $(OBJ) hello.out
```

## 指定头文件路径

一般都是通过"-I"（大写i）来指定，假设头文件在：

```
/home/develop/include
```

则可以通过-I指定：

```
-I/home/develop/include
```

将该目录添加到头文件搜索路径中，在Makefile中则可以这样写：

```
CFLAGS=-I/home/develop/include
```

然后在编译的时候，引用CFLAGS即可，如下

```
yourapp:*.c
    gcc $(CFLAGS) -o yourapp
```

## 指定库文件路径

与上面指定头文件类似只不过使用的是"-L"来指定

```
LDFLAGS=-L/usr/lib -L/path/to/your/lib
```

告诉链接器要链接哪些库文件，使用"-l"（小写L）如下：

```
LIBS = -lpthread -liconv
```

