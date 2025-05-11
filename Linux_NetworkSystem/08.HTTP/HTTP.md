# 应用层协议 HTTP
## 前置知识
- 我们上网的所有行为都是在做IO，（我的数据给别人，别人的数据给我）
- 图片。视频，音频，文本等等，都是资源
- 答复前需要先确认我要的资源在哪台服务器上（网络IP），在确定资源在什么路径上（服务器资源路径）
## HTTP 协议
虽然我们说，应用层协议是我们程序猿自己定的。但实际上，已经有大佬们定义了一些现成的，又非常好用的应用层协议，供我们直接参考使用。`HTTP`（超文本传输协议）就是其中之一。

在互联网世界中，`HTTP`（HyperText Transfer Protocol，超文本传输协议）是一个至关重要的协议。它定义了客户端（如浏览器）与服务器之间如何通信，以交换或传输超文本（如HTML文档）。

HTTP协议是客户端与服务器之间通信的基础。客户端通过HTTP协议向服务器发送请求，服务器收到请求后处理并返回响应。HTTP协议是一个**无连接、无状态**的协议，即**每次请求都需要建立新的连接**，且服务器不会保存客户端的状态信息。

## 认识URL
平时我们俗称的“网址”其实就是说的URL
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/3fef29e092324d7e8451388ff7517962.png)
- `?`右侧为传参
- URL中，`/`不一定是根目录，其实叫做web根目录，两者不一定是同一个
- 成熟的应用层协议，往往是和端口号强相关的。
### urlencode和urldecode
像 / 、? 、: 等这样的字符，已经被url当做特殊意义理解了。因此这些字符不能随意出现。比如，某个参数中需要带有这些特殊字符，就必须先对特殊字符进行转义。

转义的规则如下：
将需要转码的字符转为16进制，然后从右到左，取4位（不足4位直接处理），每2位做一位，前面加上%，编码成%XY格式

例如：“+” 被转义成了 “%2B”
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/911e933e15154fe5a31b56b8b22e8765.png)

urldecode就是urlencode的逆过程；4

