#include <iostream>
using namespace std;

// class Person
//{
// public:
//	Person()
//	{}
//
//	void Print()
//	{
//		cout << "name:" << _name << endl;
//		cout << "age:" << _age << endl;
//		//cout << _tel << endl;
//	}
//
//
//	string _name = "peter"; // 姓名
// protected:
//	int _age = 18; // 年龄
//	//...
////private:
//	// 父类定义本质，不想被子类继承
//	//int _tel = 110;
//
//};
//
//// 继承的父类的成员
// class Student : public Person
//{
// public:
//	void func()
//	{
//		// 子类用不了（不可见）
//		//cout << _tel << endl;
//
//		// 子类可以用
//		cout << _name << endl;
//		cout << _age << endl;
//
//	}
// protected:
//	int _stuid; // 学号
// };
//
// class Teacher : public Person
//{
// protected:
//	int _jobid; // 工号
// };

// int main()
//{
//	Person p;
//	p.Print();
//
//	Student s;
//	s.Print();
//
//	//s._name = "张三";
//
//	Teacher t;
//	//t._name = "张老师";
//
//	return 0;
// }

// int main()
//{
//	Student s;
//	Person p;
//
//	// 跟下面机制不一样
//	// 特殊语法规则：不是类型转换，中间没有产生临时变量
//	p = s;
//	Person* ptr = &s;
//	Person& ref = s;
//	ptr->_name += 'x';
//	ref._name += 'y';
//	s._name += 'z';
//
//	int i = 1234;
//	printf("%x\n", i);
//
//
//	// 类型转换
//	// 截断
//	char ch = i;
//	printf("%x\n", ch);
//	// 提升
//	i = ch;
//	printf("%x\n", i);
//
//	const char& refch = i;
//
//	return 0;
// }

/////////////////////////////////////////////////////////////////
// Student的_num和Person的_num构成隐藏关系，可以看出这样代码虽然能跑，但是非常容易混淆
// class Person
//{
// protected:
//	string _name = "小李子"; // 姓名
//	int _num = 111; // 身份证号
//};
//
// class Student : public Person
//{
// public:
//	void Print()
//	{
//		cout << " 姓名:" << _name << endl;
//		cout << " 学号:" << _num << endl;   // 隐藏/重定义
//		cout << " 学号:" << Person::_num << endl;
//	}
// protected:
//	int _num = 999; // 学号
//};
//
// class A
//{
// public:
//	void fun()
//	{
//		cout << "func()" << endl;
//	}
//};
//
// class B : public A
//{
// public:
//	void fun(int i)
//	{
//		cout << "func(int i)->" << i << endl;
//	}
//};

// 下面哪个是正确的（）
// fun构成重载
// fun构成隐藏
// fun构成重写
// 编译报错
// 运行报错

// int main()
//{
//	B bb;
//	bb.fun(0);
//	bb.A::fun();
//
//	return 0;
// }

// int main()
//{
//	return 0;
// }

///////////////////////////////////////////////////////////////////////
// class Person
// {
// public:
// 	// Person(const char* name = "")
// 	Person(const char *name = "")
// 		: _name(name)
// 	{
// 		cout << "Person()" << endl;
// 	}

// 	Person(const Person &p)
// 		: _name(p._name)
// 	{
// 		cout << "Person(const Person& p)" << endl;
// 	}

// 	Person &operator=(const Person &p)
// 	{
// 		cout << "Person operator=(const Person& p)" << endl;
// 		if (this != &p)
// 			_name = p._name;
// 		return *this;
// 	}

// 	~Person()
// 	{
// 		cout << "~Person()" << endl;
// 		delete[] _str;
// 	}

// protected:
// 	string _name; // 姓名

// 	char *_str = new char[10]{'x', 'y', 'z'};
// };

// class Student : public Person
// {
// public:
// 	// 父类构造显示调用，可以保证先父后子
// 	Student(const char *name = "", int x = 0, const char *address = "")
// 		: _x(x), _address(address), _name(Person::_name + 'x'), Person(name)
// 	{
// 	}

// 	Student(const Student &st)
// 		: Person(st), _x(st._x), _address(st._address)
// 	{
// 	}

// 	Student &operator=(const Student &st)
// 	{
// 		if (this != &st)
// 		{
// 			Person::operator=(st);
// 			_x = st._x;
// 			_address = st._address;
// 		}

// 		return *this;
// 	}

// 	// 由于多态，析构函数的名字会被统一处理成destructor()

// 	// 父类析构不能显示调用，因为显示调用不能保证先子后父
// 	~Student()
// 	{
// 		// 析构函数会构成隐藏，所以这里要指定类域
// 		// Person::~Person();

// 		cout << "~Student()" << endl;
// 		// delete [] _ptr;
// 		cout << _str << endl;
// 	}

// protected:
// 	int _x = 1;
// 	string _address = "西安高新区";
// 	string _name;

// 	// int* _ptr = new int[10];
// };

// // 子类默认生成的构造
// // 父类成员（整体） -- 默认构造
// // 子类自己的内置成员 -- 一般不处理
// // 子类自己的自定义成员 -- 默认构造

// // 子类默认生成的拷贝构造  赋值重载跟拷贝构造类似
// // 父类成员（整体） -- 调用父类的拷贝构造
// // 子类自己的内置成员 -- 值拷贝
// // 子类自己的自定义成员 -- 调用他的拷贝构造
// // 一般就不需要自己写了，子类成员涉及深拷贝，就必须自己实现

// // 子类默认生成的析构
// // 父类成员（整体） -- 调用父类的析构
// // 子类自己的内置成员 -- 不处理
// // 子类自己的自定义成员 -- 调用析构
// // //

