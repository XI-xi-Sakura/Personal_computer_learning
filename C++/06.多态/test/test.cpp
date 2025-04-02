#include<iostream>
using namespace std;

//class Student;
//class Person
//{
//public:
//	friend void Display(const Person& p, const Student& s);
//protected:
//	string _name; // 姓名
//};
//
//class Student : public Person
//{
//public:
//	friend void Display(const Person& p, const Student& s);
//protected:
//	int _stuNum; // 学号
//};
//
//void Display(const Person& p, const Student& s)
//{
//	cout << p._name << endl;
//	cout << s._stuNum << endl;
//}
//
//int main()
//{
//
//	return 0;
//}

//class Person
//{
//public:
//	Person() { ++_count; }
//protected:
//	string _name; // 姓名
//public:
//	static int _count; // 统计人的个数。
//};
//int Person::_count = 0;
//
//class Student : public Person
//{
//protected:
//	int _stuNum; // 学号
//};
//
//int main()
//{
//	Person p;
//	Student s;
//
//	cout << &Person::_count << endl;
//	cout << &Student::_count << endl;
//
//	return 0;
//}

//class Person
//{
//public:
//	string _name; // 姓名
//	int _id;
//	int _tel;
//	int _adress;
//};
//
//class Student : virtual public Person
//{
//protected:
//	int _num; //学号
//};
//
//class Teacher : virtual public Person
//{
//protected:
//	int _id; // 职工编号
//};
//
//class Assistant : public Student, public Teacher
//{
//protected:
//	string _majorCourse; // 主修课程
//};
//
//int main()
//{
//	// 数据冗余和二义性
//	Assistant a;
//	a.Student::_name = "小李";
//	a.Teacher::_name = "李老师";
//	a._name = "李益达";
//
//	cout << a._name << endl;
//
//	return 0;
//}

//class Tire {
//protected:
//	string _brand = "Michelin"; // 品牌
//	size_t _size = 17; // 尺寸
//};
//
//// 组合
//class Car {
//
//
//protected:
//	string _colour = "白色"; // 颜色
//	string _num = "陕ABIT00"; // 车牌号
//
//	Tire _t; // 轮胎
//};

//class A {};
//class B : public A {};
//
//class Person {
//public:
//	virtual A* BuyTicket() { 
//		cout << "买票-全价" << endl; 
//		return 0;
//	}
//};
//
//class Student : public Person {
//public:
//	// 重写/覆盖
//	virtual B* BuyTicket() { 
//		cout << "买票-半价" << endl;
//		return 0;
//	}
//};
//
//class Soldier : public Person {
//public:
//	// 重写/覆盖
//	virtual B* BuyTicket() 
//	{ 
//		cout << "买票-优先" << endl;
//		return 0;
//	}
//};
//
//// 多态条件：
//// 1、虚函数重写
//// 2、父类指针或者引用调用虚函数
//void Func(Person& p)
//{
//	p.BuyTicket();
//}
//
//int main()
//{
//	Person p;
//	Student st;
//	Soldier so;
//
//	//Func(&p);
//	Func(st);
//	Func(so);
//
//	// 隐藏
//	//st.BuyTicket();
//	//st.Person::BuyTicket();
//
//	return 0;
//}

//class Person {
//public:
//	virtual ~Person() 
//	{ 
//		cout << "~Person()" << endl;
//	}
//
//};
//
//class Student : public Person {
//public:
//	~Student() override
//	{
//		delete _ptr;
//		cout << "~Student():"<< _ptr<< endl; 
//	}
//protected:
//	int* _ptr = new int[10];
//};
//
//int main()
//{
//	//Student st;
//
//	Person* p1 = new Student;
//
//	// 实现多态，就正常了
//	// 
//	// p1->destructor() + operator delete(p1)
//	delete p1;
//
//	Person* p2 = new Person;
//	delete p2;
//
//	
//
//	return 0;
//}

