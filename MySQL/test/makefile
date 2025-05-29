test:main.c
# gcc -o $@ $^ -std=c17 -L /usr/lib/x86_64-linux-gnu -lmysqlclient
	gcc -o $@ $^ -std=c17 -I./include -L./lib -lmysqlclient
.PHONY:clean
clean:
	rm -rf test