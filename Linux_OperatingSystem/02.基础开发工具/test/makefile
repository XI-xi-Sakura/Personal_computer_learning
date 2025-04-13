SRC=$(wildcard *.c)
OBJ=$(SRC:.c=.o)
BIN=processbar

$(BIN):$(OBJ)
        gcc -o $@ $^
%.o:%.c
        gcc -c $<

.PHONY:
clean:
        rm -f $(OBJ) $(BIN)