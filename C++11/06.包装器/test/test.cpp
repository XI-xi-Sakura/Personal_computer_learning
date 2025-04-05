#include <functional>
#include <iostream>
using namespace std;
int f(int a, int b)
{
    return a + b;
}

struct Functor
{
public:
    int operator()(int a, int b)
    {
        return a + b;
    }
};

class Plus1
{
public:
    Plus1(int n = 10)
        : _n(n)
    {
    }
    static int plusi(int a, int b)
    {
        return a + b;
    }
    double plusd(double a, double b)
    {
        return (a + b) * _n;
    }

private:
    int _n;
};

int test1()
{
    // 包装各种可调用对象
    function<int(int, int)> f1 = f;
    function<int(int, int)> f2 = Functor();
    function<int(int, int)> f3 = [](int a, int b)
    { return a + b; };

    cout << f1(1, 1) << endl;
    cout << f2(1, 1) << endl;
    cout << f3(1, 1) << endl;

    // 包装静态成员函数
    // 成员函数要指定类域并且前面加&才能获取地址
    function<int(int, int)> f4 = &Plus1::plusi;
    cout << f4(1, 1) << endl;

    // 包装普通成员函数
    // 普通成员函数还有一个隐含的this指针参数，所以绑定时传对象或者对象的指针过去都可以
    function<double(Plus1 *, double, double)> f5 = &Plus1::plusd;
    Plus1 pd;
    cout << f5(&pd, 1.1, 1.1) << endl;

    function<double(Plus1, double, double)> f6 = &Plus1::plusd;
    cout << f6(pd, 1.1, 1.1) << endl;
    cout << f6(pd, 1.1, 1.1) << endl;

    function<double(Plus1 &&, double, double)> f7 = &Plus1::plusd;
    cout << f7(move(pd), 1.1, 1.1) << endl;
    cout << f7(Plus1(), 1.1, 1.1) << endl;

    return 0;
}

using placeholders::_1;
using placeholders::_2;
using placeholders::_3;

int Sub(int a, int b)
{
    return (a - b) * 10;
}

int SubX(int a, int b, int c)
{
    return (a - b - c) * 10;
}

class Plus
{
public:
    static int plusi(int a, int b)
    {
        return a + b;
    }
    double plusd(double a, double b)
    {
        return a + b;
    }
};

int main()
{
    auto sub1 = bind(Sub, _1, _2);
    cout << sub1(10, 5) << endl;

    // bind本质返回的一个仿函数对象
    // 调整参数顺序(不常用)
    // _1代表第一个实参
    // _2代表第二个实参
    //...
    auto sub2 = bind(Sub, _2, _1);
    cout << sub2(10, 5) << endl;

    // 调整参数个数 (常用)
    auto sub3 = bind(Sub, 100, _1);
    cout << sub3(5) << endl;

    auto sub4 = bind(Sub, _1, 100);
    cout << sub4(5) << endl;

    // 分别绑死第123个参数
    auto sub5 = bind(SubX, 100, _1, _2);
    cout << sub5(5, 1) << endl;

    auto sub6 = bind(SubX, _1, 100, _2);
    cout << sub6(5, 1) << endl;

    auto sub7 = bind(SubX, _1, _2, 100);
    cout << sub7(5, 1) << endl;

    // 成员函数对象进行绑死，就不需要每次都传递了
    function<double(Plus &&, double, double)> f6 = &Plus::plusd;
    Plus pd;
    cout << f6(move(pd), 1.1, 1.1) << endl;
    cout << f6(Plus(), 1.1, 1.1) << endl;

    // bind一般用于，绑死一些固定参数
    function<double(double, double)> f7 = bind(&Plus::plusd, Plus(), _1, _2);
    cout << f7(1.1, 1.1) << endl;

    // 计算复利的lambda
    auto func1 = [](double rate, double money, int year) -> double
    {
        double ret = money;
        for (int i = 0; i < year; i++)
        {
            ret += ret * rate;
        }
        return ret - money;
    };

    // 绑死一些参数，实现出支持不同年华利率，不同金额和不同年份计算出复利的结算利息
    function<double(double)> func3_1_5 = bind(func1, 0.015, _1, 3);
    function<double(double)> func5_1_5 = bind(func1, 0.015, _1, 5);
    function<double(double)> func10_2_5 = bind(func1, 0.025, _1, 10);
    function<double(double)> func20_3_5 = bind(func1, 0.035, _1, 30);

    cout << func3_1_5(1000000) << endl;
    cout << func5_1_5(1000000) << endl;
    cout << func10_2_5(1000000) << endl;
    cout << func20_3_5(1000000) << endl;

    return 0;
}