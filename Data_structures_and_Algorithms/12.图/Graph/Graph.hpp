#pragma once

#include <set>
#include <string>
#include <cstring>
#include <queue>
#include <functional>
#include <climits>

#include "UnionFindSet.hpp"

using namespace std;

// weight
namespace matrix
{
    template <class V, class W, W MAX_W = INT_MAX, bool Direction = false>
    class Graph
    {
        typedef Graph<V, W, MAX_W, Direction> Self;

    public:
        Graph() = default;

        // 图的创建
        // 1、IO输入  -- 不方便测试,oj中更适合
        // 2、图结构关系写到文件，读取文件
        // 3、手动添加边
        Graph(const V *a, size_t n)
        {
            _vertexs.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                _vertexs.push_back(a[i]);
                _indexMap[a[i]] = i;
            }

            _matrix.resize(n);
            for (size_t i = 0; i < _matrix.size(); ++i)
            {
                _matrix[i].resize(n, MAX_W);
            }
        }

        size_t GetVertexIndex(const V &v)
        {
            auto it = _indexMap.find(v);
            if (it != _indexMap.end())
            {
                return it->second;
            }
            else
            {
                // assert(false);
                cout << "不存在顶点" << ":" << v << endl;
                throw invalid_argument("顶点不存在");

                return -1;
            }
        }

        void _AddEdge(size_t srci, size_t dsti, const W &w)
        {
            _matrix[srci][dsti] = w;
            // 无向图
            if (Direction == false)
            {
                _matrix[dsti][srci] = w;
            }
        }

        void AddEdge(const V &src, const V &dst, const W &w)
        {
            size_t srci = GetVertexIndex(src);
            size_t dsti = GetVertexIndex(dst);
            _AddEdge(srci, dsti, w);
        }

        void Print()
        {
            // 顶点
            for (size_t i = 0; i < _vertexs.size(); ++i)
            {
                cout << "[" << i << "]" << "->" << _vertexs[i] << endl;
            }
            cout << endl;

            // 矩阵
            // 横下标
            cout << "  ";
            for (size_t i = 0; i < _vertexs.size(); ++i)
            {
                // cout << i << " ";
                printf("%4d", i);
            }
            cout << endl;

            for (size_t i = 0; i < _matrix.size(); ++i)
            {
                cout << i << " "; // 竖下标
                for (size_t j = 0; j < _matrix[i].size(); ++j)
                {
                    // cout << _matrix[i][j] << " ";
                    if (_matrix[i][j] == MAX_W)
                    {
                        // cout << "* ";
                        printf("%4c", '*');
                    }
                    else
                    {
                        // cout << _matrix[i][j] << " ";
                        printf("%4d", _matrix[i][j]);
                    }
                }
                cout << endl;
            }
            cout << endl;

            for (size_t i = 0; i < _matrix.size(); ++i)
            {
                for (size_t j = 0; j < _matrix[i].size(); ++j)
                {
                    if (i < j && _matrix[i][j] != MAX_W)
                    {
                        cout << _vertexs[i] << "->" << _vertexs[j] << ":" << _matrix[i][j] << endl;
                    }
                }
            }
        }

        // void BFS(const V& src)
        //{
        //	size_t srci = GetVertexIndex(src);

        //	// 队列和标记数组
        //	queue<int> q;
        //	vector<bool> visited(_vertexs.size(), false);

        //	q.push(srci);
        //	visited[srci] = true;

        //	size_t n = _vertexs.size();
        //	while (!q.empty())
        //	{
        //		int front = q.front();
        //		q.pop();
        //		cout << front <<":"<<_vertexs[front] << endl;
        //		// 把front顶点的邻接顶点入队列
        //		for (size_t i = 0; i < n; ++i)
        //		{
        //			if (_matrix[front][i] != MAX_W)
        //			{
        //				if (visited[i] == false)
        //				{
        //					q.push(i);
        //					visited[i] = true;
        //				}
        //			}
        //		}
        //	}

        //	cout << endl;
        //}

