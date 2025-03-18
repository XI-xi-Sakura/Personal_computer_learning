#include"List.h"
LTNode* LTBuyNode(LTDataType x) {
	LTNode* newnode = (LTNode*)malloc(sizeof(LTNode));
	if (newnode == NULL) {
		perror("malloc fail!");
		exit(1);
	}
	newnode->data = x;
	newnode->next = newnode->prev = newnode;

	return newnode;
}
//void LTInit(LTNode** pphead) {
//	*pphead = (LTNode*)malloc(sizeof(LTNode));
//	if (*pphead == NULL) {
//		perror("malloc fail!");
//		exit(1);
//	}
//	(*pphead)->data = -1;
//	(*pphead)->next = (*pphead)->prev = *pphead;
//}
LTNode* LTInit() {
	LTNode* phead = LTBuyNode(-1);
	return phead;
}

//尾插
void LTPushBack(LTNode* phead, LTDataType x) {
	assert(phead);
	LTNode* newnode = LTBuyNode(x);
	//phead phead->prev(ptail)  newnode
	newnode->next = phead;
	newnode->prev = phead->prev;

	phead->prev->next = newnode;
	phead->prev = newnode;
}
//头插
void LTPushFront(LTNode* phead, LTDataType x) {
	assert(phead);

	LTNode* newnode = LTBuyNode(x);
	//phead newnode phead->next
	newnode->next = phead->next;
	newnode->prev = phead;

	phead->next->prev = newnode;
	phead->next = newnode;
}

void LTPrint(LTNode* phead) {
	//phead不能为空
	assert(phead);
	LTNode* pcur = phead->next;
	while (pcur != phead)
	{
		printf("%d->", pcur->data);
		pcur = pcur->next;
	}
	printf("\n");
}
//尾删
void LTPopBack(LTNode* phead) {
	assert(phead);
	//链表为空：只有一个哨兵位节点
	assert(phead->next != phead);

	LTNode* del = phead->prev;
	LTNode* prev = del->prev;

	prev->next = phead;
	phead->prev = prev;

	free(del);
	del = NULL;
}
//头删
void LTPopFront(LTNode* phead) {
	assert(phead);
	assert(phead->next != phead);

	LTNode* del = phead->next;
	LTNode* next = del->next;

	//phead del next
	next->prev = phead;
	phead->next = next;

	free(del);
	del = NULL;
}
LTNode* LTFind(LTNode* phead, LTDataType x) {
	assert(phead);
	LTNode* pcur = phead->next;
	while (pcur != phead)
	{
		if (pcur->data == x) {
			return pcur;
		}
		pcur = pcur->next;
	}
	return NULL;
}
//在pos位置之后插入数据
void LTInsert(LTNode* pos, LTDataType x) {
	assert(pos);
	LTNode* newnode = LTBuyNode(x);
	//pos newnode pos->next
	newnode->next = pos->next;
	newnode->prev = pos;

	pos->next->prev = newnode;
	pos->next = newnode;
}
//删除pos位置的数据
void LTErase(LTNode* pos) {
	assert(pos);

	//pos->prev pos  pos->next
	pos->next->prev = pos->prev;
	pos->prev->next = pos->next;

	free(pos);
	pos = NULL;
}

//void LTDesTroy(LTNode** pphead) {
//	assert(pphead);
//	//哨兵位不能为空
//	assert(*pphead);
//
//	LTNode* pcur = (*pphead)->next;
//	while (pcur != *pphead)
//	{
//		LTNode* next = pcur->next;
//		free(pcur);
//		pcur = next;
//	}
//	//链表中只有一个哨兵位
//	free(*pphead);
//	*pphead = NULL;
//}
void LTDesTroy(LTNode* phead) {
	//哨兵位不能为空
	assert(phead);

	LTNode* pcur = phead->next;
	while (pcur != phead)
	{
		LTNode* next = pcur->next;
		free(pcur);
		pcur = next;
	}
	//链表中只有一个哨兵位
	free(phead);
	phead = NULL;
}