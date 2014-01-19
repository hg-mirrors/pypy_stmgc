/* Taken from: http://locklessinc.com/articles/locks/
   
   Sticking to semi-portable C code, we can still do a little better.
   There exists a form of the ticket lock that is designed for read-write
   locks. An example written in assembly was posted to the Linux kernel
   mailing list in 2002 by David Howells from RedHat. This was a highly
   optimized version of a read-write ticket lock developed at IBM in the
   early 90's by Joseph Seigh. Note that a similar (but not identical)
   algorithm was published by John Mellor-Crummey and Michael Scott in
   their landmark paper "Scalable Reader-Writer Synchronization for
   Shared-Memory Multiprocessors". Converting the algorithm from
   assembly language to C yields:
*/
#include <assert.h>
#include "reader_writer_lock.h"


#define EBUSY 1
#define atomic_xadd(P, V) __sync_fetch_and_add((P), (V))
#define cmpxchg(P, O, N) __sync_val_compare_and_swap((P), (O), (N))
#define atomic_inc(P) __sync_add_and_fetch((P), 1)
#define atomic_dec(P) __sync_add_and_fetch((P), -1) 
#define atomic_add(P, V) __sync_add_and_fetch((P), (V))
#define atomic_set_bit(P, V) __sync_or_and_fetch((P), 1<<(V))
#define atomic_clear_bit(P, V) __sync_and_and_fetch((P), ~(1<<(V)))
/* Compile read-write barrier */
#define barrier() asm volatile("": : :"memory")

/* Pause instruction to prevent excess processor bus usage */ 
#define cpu_relax() asm volatile("pause\n": : :"memory")



void rwticket_wrlock(rwticket *l)
{
	unsigned me = atomic_xadd(&l->u, (1<<16));
	unsigned char val = me >> 16;
	
	while (val != l->s.write) cpu_relax();
}

int rwticket_wrunlock(rwticket *l)
{
	rwticket t = *l;
	
	barrier();

	t.s.write++;
	t.s.read++;
	
	*(unsigned short *) l = t.us;
    return 0;
}

int rwticket_wrtrylock(rwticket *l)
{
    unsigned cmp = l->u;
    
	unsigned me = cmp & 0xff;//l->s.users;
	unsigned char menew = me + 1;
    //	unsigned read = (cmp & 0xffff) >> 8;//l->s.read << 8;
	//unsigned cmp = (me << 16) + read + me;
	unsigned cmpnew = (menew << 16) | (cmp & 0x0000ffff); //(menew << 16) + read + me;

	if (cmpxchg(&l->u, cmp, cmpnew) == cmp) return 0;
	
	return EBUSY;
}

void rwticket_rdlock(rwticket *l)
{
	unsigned me = atomic_xadd(&l->u, (1<<16));
	unsigned char val = me >> 16;
	
	while (val != l->s.read) cpu_relax();
	l->s.read++;
}

void rwticket_rdunlock(rwticket *l)
{
	atomic_inc(&l->s.write);
}

int rwticket_rdtrylock(rwticket *l)
{
    assert(0);
    /* XXX implement like wrtrylock */
	unsigned me = l->s.users;
	unsigned write = l->s.write;
	unsigned char menew = me + 1;
	unsigned cmp = (me << 16) + (me << 8) + write;
	unsigned cmpnew = ((unsigned) menew << 16) + (menew << 8) + write;

	if (cmpxchg(&l->u, cmp, cmpnew) == cmp) return 0;
	
	return EBUSY;
}
