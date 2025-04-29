#include <iostream>
#include <sys/types.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>
#include <cstring>

const char *PATH = ".";
const int PROJ_ID = 'a';
const int SHM_SIZE = 1024;

int main()
{
    // 生成唯一键值
    key_t key = ftok(PATH, PROJ_ID);
    if (key == -1)
    {
        perror("ftok");
        return 1;
    }

    // 创建共享内存
    int shmid = shmget(key, SHM_SIZE, IPC_CREAT | 0666);
    if (shmid == -1)
    {
        perror("shmget");
        return 1;
    }

    // 将共享内存附加到进程地址空间
    char *shm_addr = static_cast<char *>(shmat(shmid, nullptr, 0));
    if (shm_addr == reinterpret_cast<char *>(-1))
    {
        perror("shmat");
        return 1;
    }

    // 向共享内存写入数据
    const char *message = "Hello, shared memory!";
    strcpy(shm_addr, message);
    std::cout << "Writer: Data written to shared memory." << std::endl;

    // 将共享内存从进程地址空间分离
    if (shmdt(shm_addr) == -1)
    {
        perror("shmdt");
        return 1;
    }

    return 0;
}