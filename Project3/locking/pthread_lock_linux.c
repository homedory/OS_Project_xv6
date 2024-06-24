#include <stdio.h>
#include <pthread.h>
#include <unistd.h>

int shared_resource = 0;

#define NUM_ITERS 100000
#define NUM_THREADS 1000

void lock();
void unlock();

struct {
  int request[NUM_THREADS];
  int turn;
  int finished;
  pthread_t scheduler;
} lockinfo;


void* sched_thread() 
{
	// set thread cancel state and type
	pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);
	
	while(1) {
		// find a thread and give turn
		for(int i = 0; i < NUM_THREADS; i++) {
			if(lockinfo.request[i]) {
  				lockinfo.finished = 0;
				lockinfo.turn = i;
				
				// wait until chosen thread exits critical section
				while(!lockinfo.finished) {
					usleep(1);
				}
			}
  		}
	}

	pthread_exit(NULL);
}

void initlock()
{
	lockinfo.turn = -1;
	lockinfo.finished = 1;
	
	pthread_create(&lockinfo.scheduler, NULL, sched_thread, NULL);
}

void terminatelock()
{
	pthread_cancel(lockinfo.scheduler);
}

void lock(int tid)
{
	lockinfo.request[tid] = 1; 
	while(lockinfo.turn != tid){
	  usleep(1);
	}
}

void unlock(int tid)
{
	lockinfo.request[tid] = 0;
	lockinfo.finished = 1;
}

void* thread_func(void* arg) {
    int tid = *(int*)arg;
    
    lock(tid);

        for(int i = 0; i < NUM_ITERS; i++)		  shared_resource++;
    
	unlock(tid);
    
    pthread_exit(NULL);
}

int main() {
    pthread_t threads[NUM_THREADS];
    int tids[NUM_THREADS];
	
	// initiate lock
	initlock();
    
    for (int i = 0; i < NUM_THREADS; i++) {
        tids[i] = i;
        pthread_create(&threads[i], NULL, thread_func, &tids[i]);
    }
    
    for (int i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    printf("shared: %d\n", shared_resource);
	
	// terminate lock
	terminatelock();

    return 0;
}
