#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[])
{
	int p;
	printf(1, "Parent Process %d starts\n", getpid());

	for(int i = 0; i < 3; i++){
	  if((p = fork()) == 0){
		printf(1, "Child Process %d starts\n", getpid());

		if(i == 1){
		  printf(1, "Process %d - Divide with 0\n", getpid());
		  int x = 0;
		  int a = 5 / x;
		  printf(1, "a value: %d", a);
		}

		printf(1, "Process %d exits\n", getpid());
		exit();
	  }
	  else {
		wait();
	  }
	}
	printf(1, "Parent Process %d exits \n", getpid());
	exit();
}
