#define _CRT_SECURE_NO_WARNINGS 1
#include<iostream>
#include<string>
#include<list>
#include<algorithm>
using namespace std;

void test_string1()
{
	// 常用
	string s1;
	string s2("hello world");
	string s3(s2);

	// 不常用 了解
	string s4(s2, 3, 5);
	string s5(s2, 3);
	string s6(s2, 3, 30);
	string s7("hello world", 5);
	string s8(10, 'x');

	cout << s1 << endl;
	cout << s2 << endl;
	cout << s3 << endl;
	cout << s4 << endl;
	cout << s5 << endl;
	cout << s6 << endl;
	cout << s7 << endl;
	cout << s8 << endl;

	cin >> s1;
	cout << s1 << endl;
}

void push_back(const string& s)
{

}

void test_string2()
{
	// 隐式类型转换
	string s2 = "hello world";
	const string& s3 = "hello world";

	// 构造
	string s1("hello world");
	push_back(s1);

	push_back("hello world");
}

//class string
//{
//public:
//	// 引用返回
//	// 1、减少拷贝
//	// 2、修改返回对象
//	char& operator[](size_t i)
//	{
//		assert(i < _size);
//		return _str[i];
//	}
//private:
//	char* _str;
//	size_t _size;
//	size_t _capacity;
//};

void test_string3()
{
	string s1("hello world");
	s1[0] = 'x';

	cout << s1.size() << endl;
	//cout << s1.length() << endl;

	for (size_t i = 0; i < s1.size(); i++)
	{
		s1[i]++;
	}
	cout << endl;
	s1[0] = 'x';

	// 越界检查
	//s1[20];

	for (size_t i = 0; i < s1.size(); i++)
	{
		//cout << s1.operator[](i) << " ";
		cout << s1[i] << " ";
	}
	cout << endl;

	const string s2("hello world");
	// 不能修改
	//s2[0] = 'x';
}

void test_string4()
{
	string s1("hello world");

	// 遍历方式1：下标+[]
	for (size_t i = 0; i < s1.size(); i++)
	{
		cout << s1[i] << " ";
	}
	cout << endl;

	//遍历方式2: 迭代器
	//string::iterator it1 = s1.begin();
	auto it1 = s1.begin();
	while (it1 != s1.end())
	{
		*it1 += 3;

		cout << *it1 << " ";
		++it1;
	}
	cout << endl;

	//遍历方式3: 范围for
	// 底层角度，他就是迭代器
	cout << s1 << endl;
	for (auto& e : s1)
	{
		e++;
		cout << e << " ";
	}
	cout << endl;
	cout << s1 << endl;

	//cout << typeid(it1).name() << endl;

	list<int> lt1;
	lt1.push_back(1);
	lt1.push_back(2);
	lt1.push_back(3);

	list<int>::iterator it = lt1.begin();
	while (it != lt1.end())
	{
		cout << *it << " ";
		++it;
	}
	cout << endl;

	for (auto e : lt1)
	{
		cout << e << " ";
	}
	cout << endl;

	list<double> lt2;
}

//template<class T>
//struct ListNode
//{
//	ListNode<T>* _next;
//	ListNode<T>* _prev;
//	T _data;
//};
//
//template<class T>
//class list
//{
//private:
//	ListNode<T>* _head;
//};

void test_string5()
{
	const string s1("hello world");
	//string::const_iterator it1 = s1.begin();
	auto it1 = s1.begin();
	while (it1 != s1.end())
	{
		// 不能修改
		//*it1 += 3;

		cout << *it1 << " ";
		++it1;
	}
	cout << endl;

	//string::const_reverse_iterator cit1 = s1.rbegin();
	auto cit1 = s1.rbegin();
	while (cit1 != s1.rend())
	{
		// 不能修改
		//*cit1 += 3;

		cout << *cit1 << " ";
		++cit1;
	}
	cout << endl;

	string s2("hello world");
	string::reverse_iterator it2 = s2.rbegin();
	//auto it2 = s2.rbegin();
	while (it2 != s2.rend())
	{
		//*it2 += 3;

		cout << *it2 << " ";
		++it2;
	}
	cout << endl;
}

