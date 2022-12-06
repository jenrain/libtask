/* Copyright (c) 2005-2006 Russ Cox, MIT; see COPYRIGHT */

#if defined(__sun__)
#	define __EXTENSIONS__ 1 /* SunOS */
#	if defined(__SunOS5_6__) || defined(__SunOS5_7__) || defined(__SunOS5_8__)
		/* NOT USING #define __MAKECONTEXT_V2_SOURCE 1 / * SunOS */
#	else
#		define __MAKECONTEXT_V2_SOURCE 1
#	endif
#endif

#define USE_UCONTEXT 1

#if defined(__OpenBSD__) || defined(__mips__)
#undef USE_UCONTEXT
#define USE_UCONTEXT 0
#endif

#if defined(__APPLE__)
#include <AvailabilityMacros.h>
#if defined(MAC_OS_X_VERSION_10_5)
#undef USE_UCONTEXT
#define USE_UCONTEXT 0
#endif
#endif

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sched.h>
#include <signal.h>
#if USE_UCONTEXT
#include <ucontext.h>
#endif
#include <sys/utsname.h>
#include <inttypes.h>
#include "task.h"

#define nil ((void*)0)
#define nelem(x) (sizeof(x)/sizeof((x)[0]))

#define ulong task_ulong
#define uint task_uint
#define uchar task_uchar
#define ushort task_ushort
#define uvlong task_uvlong
#define vlong task_vlong

typedef unsigned long ulong;
typedef unsigned int uint;
typedef unsigned char uchar;
typedef unsigned short ushort;
typedef unsigned long long uvlong;
typedef long long vlong;

#define print task_print
#define fprint task_fprint
#define snprint task_snprint
#define seprint task_seprint
#define vprint task_vprint
#define vfprint task_vfprint
#define vsnprint task_vsnprint
#define vseprint task_vseprint
#define strecpy task_strecpy

int print(char*, ...);
int fprint(int, char*, ...);
char *snprint(char*, uint, char*, ...);
char *seprint(char*, char*, char*, ...);
int vprint(char*, va_list);
int vfprint(int, char*, va_list);
char *vsnprint(char*, uint, char*, va_list);
char *vseprint(char*, char*, char*, va_list);
char *strecpy(char*, char*, char*);

#if defined(__FreeBSD__) && __FreeBSD__ < 5
extern	int		getmcontext(mcontext_t*);
extern	void		setmcontext(const mcontext_t*);
#define	setcontext(u)	setmcontext(&(u)->uc_mcontext)
#define	getcontext(u)	getmcontext(&(u)->uc_mcontext)
extern	int		swapcontext(ucontext_t*, const ucontext_t*);
extern	void		makecontext(ucontext_t*, void(*)(), int, ...);
#endif

#if defined(__APPLE__)
#	define mcontext libthread_mcontext
#	define mcontext_t libthread_mcontext_t
#	define ucontext libthread_ucontext
#	define ucontext_t libthread_ucontext_t
#	if defined(__i386__)
#		include "386-ucontext.h"
#	elif defined(__x86_64__)
#		include "amd64-ucontext.h"
#	else
#		include "power-ucontext.h"
#	endif	
#endif

#if defined(__OpenBSD__)
#	define mcontext libthread_mcontext
#	define mcontext_t libthread_mcontext_t
#	define ucontext libthread_ucontext
#	define ucontext_t libthread_ucontext_t
#	if defined __i386__
#		include "386-ucontext.h"
#	else
#		include "power-ucontext.h"
#	endif
extern pid_t rfork_thread(int, void*, int(*)(void*), void*);
#endif

#if 0 &&  defined(__sun__)
#	define mcontext libthread_mcontext
#	define mcontext_t libthread_mcontext_t
#	define ucontext libthread_ucontext
#	define ucontext_t libthread_ucontext_t
#	include "sparc-ucontext.h"
#endif

#if defined(__arm__)
int getmcontext(mcontext_t*);
void setmcontext(const mcontext_t*);
#define	setcontext(u)	setmcontext(&(u)->uc_mcontext)
#define	getcontext(u)	getmcontext(&(u)->uc_mcontext)
#endif

#if defined(__mips__)
#include "mips-ucontext.h"
int getmcontext(mcontext_t*);
void setmcontext(const mcontext_t*);
#define	setcontext(u)	setmcontext(&(u)->uc_mcontext)
#define	getcontext(u)	getmcontext(&(u)->uc_mcontext)
#endif

typedef struct Context Context;

enum
{
	STACK = 8192
};

struct Context
{
	ucontext_t	uc;
};

// 一个Task可以看成是一个需要异步执行的任务，coroutine的抽象描述
struct Task
{
	char	name[256];	// offset known to acid
	char	state[256];
    // 前后指针
	Task	*next;
	Task	*prev;
	Task	*allnext;
	Task	*allprev;
    // 执行上下文
	Context	context;
    // 睡眠时间
	uvlong	alarmtime;
	uint	id;
    // 协程栈指针
	uchar	*stk;
    // 协程栈大小
	uint	stksize;
    // 协程是否退出了
	int	exiting;
    // 在在alltask的中的索引下标
	int	alltaskslot;
    // 是否是系统协程
	int	system;
    // 是否在就绪状态
	int	ready;
    // Task需要执行的函数
	void	(*startfn)(void*);
    // startfn的参数
	void	*startarg;
    // 自定义数据
	void	*udata;
};

// 修改协程状态为就绪态
void	taskready(Task*);
void	taskswitch(void);

void	addtask(Tasklist*, Task*);
void	deltask(Tasklist*, Task*);

// 就绪态协程链表
extern Task	*taskrunning;
// 协程的数量
extern int	taskcount;
