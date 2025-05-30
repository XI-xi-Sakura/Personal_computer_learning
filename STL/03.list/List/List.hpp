#pragma once
#include <assert.h>
#include <iostream>
using namespace std;

namespace bit
{
	template <class T>
	struct ListNode
	{
		ListNode<T> *_next;
		ListNode<T> *_prev;

		T _data;

		ListNode(const T &data = T()) // 使用 T() 调用模板类型 T 的默认构造函数来创建一个默认值。
			: _next(nullptr),		  // 这意味着如果在创建 ListNode 对象时没有提供数据，将使用 T 类型的默认值进行初始化。
			  _prev(nullptr),
			  _data(data)
		{
		}
	};

	/*
	List 的迭代器
	迭代器有两种实现方式，具体应根据容器底层数据结构实现：
	  1. 原生态指针，比如：vector
	  2. 将原生态指针进行封装，因迭代器使用形式与指针完全相同，因此在自定义的类中必须实现以下方法：
		 1. 指针可以解引用，迭代器的类中必须重载operator*()
		 2. 指针可以通过->访问其所指空间成员，迭代器类中必须重载oprator->()
		 3. 指针可以++向后移动，迭代器类中必须重载operator++()与operator++(int)
			至于operator--()/operator--(int)释放需要重载，根据具体的结构来抉择，双向链表可以向前移动，所以需要重载，如果是forward_list就不需要重载--
		 4. 迭代器需要进行是否相等的比较，因此还需要重载operator==()与operator!=()
	*/

	template <class T, class Ref, class Ptr>
	struct ListIterator
	{
		typedef ListNode<T> Node;
		typedef ListIterator<T, Ref, Ptr> Self; // typedef ListIterator<T, T &, T *> iterator;
												// typedef ListIterator<T, const T &, const T *> const_iterator;

		ListIterator(Node *node)
			: _node(node)
		{
		}

		// ++it;
		Self &operator++()
		{
			_node = _node->_next;
			return *this;
		}

		Self &operator--()
		{
			_node = _node->_prev;
			return *this;
		}

		Self operator++(int)
		{
			Self tmp(*this);
			_node = _node->_next;

			return tmp;
		}

		Self operator--(int)
		{
			Self tmp(*this);
			_node = _node->_prev;

			return tmp;
		}

		Ref operator*()
		{
			return _node->_data;
		}

		Ptr operator->()
		{
			return &_node->_data;
		}

		bool operator!=(const Self &it)
		{
			return _node != it._node;
		}

		bool operator==(const Self &it)
		{
			return _node == it._node;
		}
		Node *_node;
	};

	template <class Iterator>
	class ReverseListIterator
	{
		// 注意：此处typename的作用是明确告诉编译器，Ref是Iterator类中的一个类型，而不是静态成员变量
		// 否则编译器编译时就不知道Ref是Iterator中的类型还是静态成员变量
		// 因为静态成员变量也是按照 类名::静态成员变量名 的方式访问的
	public:
		typedef typename Iterator::Ref Ref;
		typedef typename Iterator::Ptr Ptr;
		typedef ReverseListIterator<Iterator> Self;

	public:
		//////////////////////////////////////////////
		// 构造
		ReverseListIterator(Iterator it)
			: _it(it)
		{
		}

		//////////////////////////////////////////////
		// 具有指针类似行为
		Ref operator*()
		{
			Iterator temp(_it);
			--temp;
			return *temp;
		}

		Ptr operator->()
		{
			return &(operator*());
		}

		//////////////////////////////////////////////
		// 迭代器支持移动
		Self &operator++()
		{
			--_it;
			return *this;
		}

		Self operator++(int)
		{
			Self temp(*this);
			--_it;
			return temp;
		}

		Self &operator--()
		{
			++_it;
			return *this;
		}

		Self operator--(int)
		{
			Self temp(*this);
			++_it;
			return temp;
		}