// // // class A
// // // {
// // // public:
// // // 	void fun()
// // // 	{
// // // 		cout << "func()" << endl;
// // // 	}
// // // };

// // // class B : public A
// // // {
// // // public:
// // // 	void fun(int i)
// // // 	{
// // // 		cout << "func(int i)" << i << endl;
// // // 	}
// // // };

// // // int main()
// // // {
// // //     B b;
// // //     b.fun(10);
// // //     b.fun();

// // //     return 0;
// // // };
# include <iostream>
using namespace std;
// class Person
// {
// public:
//     Person(const char* name = "peter")
//         : _name(name)
//     {
//         cout << "Person()" << endl;
//     }

//     Person(const Person& p)
//         : _name(p._name)
//     {
//         cout << "Person(const Person& p)" << endl;
//     }

//     Person& operator=(const Person& p)
//     {
//         cout << "Person operator=(const Person& p)" << endl;
//         if (this != &p)
//         {
//             _name = p._name;
//         }
//         return *this;
//     }

//     ~Person()
//     {
//         cout << "~Person()" << endl;
//     }
// protected:
//     string _name; // 姓名
// };

// class Student : public Person
// {
// public:
//     Student(const char* name, int num)
//         : Person(name)
//         , _num(num)
//     {
//         cout << "Student()" << endl;
//     }

// //     Student(const Student& s)
// //         : Person(s)
// //         , _num(s._num)
// //     {
// //         cout << "Student(const Student& s)" << endl;
// //     }

// //     Student& operator=(const Student& s)
// //     {
// //         cout << "Student& operator= (const Student& s)" << endl;
// //         if (this != &s)
// //         {
// //             // 构成隐藏，所以需要显示调用
// //             Person::operator=(s);
// //             _num = s._num;
// //         }
// //         return *this;
// //     }

// //     ~Student()
// //     {
// //         cout << "~Student()" << endl;
// //     }
// // protected:
// //     int _num; // 学号
// // };

// // int main()
// // {
// //     Student s1("jack", 18);
// //     Student s2(s1);
// //     Student s3("rose", 17);
// //     s1 = s3;

// //     return 0;
// // }



// class Person
// {
// public:
//     string _name;
//     static int _count;
// };

// int Person::_count = 0;

// class Student : public Person
// {
// protected:
//     int _stuNum;
// };



// int main()
// {
//     Person p;
//     Student s;

//     // 这里的运行结果可以看到非静态成员_name的地址是不一样的
//     // 说明派生类继承下来了，父派生类对象各有一份
//     cout << &p._name << endl;
//     cout << &s._name << endl;

//     // 这里的运行结果可以看到静态成员_count的地址是一样的
//     // 说明派生类和基类共用同一份静态成员
//     cout << &p._count << endl;
//     cout << &s._count << endl;

// 	cout << Person::_count << endl;
//     cout << Student::_count << endl;

// 	Person::_count++;
// 	Student::_count++;
// 	Student::_count++;

//     // 公有的情况下，父派生类指定类域都可以访问静态成员
//     cout << Person::_count << endl;
//     cout << Student::_count << endl;

//     return 0;
// }

// class Base1 { public: int _b1; };
// class Base2 { public: int _b2; };
// class Derive : public Base1, public Base2 { public: int _d; };
// int main()
// {
//     Derive d;
//     Base1* p1 = &d;
//     Base2* p2 = &d;
//     Derive* p3 = &d;
//     cout<< p1 << endl;
//     cout<< p2 << endl;
//     cout<< p3 << endl;  
//     return 0;
// }


// class Person
// {
// public:
//     Person(const char* name = "peter")
//         : _name(name)
//     {
//         cout << "Person()" << endl;
//     }

//     Person(const Person& p)
//         : _name(p._name)
//     {
//         cout << "Person(const Person& p)" << endl;
//     }

//     Person& operator=(const Person& p)
//     {
//         cout << "Person operator=(const Person& p)" << endl;
//         if (this != &p)
//         {
//             _name = p._name;
//         }
//         return *this;
//     }

//     ~Person()
//     {
//         cout << "~Person()" << endl;
//     }
// protected:
//     string _name; // 姓名
// };

// class Student : public Person
// {
// public:
//     Student(const char* name, int num)
//         : Person(name)
//         , _num(num)
//     {
//         cout << "Student()" << endl;
//     }

//     Student(const Student& s)
//         : Person(s)
//         , _num(s._num)
//     {
//         cout << "Student(const Student& s)" << endl;
//     }

//     Student& operator=(const Student& s)
//     {
//         cout << "Student& operator= (const Student& s)" << endl;
//         if (this != &s)
//         {
//             // 构成隐藏，所以需要显示调用
//             Person::operator=(s);
//             _num = s._num;
//         }
//         return *this;
//     }

//     ~Student()
//     {
//         cout << "~Student()" << endl;
//     }
// protected:
//     int _num; // 学号
// };

// int main()
// {
//     Student s1("jack", 18);
//     Student s2(s1);
//     Student s3("rose", 17);
//     s1 = s3;

//     return 0;
// }


// class A
// {
// public:
//     void fun()
//     {
//         cout << "func()" << endl;
//     }
// };

// class B : public A
// {
// public:
//     void fun(int i)
//     {
//         cout << "func(int i)" << i << endl;
//     }

//     void pr ()
//     {
//         cout<< "pr()" << endl;
//     }
// };

// int main()
// {
//     A* a=new B;

//     a->pr();

//     B b;
//     b.fun(10);
    

//     return 0;
// };