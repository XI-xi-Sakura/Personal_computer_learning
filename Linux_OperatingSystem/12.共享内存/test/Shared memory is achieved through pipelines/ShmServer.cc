#include "Comm.hpp"
Init init;
int main()
{
    // 1. 创建公共的Key值
    key_t k = ftok(PATH_NAME, PROJ_ID);
    assert(k != -1);
    Log("create key done", Debug) << " server key : " << TransToHex(k) << endl;
    // 2. 创建共享内存 -- 建议要创建一个全新的共享内存 -- 通信的发起者
    int shmid = shmget(k, SHM_SIZE, IPC_CREAT | IPC_EXCL | 0666);
    if (shmid == -1)
    {
        perror("shmget");
        exit(1);
    }
    Log("create shm done", Debug) << " shmid : " << shmid << endl;
    // 3. 将指定的共享内存，挂接到自己的地址空间
    char *shmaddr = (char *)shmat(shmid, nullptr, 0);
    Log("attach shm done", Debug) << " shmid : " << shmid << endl;
    // 4. 访问控制
    int fd = OpenFIFO(FIFO_NAME, O_RDONLY);
    while (true)
    {
        // 阻塞
        Wait(fd);
        // 临界区
        printf("%s\n", shmaddr);
        if (strcmp(shmaddr, "quit") == 0)
            break;
    }
    CloseFifo(fd);
    // 5. 将指定的共享内存，从自己的地址空间中去关联
    int n = shmdt(shmaddr);
    assert(n != -1);
    (void)n;
    Log("detach shm done", Debug) << " shmid : " << shmid << endl;
    // 6. 删除共享内存，IPC_RMID, 即便是有进程和当下的shm挂接，依旧删除共享内存
    n = shmctl(shmid, IPC_RMID, nullptr);
    assert(n != -1);
    (void)n;
    Log("delete shm done", Debug) << " shmid : " << shmid << endl;
    return 0;
}