[urlencode工具](https://tool.chinaz.com/Tools/urlencode.aspx)

## HTTP协议请求与响应格式
### HTTP请求
```
POST http://job.xjtu.edu.cn/companyLogin.do HTTP/1.1
Host: job.xjtu.edu.cn
Connection: keep - alive
Content - Length: 36
Cache - Control: max - age=0
Origin: http://job.xjtu.edu.cn
Upgrade - Insecure - Requests: 1
Content - Type: application/x - www - form - urlencoded
User - Agent: Mozilla/5.0 (Windows NT 6.3; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/61.0.3163.100 Safari/537.36
Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8
Referer: http://job.xjtu.edu.cn/companyLogin.do
Accept - Encoding: gzip, deflate
Accept - Language: zh - CN,zh;q=0.8
Cookie: JSESSIONID=D628a75845a74D29D91DB47A461E4FC;
Hm_lvt_783e83ce0ee350e23a9d389df580f658=1504963710,1506661798;
Hm_lpvt_783e83ce0ee350e23a9d389df580f658=1506661802
username=hgtz2222&password=222222222
```
- 首行：[方法] + [url] + [版本]
- `Header`（请求报头）：请求的属性，冒号分割的键值对；每组属性之间使用\n分隔；遇到空行表示Header部分结束
- `Body`（请求正文）：空行后面的内容都是Body。Body允许为空字符串。如果Body存在，则在Header中会有一个Content - Length属性来标识Body的长度；
![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/02158ce8ef07443ab2d5f5fd8eaa0d00.png)
>注意：http协议自己做序列化和反序列化，偏底层，不想依赖任何库。



### HTTP响应
```
HTTP/1.1 200 OK
Server: YX1k waf
Content - Type: text/html; charset=UTF - 8
Content - Language: zh - CN
Transfer - Encoding: chunked
Date: Fri, 29 Sep 2017 05:10:13 GMT
<!DOCTYPE html>
<html>
<head>
<title>西安交通大学就业网</title>
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<meta http-equiv="X-UA-Compatible" content="IE=Edge">
<meta http-equiv="Content-Type" content="text/html; charset=utf-8" />
<link href="/shortcut icon/css/href/main.css" rel="stylesheet" media="screen" />
<link href="/renovation/css/font-awesome.css" rel="stylesheet" media="screen" />
<link href="/renovation/css/font-default.css" rel="stylesheet" media="screen" />
<script type="text/javascript" src="/renovation/js/jquery1.7.1.min.js"></script>
<script type="text/javascript" src="/renovation/js/main.js"></script><!--main-->
<link href="/style/warmipsstyle.css" rel="stylesheet" type="text/css">
</head>
```
- 首行：[版本号] + [状态码] + [状态码解释]
- Header：请求的属性，冒号分割的键值对；每组属性之间使用\n分隔；遇到空行表示Header部分结束
- Body：空行后面的内容都是Body。Body允许为空字符串。如果Body存在，则在Header中会有一个Content - Length属性来标识Body的长度；如果服务器返回了一个html页面，那么html页面内容就是在body中。



![在这里插入图片描述](https://i-blog.csdnimg.cn/direct/28045c30eceb4f8899c7ecf2aa87f9b0.png)
## HTTP的方法
|方法|说明|支持的HTTP协议版本|
| ---- | ---- | ---- |
|GET|获取资源|1.0、1.1|
|POST|传输主体|1.0、1.1|
|PUT|传输文件|1.0、1.1|
|HEAD|获得报文首部|1.0、1.1|
|DELETE|删除文件|1.0、1.1|
|OPTIONS|询问支持的方法|1.0、1.1|
|TRACE|追踪路径|1.1|
|CONNECT|要求用隧道协议连接代理|1.1|
|LINK|建立和资源之间的联系|1.0|
|UNLINK|断开连接关系|1.0|

其中最常用的就是`GET`方法和`POST`方法。


1. **GET方法（重点）**
    - **用途**：用于请求URL指定的资源。
    - **示例**：GET /index.html HTTP/1.1
    - **特性**：指定资源经服务器端解析后返回响应内容。
    - **form表单**：https://www.runoob.com/html/html-forms.html
    - **C++代码示例**：
```cpp
std::string GetFileContentHelper(const std::string &path)
{
    // 一份简单的读二进制文件的代码
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return "";
    in.seekg(0, in.end);
    int filesize = in.tellg();
    in.seekg(0, in.beg);

    std::string content;
    content.resize(filesize);
    in.read((char *)content.c_str(), filesize);

    // std::vector<char> content(filesize);
    // in.read(content.data(), filesize);

    in.close();

    return content;
}
```
2. **POST方法（重点）**
    - **用途**：用于传输实体的主体，通常用于提交表单数据。
    - **示例**：POST /submit.cgi HTTP/1.1
    - **特性**：可以发送大量的数据给服务器，并且数据包含在请求体中。
    - **form表单**：https://www.runoob.com/html/html-forms.html
    - **说明**：要通过历史写的http服务器，验证POST方法，这里需要了解一下FORM表单的问题。
>`GET`通常获取网页内容，传参通过URL（拼接到URL的后面）
>`POST`通常用来上传数据，传参通过body（正文）
3. **PUT方法（不常用）**
    - **用途**：用于传输文件，将请求主体中的文件保存到请求URL指定的位置。
    - **示例**：PUT /example.html HTTP/1.1
    - **特性**：不太常用，但在某些情况下，如RESTful API中，用于更新资源。
4. **HEAD方法**
    - **用途**：与GET方法类似，但不返回报文主体部分，仅返回响应头。
    - **示例**：HEAD /index.html HTTP/1.1
    - **特性**：用于确认URL的有效性及资源更新的日期时间等。
    - **C++示例**：
```bash
// curl -i 显示
$ curl -i www.baidu.com
HTTP/1.1 200 OK
Accept - Ranges: bytes
Cache - Control: private, no - cache, no - store, proxy - revalidate, no - transform
Connection: keep - alive
Content - Length: 2381
Content - Type: text/html
Date: Sun, 16 Jun 2024 08:38:04 GMT
Etag: "588604dc - 94d"
Last - Modified: Mon, 23 Jan 2017 13:27:56 GMT
Pragma: no - cache
Server: bfe/1.0.8.18
Set - Cookie: BDORZ=27315; max - age=86400; domain=.baidu.com; path=/
<!DOCTYPE html>
...

// 使用head方法，只会返回响应头
$ curl --head www.baidu.com
HTTP/1.1 200 OK
Accept - Ranges: bytes
Cache - Control: private, no - cache, no - store, proxy - revalidate, no - transform
Connection: keep - alive
Content - Length: 277
Content - Type: text/html
Date: Sun, 16 Jun 2024 08:43:38 GMT
Etag: "575e1f71 - 115"
Last - Modified: Mon, 13 Jun 2016 02:50:25 GMT
Pragma: no - cache
Server: bfe/1.0.8.18
```
5. **DELETE方法（不常用）**
    - **用途**：用于删除文件，是PUT的相反方法。
    - **示例**：DELETE /example.html HTTP/1.1
    - **特性**：按请求URL删除指定的资源。
6. **OPTIONS方法**
    - **用途**：用于查询针对请求URL指定的资源支持的方法。
    - **示例**：OPTIONS * HTTP/1.1
    - **特性**：返回允许的方法，如GET、POST等。
    - **C++示例（搭建nginx测试）**：
```bash
// 搭建一个nginx用来测试
// sudo apt install nginx
// sudo nginx -- 开启
// ps ajx | grep nginx -- 查看
// sudo nginx -s stop -- 停止服务

$ ps ajx | grep nginx
$ sudo nginx -s stop
$ ps ajx | grep nginx

// 指明方法
$ curl -X OPTIONS -i http://127.0.0.1/
HTTP/1.1 405 Not Allowed
Server: nginx/1.18.0 (Ubuntu)
Date: Sun, 16 Jun 2024 08:48:22 GMT
Content - Type: text/html
Content - Length: 165
Connection: keep - alive
Connection: keep - alive

<!DOCTYPE html>
<head><title>405 Not Allowed</title></head>
<body>
<center><h1>405 Not Allowed</h1></center>
<hr><center>nginx/1.18.0 (Ubuntu)</center>
</body>
```

```bash
// 支持的效果
$ curl -X OPTIONS -i http://127.0.0.1/
HTTP/1.1 200 OK
Allow: GET, HEAD, POST, OPTIONS
Content - Type: text/plain
Server: nginx/1.18.0 (Ubuntu)
Date: Sun, 16 Jun 2024 09:04:44 GMT
Access - Control - Allow - Origin: *
Access - Control - Allow - Headers: Content - Type, Authorization
// 注意：这里没有响应体，因为Content - Length为0
```

## HTTP的状态码
|状态码类别|类别描述|原因短语|
| ---- | ---- | ---- |
|1XX|Informational（信息性状态码）|接收的请求正在处理|
|2XX|Success（成功状态码）|请求正常处理完毕|
|3XX|Redirection（重定向状态码）|需要进行附加操作以完成请求|
|4XX|Client Error（客户端错误状态码）|服务器无法处理请求|
|5XX|Server Error（服务器错误状态码）|服务器处理请求出错|

最常见的状态码如200(OK)、404(Not Found)、403(Forbidden)、302(Redirect, 重定向) 。

|状态码|含义|应用样例|
| ---- | ---- | ---- |
|100|Continue|上传大文件时，服务器告诉客户端可以继续上传|
|200|OK|访问网站首页，服务器返回网页内容|
|201|Created|发布新文章，服务器返回文章创建成功的信息|
|204|No Content|删除文章后，服务器返回“无内容”表示操作成功|
|301|Permanently Moved|网站换域名后，搜索引擎更新网站，自动跳转到新域名|
|302|Found 或 See Other|用户登录成功后，重定向到用户首页|
|304|Not Modified|浏览器缓存机制，对未修改的资源返回304状态码|
|400|Bad Request|填写表单时，格式不正确导致提交失败|
|401|Unauthorized|访问需要登录的页面时，未登录或认证失败|
|403|Forbidden|尝试访问没有权限查看的页面|
|404|Not Found|访问不存在的网页链接|
|500|Internal Server Error|服务器崩溃或数据库错误导致页面无法加载|
|502|Bad Gateway|使用代理服务器时，代理服务器无法从上游服务器获取有效响应|
|503|Service Unavailable|服务器维护或过载，暂时无法处理请求|

### 关于重定向的验证
301代表永久重定向，302代表临时重定向，都依赖`Location`选项。
- **HTTP状态码301（永久重定向）**：表示请求的资源已经被永久移动到新的位置。在这种情况下，服务器会在响应中添加一个Location头部，用于指定资源的新位置。例如，在HTTP响应中，可能会看到类似于以下的头部信息：
```http
HTTP/1.1 301 Moved Permanently
Location: //www.new-url.com
```
- **HTTP状态码302（临时重定向）**：当服务器返回HTTP 302状态码时，表示请求的资源临时被移动到新位置。同样地，服务器也会在响应中添加一个Location头部来指定资源的新位置。浏览器会使用新的URL进行后续请求，**但不会缓存这个重定向**。例如，在HTTP响应中，可能会看到类似于以下的头部信息：
```http
HTTP/1.1 302 Found
Location: //www.new-url.com
```
总结：无论是HTTP 301还是HTTP 302重定向，都需要依赖Location选项来指定资源的新位置。在这两个状态码的HTTP响应头部，用于告诉浏览器应该将请求重定向到哪个新的URL地址。 


## HTTP常见Header
- **Content-Type**：数据类型（text/html等 ）
- **Content-Length**：Body的长度
- **Host**：客户端告知服务器，所请求的资源是在哪个主机的哪个端口上；
- **User-Agent**：声明用户的操作系统和浏览器版本信息；
- **referer**：当前页面是从哪个页面跳转过来的；
- **Location**：搭配3xx状态码使用，告诉客户端接下来要去哪里访问；
- **Cookie**：用于在客户端存储少量信息，通常用于实现会话（session）的功能；

### 关于connection报头
HTTP中的Connection字段是HTTP报文头的一部分，它主要用于控制和管理客户端与服务器之间的连接状态。

#### 核心作用
- **管理持久连接**：Connection字段还用于管理持久连接（也称为长连接）。持久连接允许客户端和服务器在请求/响应完成后不立即关闭TCP连接，以便在同一个连接上发送多个请求和接收多个响应。

### 持久连接（长连接）
- **HTTP/1.1**：在HTTP/1.1协议中，**默认使用持久连接**。当客户端和服务器都不明确指定关闭连接时，连接将保持打开状态，以便后续的请求和响应可以复用同一个连接。
- **HTTP/1.0**：在HTTP/1.0协议中，默认连接是非持久的。如果希望在HTTP/1.0上实现持久连接，需要在请求头中显式设置Connection: keep-alive。

#### 语法格式
- **Connection: keep-alive**：表示希望保持连接以复用TCP连接。
- **Connection: close**：表示请求/响应完成后，应该关闭TCP连接。


下面附上一张关于HTTP常见header的表格
|字段名|含义|样例|
| ---- | ---- | ---- |
|Accept|客户端可接受的响应内容类型|Accept:text/html,application/xhtml+xml,application/xml;q=0.9,image/webp,image/apng,*/*;q=0.8|
|Accept-Encoding|客户端支持的数据压缩格式|Accept-Encoding: gzip, deflate, br|
|Accept-Language|客户端可接受的语言类型|Accept-Language: zh-CN,zh;q=0.9,en;q=0.8|
|Host|请求的主机名和端口号|Host: www.example.com:8080|
|User-Agent|客户端的软件环境信息|User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/91.0.4472.124 Safari/537.36|
|Cookie|客户端发送给服务器的HTTP cookie信息|Cookie: session_id=abcdefg12345; user_id=123|
|Referer|请求的来源URL|Referer: http://www.example.com/previous_page.html|
|Content-Type|实体主体的媒体类型|Content-Type: application/x-www-form-urlencoded (对于表单提交) 或 Content-Type: application/json (对于JSON数据)|
|Content-Length|实体主体的字节大小|Content-Length: 150|
|Authorization|认证信息，如用户名和密码|Authorization: Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ== (Base64编码后的用户名:密码)|
|Cache-Control|缓存控制指令|请求时: Cache-Control: no-cache 或 Cache-Control: max-age=3600; 响应时: Cache-Control: public, max-age=3600|
|Connection|请求完后是关闭还是保持连接|Connection: keep-alive 或 Connection: close|
|Date|请求或响应的日期和时间|Date: Wed, 21 Oct 2023 07:28:00 GMT|
|Location|重定向的目标URL（与3xx状态码配合使用）|Location: http://www.example.com/new_location.html (与302状态码配合使用)|
|Server|服务器类型|Server: Apache/2.4.41 (Unix)|
|Last-Modified|资源的最后修改时间|Last-Modified: Wed, 21 Oct 2023 07:20:00 GMT|
|ETag|资源的唯一标识符，用于缓存|ETag: "3f80f-1b6-5f4e2512a4100"|
|Expires|响应过期的日期和时间|Expires: Wed, 21 Oct 2023 08:28:00 GMT|