        void BFS(const V &src)
        {
            size_t srci = GetVertexIndex(src);

            // 队列和标记数组
            queue<int> q;
            vector<bool> visited(_vertexs.size(), false);

            q.push(srci);
            visited[srci] = true;
            int levelSize = 1;

            size_t n = _vertexs.size();
            while (!q.empty())
            {
                // 一层一层出
                for (int i = 0; i < levelSize; ++i)
                {
                    int front = q.front();
                    q.pop();
                    cout << front << ":" << _vertexs[front] << " ";
                    // 把front顶点的邻接顶点入队列
                    for (size_t i = 0; i < n; ++i)
                    {
                        if (_matrix[front][i] != MAX_W)
                        {
                            if (visited[i] == false)
                            {
                                q.push(i);
                                visited[i] = true;
                            }
                        }
                    }
                }
                cout << endl;

                levelSize = q.size();
            }

            cout << endl;
        }

        void _DFS(size_t srci, vector<bool> &visited)
        {
            cout << srci << ":" << _vertexs[srci] << endl;
            visited[srci] = true;

            // 找一个srci相邻的没有访问过的点，去往深度遍历
            for (size_t i = 0; i < _vertexs.size(); ++i)
            {
                if (_matrix[srci][i] != MAX_W && visited[i] == false)
                {
                    _DFS(i, visited);
                }
            }
        }

        void DFS(const V &src)
        {
            size_t srci = GetVertexIndex(src);
            vector<bool> visited(_vertexs.size(), false);

            _DFS(srci, visited);
        }

        struct Edge
        {
            size_t _srci;
            size_t _dsti;
            W _w;

            Edge(size_t srci, size_t dsti, const W &w)
                : _srci(srci), _dsti(dsti), _w(w)
            {
            }

            bool operator>(const Edge &e) const
            {
                return _w > e._w;
            }
        };

        W Kruskal(Self &minTree)
        {
            size_t n = _vertexs.size();

            minTree._vertexs = _vertexs;
            minTree._indexMap = _indexMap;
            minTree._matrix.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                minTree._matrix[i].resize(n, MAX_W);
            }

            priority_queue<Edge, vector<Edge>, greater<Edge>> minque;
            for (size_t i = 0; i < n; ++i)
            {
                for (size_t j = 0; j < n; ++j)
                {
                    if (i < j && _matrix[i][j] != MAX_W)
                    {
                        minque.push(Edge(i, j, _matrix[i][j]));
                    }
                }
            }

            // 选出n-1条边
            int size = 0;
            W totalW = W();
            UnionFindSet ufs(n);
            while (!minque.empty())
            {
                Edge min = minque.top();
                minque.pop();

                if (!ufs.InSet(min._srci, min._dsti))
                {
                    // cout << _vertexs[min._srci] << "->" << _vertexs[min._dsti] <<":"<<min._w << endl;
                    minTree._AddEdge(min._srci, min._dsti, min._w);
                    ufs.Union(min._srci, min._dsti);
                    ++size;
                    totalW += min._w;
                }
                else
                {
                    // cout << "构成环：";
                    // cout << _vertexs[min._srci] << "->" << _vertexs[min._dsti] << ":" << min._w << endl;
                }
            }

