## [list的介绍](https://legacy.cplusplus.com/reference/list/list/)
1. `list`是可以在常数范围内==在任意位置进行插入和删除的序列式容器==，并且该容器==可以前后双向迭代==。
2. `list`的底层是双向链表结构，双向链表中每个元素存储在互不相关的独立节点中，在节点中通过指针指向其前一个元素和后一个元素。
3. `list`与`forward_list`非常相似：最主要的不同在于`forward_list`是单链表，只能朝前迭代，已让其更简单高效。
4. 与其他的序列式容器相比`(array`，`vector`，`deque`)，`list`通常在**任意位置进行插入、移除元素的执行效率更好**。
5. 与其他序列式容器相比，**list和forward_list最大的缺陷是不支持任意位置的随机访问**，比如：要访问`list`的第6个元素，必须从已知的位置(比如头部或者尾部)迭代到该位置，在这段位置上迭代需要线性的时间开销；list还需要一些额外的空间，以保存每个节点的相关联信息(对于存储类型较小元素的大list来说这可能是一个重要的因素).


![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/1a08441547e344f9a8b4249477adea0f.jpeg)
##  [list的构造](https://legacy.cplusplus.com/reference/list/list/list/)
|构造函数| 函数功能 |
|--|--|
| list (size_type n, const value_type& val = value_type())  | 构造的list中包含n个值为val的元素 
|list() |构造空的list
|list (const list& x) |拷贝构造函数
|list (InputIterator first, InputIterator last)|用[first, last)区间中的元素构造list|

### list构造代码使用演示

```cpp
// list的构造
void TestList1()
{
    list<int> l1;                         // 构造空的l1
    list<int> l2(4, 100);                 // l2中放4个值为100的元素
    list<int> l3(l2.begin(), l2.end());  // 用l2的[begin(), end()）左闭右开的区间构造l3
    list<int> l4(l3);                    // 用l3拷贝构造l4

    // 以数组为迭代器区间构造l5
    int array[] = { 16,2,77,29 };
    list<int> l5(array, array + sizeof(array) / sizeof(int));

    // 列表格式初始化C++11
    list<int> l6{ 1,2,3,4,5 };

    // 用迭代器方式打印l5中的元素
    list<int>::iterator it = l5.begin();
    while (it != l5.end())
    {
        cout << *it << " ";
        ++it;
    }       
    cout << endl;

    // C++11范围for的方式遍历
    for (auto& e : l6)
        cout << e << " ";

    cout << endl;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/d9c30320defc4b1da34585eae8145d57.png)
##  list iterator的使用
|函数声明|接口说明  |
|--|--|
|begin  |返回第一个元素的迭代器
|end|返回最后一个元素下一个位置的迭代器
|rbegin|返回第一个元素的reverse_iterator,即end位置
|rend|返回最后一个元素下一个位置的reverse_iterator,即begin位置|

【注意】
1. begin与end为正向迭代器，对迭代器执行++操作，迭代器向后移动
2. rbegin(end)与rend(begin)为反向迭代器，对迭代器执行++操作，迭代器向前移动
### list迭代器代码使用演示

```cpp
// list迭代器的使用
// 注意：遍历链表只能用迭代器和范围for
void PrintList(const list<int>& l)
{
    // 注意这里调用的是list的 begin() const，返回list的const_iterator对象
    for (list<int>::const_iterator it = l.begin(); it != l.end(); ++it)
    {
        cout << *it << " ";
        // *it = 10; 编译不通过
    }

    cout << endl;
}

void TestList2()
{
    int array[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };
    list<int> l(array, array + sizeof(array) / sizeof(array[0]));
    // 使用正向迭代器正向list中的元素
    // list<int>::iterator it = l.begin();   // C++98中语法
    auto it = l.begin();                     // C++11之后推荐写法
    while (it != l.end())
    {
        cout << *it << " ";
        ++it;
    }
    cout << endl;

    // 使用反向迭代器逆向打印list中的元素
    // list<int>::reverse_iterator rit = l.rbegin();
    auto rit = l.rbegin();
    while (rit != l.rend())
    {
        cout << *rit << " ";
        ++rit;
    }
    cout << endl;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/d5cc97b52e0c442abd8fba0fd4c80a79.png)
