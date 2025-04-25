#include <stdio.h>
#include <string.h>
int main()
{
    FILE *fp = fopen("myfile", "r");
    if (!fp)
    {
        printf("fopen error!\n");
        return 1;
    }
    char buf[1024];
    const char *msg = "hello bit!\n";

    while (1)
    {
        // 注意返回值和参数，此处有坑，仔细查看man手册关于该函数的说明
        size_t s = fread(buf, 1, strlen(msg), fp);

        // size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
        // ptr：指向一块内存区域的指针，用于存储从文件中读取的数据。这块内存区域的大小至少要能容纳 size * nmemb 字节的数据。
        // size：每个数据项的大小（以字节为单位）。
        // nmemb：要读取的数据项的数量。
        // stream：指向 FILE 对象的指针，表示要从中读取数据的文件流。

        // fread 函数返回实际成功读取的数据项的数量（不是字节数）。
        if (s > 0)
        {
            buf[s] = 0;
            printf("%s", buf);
        }
        if (feof(fp)) // 检测文件流是否已经到达文件末尾
        {
            break;
        }
    }
    fclose(fp);
    return 0;
}