#pragma once
#include<iostream>
#include<vector>
#include<assert.h>
using namespace std;

enum Colour
{
	RED,
	BLACK
};

template<class T>
struct RBTreeNode
{
	T _data;

	RBTreeNode<T>* _left;
	RBTreeNode<T>* _right;
	RBTreeNode<T>* _parent;
	Colour _col;

	RBTreeNode(const T& data)
		: _data(data)
		, _left(nullptr)
		, _right(nullptr)
		, _parent(nullptr)
	{}
};

template<class T, class Ref, class Ptr>
struct RBTreeIterator
{
	typedef RBTreeNode<T> Node;
	typedef RBTreeIterator<T, Ref, Ptr> Self;

	Node* _node;
	Node* _root;

	RBTreeIterator(Node* node, Node* root)
		:_node(node)
		,_root(root)
	{}

	Self& operator++()
	{
		if (_node->_right)
		{
			// 右不为空，右子树最左节点就是中序第一个
			Node* leftMost = _node->_right;
			while (leftMost->_left)
			{
				leftMost = leftMost->_left;
			}

			_node = leftMost;
		}
		else
		{
			// 孩子是父亲左的那个祖先
			Node* cur = _node;
			Node* parent = cur->_parent;
			while (parent && cur == parent->_right)
			{
				cur = parent;
				parent = cur->_parent;
			}

			_node = parent;
		}

		return *this;
	}

	Self& operator--()
	{
		if (_node == nullptr) // end()
		{
			// --end()，特殊处理，走到中序最后一个节点，整棵树的最右节点
			Node* rightMost = _root;
			while (rightMost && rightMost->_right)
			{
				rightMost = rightMost->_right;
			}

			_node = rightMost;
		}
		else if (_node->_left)
		{
			// 左子树不为空，中序左子树最后一个
			Node* rightMost = _node->_left;
			while (rightMost->_right)
			{
				rightMost = rightMost->_right;
			}

			_node = rightMost;
		}
		else
		{
			// 孩子是父亲右的那个祖先
			Node* cur = _node;
			Node* parent = cur->_parent;
			while (parent && cur == parent->_left)
			{
				cur = parent;
				parent = cur->_parent;
			}

			_node = parent;

		}

		return *this;
	}

	Ref operator*()
	{
		return _node->_data;
	}

	Ptr operator->()
	{
		return &_node->_data;
	}

	bool operator!= (const Self& s)
	{
		return _node != s._node;
	}

	bool operator== (const Self& s)
	{
		return _node == s._node;
	}
};
//因为RBTree实现了泛型不知道T参数导致是K，还是pair<K, V>，那么insert内部进⾏插⼊逻辑⽐较时，
//就没办法进⾏⽐较，因为pair的默认⽀持的是key和value⼀起参与⽐较，我们需要时的任何时候只⽐较key，
//所以我们在map和set层分别实现⼀个MapKeyOfT和SetKeyOfT的仿函数传给RBTree的KeyOfT，
//然后RBTree中通过KeyOfT仿函数取出T类型对象中的key，再进⾏⽐较
//key参数就用K，value参数就⽤V，红⿊树中的数据类型，我们使⽤T。
//⽤K⽤来实例化KeyOfT，KeyOfT是⼀个仿函数，⽤来获取T中的key。
template<class K, class T, class KeyOfT>
class RBTree
{
	typedef RBTreeNode<T> Node;
public:
	typedef RBTreeIterator<T, T&, T*> Iterator;
	typedef RBTreeIterator<T, const T&, const T*> ConstIterator;



	Iterator Begin()
	{
		Node* leftMost = _root;
		while (leftMost && leftMost->_left)
		{
			leftMost = leftMost->_left;
		}

		return Iterator(leftMost, _root);
	}

	Iterator End()
	{
		return Iterator(nullptr, _root);
	}

	ConstIterator Begin() const
	{
		Node* leftMost = _root;
		while (leftMost && leftMost->_left)
		{
			leftMost = leftMost->_left;
		}

		return ConstIterator(leftMost, _root);
	}

