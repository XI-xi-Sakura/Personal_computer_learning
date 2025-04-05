# lambda
## lambda表达式语法
lambda表达式本质是一个**匿名函数对象**，跟普通函数不同的是它可以定义在函数内部。lambda表达式语法使用层而言没有类型，所以一般是用auto或者模板参数定义的对象去接收lambda对象。

lambda表达式的格式：`[capture-list] (parameters) mutable-> return type { function boby }`
- `[capture-list]`：捕捉列表，该列表总是出现在lambda函数的开始位置，编译器根据`[]`来判断接下来的代码是否为lambda函数，捕捉列表能够捕捉上下文中的变量供lambda函数使用，捕捉列表可以传值和传引用捕捉。捕捉列表为空也不能省略。
- `(parameters)`：参数列表，与普通函数的参数列表功能类似，如果不需要参数传递，则可以连同`()`一起省略。
- `mutable`(慎用)：默认情况下，lambda表达式纵使一个const函数，mutable可以去下其常量性。
- `->return type`：返回值类型，用**追踪返回类型形式**声明函数的返回值类型，没有返回值时此部分可省略。**一般返回值类型明确情况下，也可省略，由编译器对返回类型进行推导。**
- `{function boby}`：函数体，函数体内的实现跟普通函数完全类似，在该函数体内，除了可以使用其参数外，还可以使用所有捕获到的变量，函数体为空也不能省略。

注：在lambda函数定义中，参数列表和返回值类型都是可选部分，而捕捉列表和函数体可以为空，但不能省略，因此C++11中最简单的lambda函数为`[ ]{ }`；（该lambda函数不能做任何事情）
```cpp
int main()
{
    // 一个简单的lambda表达式
    auto add1 = [](int x, int y)->int {return x + y; };
    cout << add1(1, 2) << endl;

    // 1、捕捉为空也不能省略
    // 2、参数为空可以省略
    // 3、返回值可以省略，可以通过返回对象自动推导
    // 4、函数题不能省略
    auto func1 = [] 
    {
        cout << "hello bit" << endl;
        return 0;
    };
    func1();

    int a = 0, b = 1;
    auto swap1 = [](int& x, int& y)
    {
        int tmp = x;
        x = y;
        y = tmp;
    };
    swap1(a, b);
    cout << a << ":" << b << endl;

    return 0;
}
```
对于lambda表达式构造
- 允许一个lambda表达式拷贝构造一个新的副本
- 可以将lambda表达式赋值给相同类型的函数指针

```cpp
void (*pf) ();
int main( )
{
	auto f1=[ ]{cout<<"hello"<<endl;}
	auto f2=[ ]{cout<<"hello"<<endl;}
	
	//f1=f2 编译失败，找不到operator=( )

    auto f3 (f2);//允许一个lambda表达式拷贝构造一个新的副本


	pf =f2;//可以将lambda表达式赋值给相同类型的函数指针
	pf();
}
```

## 捕捉列表
lambda表达式中默认**只能用lambda函数体和参数中的变量**，如果想用外层作用域中的变量就需要进行捕捉。而捕捉列表描述了上下文中哪些数据可以被lambda使用，以及使用方法（传值还是传引用）。
- 参数介绍
	- [var] 表示传值方式捕捉变量 var
	- [ = ]  表示值传递方式捕获所有父作用域中的变量（this）
	- [ &var]表示引用传递捕捉变量var
	- [ & ] 表示引用传递方式捕获所有父作用域中的变量
	- [ this] 表示值传递方式捕获当前的this指针
- 捕捉方式 
	- 第一种捕捉方式是在捕捉列表中显示的传值捕捉和传引用捕捉，捕捉的多个变量用逗号分割。`[x, y, &z]`表示x和y值捕捉，z引用捕捉。
	- 第二种捕捉方式是在捕捉列表中隐式捕捉，在捕捉列表写一个`=`表示隐式值捕捉，在捕捉列表写一个`&`表示隐式引用捕捉，这样lambda表达式中用了哪些变量，编译器就会自动捕捉那些变量。
	- 第三种捕捉方式是在捕捉列表中混合使用隐式捕捉和显示捕捉。`[=, &x]`表示其他变量隐式值捕捉，x引用捕捉；`[&, x, y]`表示其他变量引用捕捉，x和y值捕捉。当使用混合捕捉时，第一个元素必须是`&`或`=`，并且`&`混合捕捉时，后面的捕捉变量必须是值捕捉，同理`=`混合捕捉时，后面的捕捉变量必须是引用捕捉。
	
- 注：
	- 父作用域包含lambda表达式的语句块
	- 捕捉列表不允许变量重复传递，否则就会导致编译错误
	- 在块作用域中的lambda函数仅能捕捉父作用域中局部变量，捕捉任何非此作用域或者非局部变量都会导致编译报错
	- lambda表达式如果定义在全局位置，捕捉列表必须为空
	- 传值捕捉的过来的对象不能修改

lambda表达式如果在函数局部域中，它可以捕捉lambda位置之前定义的变量，不能捕捉静态局部变量和全局变量，静态局部变量和全局变量也不需要捕捉，lambda表达式中可以直接使用。**这也意味着lambda表达式如果定义在全局位置，捕捉列表必须为空。**

