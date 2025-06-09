#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <sw/redis++/redis++.h>

//

using std::cout;
using std::endl;
using std::string;
using std::unordered_map;
using std::vector;

int main()
{
    // 创建 Redis 对象的时候, 需要在构造函数中, 指定 redis 服务器的地址和端口.
    sw::redis::Redis redis("tcp://127.0.0.1:6379");
    // 调用 ping 方法, 让客户端给服务器发了一个 PING, 然后服务器就会返回一个 PONG. 就通过 返回值 获取到.
    string result = redis.ping();

    std::cout << result << std::endl;
    
    return 0;
}