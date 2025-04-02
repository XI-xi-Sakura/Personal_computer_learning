## [string介绍](https://legacy.cplusplus.com/reference/string/string/)

1. 字符串是表示字符序列的类
2. 标准的字符串类提供了对此类对象的支持，其接口类似于标准字符容器的接口，但添加了专门用于操作单字节字符字符串的设计特性。
3. string类是使用char(即作为它的字符类型，使用它的默认char_traits和分配器类型(关于模板的更多信息，请参阅basic_string)。
4. string类是basic_string模板类的一个实例，**它使用char来实例化basic_string模板类**，并用char_traits和allocator作为basic_string的默认参数(根于更多的模板信息请参考basic_string)。
5. 注意，这个类独立于所使用的编码来处理字节:如果用来处理多字节或变长字符(如UTF-8)的序列，这个类的所有成员(如长度或大小)以及它的迭代器，将仍然按照字节(而不是实际编码的字符)来操作。



总结：
1. string是表示字符串的字符串类
2. 该类的接口与常规容器的接口基本相同，再添加了一些专门用来操作string的常规操作。
3. string在底层实际是：basic_string模板类的别名，typedef basic_string<char, char_traits, allocator> string;
4. 不能操作多字节或者变长字符的序列。
 
在使用string类时，必须包含`#include`头文件以及`using namespace std`;
## string类的常用接口
### [string类对象的常见构造](https://legacy.cplusplus.com/reference/string/string/string/)
|函数名称|    功能说明|
|--|--|
|    string()| 构造空的string类对象，即空字符串 
|string(const char* s)|用C-string来构造string类对象
|string(size_t n, char c)|string类对象中包含n个字符c
|string(const string&s)|拷贝构造函数

```cpp
void Teststring()
{
 string s1; // 构造空的string类对象s1
 string s2("hello bit"); // 用C格式字符串构造string类对象s2
 string s3(s2); // 拷贝构造s3
}
```

### string类对象的容量操作
|函数名称|  功能说明|
|--|--|
|size  | 返回字符串有效长度
|length|返回字符串有效长度
|capacity|返回空间总大小
|empty|检测字符串释放为空串，是返回true，否则返回false
|clear|清空有效字符
|reserve|为字符串预留空间
|resize|将有效字符的个数该成n个，多出的空间用字符c填充

```cpp
void Teststring1()
{
	// 注意：string类对象支持直接用cin和cout进行输入和输出
	string s("hello, bit!!!");
	cout << s.size() << endl;
	cout << s.length() << endl;
	cout << s.capacity() << endl;
	cout << s << endl;

	// 将s中的字符串清空，注意清空时只是将size清0，不改变底层空间的大小
	s.clear();
	cout << s.size() << endl;
	cout << s.capacity() << endl;

	// 将s中有效字符个数增加到10个，多出位置用'a'进行填充
	// “aaaaaaaaaa”
	s.resize(10, 'a');
	cout << s.size() << endl;
	cout << s.capacity() << endl;

	// 将s中有效字符个数增加到15个，多出位置用缺省值'\0'进行填充
	// "aaaaaaaaaa\0\0\0\0\0"
	// 注意此时s中有效字符个数已经增加到15个
	s.resize(15);
	cout << s.size() << endl;
	cout << s.capacity() << endl;
	cout << s << endl;

	// 将s中有效字符个数缩小到5个
	s.resize(5);
	cout << s.size() << endl;
	cout << s.capacity() << endl;
	cout << s << endl;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/b9d34ef77f1d40d18bef9b858875ee34.png)
注意：
1. size()与length()方法底层实现原理完全相同，引入size()的原因是为了与其他容器的接口保持一 致，一般情况下基本都是用size()。
2. clear()只是将string中有效字符清空，不改变底层空间大小。
3. resize(size_t n) 与 resize(size_t n, char c)都是将字符串中有效字符个数改变到n个，不同的是当字符个数增多时：resize(n)用0来填充多出的元素空间，resize(size_t n, char c)用字符c来填充多出的元素空间。注意：resize在改变元素个数时，如果是将元素个数增多，可能会改变底层容量的大小，如果是将元素个数减少，底层空间总大小不变。
4. reserve(size_t res_arg=0)：为string预留空间，不改变有效元素个数，当reserve的参数小于string的底层空间总大小时，reserver不会改变容量大小。


```cpp
void Teststring2()
{
	string s;
	// 测试reserve是否会改变string中有效元素个数
	s.reserve(100);
	cout << s.size() << endl;
	cout << s.capacity() << endl;

	// 测试reserve参数小于string的底层空间大小时，是否会将空间缩小
	s.reserve(50);
	cout << s.size() << endl;
	cout << s.capacity() << endl;
}

// 利用reserve提高插入数据的效率，避免增容带来的开销
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/3be6b4f561b24e3b907f41dffdbf7bb9.png)

