// 例子: 从键盘读取数据，写入管道，读取管道，写到屏幕
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <iostream>

int main(void)
{
    int fds[2];
    char buf[100];
    int len;

    if (pipe(fds) == -1)
    {
        perror("make pipe");
        exit(1);
    }
    else
    {
        std::cout << fds[0] <<" " << fds[1] << std::endl;
    }
    // read from stdin
    while (fgets(buf, 100, stdin))
    {
        len = strlen(buf);
        // write into pipe
        if (write(fds[1], buf, len) != len)
        {
            perror("write to pipe");
            break;
        }
        memset(buf, 0x00, sizeof(buf));
        // read from pipe
        if ((len = read(fds[0], buf, 100)) == -1)
        {
            perror("read from pipe");
            break;
        }
        // write to stdout
        if (write(1, buf, len) != len)
        {
            perror("write to stdout");
            break;
        }
    }

    return 0;
}