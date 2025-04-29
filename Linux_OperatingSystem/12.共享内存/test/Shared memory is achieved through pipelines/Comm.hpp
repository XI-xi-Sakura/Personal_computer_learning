#pragma once
#include <fcntl.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cassert>
#include <cstdio>
#include <ctime>
#include <cstring>
#include <iostream>

using namespace std;

// 日志

#define Debug 0
#define Notice 1
#define Warning 2
#define Error 3
const std::string msg[] = {
    "Debug",
    "Notice",
    "Warning",
    "Error"};
std::ostream &Log(std::string message, int level)
{
    std::cout << " | " << (unsigned)time(nullptr) << " | " << msg[level] << " | " << message;
    return std::cout;
}




#define PATH_NAME "/home/hyb"
#define PROJ_ID 0x66
#define SHM_SIZE 4096 // 共享内存的大小，最好是页(PAGE: 4096)的整数倍
#define FIFO_NAME "./fifo"
class Init
{
public:
    Init()
    {
        int n = mkfifo(FIFO_NAME, 0666);
        assert(n == 0);
        (void)n;
        Log("create fifo success", Notice) << "\n";
    }
    ~Init()
    {
        unlink(FIFO_NAME);
        Log("remove fifo success", Notice) << "\n";
    }
};


#define READ O_RDONLY
#define WRITE O_WRONLY
int OpenFIFO(std::string pathname, int flags)
{
    int fd = open(pathname.c_str(), flags);
    assert(fd >= 0);
    return fd;
}
void CloseFifo(int fd)
{
    close(fd);
}
void Wait(int fd)
{
    Log("等待中...", Notice) << "\n";
    uint32_t temp = 0;
    ssize_t s = read(fd, &temp, sizeof(uint32_t));
    assert(s == sizeof(uint32_t));
    (void)s;
}
void Signal(int fd)
{
    uint32_t temp = 1;
    ssize_t s = write(fd, &temp, sizeof(uint32_t));
    assert(s == sizeof(uint32_t));
    (void)s;
    Log("唤醒中...", Notice) << "\n";
}
string TransToHex(key_t k)
{
    char buffer[32];
    snprintf(buffer, sizeof buffer, "0x%x", k);
    return buffer;
}