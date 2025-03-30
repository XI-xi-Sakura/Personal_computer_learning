## map系列的使用

## **[map和multimap参考文档](https://legacy.cplusplus.com/reference/map/)**

## **map类的介绍**

 map是关联容器，按照特定次序（按key来比较存储由key和value组合元素）
- 在map中，键值key通常用于排序和唯一标识元素，而value中存储与此键值key关联的内容
- map允许根据顺序对元素进行直接迭代，即对map中的元素进行迭代时，可得到有序序列
- **map支持下标访问符[ ],即可以在[ ]中放入key就可以找到与可以对应的value**
- map通常被实现为**搜索二叉树**

## map的声明


- Key就是map底层关键字的类型，T是map底层value的类型，map默认要求Key支持小于比较，如果不支持或者需要的话可以自行实现仿函数传给第二个模版参数，map底层存储数据的内存是从空间配置器申请的。
- 一般情况下，我们都不需要传后两个模版参数。map底层是用**红黑树实现**，增删查改效率是$O(log N)$，迭代器遍历是走的中序，所以是按key有序顺序遍历的。
```cpp
template < class Key 									// map::key_type
		,  class T 										// map::mapped_type
		,  class Compare = less<Key>, 					// map::key_compare
		,  class Alloc = allocator<pair<const Key,T> > 	// map::allocator_type
> class map;
```

## **pair类型介绍**

map底层的红黑树节点中的数据，使用pair<Key, T>存储键值对数据。
```cpp
typedef pair<const Key, T> value_type;

template <class T1, class T2>
struct pair 
{
    typedef T1 first_type;
    typedef T2 second_type;
    T1 first;
    T2 second;
    pair(): first(T1()), second(T2()) {}
    pair(const T1& a, const T2& b): first(a), second(b) {}
    
    template<class U, class V> 
    pair (const pair<U,V>& pr): first(pr.first), second(pr.second) {}
};

template <class T1,class T2>
inline pair<T1,T2> make_pair (T1 x, T2 y)
{
    return ( pair<T1,T2>(x,y) );
}
```

## map的构造

map的构造我们关注以下几个接口即可。

- map支持正向和反向迭代遍历，遍历默认按key的升序顺序，因为底层是二叉搜索树，迭代器遍历走的中序；支持迭代器就意味着支持范围for，
- map支持修改value数据，不支持修改key数据，修改关键字数据，破坏了底层搜索树的结构。
```cpp
// empty (1) 无参默认构造
explicit map (const key_compare& comp = key_compare(),
			  const allocator_type& alloc = allocator_type());
			  
// range (2) 迭代器区间构造
template <class InputIterator>
map (InputIterator first, InputIterator last,
	const key_compare& comp = key_compare(),
	const allocator_type& = allocator_type());
	
// copy (3) 拷贝构造
map (const map& x);

// initializer list (5) initializer 列表构造
map (initializer_list<value_type> il,
	const key_compare& comp = key_compare(),
	const allocator_type& alloc = allocator_type());
	
// 迭代器是一个双向迭代器
iterator -> a bidirectional iterator to const value_type
// 正向迭代器
iterator begin();
iterator end();
// 反向迭代器
reverse_iterator rbegin();
reverse_iterator rend();
```

## map的增删查

map增接口，插入的pair键值对数据，跟set所有不同，但是查和删的接口只用关键字key跟set是完全类似的，不过find返回iterator，不仅仅可以确认key在不在，还找到key映射的value，同时通过迭代还可以修改value。

- 插入

```cpp
// 单个数据插入，如果已经key存在则插入失败，key存在相等value不相等也会插入失败
pair<iterator,bool> insert (const value_type& val);

// 列表插入，已经在容器中存在的值不会插入
void insert (initializer_list<value_type> il);

// 迭代器区间插入，已经在容器中存在的值不会插入
template <class InputIterator>
void insert (InputIterator first, InputIterator last);
```
- 查找

```cpp
// 查找k，返回k所在的迭代器，没有找到返回end()
iterator find (const key_type& k);

// 查找k，返回k的个数
size_type count (const key_type& k) const;
```
- 删除

```cpp
// 删除一个迭代器位置的值
iterator erase (const_iterator position);
// 删除k，k存在返回0，存在返回1
size_type erase (const key_type& k);
// 删除一段迭代器区间的值
iterator erase (const_iterator first, const_iterator last);
```
- 比较
```cpp
// 返回大于等k位置的迭代器
iterator lower_bound (const key_type& k);
// 返回大于k位置的迭代器
const_iterator lower_bound (const key_type& k) const;
```

## map的数据修改
前面提到`map`支持修改`mapped_type`数据，不支持修改`key`数据，修改关键字数据会破坏底层搜索树的结构。
- map第一个支持修改的方式是通过迭代器，迭代器遍历时或者find返回key所在的iterator修改。
- map还有一个非常重要的修改接口operator[]，它不仅支持修改，还支持插入数据和查找数据，是一个多功能复合接口。
从内部实现角度，map把传统说的value值定义为T类型，typedef为mapped_type。而value_type是红黑树结点中存储的pair键值对值。日常使用中习惯将这里的T映射值叫做value。

