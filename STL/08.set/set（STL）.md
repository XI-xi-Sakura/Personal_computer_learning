## 序列式容器和关联式容器

前面我们已经接触过STL中的部分容器，如：`string`、`vector`、`list`、`deque`、`array`、`forward_list`等，这些容器统称为**序列式容器**，因为逻辑结构为线性序列的数据结构，**两个位置存储的值之间一般没有紧密的关联关系**，比如交换一下，它依旧是序列式容器。顺序容器中的元素是按他们在容器中的存储位置来顺序保存和访问的。

关联式容器也是用来存储数据的，与序列式容器不同的是，关联式容器逻辑结构通常是非线性结构，**两个位置有紧密的关联关系，交换一下，它的存储结构就被破坏了**。顺序容器中的元素是按关键字来保存和访问的。关联式容器有map/set系列和unordered_map/unordered_set系列。

本章节讲解的set底层是红黑树，红黑树是一颗平衡二叉搜索树。

## 键值对
`std::pair` 是 C++ 标准库中的一个模板类，它定义在 `<utility>` 头文件中。这个类可以将两个不同类型的值组合成一个单一的对象，这在很多场景下都非常实用，比如函数需要返回两个值，或者要将两个相关的数据作为一个整体存储。

### 1. 定义和初始化
`std::pair` 的模板定义形式为 `template <class T1, class T2> struct pair;`，其中 `T1` 和 `T2` 分别是两个成员的类型。下面是几种常见的初始化方式：

```cpp
#include <iostream>
#include <utility>

int main() {
    // 默认构造函数，成员会进行默认初始化
    std::pair<int, double> p1;
    std::cout << "p1: " << p1.first << ", " << p1.second << std::endl;

    // 使用提供的参数初始化
    std::pair<int, double> p2(10, 3.14);
    std::cout << "p2: " << p2.first << ", " << p2.second << std::endl;

    // 使用 make_pair 函数，自动推导类型
    auto p3 = std::make_pair(20, 2.71);
    std::cout << "p3: " << p3.first << ", " << p3.second << std::endl;

    return 0;
}
```
在这个示例中，`p1` 使用默认构造函数进行初始化，`p2` 显式地传入参数进行初始化，`p3` 则通过 `std::make_pair` 函数创建，该函数可以自动推导类型。

### 2. 成员访问
`std::pair` 有两个公共成员 `first` 和 `second`，分别对应存储的两个值。可以直接通过这两个成员来访问和修改 `std::pair` 中的元素，就像上面示例中展示的那样。

### 3. 比较操作
`std::pair` 支持多种比较操作符，如 `==`、`!=`、`<`、`<=`、`>` 和 `>=`。比较规则是先比较 `first` 成员，如果 `first` 相等，再比较 `second` 成员。

```cpp
#include <iostream>
#include <utility>

int main() {
    std::pair<int, int> p1(1, 2);
    std::pair<int, int> p2(1, 3);
    std::pair<int, int> p3(2, 1);

    std::cout << "p1 < p2: " << (p1 < p2) << std::endl;
    std::cout << "p1 < p3: " << (p1 < p3) << std::endl;

    return 0;
}
```
在这个示例中，比较操作会根据 `first` 和 `second` 成员的值来判断大小关系。

### 4. 交换操作
`std::pair` 提供了 `swap` 成员函数，可以交换两个 `std::pair` 对象的内容。

```cpp
#include <iostream>
#include <utility>

int main() {
    std::pair<int, double> p1(10, 3.14);
    std::pair<int, double> p2(20, 2.71);

    std::cout << "Before swap: p1 = (" << p1.first << ", " << p1.second << "), p2 = (" << p2.first << ", " << p2.second << ")" << std::endl;
    p1.swap(p2);
    std::cout << "After swap: p1 = (" << p1.first << ", " << p1.second << "), p2 = (" << p2.first << ", " << p2.second << ")" << std::endl;

    return 0;
}
```
这个示例展示了如何使用 `swap` 函数交换两个 `std::pair` 对象的内容。

## set介绍
- 集是按特定顺序存储唯一元素的容器。
- 在 `set`中 ，元素的值也标识其本身（该值本身是 `key`，类型为`T` ），并且每个值必须是唯一的。 
- `set` 中元素的值不能在容器中修改（元素始终为 `const`），但可以在容器中插入或移除它们。
- 在内部， set 中的元素始终按照其内部比较对象 （类型 `Compare` ）指示的**特定严格弱排序标准进行排序**。

- `set` 容器通常比 `unordered_set` 容器通过键访问单个元素的速度慢，但它们允许根据顺序对子集进行直接迭代。