	ConstIterator End() const
	{
		return ConstIterator(nullptr, _root);
	}

	RBTree() = default;

	RBTree(const RBTree& t)
	{
		_root = Copy(t._root);
	}

	RBTree& operator=(RBTree t)
	{
		swap(_root, t._root);
		return *this;
	}

	~RBTree()
	{
		Destroy(_root);
		_root = nullptr;
	}

	pair<Iterator, bool> Insert(const T& data)
	{
		if (_root == nullptr)
		{
			_root = new Node(data);
			_root->_col = BLACK;
			return make_pair(Iterator(_root, _root), true);
		}

		KeyOfT kot;
		Node* parent = nullptr;
		Node* cur = _root;
		while (cur)
		{
			if (kot(cur->_data) < kot(data))
			{
				parent = cur;
				cur = cur->_right;
			}
			else if (kot(cur->_data) > kot(data))
			{
				parent = cur;
				cur = cur->_left;
			}
			else
			{
				return make_pair(Iterator(cur, _root), false);
			}
		}

		cur = new Node(data);
		Node* newnode = cur;

		// 新增节点。颜色红色给红色
		cur->_col = RED;
		if (kot(parent->_data) < kot(data))
		{
			parent->_right = cur;
		}
		else
		{
			parent->_left = cur;
		}
		cur->_parent = parent;

		while (parent && parent->_col == RED)
		{
			Node* grandfather = parent->_parent;
			//    g
			//  p   u
			if (parent == grandfather->_left)
			{
				Node* uncle = grandfather->_right;
				if (uncle && uncle->_col == RED)
				{
					// u存在且为红 -》变色再继续往上处理
					parent->_col = uncle->_col = BLACK;
					grandfather->_col = RED;

					cur = grandfather;
					parent = cur->_parent;
				}
				else
				{
					// u存在且为黑或不存在 -》旋转+变色
					if (cur == parent->_left)
					{
						//    g
						//  p   u
						//c
						//单旋
						RotateR(grandfather);
						parent->_col = BLACK;
						grandfather->_col = RED;
					}
					else
					{
						//    g
						//  p   u
						//    c
						//双旋
						RotateL(parent);
						RotateR(grandfather);

						cur->_col = BLACK;
						grandfather->_col = RED;
					}

					break;
				}
			}
			else
			{
				//    g
				//  u   p
				Node* uncle = grandfather->_left;
				// 叔叔存在且为红，-》变色即可
				if (uncle && uncle->_col == RED)
				{
					parent->_col = uncle->_col = BLACK;
					grandfather->_col = RED;

					// 继续往上处理
					cur = grandfather;
					parent = cur->_parent;
				}
				else // 叔叔不存在，或者存在且为黑
				{
					// 情况二：叔叔不存在或者存在且为黑
					// 旋转+变色
					//      g
					//   u     p
					//            c
					if (cur == parent->_right)
					{
						RotateL(grandfather);
						parent->_col = BLACK;
						grandfather->_col = RED;
					}
					else
					{
						//		g
						//   u     p
						//      c
						RotateR(parent);
						RotateL(grandfather);
						cur->_col = BLACK;
						grandfather->_col = RED;
					}
					break;
				}				
			}
		}

		_root->_col = BLACK;

		return make_pair(Iterator(newnode, _root), true);
	}

	void InOrder()
	{
		_InOrder(_root);
		cout << endl;
	}

	int Height()
	{
		return _Height(_root);
	}

	int Size()
	{
		return _Size(_root);
	}

	bool IsBalance()
	{
		if (_root == nullptr)
			return true;

		if (_root->_col == RED)
		{
			return false;
		}
		
		// 参考值
		int refNum = 0;
		Node* cur = _root;
		while (cur)
		{
			if (cur->_col == BLACK)
			{
				++refNum;
			}

			cur = cur->_left;
		}

		return Check(_root, 0, refNum);
	}

