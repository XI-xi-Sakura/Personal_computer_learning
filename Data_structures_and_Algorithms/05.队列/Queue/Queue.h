#pragma once
#include<stdlib.h>
#include<stdbool.h>
#include<assert.h>


typedef int QDataType;
typedef struct QueueNode
{
	int val;
	struct QueueNode* next;
}QNode;

//// 入队列
//void QueuePush(QNode** pphead, QNode** pptail);
//
//// 出队列
//void QueuePop(QNode** pphead, QNode** pptail);

typedef struct Queue
{
	QNode* phead;
	QNode* ptail;
	int size;
}Queue;

void QueueInit(Queue* pq);
void QueueDestroy(Queue* pq);

// 入队列
void QueuePush(Queue *pq, QDataType x);
// 出队列
void QueuePop(Queue* pq);

QDataType QueueFront(Queue* pq);
QDataType QueueBack(Queue* pq);
bool QueueEmpty(Queue* pq);
int QueueSize(Queue* pq);