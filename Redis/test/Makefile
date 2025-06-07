.PHONY:all
all:hello generic string list set hash zset

zset:zset.cc
	g++ -std=c++17 -o $@ $^ /usr/local/lib/libredis++.a /usr/lib/x86_64-linux-gnu/libhiredis.a -pthread

hash:hash.cc
	g++ -std=c++17 -o $@ $^ /usr/local/lib/libredis++.a /usr/lib/x86_64-linux-gnu/libhiredis.a -pthread

set:set.cc
	g++ -std=c++17 -o $@ $^ /usr/local/lib/libredis++.a /usr/lib/x86_64-linux-gnu/libhiredis.a -pthread

list:list.cc
	g++ -std=c++17 -o $@ $^ /usr/local/lib/libredis++.a /usr/lib/x86_64-linux-gnu/libhiredis.a -pthread

string:string.cc
	g++ -std=c++17 -o $@ $^ /usr/local/lib/libredis++.a /usr/lib/x86_64-linux-gnu/libhiredis.a -pthread

hello:hello.cc
	g++ -std=c++17 -o $@ $^ /usr/local/lib/libredis++.a /usr/lib/x86_64-linux-gnu/libhiredis.a -pthread

generic:generic.cc
	g++ -std=c++17 -o $@ $^ /usr/local/lib/libredis++.a /usr/lib/x86_64-linux-gnu/libhiredis.a -pthread

.PHONY:clean
clean:
	rm hello
	rm generic
	rm string
	rm list
	rm set
	rm hash
	rm zset