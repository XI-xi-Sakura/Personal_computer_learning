#include <iostream>
#include <memory>
using namespace std;

struct Date
{
    int _year;
    int _month;
    int _day;
    Date(int year = 1, int month = 1, int day = 1)
        : _year(year), _month(month), _day(day)
    {
    }

    ~Date()
    {
        cout << "~Date()" << endl;
    }
};

int test1()
{
    auto_ptr<Date> ap1(new Date);
    // 拷贝时，管理权限转移，被拷贝对象ap1悬空
    auto_ptr<Date> ap2(ap1);
    // 空指针访问，ap1对象已经悬空
    // ap1->_year++;

    unique_ptr<Date> up1(new Date);
    // 不支持拷贝
    // unique_ptr<Date> up2(up1);
    // 支持移动，但是移动后up1也悬空，所以使用移动要谨慎
    unique_ptr<Date> up3(move(up1));

    shared_ptr<Date> sp1(new Date);
    // 支持拷贝
    shared_ptr<Date> sp2(sp1);
    shared_ptr<Date> sp3(sp2);
    cout << sp1.use_count() << endl;
    sp1->_year++;
    cout << sp1->_year << endl;
    cout << sp2->_year << endl;
    cout << sp3->_year << endl;
    // 支持移动，但是移动后sp1也悬空，所以使用移动要谨慎
    shared_ptr<Date> sp4(move(sp1));
    return 0;
}

template <class T>
void DeleteArrayFunc(T *ptr)
{
    delete[] ptr;
}

template <class T>
class DeleteArray
{
public:
    void operator()(T *ptr)
    {
        delete[] ptr;
    }
};

class Fclose
{
public:
    void operator()(FILE *ptr)
    {
        cout << "fclose:" << ptr << endl;
        fclose(ptr);
    }
};

int test2()
{
    // 这样实现程序会崩溃
    // unique_ptr<Date> up1(new Date[10]);
    // shared_ptr<Date> sp1(new Date[10]);

    // 解决方案1
    // 因为new[]经常使用，所以unique_ptr和shared_ptr
    // 实现了一个特化版本，这个特化版本析构时用的delete[]
    unique_ptr<Date[]> up1(new Date[5]);
    shared_ptr<Date[]> sp1(new Date[5]);

    // 解决方案2
    // 仿函数对象做删除器
    // unique_ptr<Date, DeleteArray<Date>> up2(new Date[5], DeleteArray<Date>());

    // unique_ptr和shared_ptr支持删除器的方式有所不同
    // unique_ptr是在类模板参数支持的，shared_ptr是构造函数参数支持的
    // 这里没有使用相同的方式还是挺坑的
    // 使用仿函数unique_ptr可以不在构造函数传递，因为仿函数类型构造的对象直接就可以调用
    // 但是下面的函数指针和lambda的类型不可以
    unique_ptr<Date, DeleteArray<Date>> up2(new Date[5]);
    shared_ptr<Date> sp2(new Date[5], DeleteArray<Date>());

    // 函数指针做删除器
    unique_ptr<Date, void (*)(Date *)> up3(new Date[5], DeleteArrayFunc<Date>);
    shared_ptr<Date> sp3(new Date[5], DeleteArrayFunc<Date>);

    // lambda表达式做删除器
    auto delArrOBJ = [](Date *ptr)
    { delete[] ptr; };
    unique_ptr<Date, decltype(delArrOBJ)> up4(new Date[5], delArrOBJ);
    shared_ptr<Date> sp4(new Date[5], delArrOBJ);

    // 实现其他资源管理的删除器
    shared_ptr<FILE> sp5(fopen("Test.cpp", "r"), Fclose());
    shared_ptr<FILE> sp6(fopen("Test.cpp", "r"), [](FILE *ptr)
                         {
        cout << "fclose:" << ptr << endl;
        fclose(ptr); });
    return 0;
}


int main()
{
    std::shared_ptr<string> sp1(new string("111111"));
    std::shared_ptr<string> sp2(sp1);
    std::weak_ptr<string> wp = sp1;
    cout << wp.expired() << endl;
    cout << wp.use_count() << endl;
    
    // sp1和sp2都指向了其他资源，则weak_ptr就过期了
    sp1 = make_shared<string>("222222");
    cout << wp.expired() << endl;
    cout << wp.use_count() << endl;
    sp2 = make_shared<string>("333333");
    cout << wp.expired() << endl;
    cout << wp.use_count() << endl;
    wp = sp1;
    
    //std::shared_ptr<string> sp3 = wp.lock();
    auto sp3 = wp.lock();
    cout << wp.expired() << endl;
    cout << wp.use_count() << endl;
    *sp3 += "###";
    cout << *sp1 << endl;
    return 0;
}