```cpp
key_type -> The first template parameter (Key)
mapped_type -> The second template parameter (T)
value_type -> pair<const key_type,mapped_type>
```

```cpp
// 查找k，返回k所在的迭代器，没有找到返回end()，如果找到了通过iterator可以修改key对应的mapped_type值
iterator find (const key_type& k);

//insert插入一个pair<key, T>对象
pair<iterator,bool> insert (const value_type& val);

mapped_type& operator[] (const key_type& k);

// operator的内部实现
mapped_type& operator[] (const key_type& k)
{
    // 1、如果k不在map中，insert会插入k和mapped_type默认值，同时[]返回结点中存储mapped_type值的引用，那么我们可以通过引用修改返映射值。所以[]具备了插入+修改功能
    // 2、如果k在map中，insert会插入失败，但是insert返回pair对象的first是指向key结点的迭代器，返回值同时[]返回结点中存储mapped_type值的引用，所以[]具备了查找+修改的功能
    pair<iterator, bool> ret = insert({ k, mapped_type() });
    iterator it = ret.first;
    return it->second;
}
```
>文档中对insert返回值的说明
1、如果key已经在map中，插入失败，则返回一个pair<iterator,bool>对象，返回pair对象first是key所在结点的迭代器，second是false
2、如果key不在在map中，插入成功，则返回一个pair<iterator,bool>对象，返回pair对象first是新插入key所在结点的迭代器，second是true
也就是说无论插入成功还是失败，返回pair<iterator,bool>对象的first都会指向key所在的迭代器

>那么也就意味着insert插入失败时充当了查找的功能，正是因为这一点，insert可以用来实现operator[]
需要注意的是这里有两个pair，不要混淆了，一个是map底层红黑树节点中存的pair<key, T>，另一个是insert返回值pair<iterator,bool>

注：在元素访问时，有一个与operator[ ] 类似的操作at( )函数（不常用），都是通过key去找到key对应的value；不同的是：当key不存在时，operator[ ] 用默认value与key构成键值对然后插入，返回默认value；at( )直接抛出异常

## 使用样例

```cpp
#include<iostream>
#include<map>
using namespace std;
int main()
{
    // initializer_list构造及迭代遍历
    map<string, string> dict = { {"left", "左边"}, {"right", "右边"},
    {"insert", "插入"},{ "string", "字符串" } };
    //map<string, string>::iterator it = dict.begin();
    auto it = dict.begin();
    while (it != dict.end())
    {
        //cout << (*it).first <<":"<<(*it).second << endl;
        // map的迭代基本都使用operator->，这里省略了一个->
        // 第一个->是迭代器运算符重载，返回pair*，第二个箭头是结构指针解引用取pair数据
        //cout << it.operator->()->first << ":" << it.operator->()->second << endl;
        cout << it->first << ":" << it->second << endl;
        ++it;
    }
    cout << endl;
    // insert插入pair对象的4种方式，对比之下，最后一种最方便
    pair<string, string> kv1("first", "第一个");
    dict.insert(kv1);
    dict.insert(pair<string, string>("second", "第二个"));
    dict.insert(make_pair("sort", "排序"));
    dict.insert({ "auto", "自动的" });
    // "left"已经存在，插入失败
    dict.insert({ "left", "左边,剩余" });
    // 范围for遍历
    for (const auto& e : dict)
    {
        cout << e.first << ":" << e.second << endl;
    }
    cout << endl;
    string str;
    while (cin >> str)
    {
        auto ret = dict.find(str);
        if (ret != dict.end())
        {
            cout << str << "->" << ret->second << endl;
        }
        else
        {
            cout << "无此单词，请重新输入" << endl;
        }
    }
    // erase等接口跟set完全类似，这里就不演示讲解了
    return 0;
}
```



```cpp
#include<iostream>
#include<map>
#include<string>
using namespace std;
int main()
{
    // 利用find和iterator修改功能，统计水果出现的次数
    string arr[] = { "苹果", "西瓜", "苹果", "西瓜", "苹果", "苹果", "西瓜", 
    "苹果", "香蕉", "苹果", "香蕉" };
    map<string, int> countMap;
    for (const auto& str : arr)
    {
        // 先查找水果在不在map中
        // 1、不在，说明水果第一次出现，则插入{水果, 1}
        // 2、在，则查找到的节点中水果对应的次数++
        auto ret = countMap.find(str);
        if (ret == countMap.end())
        {
            countMap.insert({ str, 1 });
        }
        else
        {
            ret->second++;
        }
    }
    for (const auto& e : countMap)
    {
        cout << e.first << ":" << e.second << endl;
    }
    cout << endl;
    return 0;
}
```
```cpp
#include<iostream>
#include<map>
#include<string>
using namespace std;
int main()
{
    // 利用[]插入+修改功能，巧妙实现统计水果出现的次数
    string arr[] = { "苹果", "西瓜", "苹果", "西瓜", "苹果", "苹果", "西瓜", 
    "苹果", "香蕉", "苹果", "香蕉" };
    map<string, int> countMap;
    for (const auto& str : arr)
    {
        // []先查找水果在不在map中
        // 1、不在，说明水果第一次出现，则插入{水果, 0}，同时返回次数的引用，++一下就变成1次了
        // 2、在，则返回水果对应的次数++
        countMap[str]++;
    }
    for (const auto& e : countMap)
    {
        cout << e.first << ":" << e.second << endl;
    }
    cout << endl;
    return 0;
}
```
```cpp
#include<iostream>
#include<map>
#include<string>
using namespace std;
int main()
{
    map<string, string> dict;
    dict.insert(make_pair("sort", "排序"));
    // key不存在->插入 {"insert", string()}
    dict["insert"];
    // 插入+修改
    dict["left"] = "左边";
    // 修改
    dict["left"] = "左边､剩余";
    // key存在->查找
    cout << dict["left"] << endl;
    return 0;
}
```