		//////////////////////////////////////////////
		// 迭代器支持比较
		bool operator!=(const Self &l) const
		{
			return _it != l._it;
		}

		bool operator==(const Self &l) const
		{
			return _it != l._it;
		}

		Iterator _it;
	};

	// template<class T>
	// class ListConstIterator
	//{
	//	typedef ListNode<T> Node;
	//	typedef ListConstIterator<T> Self;

	//	Node* _node;
	// public:
	//	ListConstIterator(Node* node)
	//		:_node(node)
	//	{}

	//	// ++it;
	//	Self& operator++()
	//	{
	//		_node = _node->_next;
	//		return *this;
	//	}

	//	Self& operator--()
	//	{
	//		_node = _node->_prev;
	//		return *this;
	//	}

	//	Self operator++(int)
	//	{
	//		Self tmp(*this);
	//		_node = _node->_next;

	//		return tmp;
	//	}

	//	Self& operator--(int)
	//	{
	//		Self tmp(*this);
	//		_node = _node->_prev;

	//		return tmp;
	//	}

	//	//*it
	//	const T& operator*()
	//	{
	//		return _node->_data;
	//	}

	//	const T* operator->()
	//	{
	//		return &_node->_data;
	//	}

	//	bool operator!=(const Self& it)
	//	{
	//		return _node != it._node;
	//	}

	//	bool operator==(const Self& it)
	//	{
	//		return _node == it._node;
	//	}
	//};

	template <class T>
	class list
	{
		typedef ListNode<T> Node;

	public:
		// 不符合迭代器的行为，无法遍历
		// typedef Node* iterator;
		// typedef ListIterator<T> iterator;
		// typedef ListConstIterator<T> const_iterator;

		typedef ListIterator<T, T &, T *> iterator;
		typedef ListIterator<T, const T &, const T *> const_iterator;

		typedef ReverseListIterator<iterator> reverse_iterator;
		typedef ReverseListIterator<const_iterator> const_reverse_iterator;

		iterator begin()
		{
			// iterator it(_head->_next);
			// return it;
			return iterator(_head->_next);
		}

		const_iterator begin() const
		{
			return const_iterator(_head->_next);
		}

		iterator end()
		{
			return iterator(_head);
		}

		const_iterator end() const
		{
			return const_iterator(_head);
		}

		reverse_iterator rbegin()
		{
			return reverse_iterator(end());
		}

		reverse_iterator rend()
		{
			return reverse_iterator(begin());
		}

		const_reverse_iterator rbegin() const
		{
			return const_reverse_iterator(end());
		}

		const_reverse_iterator rend() const
		{
			return const_reverse_iterator(begin());
		}

		void empty_init()
		{
			_head = new Node();
			_head->_next = _head;
			_head->_prev = _head;
		}

		list()
		{
			empty_init();
		}

		list(initializer_list<T> il)
		{
			empty_init();

			for (const auto &e : il)
			{
				push_back(e);
			}
		}

		// lt2(lt1)
		list(const list<T> &lt)
		{
			empty_init();

			for (const auto &e : lt)
			{
				push_back(e);
			}
		}

		// lt1 = lt3
		list<T> &operator=(list<T> lt)
		{
			swap(_head, lt._head);

			return *this;
		}

		~list()
		{
			clear();
			delete _head;
			_head = nullptr;
		}

		void clear()
		{
			auto it = begin();
			while (it != end())
			{
				it = erase(it);
			}
		}

		void push_back(const T &x)
		{
			/*Node* newnode = new Node(x);
			Node* tail = _head->_prev;

			tail->_next = newnode;
			newnode->_prev = tail;
			newnode->_next = _head;
			_head->_prev = newnode;*/

			insert(end(), x);
		}

		void pop_back()
		{
			erase(--end());
		}

		void push_front(const T &x)
		{
			insert(begin(), x);
		}

		void pop_front()
		{
			erase(begin());
		}

