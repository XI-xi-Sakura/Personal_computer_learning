#include <iostream>
using namespace std;


class A
{
public:
	A(int a = 0)
	: _a(a)
	{
		cout << "A():" << this << endl;
	}
	
	~A()
	{
		cout << "~A():" << this << endl;
	}
private:
	int _a;
};

int main()
{

    A* p1 = (A*)malloc(sizeof(A));
    cout<<p1<<endl;
    free(p1);

    A* p2 = new A(1);
    cout<<p2<<endl;
    delete p2;


// 内置类型是几乎是一样的
    int* p3 = (int*)malloc(sizeof(int)); // C
    free(p3);

    int* p4 = new int;
    delete p4;

    A* p5 = (A*)malloc(sizeof(A)*10);
    free(p5);

    A* p6 = new A[10];
    delete[] p6;


    return 0;
}