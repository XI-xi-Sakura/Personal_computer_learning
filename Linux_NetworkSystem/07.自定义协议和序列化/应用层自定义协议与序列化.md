# 应用层自定义协议与序列化

## 应用层

我们程序员写的一个个解决我们实际问题,满足我们日常需求的网络程序,都是在应用层.

协议是一种"约定".Socket的接口,在读写数据时,都是按"字符串"的方式来发送接收的.如果我们要传输一些"结构化的数据"怎么办呢?

>其实，协议就是双方约定好的结构化的数据

## 网络版计算器

例如,我们需要实现一个服务器版的加法器.我们需要客户端把要计算的两个加数发过去, 然后由服务器进行计算,最后再把结果返回给客户端.

约定方案一:
 - 客户端发送一个形如"1+1"的字符串;
 - 这个字符串中有两个操作数,都是整形;
 - 两个数字之间会有一个字符是运算符,运算符只能是+;
 - 数字和运算符之间没有空格;
 - ...

约定方案二:
 - 定义结构体来表示我们需要交互的信息;
 - 发送数据时将这个结构体按照一个规则转换成字符串,接收到数据的时候再按照相同的规则把字符串转化回结构体;
 - 这个过程叫做"序列化"和"反序列化"

## 序列化和反序列化
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/f5331019323e4553b6aed4508cb83784.png)

无论我们采用方案一,还是方案二,还是其他的方案,只要保证,一端发送时构造的数据,在另一端能够正确的进行解析,就是ok的.这种约定,就是**应用层协议**

但是，为了让我们深刻理解协议，打算自定义实现一下协议的过程。
- 我们采用方案2，我们也要体现协议定制的细节
- 我们要引入序列化和反序列化，只不过我们课堂直接采用现成的方案--jsoncpp库
- 我们要对socket进行字节流的读取处理


## 理解read、write、recv、send和tcp支持全双工
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/0c892757f8f6403295091d09c77608e6.png)

所以：
- 在任何一台主机上，TCP连接既有发送缓冲区，又有接受缓冲区，所以，在内核中，可以在发消息的同时，也可以收消息，即**全双工**

- 这就是为什么一个tcpsockfd读写都是它的原因

- 实际数据什么时候发，发多少，出错了怎么办，由TCP控制，所以TCP叫做**传输控制协议**

## Json
### Jsoncpp
Jsoncpp是一个用于处理JSON数据的C++库。它提供了将JSON数据序列化为字符串以及从字符串反序列化为C++数据结构的功能。Jsoncpp是开源的，广泛用于各种需要处理JSON数据的C++项目中。
### 特性
1. **简单易用**：Jsoncpp提供了直观的API，使得处理JSON数据变得简单。
2. **高性能**：Jsoncpp的性能经过优化，能够高效地处理大量JSON数据。
3. **全面支持**：支持JSON标准中的所有数据类型，包括对象、数组、字符串、数字、布尔值和null。
4. **错误处理**：在解析JSON数据时，Jsoncpp提供了详细的错误信息和位置，方便开发者调试。
当使用Jsoncpp库进行JSON的序列化和反序列化时，确实存在不同的做法和工具类可供选择。以下是对Jsoncpp中序列化和反序列化操作的详细介绍：
### 安装
**C++**
- ubuntu: sudo apt - get install libjsoncpp - dev
- Centos: sudo yum install jsoncpp - devel
### 序列化
序列化指的是将数据结构或对象转换为一种格式，以便在网络上传输或存储到文件中。Jsoncpp提供了多种方式进行序列化：
1. **使用Json::Value的toStyledString方法**
    - **优点**：将Json::Value对象直接转换为格式化的JSON字符串。
    - **示例**：
```cpp
#include <iostream>
#include <jsoncpp/json/json.h>

int main()
{
    Json::Value root;
    root["name"] = "joe";
    root["sex"] = "男";
    std::string s = root.toStyledString();
    std::cout << s << std::endl;
    return 0;
}
```
执行结果：
```
{
    "name" : "joe",
    "sex" : "男"
}
```
2. **使用Json::StreamWriter**
    - **优点**：提供了更多的定制选项，如缩进、换行符等。
    - **示例**：
```cpp
#include <iostream>
#include <string>
#include <memory>
#include <jsoncpp/json/json.h>

int main()
{
    Json::Value root;
    root["name"] = "joe";
    root["sex"] = "男";
    Json::StreamWriterBuilder wbuilder; // StreamWriter的工厂
    std::unique_ptr<Json::StreamWriter> writer(wbuilder.newStreamWriter());
    std::stringstream ss;
    writer->write(root, &ss);
    std::cout << ss.str() << std::endl;
    return 0;
}
```
执行结果：
```
{
    "name" : "joe",
    "sex" : "男"
}
```
3. **使用Json::FastWriter**
    - **优点**：比StyledWriter更快，因为它不添加额外的空格和换行符。
    - **示例**：
```cpp
#include <iostream>
#include <string>
#include <memory>
#include <jsoncpp/json/json.h>

int main()
{
    Json::Value root;
    root["name"] = "joe";
    root["sex"] = "男";
    Json::FastWriter writer;
    std::string s = writer.write(root);
    std::cout << s << std::endl;
    return 0;
}
```
执行结果：
```
{"name":"joe","sex":"男"}
```
另一种写法示例：
```cpp
#include <iostream>
#include <string>
#include <memory>
#include <jsoncpp/json/json.h>

int main()
{
    Json::Value root;
    root["name"] = "joe";
    root["sex"] = "男";
    Json::StyledWriter writer;
    std::string s = writer.write(root);
    std::cout << s << std::endl;
    return 0;
}
```
执行结果：
```
{
    "name" : "joe",
    "sex" : "男"
}
```
### 反序列化
反序列化指的是将序列化后的数据重新转换为原来的数据结构或对象。Jsoncpp提供了以下方法进行反序列化：
1. **使用Json::Reader**
    - **优点**：提供详细的错误信息和位置，方便调试。
    - **示例**：
