

# Page Resharing #

The idea of page resharing is that, after a while of running a multi-threaded
program, all `S` segments have read most pages of the heap. Thus, nearly all of
the heap memory is duplicated `S` times. While very wasteful, such duplication
also negatively affects the performance of the validation operation. During
validation, all committed changes need to be imported for all accessible pages
in a segment. Thus, if all pages are accessible, all changes need to be copied
in. Instead, it is probably better to sometimes mark pages inaccessible again if
they are rarely accessed in that segment, and we therefore don't need to import
changes for these pages; lessening the work done by the validation operation.


## Situation without Resharing ##

Pages can only be ACCESSIBLE (readable *and* writable) or INACCESSIBLE (neither)
in a segment. Seg0's pages are always ACCESSIBLE.

New (old) objects get allocated during minor GCs in pages of the current
segment. On commit, these objects get copied to seg0 and to all segments in
which that page is also ACCESSIBLE. 

Whenever we access an obj (reading *or* writing) and the obj's page is not
ACCESSIBLE yet, we get a signal and transition the page from INACCESSIBLE to
ACCESSIBLE. Other segments are unaffected.


## Situation with Resharing ##

Pages can be NOACC, RO, ACC. 

`RO` provides the revision of seg0. Whenever a TX makes a page `ACC` while there
are `RO` pages around, make all `RO` pages `NOACC` in all segments. This eager
approach means that we do not need all privatization-locks (write) in many
places where we would otherwise need a `RO->NOACC` transition. Also, we expect
the `RO` pages to go out-of-date when we commit anyway (soon).

*INVARIANT*: whenever a segment has a `RO` page, that page has the same content
as seg0's version of that page.

*INVARIANT*: if there are `RO` pages around, no segment has the page `ACC`

*PROPERTY I*: Once there page is `ACC`, it stays `ACC` until major GC; no `RO` can
exist until major GC reshares (`ACC -> RO`) and makes seg0 up-to-date.

*PROPERTY II*: Validation will never try to import into `RO` pages, since (I)
guarantees that it wouldn't be a `RO` anymore if there was a change to import.


In signal handler:

    if is_read or is_write:
      if is `RO`:
        `RO -> ACC` (and `RO -> NOACC` for all others)
      else if is `NOACC`:
        if !is_write and noone has `ACC`:
          `NOACC -> RO`
        else:
          `NOACC -> ACC` (and `RO -> NOACC` for all others)

On validate: always imports into `ACC`, into `RO` would be a bug.

During major GC:

 1. Validation of seg0: gets all changes; any `RO` views still around means that
    there was *no change* in those pages, so the views stay valid.
    
    XXX: what about the modifications that major GC makes during tracing? how
    does it affect the page-sharing in the kernel?
    
 2. All other segments validate their `ACC` pages; again `RO` pages *cannot*
    have changes that need importing.
 3. While tracing modified objs and overflow objs, remember pages with
    modifications. These *must not* change from `ACC` to `RO`.
 4. Loop over some pages (resharing can be distributed over several major GCs),
    and do `ACC -> RO` for all segments iff the previous step shows that to be
    valid. After that, the INVARIANTs need to hold.
 



