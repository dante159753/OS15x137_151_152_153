#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

/*
 * 17.14定点数格式化用到的设置，
 * 在这里我们使用的是“位运算”取数，
 * 其实我们正常使用乘法也可以；
 * 不过处于性能的考虑，
 * 使用位运算的速度仍然要快于乘法
 *
 * F用于17.14定点数的格式化，十进制数值为16384
 * INT_MAX为在17.14定点数格式下可以表示到的最大整数，十进制数值为2147483647
 * INT_MIN为在17.14定点数格式下可以表示到的最小整数，十进制数值为-2147483648
 *
 * #define F (1 << 14)
 * #define INT_MAX ((1 << 31) -1)
 * #define INT_MIN (-(1 << 31))
 */
#define F (1 << 14)

/* 定义nice值 */
#define NICE_DEFAULT 0
#define NICE_MAX 20
#define NICE_MIN -20

/* 定义recent_cpu的默认值 */
#define RECENT_CPU_DEFAULT 0

/* 定义load_avg的初始值 */
#define LOAD_AVG_DEFAULT 0

/*
 * 定义系统全局的load_avg，
 * 也许没有必要使用static，
 * 不过我觉得用不用static在这里都一样
 */
int load_avg;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

    /*
     * 把原有的代码注释掉
     * Enforce preemption.
     * if (++thread_ticks >= TIME_SLICE) {
     *     intr_yield_on_return ();
     * }
     */
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}


/* Creates a new kernel thread named NAME with the given initial
 *  PRIORITY, which executes FUNCTION passing AUX as the argument,
 *  and adds it to the ready queue.  Returns the thread identifier
 *  for the new thread, or TID_ERROR if creation fails.
 *
 *  If thread_start() has been called, then the new thread may be
 *  scheduled before thread_create() returns.  It could even exit
 *  before thread_create() returns.  Contrariwise, the original
 *  thread may run for any amount of time before the new thread is
 *  scheduled.  Use a semaphore or some other form of
 *  synchronization if you need to ensure ordering.
 *
 *  The code provided sets the new thread's `priority' member to
 *  PRIORITY, but no actual priority scheduling is implemented.
 *  Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(
    const char *name,
    int priority,
    thread_func *function,
    void *aux
) {
    struct thread *t;
    struct kernel_thread_frame *kf;
    struct switch_entry_frame *ef;
    struct switch_threads_frame *sf;
    tid_t tid;
    enum intr_level old_level;

    ASSERT(function != NULL);

    /* Allocate thread. */
    t = palloc_get_page(PAL_ZERO);
    if (t == NULL) {
        return TID_ERROR;
    }

    /* Initialize thread. */
    init_thread(t, name, priority);
    tid = t -> tid = allocate_tid();

    /* Prepare thread for first run by initializing its stack.
     *    Do this atomically so intermediate values for the 'stack'
     *    member cannot be observed. */
    old_level = intr_disable();

    /* Stack frame for kernel_thread(). */
    kf = alloc_frame(t, sizeof *kf);
    kf -> eip = NULL;
    kf -> function = function;
    kf -> aux = aux;

    /* Stack frame for switch_entry(). */
    ef = alloc_frame(t, sizeof *ef);
    ef -> eip = (void (*) (void)) kernel_thread;

    /* Stack frame for switch_threads(). */
    sf = alloc_frame(t, sizeof *sf);
    sf -> eip = switch_entry;
    sf -> ebp = 0;

    intr_set_level(old_level);

    /* Add to run queue. */
    thread_unblock(t);

    /*
     * 测试是否需要使当前调用thread_create()函数的线程让出CPU，
     * 显然这样做是有充分的理由的，
     * thread_create()函数在执行完上面的thread_unblock()函数的时候，已经开创了一个新的线程了，
     * 然而调用thread_create()的线程当前依然占有着CPU，
     * 这显然是不完全正确的操作，
     * 假如新建的线程的优先级比当前调用thread_create()函数的线程的优先级高的时候，
     * 当前调用thread_create()函数的线程就应该立即让出CPU，
     * 因此我们需要调用test_yield()进行检测
     *
     * 这一步的操作也是相当关键啊，
     * 之前一直搞不清楚为什么程序运行时显示的优先级总是与实际的不符合，
     * 很大一部分的原因就是没用考虑到上述问题
     */
    old_level = intr_disable();
    test_yield();
    intr_set_level(old_level);

    return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
 *  again until awoken by thread_unblock().
 *
 *  This function must be called with interrupts turned off.  It
 *  is usually a better idea to use one of the synchronization
 *  primitives in synch.h. */
void thread_block(void) {
    ASSERT(!intr_context ());
    ASSERT(intr_get_level () == INTR_OFF);

    thread_current() -> status = THREAD_BLOCKED;
    schedule();
}

