// typedef int STDataType;
// typedef struct Stack
// {
// 	STDataType* a;
// 	int top;
// 	int capacity;
// }ST;

//后进先出

//使用动态顺序表实现栈
#include"Stack.h"

void STInit(ST* ps)
{
	assert(ps);

	ps->a = NULL;
	ps->top = 0;
	ps->capacity = 0;
}

void STDestroy(ST* ps)
{
	assert(ps);

	free(ps->a);
	ps->a = NULL;
	ps->top = ps->capacity = 0;
}

// ջ
// 11:55
void STPush(ST* ps, STDataType x)
{
	assert(ps);

	// ˣ 
	if (ps->top == ps->capacity)
	{
		int newcapacity = ps->capacity == 0 ? 4 : ps->capacity * 2;
		STDataType* tmp = (STDataType*)realloc(ps->a, newcapacity * sizeof(STDataType));
		if (tmp == NULL)
		{
			perror("realloc fail");
			return;
		}

		ps->a = tmp;
		ps->capacity = newcapacity;
	}

	ps->a[ps->top] = x;
	ps->top++;
}

void STPop(ST* ps)
{
	assert(ps);
	assert(!STEmpty(ps));

	ps->top--;//逻辑上的删除，不需要物理上的删除
}

STDataType STTop(ST* ps)
{
	assert(ps);
	assert(!STEmpty(ps));

	return ps->a[ps->top - 1];
}

int STSize(ST* ps)
{
	assert(ps);

	return ps->top;
}

bool STEmpty(ST* ps)
{
	assert(ps);

	return ps->top == 0;
}