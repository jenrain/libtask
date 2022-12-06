/* Copyright (c) 2005 Russ Cox, MIT; see COPYRIGHT */

#include "taskimpl.h"
#include <fcntl.h>
#include <stdio.h>

int	taskdebuglevel;
// 就绪态协程的数量
int	taskcount;
// 协程切换的次数
int	tasknswitch;
int	taskexitval;
// 就绪态协程列表
Task	*taskrunning;

// 当前执行协程的上下文
Context	taskschedcontext;
// taskrunqueue记录了所有就绪的协程
Tasklist	taskrunqueue;

// 保存所有协程的数组
Task	**alltask;
// alltask数组的迭代器
int		nalltask;

static char *argv0;
static	void		contextswitch(Context *from, Context *to);

static void
taskdebug(char *fmt, ...)
{
	va_list arg;
	char buf[128];
	Task *t;
	char *p;
	static int fd = -1;

return;
	va_start(arg, fmt);
	vfprint(1, fmt, arg);
	va_end(arg);
return;

	if(fd < 0){
		p = strrchr(argv0, '/');
		if(p)
			p++;
		else
			p = argv0;
		snprint(buf, sizeof buf, "/tmp/%s.tlog", p);
		if((fd = open(buf, O_CREAT|O_WRONLY, 0666)) < 0)
			fd = open("/dev/null", O_WRONLY);
	}

	va_start(arg, fmt);
	vsnprint(buf, sizeof buf, fmt, arg);
	va_end(arg);
	t = taskrunning;
	if(t)
		fprint(fd, "%d.%d: %s\n", getpid(), t->id, buf);
	else
		fprint(fd, "%d._: %s\n", getpid(), buf);
}

static void
taskstart(uint y, uint x)
{
	Task *t;
	ulong z;

	z = x<<16;	/* hide undefined 32-bit shift from 32-bit compilers */
	z <<= 16;
	z |= y;
	t = (Task*)z;

//print("taskstart %p\n", t);
	t->startfn(t->startarg);
//print("taskexits %p\n", t);
	taskexit(0);
//print("not reacehd\n");
}

static int taskidgen;

/*
 * taskalloc函数的主要逻辑是申请Task结构体所需的内存和执行时栈的内存，
 * 然后初始化各个字段。在此之后，一个协程就被创建成功了，接着执行taskready
 * 把协程加入就绪队列中。
 * */
static Task*
taskalloc(void (*fn)(void*), void *arg, uint stack)
{
	Task *t;
	sigset_t zero;
	uint x, y;
	ulong z;

	/* allocate the task and stack together */
	// 结构体本身的大小和栈大小
	t = malloc(sizeof *t+stack);
	if(t == nil){
		fprint(2, "taskalloc malloc: %r\n");
		abort();
	}
	memset(t, 0, sizeof *t);
	// 栈的内存位置
	t->stk = (uchar*)(t+1);
	// 栈大小
	t->stksize = stack;
	// 协程id
	t->id = ++taskidgen;
	// 协程工作函数和参数
	t->startfn = fn;
	t->startarg = arg;

	/* do a reasonable initialization */
	memset(&t->context.uc, 0, sizeof t->context.uc);
	sigemptyset(&zero);
	// 初始化uc_sigmask字段为空，即不阻塞信号
	sigprocmask(SIG_BLOCK, &zero, &t->context.uc.uc_sigmask);

	/* must initialize with current context */
	// 初始化uc字段
	if(getcontext(&t->context.uc) < 0){
		fprint(2, "getcontext: %r\n");
		abort();
	}

	/* call makecontext to do the real work. */
	/* leave a few words open on both ends */
	// 设置协程执行时的栈位置和大小
	t->context.uc.uc_stack.ss_sp = t->stk+8;
	t->context.uc.uc_stack.ss_size = t->stksize-64;
#if defined(__sun__) && !defined(__MAKECONTEXT_V2_SOURCE)		/* sigh */
#warning "doing sun thing"
	/* can avoid this with __MAKECONTEXT_V2_SOURCE but only on SunOS 5.9 */
	t->context.uc.uc_stack.ss_sp = 
		(char*)t->context.uc.uc_stack.ss_sp
		+t->context.uc.uc_stack.ss_size;
#endif
	/*
	 * All this magic is because you have to pass makecontext a
	 * function that takes some number of word-sized variables,
	 * and on 64-bit machines pointers are bigger than words.
	 */
//print("make %p\n", t);
	z = (ulong)t;
	y = z;
	z >>= 16;	/* hide undefined 32-bit shift from 32-bit compilers */
	x = z>>16;
	// 保存信息到uc字段
	makecontext(&t->context.uc, (void(*)())taskstart, 2, y, x);

	return t;
}

