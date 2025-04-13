#include "process.h"
#include <string.h>
#include <unistd.h>

#define NUM 101
#define STYLE '='

// vesion1
void process_v1()
{
    char buffer[NUM];
    memset(buffer, 0, sizeof(buffer));
    const char *lable="|/-\\\\";
    int len = strlen(lable);

    int cnt = 0;
    while(cnt <= 100)
    {
        printf("[%-100s][%d%%][%c]\r", buffer, cnt, lable[cnt%len]);
        fflush(stdout);
        buffer[cnt]= STYLE;
        cnt++;
        usleep(50000);
    }
    printf("\n");
}

// verison2
void FlushProcess(double total, double current)
{
    char buffer[NUM];
    memset(buffer, 0, sizeof(buffer));
    const char *lable="|/-\\\\";
    int len = strlen(lable);

    static int cnt = 0;

    // 不需要自己循环,填充#
    int num = (int)(current*100/total); // 11.0 / 1000
    int i = 0;
    for(; i < num; i++)
    {
        buffer[i] = STYLE;
    }
    double rate = current/total;
    cnt %= len;
    printf("[%-100s][%.1f%%][%c]\r", buffer, rate*100, lable[cnt]);
    cnt++;
    fflush(stdout);
}