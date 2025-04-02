## C/C++内存分布

### 代码内存相关问题

```cpp
int globalVar = 1;
static int staticGlobalVar = 1;

void Test()
{
	static int staticVar = 1;
	
	int localVar = 1;
	
	int num1[10] = { 1, 2, 3, 4 };
	
	char char2[] = "abcd";
	
	const char* pChar3 = "abcd";
	
	int* ptr1 = (int*)malloc(sizeof(int) * 4);
	
	int* ptr2 = (int*)calloc(4, sizeof(int));
	
	int* ptr3 = (int*)realloc(ptr2, sizeof(int) * 4);
	
	free(ptr1);
	free(ptr3);
}
```
一、选择题：
选项: A.栈 		B.堆 			C.数据段(静态区) 	D.代码段(常量区)

globalVar在哪里？				__==C==__

staticGlobalVar在哪里？		__C__

staticVar在哪里？				__C__

localVar在哪里？					__A__

num1 在哪里？					__A__

char2在哪里？						__A__

*char2在哪里？					__A__

pChar3在哪里？ 					__A__

*pChar3在哪里？				__==D==__

ptr1在哪里？						__==A==__ 

*ptr1在哪里？						__B__

（*pChar3指向的是字符串常量 "abcd"。字符串常量在程序编译后通常存储在代码段（常量区），这是因为字符串常量在程序运行过程中不应该被修改，具有只读属性，就如同程序的代码一样在运行期间保持不变。所以，*pChar3所指向的内容在代码段（常量区）。）

二、填空题：
sizeof(num1) = 	__40__;

sizeof(char2) = 	__5__; 	

strlen(char2) = __4__;

sizeof(pChar3) = __4/8__; 	

strlen(pChar3) = __4__;

sizeof(ptr1) = 		__4/8__;

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/1ca7bf4ad8144dbe95be4858c6f7d9f3.png)

1. `sizeof(num1)`：
   - `num1`是一个整型数组，其中包含 10 个整数。在大多数系统中，一个整数通常占用 4 个字节，所以`sizeof(num1)`的值为 10 * 4 = 40。

2. `sizeof(char2)`：
   - `char2`是一个字符数组，初始化为"abcd"。字符串"abcd"实际上存储为' a', 'b', 'c', 'd', '\0'这五个字符，每个字符占用 1 个字节，所以`sizeof(char2)`的值为 5。

3. `strlen(char2)`：
   - `strlen`函数计算的是字符串的长度，不包括字符串结束符'\0'。这里字符串"abcd"的长度为 4，所以`strlen(char2)`的值为 4。

4. `sizeof(pChar3)`：
   - `pChar3`是一个指针变量，在大多数系统中，指针变量的大小通常为 4 或 8 字节（取决于系统是 32 位还是 64 位）。所以`sizeof(pChar3)`的值为 4 或 8。

5. `strlen(pChar3)`：
   - 由于`pChar3`指向的字符串也是"abcd"，和`char2`指向的内容相同，所以`strlen(pChar3)`的值也为 4。

6. `sizeof(ptr1)`：
   - `ptr1`也是一个指针变量，其大小与`pChar3`相同，为 4 或 8 字节。

三、sizeof 和 strlen 区别？
`sizeof`和`strlen`主要有以下区别：

**1.性质不同**
- `sizeof`是一个运算符，在编译时进行计算，不需要实际运行程序就能确定结果。它给出的是操作数在内存中所占用的字节数，其结果是一个常量表达式。
- `strlen`是一个函数，在程序运行时计算，它从给定的字符指针开始，逐个字符检查，直到遇到字符串结束符'\0'为止，然后返回字符串的长度（不包括结束符）。

**2.作用对象不同**
- `sizeof`可以用于各种数据类型和变量，包括基本数据类型（如`int`、`char`、`double`等）、数组、结构体、联合体等。例如，对于一个数组`int arr[10]`，`sizeof(arr)`会给出整个数组所占用的字节数。对于一个结构体类型的变量，`sizeof`会给出该结构体变量所占用的内存空间大小。
- `strlen`只能用于以'\0'结尾的字符串，即字符指针指向的内存区域必须是以空字符结束的字符序列。

**3.计算内容不同**
- `sizeof`计算的是数据类型或变量所占用的内存空间大小，包括可能存在的填充字节等。例如，对于一个结构体，`sizeof`可能会考虑到对齐要求而包含一些额外的字节。
- `strlen`只计算字符串的有效字符长度，不包括字符串结束符'\0'。
### 内存区域划分
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/870b3120a315447096c04dafd925f88d.png)
【说明】
1. **栈**又叫堆栈——非静态局部变量/函数参数/返回值等等，==栈是向下增长==的。
2. 内存映射段是高效的I/O映射方式，用于装载一个共享的动态内存库。用户可使用系统接口创建共享共享内存，做进程间通信。
3. 堆用于程序运行时动态内存分配，==堆是向上增长==的。
4. 数据段——存储全局数据和静态数据。
5. 代码段——可执行的代码/只读常量。
##  C++内存管理方式（new/delete）
C语言内存管理方式在C++中可以继续使用，但有些地方就无能为力，而且使用起来比较麻烦，因此C++又提出了自己的内存管理方式：通过==new和delete==操作符进行动态内存管理。
### new/delete操作内置类型

