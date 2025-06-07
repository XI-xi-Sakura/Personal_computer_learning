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
using std::vector;
using std::string;
using std::unordered_map;

using sw::redis::Redis;

using namespace std::chrono_literals;

void test1(Redis& redis) {
    std::cout << "get 和 set" << std::endl;
    redis.flushall();

    redis.set("key", "111");
    auto value = redis.get("key");
    if (value) {
        std::cout << "value: " << value.value() << std::endl;
    }

    redis.set("key", "222");
    value = redis.get("key");
    if (value) {
        std::cout << "value: " << value.value() << std::endl;
    }
}

void test2(Redis& redis) {
    std::cout << "set 带有超时时间" << std::endl;
    redis.flushall();

    redis.set("key", "111", 10s);

    std::this_thread::sleep_for(3s);

    long long time = redis.ttl("key");
    std::cout << "time: " << time << std::endl;
}

void test3(Redis& redis) {
    std::cout << "set NX 和 XX" << std::endl;
    redis.flushall();

    redis.set("key", "111");

    // set 的重载版本中, 没有单独提供 NX 和 XX 的版本, 必须搭配过期时间的版本来使用. 
    redis.set("key", "222", 0s, sw::redis::UpdateType::EXIST);

    auto value = redis.get("key");
    if (value) {
        std::cout << "value: " << value.value() << std::endl;
    } else {
        std::cout << "key 不存在!" << std::endl;
    }
}

void test4(Redis& redis) {
    std::cout << "mset" << std::endl;

    redis.flushall();

    // 第一种写法, 使用初始化列表描述多个键值对
    // redis.mset({ std::make_pair("key1", "111"), std::make_pair("key2", "222"), std::make_pair("key3", "333") });

    // 第二种写法, 可以把多个键值对提前组织到容器中. 以迭代器的形式告诉 mset
    vector<std::pair<string, string>> keys = {
        {"key1", "111"},
        {"key2", "222"},
        {"key3", "333"}
    };
    redis.mset(keys.begin(), keys.end());

    auto value = redis.get("key1");
    if (value) {
        std::cout << "value: " << value.value() << std::endl;
    }

    value = redis.get("key2");
    if (value) {
        std::cout << "value: " << value.value() << std::endl;
    }

    value = redis.get("key3");
    if (value) {
        std::cout << "value: " << value.value() << std::endl;
    }
}

void test5(Redis& redis) {
    std::cout << "mget" << std::endl;
    redis.flushall();

    vector<std::pair<string, string>> keys = {
        {"key1", "111"},
        {"key2", "222"},
        {"key3", "333"}
    };
    redis.mset(keys.begin(), keys.end());

    vector<sw::redis::OptionalString> result;
    auto it = std::back_inserter(result);
    redis.mget({"key1", "key2", "key3", "key4"}, it);

    printContainerOptional(result);
}

void test6(Redis& redis) {
    std::cout << "getrange 和 setrange" << std::endl;
    redis.flushall();

    redis.set("key", "abcdefghijk");

    string result = redis.getrange("key", 2, 5);
    std::cout << "result: " << result << std::endl;

    redis.setrange("key", 2, "xyz");

    auto value = redis.get("key");
    std::cout << "value: " << value.value() << std::endl;
}

void test7(Redis& redis) {
    std::cout << "incr 和 decr" << std::endl;
    redis.flushall();

    redis.set("key", "100");

    long long result = redis.incr("key");
    std::cout << "result: " << result << std::endl;

    auto value = redis.get("key");
    std::cout << "value: " << value.value() << std::endl;

    result = redis.decr("key");
    std::cout << "result: " << result << std::endl;

    value = redis.get("key");
    std::cout << "value: " << value.value() << std::endl;
}

int main() {
    Redis redis("tcp://127.0.0.1:6379");

    // test1(redis);
    // test2(redis);
    // test3(redis);
    // test4(redis);
    // test5(redis);
    // test6(redis);
    test7(redis);
    
    return 0;
}