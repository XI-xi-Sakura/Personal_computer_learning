## [priority_queue的介绍](https://legacy.cplusplus.com/reference/queue/priority_queue/)
1. 优先队列是一种容器适配器，根据严格的弱排序标准，它的第一个元素总是它所包含的元素中最大的。

2. 此上下文类似于堆，在堆中可以随时插入元素，并且只能检索最大堆元素(优先队列中位于顶部的元素)。

3. 优先队列被实现为容器适配器，容器适配器即将特定容器类封装作为其底层容器类，`queue`提供一组特定的成员函数来访问其元素。元素从特定容器的“尾部”弹出，其称为优先队列的顶部。

4. 底层容器可以是任何标准容器类模板，也可以是其他特定设计的容器类。容器应该可以通过随机访问迭代器访问，并支持以下操作：
`empty()`：检测容器是否为空
`size()`：返回容器中有效元素个数
`front()`：返回容器中第一个元素的引用
`push_back()`：在容器尾部插入元素
`pop_back()`：删除容器尾部元素


5. 标准容器类`vector`和`deque`满足这些需求。默认情况下，如果没有为特定的priority_queue类实例化指定容器类，则使用vector。

6. 需要支持随机访问迭代器，以便始终在内部保持堆结构。容器适配器通过在需要时自动调用算法函数make_heap、push_heap和pop_heap来自动完成此操作。

## priority_queue的使用
优先级队列默认使用vector作为其底层存储数据的容器，==在vector上又使用了堆算法将vector中元素构造成堆的结构==，因此==priority_queue就是堆==，所有需要用到堆的位置，都可以考虑使用priority_queue。
注意：**默认情况下priority_queue是大堆**。
|函数声明| 接口说明 |
|--|--|
| priority_queue( )  /  priority_queue( first , last )  | 构造优先级队列 
| empty( )|检测优先级队列是否为空，是返回true，否则返回false
|top ( )|返回优先级队列中最大(最小元素)，即堆顶元素
|push(x)|在优先级队列中插入元素x
|pop ( )| 删除优先级队列中最大(最小)元素，即堆顶元素

【注意】
1. 默认情况下，priority_queue是大堆。

```cpp
#include <vector>
#include <queue>
#include <functional> // greater算法的头文件
void TestPriorityQueue()
{
 	// 默认情况下，创建的是大堆，其底层按照小于号比较
 	vector<int> v{3,2,7,6,0,4,1,9,8,5};
 	priority_queue<int> q1;
 	for (auto& e : v)
		 q1.push(e);
 	cout << q1.top() << endl;

 	// 如果要创建小堆，将第三个模板参数换成greater比较方式
	 priority_queue<int, vector<int>, greater<int>> q2(v.begin(), v.end());
 	cout << q2.top() << endl;
}
```
 2. 如果在priority_queue中放自定义类型的数据，用户需要在自定义类型中提供> 或者< 的重载。

```cpp
class Date
{
	public:
		 Date(int year = 1900, int month = 1, int day = 1)
		 : _year(year)
		 , _month(month)
		 , _day(day)
		 {}
		 
		 bool operator<(const Date& d)const
		 {
			 return (_year < d._year) ||
			 		(_year == d._year && _month < d._month) ||
			 		(_year == d._year && _month == d._month && _day < d._day);
		 }
		 
		 bool operator>(const Date& d)const
		 {
			 return (_year > d._year) ||
			 		(_year == d._year && _month > d._month) ||
			 		(_year == d._year && _month == d._month && _day > d._day);
		 }
		 
		 friend ostream& operator<<(ostream& _cout, const Date& d)
		 {
			 _cout << d._year << "-" << d._month << "-" << d._day;
			 return _cout;
		 }
	private:
		 int _year;
		 int _month;
		 int _day;
};
```

```cpp
void TestPriorityQueue()
{
 // 大堆，需要用户在自定义类型中提供<的重载
 priority_queue<Date> q1;
 q1.push(Date(2018, 10, 29));
 q1.push(Date(2018, 10, 28));
 q1.push(Date(2018, 10, 30));
 cout << q1.top() << endl;
 
 // 如果要创建小堆，需要用户提供>的重载
 priority_queue<Date, vector<Date>, greater<Date>> q2;
 q2.push(Date(2018, 10, 29));
 q2.push(Date(2018, 10, 28));
 q2.push(Date(2018, 10, 30));
 cout << q2.top() << endl;
}
```

数组中的第k个最大元素
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/e2fb0b0e57ca4089bc0bbc5faae325ed.png)

```cpp
class Solution {
public:
 int findKthLargest(vector<int>& nums, int k) {
 // 将数组中的元素先放入优先级队列中
 priority_queue<int> p(nums.begin(), nums.end());
 
 // 将优先级队列中前k-1个元素删除掉
 for(int i= 0; i < k-1; ++i)
 {
    p.pop();
 }
 
 return p.top();
 }
};
```

