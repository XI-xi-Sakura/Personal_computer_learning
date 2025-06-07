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
    std::cout << "zadd 和 zrange" << std::endl;
    redis.flushall();

    redis.zadd("key", "吕布", 99);
    redis.zadd("key", {
        std::make_pair("赵云", 98),
        std::make_pair("典韦", 97)
    });
    vector<std::pair<string, double>> members = {
        std::make_pair("关羽", 95),
        std::make_pair("张飞", 93)
    };
    redis.zadd("key", members.begin(), members.end());

    // zrange 支持两种主要的风格:
    // 1. 只查询 member, 不带 score
    // 2. 查询 member 同时带 score
    // 关键就是看插入迭代器指向的容器的类型. 
    // 指向的容器只是包含一个 string, 就是只查询 member
    // 指向的容器包含的是一个 pair, 里面有 string 和 double, 就是查询 member 同时带有 score
    vector<string> memberResults;
    auto it = std::back_inserter(memberResults);
    redis.zrange("key", 0, -1, it);
    printContainer(memberResults);

    vector<std::pair<string, double>> membersWithScore;
    auto it2 = std::back_inserter(membersWithScore);
    redis.zrange("key", 0, -1, it2);
    printContainerPair(membersWithScore);
}

void test2(Redis& redis) {
    std::cout << "zcard" << std::endl;
    redis.flushall();

    redis.zadd("key", "zhangsan", 90);
    redis.zadd("key", "lisi", 91);
    redis.zadd("key", "wangwu", 92);
    redis.zadd("key", "zhaoliu", 93);

    long long result = redis.zcard("key");
    std::cout << "result: " << result << std::endl;
}

void test3(Redis& redis) {
    std::cout << "zrem" << std::endl;
    redis.flushall();

    redis.zadd("key", "zhangsan", 90);
    redis.zadd("key", "lisi", 91);
    redis.zadd("key", "wangwu", 92);
    redis.zadd("key", "zhaoliu", 93);

    redis.zrem("key", "zhangsan");

    long long result = redis.zcard("key");
    std::cout << "result: " << result << std::endl;
}

void test4(Redis& redis) {
    std::cout << "zscore" << std::endl;
    redis.flushall();

    redis.zadd("key", "zhangsan", 90);
    redis.zadd("key", "lisi", 91);
    redis.zadd("key", "wangwu", 92);
    redis.zadd("key", "zhaoliu", 93);

    auto score = redis.zscore("key", "zhangsan");
    if (score) {
        std::cout << "score: " << score.value() << std::endl;
    } else {
        std::cout << "score 无效" << std::endl;
    }
}

void test5(Redis& redis) {
    std::cout << "zrank" << std::endl;
    redis.flushall();

    redis.zadd("key", "zhangsan", 90);
    redis.zadd("key", "lisi", 91);
    redis.zadd("key", "wangwu", 92);
    redis.zadd("key", "zhaoliu", 93);

    auto rank = redis.zrank("key", "zhaoliu");
    if (rank) {
        std::cout << "rank: " << rank.value() << std::endl;
    } else {
        std::cout << "rank 无效" << std::endl;
    }
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