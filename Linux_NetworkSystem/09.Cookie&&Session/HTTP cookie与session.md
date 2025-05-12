# HTTP cookie与session
## 引入HTTP Cookie
**定义**
HTTP Cookie（也称为Web Cookie、浏览器Cookie或简称Cookie）是**服务器发送到用户浏览器并保存在浏览器上的一小块数据**，它会在浏览器之后向同一服务器再次发起请求时被携带并发送到服务器上。通常，它用于告知服务端两个请求是否来自同一浏览器，如保持用户的登录状态、记录用户偏好等。

**工作原理**
- 当用户第一次访问网站时，服务器会在响应的HTTP头中设置Set - Cookie字段，用于发送Cookie到用户的浏览器。
- 浏览器在接收到Cookie后，会将其保存在本地（通常是按照域名进行存储）。 
- 在之后的请求中，浏览器会自动在HTTP请求头中携带Cookie字段，将之前保存的Cookie信息发送给服务器。

**分类**
- 会话`Cookie`（Session Cookie）：在浏览器关闭时失效。
- 持久`Cookie`（Persistent Cookie）：带有明确的过期日期或持续时间，可以跨多个浏览器会话存在。
- 如果cookie是一个持久性的cookie，那么它其实就是浏览器相关的，特定目录下的一个文件。但直接查看这些文件可能会看到乱码或无法读取的内容，因为cookie文件通常以二进制或sqlite格式存储。一般我们查看，直接在浏览器对应的选项中直接查看即可。

**安全性**
由于Cookie是存储在客户端的，因此存在被篡改或窃取的风险。

**用途**
- 用户认证和会话管理（最重要）
- 跟踪用户行为
- 缓存用户偏好等
- 比如在chrome浏览器下，可以直接访问：chrome://settings/cookies

## 认识cookie

 HTTP存在一个报头选项：`Set - Cookie`，可以用来进行给浏览器设置Cookie值。
- 在HTTP响应头中添加，客户端（如浏览器）获取并自行设置并保存Cookie。
### 格式
**基本格式**
```cpp
Set - Cookie: <name>=<value>
```

其中<name>是Cookie的名称，<value>是Cookie的值。

**完整的Set - Cookie示例**

```cpp
Set - Cookie: username=peter; expires=Thu, 18 Dec 2024 12:00:00 UTC; path=/; domain=.example.com; secure; HttpOnly
```

时间格式必须遵守RFC 1123标准，具体格式样例：Tue, 01 Jan 2030 12:34:56 GMT或者UTC（推荐）。

### 关于时间解释

 Tue：星期二（星期几的缩写）
- ,：逗号分隔符
- 01：日期（两位数表示）
- Jan：一月（月份的缩写）
- 2030：年份（四位数）
- 12:34:56：时间（小时、分钟、秒）
- GMT：格林威治标准时间（时区缩写）

**GMT vs UTC**
- **GMT（格林威治标准时间）**：是以英国伦敦的格林威治区为基准的世界时间标准，不受夏令时或其他因素的影响，通常用于航海、航空、科学、天文等领域，计算方式基于地球的自转和公转。
- **UTC（协调世界时）**：是国际电信联盟（ITU）制定和维护的标准时间，计算方式基于原子钟，比GMT更准确，据称世界上最精确的原子钟50亿年才会误差1秒 ，是现在用的时间标准，多数全球性的网络和软件系统将其作为标准时间。 

区别:
- 计算方式：GMT基于地球的自转和公转，而UTC基于原子钟。
- 准确度：由于UTC基于原子钟，它比基于地球自转的GMT更加精确。在实际使用中，GMT和UTC之间的差别通常很小，大多数情况下可以互换使用。但在需要高精度时间计量的场合，如科学研究、网络通信等，UTC是更为准确的选择。 

### 关于其他可选属性的解释
- `expires=<date>`[要验证]：设置Cookie的过期日期/时间。如果未指定此属性，则Cookie默认为会话Cookie，即当浏览器关闭时过期。
- `path=<some_path>`[要验证]：限制Cookie发送到服务器的哪些路径。默认为设置它的路径。 
- `domain=<domain_name>`[了解即可]：指定哪些主机可以接受该Cookie。默认为设置它的主机。 
- `secure`[了解即可]：仅当使用HTTPS协议时才发送Cookie。这有助于防止Cookie在不安全的HTTP连接中被截获。 
- `HttpOnly`[了解即可]：标记Cookie为HttpOnly，意味着该Cookie不能被客户端脚本（如JavaScript）访问。这有助于防止跨站脚本攻击（XSS）。

