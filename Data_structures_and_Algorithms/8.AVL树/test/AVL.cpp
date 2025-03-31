#pragma once
#include <iostream>
#include <assert.h>
using namespace std;

template <class K, class V>
struct AVLTreeNode
{
    pair<K, V> _kv;
    AVLTreeNode<K, V> *_left;
    AVLTreeNode<K, V> *_right;
    AVLTreeNode<K, V> *_parent;
    int _bf; // balance factor

    AVLTreeNode(const pair<K, V> &kv)
        : _kv(kv), _left(nullptr), _right(nullptr), _parent(nullptr), _bf(0)
    {
    }
};

template <class K, class V>
class AVLTree
{
    typedef AVLTreeNode<K, V> Node;

public:
    AVLTree() = default;

    AVLTree(const AVLTree<K, V> &t)
    {
        _root = Copy(t._root);
    }

    AVLTree<K, V> &operator=(AVLTree<K, V> t)
    {
        swap(_root, t._root);
        return *this;
    }

    ~AVLTree()
    {
        Destroy(_root);
        _root = nullptr;
    }

    bool Insert(const pair<K, V> &kv)
    {
        if (_root == nullptr)
        {
            _root = new Node(kv);
            return true;
        }

        Node *parent = nullptr;
        Node *cur = _root;
        while (cur)
        {
            if (cur->_kv.first < kv.first)
            {
                parent = cur;
                cur = cur->_right;
            }
            else if (cur->_kv.first > kv.first)
            {
                parent = cur;
                cur = cur->_left;
            }
            else
            {
                return false;
            }
        }

        cur = new Node(kv);
        if (parent->_kv.first < kv.first)
        {
            parent->_right = cur;
        }
        else
        {
            parent->_left = cur;
        }
        cur->_parent = parent;

        // 更新平衡因子
        while (parent)
        {
            if (cur == parent->_left)
                parent->_bf--;
            else
                parent->_bf++;

            if (parent->_bf == 0)
            {
                break;
            }
            else if (parent->_bf == 1 || parent->_bf == -1)
            {
                // 继续往上更新
                cur = parent;
                parent = parent->_parent;
            }
            else if (parent->_bf == 2 || parent->_bf == -2)
            {
                // 不平衡了，旋转处理
                if (parent->_bf == 2 && cur->_bf == 1)
                {
                    RotateL(parent);
                }
                else if (parent->_bf == -2 && cur->_bf == -1)
                {
                    RotateR(parent);
                }
                else if (parent->_bf == 2 && cur->_bf == -1)
                {
                    RotateRL(parent);
                }
                else
                {
                    RotateLR(parent);
                }

                break;
            }
            else
            {
                assert(false);
            }
        }

        return true;
    }

    Node *Find(const K &key)
    {
        Node *cur = _root;
        while (cur)
        {
            if (cur->_key < key)
            {
                cur = cur->_right;
            }
            else if (cur->_key > key)
            {
                cur = cur->_left;
            }
            else
            {
                return cur;
            }
        }

        return nullptr;
    }

    void InOrder()
    {
        _InOrder(_root);
        cout << endl;
    }

private:
    void _InOrder(Node *root)
    {
        if (root == nullptr)
        {
            return;
        }

        _InOrder(root->_left);
        cout << root->_kv.first << ":" << root->_kv.second << endl;
        _InOrder(root->_right);
    }

    void RotateL(Node *parent)
    {
        Node *subR = parent->_right;
        Node *subRL = subR->_left;

        parent->_right = subRL;
        if (subRL)
            subRL->_parent = parent;

        Node *parentParent = parent->_parent;

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

        parent->_bf = subR->_bf = 0;
    }

    void RotateR(Node *parent)
    {
        Node *subL = parent->_left;
        Node *subLR = subL->_right;

        parent->_left = subLR;
        if (subLR)
            subLR->_parent = parent;

        Node *parentParent = parent->_parent;

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

    void RotateRL(Node *parent)
    {
        Node *subR = parent->_right;
        Node *subRL = subR->_left;
        int bf = subRL->_bf;

        RotateR(parent->_right);
        RotateL(parent);

        if (bf == 0)
        {
            subR->_bf = 0;
            subRL->_bf = 0;
            parent->_bf = 0;
        }
        else if (bf == 1)
        {
            subR->_bf = 0;
            subRL->_bf = 0;
            parent->_bf = -1;
        }
        else if (bf == -1)
        {
            subR->_bf = 1;
            subRL->_bf = 0;
            parent->_bf = 0;
        }
        else
        {
            assert(false);
        }
    }

    void RotateLR(Node *parent)
    {
        Node *subL = parent->_left;
        Node *subLR = subL->_right;
        int bf = subLR->_bf;

        RotateL(parent->_left);
        RotateR(parent);

        if (bf == 0)
        {
            subL->_bf = 0;
            subLR->_bf = 0;
            parent->_bf = 0;
        }
        else if (bf == -1)
        {
            subL->_bf = 0;
            subLR->_bf = 0;
            parent->_bf = 1;
        }
        else if (bf == 1)
        {
            subL->_bf = -1;
            subLR->_bf = 0;
            parent->_bf = 0;
        }
        else
        {
            assert(false);
        }
    }
    void Destroy(Node *root)
    {
        if (root == nullptr)
            return;

        Destroy(root->_left);
        Destroy(root->_right);
        delete root;
    }

    Node *Copy(Node *root)
    {
        if (root == nullptr)
            return nullptr;

        Node *newRoot = new Node(root->_kv);
        newRoot->_left = Copy(root->_left);
        newRoot->_right = Copy(root->_right);

        return newRoot;
    }

private:
    Node *_root = nullptr;
};

void TestAVLTree()
{
    AVLTree<int, int> t;
    int a[] = {16, 3, 7, 11, 9, 26, 18, 14, 15};
    for (auto e : a)
    {
        t.Insert({e, e});
    }

    t.InOrder();

    AVLTree<int, int> t2(t);
    t2.InOrder();
}

int main()
{
    TestAVLTree();
    return 0;
}