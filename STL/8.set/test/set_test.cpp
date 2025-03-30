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


//find和erase使用样例
#include<algorithm>

// int main()
// {
//     set<int> s = { 4,2,7,2,8,5,9 };
//     for (auto e : s)
//     {
//         cout << e << " ";
//     }
//     cout << endl;

//     // 删除最小值
//     s.erase(s.begin());
//     for (auto e : s)
//     {
//         cout << e << " ";
//     }
//     cout << endl;

//     // 直接删除x
//     int x;
//     cin >> x;
//     int num = s.erase(x);
//     if (num == 0)
//     {
//         cout << x << "不存在!" << endl;
//     }
//     for (auto e : s)
//     {
//         cout << e << " ";
//     }
//     cout << endl;

//     // 直接查找在利用迭代器删除x
//     cin >> x;
//     auto pos = s.find(x);
//     if (pos != s.end())
//     {
//         s.erase(pos);
//     }
//     else
//     {
//         cout << x << "不存在!" << endl;
//     }
//     for (auto e : s)
//     {
//         cout << e << " ";
//     }
//     cout << endl;

//     // 算法库的查找 O(N)
//     auto pos1 = find(s.begin(), s.end(), x); 
//     // set自身实现的查找 O(logN)
//     auto pos2 = s.find(x);
//     // 利用count间接实现快速查找
//     cin >> x;
//     if (s.count(x))
//     {
//         cout << x << "在!" << endl;
//     }
//     else
//     {
//         cout << x << "不存在!" << endl;
//     }
//     return 0;
// }



// int main()
// {
//     std::set<int> myset;
//     for (int i = 1; i < 10; i++)
//     {
//         myset.insert(i * 10); // 10 20 30 40 50 60 70 80 90
//     }
//     for (auto e : myset)
//     {
//         cout << e << " ";
//     }
//     cout << endl;
//     // 实现查找到的[itlow,itup)包含[30, 60]区间
//     // 返回 >= 30
//     auto itlow = myset.lower_bound(30);
//     // 返回 > 60
//     auto itup = myset.upper_bound(60);
//     // 删除这段区间的值
//     myset.erase(itlow, itup);
//     for (auto e : myset)
//     {
//         cout << e << " ";
//     }
//     cout << endl;
//     return 0;
// }



int main()
{
    // 相比set不同的是，multiset是排序，但是不去重
    multiset<int> s = { 4,2,7,2,4,8,4,5,4,9 };
    auto it = s.begin();
    while (it != s.end())
    {
        cout << *it << " ";
        ++it;
    }
    cout << endl;

    // 相比set不同的是，x可能会存在多个，find查找中序的第一个
    int x;
    cin >> x;
    auto pos = s.find(x);
    while (pos != s.end() && *pos == x)
    {
        cout << *pos << " ";
        ++pos;
    }
    cout << endl;

    // 相比set不同的是，count会返回x的实际个数
    cout << s.count(x) << endl;
    
    // 相比set不同的是，erase给值时会删除所有的x
    s.erase(x);
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    return 0;
}
