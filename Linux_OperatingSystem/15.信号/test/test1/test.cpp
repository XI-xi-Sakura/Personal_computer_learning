#include <iostream>
#include <unistd.h>
#include <signal.h>

// int main()
// {
//     while (true)
//     {
//         std::cout << "I am a process, I am waiting signal!" << std::endl;
//         sleep(10);
//     }
// }

// kill -9 3412056

void handler(int signumber)
{
    std::cout << "我是: " << getpid() << ", 我获得了一个信号: " << signumber << std::endl;
}

// int main()
// {
//     std::cout << "我是进程: " << getpid() << std::endl;
//     signal(SIGINT  /*2*/, handler);
//     while (true)
//     {
//         std::cout << "I am a process, I am waiting signal!" << std::endl;
//         sleep(1);
//     }
// }

// int main()
// {
//     std::cout << "我是进程: " << getpid() << std::endl;
//     //signal(SIGTSTP /*20*/, handler);
//     while (true)
//     {
//         std::cout << "I am a process, I am waiting signal!" << std::endl;
//         sleep(1);
//     }
// }



int main()
{
    while(true){
        std::cout<<"I am a process, I am waiting signal!"<<std::endl;
        sleep(1);
    }
}