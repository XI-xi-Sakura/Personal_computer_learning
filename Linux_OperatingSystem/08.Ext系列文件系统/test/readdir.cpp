#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/types.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    // 检查命令行参数数量是否正确
    if (argc != 2)
    {
        // 若参数数量不正确，向标准错误输出使用说明
        fprintf(stderr, "Usage: %s <directory>\n", argv[0]);
        // 以失败状态退出程序
        exit(EXIT_FAILURE);
    }

    // 调用 opendir 系统调用打开指定目录，返回一个指向 DIR 结构的指针
    DIR *dir = opendir(argv[1]);
    // 检查目录是否成功打开
    if (!dir)
    {
        // 若打开失败，输出错误信息
        perror("opendir");
        // 以失败状态退出程序
        exit(EXIT_FAILURE);
    }

    // 定义一个指向 dirent 结构的指针，用于存储目录项信息
    struct dirent *entry;
    // 循环读取目录中的每一个条目，直到 readdir 返回 NULL 表示读完
    while ((entry = readdir(dir)) != NULL)
    {
        // 跳过当前目录（.）和上级目录（..）这两个特殊条目
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        // 输出文件名和对应的 inode 号
        printf("Filename: %s, Inode: %lu\n", entry->d_name, (unsigned long)entry->d_ino);
    }

    // 关闭已打开的目录
    closedir(dir);
    // 正常退出程序
    return 0;
}