// #include <stdio.h>

// #include <unistd.h>

// int main()
// {
//     printf("pid: %d\n", getpid());
//     printf("ppid: %d\n", getppid());
//     return 0;
// }

#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>

// int count = 0;

// int main()
// {
//     pid_t pid;

//     pid = fork();

//     if (pid < 0)
//     {
//         // 处理错误
//         perror("fork");
//         return 1;
//     }
//     else if (pid == 0)
//     {
//         // 子进程
//         while (1)
//         {
//             printf("子进程: 我的 PID 是 %d，我的父进程 PID 是 %d,count:%d\n", getpid(), getppid(), count);
//             sleep(1);
//         }
//         printf("子进程: 我的 PID 是 %d，我的父进程 PID 是 %d\n", getpid(), getppid());
//     }
//     else
//     {
//         // 父进程
//         while (1)
//         {
//             count++;
//             printf("父进程: 我的 PID 是 %d，我的父进程 PID 是 %d,count:%d\n", getpid(), pid, count);
//             sleep(1);
//         }
//     }

//     return 0;
// }


// #include <stdio.h>
// #include <stdlib.h>

// int main()
// {
//     pid_t id = fork();

//     if(id < 0){
//         perror("fork");
//         return 1;
//     }
//     else if(id > 0){ //parent
//         printf("parent[%d] is sleeping...,parentpid is %d\n", getpid(), getppid());
//         sleep(3);
//     }
//     else{
//         printf("child[%d] is begin Z...,parentpid is %d\n", getpid(), getppid());
//         sleep(5);
//         exit(EXIT_SUCCESS);
//     }
//     return 0;
// }