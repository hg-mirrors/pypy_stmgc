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


Memory organization
-------------------

We allocate a big mmap that contains enough addresses for N times M
bytes, where N is the number of threads and M is an upper bound on the
total size of the objects.  Then we use remap_file_pages() to make these
N regions all map to the same physical memory.  In each thread,
%gs is made to point to the start of the corresponding region.  This
means that %gs-relative accesses will go to different addresses in
each thread, but these addresses are then (initially) mapped to the
same physical memory, so the effect is as if we used neither %gs nor
remap_file_pages().

The exception comes from pages that contain objects that are already
committed, but are being modified by the current transaction.  Such
changes must not be visible to other threads before the current
transaction commits.  This is done by using another remap_file_pages()
to "unshare" the page, i.e. stop the corresponding %gs-relative,
thread-local page from mapping to the same physical page as others.  We
get a fresh new physical page, and duplicate its content --- much like
the OS does after a fork() for pages modified by one or the other
process.

In more details: the first page of addresses in each thread-local region
(4096 bytes) is made non-accessible, to detect errors of accessing the
NULL pointer.  The second page is reserved for thread-local data.  The
rest is divided into 1/16 for thread-local read markers, followed by
15/16 for the real objects.  We initially use remap_file_pages() on this
15/16 range.

Each transaction records the objects that it changed.  These are
necessarily within unshared pages.  When other threads are about to
commit their own transaction, they first copy these objects into their
version of the page.  The point is that, from another thread's point of
view, the memory didn't appear to change unexpectedly, but only when
that other thread decides to copy the change explicitly.

Each transaction uses their own (private) read markers to track which
objects have been read.  When a thread "imports" changes done to some
objects, it can quickly check if these objects have also been read by
the current transaction, and if so, we know we have a conflict.


STM details
-----------

Here is how the STM works in terms that are hopefully common in STM
research.  The transactions run from a "start time" to a "commit time",
but these are not explicitly represented numerically.  The start time
defines the initial state of the objects as seen in this thread.  We use
the "extendable timestamps" approach in order to regularly bump the
start time of running transactions (not only when a potential conflict
is detected, but more eagerly).

Each thread records privately its read objects (using a byte-map) and
publicly its written objects (using an array of pointers as well as a
global flag in the object).  Read-write conflicts are detected during
the start time bumps.  Write-write conflicts are detected eagerly ---
only one transaction can be concurrently running with a given object
modified.  (In the case of write-write conficts, there are several
possible contention management policies; for now we always abort the
transaction that comes later in its attempt to modify the object.)

Special care is taken for objects allocated in the current transaction.
We expect these objects to be the vast majority of modified objects, and
also most of them to die quickly.  More about it below.

We use what looks like an "undo log" approach, where objects are
modified in-place and aborts cause them to be copied back from somewhere
else.  However, it is implemented without any explicit undo log, but by
copying objects between multiple thread-local copies.  Memory pages
containing modified objects are duplicated anyway, and so we already
have around several copies of the objects at potentially different
versions.

At most one thread is called the "leader" (this is new terminology as
far as I know).  The leader is the thread running a transaction whose
start time is higher than the start time of any other running
transaction.  If there are several threads with the same highest start
time, we have no leader.  Leadership is a temporary condition: it is
acquired (typically) by the thread whose transaction commits and whose
next transaction starts; but it is lost again as soon as any other
thread updates its transaction's start time to match.

The point of the notion of leadership is that when the leader wants to
modify an object, it must first make sure that the original version is
also present somewhere else.  Only the leader thread, if there is any,
needs to worry about it.  We don't need to remember the original version
of an older object, because if we need to abort a transaction, we may as
well update all objects to the latest version.  And if there are several
threads with the same highest start time, we can be sure that the
original version of the object is somewhere among them --- this is the
point of detecting write-write conflicts eagerly.  The only remaining
case is the one in which there is a leader thread, this leader thread
has the only latest version of an object, and it tries to further modify
this object.  To handle this precise case, for now, we simply wait until
another thread updates and we are no longer the leader.  (*)

(*) the code in core.c contains, or contained, or will again contain, an
explicit undo log that would be filled in this case only.


Object creation and GC
----------------------

XXX write me
