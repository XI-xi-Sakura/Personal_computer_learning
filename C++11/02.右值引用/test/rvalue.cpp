// 由于引用折叠限定,f1实例化以后总是一个左值引用
template<class T>
void f1(T& x)
{}

// 由于引用折叠限定,f2实例化后可以是左值引用,也可以是右值引用
template<class T>
void f2(T&& x)
{}

int test() {
    typedef int& lref;
    typedef int&& rref;
    int n = 0;

    lref& r1 = n; // r1 的类型是 int&
    lref&& r2 = n; // r2 的类型是 int&
    rref& r3 = n; // r3 的类型是 int&
    rref&& r4 = 1; // r4 的类型是 int&&

    // 没有折叠->实例化为void f1(int& x)
    f1<int>(n);
    //f1<int>(0); // 报错

    // 折叠->实例化为void f1(int& x)
    f1<int&>(n);
    //f1<int&>(0); // 报错

    // 折叠->实例化为void f1(int& x)
    f1<int&&>(n);
    //f1<int&&>(0); // 报错

    // 折叠->实例化为void f1(const int& x)
    f1<const int&>(n);
    f1<const int&>(0);

    // 折叠->实例化为void f1(const int& x)
    f1<const int&&>(n);
    f1<const int&&>(0);

    // 没有折叠->实例化为void f2(int&& x)
    //f2<int>(n); // 报错
    f2<int>(0);

    // 折叠->实例化为void f2(int& x)
    f2<int&>(n);
    //f2<int&>(0); // 报错

    // 折叠->实例化为void f2(int&& x)
    //f2<int&&>(n); // 报错
    f2<int&&>(0);

    return 0;
}

#include <iostream>
using namespace std;
// template<class T>
// void Function(T&& t)
// {
//     int a = 0;
//     T x = a;
//     //x++;
//     cout << &a << endl;
//     cout << &x << endl << endl;
// }

// int main()
// {
//     // 10是右值,推导出T为int,模板实例化为void Function(int&& t)
//     Function(10); // 右值

//     int a;
//     // a是左值,推导出T为int&,引用折叠,模板实例化为void Function(int& t)
//     Function(a); // 左值

//     // std::move(a)是右值,推导出T为int,模板实例化为void Function(int&& t)
//     Function(std::move(a)); // 右值

//     const int b = 8;
//     // a是左值,推导出T为const int&,引用折叠,模板实例化为void Function(const int& t)
//     // 所以Function内部会编译报错,x不能++
//     Function(b); // const 左值

//     // std::move(b)右值,推导出T为const int,模板实例化为void Function(const int&& t)
//     // 所以Function内部会编译报错,x不能++
//     Function(std::move(b)); // const 右值

//     return 0;
// }


// template <class _Ty>
// _Ty&& forward(remove_reference_t<_Ty>& _Arg) noexcept
// { // forward an lvalue as either an lvalue or an rvalue
//     return static_cast<_Ty&&>(_Arg);
// }

void Fun(int& x) { cout << "左值引用" << endl; }
void Fun(const int& x) { cout << "const 左值引用" << endl; }
void Fun(int&& x) { cout << "右值引用" << endl; }
void Fun(const int&& x) { cout << "const 右值引用" << endl; }

template<class T>
void Function(T&& t)
{
    //Fun(t);
    Fun(forward<T>(t));
}

int main()
{
    // 10是右值,推导出T为int,模板实例化为void Function(int&& t)
    Function(10); // 右值

    int a;
    // a是左值,推导出T为int&,引用折叠,模板实例化为void Function(int& t)
    Function(a); // 左值

    // std::move(a)是右值,推导出T为int,模板实例化为void Function(int&& t)
    Function(std::move(a)); // 右值

    const int b = 8;
    // a是左值,推导出T为const int&,引用折叠,模板实例化为void Function(const int& t)

    Function(b); // const 左值

    // std::move(b)右值,推导出T为const int,模板实例化为void Function(const int&& t)

    Function(std::move(b)); // const 右值

    return 0;
}