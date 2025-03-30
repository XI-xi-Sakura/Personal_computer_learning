#include<string>
#include<iostream>
using namespace std;

#include<set>

//int main()
//{
//	// 去重+排序
//	set<int> s;
//	s.insert(5);
//	s.insert(2);
//	s.insert(7);
//	s.insert(4);
//	s.insert(9);
//	s.insert(9);
//	s.insert(9);
//	s.insert(1);
//	s.insert(5);
//	s.insert(9);
//
//	//set<int>::iterator it = s.begin();
//	auto it = s.begin();
//	while (it != s.end())
//	{
//		cout << *it << " ";
//		++it;
//	}
//	cout << endl;
//
//	for (auto e : s)
//	{
//		cout << e << " ";
//	}
//	cout << endl;
//
//	keyValue::BSTree<string, string> dict;
//	dict.Insert("left", "左边");
//	dict.Insert("right", "右边");
//	dict.Insert("insert", "插入");
//	dict.Insert("string", "字符串");
//
//	keyValue::BSTree<string, string> copy(dict);
//
//
//	return 0;
//}

//int main()
//{
//	// 去重+排序
//	set<int> s;
//	s.insert(5);
//	s.insert(2);
//	s.insert(7);
//	s.insert(4);
//	s.insert(9);
//	s.insert(9);
//	s.insert(9);
//	s.insert(1);
//	s.insert(5);
//	s.insert(9);
//
//	//set<int>::iterator it = s.begin();
//	auto it = s.begin();
//	while (it != s.end())
//	{
//		cout << *it << " ";
//		++it;
//	}
//	cout << endl;
//
//	// 删除最小值
//	s.erase(s.begin());
//	int x;
//	cin >> x;
//	/*int num = s.erase(x);
//	if (num == 0)
//	{
//		cout << x << "不存在！" << endl;
//	}*/
//
//	auto pos = s.find(x);
//	if (pos != s.end())
//	{
//		s.erase(pos);
//	}
//	else
//	{
//		cout << x << "不存在！" << endl;
//	}
//
//	for (auto e : s)
//	{
//		cout << e << " ";
//	}
//	cout << endl;
//
//	auto pos1 = find(s.begin(), s.end(), x);  // O(N)
//	auto pos2 = s.find(x);                    // O(logN)
//
//	cin >> x;
//	if (s.count(x))                           // O(log(N))
//	{
//		cout << x << "在！" << endl;
//	}
//	else
//	{
//		cout << x << "不存在！" << endl;
//	}
//
//	return 0;
//}

//int main()
//{
//    std::set<int> myset;
//    std::set<int>::iterator itlow, itup;
//
//    for (int i = 1; i < 10; i++) myset.insert(i * 10); // 10 20 30 40 50 60 70 80 90
//
//    // [30, 60]
//    // >= 30
//    itlow = myset.lower_bound(30);                //
//    // > 60
//    itup = myset.upper_bound(60);                 //               
//    myset.erase(itlow, itup);                     // 10 20 70 80 90
//
//    std::cout << "myset contains:";
//    for (std::set<int>::iterator it = myset.begin(); it != myset.end(); ++it)
//        std::cout << ' ' << *it;
//    std::cout << '\n';
//
//    return 0;
//}

//int main()
//{
//	// 排序
//	multiset<int> s;
//	s.insert(5);
//	s.insert(2);
//	s.insert(7);
//	s.insert(4);
//	s.insert(9);
//	s.insert(9);
//	s.insert(9);
//	s.insert(1);
//	s.insert(5);
//	s.insert(9);
//
//	auto it = s.begin();
//	while (it != s.end())
//	{
//		cout << *it << " ";
//		++it;
//	}
//	cout << endl;
//
//	// 11:50
//	int x;
//	cin >> x;
//	auto pos = s.find(x);
//	while (pos != s.end() && *pos == x)
//	{
//		cout << *pos << " ";
//		++pos;
//	}
//	cout << endl;
//
//	cout << s.count(x) << endl;
//
//	s.erase(x);
//	for (auto e : s)
//	{
//		cout << e << " ";
//	}
//	cout << endl;
//
//	return 0;
//}

