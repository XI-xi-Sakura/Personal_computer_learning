#include <iostream>
#include <map>
#include <string>
using namespace std;
int test1()
{
    // initializer_list构造及迭代遍历
    map<string, string> dict = {{"left", "左边"}, {"right", "右边"}, {"insert", "插入"}, {"string", "字符串"}};

    // map<string, string>::iterator it = dict.begin();
    auto it = dict.begin();
    while (it != dict.end())
    {
        // cout << (*it).first <<":"<<(*it).second << endl;
        //  map的迭代基本都使用operator->，这里省略了一个->
        //  第一个->是迭代器运算符重载，返回pair*，第二个箭头是结构指针解引用取pair数据
        // cout << it.operator->()->first << ":" << it.operator->()->second << endl;
        cout << it->first << ":" << it->second << endl;
        ++it;
    }
    cout << endl;

    // insert插入pair对象的4种方式，对比之下，最后一种最方便
    pair<string, string> kv1("first", "第一个");
    dict.insert(kv1);
    dict.insert(pair<string, string>("second", "第二个"));
    dict.insert(make_pair("sort", "排序"));
    dict.insert({"auto", "自动的"});

    // "left"已经存在，插入失败
    dict.insert({"left", "左边,剩余"});
    // 范围for遍历
    for (const auto &e : dict)
    {
        cout << e.first << ":" << e.second << endl;
    }
    cout << endl;

    string str;
    while (cin >> str)
    {
        auto ret = dict.find(str);
        if (ret != dict.end())
        {
            cout << str << "->" << ret->second << endl;
        }
        else
        {
            cout << "无此单词，请重新输入" << endl;
        }
    }
    // erase等接口跟set完全类似，这里就不演示讲解了
    return 0;
}

int test2()
{
    // 利用find和iterator修改功能，统计水果出现的次数
    string arr[] = {"苹果", "西瓜", "苹果", "西瓜", "苹果", "苹果", "西瓜",
                    "苹果", "香蕉", "苹果", "香蕉"};
    map<string, int> countMap;
    for (const auto &str : arr)
    {
        // 先查找水果在不在map中
        // 1、不在，说明水果第一次出现，则插入{水果, 1}
        // 2、在，则查找到的节点中水果对应的次数++
        auto ret = countMap.find(str);
        if (ret == countMap.end())
        {
            countMap.insert({str, 1});
        }
        else
        {
            ret->second++;
        }
    }
    for (const auto &e : countMap)
    {
        cout << e.first << ":" << e.second << endl;
    }
    cout << endl;
    return 0;
}

int test3()
{
    // 利用[]插入+修改功能，巧妙实现统计水果出现的次数
    string arr[] = {"苹果", "西瓜", "苹果", "西瓜", "苹果", "苹果", "西瓜",
                    "苹果", "香蕉", "苹果", "香蕉"};
    map<string, int> countMap;
    for (const auto &str : arr)
    {
        // []先查找水果在不在map中
        // 1、不在，说明水果第一次出现，则插入{水果, 0}，同时返回次数的引用，++一下就变成1次了
        // 2、在，则返回水果对应的次数++
        countMap[str]++;
    }
    for (const auto &e : countMap)
    {
        cout << e.first << ":" << e.second << endl;
    }
    cout << endl;
    return 0;
}

int test4()
{
    map<string, string> dict;
    dict.insert(make_pair("sort", "排序"));
    // key不存在->插入 {"insert", string()}
    dict["insert"];
    // 插入+修改
    dict["left"] = "左边";
    // 修改
    dict["left"] = "左边､剩余";
    // key存在->查找
    cout << dict["left"] << endl;
    return 0;
}

int main()
{
    test1();
    return 0;
}