##  list capacity
|函数声明|接口说明  |
|--|--|
| [empty](https://legacy.cplusplus.com/reference/list/list/empty/) | 检测list是否为空，是返回true，否则返回false
|[size](https://legacy.cplusplus.com/reference/list/list/size/)|返回list中有效节点的个数

##  list element access
|函数说明| 接口说明 |
|--|--|
| [front](https://legacy.cplusplus.com/reference/list/list/front/) | 返回list的第一个节点中值的引用 
| [back](https://legacy.cplusplus.com/reference/list/list/back/) |返回list的最后一个节点中值的引用

##  list modifiers
|函数说明|接口说明  |
|--|--|
| push_front | 在list首元素前插入值为val的元素 
| pop_front|删除list中第一个元素
|push_back|在list尾部插入值为val的元素
|pop_back|删除list中最后一个元素
|insert|在list position 位置中插入值为val的元素
|erase|删除list position位置的元素
|swap|交换两个list中的元素
|clear|清空list中的有效元素
### list的插入和删除使用代码演示
```cpp
// list插入和删除
// push_back/pop_back/push_front/pop_front
void TestList3()
{
    int array[] = { 1, 2, 3 };
    list<int> L(array, array + sizeof(array) / sizeof(array[0]));

    // 在list的尾部插入4，头部插入0
    L.push_back(4);
    L.push_front(0);
    PrintList(L);

    // 删除list尾部节点和头部节点
    L.pop_back();
    L.pop_front();
    PrintList(L);
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/b8f806e66a7740ee8036f385a6c4bfc3.png)

```cpp
// insert /erase 
void TestList4()
{
    int array1[] = { 1, 2, 3 };
    list<int> L(array1, array1 + sizeof(array1) / sizeof(array1[0]));

    // 获取链表中第二个节点
    auto pos = ++L.begin();
    cout << *pos << endl;

    // 在pos前插入值为4的元素
    L.insert(pos, 4);
    PrintList(L);

    // 在pos前插入5个值为5的元素
    L.insert(pos, 5, 5);
    PrintList(L);

    // 在pos前插入[v.begin(), v.end)区间中的元素
    vector<int> v{ 7, 8, 9 };
    L.insert(pos, v.begin(), v.end());
    PrintList(L);

    // 删除pos位置上的元素
    L.erase(pos);
    PrintList(L);

    // 删除list中[begin, end)区间中的元素，即删除list中的所有元素
    L.erase(L.begin(), L.end());
    PrintList(L);
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/6bad40aabde4443e9feac5b2d5301c39.png)

```cpp
// resize/swap/clear
void TestList5()
{
    // 用数组来构造list
    int array1[] = { 1, 2, 3 };
    list<int> l1(array1, array1 + sizeof(array1) / sizeof(array1[0]));
    PrintList(l1);

    // 交换l1和l2中的元素
    list<int> l2;
    l1.swap(l2);
    PrintList(l1);
    PrintList(l2);

    // 将l2中的元素清空
    l2.clear();
    cout << l2.size() << endl;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/360e22a25c4448e6a59fbb5be8f440c4.png)
##  list的迭代器失效
此处大家可将迭代器暂时理解成类似于指针，
迭代器失效即迭代器所指向的节点的无效，即该节点被删除了。因为list的底层结构为带头结点的双向循环链表，因此在list中进行插入时是不会导致list的迭代器失效的，只有在删除时才会失效，并且失效的只是指向被删除节点的迭代器，其他迭代器不会受到影响。

```cpp
void TestListIterator1()
{
	 int array[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };
	 list<int> l(array, array+sizeof(array)/sizeof(array[0]));
	 auto it = l.begin();
	 while (it != l.end())
	 {
		 // erase()函数执行后，it所指向的节点已被删除，因此it无效，在下一次使用it时，必须先给其赋值
		 l.erase(it); 
		 ++it;
	 }
}
```

```cpp
// 改正
void TestListIterator()
{
	 int array[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 0 };
	 list<int> l(array, array+sizeof(array)/sizeof(array[0]));
	 auto it = l.begin();
	 while (it != l.end())
	 {
	 	l.erase(it++); // it = l.erase(it);
	 }
}
```
## list与vector的对比
vector与list都是STL中非常重要的序列式容器，由于两个容器的底层结构不同，导致其特性以及应用场景不同，其主要不同如下：
|        | vector | list|
|--|--|--|
| 底层结构 | 动态顺序表，一段连续空间  | 带头结点的双向循环链表 
|随机访问|支持随机访问，访问某个元素效率O(1)|不支持随机访问，访问某个元素效率O(N)
|插入删除|任意位置插入和删除效率低，需要搬移元素，时间复杂度为O(N)，插入时有可能需要增容，增容：开辟新空间，拷贝元素，释放旧空间，导致效率更低|任意位置插入和删除效率高，不需要搬移元素，时间复杂度为O(1)
|空间利用率|底层为连续空间，不容易造成内存碎片，空间利用率高，缓存利用率高|底层节点动态开辟，小节点容易造成内存碎片，空间利用率低，缓存利用率低
|迭代器|原生态指针|对原生态指针(节点指针)进行封装
|迭代器失效问题|在插入元素时，要给所有的迭代器重新赋值，因为插入元素有可能会导致重新扩容，致使原来迭代器失效，删除时，当前迭代器需要重新赋值否则会失效|插入元素不会导致迭代器失效，删除元素时，只会导致当前迭代器失效，其他迭代器不受影响
|使用场景|需要高效存储，支持随机访问，不关心插入删除效率|大量插入和删除操作，不关心随机访问
