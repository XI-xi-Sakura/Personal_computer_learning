#include <iostream>
using namespace std;

#include "UnionFindSet.hpp"
#include "Graph.hpp"

// int main()
//{
//	string a[] = { "", "", "", "" };
//	UnionFindSet<string> ufs(a, 4);
//
//	return 0;
// }

int main()
{
    // matrix::TestGraph1();
    // matrix::TestBDFS();
    // matrix::TestGraphMinTree();
    // matrix::TestGraphDijkstra();
    // matrix::TestGraphBellmanFord();
    matrix::TestFloydWarShall();

    // link_table::TestGraph1();

    // TestUnionFindSet();

    return 0;
}