            if (size == n - 1)
            {
                return totalW;
            }
            else
            {
                return W();
            }
        }

        W Prim(Self &minTree, const W &src)
        {
            size_t srci = GetVertexIndex(src);
            size_t n = _vertexs.size();

            minTree._vertexs = _vertexs;
            minTree._indexMap = _indexMap;
            minTree._matrix.resize(n);
            for (size_t i = 0; i < n; ++i)
            {
                minTree._matrix[i].resize(n, MAX_W);
            }

            /*set<int> X;
            set<int> Y;
            X.insert(srci);
            for (size_t i = 0; i < n; ++i)
            {
                if (i != srci)
                {
                    Y.insert(i);
                }
            }*/

            vector<bool> X(n, false);
            vector<bool> Y(n, true);
            X[srci] = true;
            Y[srci] = false;

            // 从X->Y集合中连接的边里面选出最小的边
            priority_queue<Edge, vector<Edge>, greater<Edge>> minq;
            // 先把srci连接的边添加到队列中
            for (size_t i = 0; i < n; ++i)
            {
                if (_matrix[srci][i] != MAX_W)
                {
                    minq.push(Edge(srci, i, _matrix[srci][i]));
                }
            }

            cout << "Prim开始选边" << endl;
            size_t size = 0;
            W totalW = W();
            while (!minq.empty())
            {
                Edge min = minq.top();
                minq.pop();

                // 最小边的目标点也在X集合，则构成环
                if (X[min._dsti])
                {
                    // cout << "构成环:";
                    // cout << _vertexs[min._srci] << "->" << _vertexs[min._dsti] << ":" << min._w << endl;
                }
                else
                {
                    minTree._AddEdge(min._srci, min._dsti, min._w);
                    // cout << _vertexs[min._srci] << "->" << _vertexs[min._dsti] << ":" << min._w << endl;
                    X[min._dsti] = true;
                    Y[min._dsti] = false;
                    ++size;
                    totalW += min._w;
                    if (size == n - 1)
                        break;

                    for (size_t i = 0; i < n; ++i)
                    {
                        if (_matrix[min._dsti][i] != MAX_W && Y[i])
                        {
                            minq.push(Edge(min._dsti, i, _matrix[min._dsti][i]));
                        }
                    }
                }
            }

            if (size == n - 1)
            {
                return totalW;
            }
            else
            {
                return W();
            }
        }

        void PrintShortPath(const V &src, const vector<W> &dist, const vector<int> &pPath)
        {
            size_t srci = GetVertexIndex(src);
            size_t n = _vertexs.size();
            for (size_t i = 0; i < n; ++i)
            {
                if (i != srci)
                {
                    // 找出i顶点的路径
                    vector<int> path;
                    size_t parenti = i;
                    while (parenti != srci)
                    {
                        path.push_back(parenti);
                        parenti = pPath[parenti];
                    }
                    path.push_back(srci);
                    reverse(path.begin(), path.end());

                    for (auto index : path)
                    {
                        cout << _vertexs[index] << "->";
                    }
                    cout << "权值和：" << dist[i] << endl;
                }
            }
        }

        // 顶点个数是N  -> 时间复杂度：O（N^2）空间复杂度：O（N）
        void Dijkstra(const V &src, vector<W> &dist, vector<int> &pPath)
        {
            size_t srci = GetVertexIndex(src);
            size_t n = _vertexs.size();
            dist.resize(n, MAX_W);
            pPath.resize(n, -1);

            dist[srci] = 0;
            pPath[srci] = srci;

            // 已经确定最短路径的顶点集合
            vector<bool> S(n, false);

            for (size_t j = 0; j < n; ++j)
            {
                // 选最短路径顶点且不在S更新其他路径
                int u = 0;
                W min = MAX_W;
                for (size_t i = 0; i < n; ++i)
                {
                    if (S[i] == false && dist[i] < min)
                    {
                        u = i;
                        min = dist[i];
                    }
                }

                S[u] = true;
                // 松弛更新u连接顶点v  srci->u + u->v <  srci->v  更新
                for (size_t v = 0; v < n; ++v)
                {
                    if (S[v] == false && _matrix[u][v] != MAX_W && dist[u] + _matrix[u][v] < dist[v])
                    {
                        dist[v] = dist[u] + _matrix[u][v];
                        pPath[v] = u;
                    }
                }
            }
        }

        // 时间复杂度：O(N^3) 空间复杂度：O（N）
        bool BellmanFord(const V &src, vector<W> &dist, vector<int> &pPath)
        {
            size_t n = _vertexs.size();
            size_t srci = GetVertexIndex(src);

            // vector<W> dist,记录srci-其他顶点最短路径权值数组
            dist.resize(n, MAX_W);

            // vector<int> pPath 记录srci-其他顶点最短路径父顶点数组
            pPath.resize(n, -1);

            // 先更新srci->srci为缺省值
            dist[srci] = W();

            // cout << "更新边：i->j" << endl;

            // 总体最多更新n轮
            for (size_t k = 0; k < n; ++k)
            {
                // i->j 更新松弛
                bool update = false;
                cout << "更新第:" << k << "轮" << endl;
                for (size_t i = 0; i < n; ++i)
                {
                    for (size_t j = 0; j < n; ++j)
                    {
                        // srci -> i + i ->j
                        if (_matrix[i][j] != MAX_W && dist[i] != MAX_W && dist[i] + _matrix[i][j] < dist[j])
                        {
                            update = true;
                            // cout << _vertexs[i] << "->" << _vertexs[j] << ":" << _matrix[i][j] << endl;
                            dist[j] = dist[i] + _matrix[i][j];
                            pPath[j] = i;
                        }
                    }
                }

                // 如果这个轮次中没有更新出更短路径，那么后续轮次就不需要再走了
                if (update == false)
                {
                    break;
                }
            }

            // 还能更新就是带负权回路
            for (size_t i = 0; i < n; ++i)
            {
                for (size_t j = 0; j < n; ++j)
                {
                    // srci -> i + i ->j
                    if (_matrix[i][j] != MAX_W && dist[i] + _matrix[i][j] < dist[j])
                    {
                        return false;
                    }
                }
            }

            return true;
        }

        void FloydWarshall(vector<vector<W>> &vvDist, vector<vector<int>> &vvpPath)
        {
            size_t n = _vertexs.size();
            vvDist.resize(n);
            vvpPath.resize(n);

            // 初始化权值和路径矩阵
            for (size_t i = 0; i < n; ++i)
            {
                vvDist[i].resize(n, MAX_W);
                vvpPath[i].resize(n, -1);
            }

            // 直接相连的边更新一下
            for (size_t i = 0; i < n; ++i)
            {
                for (size_t j = 0; j < n; ++j)
                {
                    if (_matrix[i][j] != MAX_W)
                    {
                        vvDist[i][j] = _matrix[i][j];
                        vvpPath[i][j] = i;
                    }

                    if (i == j)
                    {
                        vvDist[i][j] = W();
                    }
                }
            }

            // abcdef  a {} f ||  b {} c
            // 最短路径的更新i-> {其他顶点} ->j
            for (size_t k = 0; k < n; ++k)
            {
                for (size_t i = 0; i < n; ++i)
                {
                    for (size_t j = 0; j < n; ++j)
                    {
                        // k 作为的中间点尝试去更新i->j的路径
                        if (vvDist[i][k] != MAX_W && vvDist[k][j] != MAX_W && vvDist[i][k] + vvDist[k][j] < vvDist[i][j])
                        {
                            vvDist[i][j] = vvDist[i][k] + vvDist[k][j];

                            // 找跟j相连的上一个邻接顶点
                            // 如果k->j 直接相连，上一个点就k，vvpPath[k][j]存就是k
                            // 如果k->j 没有直接相连，k->...->x->j，vvpPath[k][j]存就是x

                            vvpPath[i][j] = vvpPath[k][j];
                        }
                    }
                }

                // 打印权值和路径矩阵观察数据
                for (size_t i = 0; i < n; ++i)
                {
                    for (size_t j = 0; j < n; ++j)
                    {
                        if (vvDist[i][j] == MAX_W)
                        {
                            // cout << "*" << " ";
                            printf("%3c", '*');
                        }
                        else
                        {
                            // cout << vvDist[i][j] << " ";
                            printf("%3d", vvDist[i][j]);
                        }
                    }
                    cout << endl;
                }
                cout << endl;

                for (size_t i = 0; i < n; ++i)
                {
                    for (size_t j = 0; j < n; ++j)
                    {
                        // cout << vvParentPath[i][j] << " ";
                        printf("%3d", vvpPath[i][j]);
                    }
                    cout << endl;
                }
                cout << "=================================" << endl;
            }
        }

    private:
        vector<V> _vertexs;        // 顶点集合
        map<V, int> _indexMap;     // 顶点映射下标
        vector<vector<W>> _matrix; // 邻接矩阵
    };

    void TestGraph1()
    {
        Graph<char, int, INT_MAX, true> g("0123", 4);
        g.AddEdge('0', '1', 1);
        g.AddEdge('0', '3', 4);
        g.AddEdge('1', '3', 2);
        g.AddEdge('1', '2', 9);
        g.AddEdge('2', '3', 8);
        g.AddEdge('2', '1', 5);
        g.AddEdge('2', '0', 3);
        g.AddEdge('3', '2', 6);

        g.Print();
    }

    void TestBDFS()
    {
        string a[] = {"张三", "李四", "王五", "赵六", "周七"};
        Graph<string, int> g1(a, sizeof(a) / sizeof(string));
        g1.AddEdge("张三", "李四", 100);
        g1.AddEdge("张三", "王五", 200);
        g1.AddEdge("王五", "赵六", 30);
        g1.AddEdge("王五", "周七", 30);
        g1.Print();

        g1.BFS("张三");
        g1.DFS("张三");
    }

    void TestGraphMinTree()
    {
        const char str[] = "abcdefghi";
        Graph<char, int> g(str, strlen(str));
        g.AddEdge('a', 'b', 4);
        g.AddEdge('a', 'h', 8);
        // g.AddEdge('a', 'h', 9);
        g.AddEdge('b', 'c', 8);
        g.AddEdge('b', 'h', 11);
        g.AddEdge('c', 'i', 2);
        g.AddEdge('c', 'f', 4);
        g.AddEdge('c', 'd', 7);
        g.AddEdge('d', 'f', 14);
        g.AddEdge('d', 'e', 9);
        g.AddEdge('e', 'f', 10);
        g.AddEdge('f', 'g', 2);
        g.AddEdge('g', 'h', 1);
        g.AddEdge('g', 'i', 6);
        g.AddEdge('h', 'i', 7);

        Graph<char, int> kminTree;
        cout << "Kruskal:" << g.Kruskal(kminTree) << endl;
        kminTree.Print();
        cout << endl
             << endl;

        Graph<char, int> pminTree;
        cout << "Prim:" << g.Prim(pminTree, 'a') << endl;
        pminTree.Print();
        cout << endl;

        for (size_t i = 0; i < strlen(str); ++i)
        {
            cout << "Prim:" << g.Prim(pminTree, str[i]) << endl;
        }
    }

    void TestGraphDijkstra()
    {
        /*	const char* str = "syztx";
            Graph<char, int, INT_MAX, true> g(str, strlen(str));
            g.AddEdge('s', 't', 10);
            g.AddEdge('s', 'y', 5);
            g.AddEdge('y', 't', 3);
            g.AddEdge('y', 'x', 9);
            g.AddEdge('y', 'z', 2);
            g.AddEdge('z', 's', 7);
            g.AddEdge('z', 'x', 6);
            g.AddEdge('t', 'y', 2);
            g.AddEdge('t', 'x', 1);
            g.AddEdge('x', 'z', 4);

            vector<int> dist;
            vector<int> parentPath;
            g.Dijkstra('s', dist, parentPath);
            g.PrintShortPath('s', dist, parentPath);*/

        // 图中带有负权路径时，贪心策略则失效了。
        // 测试结果可以看到s->t->y之间的最短路径没更新出来
        /*const char* str = "sytx";
        Graph<char, int, INT_MAX, true> g(str, strlen(str));
        g.AddEdge('s', 't', 10);
        g.AddEdge('s', 'y', 5);
        g.AddEdge('t', 'y', -7);
        g.AddEdge('y', 'x', 3);
        vector<int> dist;
        vector<int> parentPath;
        g.Dijkstra('s', dist, parentPath);
        g.PrintShortPath('s', dist, parentPath);*/

        // const char* str = "syztx";
        // Graph<char, int, INT_MAX, true> g(str, strlen(str));
        // g.AddEdge('s', 't', 6);
        // g.AddEdge('s', 'y', 7);
        // g.AddEdge('y', 'z', 9);
        // g.AddEdge('y', 'x', -3);
        // g.AddEdge('z', 's', 2);
        // g.AddEdge('z', 'x', 7);
        // g.AddEdge('t', 'x', 5);
        // g.AddEdge('t', 'y', 8);
        // g.AddEdge('t', 'z', -4);
        // g.AddEdge('x', 't', -2);
        // vector<int> dist;
        // vector<int> parentPath;
        // g.Dijkstra('s', dist, parentPath);
        // g.PrintShortPath('s', dist, parentPath);
    }

    void TestGraphBellmanFord()
    {
        /*	const char* str = "syztx";
            Graph<char, int, INT_MAX, true> g(str, strlen(str));
            g.AddEdge('s', 't', 6);
            g.AddEdge('s', 'y', 7);
            g.AddEdge('y', 'z', 9);
            g.AddEdge('y', 'x', -3);
            g.AddEdge('z', 's', 2);
            g.AddEdge('z', 'x', 7);
            g.AddEdge('t', 'x', 5);
            g.AddEdge('t', 'y', 8);
            g.AddEdge('t', 'z', -4);
            g.AddEdge('x', 't', -2);
            vector<int> dist;
            vector<int> parentPath;
            g.BellmanFord('s', dist, parentPath);
            g.PrintShortPath('s', dist, parentPath);*/

        const char *str = "syztx";
        Graph<char, int, INT_MAX, true> g(str, strlen(str));
        g.AddEdge('s', 't', 6);
        g.AddEdge('s', 'y', 7);
        g.AddEdge('y', 'z', 9);
        g.AddEdge('y', 'x', -3);
        // g.AddEdge('y', 's', 1); // 新增
        g.AddEdge('z', 's', 2);
        g.AddEdge('z', 'x', 7);
        g.AddEdge('t', 'x', 5);
        // g.AddEdge('t', 'y', -8); //更改
        g.AddEdge('t', 'y', 8);

        g.AddEdge('t', 'z', -4);
        g.AddEdge('x', 't', -2);
        vector<int> dist;
        vector<int> parentPath;
        if (g.BellmanFord('s', dist, parentPath))
            g.PrintShortPath('s', dist, parentPath);
        else
            cout << "带负权回路" << endl;
    }

    void TestFloydWarShall()
    {
        const char *str = "12345";
        Graph<char, int, INT_MAX, true> g(str, strlen(str));
        g.AddEdge('1', '2', 3);
        g.AddEdge('1', '3', 8);
        g.AddEdge('1', '5', -4);
        g.AddEdge('2', '4', 1);
        g.AddEdge('2', '5', 7);
        g.AddEdge('3', '2', 4);
        g.AddEdge('4', '1', 2);
        g.AddEdge('4', '3', -5);
        g.AddEdge('5', '4', 6);
        vector<vector<int>> vvDist;
        vector<vector<int>> vvParentPath;
        g.FloydWarshall(vvDist, vvParentPath);

        // 打印任意两点之间的最短路径
        for (size_t i = 0; i < strlen(str); ++i)
        {
            g.PrintShortPath(str[i], vvDist[i], vvParentPath[i]);
            cout << endl;
        }
    }
}

