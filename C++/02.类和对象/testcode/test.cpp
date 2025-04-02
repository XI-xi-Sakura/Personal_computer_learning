// //测试this指针能否为空

// // 1.下面程序编译运行结果是？ A、编译报错 B、运行崩溃 C、正常运行

// class A
// {
// public:
// 	 void Print()
// 	 {
// 	 	cout << "Print()" << endl;
// 	 }
// private:
//  	int _a;
// };

// // int main()
// // {
// // 	 A* p = nullptr;
// // 	 p->Print();
// // 	 return 0;
// // }

// // 2.下面程序编译运行结果是？ A、编译报错 B、运行崩溃 C、正常运行
// class A
// {
// public:
//     void PrintA()
//    {
//         cout<<_a<<endl;
//    }
// private:
//  int _a;
// };
// // int main()
// // {
// //     A* p = nullptr;
// //     p->PrintA();
// //     return 0;
// // }

// 默认构造
//  class Time
//  {
//  public:
//  	 Time()
//  	 {
//  		 cout << "Time()" << endl;
//  		 _hour = 0;
//  		 _minute = 0;
//  		 _second = 0;
//  	 }
//  private:
//  	 int _hour;
//  	 int _minute;
//  	 int _second;
//  };
//  class Date
//  {
//  private:
//  	 // 基本类型(内置类型)
//  	 int _year = 1970;
//  	 int _month = 1;
//  	 int _day = 1;

// 	 // 自定义类型
// 	 Time _t;
// };

// class Date
// {
// public:
// 	 Date()
// 	 {
// 		 _year = 1900;
// 		 _month = 1;
// 		 _day = 1;
// 	 }
// 	 Date(int year = 1900, int month = 1, int day = 1)
// 	 {
// 		 _year = year;
// 		 _month = month;
// 		 _day = day;
// 	 }
// private:
// 	 int _year;
// 	 int _month;
// 	 int _day;
// };
// // 以下测试函数能通过编译吗？
// void Test()
// {
//  	//Date d1;
// }

// int main()
// {
//  //Date d;
//  return 0;
// }

// 析构
#include <iostream>
using namespace std;

// class Time
// {
// public:
// 	 ~Time()
// 	 {
// 	 cout << "~Time()" << endl;
// 	 }
// private:
// 	 int _hour;
// 	 int _minute;
// 	 int _second;
// };

// class Date
// {
// private:
// 	 // 基本类型(内置类型)
// 	 int _year = 1970;
// 	 int _month = 1;
// 	 int _day = 1;
// 	 // 自定义类型
// 	 Time _t;
// };

// int main()
// {
//  	Date d;
//  	return 0;
// }

// 拷贝构造
// class Date
// {
// public:
// 	Date(int year, int minute, int day)
// 	{
// 		cout << "Date(int,int,int):" << this << endl;
// 	}

// 	Date(const Date &d)
// 	{
// 		cout << "Date(const Date& d):" << this << endl;
// 	}

// 	~Date()
// 	{
// 		cout << "~Date():" << this << endl;
// 	}

// private:
// 	int _year;
// 	int _month;
// 	int _day;
// };

// Date Test(Date d)
// {
// 	Date temp(d);
// 	return temp;
// }

// class A
// {
// public:
// 	 A() { ++_scount; }
// 	 A(const A& t) { ++_scount; }
// 	 ~A() { --_scount; }
// 	 static int GetACount() { return _scount; }
// private:
//  	static int _scount;
// };

// int A::_scount = 0;

// void TestA()
// {
// 	cout << A::GetACount() << endl;
// 	A a1, a2;
// 	A a3(a1);
// 	cout << A::GetACount() << endl;
// }


class A
{
private:
	 static int k;
	 int h =100;
	 
public:

 class B // B天生就是A的友元
 {
 public:
	 void foo(const A& a)
	 {
		 cout << k << endl;//OK
		 cout << a.h << endl;//OK
	 }
 };
};

int A::k = 1;

int main()
{
 	A::B b;
 	b.foo(A());
 
 	return 0;
}

// int main()
// {
// 	// Date d1(2022, 1, 13);
// 	TestA();
// 	return 0;
// }