## multimap和map的差异

multimap和map的使用基本完全类似，主要区别点在于multimap支持关键值key冗余，那么insert/find/count/erase都围绕着支持关键值key冗余有所差异，这里跟set和multiset完全一样，比如find时，有多个key，返回中序第一个。其次就是multimap不支持[]，因为支持key冗余，[]就只能支持插入了，不能支持修改。
## OJ题目
-  **138.随机链表的复制-力扣(LeetCode)**：数据结构初阶阶段，为了控制随机指针，我们将拷贝结点链接在原节点的后面解决，后面拷贝节点还得解下来链接，非常麻烦。这里我们直接让{原结点,拷贝结点}建立映射关系放到map中，控制随机指针会非常简单方便，这里体现了map在解决一些问题时的价值，完全是降维打击。
```cpp
class Solution{
public:
    Node* copyRandomList (Node* head){
        map<Node*,Node>nodeMap;
        Node* copyhead =nullptr,copytail = nullptr;
        Node* cur = head;
        while(cur)
        {
            if(copytail ==nullptr)
            {
                copyhead= copytail = new Node(cur->val);
            }
            else
            {
                copytail->next = new Node(cur->val);
                copytail= copytail->next;
            }
            //原节点和拷贝节点map kv存储
            nodeMap[cur]=copytail;
            cur=cur->next;
        }
        //处理random
        cur = head;
        Node* copy = copyhead;
        while(cur)
        {
            if(cur->random==nullptr)
            {
                copy->random = nullptr;
            }
            else
            {
                copy->random=nodeMap[cur->random];
            }
            cur=cur->next;
            copy = copy->next;
        }
        return copyhead;
    }
};
```
- **692. 前K个高频单词 - 力扣(LeetCode)**：本题目我们利用map统计出次数以后，返回的答案应该按单词出现频率由高到低排序，有一个特殊要求，如果不同的单词有相同出现频率，按字典顺序排序。
    - **解决思路1**：用排序找前k个单词，因为map中已经对key单词排序过，也就意味着遍历map时，次数相同的单词，字典序小的在前面，字典序大的在后面。那么我们将数据放到vector中用一个稳定的排序就可以实现上面特殊要求，但是sort底层是快排，是不稳定的，所以我们要用stable_sort，它是稳定的。
```cpp
class Solution {
public:
    struct Compare
    {
        bool operator()(const pair<string, int>& x, const pair<string, int>& y) const
        {
            return x.second > y.second;
        }
    };
    vector<string> topKFrequent(vector<string>& words, int k) {
        map<string, int> countMap;
        for(auto& e : words)
        {
            countMap[e]++;
        }
        vector<pair<string, int>> v(countMap.begin(), countMap.end());
        // 仿函数控制降序
        stable_sort(v.begin(), v.end(), Compare());
        //sort(v.begin(), v.end(), Compare());
        // 取前k个
        vector<string> strV;
        for(int i = 0; i < k; ++i)
        {
            strV.push_back(v[i].first);
        }
        return strV;
    }
};
```
   - **解决思路2**：将map统计出的次数的数据放到vector中排序，或者放到priority_queue中来选出前k个。利用仿函数强行控制次数相等的，字典序小的在前面。
```cpp
class Solution {
public:
    struct Compare
    {
        bool operator()(const pair<string, int>& x, const pair<string, int>& y) const
        {
            return x.second > y.second || (x.second == y.second && x.first < y.first);
        }
    };
    vector<string> topKFrequent(vector<string>& words, int k) {
        map<string, int> countMap;
        for(auto& e : words)
        {
            countMap[e]++;
        }
        vector<pair<string, int>> v(countMap.begin(), countMap.end());
        // 仿函数控制降序，仿函数控制次数相等，字典序小的在前面
        sort(v.begin(), v.end(), Compare());
        // 取前k个
        vector<string> strV;
        for(int i = 0; i < k; ++i)
        {
            strV.push_back(v[i].first);
        }
        return strV;
    }
};
```

