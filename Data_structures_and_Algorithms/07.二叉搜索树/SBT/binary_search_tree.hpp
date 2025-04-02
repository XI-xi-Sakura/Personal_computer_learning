template <class K>
struct BSTNode
{
    K _key;
    BSTNode<K> *_left;
    BSTNode<K> *_right;

    BSTNode(const K &key)
        : _key(key), _left(nullptr), _right(nullptr)
    {
    }
};

// Binary Search Tree
template <class K>
class BSTree
{
    typedef BSTNode<K> Node;

public:
    bool Insert(const K &key)
    {
        if (_root == nullptr)
        {
            _root = new Node(key);
            return true;
        }
        Node *parent = nullptr;
        Node *cur = _root;
        while (cur)
        {
            if (cur->_key < key)
            {
                parent = cur;
                cur = cur->_right;
            }
            else if (cur->_key > key)
            {
                parent = cur;
                cur = cur->_left;
            }
            else
            {
                return false;
            }
        }
        cur = new Node(key);
        if (parent->_key < key)
        {
            parent->_right = cur;
        }
        else
        {
            parent->_left = cur;
        }
        return true;
    }

    bool Find(const K &key)
    {
        Node cur = _root;
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
                return true;
            }
        }
        return false;
    }

    bool Erase(const K &key)
    {
        Node *parent = nullptr;
        Node *cur = _root;
        while (cur)
        {
            if (cur->_key < key)
            {
                parent = cur;
                cur = cur->_right;
            }
            else if (cur->_key > key)
            {
                parent = cur;
                cur = cur->_left;
            }
            else
            {
                // 0-1个孩子的情况
                // 删除情况123均可以直接删除,改变父亲对应孩子指针指向即可
                if (cur->_left == nullptr)
                {
                    if (parent == nullptr)
                    {
                        _root = cur->_right;
                    }
                    else
                    {
                        if (parent->_left == cur)
                            parent->_left = cur->_right;
                        else
                            parent->_right = cur->_right;
                    }
                    delete cur;
                    return true;
                }
                else if (cur->_right == nullptr)
                {
                    if (parent == nullptr)
                    {
                        _root = cur->_left;
                    }
                    else
                    {
                        if (parent->_left == cur)
                            parent->_left = cur->_left;
                        else
                            parent->_right = cur->_left;
                    }
                    delete cur;
                    return true;
                }
                else
                {
                    // 2个孩子的情况
                    // 删除情况4,替换法删除
                    // 假设这里我们取右子树的最小结点作为替代结点去删除
                    // 这里尤其要注意右子树的根就是最小情况的情况的处理,对应课件图中删除8的情况
                    // 一定要把cur给rightMinP,否会报错｡
                    Node *rightMinP = cur;
                    Node *rightMin = cur->_right;
                    while (rightMin->_left)
                    {
                        rightMinP = rightMin;
                        rightMin = rightMin->_left;
                    }

                    cur->_key = rightMin->_key;

                    if (rightMinP->_left == rightMin)   // 这里一定要判断,否则会报错
                        rightMinP->_left = rightMin->_right;
                    else
                        rightMinP->_right = rightMin->_right;
                        
                    delete rightMin;
                    return true;
                }
            }
        }
        return false;
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
        cout << root->_key << " ";
        _InOrder(root->_right);
    }

private:
    Node *_root = nullptr;
};