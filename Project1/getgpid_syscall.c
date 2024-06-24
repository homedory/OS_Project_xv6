#include <stddef.h>
#include "types.h"
#include "defs.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"

//System call
int 
getgpid(void)
{
	struct proc* parentproc = myproc()->parent;
	if (parentproc == NULL)
		return -1;

	struct proc* gparentproc = parentproc->parent;
	if (gparentproc == NULL)
		return -1;

	return gparentproc->pid;
}

//Wrapper for getgpid
int
sys_getgpid(void)
{
	return getgpid();
}