## 对Set - Cookie头部字段的简洁介绍
|属性|值|描述|
| ---- | ---- | ---- |
|username|peter|这是Cookie的名称和值，标识用户名为"peter"。|
|expires|Thu, 18 Dec 2024 12:00:00 UTC|指定Cookie的过期时间。在这个例子中，Cookie将在2024年12月18日12:00:00 UTC后过期。|
|path|/|定义Cookie的作用范围。这里设置为根路径/，意味着Cookie对.example.com域名下的所有路径都可用。|
|domain|.example.com|指定哪些域名可以接收这个Cookie。点前缀（.）表示包括所有子域名。|
|secure|-|指示Cookie只能通过HTTPS协议发送，不能通过HTTP协议发送，增加安全性。|
|HttpOnly|-|阻止客户端脚本（如JavaScript）访问此Cookie，有助于防止跨站脚本攻击（XSS）。|

### 注意事项
- 每个Cookie属性都以分号（;）和空格（ ）分隔。 
- 名称和值之间使用等号（=）分隔。 
- 如果Cookie的名称或值包含特殊字符（如空格、分号、逗号等），则需要进行URL编码。

## Cookie的生命周期
- 如果设置了expires属性，则Cookie将在指定的日期/时间后过期。 
- 如果没有设置expires属性，则Cookie默认为会话Cookie，即当浏览器关闭时过期。

## 安全性考虑
- 使用secure标志可以确保Cookie仅在HTTPS连接上发送，从而提高安全性。 
- 使用HttpOnly标志可以防止客户端脚本（如JavaScript）访问Cookie，从而防止XSS攻击。 
- 通过合理设置Set - Cookie的格式和属性，可以确保Cookie的安全性、有效性和可访问性，从而满足Web应用程序的需求。

## 实验测试cookie
- [测试cookie的代码](https://gitee.com/xixi-sakura/personal_computer_learning/tree/main/Linux_NetworkSystem/09.Cookie&&Session/cookie)
- chrome浏览器查看cookie不方便，我们推荐使用windows自带浏览器：Microsoft Edge应用


### 测试cookie写入到浏览器

![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/894da3101f4b428e9823a1c99b43b8d7.png)

### 测试自动提交
- 刷新浏览器，刚刚写入的cookie会自动被提交给服务器端
```
_req_line: GET /favicon.ico HTTP/1.1
---> Host: 8.137.19.140:8888
---> Connection: keep - alive
---> User - Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36 Edg/126.0.0
---> Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,image/webp,image/*,*/*;q=0.8
---> Referer: http://8.137.19.140:8888
---> Accept - Encoding: gzip, deflate
---> Accept - Language: zh - CN,zh;q=0.9,en;q=0.8,en - GB;q=0.7,en - US;q=0.6
---> Cookie: username=zhangsan
```

### 测试写入过期时间
- 这里要由我们自己形成UTC统一标准时间，下面是对应的C++样例代码，以供参考
```cpp
std::string GetMonthName(int month)
{
    std::vector<std::string> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    return months[month];
}
std::string GetWeekDayName(int day)
{
    std::vector<std::string> weekdays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    return weekdays[day];
}
std::string ExpireTimeUseRfc1123(int t) // 秒级别的未来UTC时间
{
    time_t timeout = time(nullptr) + t; // 这个地方有坑哦
    struct tm *tm = gmtime(&timeout); // 这里不能用localtime，因为localtime是默认带了时区的. gmtime获取的就是UTC统一时间
    char timebuffer[1024]; //时间格式如: expires=Thu, 18 Dec 2024 12:00:00 UTC
    snprintf(timebuffer, sizeof(timebuffer),
            "%s, %02d %s %d %02d:%02d:%02d UTC",
            GetWeekDayName(tm->tm_wday).c_str(),
            tm->tm_mday,
            GetMonthName(tm->tm_mon).c_str(), tm->tm_year+1900,
            tm->tm_hour,
            tm->tm_min, tm->tm_sec
            );
    return timebuffer;
}
```

### 测试路径path
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/21236a76755f4e309e93c669138b5b41.png)


 **附上部分核心代码**
