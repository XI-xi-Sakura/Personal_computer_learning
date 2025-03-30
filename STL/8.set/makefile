# 查找当前目录下所有的 .cpp 文件
SRC = $(wildcard *.cpp)
# 将 .cpp 文件转换为 .o 文件
OBJ = $(patsubst %.cpp, %.o, $(SRC))
# 最终要生成的可执行文件，将每个 .cpp 文件对应一个可执行文件
TARGETS = $(patsubst %.cpp, %, $(SRC))

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