/* Transitions a blocked thread T to the ready-to-run state.
 *  This is an error if T is not blocked.  (Use thread_yield() to
 *  make the running thread ready.)
 *
 *  This function does not preempt the running thread.  This can
 *  be important: if the caller had disabled interrupts itself,
 *  it may expect that it can atomically unblock a thread and
 *  update other data. */
void thread_unblock(struct thread *t) {
    enum intr_level old_level;

    ASSERT(is_thread(t));

    old_level = intr_disable();
    ASSERT(t -> status == THREAD_BLOCKED);
    /*
     * 注释掉原有的代码
     * list_push_back(&ready_list, &t -> elem);
     * 并按照优先级降序将线程插入到ready_list中
     */
    list_insert_ordered(
            &ready_list,
            &t -> elem,
            (list_less_func *) &cmp_priority,
            NULL
    );

    t -> status = THREAD_READY;
    intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it call schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}


/* Yields the CPU.  The current thread is not put to sleep and
 *  may be scheduled again immediately at the scheduler's whim. */
void thread_yield(void) {
    struct thread *cur = thread_current();
    enum intr_level old_level;

    ASSERT(!intr_context());

    old_level = intr_disable();
    if (cur != idle_thread) {
        /*
         * 注释掉原有的代码
         * list_push_back (&ready_list, &cur->elem);
         */
        list_insert_ordered(
                &ready_list,
                &cur -> elem,
                (list_less_func *) &cmp_priority,
                NULL
        );
    }
    cur -> status = THREAD_READY;
    schedule();
    intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}



/* 以下为为对源码的自定义部分或者修改部分 */
/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) {
    /*
     * 注释掉原有的代码
     * thread_current ()->priority = new_priority;
     */

    enum intr_level old_level;
    int old_priority;

    old_level = intr_disable();
    /* 获取当前线程的优先级 */
    old_priority = thread_current() -> priority;
    /* 把new_priority赋给init_priority，将会被用于refresh_priority()函数 */
    thread_current() -> init_priority = new_priority;
    /* 刷新当前线程的优先级 */
    refresh_priority();
    /*
     * 如果设置完优先级之后，
     * 当前线程的优先级比之前要高，
     * 则必须从新donate_priority()，
     * 因为涉及到“嵌套”问题
     */
    if (old_priority < thread_current() -> priority) {
        donate_priority();
    }
    /*
     * 如果设置完优先级之后，
     * 当前线程的优先级比之前的要低，
     * 则通过donate_yield()函数测试当前线程是否需要放弃对CPU的占用
     */
    if (old_priority > thread_current() -> priority) {
        test_yield();
    }
    intr_set_level(old_level);
}
/* Returns the current thread's priority. */
int thread_get_priority(void) {
    enum intr_level old_level = intr_disable();
    int tmp = thread_current() -> priority;
    intr_set_level(old_level);
    return tmp;
}

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice) {
    enum intr_level old_level = intr_disable();
    thread_current() -> nice = nice;
    /*
     * 然后需要计算线程的优先级，
     * 如果当前线程不再具有最高的优先级了，
     * 把它yield
     */
    calculate_mlfqs_priority(thread_current());
    test_yield();
    intr_set_level(old_level);
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
    enum intr_level old_level = intr_disable ();
    int temp = thread_current() -> nice;
    intr_set_level(old_level);

    return temp;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
    enum intr_level old_level = intr_disable();
    int temp = fp_to_int_round_nearest(
            fp_mul_int(load_avg, 100)
    );
    intr_set_level(old_level);

    return temp;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
    enum intr_level old_level = intr_disable();
    int temp = fp_to_int_round_nearest(
            fp_mul_int(thread_current() -> recent_cpu, 100)
    );
    intr_set_level(old_level);

    return temp;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}
/* Does basic initialization of T as a blocked thread named NAME. */
static void init_thread(struct thread *t, const char *name, int priority) {
    ASSERT(t != NULL);
    ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
    ASSERT(name != NULL);

    memset(t, 0, sizeof(*t));
    t -> status = THREAD_BLOCKED;
    strlcpy((t -> name), name, sizeof(t -> name));
    t -> stack = (uint8_t *) t + PGSIZE;
    t -> priority = priority;
    t -> magic = THREAD_MAGIC;

    /* 初始化2.2.3 Priority Scheduling中用到的变量 */
    t -> wait_on_lock = NULL;
    list_init(&t -> donation_list);
    t -> init_priority = priority;

    list_push_back(&all_list, &t -> allelem);
}