namespace link_table
{
    template <class W>
    struct Edge
    {
        // int _srci;
        int _dsti; // 目标点的下标
        W _w;      // 权值
        Edge<W> *_next;

        Edge(int dsti, const W &w)
            : _dsti(dsti), _w(w), _next(nullptr)
        {
        }
    };

    template <class V, class W, bool Direction = false>
    class Graph
    {
        typedef Edge<W> Edge;

    public:
        Graph(const V *a, size_t n)
        {
            _vertexs.reserve(n);
            for (size_t i = 0; i < n; ++i)
            {
                _vertexs.push_back(a[i]);
                _indexMap[a[i]] = i;
            }

            _tables.resize(n, nullptr);
        }

        size_t GetVertexIndex(const V &v)
        {
            auto it = _indexMap.find(v);
            if (it != _indexMap.end())
            {
                return it->second;
            }
            else
            {
                // assert(false);
                throw invalid_argument("顶点不存在");

                return -1;
            }
        }

        void AddEdge(const V &src, const V &dst, const W &w)
        {
            size_t srci = GetVertexIndex(src);
            size_t dsti = GetVertexIndex(dst);

            // 1->2
            Edge *eg = new Edge(dsti, w);
            eg->_next = _tables[srci];
            _tables[srci] = eg;

            // 2->1
            if (Direction == false)
            {
                Edge *eg = new Edge(srci, w);
                eg->_next = _tables[dsti];
                _tables[dsti] = eg;
            }
        }

