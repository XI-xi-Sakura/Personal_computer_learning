#include <iostream>
#include <memory>
#include <functional>
using namespace std;

namespace bit
{
    template <class T>
    class auto_ptr
    {
    public:
        auto_ptr(T *ptr)
            : _ptr(ptr)
        {
        }
        auto_ptr(auto_ptr<T> &sp)
            : _ptr(sp._ptr)
        {
            // 管理权转移
            sp._ptr = nullptr;  //重点
        }
        auto_ptr<T> &operator=(auto_ptr<T> &ap)
        {
            // 检测是否为自己给自己赋值
            if (this != &ap)
            {
                // 释放当前对象中资源
                if (_ptr)
                    delete _ptr;
                // 转移ap中资源到当前对象中
                _ptr = ap._ptr;
                ap._ptr = NULL;   //重点
            }
            return *this;
        }
        ~auto_ptr()
        {
            if (_ptr)
            {
                cout << "delete:" << _ptr << endl;
                delete _ptr;
            }
        }
        // 像指针一样使用
        T &operator*()
        {
            return *_ptr;
        }
        T *operator->()
        {
            return _ptr;
        }

    private:
        T *_ptr;
    };

    template <class T>
    class unique_ptr
    {
    public:
        explicit unique_ptr(T *ptr)
            : _ptr(ptr)
        {
        }
        unique_ptr()
        {
        }
        ~unique_ptr()
        {
            if (_ptr)
            {
                cout << "delete:" << _ptr << endl;
                delete _ptr;
            }
        }
        // 像指针一样使用
        T &operator*()
        {
            return *_ptr;
        }
        T *operator->()
        {
            return _ptr;
        }
        unique_ptr(const unique_ptr<T> &sp) = delete;   //直接不让拷贝构造和拷贝赋值
        unique_ptr<T> &operator=(const unique_ptr<T> &sp) = delete;
        unique_ptr(unique_ptr<T> &&sp)
            : _ptr(sp._ptr)
        {
            sp._ptr = nullptr;
        }
        unique_ptr<T> &operator=(unique_ptr<T> &&sp)
        {
            delete _ptr;
            _ptr = sp._ptr;
            sp._ptr = nullptr;
            return *this;
        }

    private:
        T *_ptr;
    };

    template <class T>
    class shared_ptr
    {
    public:
        explicit shared_ptr(T *ptr = nullptr)  //explicit防止隐式转换
            : _ptr(ptr), _pcount(new int(1))
        {
        }
        template <class D>
        shared_ptr(T *ptr, D del)
            : _ptr(ptr), _pcount(new int(1)), _del(del)
        {
        }
        shared_ptr(const shared_ptr<T> &sp)
            : _ptr(sp._ptr), _pcount(sp._pcount), _del(sp._del)
        {
            ++(*_pcount);
        }
        void release()
        {
            if (--(*_pcount) == 0)
            {
                // 最后一个管理的对象，释放资源
                _del(_ptr);
                delete _pcount;
                _ptr = nullptr;
                _pcount = nullptr;
            }
        }
        shared_ptr<T> &operator=(const shared_ptr<T> &sp)
        {
            if (_ptr != sp._ptr)
            {
                release();
                _ptr = sp._ptr;
                _pcount = sp._pcount;
                ++(*_pcount);
                _del = sp._del;
            }
            return *this;
        }
        ~shared_ptr()
        {
            release();
        }
        T *get() const
        {
            return _ptr;
        }
        int use_count() const
        {
            return *_pcount;
        }
        T &operator*()
        {
            return *_ptr;
        }
        T *operator->()
        {
            return _ptr;
        }

    private:
        T *_ptr;
        int *_pcount;  // 引用计数
        // atomic<int>* _pcount;
        function<void(T *)> _del = [](T *ptr)
        { delete ptr; };
    };

    // 需要注意,这里实现的shared_ptr和weak_ptr都是以最简洁的方式实现的，
    // 只能满足基本的功能，这里的weak_ptr lock等功能是无法实现的，想要实现就要
    // 把shared_ptr和weak_ptr一起改了，把引用计数拿出来放到一个单独类型，shared_ptr
    // 和weak_ptr都要存储指向这个类的对象才能实现，有兴趣可以去翻翻源代码
    template <class T>
    class weak_ptr
    {
    public:
        weak_ptr()
        {
        }
        weak_ptr(const shared_ptr<T> &sp)
            : _ptr(sp.get())
        {
        }
        weak_ptr<T> &operator=(const shared_ptr<T> &sp)
        {
            _ptr = sp.get();
            return *this;
        }

    private:
        T *_ptr = nullptr;
    };
}

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

int main()
{
    bit::auto_ptr<Date> ap1(new Date);
    // 拷贝时，管理权限转移，被拷贝对象ap1悬空
    bit::auto_ptr<Date> ap2(ap1);
    // 空指针访问，ap1对象已经悬空
    // ap1->_year++;

    bit::unique_ptr<Date> up1(new Date);
    // 不支持拷贝
    // unique_ptr<Date> up2(up1);
    // 支持移动，但是移动后up1也悬空，所以使用移动要谨慎
    bit::unique_ptr<Date> up3(move(up1));

    bit::shared_ptr<Date> sp1(new Date);
    // 支持拷贝
    bit::shared_ptr<Date> sp2(sp1);
    bit::shared_ptr<Date> sp3(sp2);
    cout << sp1.use_count() << endl;
    sp1->_year++;
    cout << sp1->_year << endl;
    cout << sp2->_year << endl;
    cout << sp3->_year << endl;
    return 0;
}
