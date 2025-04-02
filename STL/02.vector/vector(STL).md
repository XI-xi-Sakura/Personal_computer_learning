## [vector的介绍](https://legacy.cplusplus.com/reference/vector/vector/)
1. `vector`是表示可变大小数组的序列容器。
2. 就像数组一样，`vector`也采用连续存储空间来存储元素。也就意味着可以采用下标对vector的元素进行访问，和数组一样高效。但是又不像数组，它的大小是可以动态改变的，而且它的大小会被容器自动处理。
3. 本质上，`vector`使用动态分配数组来存储它的元素。当新元素插入时候，这个数组需要被重新分配大小，为了增加存储空间。其做法是，分配一个新的数组，然后将全部元素移到这个数组。就时间而言，这是一个相对代价高的任务，因为每当一个新的元素加入到容器的时候，`vector`并不会每次都重新分配大小。
4. `vector`分配空间策略：`vector`会分配一些额外的空间以适应可能的增长，因为存储空间比实际需要的存储空间更大。不同的库采用不同的策略权衡空间的使用和重新分配。但是无论如何，重新分配都应该是对数增长的间隔大小，以至于在末尾插入一个元素的时候是在常数时间的复杂度完成的。
5. 因此，`vector`占用了更多的存储空间，为了获得管理存储空间的能力，并且以一种有效的方式动态增长。
6. 与其它动态序列容器相比（`deque`, `list` and `forward_list`）， `vector`在访问元素的时候更加高效，在末尾添加和删除元素相对高效。对于其它不在末尾的删除和插入操作，效率更低。比起`list`和`forward_list`统一的迭代器和引用更好。
## [vector构造](https://legacy.cplusplus.com/reference/vector/vector/vector/)
| 函数声明 | 功能 |
|--|--|
| vector() |  无参构造
|vector（size_type n, const value_type& val = value_type()）|构造并初始化n个val
|vector (InputIterator first, InputIterator last);|使用迭代器进行初始化构造
|vector (const vector& x);|拷贝构造|

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/c7d3cf939c224b848e346b98e67423dc.png)

### vector代码构造演示

```cpp
////////////////////////////////////////////////////////////////////
//    vector的构造
////////////////////////////////////////////////////////////////////
int TestVector1()
{
    // constructors used in the same order as described above:
    vector<int> first;                                // empty vector of ints
    vector<int> second(4, 100);                       // four ints with value 100
    vector<int> third(second.begin(), second.end());  // iterating through second
    vector<int> fourth(third);                       // a copy of third

    // 下面涉及迭代器初始化的部分，我们学习完迭代器再来看这部分
    // the iterator constructor can also be used to construct from arrays:
    int myints[] = { 16,2,77,29 };
    vector<int> fifth(myints, myints + sizeof(myints) / sizeof(int));

    cout << "The contents of fifth are:";
    for (vector<int>::iterator it = fifth.begin(); it != fifth.end(); ++it)
        cout << ' ' << *it;
    cout << '\n';

    return 0;
}
```
## vector iterator 
|iterator使用|接口说明  |
|--|--|
|  begin| 获取第一个数据位置的iterator/const_iterator
|end|获取最后一个数据的下一个位置的iterator/const_iterator
|rbegin|获取最后一个数据位置的reverse_iterator
|rend|获取第一个数据前一个位置的reverse_iterator


![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/534c040eb8c54593aac4ed00e24f3474.jpeg)

### vector迭代器使用演示

```cpp
////////////////////////////////////////////////////////////////////////
//  vector的迭代器
////////////////////////////////////////////////////////////////////////
void PrintVector(const vector<int>& v)
{
	// const对象使用const迭代器进行遍历打印
	vector<int>::const_iterator it = v.begin();
	while (it != v.end())
	{
		cout << *it << " ";
		++it;
	}
	cout << endl;
}

void TestVector2()
{
	// 使用push_back插入4个数据
	vector<int> v;
	v.push_back(1);
	v.push_back(2);
	v.push_back(3);
	v.push_back(4);

	// 使用迭代器进行遍历打印
	vector<int>::iterator it = v.begin();
	while (it != v.end())
	{
		cout << *it << " ";
		++it;
	}
	cout << endl;

	// 使用迭代器进行修改
	it = v.begin();
	while (it != v.end())
	{
		*it *= 2;
		++it;
	}

	// 使用反向迭代器进行遍历再打印
	// vector<int>::reverse_iterator rit = v.rbegin();
	auto rit = v.rbegin();
	while (rit != v.rend())
	{
		cout << *rit << " ";
		++rit;
	}
	cout << endl;

	PrintVector(v);
}
```
## vector 空间
|函数名称| 接口说明 |
|--|--|
| size| 获取数据个数
|capacity|获取容量大小
|empty|判断是否为空
|resize|改变vector的size
|reserve|改变vector的capacity

