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
class Date
{
public:
	Date(int year, int minute, int day)
	{
		cout << "Date(int,int,int):" << this << endl;
	}

	Date(const Date &d)
	{
		cout << "Date(const Date& d):" << this << endl;
	}

	~Date()
	{
		cout << "~Date():" << this << endl;
	}

private:
	int _year;
	int _month;
	int _day;
};

Date Test(Date d)
{
	Date temp(d);
	return temp;
}

int main()
{
	Date d1(2022, 1, 13);
	Test(d1);
	return 0;
}