void test_string6()
{
	string s1("hello world");
	cout << s1 << endl;

	// s1按字典序排序
	//sort(s1.begin(), s1.end());
	// 第一个和最后一个参与排序
	//sort(++s1.begin(), --s1.end());

	// 前5个排序 [0, 5)
	//sort(s1.begin(), s1.begin()+5);

	cout << s1 << endl;
}

void test_string7()
{
	string s1("hello world");
	cout << s1 << endl;

	s1.push_back('x');
	cout << s1 << endl;

	s1.append(" yyyyyy!!");
	cout << s1 << endl;

	string s2("111111");

	s1 += 'y';
	s1 += "zzzzzzzz";
	s1 += s2;
	cout << s1 << endl;
}

// vector<char>
// string

void test_string8()
{
	string s1("hello world");
	cout << s1 << endl;

	s1.assign("111111");
	cout << s1 << endl;

	// 慎用，因为效率不高 -> O(N)
	// 实践中需求也不高
	string s2("hello world");
	s2.insert(0, "xxxx");
	cout << s2 << endl;

	char ch = 'y';
	cin >> ch;
	s2.insert(0, 1, ch);
	cout << s2 << endl;

	s2.insert(s2.begin(), 'y');
	cout << s2 << endl;

	s2.insert(s2.begin(), s1.begin(), s1.end());
	cout << s2 << endl;
}

void test_string9()
{
	string s1("hello world");
	cout << s1 << endl;

	// erase效率不高，慎用，和insert类似,要挪动数据
	s1.erase(0, 1);
	cout << s1 << endl;

	//s1.erase(5);
	s1.erase(5, 100);
	cout << s1 << endl;

	// replace效率不高，慎用，和insert类似,要挪动数据
	string s2("hello world");
	s2.replace(5, 1, "%20");
	cout << s2 << endl;

	string s3("hello world hello bit");
	for (size_t i = 0; i < s3.size(); )
	{
		if (s3[i] == ' ')
		{
			s3.replace(i, 1, "%20");
			i += 3;
		}
		else
		{
			i++;
		}
	}
	cout << s3 << endl;

	string s4("hello world hello bit");
	string s5;
	for (auto ch : s4)
	{
		if (ch != ' ')
		{
			s5 += ch;
		}
		else
		{
			s5 += "%20";
		}
	}
	cout << s5 << endl;
}

void TestPushBack()
{
	string s;
	// 知道需要多少空间，提前开好
	s.reserve(200);
	s[100] = 'x';

	size_t sz = s.capacity();
	cout << "capacity changed: " << sz << '\n';

	cout << "making s grow:\n";
	for (int i = 0; i < 200; ++i)
	{
		s.push_back('c');
		if (sz != s.capacity())
		{
			sz = s.capacity();
			cout << "capacity changed: " << sz << '\n';
		}
	}
}

void test_string10()
{
	//string s1("hello world hello bit");
	//string s1;
	//cout << s1.size() << endl;
	//cout << s1.capacity() << endl;
	//cout << s1.max_size() << endl;
	//for (size_t i = 0; i < s1.max_size(); i++)
	//{
	//	s1 += 'x';
	//}

	//cout << s1.size() << endl;

	TestPushBack();

	string s1("111111111");
	string s2("11111111111111111111111111111111111111111111111111");

	cout << s1.capacity() << endl;

	// reserve 保留
	// reverse 逆置 反转
	s1.reserve(100);
	cout << s1.capacity() << endl;

	s1.reserve(20);
	cout << s1.capacity() << endl;
}

void test_string11()
{
	string s1;
	//s1.resize(5, '0');
	s1.resize(5);
	s1[4] = '3';
	s1[3] = '4';
	s1[2] = '5';
	s1[1] = '6';
	s1[0] = '7';
	// 76543

	// 插入(空间不够会扩容)
	string s2("hello world");
	s2.resize(20, 'x');

	// 删除
	s2.resize(5);

	try
	{
		//s2[10];
		//s2.at(10);
	}
	catch (const exception& e)
	{
		cout << e.what() << endl;
	}

	string file("test.cpp");
	FILE* fout = fopen(file.c_str(), "r");
	char ch = fgetc(fout);
	while (ch != EOF)
	{
		cout << ch;
		ch = fgetc(fout);
	}
}