```cpp
void Test()
{
// 动态申请一个int类型的空间
int* ptr4 = new int;

// 动态申请一个int类型的空间并初始化为10
int* ptr5 = new int(10);

// 动态申请10个int类型的空间
int* ptr6 = new int[10];

delete ptr4;
delete ptr5;
delete[] ptr6;
}
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/7bfc4dd102f745e0a420a96d68e24441.png)
注意：申请和释放单个元素的空间，使用new和delete操作符，申请和释放连续的空间，使用new[ ]和delete[ ]，注意：匹配起来使用。
###  new和delete操作自定义类型
new/delete 和 malloc/free最大区别是 ==new/delete对于【自定义类型】除了开空间还会调用构造函数和析构函数==。
```cpp
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
	free(p1);

	A* p2 = new A(1);
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
```
注意：**在申请自定义类型的空间时，new会调用构造函数，delete会调用析构函数，而malloc与free不会**。
### operator new与operator delete函数
new和delete是用户进行动态内存申请和释放的操作符，operator new 和operator delete是系统提供的全局函数，new在底层调用operator new全局函数来申请空间，delete在底层通过operator delete全局函数来释放空间。


**operator new**：该函数实际通过malloc来申请空间，当malloc申请空间成功时直接返回；申请空间失败，尝试执行空间不足应对措施，如果改应对措施用户设置了，则继续申请，否则抛异常。

```cpp
void *__CRTDECL operator new(size_t size) _THROW1(_STD bad_alloc)
{
	// try to allocate size bytes
	void *p;
	
	while ((p = malloc(size)) == 0)
		if (_callnewh(size) == 0)
		{
				// report no memory
				// 如果申请内存失败了，这里会抛出bad_alloc 类型异常
			static const std::bad_alloc nomem;
			_RAISE(nomem);
		}
	return (p);
}
```
**operator delete**: 该函数最终是通过free来释放空间的。

```cpp
void operator delete(void *pUserData)
{
	_CrtMemBlockHeader * pHead;
	RTCCALLBACK(_RTC_Free_hook, (pUserData, 0));
	
	if (pUserData == NULL)
	return;
	
	_mlock(_HEAP_LOCK); /* block other threads */
	__TRY
	
	/* get a pointer to memory block header */
	pHead = pHdr(pUserData);
	/* verify block type */
	
	_ASSERTE(_BLOCK_TYPE_IS_VALID(pHead->nBlockUse));
	_free_dbg( pUserData, pHead->nBlockUse );
	__FINALLY
	
	_munlock(_HEAP_LOCK); /* release other threads */
	__END_TRY_FINALLY
	
	return;
}
/*
free的实现
*/
#define free(p) _free_dbg(p, _NORMAL_BLOCK)
```
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/9da6ae66534b4be49c669c007bf1c056.png)
通过上述两个全局函数的实现知道，**operator new 实际也是通过malloc**来申请空间，如果malloc申请空间成功就直接返回，否则执行用户提供的空间不足应对措施，如果用户提供该措施就继续申请，否则就抛异常。**operator delete 最终是通过free**来释放空间的。
##  new和delete的实现原理
### 内置类型
如果申请的是内置类型的空间，new和malloc，delete和free基本类似，不同的地方是：new/delete申请和释放的是单个元素的空间，new[ ]和delete[ ]申请的是连续空间，而且new在申请空间失败时会抛异常，malloc会返回NULL。
### 自定义类型
- `new`的原理
1. 调用operator new函数申请空间
2. 在申请的空间上执行构造函数，完成对象的构造

- `delete`的原理
1. 在空间上执行析构函数，完成对象中资源的清理工作
4. 调用operator delete函数释放对象的空间

- `new T[N]`的原理
1. 调用operator new[]函数，在operator new[]中实际调用operator new函数完成N个对象空间的申请
6. 在申请的空间上执行N次构造函数

- `delete[]`的原理
1. 在释放的对象空间上执行N次析构函数，完成N个对象中资源的清理
8. 调用operator delete[ ]释放空间，实际在operator delete[ ]中调用operator delete来释放空间。
## 定位new表达式(placement-new)
定位new表达式是在已分配的原始内存空间中调用构造函数初始化一个对象。



使用格式：
new (place_address) type或者new (place_address) type(initializer-list)
place_address必须是一个指针，initializer-list是类型的初始化列表


使用场景：
定位new表达式在实际中一般是配合内存池使用。
因为内存池分配出的内存没有初始化，所以如果是自定义类型的对象，需要使用new的定义表达式进行显示调构造函数进行初始化。

```cpp
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
```

```cpp
// 定位new/replacement new
int main()
{
	// p1现在指向的只不过是与A对象相同大小的一段空间，还不能算是一个对象，因为构造函数没有执行
	A* p1 = (A*)malloc(sizeof(A));
	new(p1)A; // 注意：如果A类的构造函数有参数时，此处需要传参
	p1->~A();
	free(p1);
	
	
	A* p2 = (A*)operator new(sizeof(A));
	new(p2)A(10);
	p2->~A();
	operator delete(p2);

return 0;
}
```

