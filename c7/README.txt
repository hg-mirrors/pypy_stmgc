============================================================
STMGC-C7
============================================================


An STM system, focusing on low numbers of CPUs.  It requires Linux
running 64-bit, and must be compiled with clang.


The %gs segment prefix
----------------------

This a hack using __attribute__((address_space(256))) on structs, which
makes clang write all pointer dereferences to them using the "%gs:"
prefix at the assembler level.  This is a rarely-used way to shift all
memory accesses by some offset stored in the %gs special register.  Each
thread has its own value in %gs.  Note that %fs is used in a similar way
by the pthread library to offset the thread-local variables; what we
need is similar to thread-local variables, but in large quantity.

I did not find any measurable slow-down from any example using the %gs
prefix, so I expect the real performance hit to be tiny (along the lines
of the extra stress on instruction caches caused by the extra byte for
each load/store instruction).


remap_file_pages
----------------

The Linux-only system call remap_file_pages() allows us to tweak a
mmap() region of memory.  It makes explicit one extra level of the
memory-mapped management of the CPU.  Let us focus on mmaps that are not
backed by a file.  A call to mmap() reserves a number of physical pages
4096 bytes each, initialized to zero (and actually lazily allocated when
the process really needs them, rather than all at once).  It also
reserves a range of addresses in the current process, of the same size,
which correspond to the physical pages.  But by using
remap_file_pages(), we can change the mapping of the addresses to the
physical pages.  The total amount of both quantities is identical, and
invariable, but we can take any page-sized range of addresses and ask
that it now maps to a different physical page.  Most probably, this
comes with no overhead once the change is done: neither in terms of
performance nor in extra memory in the kernel.  The trick here is that
different ranges of addresses can map to the same physical page of
memory, which gives a zero-cost way to share data at different
addresses.

NOTE: this functionality is only available on Linux.  There are
potential ideas for other OSes, like a Windows device driver that would
tweak the OS' page tables.  But it would need serious research to know
if it is feasible.


Memory organization
-------------------

We have a small, fixed number of big pieces of memory called "segments".
Each segment has enough (virtual) address space for all the objects that
the program needs.  This is actually allocated from a single big mmap()
so that pages can be exchanged between segments with remap_file_pages().
We call N the number of segments.  Actual threads are not limited in
number; they grab one segment in order to run GC-manipulating code, and
release it afterwards.  This is similar to what occurs with the GIL,
except we have up to N threads that can run in parallel, instead of 1.

The first step when the process starts is to use remap_file_pages() to
make these N regions all map to the same physical memory.  In each
thread, when it grabs a segment, %gs is made to point to the start of
the segment.  This means that %gs-relative accesses will go to different
real addresses in each thread, but these addresses are then (initially)
mapped to the same physical memory, so the effect is as if we used
neither %gs nor remap_file_pages().

The interesting exception to that rule comes from pages that contain
objects that are already committed, but are being modified by the
current transaction.  Such changes must not be visible to other threads
before the current transaction commits.  This is done by using another
remap_file_pages() to "unshare" the page, i.e. stop the corresponding
%gs-relative, thread-local page from mapping to the same physical page
as others.  We get a fresh new physical page, and duplicate its content
--- much like the OS does after a fork() for pages modified by one or
the other process.

In more details: the first page of addresses in each thread-local region
(4096 bytes) is made non-accessible, to detect errors of accessing the
NULL pointer.  The second page is reserved for thread-local data.  The
rest is divided into 1/16 for thread-local read markers, followed by
15/16 for the real objects.  We initially use remap_file_pages() on this
15/16 range.  The read markers are described below.

Each transaction records the objects that it changed.  These are
necessarily within unshared pages.  When we want to commit a
transaction, we ask for a safe-point (suspending the other threads in a
known state), and then we copy again the modified objects into the other
version(s) of that data.  The point is that, from another thread's point
of view, the memory didn't appear to change unexpectedly, but only when
waiting in a safe-point.

Moreover, we detect read-write conflicts when trying to commit.  To do
this, each transaction needs to track in their own (private) read
markers which objects it has read.  When we try to commit one segment's
version, when we would write a modified object to the other segments, we
can check the other segments' read markers.  If a conflict is detected,
we either abort the committing thread, or mark the other thread as
requiring an abort (which it will do when trying to leave the
safe-point).

On the other hand, write-write conflicts are detected eagerly, which is
necessary to avoid that all segments contain a modified version of the
object and no segment is left with the original version.  It is done
with a compare-and-swap into an array of write locks (only the first
time a given old object is modified by a given transaction).


Object creation and GC
----------------------

We use a GC that is similar to the one in PyPy:

- a generational GC, with one nursery per segment containing
  transaction-local objects only, and moved outside when full or when the
  transaction commits.

- nomenclature: objects are either "young" or "old" depending on whether
  they were created by the current transaction or not.  Old objects are
  always outside the nursery.  We call "overflow" objects the young
  objects that are also outside the nursery.

- pages need to be unshared when they contain old objects that are then
  modified.

- we need a write barrier to detect the changes done to any non-nursery
  object (the first time only).  This is just a flag check.  Then the
  slow-path of this write barrier distinguishes between overflow
  objects and old objects, and the latter need to be unshared.

- the old generation is collected with mark-and-sweep, during a major
  collection step that walks *all* objects.  This requires all threads
  to be synchronized, but ideally the threads should then proceed
  to do a parallel GC (i.e. mark in all threads in parallel, and
  then sweep in al threads in parallel, with one arbitrary thread
  taking on the additional coordination role needed).

- the major collections should be triggered by the amount of really-used
  memory, which means: counting the unshared pages as N pages.  Major
  collection should then re-share the pages as much as possible.  This is
  the essential part that guarantees that old, no-longer-modified
  bunches of objects are eventually present in only one copy in memory,
  in shared pages --- while at the same time bounding the number of
  calls to remap_file_pages() for each page at N-1 per major collection
  cycle.
