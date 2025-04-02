#include <iostream>
// std是C++标准库的命名空间名，C++将标准库的定义实现都放到这个命名空间中
using namespace std;

// void TestConstRef()
// {
//     const int a = 10;

//     // int& ra = a;   // 该语句编译时会出错，a为常量
//     const int &ra = a;

//     // int& b = 10; // 该语句编译时会出错，b为常量
//     const int &b = 10;

//     double d = 12.34;

//     // int& rd = d; // 该语句编译时会出错，类型不同
//     const int &rd = d;
//     cout << rd << endl;
// }
// int main()
// {

//     int a = 10;
//     int &ra = a;
//     cout << "&a = " << &a << endl;
//     cout << "&ra = " << &ra << endl;

//     TestConstRef();
//     cout << "Hello world!!!" << endl;
//     return 0;
// }

void TestFor()
{
    int array[] = {1, 2, 3, 4, 5};
    for (auto &e : array)
        e *= 2;
    for (auto e : array)
        cout << e << " ";
    return;
}

#include <iostream>

// 定义一个模板函数，接收数组的引用
template <size_t N>
void TestFor1(int (&array)[N])
{
    for (auto &e : array)
        std::cout << e << std::endl;
}
int main()
{
    int x = 10;
    auto a = &x;
    auto *b = &x;
    auto &c = x; // 用auto声明引用类型时则必须加&
    cout << typeid(a).name() << endl;
    cout << typeid(b).name() << endl;
    cout << typeid(c).name() << endl;
    *a = 20;
    *b = 30;
    c = 40;
    TestFor();
    return 0;
}