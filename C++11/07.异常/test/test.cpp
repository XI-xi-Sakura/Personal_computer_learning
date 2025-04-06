#include <iostream>
#include <string>
using namespace std;

double Divide(int a, int b)
{
    try
    {
        // 当b == 0时抛出异常
        if (b == 0)
        {
            string s("Divide by zero condition!");
            throw s;
        }
        else
        {
            return ((double)a / (double)b);
        }
    }
    catch (int errid)
    {
        cout << errid << endl;
        return 0;
    }
}

void Func()
{
    int len, time;
    cin >> len >> time;
    try
    {
        cout << Divide(len, time) << endl;
    }
    catch (const char *errmsg)
    {
        cout << errmsg << endl;
    }
    cout << __FUNCTION__ << ":" << __LINE__ << "行执行" << endl;
}

int test1()
{
    while (1)
    {
        try
        {
            Func();
        }
        catch (const string &errmsg)
        {
            cout << errmsg << endl;
        }
    }
    return 0;
}

#include <thread>
// 一般大型项目程序才会使用异常，下面我们模拟设计一个服务的几个模块
// 每个模块的继承都是Exception的派生类，每个模块可以添加自己的数据
// 最后捕获时，我们捕获基类就可以
class Exception
{
public:
    Exception(const string &errmsg, int id)
        : _errmsg(errmsg), _id(id)
    {
    }

    virtual string what() const
    {
        return _errmsg;
    }

    int getid() const
    {
        return _id;
    }

protected:
    string _errmsg;
    int _id;
};

class SqlException : public Exception
{
public:
    SqlException(const string &errmsg, int id, const string &sql)
        : Exception(errmsg, id), _sql(sql)
    {
    }

    virtual string what() const
    {
        string str = "SqlException:";
        str += _errmsg;
        str += "->";
        str += _sql;
        return str;
    }

private:
    const string _sql;
};

class CacheException : public Exception
{
public:
    CacheException(const string &errmsg, int id)
        : Exception(errmsg, id)
    {
    }

    virtual string what() const
    {
        string str = "CacheException:";
        str += _errmsg;
        return str;
    }
};

class HttpException : public Exception
{
public:
    HttpException(const string &errmsg, int id, const string &type)
        : Exception(errmsg, id), _type(type)
    {
    }

    virtual string what() const
    {
        string str = "HttpException:";
        str += _type;
        str += ":";
        str += _errmsg;
        return str;
    }

private:
    const string _type;
};

void SQLMgr()
{
    if (rand() % 7 == 0)
    {
        throw SqlException("权限不足", 100, "select * from name = '张三'");
    }
    else
    {
        cout << "SQLMgr调用成功" << endl;
    }
}

void CacheMgr()
{
    if (rand() % 5 == 0)
    {
        throw CacheException("权限不足", 100);
    }
    else if (rand() % 6 == 0)
    {
        throw CacheException("数据不存在", 101);
    }
    else
    {
        cout << "CacheMgr调用成功" << endl;
        SQLMgr();
    }
}

void HttpServer()
{
    if (rand() % 3 == 0)
    {
        throw HttpException("请求资源不存在", 100, "get");
    }
    else if (rand() % 4 == 0)
    {
        throw HttpException("权限不足", 101, "post");
    }
    else
    {
        cout << "HttpServer调用成功" << endl;
        CacheMgr();
    }
}

int test2()
{
    srand(time(0));
    while (1)
    {
        this_thread::sleep_for(chrono::seconds(1));
        try
        {
            HttpServer();
        }
        catch (const Exception &e) // 这里捕获基类，基类对象和派生类对象都可以被捕获
        {
            cout << e.what() << endl;
        }
        catch (...)
        {
            cout << "Unkown Exception" << endl;
        }
    }
    return 0;
}

void _SeedMsg(const string &s)
{
    if (rand() % 2 == 0)
    {
        throw HttpException("网络不稳定，发送失败", 102, "put");
    }
    else if (rand() % 7 == 0)
    {
        throw HttpException("你已经不是对象的好友，发送失败", 103, "put");
    }
    else
    {
        cout << "发送成功" << endl;
    }
}

void SendMsg(const string &s)
{
    // 发送消息失败，则再重试3次
    for (size_t i = 0; i < 4; i++)
    {
        try
        {
            _SeedMsg(s);
            break;
        }
        catch (const Exception &e)
        {
            // 捕获异常，if中是102号错误，网络不稳定，则重新发送
            // 捕获异常，else中不是102号错误，则将异常重新抛出
            if (e.getid() == 102)
            {
                // 重试三次以后否失败了，则说明网络太差了，重新抛出异常
                if (i == 3)
                    throw;
                cout << "开始第" << i + 1 << "重试" << endl;
            }
            else
            {
                throw;
            }
        }
    }
}

int main()
{
    srand(time(0));
    string str;
    while (cin >> str)
    {
        try
        {
            SendMsg(str);
        }
        catch (const Exception &e)
        {
            cout << e.what() << endl
                 << endl;
        }
        catch (...)
        {
            cout << "Unkown Exception" << endl;
        }
    }
    return 0;
}