		// 没有iterator失效
		iterator insert(iterator pos, const T &x)
		{
			Node *cur = pos._node;
			Node *newnode = new Node(x);
			Node *prev = cur->_prev;

			// prev  newnode  cur
			prev->_next = newnode;
			newnode->_prev = prev;
			newnode->_next = cur;
			cur->_prev = newnode;

			return iterator(newnode);
		}

		// erase 后 pos失效了，pos指向节点被释放了
		iterator erase(iterator pos)
		{
			assert(pos != end());

			Node *cur = pos._node;
			Node *prev = cur->_prev;
			Node *next = cur->_next;

			prev->_next = next;
			next->_prev = prev;

			delete cur;

			return iterator(next);
		}

	private:
		Node *_head;
	};

	void Func(const list<int> &lt)
	{
		// const iterator const 迭代器不能普通迭代器前面加const修饰
		// const 迭代器目标本身可以修改，指向的内容不能修改 类似const T* p
		list<int>::const_iterator it = lt.begin();
		while (it != lt.end())
		{
			// 指向的内容不能修改
			//*it += 10;

			cout << *it << " ";
			++it;
		}
		cout << endl;
	}

	void test_list1()
	{
		list<int> lt1;

		// 按需实例化（不调用就不实例化这个成员函数）
		lt1.push_back(1);
		lt1.push_back(2);
		lt1.push_back(3);
		lt1.push_back(4);
		lt1.push_back(5);

		Func(lt1);

		// ListIterator<int> it = lt1.begin();
		list<int>::iterator it = lt1.begin();
		while (it != lt1.end())
		{
			*it += 10;

			cout << *it << " ";
			++it;
		}
		cout << endl;

		for (auto e : lt1)
		{
			cout << e << " ";
		}
		cout << endl;
	}

	struct Pos
	{
		int _row;
		int _col;

		Pos(int row = 0, int col = 0)
			: _row(row), _col(col)
		{
		}
	};

	void test_list2()
	{
		list<Pos> lt1;
		lt1.push_back(Pos(100, 100));
		lt1.push_back(Pos(200, 200));
		lt1.push_back(Pos(300, 300));

		list<Pos>::iterator it = lt1.begin();
		while (it != lt1.end())
		{
			// cout << (*it)._row << ":" << (*it)._col << endl;
			//  为了可读性，省略了一个->
			cout << it->_row << ":" << it->_col << endl;
			// cout << it->->_row << ":" << it->->_col << endl;
			cout << it.operator->()->_row << ":" << it.operator->()->_col << endl;

			++it;
		}
		cout << endl;
	}

	void test_list4()
	{
		list<int> lt1;
		lt1.push_back(1);
		lt1.push_back(2);
		lt1.push_back(3);
		lt1.push_back(4);
		lt1.push_back(5);

		Func(lt1);

		lt1.push_front(10);
		lt1.push_front(20);
		lt1.push_front(30);

		Func(lt1);

		lt1.pop_front();
		lt1.pop_front();
		Func(lt1);

		lt1.pop_back();
		lt1.pop_back();
		Func(lt1);

		lt1.pop_back();
		lt1.pop_back();
		lt1.pop_back();
		lt1.pop_back();
		// lt1.pop_back();
		Func(lt1);
	}

	void test_list5()
	{
		list<int> lt1;
		lt1.push_back(1);
		lt1.push_back(2);
		lt1.push_back(3);
		lt1.push_back(4);
		lt1.push_back(5);
		Func(lt1);

		list<int> lt2(lt1);

		lt1.push_back(6);

		Func(lt1);
		Func(lt2);

		list<int> lt3;
		lt3.push_back(10);
		lt3.push_back(20);
		lt3.push_back(30);

		lt1 = lt3;
		Func(lt1);
		Func(lt3);
	}

	void test_list6()
	{
		list<int> lt1 = {1, 2, 3, 4, 5, 6};
		Func(lt1);
	}
}