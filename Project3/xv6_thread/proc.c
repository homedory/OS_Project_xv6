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
  struct comthd comthd[NPROC];
} ptable;

struct spinlock szlock;

static struct proc *initproc;

struct thrret {
  thread_t tid;
  void* retval;
};

struct thrret tret[NPROC];

int nextpid = 1;
int nexttid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void updatesz(struct comthd *ct, uint sz);
void dealloccomthd(struct comthd *ct);
int alloccomthd(struct comthd **ctaddr);
int incrementthdcnt(void);
int decrementthdcnt(void);

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
  alloccomthd(&p->comthd);
  updatesz(p->comthd, PGSIZE);
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
  acquire(&szlock);
  sz = curproc->comthd->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0){
	  release(&szlock);
      return -1;
	}
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0){
	  release(&szlock);
      return -1;
	}
  }
  updatesz(curproc->comthd, sz);
  switchuvm(curproc);

  release(&szlock);
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
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->comthd->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  alloccomthd(&np->comthd);
  updatesz(np->comthd, curproc->comthd->sz);
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
  
  np->tid = nexttid++;
  np->jointhd = 0;

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
 
  // kill all threads
  cleanthreads();
  
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
    if(p->parent->pid == curproc->pid){
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
		dealloccomthd(p->comthd);
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

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
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
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

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
  int findproc = 0;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
	  findproc = 1;
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
    }
  }
  release(&ptable.lock);
  
  if(findproc)
	return 0;
  
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

// clean all threads except current process
void
cleanthreads(void)
{
  struct proc *p;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
	if(p->pid == curproc->pid && p != curproc){
	  // decrement thdcnt not using decrementthdcnt, beacause of ptable.lock 
	  if(p->comthd->thdcnt > 1)
		p->comthd->thdcnt--;
	  
	  p->comthd = 0;	
	  kfree(p->kstack);
	  p->kstack = 0;
	  p->pid = 0;
	  p->parent = 0;
	  p->name[0] = 0;
	  p->killed = 0;
	  p->tid = 0;
	  p->jointhd = 0;
	  p->state = UNUSED;
	}
  }
  release(&ptable.lock);
}

void
updatesz(struct comthd* ct, uint sz)
{
  acquire(&ptable.lock);
  ct->sz = sz; 
  release(&ptable.lock);
}

void
dealloccomthd(struct comthd *ct)
{
  ct->sz = 0;
  ct->thdcnt = 0;
}

int
alloccomthd(struct comthd **ctaddr)
{
  struct comthd *ct;
  acquire(&ptable.lock);
  for(ct = ptable.comthd; ct < &ptable.comthd[NPROC]; ct++){
	if(ct->sz == 0 && ct->thdcnt == 0){
	  ct->thdcnt = 1; 
	  *ctaddr = ct;
	  release(&ptable.lock);
	  return 1;
	}
  }
  release(&ptable.lock);
  return 0;
}

int
incrementthdcnt(void)
{
  struct proc *p = myproc();
  acquire(&ptable.lock);
  if(p->comthd->thdcnt == NTHD){
	release(&ptable.lock);
	return 0;
  } 
  p->comthd->thdcnt++;
  release(&ptable.lock);
  return 1;
}

int decrementthdcnt(void)
{
  struct proc *p = myproc();
  acquire(&ptable.lock);
  if(p->comthd->thdcnt == 1){
	release(&ptable.lock);
	return 0;
  } 
  p->comthd->thdcnt--;
  release(&ptable.lock);
  return 1;
}


int
thread_create(thread_t *thread, void *(*start_routine)(void*), void *arg)
{
  struct proc *p;
  struct proc *curproc = myproc();
  char* sp;
  int tid;
  
  // check thread count boundary and increment
  if(incrementthdcnt() == 0){
	cprintf("reaached maximum thread num\n");
	return -1;
  }
  
  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return -1;

found:
  p->state = EMBRYO;
  p->pid = myproc()->pid;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return -1;
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
  
  // share pgdir among threads
  p->pgdir = curproc->pgdir;
  
  // szlock 
  acquire(&szlock);

  p->comthd = curproc->comthd;
  uint sz = curproc->comthd->sz;
  if((sz = allocuvm(p->pgdir,sz, sz + 2*PGSIZE)) == 0){
	// fail to allocate new user stack
	kfree(p->kstack);
	p->kstack = 0;
	acquire(&ptable.lock);
	p->state = UNUSED;
    release(&ptable.lock);
	decrementthdcnt();
	return -1;
  }
  // guard page
  clearpteu(p->pgdir, (char*)(sz - 2*PGSIZE));
  
  uint usp = sz;
  // space for argument and fake return
  usp -= 4;
  *((uint*)usp) = (uint) arg;
  usp-=4;
  *((uint*)usp) = 0xffffffff;
  
  updatesz(p->comthd, sz);
  
  release(&szlock);

  p->parent = curproc->parent;
  
  // set trap frame
  *p->tf = *curproc->tf;
  p->tf->eip = (uint)start_routine; 
  p->tf->esp = usp;

  p->tid = nexttid++;
  tid = p->tid; 
  p->jointhd = 0;
 
  int i; 
  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      p->ofile[i] = curproc->ofile[i];
  
  p->cwd = idup(curproc->cwd);

  safestrcpy(p->name, curproc->name, sizeof(curproc->name));

  acquire(&ptable.lock);
  p->state = RUNNABLE;
  release(&ptable.lock);
  
  *thread = tid;
  return 0;
}

void
thread_exit(void *retval)
{
  struct thrret *tr;
  // insert thread retval into thrret
  for(tr = tret; tr < &tret[NPROC]; tr++) {
	if(tr->tid == 0) {
	  tr->retval = retval;
	  tr->tid = myproc()->tid;
	  break;
	}
  }
  
  // wakeup jointhd
  if(myproc()->jointhd != 0)
	wakeup1(myproc()->jointhd);
  
  decrementthdcnt();

  acquire(&ptable.lock);
  myproc()->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

int
thread_join(thread_t thread, void **retval)
{
  struct proc *p;
  int foundthd, thdexit;
  struct proc *curproc = myproc();
  struct thrret *tr;

  acquire(&ptable.lock);
  for(;;){
	// Scan through table looking for the thread
    foundthd = 0;
	thdexit = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
	  if(p->pid != curproc->pid || p->tid != thread)
		continue;
	  
	  foundthd = 1;
	  // set curr thread as a jointhd of target thhread
	  p->jointhd = curproc;

      if(p->state == ZOMBIE){
        // Found one.
        kfree(p->kstack);
		p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
		p->tid = 0;
		p->jointhd = 0;
		thdexit = 1;
        release(&ptable.lock);
		break;
      }
    }
		
	if(thdexit)
	  break;
    
	// No point waiting if we don't have any thread whose tid is thread.
    if(!foundthd || curproc->killed){
	  release(&ptable.lock);
	  return -1;
    }
    // sleep until target thread exits
    sleep(curproc, &ptable.lock);
  }
  
  // set retval
  for(tr = tret; tr < &tret[NPROC]; tr++){
	if(tr->tid == thread) {
	  *retval = (void*)tr->retval;
	  // clear the element 
	  tr->tid = 0;
	  break;
	}
  }

  return 0;
}
