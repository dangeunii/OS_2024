#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
  int numProc[6];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}
// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

// Priority Boosting 지원
// global tick이 100이 되는 경우 모든 프로세스를 L0으로 재배치 및 모든 상태 초기화
// starvatioin을 예방하기 위함
void 
priority_boosting(void){
  acquire(&ptable.lock);
  ptable.numProc[0] = 0;
  for(struct proc *p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p -> state == RUNNABLE && p->level == 0 && p->monopolize == 0){
      ptable.numProc[0]++;
    }
    else if(p -> state == RUNNABLE && (p->level == 1 || p->level == 2 || p -> level == 3) && p->monopolize == 0){
      ptable.numProc[0]++;
      ptable.numProc[p->level]--;
    }

    p -> level = 0;
    p -> tick = 0;
    //p -> priority = 0;
  }
  release(&ptable.lock);
}

// 특정 pid를 갖는 프로세스의 priority를 설정
int setpriority(int pid, int priority){
  struct proc *p;
  //cprintf("priority : %d", priority);

  if(priority < 0 || priority >10){
    return -2;
  }

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    
    if(pid == p->pid){
      p->priority = priority;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
 
}

// 프로세스가 속한 큐의 레벨을 반환
int getlev(void){
  if(myproc() && myproc()->monopolize == 0){
    return myproc()->level;
  }
  else if(myproc() && myproc()->monopolize == 1){
    return 99;
  }
  else{
    return -1;
  }
}

int setmonopoly(int pid, int password) {
  struct proc *p;
  struct proc *curproc = myproc();

  cprintf("in proc.c pid : %d\n", pid);
  cprintf("in proc.c password : %d\n", password);

  acquire(&ptable.lock);
  cprintf("myproc : %d\n", curproc->pid);

  // 자기자신을 MoQ에 할당하려고 하는 경우 -4 반환
  if (curproc && pid == curproc->pid) {
    
    release(&ptable.lock); 
    return -4;
  }

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    if (p->pid == pid) {
      if (password == 2021057301) {
        cprintf("correct!\n");
        cprintf("p->monopolize : %d\n", p->monopolize);
        if (p->monopolize == 0) {
          p->monopolize = 1;
          cprintf("p->level : %d\n", p->level);
          ptable.numProc[p->level]--;
          ptable.numProc[4]++;
          cprintf("ptable.numProc[4] : %d\n", ptable.numProc[4]);
          int numProc = ptable.numProc[4];
          cprintf("numProc : %d\n", numProc);
          release(&ptable.lock); 
          return numProc;
        } else {
          // 이미 MoQ에 존재하는 경우
          release(&ptable.lock); 
          return -3;
        }
      } else {
        // 암호에 일치하지 않는 경우
        cprintf("password : %d", password);
        release(&ptable.lock);
        return -2;
      }
    }
  }
  // pid에 해당하는 프로세스가 존재하지 않는 경우
  release(&ptable.lock);
  return -1;
}

void unmonopolize() {
    struct proc *p = myproc();
    if (p->monopolize == 1) {
        acquire(&ptable.lock);
        p->state = RUNNABLE;

        if (p->level == 0) {
            ptable.numProc[0]++;
            ptable.numProc[4]--;
        } else if (p->level == 1) {
            ptable.numProc[1]++;
            ptable.numProc[4]--;
        } else if (p->level == 2) {
            ptable.numProc[2]++;
            ptable.numProc[4]--;
        } else if (p->level == 3) {
            ptable.numProc[3]++;
            ptable.numProc[4]--;
        }
        release(&ptable.lock);
    }
}

void monopolize() {
    struct proc *p;
    struct cpu *c = mycpu();
    c->proc = 0;

    if (myproc()->monopolize == 1) {
        p = myproc();

        acquire(&ptable.lock);
        c->proc = p;
        switchuvm(p);
        p->state = RUNNABLE;
        release(&ptable.lock);

        swtch(&(c->scheduler), p->context);
        switchkvm();

        c->proc = 0;
        unmonopolize();
    }
}

void
yield(void)
{
  acquire(&ptable.lock); 
  myproc()->state = RUNNABLE;
  ptable.numProc[myproc()->level]++;
  sched();
  release(&ptable.lock);
}


//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      if(p->monopolize == 1){
        // monopolize 함수를 실행해서 스케줄링하도록 실행
        monopolize();
      }
      // L0의 경우
      else if(ptable.numProc[0] != 0 && p->level == 0 && p->monopolize == 0){
        // 기본 RR
        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;

        // L0에 존재하는 Runnable 프로세스 수 1 감소하기
        ptable.numProc[0]--; 
        swtch(&(c->scheduler), p->context); 
        switchkvm();
        
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      // L1인 경우
      // L0에서 Runnable 한 프로세스의 갯수가 0이고, 현재 L1에 진행할 프로세스가 존재하는 경우
      else if(ptable.numProc[0] == 0 && ptable.numProc[1] > 0 && p->level == 1 && p->monopolize == 0){
        // 기본 RR
        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        
        // L1에 존재하는 Runnable 프로세스 수 1 감소하기
        ptable.numProc[1]--; 
        swtch(&(c->scheduler), p->context); 
        switchkvm();
        
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      // L2인 경우
      // L0에서 Runnable 한 프로세스의 갯수가 0이고, L1에서 Runnable 한 프로세스의 갯수가 0이고, 현재 L2에 진행할 프로세스가 존재하는 경우
      else if(ptable.numProc[0] == 0 && ptable.numProc[1] == 0 && ptable.numProc[2] > 0 && p->level == 2 && p->monopolize == 0){
        // 기본 RR
        // Switch to chosen process.  It is the process's job
        // to release ptable.lock and then reacquire it
        // before jumping back to us.
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        
        // L2에 존재하는 Runnable 프로세스 수 1 감소하기
        ptable.numProc[2]--; 
        swtch(&(c->scheduler), p->context); 
        switchkvm();
        
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      // L3인 경우
      // L0에서 Runnable 한 프로세스의 갯수가 0이고, L1에서 Runnable 한 프로세스의 갯수가 0이고, L1에서 Runnable 한 프로세스의 갯수가 0이고, 현재 L3에 진행할 프로세스가 존재하는 경우
      else{
        // 기본 RR이 아닌 우선순위에 의해서 동작하도록 수정
        // priority 숫자가 크면 우선순위가 높으므로 높은 것 먼저 실행
        // 만약 priority 숫자가 같다면 pid 작은 것을 먼저 실행
        struct proc *tmp;
        for(tmp = ptable.proc; tmp < &ptable.proc[NPROC]; tmp++){
          if(tmp -> state == RUNNABLE && tmp ->level == 3 && tmp->priority > p->priority){
            p = tmp;
          }
          else if(tmp -> state == RUNNABLE && tmp ->level == 3 && tmp->priority == p->priority){
            if(tmp -> pid < p -> pid){
              p = tmp;
            }
          }
        }
      
        c->proc = p;
        switchuvm(p);
        p->state = RUNNING;
        
        // L3에 존재하는 Runnable 프로세스 수 1 감소하기
        ptable.numProc[3]--; 
        swtch(&(c->scheduler), p->context); 
        switchkvm();
        
        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
// void
// yield(void)
// {
//   acquire(&ptable.lock);  //DOC: yieldlock
//   myproc()->state = RUNNABLE;
//   sched();
//   release(&ptable.lock);
// }

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
