#include <iostream>
#include <vector>
#include <string>
#include <unordered_map>
#include <sw/redis++/redis++.h>

#include "util.hpp"

using std::cout;
using std::endl;
using std::vector;
using std::string;
using std::unordered_map;

using sw::redis::Redis;

void test1(Redis& redis) {
    std::cout << "hset 和 hget" << std::endl;
    redis.flushall();

    redis.hset("key", "f1", "111");
    redis.hset("key", std::make_pair("f2", "222"));
    // hset 能够一次性插入多个 field-value 对!!
    redis.hset("key", {
        std::make_pair("f3", "333"),
        std::make_pair("f4", "444")
    });
    vector<std::pair<string, string>> fields = {
        std::make_pair("f5", "555"),
        std::make_pair("f6", "666")
    };
    redis.hset("key", fields.begin(), fields.end());

    auto result = redis.hget("key", "f3");
    if (result) {
        std::cout << "result: " << result.value() << std::endl;
    } else {
        std::cout << "result 无效!" << std::endl;
    }
}

void test2(Redis& redis) {
    std::cout << "hexits" << std::endl;
    redis.flushall();

    redis.hset("key", "f1", "111");
    redis.hset("key", "f2", "222");
    redis.hset("key", "f3", "333");

    bool result = redis.hexists("key", "f4");
    std::cout << "result: " << result << std::endl;
}

void test3(Redis& redis) {
    std::cout << "hdel" << std::endl;
    redis.flushall();

    redis.hset("key", "f1", "111");
    redis.hset("key", "f2", "222");
    redis.hset("key", "f3", "333");

    long long result = redis.hdel("key", "f1");
    std::cout << "result: " << result << std::endl;

    result = redis.hdel("key", {"f2", "f3"});
    std::cout << "result: " << result << std::endl;

    long long len = redis.hlen("key");
    std::cout << "len: " << len << std::endl;
}

void test4(Redis& redis) {
    std::cout << "hkeys 和 hvals" << std::endl;
    redis.flushall();

    redis.hset("key", "f1", "111");
    redis.hset("key", "f2", "222");
    redis.hset("key", "f3", "333");

    vector<string> fields;
    auto itFields = std::back_inserter(fields);
    redis.hkeys("key", itFields);
    printContainer(fields);

    vector<string> values;
    auto itValues = std::back_inserter(values);
    redis.hvals("key", itValues);
    printContainer(values);
}

void test5(Redis& redis) {
    std::cout << "hmget 和 hmset" << std::endl;
    redis.flushall();

    redis.hmset("key", {
        std::make_pair("f1", "111"),
        std::make_pair("f2", "222"),
        std::make_pair("f3", "333")
    });

    vector<std::pair<string, string>> pairs = {
        std::make_pair("f4", "444"),
        std::make_pair("f5", "555"),
        std::make_pair("f6", "666")
    };
    redis.hmset("key", pairs.begin(), pairs.end());

    vector<string> values;
    auto it = std::back_inserter(values);
    redis.hmget("key", {"f1", "f2", "f3"}, it);
    printContainer(values);
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