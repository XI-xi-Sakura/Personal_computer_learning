#include <list>
#include <unordered_map>
using namespace std;

class LRUCache
{
public:
    LRUCache(int capacity)
    {
        _capacity = capacity;
    }

    int get(int key)
    {
        // 如果key对应的值存在，则listit取出，这里就可以看出hashmap的value存的是list的iterator的好处：找到key
        // 也就找到key存的值在list中的iterator，也就直接删除，再进行头插，实现O(1)的数据挪动。
        auto hashit = _hashmap.find(key);
        if (hashit != _hashmap.end())
        {
            auto listit = hashit->second;
            pair<int, int> kv = *listit;

            _list.erase(listit);
            _list.push_front(kv);
            _hashmap[key] = _list.begin();

            return kv.second;
        }
        else
        {
            return -1;
        }
    }

    void put(int key, int value)
    {
        // 1.如果没有数据则进行插入数据
        // 2.如果有数据则进行数据更新
        auto hashit = _hashmap.find(key);
        if (hashit == _hashmap.end())
        {
            // 插入数据时，如果数据已经达到上限，则删除链表尾的数据和hashmap中的数据，两个删除操作都是O(1)
            if (_list.size() >= _capacity)
            {
                _hashmap.erase(_list.back().first);
                _list.pop_back();
            }

            _list.push_front(make_pair(key, value));
            _hashmap[key] = _list.begin();
        }
        else
        {
            // 再次put，将数据挪动list前面
            auto listit = hashit->second;
            pair<int, int> kv = *listit;
            kv.second = value;

            _list.erase(listit);
            _list.push_front(kv);
            _hashmap[key] = _list.begin();
        }
    }

private:
    list<pair<int, int>> _list; // 将最近用过的往链表的投上移动，保持LRU

    unordered_map<int, list<pair<int, int>>::iterator> _hashmap;

    size_t _capacity; // 容量大小，超过容量则换出，保持LRU

    // 使用unordered_map，让搜索效率达到O(1)
    // 需要注意：这里最巧的设计就是将unordered_map的value type放成list<pair<int, int>>::iterator，因为这样，当get一个已有的值以后，就可以直接找到key在list中对应的iterator，然后将这个值移动到链表的头部，保持LRU。
};

// 具体LRU Cache的过程和顺序，参考这个OJ题中的举例描述：

// LRUCache cache = new LRUCache( 2 /* 缓存容量 */ );

// cache.put(1, 1);  // 插入<1,1>
// cache.put(2, 2);  // 插入<2,2>

// cache.get(1);     // 返回  1
// cache.put(3, 3);  // 该操作会使得密钥 2 作废
// cache.get(2);     // 返回 -1 (未找到)
// cache.put(4, 4);  // 该操作会使得密钥 1 作废
// cache.get(1);     // 返回 -1 (未找到)
// cache.get(3);     // 返回  3
// cache.get(4);     // 返回  4

/**
 * Your LRUCache object will be instantiated and called as such:
 * LRUCache* obj = new LRUCache(capacity);
 * int param_1 = obj->get(key);
 * obj->put(key,value);
 */