#include <cstdio>
#include <iostream>

int main()
{
    // 标准输出 -> 1
    std::cout <<"hello cout" <<std::endl;
    printf("hello printf\n");
    // 标准错误 -> 2
    std::cerr << "hello cerr" <<std::endl;
    fprintf(stderr, "hello fprintf\n");
    return 0;
}
