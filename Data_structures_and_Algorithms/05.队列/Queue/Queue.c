#include"Queue.h"

// typedef int QDataType;
// typedef struct QueueNode
// {
// 	int val;
// 	struct QueueNode* next;
// }QNode;
//用链表来模拟队列

//后进先出

//初始化队列
void QueueInit(Queue* pq)
{
	assert(pq);

	pq->phead = NULL;
	pq->ptail = NULL;
	pq->size = 0;
}

void QueueDestroy(Queue* pq)
{
	assert(pq);

	QNode* cur = pq->phead;
	while (cur)
	{
		QNode* next = cur->next;
		free(cur);

		cur = next;
	}

	pq->phead = pq->ptail = NULL;
	pq->size = 0;
}

// 入队列
void QueuePush(Queue* pq, QDataType x)
{
	assert(pq);
	QNode* newnode = (QNode*)malloc(sizeof(QNode));
	if (newnode == NULL)
	{
		perror("malloc fail");
		return;
	}

	newnode->val = x;
	newnode->next = NULL;

	if (pq->ptail)
	{
		pq->ptail->next = newnode;
		pq->ptail = newnode;
	}
	else
	{
		pq->phead = pq->ptail = newnode;
	}

	pq->size++;
}

// 出队列
void QueuePop(Queue* pq)
{
	assert(pq);

	// 0个节点
	// 温柔检查
	//if (pq->phead == NULL)
	//	return;
	
	// 暴力检查 
	assert(pq->phead != NULL);

	// 一个节点
	
	if (pq->phead->next == NULL)
	{
		free(pq->phead);
		pq->phead = pq->ptail = NULL;
	}
	else// 多个节点 头删 尾指针不变，头指针后移一个节点 指向后一个节点的地址 ，释放第一个节点的地址，
	{
		QNode* next = pq->phead->next;
		free(pq->phead);
		pq->phead = next;
	}

	pq->size--;
}

QDataType QueueFront(Queue* pq)
{
	assert(pq);

	// 暴力检查 
	assert(pq->phead != NULL);

	return pq->phead->val;
}

QDataType QueueBack(Queue* pq)
{
	assert(pq);

	// 暴力检查 
	assert(pq->ptail != NULL);

	return pq->ptail->val;
}

bool QueueEmpty(Queue* pq)
{
	assert(pq);

	return pq->size == 0;
}

int QueueSize(Queue* pq)
{
	assert(pq);

	return pq->size;
}