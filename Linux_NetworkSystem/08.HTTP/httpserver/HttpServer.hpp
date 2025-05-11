#pragma once
#include <iostream>
#include <string>
#include <functional>
#include <unordered_map>
#include "TcpServer.hpp"
#include "HttpProtocol.hpp"

using namespace TcpServerModule;

using http_handler_t = std::function<void(HttpRequest &, HttpResponse &)>;

class HttpServer
{
public:
    HttpServer(int port)
        : _tsvr(std::make_unique<TcpServer>(port))
    {
    }
    void Resgiter(std::string funcname, http_handler_t func)
    {
        _route[funcname] = func;
    }
    bool SafeCheck(const std::string &service)
    {
        auto iter = _route.find(service);
        return iter != _route.end();
    }
    void Start()
    {
        _tsvr->InitServer([this](SockPtr sockfd, InetAddr client)
                          { return this->HandlerHttpRequest(sockfd, client); });
        _tsvr->Loop();
    }
    // 就是我们处理http的入口
    bool HandlerHttpRequest(SockPtr sockfd, InetAddr client)
    {
        LOG(LogLevel::DEBUG) << "HttpServer: get a new client: " << sockfd->Fd() << " addr info: " << client.Addr();
        std::string http_request;
        sockfd->Recv(&http_request); // 在HTTP这里，我们不做报文完整性的处理
        HttpRequest req;
        req.Deserialize(http_request);
        HttpResponse resp;
        // 请求应该被分成两类: 1. 请求一般的静态资源 2. 提交参数，携带参数，需要我们进行交互设置
        if (req.IsHasArgs())
        {
            std::cout << "-----IsHasArgs()" << std::endl;
            // 2. 提交参数，携带参数，需要我们进行交互设置
            std::string service = req.Path();
            if (SafeCheck(service))
                _route[req.Path()](req, resp); // /login
            else
                resp.Build(req); // debug
        }
        else
        {
            std::cout << "-----Non IsHasArgs()" << std::endl;
            resp.Build(req);
        }
        std::string resp_str;
        resp.Serialize(&resp_str);
        sockfd->Send(resp_str);
        return true;

        // std::cout << http_request << std::endl; //字节流的请求
        //  读取请求，对请求进行分析处理 --> 文本处理！
        //  1. 读取请求的完整性 --- 暂时不做
        //  2. 完整http反序列化，http response序列化...
        //  demo 1 : 直接返回一个固定的内容
        //  std::string status_line = "HTTP/1.1 200 OK" + Sep + BlankLine;

        // // 直接返回一个html网页
        // std::string body = "<!DOCTYPE html>\
        //                     <html>\
        //                    <head>\
        //                    <meta charset = \"UTF-8\">\
        //                    <title> Hello World</title>\
        //                    </head>\
        //                    <body>\
        //                    <p> Hello World</ p>\
        //                    </body> </html>";

        // std::string httpresponse = status_line + body;
        // sockfd->Send(httpresponse);
        return true;
    }
    ~HttpServer() {}

private:
    std::unique_ptr<TcpServer> _tsvr;
    std::unordered_map<std::string, http_handler_t> _route; // 功能路由
};