**注：**
1. `capacity`的代码在`vs`和`g++`下分别运行会发现，`vs`下`capacity`是按1.5倍增长的，`g++`是按2倍增长的。
这个问题经常会考察，不要固化的认为，`vector`增容都是2倍，具体增长多少是根据具体的需求定义的。vs是PJ版本STL，g++是SGI版本STL。
2. `reserve`只负责开辟空间，如果确定知道需要用多少空间，`reserve`可以缓解`vector`增容的代价缺陷问题。
3. `resize`在开空间的同时还会进行初始化，影响`size`。
### vector容量接口使用示例
```cpp
// 测试vector的默认扩容机制
void TestVectorExpand()
{
	 size_t sz;
	 vector<int> v;
	 sz = v.capacity();
	 cout << "making v grow:\n";
	 for (int i = 0; i < 100; ++i) 
	 {
		 v.push_back(i);
		 if (sz != v.capacity()) 
		 {
			 sz = v.capacity();
			 cout << "capacity changed: " << sz << '\n';
		 }
	 }
}
```

```cpp
vs：运行结果：vs下使用的STL基本是按照1.5倍方式扩容
making foo grow:
capacity changed: 1
capacity changed: 2
capacity changed: 3
capacity changed: 4
capacity changed: 6
capacity changed: 9
capacity changed: 13
capacity changed: 19
capacity changed: 28
capacity changed: 42
capacity changed: 63
capacity changed: 94
capacity changed: 141
```

```cpp
g++运行结果：linux下使用的STL基本是按照2倍方式扩容
making foo grow:
capacity changed: 1
capacity changed: 2
capacity changed: 4
capacity changed: 8
capacity changed: 16
capacity changed: 32
capacity changed: 64
capacity changed: 128
```

```cpp
// resize(size_t n, const T& data = T())
// 将有效元素个数设置为n个，如果时增多时，增多的元素使用data进行填充
// 注意：resize在增多元素个数时可能会扩容
void TestVector3()
{
	vector<int> v;

	// set some initial content:
	for (int i = 1; i < 10; i++)
		v.push_back(i);

	v.resize(5);
	v.resize(8, 100);
	v.resize(12);

	cout << "v contains:";
	for (size_t i = 0; i < v.size(); i++)
		cout << ' ' << v[i];
	cout << '\n';
}

```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/4efd903cb6f24357aac05ebdff1e4aea.png)

```cpp
// 往vector中插入元素时，如果大概已经知道要存放多少个元素
// 可以通过reserve方法提前将容量设置好，避免边插入边扩容效率低
void TestVectorExpandOP()
{
	vector<int> v;
	size_t sz = v.capacity();
	v.reserve(100);   // 提前将容量设置好，可以避免一遍插入一遍扩容
	cout << "making bar grow:\n";
	for (int i = 0; i < 100; ++i) 
	{
		v.push_back(i);
		if (sz != v.capacity())
		{
			sz = v.capacity();
			cout << "capacity changed: " << sz << '\n';
		}
	}
}
```
## vector 增删查改
|函数名称|接口说明  |
|--|--|
|push_back  |尾插  
|pop_back|尾删
|find|查找（注意这个是算法模块实现，不是vector的成员接口）
|insert|在指定位置前插入val
|erase|删除该位置的val
|swap|交换两个vector的数据空间
|operator[ ]|像数组一样访问


### vector 增删查改接口使用演示
```cpp
////////////////////////////////////////////////////////////////////////
//  vector的增删改查
////////////////////////////////////////////////////////////////////////
// 尾插和尾删：push_back/pop_back
void TestVector4()
{
	vector<int> v;
	v.push_back(1);
	v.push_back(2);
	v.push_back(3);
	v.push_back(4);

	auto it = v.begin();
	while (it != v.end()) 
	{
		cout << *it << " ";
		++it;
	}
	cout << endl;

	v.pop_back();
	v.pop_back();

	it = v.begin();
	while (it != v.end()) 
	{
		cout << *it << " ";
		++it;
	}
	cout << endl;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/db28c08565b5498399d7d86308fa03cc.png)

```cpp
// 任意位置插入：insert和erase，以及查找find
// 注意find不是vector自身提供的方法，是STL提供的算法
void TestVector5()
{
	// 使用列表方式初始化，C++11新语法
	vector<int> v{ 1, 2, 3, 4 };

	// 在指定位置前插入值为val的元素，比如：3之前插入30,如果没有则不插入
	// 1. 先使用find查找3所在位置
	// 注意：vector没有提供find方法，如果要查找只能使用STL提供的全局find
	auto pos = find(v.begin(), v.end(), 3);
	if (pos != v.end())
	{
		// 2. 在pos位置之前插入30
		v.insert(pos, 30);
	}

	vector<int>::iterator it = v.begin();
	while (it != v.end()) 
	{
		cout << *it << " ";
		++it;
	}
	cout << endl;

	pos = find(v.begin(), v.end(), 3);
	// 删除pos位置的数据
	v.erase(pos);

	it = v.begin();
	while (it != v.end()) {
		cout << *it << " ";
		++it;
	}
	cout << endl;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/122e86b19b6b4c92a8f04b9b85ecb405.png)

