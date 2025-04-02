## [stack的介绍](https://legacy.cplusplus.com/reference/stack/stack/)
1. `stack`是一种容器适配器，专门用在具有后进先出操作的上下文环境中，其删除只能从容器的一端进行元素的插入与提取操作。
2. `stack`是作为容器适配器被实现的，容器适配器即是对特定类封装作为其底层的容器，并提供一组特定的成员函数来访问其元素，将特定类作为其底层的，元素特定容器的尾部(即栈顶)被压入和弹出。
3. `stack`的底层容器可以是任何标准的容器类模板或者一些其他特定的容器类，这些容器类应该支持以下操作：
`empty`：判空操作
`back`：获取尾部元素操作
`push_back`：尾部插入元素操作
`pop_back`：尾部删除元素操作
4. 标准容器`vector`、`deque`、`list`均符合这些需求，默认情况下，如果没有为`stack`指定特定的底层容器，默认情况下使用`deque`。

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/45b7771901df4a6c9cc183d009091492.png)
## stack的使用
|函数说明| 接口说明 |
|--|--|
| stack( ) | 构造空的栈 
| empty( )| 检测stack是否为空
| size ( )|返回stack中元素的个数
|top( ) |返回栈顶元素的引用
| push( )|将元素val压入stack中
|pop( ) |将stack中尾部的元素弹出

[最小栈](https://leetcode.cn/problems/min-stack/)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/eb14d484ae094f45bfb0fd21ac2f93b0.png)

```cpp
#include <iostream>
#include <stack>

class MinStack {
public:
    MinStack() {}

    void push(int x) {
        _elem.push(x);
        if (_min.empty() || x <= _min.top())
            _min.push(x);
    }

    void pop() {
        if (_min.top() == _elem.top())
            _min.pop();
        _elem.pop();
    }

    int top() {
        return _elem.top();
    }

    int getMin() {
        return _min.top();
    }

private:
    std::stack<int> _elem;
    std::stack<int> _min;
};
```
[栈的压入弹出序列](https://www.nowcoder.com/practice/d77d11405cc7470d82554cb392585106?tpId=13&&tqId=11174&rp=1&ru=/activity/oj&qru=/ta/coding-interviews/question-ranking)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/8fffa36101ed4aaf9773237d131efb42.png)

```cpp
class Solution {
public:
 bool IsPopOrder(vector<int> pushV,vector<int> popV) {
 //入栈和出栈的元素个数必须相同
 if(pushV.size() != popV.size())
 return false;
 
 // 用s来模拟入栈与出栈的过程
 int outIdx = 0;
 int inIdx = 0;
 stack<int> s;
 
 while(outIdx < popV.size())
 {
 // 如果s是空，或者栈顶元素与出栈的元素不相等，就入栈
 while(s.empty() || s.top() != popV[outIdx])
 {
 if(inIdx < pushV.size())
 s.push(pushV[inIdx++]);
 else
 return false;
 }
 
 // 栈顶元素与出栈的元素相等，出栈
 s.pop();
 outIdx++;
 }
 
 return true;
 }
};
```
[逆波兰式求值](https://leetcode.cn/problems/evaluate-reverse-polish-notation/description/)
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/128eb1c005bb4d70ba3f6b88c38de445.png)


```cpp
class Solution {
public:
 int evalRPN(vector<string>& tokens) 
 {
	 stack<int> s;
	 
	 for (size_t i = 0; i < tokens.size(); ++i)
	 {
		 string& str = tokens[i];
		 // str为数字
		 if (!("+" == str || "-" == str || "*" == str || "/" == str))
		 {
		 	s.push(atoi(str.c_str()));
		 }
		 else
		 {
			 // str为操作符
			 int right = s.top();
			 s.pop();
			 
			 int left = s.top();
			 s.pop();
			 
			 switch (str[0])
			 {
				 case '+':
					 s.push(left + right);
					 break;
					 
				 case '-':
					 s.push(left - right);
					 break;
				 
				 case '*':
					 s.push(left * right);
					 break;
				 
				 case '/':
					 // 题目说明了不存在除数为0的情况
					 s.push(left / right);
					 break;
			 }
		 }
	 }
 return s.top();
 }
};
```

