#pragma once
#include<stdio.h>
#include<stdlib.h>
#include<assert.h>

//定义双向链表中节点的结构
typedef int LTDataType;
typedef struct ListNode {
	LTDataType data;
	struct ListNode* prev;
	struct ListNode* next;
}LTNode;

//注意，双向链表是带有哨兵位的，插入数据之前链表中必须要先初始化一个哨兵位
//void LTInit(LTNode** pphead);
LTNode* LTInit();
//void LTDesTroy(LTNode** pphead);
void LTDesTroy(LTNode* phead);   //推荐一级（保持接口一致性）

void LTPrint(LTNode* phead);

//不需要改变哨兵位，则不需要传二级指针
//如果需要修改哨兵位的话，则传二级指针
void LTPushBack(LTNode* phead, LTDataType x);
void LTPushFront(LTNode* phead, LTDataType x);

//头删、尾删
void LTPopBack(LTNode* phead);
void LTPopFront(LTNode* phead);

//查找
LTNode* LTFind(LTNode* phead, LTDataType x);

//在pos位置之后插入数据
void LTInsert(LTNode* pos, LTDataType x);
//删除pos位置的数据
void LTErase(LTNode* pos);