#define _CRT_SECURE_NO_WARNINGS 1
#include <iostream>
#include <assert.h>
#include <string.h>
#include <algorithm>
using namespace std;
namespace bit
{
    class string
    {
    public:
        typedef char *iterator;
        typedef const char *const_iterator;

        iterator begin()
        {
            return _str;
        }
        iterator end()
        {
            return _str + _size;
        }
        const_iterator begin() const
        {
            return _str;
        }
        const_iterator end() const
        {
            return _str + _size;
        }
        string(const char *str = "")
            : _size(strlen(str)), _capacity(_size)
        {
            cout << "string(char* str)-构造" << endl;
            _str = new char[_capacity + 1];
            strcpy(_str, str);
        }
        void swap(string &s)
        {
            ::swap(_str, s._str);
            ::swap(_size, s._size);
            ::swap(_capacity, s._capacity);
        }
        string(const string &s)
        {
            cout << "string(const string& s) -- 拷贝构造" << endl;
            reserve(s._capacity);
            for (auto ch : s)
            {
                push_back(ch);
            }
        }
        // 移动构造
        string(string &&s)
        {
            cout << "string(string&& s)--移动构造" << endl;
            swap(s);
        }
        string &operator=(const string &s)
        {
            cout << "string& operator=(const string&s)--拷贝赋值" << endl;
            if (this != &s)
            {
                _str[0] = '0';
                _size = 0;
                reserve(s._capacity);
                for (auto ch : s)
                {
                    push_back(ch);
                }
            }
            return *this;
        }
        // 移动赋值
        string &operator=(string &&s)
        {
            cout << "string& operator=(string&&s)--移动赋值" << endl;
            swap(s);
            return *this;
        }
        ~string()
        {
            cout << "~string()--析构" << endl;
            delete[] _str;
            _str = nullptr;
        }
        char &operator[](size_t pos)
        {
            assert(pos < _size);
            return _str[pos];
        }
        void reserve(size_t n)
        {
            if (n > _capacity)
            {
                char *tmp = new char[n + 1];
                if (_str)
                {
                    strcpy(tmp, _str);
                    delete[] _str;
                }
                _str = tmp;
                _capacity = n;
            }
        }
        void push_back(char ch)
        {
            if (_size >= _capacity)
            {
                size_t newcapacity = _capacity == 0 ? 4 : _capacity * 2;
                reserve(newcapacity);
            }
            _str[_size] = ch;
            ++_size;
            _str[_size] = '\0';
        }
        string &operator+=(char ch)
        {
            push_back(ch);
            return *this;
        }
        const char *C_str() const
        {
            return _str;
        }
        size_t size() const
        {
            return _size;
        }

    private:
        char *_str = nullptr;
        size_t _size = 0;
        size_t _capacity = 0;
    };
}
namespace bit
{
    template <class T>
    struct ListNode
    {
        ListNode<T> *_next;
        ListNode<T> *_prev;

        T _data;

        ListNode(const T &data = T())
            : _next(nullptr), _prev(nullptr), _data(data) {}

        ListNode(T &&data)
            : _next(nullptr), _prev(nullptr), _data(move(data)) {}
    };

    template <class T, class Ref, class Ptr>
    struct ListIterator
    {
        typedef ListNode<T> Node;
        typedef ListIterator<T, Ref, Ptr> Self;

        Node *_node;

        ListIterator(Node *node)
            : _node(node) {}

        Self &operator++()
        {
            _node = _node->_next;
            return *this;
        }
        T operator*()
        {
            return _node->_data;
        }
        bool operator!=(const Self &it)
        {
            return _node != it._node;
        }
    };
    template <class T>
    class List
    {
        typedef ListNode<T> Node;

    public:
        typedef ListIterator<T, T &, T> iterator;
        typedef ListIterator<T, const T &, const T> const_iterator;
        iterator begin()
        {
            return iterator(_head->_next);
        }
        iterator end()
        {
            return iterator(_head);
        }
        void empty_init()
        {
            _head = new Node();
            _head->_next = _head;
            _head->_prev = _head;
        }
        List()
        {
            empty_init();
        }
        void push_back(const T &x)
        {
            insert(end(), x);
        }
        void push_back(T &&x)
        {
            insert(end(), move(x));
        }
        iterator insert(iterator pos, const T &x)
        {
            Node *cur = pos._node;
            Node *newnode = new Node(x);
            Node *prev = cur->_prev;

            // prev newnode cur
            prev->_next = newnode;
            newnode->_prev = prev;
            newnode->_next = cur;
            cur->_prev = newnode;

            return iterator(newnode);
        }
        iterator insert(iterator pos, T &&x)
        {
            Node *cur = pos._node;
            Node *newnode = new Node(move(x));
            Node *prev = cur->_prev;

            // prev newnode cur
            prev->_next = newnode;
            newnode->_prev = prev;
            newnode->_next = cur;
            cur->_prev = newnode;

            return iterator(newnode);
        }

    private:
        Node *_head;
    };
}
// int main()
// {
//     bit::string s1("xxxxx");
//     // 拷贝构造
//     bit::string s2 = s1;
//     // 构造+移动构造,优化后直接构造
//     bit::string s3 = bit::string("yyyyy");
//     // 移动构造
//     bit::string s4 = move(s1);
//     cout << "******************************" << endl;

//     return 0;
// }

template <class T>
void Function(T &&t)
{
    int a = 0;
    T x = a;
    x++;
  
    cout << a << endl;
    cout << x << endl;
    cout << &a << endl;
    cout << &x << endl
         << endl;
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
    // a是左值,推导出T为const int&,引用折叠,模板实例化为void Function(const int&
    // t)
    // 所以Function内部会编译报错,x不能++
    // Function(b); // const 左值

    // std::move(b)右值,推导出T为const int,模板实例化为void Function(const int&&
    // t)
    // 所以Function内部会编译报错,x不能++
    // Function(std::move(b)); // const 右值

    return 0;
}