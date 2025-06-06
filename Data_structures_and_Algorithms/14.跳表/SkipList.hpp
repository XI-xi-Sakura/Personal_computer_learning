#include <iostream>
#include <vector>
#include <time.h>
#include <random>
#include <chrono>
using namespace std;

struct SkiplistNode
{
    int _val;
    vector<SkiplistNode *> _nextV;

    SkiplistNode(int val, int level)
        : _val(val), _nextV(level, nullptr)
    {
    }
};

class Skiplist
{
    typedef SkiplistNode Node;

public:
    Skiplist()
    {
        srand(time(0));

        // 头节点，层数是1
        _head = new SkiplistNode(-1, 1);
    }

    bool search(int target)
    {
        Node *cur = _head;
        int level = _head->_nextV.size() - 1;
        while (level >= 0)
        {
            // 目标值比下一个节点值要大，向右走
            // 下一个节点是空(尾)，目标值比下一个节点值要小，向下走
            if (cur->_nextV[level] && cur->_nextV[level]->_val < target)
            {
                // 向右走
                cur = cur->_nextV[level];
            }
            else if (cur->_nextV[level] == nullptr || cur->_nextV[level]->_val > target)
            {
                // 向下走
                --level;
            }
            else
            {
                return true;
            }
        }

        return false;
    }

    vector<Node *> FindPrevNode(int num)
    {
        Node *cur = _head;
        int level = _head->_nextV.size() - 1;

        // 插入位置每一层前一个节点指针
        vector<Node *> prevV(level + 1, _head);

        while (level >= 0)
        {
            // 目标值比下一个节点值要大，向右走
            // 下一个节点是空(尾)，目标值比下一个节点值要小，向下走
            if (cur->_nextV[level] && cur->_nextV[level]->_val < num)
            {
                // 向右走
                cur = cur->_nextV[level];
            }
            else if (cur->_nextV[level] == nullptr || cur->_nextV[level]->_val >= num)
            {
                // 更新level层前一个
                prevV[level] = cur;

                // 向下走
                --level;
            }
        }

        return prevV;
    }

    void add(int num)
    {
        vector<Node *> prevV = FindPrevNode(num);

        int n = RandomLevel();
        Node *newnode = new Node(num, n);

        // 如果n超过当前最大的层数，那就升高一下_head的层数
        if (n > _head->_nextV.size())
        {
            _head->_nextV.resize(n, nullptr);
            prevV.resize(n, _head);
        }

        // 链接前后节点
        for (size_t i = 0; i < n; ++i)
        {
            newnode->_nextV[i] = prevV[i]->_nextV[i];
            prevV[i]->_nextV[i] = newnode;
        }
    }

    bool erase(int num)
    {
        vector<Node *> prevV = FindPrevNode(num);

        // 第一层下一个不是val，val不在表中
        if (prevV[0]->_nextV[0] == nullptr || prevV[0]->_nextV[0]->_val != num)
        {
            return false;
        }
        else
        {
            Node *del = prevV[0]->_nextV[0];
            // del节点每一层的前后指针链接起来
            for (size_t i = 0; i < del->_nextV.size(); i++)
            {
                prevV[i]->_nextV[i] = del->_nextV[i];
            }
            delete del;

            // 如果删除最高层节点，把头节点的层数也降一下
            int i = _head->_nextV.size() - 1;
            while (i >= 0)
            {
                if (_head->_nextV[i] == nullptr)
                    --i;
                else
                    break;
            }
            _head->_nextV.resize(i + 1);

            return true;
        }
    }

    // int RandomLevel()
    //{
    //	size_t level = 1;
    //	// rand() ->[0, RAND_MAX]之间
    //	while (rand() <= RAND_MAX*_p && level < _maxLevel)
    //	{
    //		++level;
    //	}

    //	return level;
    //}

    int RandomLevel()
    {
        static std::default_random_engine generator(std::chrono::system_clock::now().time_since_epoch().count());
        static std::uniform_real_distribution<double> distribution(0.0, 1.0);

        size_t level = 1;
        while (distribution(generator) <= _p && level < _maxLevel)
        {
            ++level;
        }

        return level;
    }

    void Print()
    {
        /*int level = _head->_nextV.size();
        for (int i = level - 1; i >= 0; --i)
        {
            Node* cur = _head;
            while (cur)
            {
                printf("%d->", cur->_val);
                cur = cur->_nextV[i];
            }
            printf("\n");
        }*/

        Node *cur = _head;
        while (cur)
        {
            printf("%2d\n", cur->_val);
            // 打印每个每个cur节点
            for (auto e : cur->_nextV)
            {
                printf("%2s", "↓");
            }
            printf("\n");

            cur = cur->_nextV[0];
        }
    }

private:
    Node *_head;
    size_t _maxLevel = 32;
    double _p = 0.5;
};

/**
 * Your Skiplist object will be instantiated and called as such:
 * Skiplist* obj = new Skiplist();
 * bool param_1 = obj->search(target);
 * obj->add(num);
 * bool param_3 = obj->erase(num);
 */

// int main()
//{
//	Skiplist sl;
//	int max = 0;
//	for (size_t i = 0; i < 1000000000; ++i)
//	{
//		//cout << sl.RandomLevel() <<" ";
//		int r = sl.RandomLevel();
//		if (r > max)
//			max = r;
//	}
//	cout <<max<< endl;
//
//	return 0;
// }

int main()
{
    Skiplist sl;
    // int a[] = { 5, 2, 3, 8, 9, 6, 5, 2, 3, 8, 9, 6, 5, 2, 3, 8, 9, 6 };
    int a[] = {5, 2, 3, 8, 9, 6};
    for (auto e : a)
    {
        // sl.Print();
        // printf("--------------------------------------\n");

        sl.add(e);
    }
    sl.Print();

    int x;
    cin >> x;
    sl.erase(x);

    return 0;
}

// int main()
//{
//	unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
//	std::default_random_engine generator(seed);
//
//	std::uniform_real_distribution<double> distribution(0.0, 1.0);
//	size_t count = 0;
//	for (int i = 0; i < 100; ++i)
//	{
//		if (distribution(generator) <= 0.25)
//			++count;
//	}
//	cout << count << endl;
//
//	return 0;
// }