        void Print()
        {
            // 顶点
            for (size_t i = 0; i < _vertexs.size(); ++i)
            {
                cout << "[" << i << "]" << "->" << _vertexs[i] << endl;
            }
            cout << endl;

            for (size_t i = 0; i < _tables.size(); ++i)
            {
                cout << _vertexs[i] << "[" << i << "]->";
                Edge *cur = _tables[i];
                while (cur)
                {
                    cout << "[" << _vertexs[cur->_dsti] << ":" << cur->_dsti << ":" << cur->_w << "]->";
                    cur = cur->_next;
                }
                cout << "nullptr" << endl;
            }
        }

    private:
        vector<V> _vertexs;     // 顶点集合
        map<V, int> _indexMap;  // 顶点映射下标
        vector<Edge *> _tables; // 邻接表
    };

    void TestGraph1()
    {
        /*Graph<char, int, true> g("0123", 4);
        g.AddEdge('0', '1', 1);
        g.AddEdge('0', '3', 4);
        g.AddEdge('1', '3', 2);
        g.AddEdge('1', '2', 9);
        g.AddEdge('2', '3', 8);
        g.AddEdge('2', '1', 5);
        g.AddEdge('2', '0', 3);
        g.AddEdge('3', '2', 6);

        g.Print();*/

        string a[] = {"张三", "李四", "王五", "赵六"};
        Graph<string, int, true> g1(a, 4);
        g1.AddEdge("张三", "李四", 100);
        g1.AddEdge("张三", "王五", 200);
        g1.AddEdge("王五", "赵六", 30);
        g1.Print();
    }
}