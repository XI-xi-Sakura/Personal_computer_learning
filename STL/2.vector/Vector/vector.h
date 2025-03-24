#pragma once
#include <assert.h>
#include <iostream>
#include <list>
#include <string>
#include <vector>
#include <algorithm>
using namespace std;

// 抓重点

namespace bit
{
    /*template<class T>
    class vector
    {
    public:
        typedef T* iterator;

    private:
        T* _a;
        size_t _size;
        size_t _capacity;
    };*/

    template <class T>
    class vector
    {
    public:
        typedef T *iterator;
        typedef const T *const_iterator;

        const_iterator begin() const
        {
            return _start;
        }

        const_iterator end() const
        {
            return _finish;
        }

        iterator begin()
        {
            return _start;
        }

        iterator end()
        {
            return _finish;
        }

        // 类模板的成员函数
        // 函数模板 -- 目的支持任意容器的迭代器区间初始化
        template <class InputIterator>
        vector(InputIterator first, InputIterator last)
        {
            while (first != last)
            {
                push_back(*first);
                ++first;
            }
        }

        vector(size_t n, const T &val = T())
        {
            reserve(n);
            for (size_t i = 0; i < n; i++)
            {
                push_back(val);
            }
        }

        vector(initializer_list<T> il)
        {
            reserve(il.size());
            for (auto e : il)
            {
                push_back(e);
            }
        }

        vector(int n, const T &val = T())
        {
            reserve(n);
            for (int i = 0; i < n; i++)
            {
                push_back(val);
            }
        }

        // 强制编译器生成默认的
        vector() = default;

        // v2(v1)
        vector(const vector<T> &v)
        {
            reserve(v.capacity());
            for (auto e : v)
            {
                push_back(e);
            }
        }
        // 16:05继续
        void swap(vector<T> &v)
        {
            std::swap(_start, v._start);
            std::swap(_finish, v._finish);
            std::swap(_end_of_storage, v._end_of_storage);
        }

        // v1 = v3
        vector<T> &operator=(vector<T> v)
        {
            swap(v);
            return *this;
        }

        ~vector()
        {
            if (_start)
            {
                delete[] _start;
                _start = _finish = _end_of_storage = nullptr;
            }
        }

        void reserve(size_t n)
        {
            if (n > capacity())
            {
                size_t oldsize = size();
                T *tmp = new T[n];

                if (_start)
                {
                    // memcpy(tmp, _start, sizeof(T) * old_size);
                    for (size_t i = 0; i < oldsize; i++)
                    {
                        tmp[i] = _start[i];
                    }

                    delete[] _start;
                }

                _start = tmp;
                _finish = _start + oldsize;
                _end_of_storage = _start + n;
            }
        }

        size_t capacity() const
        {
            return _end_of_storage - _start;
        }

        size_t size() const
        {
            return _finish - _start;
        }

        T &operator[](size_t i)
        {
            assert(i < size());

            return _start[i];
        }

        const T &operator[](size_t i) const
        {
            assert(i < size());

            return _start[i];
        }

        void push_back(const T &x)
        {
            /*if (_finish == _end_of_storage)
            {
                size_t newcapacity = capacity() == 0 ? 4 : capacity() * 2;
                reserve(newcapacity);
            }

            *_finish = x;
            ++_finish;*/

            insert(end(), x);
        }

        void pop_back()
        {
            assert(size() > 0);

            --_finish;
        }

        iterator insert(iterator pos, const T &x)
        {
            assert(pos >= _start);
            assert(pos <= _finish);

            if (_finish == _end_of_storage)
            {
                size_t len = pos - _start;

                size_t newcapacity = capacity() == 0 ? 4 : capacity() * 2;
                reserve(newcapacity);

                pos = _start + len;
            }

            iterator end = _finish - 1;
            while (end >= pos)
            {
                *(end + 1) = *end;
                --end;
            }

            *pos = x;
            ++_finish;

            return pos;
        }

        // 迭代器
        // 勿在浮沙筑高台
        void erase(iterator pos)
        {
            assert(pos >= _start);
            assert(pos < _finish);

            iterator it = pos + 1;
            while (it != _finish)
            {
                *(it - 1) = *it;
                ++it;
            }

            --_finish;
        }

    private:
        iterator _start = nullptr;
        iterator _finish = nullptr;
        iterator _end_of_storage = nullptr;
    };

    void test_vector1()
    {
        bit::vector<int> v1;
        v1.push_back(1);
        v1.push_back(2);
        v1.push_back(3);
        v1.push_back(4);
        v1.push_back(4);
        v1.push_back(4);

        for (size_t i = 0; i < v1.size(); i++)
        {
            cout << v1[i] << " ";
        }
        cout << endl;

        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;

        // int* it = v1.begin();
        //  封装
        bit::vector<int>::iterator it = v1.begin();
        while (it != v1.end())
        {
            cout << *it << " ";
            ++it;
        }
        cout << endl;

        // cout << typeid(it).name() << endl;
    }