	Iterator Find(const K& key)
	{
		Node* cur = _root;
		while (cur)
		{
			if (cur->_kv.first < key)
			{
				cur = cur->_right;
			}
			else if (cur->_kv.first > key)
			{
				cur = cur->_left;
			}
			else
			{
				return Iterator(cur, _root);
			}
		}

		return End();
	}

private:
	bool Check(Node* root, int blackNum, const int refNum)
	{
		if (root == nullptr)
		{
			//cout << blackNum << endl;
			if (refNum != blackNum)
			{
				cout << "存在黑色节点的数量不相等的路径" << endl;
				return false;
			}

			return true;
		}

		if (root->_col == RED && root->_parent->_col == RED)
		{
			cout << root->_kv.first << "存在连续的红色节点" << endl;
			return false;
		}

		if (root->_col == BLACK)
		{
			blackNum++;
		}

		return Check(root->_left, blackNum, refNum)
			&& Check(root->_right, blackNum, refNum);
	}

	int _Size(Node* root)
	{
		return root == nullptr ? 0 : _Size(root->_left) + _Size(root->_right) + 1;
	}

	int _Height(Node* root)
	{
		if (root == nullptr)
			return 0;

		int leftHeight = _Height(root->_left);
		int rightHeight = _Height(root->_right);

		return leftHeight > rightHeight ? leftHeight + 1 : rightHeight + 1;
	}

	void _InOrder(Node* root)
	{
		if (root == nullptr)
		{
			return;
		}

		_InOrder(root->_left);
		cout << root->_kv.first << ":" << root->_kv.second << endl;
		_InOrder(root->_right);
	}

	void RotateL(Node* parent)
	{
		_rotateNum++;
		Node* subR = parent->_right;
		Node* subRL = subR->_left;

		parent->_right = subRL;
		if (subRL)
			subRL->_parent = parent;

		Node* parentParent = parent->_parent;

		subR->_left = parent;
		parent->_parent = subR;

		if (parentParent == nullptr)
		{
			_root = subR;
			subR->_parent = nullptr;
		}
		else
		{
			if (parent == parentParent->_left)
			{
				parentParent->_left = subR;
			}
			else
			{
				parentParent->_right = subR;
			}

			subR->_parent = parentParent;
		}
	}

	void  RotateR(Node* parent)
	{
		_rotateNum++;

		Node* subL = parent->_left;
		Node* subLR = subL->_right;

		parent->_left = subLR;
		if (subLR)
			subLR->_parent = parent;

		Node* parentParent = parent->_parent;

		subL->_right = parent;
		parent->_parent = subL;

		if (parentParent == nullptr)
		{
			_root = subL;
			subL->_parent = nullptr;
		}
		else
		{
			if (parent == parentParent->_left)
			{
				parentParent->_left = subL;
			}
			else
			{
				parentParent->_right = subL;
			}

			subL->_parent = parentParent;
		}

	}

	void Destroy(Node* root)
	{
		if (root == nullptr)
			return;

		Destroy(root->_left);
		Destroy(root->_right);
		delete root;
	}

	Node* Copy(Node* root)
	{
		if (root == nullptr)
			return nullptr;

		Node* newRoot = new Node(root->_kv);
		newRoot->_left = Copy(root->_left);
		newRoot->_right = Copy(root->_right);

		return newRoot;
	}

private:
	Node* _root = nullptr;
public:
	int _rotateNum = 0;
};

//void TestRBTree1()
//{
//	RBTree<int, int> t;
//	//int a[] = { 16, 3, 7, 11, 9, 26, 18, 14, 15 };
//	int a[] = { 4, 2, 6, 1, 3, 5, 15, 7, 16, 14 };
//	for (auto e : a)
//	{
//		/*if (e == 9)
//		{
//			int i = 0;
//		}*/
//
//		t.Insert({ e, e });
//
//		//cout << e << "->" << t.IsBalanceTree() << endl;
//	}
//
//	t.InOrder();
//	cout << t.IsBalance() << endl;
//}