###  string类对象的访问及遍历操作
|函数名称|功能说明  |
|--|--|
|operator[ ]  | 返回pos位置的字符，const string类对象调用
|begin/end|begin获取一个字符的迭代器 /end获取最后一个字符下一个位置的迭代器
|rebegin/rend|begin获取一个字符的迭代器 + end获取最后一个字符下一个位置的迭代器
|范围for|C++11支持更简洁的范围for的新遍历方式


```cpp
// string的遍历
// begin()+end()   for+[]  范围for
// 注意：string遍历时使用最多的还是for+下标 或者 范围for(C++11后才支持)
// begin()+end()大多数使用在需要使用STL提供的算法操作string时，比如：采用reverse逆置string
void Teststring3()
{
	string s1("hello Bit");
	const string s2("Hello Bit");
	cout << s1 << " " << s2 << endl;
	cout << s1[0] << " " << s2[0] << endl;

	s1[0] = 'H';
	cout << s1 << endl;

	// s2[0] = 'h';   代码编译失败，因为const类型对象不能修改
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/c673178da0a147d8b9d0d7fdb9342f08.png)


```cpp
void Teststring4()
{
	string s("hello Bit");
	// 3种遍历方式：
	// 需要注意的以下三种方式除了遍历string对象，还可以遍历是修改string中的字符，
	// 另外以下三种方式对于string而言，第一种使用最多
	

	// 1. for+operator[]
	for (size_t i = 0; i < s.size(); ++i)
		cout << s[i] ;
	cout << endl;

	// 2.迭代器
	string::iterator it = s.begin();
	while (it != s.end())
	{
		cout << *it;
		++it;
	}
	cout << endl;

	// string::reverse_iterator rit = s.rbegin();
	// C++11之后，直接使用auto定义迭代器，让编译器推到迭代器的类型
	auto rit = s.rbegin();
	while (rit != s.rend())
	{
		cout << *rit ;
		rit++;
	}
	cout << endl;


	// 3.范围for
	for (auto ch : s)
		cout << ch;
	cout << endl;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/73baa7bfeee24eccb9c06ca6b7075c96.png)
### string类对象的修改操作
|函数名称|功能说明  |
|--|--|
| push_back |在字符串后尾插字符c
|append|在字符串后追加一个字符串
|operator+=|在字符串后追加字符串str
|c_str|返回C格式字符串
|find|从字符串pos位置开始往后找字符c，返回该字符在字符串中的位置
|rfind|从字符串pos位置开始往前找字符c，返回该字符在字符串中的位置
|substr|在str中从pos位置开始，截取n个字符，然后将其返回

注意：
1. 在string尾部追加字符时，s.push_back(c) / s.append(1, c) / s += 'c'三种的实现方式差不多，一般情况下string类的+=操作用的比较多，+=操作不仅可以连接单个字符，还可以连接字符串。
2. 对string操作时，如果能够大概预估到放多少字符，可以先通过reserve把空间预留好。