```cpp
class Http {
private:
    std::string GetMonthName(int month) {
        std::vector<std::string> months = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        return months[month];
    }
    std::string GetWeekDayName(int day) {
        std::vector<std::string> weekdays = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        return weekdays[day];
    }
    std::string ExpireTimeUseRfc1123(int t) { 
        time_t timeout = time(nullptr) + t;
        struct tm *tm = gmtime(&timeout); 
        char timebuffer[1024];
        snprintf(timebuffer, sizeof(timebuffer),
                "%s, %02d %s %d %02d:%02d:%02d UTC",
                GetWeekDayName(tm->tm_wday).c_str(),
                tm->tm_mday,
                GetMonthName(tm->tm_mon).c_str(), tm->tm_year + 1900,
                tm->tm_hour,
                tm->tm_min, tm->tm_sec
                );
        return timebuffer;
    }
public:
    Http(uint16_t port) {
        _tsvr = std::make_unique<TcpServer>(port,
        std::bind(&Http::HandlerHttp, this, std::placeholders::_1));
        _tsvr->Init();
    }
    std::string ProveCookieWrite() { 
        return "Set - Cookie: username=zhangsan;";
    }
    std::string ProveCookieTimeOut() { 
        return "Set - Cookie: username=zhangsan; expires=" + ExpireTimeUseRfc1123(60) + ";"; 
    }
    std::string ProvePath() { 
        return "Set - Cookie: username=zhangsan; path=/a/b;";
    }
    std::string HandlerHttp(std::string request) {
        HttpRequest req;
        req.Deserialize(request);
        req.Debug();
        lg.LogMessage(Debug, "%s\n", ExpireTimeUseRfc1123(60).c_str());
        HttpResponse resp;
        resp.SetCode(200);
        resp.SetDesc("OK");
        resp.AddHeader("Content - Type: text/html");
        //resp.AddHeader(ProveCookieWrite()); //测试cookie被写入与自动提交
        //resp.AddHeader(ProveCookieTimeOut()); //测试过期时间的写入
        resp.AddHeader(ProvePath()); // 测试路径
        resp.AddContent("<html><h1>helloworld</h1></html>");
        return resp.Serialize();
    }
    void Run() {
        _tsvr->Start();
    }
    ~Http() {}
private:
    std::unique_ptr<TcpServer> _tsvr;
};
```
- 通过注释绿色区域，即可完成测试

## Cookie问题
- 我们写入的是测试数据，如果写入的是用户的私密数据呢？比如，用户名密码，浏览痕迹等。
- 本质问题在于这些用户私密数据在浏览器(用户端)保存，非常容易被人盗取，更重要的是，除了被盗取，还有就是用户私密数据也就泄漏了。

## 引入HTTP Session
**定义**
**HTTP是无状态的，服务器无法跟踪用户的服务交互，因此服务器通过Session机制来记住用户的信息**。

**工作原理**
- 当用户首次访问网站时，服务器会为用户创建一个唯一的Session ID，并通过Cookie将其发送到客户端。
- 客户端在之后的请求中会携带这个Session ID，服务器通过Session ID来识别用户，从而获取用户的会话信息。
- 服务器通常会将Session信息存储在内存、数据库或缓存中。

**安全性**
- 与Cookie相似，由于Session ID是在客户端和服务器之间传递的，因此也存在被窃取的风险。
- 但是一般虽然Cookie被盗取了，但是用户只泄漏了一个Session ID，私密信息暂时没有被泄露的风险。
- Session便于服务器进行客户端有效性的管理，比如异地登录。
- 可以通过HTTPS和设置合适的Cookie属性（如HttpOnly和Secure）来增强安全性。

**超时和失效**
- Session可以设置超时时间，当超过这个时间后，Session会自动失效。
- 服务器也可以主动使Session失效，例如当用户登出时。

**用途**
- 用户认证和会话管理
- 存储分布式系统的数据（如购物车内容）
- 实现分布式系统的会话共享（通过将会话数据存储在共享数据库或缓存中）

## 模拟Session行为 

[代码](https://gitee.com/xixi-sakura/personal_computer_learning/tree/main/Linux_NetworkSystem/09.Cookie&&Session/session)

## 总结

HTTP Cookie和Session都是用于在Web应用中跟踪用户状态的机制。Cookie是存
储在客户端的，而Session是存储在服务器端的。它们各有优缺点，通常在实际应用
中会结合使用，以达到最佳的用户体验和安全性。

