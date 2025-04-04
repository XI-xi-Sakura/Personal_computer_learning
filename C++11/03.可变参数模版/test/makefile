# SRC = $(wildcard *.cpp)
# OBJ = $(patsubst %.cpp, %.o, $(SRC))
 
# ALL: test
 
# test: $(OBJ)
# 	g++ $^ -o $@
 
# $(OBJ): $(SRC)
# 	g++ -Wall -c $< -o $@
 
# clean:
# 	rm -rf $(OBJ) test

# process:process.cc
# 	g++ -o $@ $^ -std=c++11

# .PHONY:clean
# clean:
# 	rm -f process


# SRC = $(wildcard *.cpp)
# OBJ = $(patsubst %.cpp, %.o, $(SRC))

# ALL: test

# test: $(OBJ)
# 	g++ $^ -o $@

# %.o: %.cpp
# 	g++ -Wall -c $< -o $@

# .PHONY:clean
# clean:
# 	rm -rf $(OBJ) test


# 查找当前目录下所有的 .cpp 文件
SRC = $(wildcard *.cpp)
# 将 .cpp 文件转换为 .o 文件
OBJ = $(patsubst %.cpp, %.o, $(SRC))
# 最终要生成的可执行文件
TARGET = main

# 默认目标，生成可执行文件
all: $(TARGET)

# 生成可执行文件的规则
$(TARGET): $(OBJ)
	g++ $^ -o $@

# 生成 .o 文件的规则
%.o: %.cpp
	g++ -c $^ -o $@

# 清理生成的文件
.PHONY: clean
clean:
	rm -f $(OBJ) $(TARGET)