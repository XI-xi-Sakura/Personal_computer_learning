
/*
    可变参数模板
*/
// template <class...Args>
// void Print(Args&&... args)
// {
//     cout << sizeof...(args) << endl;
// }

// int main()
// {
//     double x = 2.2;
//     Print(); // 包里有0个参数
//     Print(1); // 包里有1个参数
//     Print(1, string("xxxxx")); // 包里有2个参数
//     Print(1.1, string("xxxxx"), x); // 包里有3个参数

//     return 0;
// }



// int main()
// {
//     list<string> lt;
//     // 传左值，跟push_back一样，走拷贝构造
//     string s1("111111111111");
//     lt.emplace_back(s1);
//     cout << "*********************************" << endl;
    
//     // 右值，跟push_back一样，走移动构造
//     lt.emplace_back(move(s1));
//     cout << "*********************************" << endl;
    
//     // 直接把构造string参数包往下传，直接用string参数包构造string
//     // 这里达到的效果是push_back做不到的
//     lt.emplace_back("111111111111");
//     cout << "*********************************" << endl;
    
//     list<pair<string, int>> lt1;
//     // 跟push_back一样
//     // 构造pair + 拷贝/移动构造pair到list的节点中data上
//     pair<string, int> kv("苹果", 1);
//     lt1.emplace_back(kv);
//     cout << "*********************************" << endl;
    
//     // 跟push_back一样
//     lt1.emplace_back(move(kv));
//     cout << "*********************************" << endl;
    
//     // 直接把构造pair参数包往下传，直接用pair参数包构造pair
//     // 这里达到的效果是push_back做不到的
//     lt1.emplace_back("苹果", 1);
//     cout << "*********************************" << endl;

//     return 0;
// }

#include"listTest.hpp"
int main()
{
    bit::List<string> lt;
    // 传左值，跟push_back一样，走拷贝构造
    string s1("111111111111");
    lt.emplace_back(s1);
    cout << "*********************************" << endl;

    // 右值，跟push_back一样，走移动构造
    lt.emplace_back(move(s1));
    cout << "*********************************" << endl;

    // 直接把构造string参数包往下传，直接用string参数包构造string
    // 这里达到的效果是push_back做不到的
    lt.emplace_back("111111111111");
    cout << "*********************************" << endl;

    bit::List<pair<string, int>> lt1;
    // 跟push_back一样
    // 构造pair + 拷贝/移动构造pair到list的节点中data上
    pair<string, int> kv("苹果", 1);
    lt1.emplace_back(kv);
    cout << "*********************************" << endl;

    // 跟push_back一样
    lt1.emplace_back(move(kv));
    cout << "*********************************" << endl;

    // 直接把构造pair参数包往下传，直接用pair参数包构造pair
    // 这里达到的效果是push_back做不到的
    lt1.emplace_back("苹果", 1);
    cout << "*********************************" << endl;

    return 0;
}