/* 刷新当前线程的优先级 */
void refresh_priority(void) {
    struct thread *c;
    struct thread *f;

    c = thread_current();
    c -> priority = c -> init_priority;

    if (list_empty(&c -> donation_list)) {
        return;
    }

    f = list_entry(
            list_front(&c -> donation_list),
            struct thread,
            donation_list_elem
    );
    /*
     * 显然，
     * 当前线程的优先级不可能比它自身的donation_list中的线程的优先级要低，
     * 也就是说，
     * 当前线程的优先级设置的最低限度是其自身donation_list中优先级最高的线程的优先级
     */
    if ((f -> priority) > (c -> priority)) {
        c -> priority = f -> priority;
    }
}

/* “优先级继承” */
#define DEPTH_LIMIT 8
void donate_priority(void) {
    int depth;
    struct thread *t;
    struct lock *l;

    depth = 0;
    t = thread_current();
    l = t -> wait_on_lock;
    /* 当lock存在且在允许“嵌套”的深度范围内 */
    while (l && depth < DEPTH_LIMIT) {
        depth++;
        /* 如果没有线程持有lock，则直接返回 */
        if (l -> holder == NULL) {
            return;
        }
        /* 如果持有lock的线程比当前线程的优先级要高，则直接返回 */
        if ((l -> holder -> priority) >= t -> priority) {
            return;
        }
        /*
         * 这里就是解决“优先级嵌套”的精髓所在，
         * 首先我们让持有lock的线程获取当前线程的优先级，
         * 注意进过之前的两个if()语句，
         * 我们已经确保此时持有lock的线程的优先级比当前线程的优先级要低了，
         * 因此在这里进行赋值并没有任何问题
         */
        l -> holder -> priority = t -> priority;
        /*
         * 接着，我们让t变量指向持有lock的线程，
         * 注意此前t变量指向的是当前线程
         */
        t = l -> holder;
        /*
         * 最后，
         * 我们取得持有lock（假设是lock A）的线程正在等待的lock（假设是lock B），
         * 注意，
         * 在这一步操作之后，一个while()循环到此结束，
         * 在满足条件的情况下，while()将会进入下一个循环，
         * 重复上面的操作，
         * 每次进行
         *     l -> holder -> priority = t -> priority;
         *     t = l -> holder;
         *     l = t -> wait_on_lock;
         * 这三步的时候，都是对“嵌套”的一次向外剥离，
         * 直到找到“嵌套”的最外层，也就是没有因为请求lock而阻塞的那个线程，
         * 解决“优先级翻转”的最关键，就是提升最外层的那个没有被阻塞的线程的优先级，
         * 也就是，提升H -> M -> L中L线程的优先级，
         * 当然，在循环的过程中，实际上M的优先级也一并被提升到了H的优先级，
         * 只不过因为M还有需要等待的lock，所以依然被阻塞，只有L可以运行
         */
        l = t -> wait_on_lock;
    }
}

