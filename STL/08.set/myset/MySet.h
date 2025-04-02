#pragma once

#include"RBTree.h"

namespace bit
{
	template<class K>
	class set
	{
		struct SetKeyOfT
		{
			const K& operator()(const K& key)
			{
				return key;
			}
		};

	public:
		typedef typename RBTree<K, const K, SetKeyOfT>::Iterator iterator;
		typedef typename RBTree<K, const K, SetKeyOfT>::ConstIterator const_iterator;

		iterator begin()
		{
			return _t.Begin();
		}

		iterator end()
		{
			return _t.End();
		}

		const_iterator begin() const
		{
			return _t.Begin();
		}

		const_iterator end() const
		{
			return _t.End();
		}

		pair<iterator, bool> insert(const K& key)
		{
			return _t.Insert(key);
		}

		iterator find(const K& key)
		{
			return _t.Find(key);
		}

	private:
		RBTree<K, const K, SetKeyOfT> _t;
	};

	void Print(const set<int>& s)
	{
		set<int>::const_iterator it = s.end();
		while (it != s.begin())
		{
			--it;
			//*it += 2;
			cout << *it << " ";
		}
		cout << endl;
	}

	void test_set()
	{
		set<int> s;
		int a[] = { 4, 2, 6, 1, 3, 5, 15, 7, 16, 14 };
		for (auto e : a)
		{
			s.insert(e);
		}

		for (auto e : s)
		{
			cout << e << " ";
		}
		cout << endl;

		set<int>::iterator it = s.end();
		while (it != s.begin())
		{
			--it;

			cout << *it << " ";
		}
		cout << endl;

		it = s.begin();
		//*it += 10;
		while (it != s.end())
		{
			cout << *it << " ";
			++it;
		}
		cout << endl;

		Print(s);
	}
}
