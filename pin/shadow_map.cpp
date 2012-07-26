#include "pin.H"
#include <iostream>
#include <fstream>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <map>
#include <set>
#include <string.h>
#include <sys/time.h>
#include <sys/resource.h>

#include <map>
#include "syscall_handle.h"
#include "inst_handle.h"
#include "shadow_map.h"

#define EXPAND_STACK "expand_stack"
#define MALLOC "malloc"
#define FREE "free"
#define MMAP "mmap"
#define STACK "[stack]"
#define HEAP "[heap]"

#if __x86_64__
	int x86 = 64;
#else
	int x86 = 32;
#endif

pid_t pid;

struct mov_count stack_count;
struct mov_count other_count;
struct mov_count heap_count;
struct mov_count global_count;

struct range heap_range;
struct range stack_range;
struct range global_range;

unsigned int offset;
unsigned long free_addr;

struct rlimit limit;

int byte_size = 4;

int heap_suc;
int heap_fail;

// read /proc/pid/maps to get memory mapping
void read_map()
{
	fstream proc_map;
	struct range temp;
	char buff[10];
	char name[20] = "/proc/";
	char line[256];
	char previous_line[256];

	sprintf(buff, "%d", pid);
	strncpy(name + 6, buff, strlen(buff));
	strcat(name, "/maps");
	
	proc_map.open (name);

	getrlimit(RLIMIT_STACK, &limit);

	while (!proc_map.eof()) {
		proc_map.getline(line, 256);
		int len = strlen(line);
		// Get Stack Size

		if (strncmp(line + len - strlen(STACK), STACK, strlen(STACK)) == 0) {
			sscanf(line, "%p-%p", &temp.lower_addr, &temp.upper_addr);

			temp.lower = (unsigned long)temp.lower_addr;
			temp.upper = (unsigned long)temp.upper_addr;

			if (stack_range.upper != temp.upper) {
				stack_range = temp;
				stack_range.lower = stack_range.upper - limit.rlim_cur;
				stack_range.lower_addr = (void *)stack_range.lower;

/*
				heap_range.upper = stack_range.lower;
				heap_range.upper_addr = (void *)stack_range.lower;
				*/
			}

		}
		// Get Global & Heap Size
//		else 
		if (strncmp(line + len - strlen(HEAP), HEAP, strlen(HEAP)) == 0) {
			sscanf(previous_line, "%p-%p", &temp.lower_addr, &temp.upper_addr);

			temp.lower = (unsigned long long)temp.lower_addr;
			temp.upper = (unsigned long long)temp.upper_addr;

			if (global_range.lower != temp.lower ||
				global_range.upper != temp.upper) {
				global_range = temp;
			}
			sscanf(line, "%p-%p", &temp.lower_addr, &temp.upper_addr);

			temp.lower = (unsigned long long)temp.lower_addr;
			temp.upper = (unsigned long long)temp.upper_addr;

			if (heap_range.lower != temp.lower ||
				heap_range.upper != temp.upper) {
				heap_range = temp;
			}
		}
		strcpy(previous_line, line);
	}
	proc_map.close();
}

// reserve memory from 0x2000000
void reserveShadowMemory()
{
	offset = (int) mmap(NULL, shadowMemSize, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
//	printf("Shadow Memory at %p\n", (void *)offset);
}

// remove shadow memory
void freeShadowMemory()
{
	int ret = syscall(__NR_munmap, (void *)offset, shadowMemSize);

	if (ret < 0)
		printf("Shadow Memory at %p Free Failed!\n", (void *)offset);
}

int checkShadowMap(int addr, int size)
{
	int ct = 0;
	char wh;
	unsigned long tmp_addr;
	unsigned long new_addr;
	unsigned char *temp_addr;

	tmp_addr = addr;
//	printf("Shadow Memory at %p\n", (void *)offset);
	for (int i = 0; i < size; i++) {
		new_addr = ((tmp_addr + i) >> 3) + offset;
		temp_addr = (unsigned char *)new_addr;
		wh = (tmp_addr + i) & 7;
//		printf("	check %p %p %d\n", (tmp_addr + i), temp_addr, *temp_addr);
		wh = (*temp_addr >> wh) & 1;
		ct += wh;
	}
	return ct;
}

// mark malloc
int markMalloc(unsigned long addr, int size)
{
	char wh;
	unsigned long tmp_addr;
	unsigned long new_addr;
	unsigned char *temp_addr;

	tmp_addr = addr;
	// mark shadow memory bit by bit
//	printf("Shadow Memory at %p\n", (void *)offset);
	for (int i = 0; i < size; i++) {
		new_addr = ((tmp_addr + i) >> 3) + offset;
		temp_addr = (unsigned char *)new_addr;
		wh = (tmp_addr + i) & 7;
//		printf("	mark %p %p %d\n", (tmp_addr + i), temp_addr, *temp_addr);
		*temp_addr = *temp_addr | (1 << wh);
	}

	return 0;
}

// unmark malloc
int unmarkMalloc(unsigned long addr, int size)
{
	unsigned long tmp_addr;
	unsigned long new_addr;
	unsigned char *temp_addr;

	tmp_addr = addr;
	// unmark shadow memory by byte
//	printf("unmark Memory at %p size %d\n", (void *)offset, size);
	for (int i = 0; i < size; i += 8) {
		new_addr = ((tmp_addr + i) >> 3) + offset;
		temp_addr = (unsigned char *)new_addr;
		// if 8 byte is going to be unmarked the shadow memory will be 0
		if (i + 8 < size) {
			*temp_addr = 0;
		}
		// if less than 8 bytes left
		else {
			*temp_addr = (*temp_addr >> (i + 8 - size)) << (i + 8 - size);
		}
	}

	return 0;
}

// write argument 
VOID MallocBefore(CHAR * name, ADDRINT size)
{
	if (!isMalloced) {
		malloc_size = size;
//		printf("Malloc %d\n", size);
		isMalloced = 1;
	}
}

// erase address when free
VOID BeforeFree(CHAR * name, ADDRINT addr)
{
	if (addr) {
		free_addr = addr;
		no_free = 1;
	}
}

// write return address
VOID MallocAfter(ADDRINT ret)
{
	int left_over;

	if (isMalloced) {
//		printf("Malloc %p\n", (void *)ret);
		isMalloced = 0;
		mlc_size[ret] = malloc_size;
		/* for align */
		left_over = 4 - malloc_size % 4;

		markMalloc((unsigned long)ret - byte_size, malloc_size + byte_size * 2 + left_over);
//		read_map();
	}
}

// after free
// check if memory mapping has changed
VOID AfterFree()
{
	if (no_free) {
		unmarkMalloc(free_addr, mlc_size.find(free_addr)->second);
		mlc_size.erase(free_addr);
//		read_map();
		no_free = 0;
	}
}