int
taskcreate(void (*fn)(void*), void *arg, uint stack)
{
	int id;
	Task *t;

    // 分配task和stack的空间
	t = taskalloc(fn, arg, stack);
	// 协程的数量+1
	taskcount++;
	id = t->id;
	if(nalltask%64 == 0){
		alltask = realloc(alltask, (nalltask+64)*sizeof(alltask[0]));
		if(alltask == nil){
			fprint(2, "out of memory\n");
			abort();
		}
	}
    // 记录位置
	t->alltaskslot = nalltask;
    // 保存到alltask中
	alltask[nalltask++] = t;
    // 修改状态为就绪，可以被调度，并且加入到就绪队列
	taskready(t);
	return id;
}

void
tasksystem(void)
{
	if(!taskrunning->system){
		taskrunning->system = 1;
		--taskcount;
	}
}

/*
 * 切换协程
 * */
void
taskswitch(void)
{
	needstack(0);
	// 将当前正在执行的协程的taskrunning->context保存到taskschedcontext中
	contextswitch(&taskrunning->context, &taskschedcontext);
}

/*
 * 修改协程的状态并加入就绪队列
 * */
void
taskready(Task *t)
{
	t->ready = 1;
	addtask(&taskrunqueue, t);
}

/*
 * 协程主动让出CPU
 * 1.将主动让出的协程重新加入到就绪队列
 * 2.将当前协程的状态标记为让出
 * 3.执行协程切换的逻辑
 * */
int
taskyield(void)
{
	int n;
	
	// 协程的让出次数
	n = tasknswitch;

	// 将当前主动让出的协程放进等待队列
	taskready(taskrunning);
	// 标记当前协程的状态为“让出”
	taskstate("yield");
	// 切换协程
	taskswitch();
	// 等于0说明当前只有自己一个协程，调度的时候taskswitch加1，所以这里要减1
	return tasknswitch - n - 1;
}

int
anyready(void)
{
	return taskrunqueue.head != nil;
}

void
taskexitall(int val)
{
	exit(val);
}

void
taskexit(int val)
{
	taskexitval = val;
	taskrunning->exiting = 1;
	taskswitch();
}

/*
 * 交换两个上下文信息
 * */
static void
contextswitch(Context *from, Context *to)
{
	if(swapcontext(&from->uc, &to->uc) < 0){
		fprint(2, "swapcontext failed: %r\n");
		assert(0);
	}
}

/*
 * 协程的调度器
 * 调度器的核心逻辑是：
 * 1.从就绪队列中拿出一个协程t，并把t移出就绪队列
 * 2.通过contextswitch将协程t的上下文信息切换到taskschedcontext中执行
 * 3.将协程t切换回调度中心，如果t已经退出，修改数据结构，然后回收他的内存，然后继续调度其它的协程执行
 * 这里的调度机制比较简单，是非抢占式的协作式调度，没有时间片的概念，一个协程的执行时间由自己决定，放弃执行的权力也是自己控制的，当协程不想执行了可以调用taskyield让出cpu
 * */
