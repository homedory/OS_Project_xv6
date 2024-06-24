#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

int
sys_fork(void)
{
  return fork();
}

int
sys_exit(void)
{
  exit();
  return 0;  // not reached
}

int
sys_wait(void)
{
  return wait();
}

int
sys_kill(void)
{
  int pid;

  if(argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int
sys_getpid(void)
{
  return myproc()->pid;
}

int
sys_sbrk(void)
{
  int addr;
  int n;

  if(argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

int
sys_sleep(void)
{
  int n;
  uint ticks0;

  if(argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(myproc()->killed){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

int
sys_yield(void)
{
  yield();
  return 0;
}

int
sys_getlev(void)
{
  return getlev();
}

int
sys_setpriority(void)
{
  int pid;
  int prio;
  
  argint(0, &pid);
  argint(1, &prio);

  return setpriority(pid, prio);
}

int
sys_setmonopoly(void)
{
  int pid;
  int pwd;
  
  argint(0, &pid);
  argint(1, &pwd);

  return setmonopoly(pid, pwd);
}

int
sys_monopolize(void)
{
  // If moq proc calls monopolize, does nothing
  if(myproc()->mon)
	return 0;

  monopolize();
  yield();
  return 0;
}

int
sys_unmonopolize(void)
{
  // If mlfq proc calls unmonopolize, does nothing
  if(!myproc()->mon)
	return 0;

  unmonopolize();
  yield();
  return 0;
}

