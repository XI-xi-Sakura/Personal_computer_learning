#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

void handler(int sig)
{
    pid_t id = waitpid(-1, NULL, WNOHANG);
    while (id > 0)
    {
        printf("wait child success: %d\n", id);
        id = waitpid(-1, NULL, WNOHANG);
    }
    printf("child is quit! \n");
}

int main()
{
    signal(SIGCHLD, handler);
    pid_t cid;
    if ((cid = fork()) == 0)
    {
        printf("child id: %d\n", getpid());
        sleep(3);
        exit(1);
    }
    while (1)
    {
        printf("father proc is doing some thing!\n");
        sleep(1);
    }
    return 0;
}