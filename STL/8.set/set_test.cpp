#include<iostream>
#include<set>
using namespace std;

//insert和迭代器遍历使用样例
// int main()
// {
//     // 去重+升序排序
//     set<int> s;
//     // 去重+降序排序(给一个大于的仿函数)
//     //set<int, greater<int>> s;
//     s.insert(5);
//     s.insert(2);
//     s.insert(7);
//     s.insert(5);
//     //set<int>::iterator it = s.begin();
//     auto it = s.begin();
//     while (it != s.end())
//     {
//         // error C3892: “it”: 不能给常量赋值
//         // *it = 1;
//         cout << *it << " ";
//         ++it;
//     }
//     cout << endl;
//     // 插入一段initializer_list列表值，已经存在的值插入失败
//     s.insert({ 2,8,3,9 });
//     for (auto e : s)
//     {
//         cout << e << " ";
//     }
//     cout << endl;
//     set<string> strset = { "sort", "insert", "add" };
//     // 遍历string比较ascll码大小顺序遍历的
//     for (auto& e : strset)
//     {
//         cout << e << " ";
//     }
//     cout << endl;
//     return 0;
// }
#include<algorithm>

int main()
{
    set<int> s = { 4,2,7,2,8,5,9 };
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;

    // 删除最小值
    s.erase(s.begin());
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;

    // 直接删除x
    int x;
    cin >> x;
    int num = s.erase(x);
    if (num == 0)
    {
        cout << x << "不存在!" << endl;
    }
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;

    // 直接查找在利用迭代器删除x
    cin >> x;
    auto pos = s.find(x);
    if (pos != s.end())
    {
        s.erase(pos);
    }
    else
    {
        cout << x << "不存在!" << endl;
    }
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    
    // 算法库的查找 O(N)
    auto pos1 = find(s.begin(), s.end(), x); 
    // set自身实现的查找 O(logN)
    auto pos2 = s.find(x);
    // 利用count间接实现快速查找
    cin >> x;
    if (s.count(x))
    {
        cout << x << "在!" << endl;
    }
    else
    {
        cout << x << "不存在!" << endl;
    }
    return 0;
}