默认情况下，lambda捕捉列表是被const修饰的，也就是说**传值捕捉的过来的对象不能修改**，`mutable`加在参数列表的后面**可以取消其常量性**，也就是说使用该修饰符后，传值捕捉的对象就可以修改了，但是修改还是形参对象，不会影响实参。使用该修饰符后，参数列表不可省略（即使参数为空）。
```cpp
int x = 0;
// 捕捉列表必须为空，因为全局变量不用捕捉就可以用，没有可被捕捉的变量
auto func1 = []()
{
    x++;
};

int main()
{
    // 只能用当前lambda局部域和捕捉的对象和全局对象
    int a = 0, b = 1, c = 2, d = 3;
    auto func1 = [a, &b]
    {
        // 值捕捉的变量不能修改，引用捕捉的变量可以修改
        //a++;
        b++;
        int ret = a + b;
        return ret;
    };
    cout << func1() << endl;

    // 隐式值捕捉
    // 用了哪些变量就捕捉哪些变量
    auto func2 = [=]
    {
        int ret = a + b + c;
        return ret;
    };
    cout << func2() << endl;

    // 隐式引用捕捉
    // 用了哪些变量就捕捉哪些变量
    auto func3 = [&]
    {
        c++;
        d++;
    };
    func3();
    cout << a <<" "<< b <<" "<< c <<" "<< d <<endl;

    // 混合捕捉1
    auto func4 = [&, a, b]
    {
        //a++;
        //b++;
        c++;
        d++;
        return a + b + c + d;
    };
    func4();
    cout << a << " " << b << " " << c << " " << d << endl;

    // 混合捕捉1
    auto func5 = [=, &a, &b]
    {
        a++;
        b++;
        /*c++;
        d++;*/
        return a + b + c + d;
    };
    func5();
    cout << a << " " << b << " " << c << " " << d << endl;

    // 局部的静态和全局变量不能捕捉，也不需要捕捉
    static int m = 0;
    auto func6 = []
    {
        int ret = x + m;
        return ret;
    };

    // 传值捕捉本质是一种拷贝，并且被const修饰了
    // mutable相当于去掉const属性，可以修改了
    // 但是修改了不会影响外面被捕捉的值，因为是一种拷贝
    auto func7 = [=]()mutable
    {
        a++;
        b++;
        c++;
        d++;
        return a + b + c + d;
    };
    cout << func7() << endl;
    cout << a << " " << b << " " << c << " " << d << endl;

    return 0;
}
```
## lambda的应用
在学习lambda表达式之前，使用的可调用对象只有**函数指针**和**仿函数**对象，函数指针的类型定义起来比较麻烦，仿函数要定义一个类，相对会比较麻烦。使用lambda去定义可调用对象，既简单又方便。
lambda在很多其他地方用起来也很好用。比如线程中定义线程的执行函数逻辑，智能指针中定制删除器等，lambda的应用还是很广泛的，以后会不断接触到。
```cpp
struct Goods
{
    string _name; // 名字
    double _price; // 价格
    int _evaluate; // 评价
    // ...

    Goods(const char* str, double price, int evaluate)
    :_name(str)
    , _price(price)
    , _evaluate(evaluate)
    {}
};

struct ComparePriceLess
{
    bool operator()(const Goods& gl, const Goods& gr)
    {
        return gl._price < gr._price;
    }
};

struct ComparePriceGreater
{
    bool operator()(const Goods& gl, const Goods& gr)
    {
        return gl._price > gr._price;
    }
};

int main()
{
    vector<Goods> v = { { "苹果", 2.1, 5 }, { "香蕉", 3, 4 }, { "橙子", 2.2, 3 }, { "菠萝", 1.5, 4 } };
    // 类似这样的场景，实现仿函数对象或者函数指针支持商品中不同项的比较，相对还是比较麻烦的，那么这里lambda就很好用了
    sort(v.begin(), v.end(), ComparePriceLess());
    sort(v.begin(), v.end(), ComparePriceGreater());

    sort(v.begin(), v.end(), [](const Goods& g1, const Goods& g2) {
        return g1._price < g2._price;
        });

    sort(v.begin(), v.end(), [](const Goods& g1, const Goods& g2) {
        return g1._price > g2._price; 
        });

    sort(v.begin(), v.end(), [](const Goods& g1, const Goods& g2) {
        return g1._evaluate < g2._evaluate;
        });

    sort(v.begin(), v.end(), [](const Goods& g1, const Goods& g2) {
        return g1._evaluate > g2._evaluate;
        });

    return 0;
}
```
## lambda的原理
lambda的原理和范围for很像，编译后从汇编指令层的角度看，压根就没有lambda和范围for这样的东西。范围for底层是迭代器，**而lambda底层是仿函数对象**，也就是说写了一个lambda以后，编译器会生成一个对应的仿函数的类。
仿函数的类名是编译按一定规则生成的，保证不同的lambda生成的类名不同，lambda参数/返回类型/函数体就是仿函数`operator()`的参数/返回类型/函数体，lambda的捕捉列表**本质是生成的仿函数类的成员变量**，也就是说捕捉列表的变量都是lambda类构造函数的实参，当然隐式捕捉，编译器要看使用哪些就传那些对象。
```cpp
class Rate
{
public:
    Rate(double rate) 
    : _rate(rate)
    {}

    double operator()(double money, int year)
    {
        return money * _rate * year;
    }

private:
    double _rate;
};

int main()
{
    double rate = 0.49;

    // lambda
    auto r2 = [rate](double money, int year) {
        return money * rate * year;
    };

    // 函数对象
    Rate r1(rate);
    r1(10000, 2);

    r2(10000, 2);

    auto func1 = [] {
        cout << "hello world" << endl;
    };
    func1();

    return 0;
}
```