```cpp
// operator[]+index 和 C++11中vector的新式for+auto的遍历
// vector使用这两种遍历方式是比较便捷的。
void TestVector6()
{
	vector<int> v{ 1, 2, 3, 4 };

	// 通过[]读写第0个位置。
	v[0] = 10;
	cout << v[0] << endl;

	// 1. 使用for+[]小标方式遍历
	for (size_t i = 0; i < v.size(); ++i)
		cout << v[i] << " ";
	cout << endl;

	vector<int> swapv;
	swapv.swap(v);

	cout << "v data:";
	for (size_t i = 0; i < v.size(); ++i)
		cout << v[i] << " ";
	cout << endl;

	// 2. 使用迭代器遍历
	cout << "swapv data:";
	auto it = swapv.begin();
	while (it != swapv.end())
	{
		cout << *it << " ";
		++it;
	}

	// 3. 使用范围for遍历
	for (auto x : v)
		cout << x << " ";
	cout << endl;
}
```
## vector 迭代器失效问题
迭代器的主要作用就是让算法能够不用关心底层数据结构，其底层实际就是一个指针，或者是对指针进行了封装，比如：`vector`的迭代器就是原生态指针`T*` 。

==因此迭代器失效，实际就是迭代器底层对应指针所指向的空间被销毁了==，而使用一块已经被释放的空间，造成的后果是程序崩溃(即如果继续使用已经失效的迭代器，
程序可能会崩溃)。

对于vector可能会导致其迭代器失效的操作有：

**一、** 会引起其底层空间改变的操作，都有可能是迭代器失效，
比如：`resize`、`reserve`、`insert`、`assign`、`push_back`等。

将有效元素个数增加到100个，多出的位置使用8填充，操作期间底层会扩容

```cpp
 v.resize(100, 8);
```

 



 reserve的作用就是改变扩容大小但不改变有效元素个数，操作期间可能会引起底层容量改变
 
```cpp
v.reserve(100);
```


 
 插入元素期间，可能会引起扩容，而导致原空间被释放
```cpp
  v.insert(v.begin(), 0);
  v.push_back(8);
```
 
 给vector重新赋值，可能会引起底层容量改变
 
 出错原因：**以上操作，都有可能会导致vector扩容，也就是说vector底层原理旧空间被释放掉，而在打印时，it还使用的是释放之间的旧空间，在对it迭代器操作时，实际操作的是一块已经被释放的空间，而引起代码运行时崩溃。**
 
 解决方式：==在以上操作完成之后，如果想要继续通过迭代器操作vector中的元素，只需给it重新赋值即可==。
 
**二、** 指定位置元素的删除操作--`erase`

`erase`删除pos位置元素后，pos位置之后的元素会往前搬移，没有导致底层空间的改变，理论上讲迭代器不应该会失效，

但是：如果pos刚好是最后一个元素，删完之后pos刚好是end的位置，而end位置是没有元素的，那么pos就失效了。因此删除vector中任意位置上元素时，vs就认为该位置迭代器失效了。

```cpp
#include <iostream>
using namespace std;
#include <vector>
int main()
{
 	int a[] = { 1, 2, 3, 4 };
 	vector<int> v(a, a + sizeof(a) / sizeof(int));
 	// 使用find查找3所在位置的iterator
 	vector<int>::iterator pos = find(v.begin(), v.end(), 3);
 	// 删除pos位置的数据，导致pos迭代器失效。
 	v.erase(pos);
 	cout << *pos << endl; // 此处会导致非法访问
 	return 0;
}
```
以下代码的功能是删除vector中所有的偶数，请问那个代码是正确的，为什么？

```cpp
#include <iostream>
using namespace std;
#include <vector>


int main()
{
 vector<int> v{ 1, 2, 3, 4 };
 auto it = v.begin();
 
 while (it != v.end())
 {
	 if (*it % 2 == 0)
	 v.erase(it);
	 ++it;
 }
 
 return 0;
}


int main()
{
 vector<int> v{ 1, 2, 3, 4 };
 auto it = v.begin();
 
 while (it != v.end())
 {
	 if (*it % 2 == 0)
	 it = v.erase(it);
	 else
	 ++it;
 }
 
 return 0;
}
```
**注：
与vector类似，string在插入+扩容操作+erase之后，迭代器也会失效**

**迭代器失效解决办法**：==在使用前，对迭代器重新赋值即可==。
