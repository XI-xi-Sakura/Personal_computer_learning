#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <thread>
#include <chrono>
#include <sw/redis++/redis++.h>
#include "util.hpp"

using std::cout;
using std::endl;
using std::vector;
using std::string;
using std::unordered_map;
using sw::redis::Redis;

using namespace std::chrono_literals;


// list 的基本操作

// 1. lpush 和 lrange
void test1(Redis& redis) {
    std::cout << "lpush 和 lrange" << std::endl;
    redis.flushall();

    // 插入单个元素
    redis.lpush("key", "111");

    // 插入一组元素, 基于初始化列表
    redis.lpush("key", {"222", "333", "444"});

    // 插入一组元素, 基于迭代器
    vector<string> values = {"555", "666", "777"};
    redis.lpush("key", values.begin(), values.end());

    // lrange 获取到列表中的元素
    vector<string> results;
    auto it = std::back_inserter(results);
    redis.lrange("key", 0, -1, it);

    printContainer(results);
}

// 2. rpush 和 lrange
void test2(Redis& redis) {
    std::cout << "rpush" << std::endl;
    redis.flushall();

    // 插入单个元素
    redis.rpush("key", "111");

    // 插入多个元素, 基于初始化列表
    redis.rpush("key", {"222", "333", "444"});

    // 插入多个元素, 基于容器
    vector<string> values = {"555", "666", "777"};
    redis.rpush("key", values.begin(), values.end());

    // 使用 lrange 获取元素
    vector<string> results;
    auto it = std::back_inserter(results);
    redis.lrange("key", 0, -1, it);

    printContainer(results);
}

// 3. lpop 和 rpop
void test3(Redis& redis) {
    std::cout << "lpop 和 rpop" << std::endl;
    redis.flushall();

    // 构造一个 list
    redis.rpush("key", {"1", "2", "3", "4"});

    auto result = redis.lpop("key");
    if (result) {
        std::cout << "lpop: " << result.value() << std::endl;
    }

    result = redis.rpop("key");
    if (result) {
        std::cout << "rpop: " << result.value() << std::endl;
    }
}

// 4. blpop 和 brpop
// blpop 和 brpop 是阻塞的版本
void test4(Redis& redis) {
    using namespace std::chrono_literals;
    std::cout << "blpop" << std::endl;
    redis.flushall();

    auto result = redis.blpop({"key", "key2", "key3"}, 10s);
    if (result) {
        std::cout << "key:" << result->first << std::endl;
        std::cout << "elem:" << result->second << std::endl;
    } else {
        std::cout << "result 无效!" << std::endl;
    }
}

// 5. llen
void test5(Redis& redis) {
    std::cout << "llen" << std::endl;
    redis.flushall();

    redis.lpush("key", {"111", "222", "333", "444"});
    long long len = redis.llen("key");
    std::cout << "len: " << len << std::endl;
}

int main() {
    Redis redis("tcp://127.0.0.1:6379");
    // test1(redis);
    // test2(redis);
    // test3(redis);
    // test4(redis);
    test5(redis);
    return 0;
}