//class A final
//class A
//{
//public:
//	static A CreateObj()
//	{
//		return A();
//	}
//
//private:
//	A()
//	{}
//};
//
//class B : public A
//{};
//
//int main()
//{
//	//B bb;
//	A::CreateObj();
//
//	return 0;
//}

//class Car
//{
//public:
//	virtual void Drive() = 0;
//};
//
//class Benz :public Car
//{
//public:
//	virtual void Drive()
//	{
//		cout << "BMW-舒适" << endl;
//	}
//
//	//virtual void func() = 0;
//};
//
//class BMW :public Car
//{
//public:
//	virtual void Drive()
//	{
//		cout << "BMW-操控" << endl;
//	}
//};
//
//int main()
//{
//	Benz bz;
//
//	return 0;
//}

//class A
//{
//public:
//	virtual void func(int val = 1) { std::cout << "A->" << val << std::endl; }
//	virtual void test() { func(); }
//};
//
//class B : public A
//{
//private:
////public:
//	void func(int val = 0) { std::cout << "B->" << val << std::endl; }
//};
//
//int main(int argc, char* argv[])
//{
//	A* p = new B;
//	//p->test();
//	p->func();
//
// //	return 0;
// //}
// class Base
// {
// public:
// 	virtual void Func1()
// 	{
// 		cout << "Func1()" << endl;
// 	}

// 	virtual void Func2()
// 	{
// 		cout << "Func1()" << endl;
// 	}

// 	void Func3()
// 	{
// 		cout << "Func1()" << endl;
// 	}
// private:
// 	int _b = 1;
// 	char _ch = 'x';
// };

// int main()
// {
// 	cout << sizeof(Base) << endl;
// 	Base b;

// 	return 0;
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
//     virtual void BuyTicket() { cout << "买票 - 全价" << endl; }
// };
// class Student : public Person
// {
// public:
//     virtual void BuyTicket() { cout << "买票 - 打折" << endl; }
// };
// void Func(Person* ptr)
// {
//     // 这里可以看到虽然都是Person指针Ptr在调用BuyTicket
//     // 但是跟ptr没关系，而是由ptr指向的对象决定的。
//     ptr->BuyTicket();
// }
// int main()
// {
//     Person ps;
//     Student st;
//     Func(&ps);
//     Func(&st);
//     return 0;
// }
// class Animal
// {
// public:
//     virtual void talk() const {}
// };
// class Dog : public Animal
// {
// public:
//     virtual void talk() const
//     {
//         std::cout << "汪汪" << std::endl;
//     }
// };
// class Cat : public Animal
// {
// public:
//     virtual void talk() const
//     {
//         std::cout << "(>^ω^<)喵" << std::endl;
//     }
// };
// void letsHear(const Animal& animal)
// {
//     animal.talk();
// }
// int main()
// {
//     Cat cat;
//     Dog dog;
//     letsHear(cat);
//     letsHear(dog);
//     return 0;
// }


// class A
// {
// public:
//     virtual void func(int val = 1){ std::cout<<"A->"<< val <<std::endl;}
//     virtual void test(){ func();}
// };
// class B : public A
// {
// public:
//     void func(int val = 0){ std::cout<<"B->"<< val <<std::endl; }
// };
// int main(int argc,char* argv[])
// {
//     B*p = new B;
//     p->test();
//     return 0;
// }


class A
{
public:
    virtual ~A()
    {
        cout << "~A()" << endl;
    }
};
class B : public A {
public:
    B()
    {
        cout << "B()" << endl;
    }
    ~B()
    {
        cout << "~B()->delete:"<<_p<< endl;
        delete _p;
    }
protected:
    int* _p = new int[10];
};
// 只有派生类Student的析构函数重写了Person的析构函数，
//下面的delete对象调用析构函数，才能构成多态，才能保证p1和p2指向的对象正确的调用析构函数。
int main()
{
    A* p1 = new A;
    A* p2 = new B;
    delete p1;
    delete p2;
    //如果基类的析构函数不是虚函数，那么在通过基类指针删除派生类对象时，
    //只会调用基类的析构函数，而不会调用派生类的析构函数。
    return 0;
}