void test_string12()
{
	string file("string.cpp.zip");
	size_t pos = file.rfind('.');
	//string suffix = file.substr(pos, file.size() - pos);
	string suffix = file.substr(pos);

	cout << suffix << endl;

	string url("https://gitee.com/ailiangshilove/cpp-class/blob/master/%E8%AF%BE%E4%BB%B6%E4%BB%A3%E7%A0%81/C++%E8%AF%BE%E4%BB%B6V6/string%E7%9A%84%E6%8E%A5%E5%8F%A3%E6%B5%8B%E8%AF%95%E5%8F%8A%E4%BD%BF%E7%94%A8/TestString.cpp");
	size_t pos1 = url.find(':');
	string url1 = url.substr(0, pos1 - 0);
	cout << url1 << endl;

	size_t pos2 = url.find('/', pos1 + 3);
	string url2 = url.substr(pos1 + 3, pos2 - (pos1 + 3));
	cout << url2 << endl;

	string url3 = url.substr(pos2 + 1);
	cout << url3 << endl;
}

//void test_string13()
//{
//
//	// strtok
//	std::string str("Please, replace the vowels in this sentence by asterisks.");
//	std::size_t found = str.find_first_of("aeiou");
//	while (found != std::string::npos)
//	{
//		str[found] = '*';
//		found = str.find_first_of("aeiou", found + 1);
//	}
//
//	std::cout << str << '\n';
//}
//
//int main()
//{
//	test_string13();
//
//	return 0;
//}

void SplitFilename(const std::string& str)
{
	std::cout << "Splitting: " << str << '\n';
	std::size_t found = str.find_last_of("/\\");
	std::cout << " path: " << str.substr(0, found) << '\n';
	std::cout << " file: " << str.substr(found + 1) << '\n';
}

//int main()
//{
//	std::string str1("/usr/bin/man");
//	std::string str2("c:\\windows\\winhelp.exe");
//
//	cout << str2 << endl;
//
//	SplitFilename(str1);
//	SplitFilename(str2);
//
//	return 0;
//}

void test_string14()
{
	string s1 = "hello";
	string s2 = "hello11";

	string ret1 = s1 + s2;
	cout << ret1 << endl;

	string ret2 = s1 + "xxxxx";
	cout << ret2 << endl;

	string ret3 = "xxxxx" + s1;
	cout << ret3 << endl;

	// 字典序比较
	cout << (s1 < s2) << endl;
}

//int main()
//{
//	test_string14();
//
//	return 0;
//}

//int main()
//{
//	// 默认规定空格或者换行是多个值之间分割
//	string str;
//	//cin >> str;
//	getline(cin, str);
//
//	size_t pos = str.rfind(' ');
//	cout << str.size() - (pos + 1) << endl;
//
//	return 0;
//}

//int main()
//{
//	// 默认规定空格或者换行是多个值之间分割
//	string str;
//	while (cin>>str)
//	{
//		cout << str << endl;
//	}
//
//	return 0;
//}

//int main()
//{
//	// atoi  itoa
//
//	// to_string
//	int x = 0, y = 0;
//	cin >> x>>y;
//	string str = to_string(x + y);
//	cout << str << endl;
//
//	int z = stoi(str);
//
//	return 0;
//}

//int main()
//{
//	char buff1[] = "abcd";
//	char buff2[] = "比特";
//
//	cout << sizeof(buff1) << endl;
//	cout << sizeof(buff2) << endl;
//
//	cout << buff1 << endl;
//	cout << buff2 << endl;
//
//	return 0;
//}

#include "string.h"

