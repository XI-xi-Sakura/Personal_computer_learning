#include "Comm.hpp"
int main()
{
    // 1. 创建公共的Key值
    key_t k = ftok(PATH_NAME, PROJ_ID);
    if (k < 0)
    {
        Log("create key failed", Error) << " client key : " << TransToHex(k) << endl;
        exit(1);
    }
    Log("create key done", Debug) << " client key : " << TransToHex(k) << endl;
    // 2. 获取共享内存
    int shmid = shmget(k, SHM_SIZE, 0);
    if (shmid < 0)
    {
        Log("create key failed", Error) << " client key : " << TransToHex(k) << endl;
        exit(2);
    }
    Log("create shm success", Error) << " client key : " << TransToHex(k) << endl;
    // 3. 挂接共享内存
    char *shmaddr = (char *)shmat(shmid, nullptr, 0);
    if (shmaddr == (char *)-1)
    {
        Log("attach shm failed", Error) << " client key : " << TransToHex(k) << endl;
        exit(3);
    }
    Log("attach shm success", Error) << " client key : " << TransToHex(k) << endl;
    // 4. 写
    int fd = OpenFIFO(FIFO_NAME, O_WRONLY);
    while (true)
    {
        ssize_t s = read(0, shmaddr, SHM_SIZE - 1);
        if (s > 0)
        {
            shmaddr[s - 1] = 0;
            Signal(fd);
            if (strcmp(shmaddr, "quit") == 0)
                break;
        }
    }
    CloseFifo(fd);
    // 5. 去关联
    int n = shmdt(shmaddr);
    assert(n != -1);
    Log("detach shm success", Error) << " client key : " << TransToHex(k) << endl;
    return 0;
}