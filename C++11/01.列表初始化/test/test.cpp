#include <iostream>
#include <vector>
#include <string>
#include <map>
using namespace std;

struct Point
{
    int _x;
    int _y;
};

class Date
{
public:
    Date(int year = 1, int month = 1, int day = 1)
        : _year(year), _month(month), _day(day)
    {
        cout << "Date(int year, int month, int day)" << endl;
    }

    Date(const Date &d)
        : _year(d._year), _month(d._month), _day(d._day)
    {
        cout << "Date(const Date& d)" << endl;
    }

private:
    int _year;
    int _month;
    int _day;
};

// 一切皆可用列表初始化，且可以不加=
int main()
{
    // C++98支持的

    int a1[] = {1, 2, 3, 4, 5};
    int a2[5] = {0};
    Point p = {1, 2};

    // C++11支持的

    // 内置类型支持
    int x1 = {2};

    // 自定义类型支持
    // 这里本质是用{ 2025, 1, 1}构造一个Date临时对象
    // 临时对象再去拷贝构造d1，编译器优化后合二为一变成{ 2025, 1, 1}直接构造初始化d1
    // 运行一下，我们可以验证上面的理论，发现是没调用拷贝构造的
    Date d1 = {2025, 1, 1};

    // 这里d2引用的是{ 2024, 7, 25 }构造的临时对象
    const Date &d2 = {2024, 7, 25};

    // 需要注意的是C++98支持单参数时类型转换，也可以不用{}
    Date d3 = {2025};
    Date d4 = 2025;

    // 可以省略掉=
    Point p1{1, 2};
    int x2{2};
    Date d6{2024, 7, 25};
    const Date &d7{2024, 7, 25};

    // 初始化列表也可适用于new表达式中
    Date *d8 = new Date{2024, 7, 25};

    // 不支持，只有{}初始化，才能省略=
    // Date d9 2025;

    vector<Date> v;
    v.push_back(d1);
    v.push_back(Date(2025, 1, 1));

    // 比起有名对象和匿名对象传参，这里{}更有性价比
    v.push_back({2025, 1, 1});
    return 0;
}

// int main()
// {
//     std::initializer_list<int> mylist;
//     mylist = { 10, 20, 30 };
//     cout << sizeof(mylist) << endl;

//     // 这里begin和end返回的值initializer_list对象中存的两个指针
//     // 这两个指针的值跟i的地址跟接近，说明数组存在栈上
//     int i = 0;
//     cout << mylist.begin() << endl;
//     cout << mylist.end() << endl;
//     cout << &i << endl;

//     // {}列表中可以有任意多个值
//     // 这两个写法语义上还是有差别的，第一个v1是直接构造，
//     // 第二个v2是构造临时对象+临时对象拷贝v2+优化为直接构造
//     vector<int> v1({ 1,2,3,4,5 });
//     vector<int> v2 = { 1,2,3,4,5 };
//     const vector<int>& v3 = { 1,2,3,4,5 };

//     // 这里是pair对象的{}初始化和map的initializer_list构造结合到一起用了
//     map<string, string> dict = { {"sort", "排序"}, {"string", "字符串"}};

//     // initializer_list版本的赋值支持
//     v1 = { 10,20,30,40,50 };

//     return 0;
// }