#include"string.h"


// 		char* _str;
// 		size_t _size;
// 		size_t _capacity;


namespace bit
{
	const size_t string::npos = -1;

	//string::string()
	//{
	//	_str = new char[1]{'\0'};
	//	_size = 0;
	//	_capacity = 0;
	//}

	//typedef char* iterator;
	//typedef const char* const_iterator;

	string::iterator string::begin()
	{
		return _str;
	}

	string::iterator string::end()
	{
		return _str + _size;
	}

	string::const_iterator string::begin() const
	{
		return _str;
	}

	string::const_iterator string::end() const
	{
		return _str + _size;
	}

	
	string::string(const char* str)
		:_size(strlen(str))
	{
		_str = new char[_size + 1];
		_capacity = _size;
		strcpy(_str, str);
	}

	// s2(s1)
	string::string(const string& s)
	{
		_str = new char[s._capacity + 1];
		strcpy(_str, s._str);
		_size = s._size;
		_capacity = s._capacity;
	}

	// s1 = s3
	// s1 = s1
	string& string::operator=(const string& s)
	{
		if (this != &s)
		{
			char* tmp = new char[s._capacity + 1];
			strcpy(tmp, s._str);
			delete[] _str;

			_str = tmp;
			_size = s._size;
			_capacity = s._capacity;
		}

		return *this;
	}

	string::~string()
	{
		delete[] _str;
		_str = nullptr;
		_size = _capacity = 0;
	}

	const char* string::c_str() const
	{
		return _str;
	}

	size_t string::size() const
	{
		return _size;
	}

	char& string::operator[](size_t pos)
	{
		assert(pos < _size);
		return _str[pos];
	}

	const char& string::operator[](size_t pos) const
	{
		assert(pos < _size);
		return _str[pos];
	}

	void string::reserve(size_t n)
	{
		if (n > _capacity)
		{
			char* tmp = new char[n + 1];
			strcpy(tmp, _str);
			delete[] _str;

			_str = tmp;
			_capacity = n;
		}
	}

	void string::push_back(char ch)
	{
		/*if (_size == _capacity)
		{
			size_t newcapacity = _capacity == 0 ? 4 : _capacity * 2;
			reserve(newcapacity);
		}

		_str[_size] = ch;
		_str[_size + 1] = '\0';
		++_size;*/

		insert(_size, ch);
	}

	// "hello"  "xxxxxxxxxxxxx"
	void string::append(const char* str)
	{
		/*size_t len = strlen(str);
		if (_size + len > _capacity)
		{
			reserve(_size + len);
		}

		strcpy(_str+_size, str);
		_size += len;*/

		insert(_size, str);
	}

	string& string::operator+=(char ch)
	{
		push_back(ch);

		return *this;
	}

	string& string::operator+=(const char* str)
	{
		append(str);

		return *this;
	}

	void string::insert(size_t pos, char ch)
	{
		assert(pos <= _size);

		if (_size == _capacity)
		{
			size_t newcapacity = _capacity == 0 ? 4 : _capacity * 2;
			reserve(newcapacity);
		}

		/*int end = _size;
		while (end >= (int)pos)
		{
			_str[end + 1] = _str[end];
			--end;
		}*/

		size_t end = _size + 1;
		while (end > pos)
		{
			_str[end] = _str[end-1];
			--end;
		}

		_str[pos] = ch;
		++_size;
	}

	void string::insert(size_t pos, const char* str)
	{
		assert(pos <= _size);

		size_t len = strlen(str);
		if (_size + len > _capacity)
		{
			reserve(_size + len);
		}

		/*int end = _size;
		while (end >= (int)pos)
		{
			_str[end + len] = _str[end];
			--end;
		}*/

		size_t end = _size+len;
		while (end > pos+len-1)
		{
			_str[end] = _str[end - len];
			--end;
		}

		memcpy(_str + pos, str, len);
		_size += len;
	}

	// 17:10
	void string::erase(size_t pos, size_t len)
	{
		assert(pos < _size);

		// len大于后面剩余字符个数时，有多少删多少
		if (len >= _size-pos)
		{
			_str[pos] = '\0';
			_size = pos;
		}
		else
		{
			strcpy(_str + pos, _str + pos + len);//从 _str + pos + len 起始的字符串复制到 _str + pos 处。
			_size -= len;
		}
	}

	size_t string::find(char ch, size_t pos)
	{
		for (size_t i = pos; i < _size; i++)
		{
			if (_str[i] == ch)
			{
				return i;
			}
		}

		return npos;
	}

	size_t string::find(const char* sub, size_t pos)
	{
		char* p = strstr(_str + pos, sub);//在字符串中查找子字符串第一次出现的位置
		return  p - _str;
	}

	// s1.swap(s3)
	void string::swap(string& s)
	{
		std::swap(_str, s._str);
		std::swap(_size, s._size);
		std::swap(_capacity, s._capacity);
	}

	string string::substr(size_t pos, size_t len)
	{
		// len大于后面剩余字符，有多少取多少
		if (len > _size - pos)
		{
			string sub(_str + pos);
			return sub;
		}
		else
		{
			string sub;
			sub.reserve(len);
			for (size_t i = 0; i < len; i++)
			{
				sub += _str[pos + i];
			}

			return sub;
		}
	}

	bool string::operator<(const string& s) const
	{
		return strcmp(_str, s._str) < 0;
	}

	bool string::operator>(const string& s) const
	{
		return !(*this <= s);
	}

	bool string::operator<=(const string& s) const
	{
		return *this < s || *this == s;
	}

	bool string::operator>=(const string& s) const
	{
		return !(*this < s);
	}

	bool string::operator==(const string& s) const
	{
		return strcmp(_str, s._str) == 0;
	}

	bool string::operator!=(const string& s) const
	{
		return !(*this == s);
	}

	void string::clear()
	{
		_str[0] = '\0';
		_size = 0;
	}

	istream& operator>> (istream& is, string& str)
	{
		str.clear();
		char ch = is.get();
		while (ch != ' ' && ch != '\n')
		{
			str += ch;
			ch = is.get();
		}

		return is;
	}

	ostream& operator<< (ostream& os, const string& str)
	{
		for (size_t i = 0; i < str.size(); i++)
		{
			os << str[i];
		}

		return os;
	}
}