- set通常实现为**二叉搜索树** 。

注：

1. set只放value，但在其底层实际存放的是由<value,value>构成的键值对
2. set插入元素时，无需构建键值对，秩序插入value即可
3. set元素不可重复（可用set去重）
4. 使用迭代器遍历set元素，可得到有序序列
5. set最终查找某个元素，时间复杂度为log^N^
 

## set系列的使用

   - **[set和multiset参考文档](https://legacy.cplusplus.com/reference/set/)**
   - **set类的介绍**：
   		- set的声明如下，**T就是set底层关键字的类型。**
   		- set默认要求`T`支持小于比较，如果不支持或者想按自己的需求走可以自行实现仿函数传给第二个模版参数。
   		- set底层存储数据的内存是从空间配置器申请的，如果需要可以自己实现内存池，传给第三个参数。一般情况下，我们都不需要传后两个模版参数。
   		- set底层是用红黑树实现，增删查效率是$O(log N)$，迭代器遍历是走的搜索树的中序，所以是有序的。
```cpp
template < class T 						// set::key_type/value_type
	, class Compare = less<T>, 			// set::key_compare/value_compare
	, class Alloc = allocator<T> 		// set::allocator_type
> class set;
```

   - **set的构造和迭代器**：set的构造我们关注以下几个接口即可。
   		- set支持正向和反向迭代遍历，遍历默认按升序顺序，因为底层是二叉搜索树，迭代器遍历走的中序；
   		- 支持迭代器就意味着支持范围for，set的iterator和const_iterator都不支持迭代器修改数据，修改关键字数据，破坏了底层搜索树的结构。
```cpp
// empty (1) 无参默认构造
explicit set (const key_compare& comp = key_compare(),
			const allocator_type& alloc = allocator_type());

// range (2) 迭代器区间构造
template <class InputIterator>
set (InputIterator first, InputIterator last,
	const key_compare& comp = key_compare(),
	const allocator_type& = allocator_type());

// copy (3) 拷贝构造
set (const set& x);

// initializer list (5) initializer 列表构造
set (initializer_list<value_type> il,
	const key_compare& comp = key_compare(),
	const allocator_type& alloc = allocator_type());
	

```

```cpp
// 迭代器是一个双向迭代器
// 正向迭代器
iterator begin();
iterator end();

// 反向迭代器
reverse_iterator rbegin();
reverse_iterator rend();
```

 - **set的插入**：

```cpp
// 如果已经存在则插入失败
pair<iterator,bool> insert (value_type&& val);

// 列表插入，已经在容器中存在的值不会插入
void insert (initializer_list<value_type> il);

// 迭代器区间插入，已经在容器中存在的值不会插入
template <class InputIterator>
void insert (InputIterator first, InputIterator last);
```
	>返回一个键值对，其成员 pair::first 被设置为指向新插入的元素或已存在于集合中的等效元素的迭代器。
	如果新元素被插入,对键值对中的 pair::second 元素被设置为 true ，如果等效元素已存在，设置为 false .
		
- **set的查找**：
		
```cpp
// 查找val，返回val所在的迭代器，没有找到返回end()
iterator find (const value_type& val);

// 查找val，返回Val的个数
size_type count (const value_type& val) const;
```
- **set的删除**：


```cpp
iterator erase (const_iterator position);

// 删除val，val不存在返回0，存在返回1
size_type erase (const value_type& val);

// 删除一段迭代器区间的值
iterator erase (const_iterator first, const_iterator last);
```

- **set的比较**：

```cpp
// 返回大于等于val位置的迭代器
iterator lower_bound (const value_type& val) const;

// 返回大于val位置的迭代器
iterator upper_bound (const value_type& val) const;
```
- **set的容量**：

```cpp
//测试容器是否为空
bool empty() const；

//返回容器大小
size_type size() const;
```

### 使用样例

  - **insert和迭代器遍历使用样例**
```cpp
#include<iostream>
#include<set>
using namespace std;
int main()
{
    // 去重+升序排序
    set<int> s;
    // 去重+降序排序(给一个大于的仿函数)
    //set<int, greater<int>> s;
    s.insert(5);
    s.insert(2);
    s.insert(7);
    s.insert(5);
    //set<int>::iterator it = s.begin();
    auto it = s.begin();
    while (it != s.end())
    {
        // error C3892: “it”: 不能给常量赋值
        // *it = 1;
        cout << *it << " ";
        ++it;
    }
    cout << endl;
    // 插入一段initializer_list列表值，已经存在的值插入失败
    s.insert({ 2,8,3,9 });
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    set<string> strset = { "sort", "insert", "add" };
    // 遍历string比较ascll码大小顺序遍历的
    for (auto& e : strset)
    {
        cout << e << " ";
    }
    cout << endl;
    return 0;
}
```
   - **find和erase使用样例**
```cpp
#include<iostream>
#include<set>
using namespace std;
int main()
{
    set<int> s = { 4,2,7,2,8,5,9 };
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    // 删除最小值
    s.erase(s.begin());
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    // 直接删除x
    int x;
    cin >> x;
    int num = s.erase(x);
    if (num == 0)
    {
        cout << x << "不存在!" << endl;
    }
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    // 直接查找在利用迭代器删除x
    cin >> x;
    auto pos = s.find(x);
    if (pos != s.end())
    {
        s.erase(pos);
    }
    else
    {
        cout << x << "不存在!" << endl;
    }
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    // 算法库的查找 O(N)
    auto pos1 = find(s.begin(), s.end(), x); 
    // set自身实现的查找 O(logN)
    auto pos2 = s.find(x);
    // 利用count间接实现快速查找
    cin >> x;
    if (s.count(x))
    {
        cout << x << "在!" << endl;
    }
    else
    {
        cout << x << "不存在!" << endl;
    }
    return 0;
}
```
```cpp
#include<iostream>
#include<set>
using namespace std;
int main()
{
    std::set<int> myset;
    for (int i = 1; i < 10; i++)
    {
        myset.insert(i * 10); // 10 20 30 40 50 60 70 80 90
    }
    for (auto e : myset)
    {
        cout << e << " ";
    }
    cout << endl;
    // 实现查找到的[itlow,itup)包含[30, 60]区间
    // 返回 >= 30
    auto itlow = myset.lower_bound(30);
    // 返回 > 60
    auto itup = myset.upper_bound(60);
    // 删除这段区间的值
    myset.erase(itlow, itup);
    for (auto e : myset)
    {
        cout << e << " ";
    }
    cout << endl;
    return 0;
}
```

## multiset
- multiset和set的差异：
   - multiset和set的使用基本完全类似，主要区别点在于multiset支持值冗余，那么insert/find/count/erase都围绕着支持值冗余有所差异，具体参看下面的样例代码理解。
```cpp
#include<iostream>
#include<set>
using namespace std;
int main()
{
    // 相比set不同的是，multiset是排序，但是不去重
    multiset<int> s = { 4,2,7,2,4,8,4,5,4,9 };
    auto it = s.begin();
    while (it != s.end())
    {
        cout << *it << " ";
        ++it;
    }
    cout << endl;
    // 相比set不同的是，x可能会存在多个，find查找中序的第一个
    int x;
    cin >> x;
    auto pos = s.find(x);
    while (pos != s.end() && *pos == x)
    {
        cout << *pos << " ";
        ++pos;
    }
    cout << endl;
    // 相比set不同的是，count会返回x的实际个数
    cout << s.count(x) << endl;
    // 相比set不同的是，erase给值时会删除所有的x
    s.erase(x);
    for (auto e : s)
    {
        cout << e << " ";
    }
    cout << endl;
    return 0;
}
```
   
  - **349. 两个数组的交集 - 力扣(LeetCode)**
```cpp
class Solution{
public:
    vector<int>intersection(vector<int>& nums1, vector<int>& nums2){
        set<int>s1(nums1.begin(),nums1.end());
        set<int>s2(nums2.begin(),nums2.end());
        //因为set遍历是有序的，有序值，依次比较
        //小的++，相等的就是交集
        vector<int>ret;
        auto it1 = s1.begin();
        auto it2 = s2.begin();
        while(it1!=s1.end() &&it2!=s2.end())
        {
            if(*it1<*it2)
            {
                it1++;
            }
            else if(*it1>*it2)
            {
                it2++;
            }
            else
            {
                ret.push_back(*it1);
                it1++;
                it2++;
            }
        }
        return ret;
    }
};
```
   - **142.环形链表II-力扣(LeetCode)**：数据结构初阶阶段，我们通过证明一个指针从头开始走一个指针从相遇点开始走，会在入口点相遇，理解证明都会很麻烦。这里我们使用set查找记录解决非常简单方便，这里体现了set在解决一些问题时的价值，完全是降维打击。
```cpp
class Solution{
public:
    ListNode *detectCycle(ListNode* head){
        set<ListNode*>s;
        ListNode* cur = head;
        while(cur)
        {
            auto ret = s.insert(cur);
            if(ret.second == false)
            {
                return cur;
            }
            cur = cur->next;
        }
        return nullptr;
    }
};
```