    void test_vector2()
    {
        bit::vector<int> v1;
        v1.push_back(1);
        v1.push_back(2);
        v1.push_back(3);
        v1.push_back(4);
        // v1.push_back(5);
        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;

        v1.insert(v1.begin(), 0);
        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;

        v1.erase(v1.begin());
        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;

        // v1.insert(v1.begin() + 2, 20);
        // for (auto e : v1)
        //{
        //	cout << e << " ";
        // }
        // cout << endl;

        int x;
        cin >> x;
        // 没有x就不插入，有x的前面插入  10:50继续
        vector<int>::iterator it = find(v1.begin(), v1.end(), x);
        if (it != v1.end())
        {
            // insert以后it这个实参会不会失效？
            it = v1.insert(it, 1000);

            // 建议失效后迭代器不要访问。除非赋值更新一下这个失效的迭代器
            cout << *it << endl;
        }

        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;
    }

    void test_vector3()
    {
        std::vector<int> v1;
        v1.push_back(1);
        v1.push_back(2);
        v1.push_back(3);
        v1.push_back(4);
        v1.push_back(5);

        int x;
        cin >> x;
        std::vector<int>::iterator it = find(v1.begin(), v1.end(), x);
        if (it != v1.end())
        {
            // erase it以后，it是否失效呢？失效
            it = v1.erase(it);

            if (it != v1.end())
                cout << *it << endl;
        }

        cout << typeid(it).name() << endl;
    }

    void test_vector4()
    {
        std::vector<int> v1;
        v1.push_back(1);
        v1.push_back(2);
        v1.push_back(3);
        v1.push_back(4);
        v1.push_back(5);

        std::vector<int>::iterator it = v1.begin();
        while (it != v1.end())
        {
            if (*it % 2 == 0)
            {
                // erase it以后，it是否失效呢？失效
                it = v1.erase(it);
            }
            else
            {
                ++it;
            }
        }

        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;
    }

    void test_vector5()
    {
        vector<int> v1;
        v1.push_back(1);
        v1.push_back(2);
        v1.push_back(3);
        v1.push_back(4);
        v1.push_back(5);

        vector<int> v2(v1);
        for (auto e : v2)
        {
            cout << e << " ";
        }
        cout << endl;

        vector<int> v3;
        v3.push_back(10);
        v3.push_back(20);
        v3.push_back(30);

        v1 = v3;

        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;
    }

    // void test_vector6()
    //{
    //	vector<int> v1;
    //	v1.push_back(1);
    //	v1.push_back(2);
    //	v1.push_back(3);
    //	v1.push_back(4);
    //	v1.push_back(5);

    //	vector<int> v2(v1.begin()+1, v1.end());
    //	for (auto e : v2)
    //	{
    //		cout << e << " ";
    //	}
    //	cout << endl;

    //	string s("hello");
    //	vector<int> v3(s.begin(), s.end());
    //	for (auto e : v3)
    //	{
    //		cout << e << " ";
    //	}
    //	cout << endl;

    //	list<int> lt;
    //	lt.push_back(100);
    //	lt.push_back(100);
    //	lt.push_back(100);
    //	vector<int> v4(lt.begin(), lt.end());
    //	for (auto e : v4)
    //	{
    //		cout << e << " ";
    //	}
    //	cout << endl;
    //}

    void test_vector7()
    {
        // C++内置类型进行了升序，也有构造
        int i = 0;
        int j(1);
        int k = int();
        int x = int(2);

        vector<string> v1(10);
        vector<string> v2(10, "xxx");
        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;

        for (auto e : v2)
        {
            cout << e << " ";
        }
        cout << endl;

        vector<int> v3(10, 1);
        for (auto e : v3)
        {
            cout << e << " ";
        }
        cout << endl;

        vector<int> v4(10, 1);
        for (auto e : v4)
        {
            cout << e << " ";
        }
        cout << endl;
    }

    class A
    {
    public:
        A(int a1 = 0)
            : _a1(a1), _a2(0)
        {
        }

        A(int a1, int a2)
            : _a1(a1), _a2(a2)
        {
        }

    private:
        int _a1;
        int _a2;
    };

    void test_vector8()
    {
        // 17:20继续
        // 单参数和多参数对象隐式类型转换
        // 省略赋值符号
        A aa1(1, 1);
        A aa2 = {2, 2};
        A aa9{2, 2}; // 不要
        const A &aa8 = {1, 1};

        A aa3(1);
        A aa4 = 1;

        A aa5(1);
        A aa6 = {1}; // 不要
        A aa7{1};    // 不要

        // 这里的隐式类型转换，跟上面不一样，这里参数个数不固定
        vector<int> v1({1, 2, 3, 4, 5, 6});
        vector<int> v2 = {10, 20, 30};
        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;
        for (auto e : v2)
        {
            cout << e << " ";
        }
        cout << endl;

        auto il1 = {1, 2, 3, 4, 5, 6};
        initializer_list<int> il2 = {1, 2, 3};
        cout << typeid(il1).name() << endl;
        cout << sizeof(il2) << endl;
        for (auto e : il1)
        {
            cout << e << " ";
        }
        cout << endl;

        vector<A> v3 = {1, A(1), A(2, 2), A{1}, A{2, 2}, {1}, {2, 2}};
    }

    void test_vector9()
    {
        vector<string> v1;
        v1.push_back("111111111111111111");
        v1.push_back("111111111111111111");
        v1.push_back("111111111111111111");
        v1.push_back("111111111111111111");
        v1.push_back("111111111111111111");
        for (auto e : v1)
        {
            cout << e << " ";
        }
        cout << endl;
    }
}
