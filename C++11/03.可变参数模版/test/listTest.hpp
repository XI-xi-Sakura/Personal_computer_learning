#include <iostream>
#include <string>
using namespace std;

namespace bit
{
    template <class T>
    struct ListNode
    {
        ListNode<T> *_next;
        ListNode<T> *_prev;
        T _data;
        ListNode(T &&data)
            : _next(nullptr), _prev(nullptr), _data(std::forward<T>(data)) {}

        template <class... Args>
        ListNode(Args &&...args)
            : _next(nullptr), _prev(nullptr), _data(std::forward<Args>(args)...) {}
    };
    template <class T, class Ref, class Ptr>
    struct ListIterator
    {
        typedef ListNode<T> Node;
        typedef ListIterator<T, Ref, Ptr> Self;
        Node *_node;
        ListIterator(Node *node)
            : _node(node) {}
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
        Ref operator*()
        {
            return _node->_data;
        }
        bool operator!=(const Self &it)
        {
            return _node != it._node;
        }
    };
    template <class T>
    class List
    {
        typedef ListNode<T> Node;

    public:
        typedef ListIterator<T, T &, T> iterator;
        typedef ListIterator<T, const T &, const T> const_iterator;
        iterator begin()
        {
            return iterator(_head->_next);
        }
        iterator end()
        {
            return iterator(_head);
        }
        void empty_init()
        {
            _head = new Node();
            _head->_next = _head;
            _head->_prev = _head;
        }
        List()
        {
            empty_init();
        }

        void push_back(T &&x)
        {
            insert(end(), std::forward<T>(x));
        }

        iterator insert(iterator pos, T &&x)
        {
            Node *cur = pos._node;
            Node *newnode = new Node(std::forward<T>(x));
            Node *prev = cur->_prev;

            // prev newnode cur
            prev->_next = newnode;
            newnode->_prev = prev;
            newnode->_next = cur;
            cur->_prev = newnode;

            return iterator(newnode);
        }

        template <class... Args>
        void emplace_back(Args &&...args)
        {
            insert(end(), std::forward<Args>(args)...);
        }

        template <class... Args>
        iterator insert(iterator pos, Args &&...args)
        {
            Node *cur = pos._node;
            Node *newnode = new Node(std::forward<Args>(args)...);
            Node *prev = cur->_prev;

            // prev newnode cur
            prev->_next = newnode;
            newnode->_prev = prev;
            newnode->_next = cur;
            cur->_prev = newnode;
            return iterator(newnode);
        }

    private:
        Node *_head;
    };
}