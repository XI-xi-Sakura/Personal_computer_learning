#include<iostream>
using namespace std;

//class Animal
//{
//public:
//	virtual void sound() const = 0;
//};
//
//class Cat : public Animal
//{
//public:
//	virtual void sound() const
//	{
//		cout << "喵喵" << endl;
//	}
//};
//
//class Dog : public Animal
//{
//public:
//	virtual void sound() const
//	{
//		cout << "汪汪" << endl;
//	}
//};
//
//void AnimalSound(const Animal& anm)
//{
//	anm.sound();
//}
//
//int main()
//{
//	AnimalSound(Cat());
//	AnimalSound(Dog());
//
//	return 0;
//}

class Base
{
public:
	virtual void Func1()
	{
		cout << "Base::Func1()" << endl;
	}

	virtual void Func2()
	{
		cout << "Base::Func2()" << endl;
	}

	void Func3()
	{
		cout << "Base::Func3()" << endl;
	}
private:
	int _b = 1;
};

class Derive : public Base
{
public:
	virtual void Func1()
	{
		cout << "Derive::Func1()" << endl;
	}
private:
	int _d = 2;
};

void Func1(Base* p)
{
	// 运行时绑定/动态绑定
	p->Func1();

	// 编译时绑定/静态绑定
	p->Func3();
}

//int main()
//{
//	Base b;
//	Derive d;
//	
//	Func1(&b);
//	Func1(&d);
//
//	return 0;
//}

//int main()
//{
//	const char* str = "11111111";
//	cout << str << endl;
//
//	Base b;
//
//	printf("%p\n", str);
//	printf("%p\n", &Base::Func1);
//	printf("%p\n", &Base::Func2);
//	printf("%p\n", &Base::Func3);
//
//	int a = 0;
//	printf("%p\n", &a);
//
//	return 0;
//}

//int main()
//{
//	int i = 0;
//	static int j = 1;
//	int* p1 = new int;
//	const char* p2 = "xxxxxxxx";
//	printf("栈:%p\n", &i);
//	printf("静态区:%p\n", &j);
//	printf("堆:%p\n", p1);
//	printf("常量区:%p\n", p2);
//
//	Base b;
//	Derive d;
//
//	// int/double/char
//	// int和int*  指针之间
//	printf("Base虚表地址:%p\n", *((int*)&b));
//	printf("Derive虚表地址:%p\n", *((int*)&d));
//	printf("Base对象地址:%p\n", &b);
//	printf("Derive对象地址:%p\n", &d);
//
//	Base b1;
//	Base b2;
//	Base b3;
//
//	return 0;
//}

//int main()
//{
//	int i = 0; 
//	double d = 1.1;
//
//	cout << i;
//	cout << d;
//
//	int a = 1, b = 2;
//	char x = 'x', y = 'y';
//	swap(a, b);
//	swap(x, y);
//}

class Base1 { public: int _b1; };
class Base2 { public: int _b2; };

class Derive : public Base1, public Base2 { public: int _d; };
int main() {
	Derive d;
	Base1* p1 = &d;
	Base2* p2 = &d;
	Derive* p3 = &d;

	return 0;
}