/* 从donation_list中移除线程 */
void remove_with_lock(struct lock *lock) {
    struct list_elem *e;
    struct thread *t;

    for (
        e = list_begin(&thread_current() -> donation_list);
        e != list_end(&thread_current() -> donation_list);
        e = list_next(e)
    ) {
        t = list_entry(e, struct thread, donation_list_elem);
        if (t -> wait_on_lock == lock) {
            list_remove(e);
        }
    }
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
schedule_tail (struct thread *prev) 
{
  struct thread *cur = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until schedule_tail() has
   completed. */
static void
schedule (void) 
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  schedule_tail (prev); 
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);


/* 对优先级进行降序 */
bool cmp_priority (
    const struct list_elem *a,
    const struct list_elem *b,
    void *aux UNUSED
) {
    struct thread *ta = list_entry(a, struct thread, elem);
    struct thread *tb = list_entry(b, struct thread, elem);
    if ((ta -> priority) > (tb -> priority)) {
        return true;
    }
    return false;
}

/* 判断设置优先级过后，当前线程是否应该放弃对CPU的占用 */
void test_yield(void) {
    struct thread *t;

    if (list_empty(&ready_list)) {
        return;
    }

    t = list_entry(
            list_front(&ready_list),
            struct thread,
            elem
    );

    /*
     * 处于外部中断的情况下，
     * 如果当前线程的不再具有最高的优先级，
     * 或者它的时间片用完了，
     * 则应当在中断返回的时候立即释放资源。
     * 为什么要这样做呢？
     * 我们可以这样理解，
     * 假设有一台打印机正在打印东西，
     * 此时有一个优先级更高的线程请求占用打印机，
     * 那么我们正在打印的东西是不是就应该立即停止的呢？
     * 当然不是啦，
     * 我们应该让当前正在打印的东西打印完毕，
     * 然后才会去相应更高优先级的线程，
     * 所以是在中断返回的时候才释放资源啦
     *
     * 注意thread_tick()函数也许起着相同的作用，
     * 有必要的话可以把这几行代码注释掉
     */
    if(intr_context()) {
        thread_ticks++; // ？
        if(
                thread_current() -> priority < t -> priority ||
                (thread_ticks >= TIME_SLICE &&
                thread_current() -> priority == t -> priority)
        ) {
            intr_yield_on_return();
        }
        return;
    }

    if ((thread_current() -> priority) < t -> priority) {
        thread_yield();
    }
}

/* 用于计算load_avg */
void calculate_load_avg(void) {
    int length = 0;
    int temp_59 = 0;
    int temp_1 = 0;

    length = list_size(&ready_list);
    if (thread_current() != idle_thread) {
        length++;
    }

    /* 注意全部先转换成定点数计算出相应值，然后在换算成对应的整数 */
    temp_59 = fp_div(int_to_fp(59) ,int_to_fp(60));
    temp_1 = fp_div(int_to_fp(1), int_to_fp(60));
    load_avg = fp_mul(temp_59, load_avg) + fp_mul_int(temp_1, length);
}

/* 用于计算线程的recent_cpu */
void calculate_recent_cpu(struct thread *t) {
    int temp_mem = 0;
    int temp_den = 0;
    int temp_con = 0;
    int temp_pro = 0;

    if (t == idle_thread) {
        return;
    }

    /* 注意全部先转换成定点数计算出相应值，然后在换算成对应的整数 */
    temp_mem = fp_mul_int(load_avg, 2);
    temp_den = fp_add_int(temp_mem, 1);
    temp_con = fp_div(temp_mem, temp_den);
    temp_pro = fp_mul(temp_con, t -> recent_cpu);
    t -> recent_cpu = fp_add_int(temp_pro, t -> nice);
}

/* 用于计算多级反馈队列调度算法情况下，每个线程的优先级 */
void calculate_mlfqs_priority(struct thread *t) {
    int temp_con = 0;
    int temp_pro = 0;
    int temp_dif = 0;

    if (t == idle_thread) {
        return;
    }

    /* 注意全部先转换成定点数计算出相应值，然后在换算成对应的整数 */
    temp_con = fp_div(t -> recent_cpu, int_to_fp(4));
    temp_pro = fp_mul(int_to_fp(t -> nice), int_to_fp(2));
    temp_dif = fp_sub(int_to_fp(PRI_MAX), temp_con);
    temp_dif = fp_sub(temp_dif, temp_pro);
    /* priority必然需要转换为对应的整数 */
    t -> priority = fp_to_int_round_zero(temp_dif);

    /* 保证计算出的线程优先级在限制范围内 */
    if (t -> priority < PRI_MIN) {
        t -> priority = PRI_MIN;
    }
    if (t -> priority > PRI_MAX) {
        t -> priority = PRI_MAX;
    }
}

/* 用于线程recent_cpu的自增运算 */
void recent_cpu_increment(void) {
    if (thread_current() == idle_thread) {
        return;
    }

    thread_current() -> recent_cpu = fp_add(
            thread_current() -> recent_cpu,
            int_to_fp(1)
    );
}

/* 更新recent_cpu和load_avg */
void update_recent_cpu_and_load_avg(void) {
    struct list_elem *e;

    for (
            e = list_begin(&all_list);
            e != list_end(&all_list);
            e = list_next(e)
    ) {
        struct thread *t = list_entry(e, struct thread, allelem);
        calculate_recent_cpu(t);
        calculate_mlfqs_priority(t);
    }
}

/* 整数格式化为定点数 */
int int_to_fp(int n) {
    return n * F;
}

/* 定点数转化为整数（舍入到0） */
int fp_to_int_round_zero(int x) {
    return x / F;
}

/* 定点数转化为整数（四舍五入到最近的） */
int fp_to_int_round_nearest(int x) {
    if (x >= 0) {
        return (x + F / 2) / F;
    } else {
        return (x - F / 2) / F;
    }
}

/* 两个定点数相加 */
int fp_add(int x, int y) {
    return x + y;
}

/* 两个定点数相减 */
int fp_sub(int x, int y) {
    return x - y;
}

/* 整数与定点数相加 */
int fp_add_int(int x, int n) {
    return x + n * F;
}

/* 整数减定点数 */
int fp_sub_int(int x, int n) {
    return x - n * F;
}

/* 两个定点数相乘 */
int fp_mul(int x, int y) {
    return ((int64_t) x) * y / F;
}

/* 两个定点数相除 */
int fp_div(int x, int y) {
    return ((int64_t) x) * F / y;
}

/* 定点数与整数相乘 */
int fp_mul_int(int x, int n) {
    return x * n;
}

/* 定点数除以整数 */
int fp_div_int(int x, int n) {
    return x / n;
}
