using namespace std;

#include"vector.h"


//template<class T>
//class vector
//{
//private:
//	T* _a;
//	size_t _size;
//	size_t _capacity;
//};

//vector<int>
//class vector
//{
//public:
//	int& operator[](size_t i)
//	{
//		//....
//		return _a[i];
//	}
//private:
//	int* _a;
//	size_t _size;
//	size_t _capacity;
//};
//
////vector<vector<int>>
//class vector
//{
//public:
//	vector<int>& operator[](size_t i)
//	{
//		//....
//		return _a[i];
//	}
//private:
//	vector<int>* _a;
//	size_t _size;
//	size_t _capacity;
//};

void test_vector1()
{
	vector<double> v2;

	// string
	// vector<char>

	vector<int> v1;
	v1.push_back(1);
	v1.push_back(2);
	v1.push_back(3);
	v1.push_back(4);

	for (size_t i = 0; i < v1.size(); i++)
	{
		cout << v1[i] << " ";
	}
	cout << endl;

	// 大众 奥迪 保时捷
	vector<int>::iterator it1 = v1.begin();
	while (it1 != v1.end())
	{
		cout << *it1 << " ";
		++it1;
	}
	cout << endl;

	for (auto e : v1)
	{
		cout << e << " ";
	}
	cout << endl;
}

//void push_back(const string& s)
//{}

void test_vector2()
{
	vector<string> v2;

	string s1("张三");
	v2.push_back(s1);
	v2.push_back(string("李四"));
	v2.push_back("王五");

	v2[1] += "来";

	for (const auto& e : v2)
	{
		cout << e << " ";
	}
	cout << endl;
}

void test_vector3()
{
	vector<int> v1;
	v1.push_back(10);
	v1.push_back(2);
	v1.push_back(30);
	v1.push_back(4);
	v1.push_back(44);
	v1.push_back(4);
	v1.push_back(40);
	v1.push_back(4);


	/*greater<int> gt;
	cout << gt(2, 3) << endl;
	cout << gt.operator()(2, 3) << endl;
	cout << gt(3, 2) << endl;*/

	//sort(v1.begin(), v1.end(), gt);
	//sort(v1.begin()+1, v1.end()-1);
	//sort(v1.begin(), v1.begin() + v1.size() / 2);

	// 默认是升序
	// 降序
	sort(v1.begin(), v1.end(), greater<int>());
	for (const auto & e : v1)
	{
		cout << e << " ";
	}
	cout << endl;
}

void test_list1()
{
	list<int> lt1 = { 10,2,3,3,4,3,5,6};
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

	//sort(lt1.begin(), lt1.end());
	lt1.sort();
	for (auto e : lt1)
	{
		cout << e << " ";
	}
	cout << endl;

	lt1.sort(greater<int>());
	for (auto e : lt1)
	{
		cout << e << " ";
	}
	cout << endl;

	lt1.unique();
	for (auto e : lt1)
	{
		cout << e << " ";
	}
	cout << endl;
}

void test_op1()
{
	srand(time(0));
	const int N = 10000000;

	list<int> lt1;
	list<int> lt2;

	vector<int> v;

	for (int i = 0; i < N; ++i)
	{
		auto e = rand() + i;
		lt1.push_back(e);
		v.push_back(e);
	}

	int begin1 = clock();
	// 排序
	sort(v.begin(), v.end());
	int end1 = clock();

	int begin2 = clock();
	lt1.sort();
	int end2 = clock();

	printf("vector sort:%d\n", end1 - begin1);
	printf("list sort:%d\n", end2 - begin2);
}

void test_op2()
{
	srand(time(0));
	const int N = 10000000;

	list<int> lt1;
	list<int> lt2;

	for (int i = 0; i < N; ++i)
	{
		auto e = rand()+i;
		lt1.push_back(e);
		lt2.push_back(e);
	}

	int begin1 = clock();
	// 拷贝vector

	vector<int> v(lt2.begin(), lt2.end());
	// 排序
	sort(v.begin(), v.end());

	// 拷贝回lt2
	lt2.assign(v.begin(), v.end());

	int end1 = clock();

	int begin2 = clock();
	lt1.sort();
	int end2 = clock();

	printf("list copy vector sort copy list sort:%d\n", end1 - begin1);
	printf("list sort:%d\n", end2 - begin2);
}

int main()
{
	//test_vector3();
	//bit::test_vector9();
	//test_list1();
	test_op2();

	return 0;
}