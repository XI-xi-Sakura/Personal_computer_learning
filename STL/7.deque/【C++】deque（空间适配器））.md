## 适配器
适配器是一种设计模式(设计模式是一套被反复使用的、多数人知晓的、经过分类编目的、代码设计经验的总结)，该种模式是将一个类的接口转换成客户希望的另外一个接口。

## STL标准库中`stack`和`queue`的底层结构
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/583a7b20622c4dc9a270e5a1352847c9.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/302855cb7cf94ca9a67df4c31cde3d92.png)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/60e9661ea3fc42948c78230083ae69d0.png)
## `deque`原理介绍

`deque`(双端队列)：**是一种双开口的"连续"空间的数据结构**，
双开口的含义是：可以在头尾两端进行插入和删除操作，且时间复杂度为O(1)。

与vector比较，头插效率高，不需要搬移元素；与list比较，空间利用率比较高。
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/e69fbce238224e14bc0e649caee830d2.png)

`deque`**并不是真正连续的空间，而是由一段段连续的小空间拼接而成的**，实际`deque`类似于一个动态的二维数组，其底层结构如下图所示：
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/25a8be2a82d945e18fb6315f2a78be74.png)
双端队列底层是一段假象的连续空间，实际是分段连续的，为了维护其“整体连续”以及随机访问的假象，落在了deque的迭代器身上，因此deque的迭代器设计就比较复杂，如下图所示：
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/f8a29dca5c2d4f5eb935da3585685263.png)

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/446d289a827d4f6c8b7b5e79eb110751.png)
## deque使用
### [构造](https://legacy.cplusplus.com/reference/deque/deque/deque/)
|函数原型| 功能实现 |
|--|--|
| deque(  )| 默认构造 
| deque( n , val)| 构造一个包含 n 个元素值为val的容器
| deque （InputIterator first， InputIterator last）|迭代器范围构造
| deque (const deque& x)|拷贝构造|


### [成员函数](https://legacy.cplusplus.com/reference/deque/deque/?kw=deque)
|函数原型| 功能实现 |
|--|--|
| push_back() |  在队列的尾部插入元素。
|emplace_front()|与push_front()的作用一样
|push_front()|在队列的头部插入元素
|emplace_back()|与push_back()的作用一样 
|back()|回队列尾部元素的引用
|front()|返回队列头部元素的引用
|pop_back()|删除队列尾部的元素。
|pop_front()|删除队列头部的元素
|empty()|判断队列是否为空
|size()|返回队列中元素的个数
|begin()|返回头位置的迭代器
|end()|返回尾+1位置的迭代器
|insert()|在指定位置插入元素 
|erase()|在指定位置删除元素 





##  deque的缺陷
与`vector`比较，`deque`的优势是：头部插入和删除时，不需要搬移元素，效率特别高，而且在扩容时，也不需要搬移大量的元素，因此其效率是必`vector`高的。

与`list`比较，其底层是连续空间，空间利用率比较高，不需要存储额外字段。

但是，deque有一个致命缺陷：**不适合遍历**，因为在遍历时，==deque的迭代器要频繁的去检测其是否移动到某段小空间的边界，导致效率低下==，而序列式场景中，可能需要经常遍历，
因此在实际中，需要线性结构时，大多数情况下优先考虑`vector`和`list`，deque的应用并不多，而目前能看到的一个应用就是，STL用其作为`stack`和`queue`的底层数据结构。

## 为什么选择deque作为stack和queue的底层默认容器?
`stack`是一种后进先出的特殊线性数据结构，因此只要具有`push_back()`和`pop_back()`操作的线性结构，都可
以作为`stack`的底层容器，比如`vector`和`list`都可以；
`queue`是先进先出的特殊线性数据结构，只要具有`push_back`和`pop_front`操作的线性结构，都可以作为`queue`的底层容器，比如`list`.

但是STL中对stack和queue默认选择deque作为其底层容器，主要是因为：
1. `stack`和`queue`不需要遍历(因此`stack`和`queue`没有迭代器)，只需要在固定的一端或者两端进行操作。
2. 在`stack`中元素增长时，`deque`比`vector`的效率高(扩容时不需要搬移大量数据)；`queue`中的元素增长
时，`deque`不仅效率高，而且内存使用率高。

结合了deque的优点，而完美的避开了其缺陷。

##  STL标准库中对于stack和queue的模拟实现
### stack的模拟实现

```cpp
#include<deque>
namespace bite
{ 
	  template<class T, class Con = deque<T>>
	 //template<class T, class Con = vector<T>>
	 //template<class T, class Con = list<T>>
	 class stack
	 {
		 public:
			 stack() {}
			 void push(const T& x) {_c.push_back(x);}
			 void pop() {_c.pop_back();}
			 T& top() {return _c.back();}
			 const T& top()const {return _c.back();}
			 size_t size()const {return _c.size();}
			 bool empty()const {return _c.empty();}
		 
		 private:
		 	Con _c;
 	};
}
```
 

### queue的模拟实现

```cpp
#include<deque>
#include <list>
namespace bite
{
	 template<class T, class Con = deque<T>>
	 //template<class T, class Con = list<T>>
	 class queue
	 {
		 public:
			 queue() {}
			 void push(const T& x) {_c.push_back(x);}
			 void pop() {_c.pop_front();}
			 T& back() {return _c.back();}
			 const T& back()const {return _c.back();}
			 T& front() {return _c.front();}
			 const T& front()const {return _c.front();}
			 size_t size()const {return _c.size();}
			 bool empty()const {return _c.empty();}
		 private:
			 Con _c;
	 };
}
```