namespace bit
{
	void test_string1()
	{
		bit::string s1("hello world");
		cout << s1.c_str() << endl;

		for (size_t i = 0; i < s1.size(); i++)
		{
			s1[i]++;
		}

		for (size_t i = 0; i < s1.size(); i++)
		{
			cout << s1[i] << " ";
		}
		cout << endl;

		// 封装：屏蔽了底层实现细节，提供了一种简单通用访问容器的方式
		// 支付宝和微信的支付
		// 网银
		string::iterator it1 = s1.begin();
		while (it1 != s1.end())
		{
			cout << *it1 << " ";
			++it1;
		}
		cout<<endl;

		for (auto e : s1)
		{
			cout << e << " ";
		}
		cout << endl;

		bit::string s2;
		cout << s2.c_str() << endl;

		const bit::string s3("xxxxxxx");

		string::const_iterator it3 = s3.begin();
		while (it3 != s3.end())
		{
			// *it3 = 'y';

			cout << *it3 << " ";
			++it3;
		}
		cout << endl;

		for (size_t i = 0; i < s3.size(); i++)
		{
			//s3[i]++;
			cout << s3[i] << " ";
		}
		cout << endl;
	}

	void test_string2()
	{
		bit::string s1("hello world");
		cout << s1.c_str() << endl;

		s1.push_back('x');
		cout << s1.c_str() << endl;

		s1.append("yyyyy");
		cout << s1.c_str() << endl;

		s1 += 'z';
		s1 += "密码密密麻麻";
		cout << s1.c_str() << endl;
	}

	void test_string3()
	{
		bit::string s1("hello world");
		cout << s1.c_str() << endl;

		s1.insert(6, 'x');
		cout << s1.c_str() << endl;

		s1.insert(0, 'x');
		cout << s1.c_str() << endl;

		bit::string s2("hello world");
		cout << s2.c_str() << endl;

		s2.insert(6, "yyy");
		cout << s2.c_str() << endl;

		s2.insert(0, "yyy");
		cout << s2.c_str() << endl;

		bit::string s3("hello world");
		cout << s3.c_str() << endl;
		//s3.erase(6, 10);
		s3.erase(6);
		cout << s3.c_str() << endl;

		bit::string s4("hello world");
		cout << s4.c_str() << endl;
		s4.erase(6, 3);
		cout << s4.c_str() << endl;
	}

	void test_string4()
	{
		bit::string s1("hello world");
		cout << s1.find('o') << endl;
		cout << s1.find("wor") << endl;
	}

	void test_string5()
	{
		bit::string s1("hello world");
		bit::string s2(s1);

		s1[0] = 'x';
		cout << s1.c_str() << endl;
		cout << s2.c_str() << endl;

		bit::string s3("yyyy");
		s1 = s3;
		cout << s1.c_str() << endl;
		cout << s3.c_str() << endl;

		bit::string s4("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
		s1 = s4;
		cout << s1.c_str() << endl;
		cout << s4.c_str() << endl;

		s1 = s1;
		cout << s1.c_str() << endl;
		cout << s3.c_str() << endl;

		std::swap(s1, s3);
		cout << s1.c_str() << endl;
		cout << s3.c_str() << endl;

		s1.swap(s3);
		cout << s1.c_str() << endl;
		cout << s3.c_str() << endl;
	}


	void test_string6()
	{
		bit::string url("https://gitee.com/ailiangshilove/cpp-class/blob/master/%E8%AF%BE%E4%BB%B6%E4%BB%A3%E7%A0%81/C++%E8%AF%BE%E4%BB%B6V6/string%E7%9A%84%E6%8E%A5%E5%8F%A3%E6%B5%8B%E8%AF%95%E5%8F%8A%E4%BD%BF%E7%94%A8/TestString.cpp");
		size_t pos1 = url.find(':');
		bit::string url1 = url.substr(0, pos1 - 0);
		cout << url1.c_str() << endl;

		size_t pos2 = url.find('/', pos1 + 3);
		bit::string url2 = url.substr(pos1 + 3, pos2 - (pos1 + 3));
		cout << url2.c_str() << endl;

		bit::string url3 = url.substr(pos2 + 1);
		cout << url3.c_str() << endl;
	}

	void test_string7()
	{
		bit::string s1("hello world");
		cout << s1 << endl;

		cin >> s1;
		cout << s1 << endl;
	}
}

int main()
{
	bit::test_string7();

	return 0;
}