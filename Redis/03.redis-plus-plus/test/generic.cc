#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <sw/redis++/redis++.h>

#include "util.hpp"

using std::cout;
using std::endl;
using std::string;
using std::unordered_map;
using std::vector;

// get 和 set
void test1(sw::redis::Redis &redis)
{
    std::cout << "get 和 set 的使用" << std::endl;

    // 清空一下数据库, 避免之前残留的数据有干扰.
    redis.flushall();

    // 使用 set 设置 key
    redis.set("key1", "111");
    redis.set("key2", "222");
    redis.set("key3", "333");

    // 使用 get 获取到 key 对应的 value
    auto value1 = redis.get("key1");
    // optional 可以隐式转成 bool 类型, 可以直接在 if 中判定. 如果是无效元素, 就是返回 false
    if (value1)
    {
        std::cout << "value1=" << value1.value() << std::endl;
    }

    auto value2 = redis.get("key2");
    if (value2)
    {
        std::cout << "value2=" << value2.value() << std::endl;
    }

    auto value3 = redis.get("key3");
    if (value3)
    {
        std::cout << "value3=" << value3.value() << std::endl;
    }

    auto value4 = redis.get("key4");
    if (value4)
    {
        std::cout << "value4=" << value4.value() << std::endl;
    }
}

// exists
void test2(sw::redis::Redis &redis)
{
    std::cout << "exists" << std::endl;

    redis.flushall();

    redis.set("key", "111");
    redis.set("key3", "111");

    auto ret = redis.exists("key");
    std::cout << ret << std::endl;

    ret = redis.exists("key2");
    std::cout << ret << std::endl;

    ret = redis.exists({"key", "key2", "key3"});
    std::cout << ret << std::endl;
}

// del
void test3(sw::redis::Redis &redis)
{
    std::cout << "del" << std::endl;
    // 清除库非常必要的!
    redis.flushall();

    redis.set("key", "111");
    redis.set("key2", "111");

    // redis.del("key");

    auto ret = redis.del({"key", "key2", "key3"});
    std::cout << ret << std::endl;

    ret = redis.exists({"key", "key2"});
    std::cout << ret << std::endl;
}

// keys
void test4(sw::redis::Redis &redis)
{
    std::cout << "keys" << std::endl;
    redis.flushall();

    redis.set("key", "111");
    redis.set("key2", "222");
    redis.set("key3", "333");
    redis.set("key4", "444");
    redis.set("key5", "555");
    redis.set("key6", "666");

    // keys 的第二个参数, 是一个 "插入迭代器". 咱们需要先准备好一个保存结果的容器.
    // 接下来再创建一个插入迭代器指向容器的位置. 就可以把 keys 获取到的结果依次通过刚才的插入迭代器插入到容器的指定位置中了.
    vector<string> result;
    
    // 插入迭代器: 可以把容器当作一个序列, 然后通过这个迭代器, 可以依次把容器中的元素插入到容器的指定位置中.
    // 此处, 我们把 result 当作一个序列, 然后通过 back_inserter 函数, 得到一个插入迭代器, 指向 result 的末尾.
    // 这样, 就可以把 keys 获取到的结果依次插入到 result 的末尾了.
    auto it = std::back_inserter(result);

    redis.keys("*", it);
    printContainer(result);
}

// expire 和 ttl
void test5(sw::redis::Redis &redis)
{
    using namespace std::chrono_literals;

    std::cout << "expire and ttl" << std::endl;
    redis.flushall();

    redis.set("key", "111");
    // 10s => std::chrono::seconds(10)
    redis.expire("key", 10s);

    std::this_thread::sleep_for(3s);

    auto time = redis.ttl("key");
    std::cout << time << std::endl;
}

// type
void test6(sw::redis::Redis &redis)
{
    std::cout << "type" << std::endl;
    redis.flushall();

    redis.set("key", "111");
    string result = redis.type("key");
    std::cout << "key: " << result << std::endl;

    redis.lpush("key2", "111");
    result = redis.type("key2");
    std::cout << "key2: " << result << std::endl;

    redis.hset("key3", "aaa", "111");
    result = redis.type("key3");
    std::cout << "key3: " << result << std::endl;

    redis.sadd("key4", "aaa");
    result = redis.type("key4");
    std::cout << "key4: " << result << std::endl;

    redis.zadd("key5", "吕布", 99);
    result = redis.type("key5");
    std::cout << "key5: " << result << std::endl;
}

int main()
{
    sw::redis::Redis redis("tcp://127.0.0.1:6379");

    // test1(redis);
    // test2(redis);
    // test3(redis);
    // test4(redis);
    // test5(redis);
    test6(redis);

    return 0;
}