```cpp
// 利用reserve提高插入数据的效率，避免增容带来的开销
//====================================================================================
void TestPushBack()
{
	string s;
	size_t sz = s.capacity();
	cout << "making s grow:\n";
	for (int i = 0; i < 100; ++i)
	{
		s.push_back('c');
		if (sz != s.capacity())
		{
			sz = s.capacity();
			cout << "capacity changed: " << sz << '\n';
		}
	}
}
```

```cpp
// 构建string时，如果提前已经知道string中大概要放多少个元素，可以提前将string中空间设置好
void TestPushBackReserve()
{
	string s;
	s.reserve(100);
	size_t sz = s.capacity();

	cout << "making s grow:\n";
	for (int i = 0; i < 100; ++i)
	{
		s.push_back('c');
		if (sz != s.capacity())
		{
			sz = s.capacity();
			cout << "capacity changed: " << sz << '\n';
		}
	}
}

```

```cpp
/////////////////////////////////////////////////////////////
// 测试string：
// 1. 插入(拼接)方式：push_back  append  operator+= 
// 2. 正向和反向查找：find() + rfind()
// 3. 截取子串：substr()
// 4. 删除：erase
void Teststring5()
{
	string str;
	str.push_back(' ');   // 在str后插入空格
	str.append("hello");  // 在str后追加一个字符"hello"
	str += 'b';           // 在str后追加一个字符'b'   
	str += "it";          // 在str后追加一个字符串"it"
	cout << str << endl;
	cout << str.c_str() << endl;   // 以C语言的方式打印字符串

	// 获取file的后缀
	string file("string.cpp");
	size_t pos = file.rfind('.');
	string suffix(file.substr(pos, file.size() - pos));
	cout << suffix << endl;

	// npos是string里面的一个静态成员变量
	// static const size_t npos = -1;

	// 取出url中的域名
	string url("http://www.cplusplus.com/reference/string/string/find/");
	cout << url << endl;
	size_t start = url.find("://");
	if (start == string::npos)
	{
		cout << "invalid url" << endl;
		return;
	}
	start += 3;
	size_t finish = url.find('/', start);
	string address = url.substr(start, finish - start);
	cout << address << endl;

	// 删除url的协议前缀
	pos = url.find("://");
	url.erase(0, pos + 3);
	cout << url << endl;
}
```
###  string类非成员函数
|函数|功能说明  |
|--|--|
| operator+ | 尽量少用，因为传值返回，导致深拷贝效率低
|operator<<|输入运算符重载
|operator>>|输出运算符重载
|getline|获取一行字符串
|relational operators|大小比较
## vs和g++下string结构的说明
注意：下述结构是在32位平台下进行验证，32位平台下指针占4个字节。

### vs下string的结构

string总共占28个字节，内部结构稍微复杂一点，先是有一个联合体，联合体用来定义string中字符串的存储空间：
当字符串长度小于16时，使用内部固定的字符数组来存放；
当字符串长度大于等于16时，从堆上开辟空间。

```cpp
union _Bxty
{

 // storage for small buffer or pointer to larger one
 
 value_type _Buf[_BUF_SIZE];
 pointer _Ptr;
 char _Alias[_BUF_SIZE];    // to permit aliasing

} _Bx;
```
这种设计也是有一定道理的，大多数情况下字符串的长度都小于16，那string对象创建好之后，内部已经有了16个字符数组的固定空间，不需要通过堆创建，效率高。
其次：还有一个size_t字段保存字符串长度，一个size_t字段保存从堆上开辟空间总的容量；最后还有一个指针做一些其他事情。
故总共占16+4+4+4=28个字节。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/91b3286403134b66a4d831e303a9a73d.png)
### g++下string的结构
G++下，string是通过写时拷贝实现的，string对象总共占4个字节，内部只包含了一个指针，该指针将来指向一块堆空间，内部包含了如下字段：
空间总大小
字符串有效长度
引用计数

```cpp
struct _Rep_base
{
 size_type _M_length;
 size_type _M_capacity;
 _Atomic_word _M_refcount;
};
```

