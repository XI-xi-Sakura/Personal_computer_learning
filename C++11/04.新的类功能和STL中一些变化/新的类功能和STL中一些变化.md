## 新的类功能
### 默认的移动构造和移动赋值
原来C++类中有6个默认成员函数：
- 构造函数
- 析构函数
- 拷贝构造函数
- 拷贝赋值重载
- 取地址重载
- const取地址重载

其中前4个较为重要，后两个用处不大。默认成员函数是指我们不写时编译器会生成一个默认的。

C++11新增了两个默认成员函数，即**移动构造函数**和**移动赋值运算符重载**。

如果你没有自己实现移动构造函数，且没有实现析构函数、拷贝构造、拷贝赋值重载中的任意一个，那么编译器会自动生成一个默认移动构造。

默认生成的移动构造函数，对于内置类型成员会执行逐成员按字节拷贝，自定义类型成员，则需要看这个成员是否实现移动构造，如果实现了就调用移动构造，没有实现就调用拷贝构造。

如果你没有自己实现移动赋值重载函数，且没有实现析构函数、拷贝构造、拷贝赋值重载中的任意一个，那么编译器会自动生成一个默认移动赋值。默认生成的移动构造函数，对于内置类型成员会执行逐成员按字节拷贝，自定义类型成员，则需要看这个成员是否实现移动赋值，如果实现了就调用移动赋值，没有实现就调用拷贝赋值。（默认移动赋值跟上面移动构造完全类似）

如果你提供了移动构造或者移动赋值，编译器不会自动提供拷贝构造和拷贝赋值。
```cpp
class Person
{
public:
    Person(const char* name = "", int age = 0)
    :_name(name)
    , _age(age)
    {}

    /*Person(const Person& p)
    :_name(p._name)
    ,_age(p._age)
    {}*/

    /*Person& operator=(const Person& p)
    {
        if(this != &p)
        {
            _name = p._name;
            _age = p._age;
        }
        return *this;
    }*/

    /*~Person()
    {}*/

private:
    bit::string _name;
    int _age;
};

int main()
{
    Person s1;
    Person s2 = s1;
    Person s3 = std::move(s1);
    Person s4;
    s4 = std::move(s2);

    return 0;
}
```
### 成员变量声明时给缺省值
成员变量声明时给缺省值是给初始化列表用的，如果没有显示在初始化列表初始化，就会在初始化列表用这个缺省值初始化。
### defult和delete
C++11可以让你更好的控制要使用的默认函数。假设你要使用某个默认的函数，但是因为一些原因这个函数没有默认生成。

比如：我们提供了拷贝构造，就不会生成移动构造了，那么我们可以**使用default关键字显示指定移动构造生成**。

如果想要限制某些默认函数的生成，在C++98中，是**将该函数设置成private，并且只声明不定义**，这样只要其他人想要调用就会报错。

在C++11中更简单，只需在该函数声明加上=delete即可，该语法**指示编译器不生成对应函数的默认版本**，称=delete修饰的函数为**删除函数**。
```cpp
class Person
{
public:
    Person(const char* name = "", int age = 0)
    :_name(name)
    , _age(age)
    {}

    Person(const Person& p)
    :_name(p._name)
    ,_age(p._age)
    {}

    Person(Person&& p) = default;

    //Person(const Person& p) = delete;

private:
    bit::string _name;
    int _age;
};

int main()
{
    Person s1;
    Person s2 = s1;
    Person s3 = std::move(s1);

    return 0;
}
```

## STL中一些变化
- 下图圈起来的就是STL中的新容器，但是实际最有用的是`unordered_map`和`unordered_set`。这两个前面已经进行了非常详细的讲解，其他的了解一下即可。
- STL中容器的新接口也不少，最重要的就是右值引用和移动语义相关的push/insert/emplace系列接口和移动构造和移动赋值，还有initializer_list版本的构造等，这些前面都讲过了，还有一些如cbegin/cend等需要时查查文档即可。


![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/905fb7bc7a5c4ac0a5cab1ca19a7d8c0.png)

