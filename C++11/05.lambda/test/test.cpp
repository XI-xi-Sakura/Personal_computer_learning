#include <iostream>
using namespace std;
#include <vector>
#include <algorithm>

int test1()
{
    // 一个简单的lambda表达式
    auto add1 = [](int x, int y) -> int
    { return x + y; };
    cout << add1(1, 2) << endl;

    // 1、捕捉为空也不能省略
    // 2、参数为空可以省略
    // 3、返回值可以省略，可以通过返回对象自动推导
    // 4、函数题不能省略
    auto func1 = []
    {
        cout << "hello bit" << endl;
        return 0;
    };
    func1();

    int a = 0, b = 1;
    auto swap1 = [](int &x, int &y)
    {
        int tmp = x;
        x = y;
        y = tmp;
    };
    swap1(a, b);
    cout << a << ":" << b << endl;

    return 0;
}

int x = 0;
// 捕捉列表必须为空，因为全局变量不用捕捉就可以用，没有可被捕捉的变量
auto func1 = []()
{
    x++;
};

int test2()
{
    // 只能用当前lambda局部域和捕捉的对象和全局对象
    int a = 0, b = 1, c = 2, d = 3;
    auto func1 = [a, &b]
    {
        // 值捕捉的变量不能修改，引用捕捉的变量可以修改
        // a++;
        b++;
        int ret = a + b;
        return ret;
    };
    cout << func1() << endl;

    // 隐式值捕捉
    // 用了哪些变量就捕捉哪些变量
    auto func2 = [=]
    {
        int ret = a + b + c;
        return ret;
    };
    cout << func2() << endl;

    // 隐式引用捕捉
    // 用了哪些变量就捕捉哪些变量
    auto func3 = [&]
    {
        c++;
        d++;
    };
    func3();
    cout << a << " " << b << " " << c << " " << d << endl;

    // 混合捕捉1
    auto func4 = [&, a, b]
    {
        // a++;
        // b++;
        c++;
        d++;
        return a + b + c + d;
    };
    func4();
    cout << a << " " << b << " " << c << " " << d << endl;

    // 混合捕捉1
    auto func5 = [=, &a, &b]
    {
        a++;
        b++;
        /*c++;
        d++;*/
        return a + b + c + d;
    };
    func5();
    cout << a << " " << b << " " << c << " " << d << endl;

    // 局部的静态和全局变量不能捕捉，也不需要捕捉
    static int m = 0;
    auto func6 = []
    {
        int ret = x + m;
        return ret;
    };

    // 传值捕捉本质是一种拷贝，并且被const修饰了
    // mutable相当于去掉const属性，可以修改了
    // 但是修改了不会影响外面被捕捉的值，因为是一种拷贝
    auto func7 = [=]() mutable
    {
        a++;
        b++;
        c++;
        d++;
        return a + b + c + d;
    };
    cout << func7() << endl;
    cout << a << " " << b << " " << c << " " << d << endl;

    return 0;
}

struct Goods
{
    string _name;  // 名字
    double _price; // 价格
    int _evaluate; // 评价
    // ...

    Goods(const char *str, double price, int evaluate)
        : _name(str), _price(price), _evaluate(evaluate)
    {
    }
    void Print()
    {
        cout << _name << " " << _price << " " << _evaluate << endl;
    }
};

struct ComparePriceLess
{
    bool operator()(const Goods &gl, const Goods &gr)
    {
        return gl._price < gr._price;
    }
};

struct ComparePriceGreater
{
    bool operator()(const Goods &gl, const Goods &gr)
    {
        return gl._price > gr._price;
    }
};

int main()
{
    vector<Goods> v = {{"苹果", 2.1, 5}, {"香蕉", 3, 4}, {"橙子", 2.2, 3}, {"菠萝", 1.5, 4}};
    // 类似这样的场景，实现仿函数对象或者函数指针支持商品中不同项的比较，相对还是比较麻烦的，那么这里lambda就很好用了
    sort(v.begin(), v.end(), ComparePriceLess());
    sort(v.begin(), v.end(), ComparePriceGreater());

    sort(v.begin(), v.end(), [](const Goods &g1, const Goods &g2)
         { return g1._price < g2._price; });
         

    sort(v.begin(), v.end(), [](const Goods &g1, const Goods &g2)
         { return g1._price > g2._price; });

    sort(v.begin(), v.end(), [](const Goods &g1, const Goods &g2)
         { return g1._evaluate < g2._evaluate; });

    sort(v.begin(), v.end(), [](const Goods &g1, const Goods &g2)
         { return g1._evaluate > g2._evaluate; });

    return 0;
}