```cpp
#include <iostream>
#include <jsoncpp/json/json.h>

int main() {
    std::string json_string = "{\"name\":\"张三\",\"age\":30,\"city\":\"北京\"}";
    // 解析JSON字符串
    Json::Value root;
    Json::Reader reader;
    bool parsingSuccessful = reader.parse(json_string, root);
    if (!parsingSuccessful) {
        // 解析失败，输出错误信息
        std::cout << "Failed to parse JSON: " << reader.getFormattedErrorMessages() << std::endl;
        return 1;
    }
    // 访问JSON数据
    std::string name = root["name"].asString();
    int age = root["age"].asInt();
    std::string city = root["city"].asString();
    // 输出结果
    std::cout << "Name: " << name << std::endl;
    std::cout << "Age: " << age << std::endl;
    std::cout << "City: " << city << std::endl;
    return 0;
}
```
执行结果：
```
Name: 张三
Age: 30
City: 北京
```
2. **使用Json::CharReader的派生类(不推荐，上面的足够了)**
在某些情况下，你可能需要更精细地控制解析过程，可以直接使用Json::CharReader的派生类。
  但通常情况下，使用Json::parseFromString或Json::Reader的parse方法就足够了。
### 总结
你可以用toStyledString、StreamWriter和FastWriter提供了不同的序列化选项，它们提供强大的错误处理机制。
Json::Reader和parseFromString函数是Jsoncpp中主要的反序列化工具，在进行序列化和反序列化时，请确保处理所有可能的错误情况，并验证输入和输出的有效性。
### Json::Value
Json::Value是Jsoncpp库中的一个重要类，用于表示和操作JSON数据结构。以下是一些常用的Json::Value操作列表：
1. **构造函数**
    - **Json::Value()**：默认构造函数，创建一个空的Json::Value对象。
    - **Json::Value(Json::ValueType type, bool aValue = false)**：根据给定的类型创建一个Json::Value对象。
2. **访问元素**
    - **Json::Value& operator[](const char* key)**：通过键（字符串）访问对象中的元素。如果键不存在，则创建一个新的元素。
    - **Json::Value& operator[](const std::string& key)**：同上，但使用std::string类型的键。
    - **Json::Value& operator[](ArrayIndex index)**：通过索引访问数组中的元素。如果索引超出范围，则创建一个新的元素。
    - **Json::Value at(const char* key)**：通过键访问对象中的元素，如果键不存在则抛出异常。
    - **Json::Value at(const std::string& key)**：同上，但使用std::string类型的键。
3. **类型检查**
    - **bool isNull()**：检查值是否为null类型。
    - **bool isBool()**：检查值是否为布尔类型。
    - **bool isInt()**：检查值是否为32位整数类型。
    - **bool isInt64()**：检查值是否为64位整数类型。
    - **bool isUInt()**：检查值是否为无符号整数类型。
    - **bool isUInt64()**：检查值是否为64位无符号整数类型。
    - **bool isIntegral()**：检查值是否为整数或可转换为整数的浮点数。
    - **bool isDouble()**：检查值是否为双精度浮点数。
    - **bool isNumeric()**：检查值是否为数字（整数或浮点数）。
    - **bool isString()**：检查值是否为字符串。
    - **bool isArray()**：检查值是否为数组。
    - **bool isObject()**：检查值是否为对象（即键值对的集合）。
4. **赋值和类型转换**
    - **Json::Value& operator=(bool value)**：将布尔值赋给Json::Value对象。
    - **Json::Value& operator=(int value)**：将32位整数赋给Json::Value对象。
    - **Json::Value& operator=(int64_t value)**：将64位整数赋给Json::Value对象。
    - **Json::Value& operator=(uint value)**：将无符号整数赋给Json::Value对象。
    - **Json::Value& operator=(uint64_t value)**：将64位无符号整数赋给Json::Value对象。
    - **Json::Value& operator=(double value)**：将双精度浮点数赋给Json::Value对象。
    - **Json::Value& operator=(const char* value)**：将C字符串赋给Json::Value对象。
    - **Json::Value& operator=(const std::string& value)**：将std::string赋给Json::Value对象。
    - **bool asBool()**：将值转换为布尔类型（如果可能）。
    - **int asInt()**：将值转换为32位整数类型（如果可能）。
    - **Int64 asInt64()**：将值转换为64位整数类型（如果可能）。
    - **UInt64 asUInt64()**：将值转换为64位无符号整数类型（如果可能）。
    - **std::string asString()**：将值转换为字符串类型（如果可能）。
5. **数组和对象操作**
    - **size_t size()**：返回数组或对象中的元素数量。
    - **bool empty()**：检查数组或对象是否为空。
    - **void resize(ArrayIndex newSize)**：调整数组的大小。
    - **void clear()**：删除数组或对象中的所有元素。
    - **void append(const Json::Value& value)**：在数组末尾添加一个新元素。
    - **Json::Value& operator[](const char* key, const Json::Value& defaultValue = Json::nullValue)**：在对象中插入或访问一个元素，如果键不存在则使用默认值。
    - **Json::Value& operator[](const std::string& key, const Json::Value& defaultValue = Json::nullValue)**：在对象中插入或访问一个元素，如果键不存在则使用默认值，但使用std::string类型的键。 

