.PHONY:all
all:tcp_server tcp_client

tcp_server:TcpServer.cc
	g++ -o $@ $^ -std=c++14
tcp_client:TcpClient.cc
	g++ -o $@ $^ -std=c++14

.PHONY:clean
clean:
	rm -rf tcp_server tcp_client