static void
taskscheduler(void)
{
	int i;
	Task *t;

	taskdebug("scheduler enter");
	for(;;){
		// 如果没有就绪态协程了，就退出
		if(taskcount == 0)
			exit(taskexitval);
		// 从就绪队列中拿出一个协程
		t = taskrunqueue.head;
		if(t == nil){
			fprint(2, "no runnable tasks! %d tasks stalled\n", taskcount);
			exit(1);
		}
		// 从就绪队列中删除这个协程
		deltask(&taskrunqueue, t);
		 // 将协程状态改为非就绪态
		t->ready = 0;
		// 保存正在执行的协程
		taskrunning = t;
		// 切换次数+1
		tasknswitch++;
		taskdebug("run %d (%s)", t->id, t->name);
		// 切换到t执行，并且保存当前上下文到taskschedcontext
		contextswitch(&taskschedcontext, &t->context);
//print("back in scheduler\n");
		// 执行结束
		taskrunning = nil;
		// 刚才执行的协程t退出了
		if(t->exiting){
			// 如果不是系统协程，协程个数减一
			if(!t->system)
				taskcount--;
			// 保存当前协程在alltask的索引
			i = t->alltaskslot;
			// 将最后一个协程切换到当前协程的位置，因为当前协程要退出了
			alltask[i] = alltask[--nalltask];
			// 更新被置换协程的索引
			alltask[i]->alltaskslot = i;
			// 释放堆内存
			free(t);
		}
	}
}

void**
taskdata(void)
{
	return &taskrunning->udata;
}

/*
 * debugging
 */
void
taskname(char *fmt, ...)
{
	va_list arg;
	Task *t;

	t = taskrunning;
	va_start(arg, fmt);
	vsnprint(t->name, sizeof t->name, fmt, arg);
	va_end(arg);
}

char*
taskgetname(void)
{
	return taskrunning->name;
}

// 标记协程的状态
void
taskstate(char *fmt, ...)
{
	// 可变参数列表，实际上是指向可变参数的指针
	va_list arg;
	Task *t;

	t = taskrunning;

	// 对arg进行初始化，让arg指向可变参数列表里面的第一个参数
	va_start(arg, fmt);
	// 将arg的内容写入t->state中
	vsnprint(t->state, sizeof t->name, fmt, arg);
	// 释放指针，将ap置为NULL
	va_end(arg);
}

char*
taskgetstate(void)
{
	return taskrunning->state;
}

void
needstack(int n)
{
	Task *t;

	t = taskrunning;

	if((char*)&t <= (char*)t->stk
	|| (char*)&t - (char*)t->stk < 256+n){
		fprint(2, "task stack overflow: &t=%p tstk=%p n=%d\n", &t, t->stk, 256+n);
		abort();
	}
}

static void
taskinfo(int s)
{
	int i;
	Task *t;
	char *extra;

	fprint(2, "task list:\n");
	for(i=0; i<nalltask; i++){
		t = alltask[i];
		if(t == taskrunning)
			extra = " (running)";
		else if(t->ready)
			extra = " (ready)";
		else
			extra = "";
		fprint(2, "%6d%c %-20s %s%s\n", 
			t->id, t->system ? 's' : ' ', 
			t->name, t->state, extra);
	}
}

/*
 * startup
 */

static int taskargc;
static char **taskargv;
int mainstacksize;

static void
taskmainstart(void *v)
{
	taskname("taskmain");
	taskmain(taskargc, taskargv);
}

int
main(int argc, char **argv)
{
	struct sigaction sa, osa;

	memset(&sa, 0, sizeof sa);
	sa.sa_handler = taskinfo;
	sa.sa_flags = SA_RESTART;
	sigaction(SIGQUIT, &sa, &osa);

#ifdef SIGINFO
	sigaction(SIGINFO, &sa, &osa);
#endif

	argv0 = argv[0];
	taskargc = argc;
	taskargv = argv;

	if(mainstacksize == 0)
		mainstacksize = 256*1024;
	taskcreate(taskmainstart, nil, mainstacksize);
	taskscheduler();
	fprint(2, "taskscheduler returned in main!\n");
	abort();
	return 0;
}

/*
 * 将协程插入队列中，如果之前在其它队列，则会被移除
 * */
void
addtask(Tasklist *l, Task *t)
{
	if(l->tail){
		l->tail->next = t;
		t->prev = l->tail;
	}else{
		l->head = t;
		t->prev = nil;
	}
	l->tail = t;
	t->next = nil;
}

void
deltask(Tasklist *l, Task *t)
{
	if(t->prev)
		t->prev->next = t->next;
	else
		l->head = t->next;
	if(t->next)
		t->next->prev = t->prev;
	else
		l->tail = t->prev;
}

unsigned int
taskid(void)
{
	return taskrunning->id;
}

