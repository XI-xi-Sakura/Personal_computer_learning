#define _CRT_SECURE_NO_WARNINGS 1
#pragma once
#include<stdio.h>
#include<stdlib.h>
#include<assert.h>
#include<stdbool.h>
#include<string.h>
#include<time.h>

typedef int HPDataType;

typedef struct Heap
{
	HPDataType* a;
	int size;
	int capacity;
}HP;

void HPInit(HP* php);
void HPInitArray(HP* php, HPDataType* a, int n);

void HPDestroy(HP* php);
// 插入后保持数据是堆
void HPPush(HP* php, HPDataType x);
HPDataType HPTop(HP* php);

// 删除堆顶的数据
void HPPop(HP* php);

bool HPEmpty(HP* php);

void AdjustUp(HPDataType* a, int child);
void AdjustDown(HPDataType* a, int n, int parent);
void Swap(HPDataType* px, HPDataType* py);