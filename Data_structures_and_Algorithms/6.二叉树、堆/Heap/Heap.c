#include"Heap.h"


// typedef int HPDataType;

// typedef struct Heap
// {
// 	HPDataType* a;
// 	int size;
// 	int capacity;
// }HP;

void HPInit(HP* php)
{
	assert(php);
	php->a = NULL;
	php->size = 0;
	php->capacity = 0;
}

void HPInitArray(HP* php, HPDataType* a, int n)
{
	assert(php);
	
	php->a = (HPDataType*)malloc(sizeof(HPDataType) * n);
	if (php->a == NULL)
	{
		perror("malloc fail");
		return;
	}
	memcpy(php->a, a, sizeof(HPDataType) * n);
	php->capacity = php->size = n;

	// 向上调整，建堆 O(N*logN)
	//for (int i = 1; i < php->size; i++)
	//{
	//	AdjustUp(php->a, i);
	//}//向上调整，从堆顶元素开始

	// 向下调整，建堆 O(N)
	for (int i = (php->size-1 - 1)/2; i >= 0; i--)
	{
		AdjustDown(php->a, php->size, i);//向下调整，从堆底元素开始
	}
}

void HPDestroy(HP* php)
{
	assert(php);
	free(php->a);
	php->a = NULL;
	php->capacity = 0;
	php->size = 0;
}

void Swap(HPDataType* px, HPDataType* py)
{
	HPDataType tmp = *px;
	*px = *py;
	*py = tmp;
}

void AdjustUp(HPDataType* a, int child)// 向上调整算法 O(logN)大根堆
{
	int parent = (child - 1) / 2;
	//while (parent >= 0)
	while(child > 0)
	{
		if (a[child] > a[parent])
		{
			Swap(&a[child], &a[parent]);
			child = parent;
			parent = (parent - 1) / 2;
		}
		else
		{
			break;
		}
	}
}

// 时间复杂度：
void HPPush(HP* php, HPDataType x)
{
	assert(php);

	if (php->size == php->capacity)
	{
		size_t newCapacity = php->capacity == 0 ? 4 : php->capacity * 2;
		HPDataType* tmp = realloc(php->a, sizeof(HPDataType) * newCapacity);
		if (tmp == NULL)
		{
			perror("realloc fail");
			return;
		}
		php->a = tmp;
		php->capacity = newCapacity;
	}

	php->a[php->size] = x;
	php->size++;

	AdjustUp(php->a, php->size-1);
}

HPDataType HPTop(HP* php)
{
	assert(php);

	return php->a[0];
}

void AdjustDown(HPDataType* a, int n, int parent)// 向下调整算法 O(logN)小根堆
// 左右孩子中小的那个孩子与父亲交换，直到没有孩子或者父亲比孩子小为止
// 左右孩子中小的那个孩子的下标是：child = parent * 2 + 1;
{
	int child = parent * 2 + 1;
	while (child < n)
	{
		// 假设法，选出左右孩子中小的那个孩子
		if (child+1 < n && a[child + 1] < a[child])
		{
			++child;
		}

		if (a[child] < a[parent])//父亲节点与孩子节点比较
		{
			Swap(&a[child], &a[parent]);
			parent = child;
			child = parent * 2 + 1;
		}
		else
		{
			break;
		}
	}
}

// 时间复杂度：logN
void HPPop(HP* php)
{
	assert(php);
	assert(php->size > 0);

	Swap(&php->a[0], &php->a[php->size - 1]);
	php->size--;

	AdjustDown(php->a, php->size, 0);
}


bool HPEmpty(HP* php)
{
	assert(php);

	return php->size == 0;
}