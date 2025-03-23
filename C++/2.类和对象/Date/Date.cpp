#include "Date.hpp"
bool CheckDate();
bool Date::CheckDate()
{
    if (_month < 1 || _month > 12 || _day < 1 || _day > GetMonthDay(_year, _month))
    {
        return false;
    }
    else
    {
        return true;
    }
}

Date::Date(int year, int month, int day)
{
    _year = year;
    _month = month;
    _day = day;
    if (!CheckDate())
    {
        cout << "⽇期⾮法" << endl;
    }
}
void Date::Print() const
{
    cout << _year << "-" << _month << "-" << _day << endl;
}
// d1 < d2
bool Date::operator<(const Date &d) const
{
    if (_year < d._year)
    {
        return true;
    }
    else if (_year == d._year)
    {
        if (_month < d._month)
        {
            return true;
        }
        else if (_month == d._month)
        {
            return _day < d._day;
        }
    }
    return false;
}
// d1 <= d2
bool Date::operator<=(const Date &d) const
{
    return *this < d || *this == d;
}
bool Date::operator>(const Date &d) const
{
    return !(*this <= d);
}
bool Date::operator>=(const Date &d) const
{
    return !(*this < d);
}
bool Date::operator==(const Date &d) const
{
    return _year == d._year && _month == d._month && _day == d._day;
}
bool Date::operator!=(const Date &d) const
{
    return !(*this == d);
}
// d1 += 50
// d1 += -50
Date &Date::operator+=(int day)
{
    if (day < 0)
    {
        return *this -= -day;
    }
    _day += day;
    while (_day > GetMonthDay(_year, _month))
    {
        _day -= GetMonthDay(_year, _month);
        ++_month;
        if (_month == 13)
        {
            ++_year;
            _month = 1;
        }
    }
    return *this;//+=返回本身
}
Date Date::operator+(int day) const
{
    Date tmp = *this;
    tmp += day;
    return tmp; //返回临时对象，函数返回类型不能是引用
}
// d1 -= 100
Date &Date::operator-=(int day)
{
    if (day < 0)
    {
        return *this += -day;
    }
    _day -= day;
    while (_day <= 0)
    {
        --_month;
        if (_month == 0)
        {
            _month = 12;
            _year--;
        }
        // 借上⼀个⽉的天数
        _day += GetMonthDay(_year, _month);
    }
    return *this;
}
Date Date::operator-(int day) const
{
    Date tmp = *this;
    tmp -= day;
    return tmp;
}
//++d1
Date &Date::operator++()
{
    *this += 1;
    return *this;
}
// d1++
Date Date::operator++(int)
{
    Date tmp(*this);
    *this += 1;
    return tmp;//后置++，返回本身，需要对原有对象进行拷贝，在对目标对象进行++操作
}
Date &Date::operator--()
{
    *this -= 1;
    return *this;
}

Date Date::operator--(int)
{
    Date tmp = *this;
    *this -= 1;
    return tmp;
}
// d1 - d2
int Date::operator-(const Date &d) const
{
    Date max = *this;
    Date min = d;
    int flag = 1;
    if (*this < d)
    {
        max = d;
        min = *this;
        flag = -1;
    }
    int n = 0;
    while (min != max)
    {
        ++min;
        ++n;
    }
    return n * flag;
}
ostream &operator<<(ostream &out, const Date &d)
{
    out << d._year << "年" << d._month << "⽉" << d._day << "⽇" << endl;
    return out;
}
istream &operator>>(istream &in, Date &d)
{
    cout << "请依次输⼊年⽉⽇:>";
    in >> d._year >> d._month >> d._day;
    if (!d.CheckDate())
    {
        cout << "⽇期⾮法" << endl;
    }

    return in;
}