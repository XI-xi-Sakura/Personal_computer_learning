#pragma once

#include <iostream>
#include <vector>
#include <map>
#include <algorithm>

// template<class T>
// class UnionFindSet
//{
// public:
//	UnionFindSet(const T* a, size_t n)
//	{
//		for (size_t i = 0; i < n; ++i)
//		{
//			_a.push_back(a[i]);
//			_indexMap[a[i]] = i;
//		}
//	}
// private:
//	vector<T> _a;                  // 编号找人
//	map<T, int> _indexMap;          // 人找编号
// };

class UnionFindSet
{
public:
    UnionFindSet(size_t n)
        : _ufs(n, -1)
    {
    }

    void Union(int x1, int x2)
    {
        int root1 = FindRoot(x1);
        int root2 = FindRoot(x2);
        // 如果本身就在一个集合就没必要合并了
        if (root1 == root2)
            return;

        // 控制数据量小的往大的集合合并
        if (abs(_ufs[root1]) < abs(_ufs[root2]))
            std::swap(root1, root2);

        _ufs[root1] += _ufs[root2];
        _ufs[root2] = root1;
    }

    int FindRoot(int x)
    {
        int root = x;
        while (_ufs[root] >= 0)
        {
            root = _ufs[root];
        }

        // 路径压缩
        while (_ufs[x] >= 0)
        {
            int parent = _ufs[x];
            _ufs[x] = root;

            x = parent;
        }

        return root;
    }

    bool InSet(int x1, int x2)
    {
        return FindRoot(x1) == FindRoot(x2);
    }

    size_t SetSize()
    {
        size_t size = 0;
        for (size_t i = 0; i < _ufs.size(); ++i)
        {
            if (_ufs[i] < 0)
            {
                ++size;
            }
        }

        return size;
    }

private:
    std::vector<int> _ufs;
};

void TestUnionFindSet()
{
    UnionFindSet ufs(10);
    ufs.Union(8, 9);
    ufs.Union(7, 8);
    ufs.Union(6, 7);
    ufs.Union(5, 6);
    ufs.Union(4, 5);

    ufs.FindRoot(6);
    ufs.FindRoot(9);
}