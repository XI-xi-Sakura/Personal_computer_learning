#pragma once

#include"HashTable.h"

namespace bit
{
	template<class K, class Hash = HashFunc<K>>
	class unordered_set
	{
		struct SetKeyOfT
		{
			const K& operator()(const K& key)
			{
				return key;
			}
		};
	public:
		typedef typename hash_bucket::HashTable<K, const K, SetKeyOfT, Hash>::Iterator iterator;
		typedef typename hash_bucket::HashTable<K, const K, SetKeyOfT, Hash>::ConstIterator const_iterator;


		iterator begin()
		{
			return _ht.Begin();
		}

		iterator end()
		{
			return _ht.End();
		}

		const_iterator begin() const
		{
			return _ht.Begin();
		}

		const_iterator end() const
		{
			return _ht.End();
		}

		pair<iterator, bool> insert(const K& key)
		{
			return _ht.Insert(key);
		}

		iterator Find(const K& key)
		{
			return _ht.Find(key);
		}

		bool Erase(const K& key)
		{
			return _ht.Erase(key);
		}

	private:
		hash_bucket::HashTable<K, const K, SetKeyOfT, Hash> _ht;
	};

	void Print(const unordered_set<int>& s)
	{
		unordered_set<int>::const_iterator it = s.begin();
		while (it != s.end())
		{
			// *it += 1;
			cout << *it << " ";
			++it;
		}
		cout << endl;
	}

	struct Date 
	{
		int _year;
		int _month;
		int _day;

		bool operator==(const Date& d) const
		{
			return _year == d._year
				&& _month == d._month
				&& _day == d._day;
		}
	};

	struct HashDate
	{
		size_t operator()(const Date& key)
		{
			// 112
			// 121
			return (key._year * 31 + key._month) * 31 + key._day;
		}
	};

	void test_set()
	{
		unordered_set<int> s;
		int a[] = { 4, 2, 6, 1, 3, 5, 15, 7, 16, 14, 3,3,15 };
		for (auto e : a)
		{
			s.insert(e);
		}

		for (auto e : s)
		{
			cout << e << " ";
		}
		cout << endl;

		unordered_set<int>::iterator it = s.begin();
		while (it != s.end())
		{
			//*it += 1;

			cout << *it << " ";
			++it;
		}
		cout << endl;

		unordered_set<Date, HashDate> us;
		us.insert({ 2024, 7, 25 });
		us.insert({ 2024, 7, 26 });

		Print(s);
	}
}
