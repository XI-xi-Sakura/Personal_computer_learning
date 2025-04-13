#include "process.h"
#include <stdio.h>
#include <unistd.h>

double total = 1024.0;
double speed = 1.0;

void DownLoad()
{
    double current = 0;
    while(current <= total)
    {
        FlushProcess(total, current);
        // 下载代码
        usleep(3000); // 充当下载数据
        current += speed;
    }
    printf("\ndownload %.2lfMB Done\n", current);
}

int main()
{
    DownLoad();
    DownLoad();
    DownLoad();
    DownLoad();
    DownLoad();
    DownLoad();
    DownLoad();
    return 0;
}