#include <string.h>
#include "duhton.h"


/* 'tuple' objects are only used internally as the current items
   of 'list' objects
*/
typedef TLPREFIX struct DuTupleObject_s {
    DuOBJECT_HEAD1
    int ob_count;
    int ob_capacity;
    DuObject *ob_items[1];
} DuTupleObject;

typedef TLPREFIX struct DuListObject_s {
    DuOBJECT_HEAD1
    DuTupleObject *ob_tuple;
} DuListObject;


void tuple_trace(struct DuTupleObject_s *ob, void visit(object_t **))
{
    int i;
    for (i=ob->ob_count-1; i>=0; i--) {
        visit((object_t **)&ob->ob_items[i]);
    }
}

size_t tuple_bytesize(struct DuTupleObject_s *ob)
{
    return sizeof(DuTupleObject) + (ob->ob_capacity - 1) * sizeof(DuObject *);
}

void list_trace(struct DuListObject_s *ob, void visit(object_t **))
{
    visit((object_t **)&ob->ob_tuple);
}


void list_print(DuListObject *ob)
{
    int i;
    _du_read1(ob);

    DuTupleObject *p = ob->ob_tuple;
    _du_read1(p);

    if (p->ob_count == 0) {
        printf("[]");
    }
    else {
        printf("[ ");
        for (i=0; i<p->ob_count; i++) {
            Du_Print(p->ob_items[i], 0);
            printf(" ");
        }
        printf("]");
    }
}

int list_length(DuListObject *ob)
{
    _du_read1(ob);
    DuTupleObject *p = ob->ob_tuple;
    _du_read1(p);
    return p->ob_count;
}

DuTupleObject *DuTuple_New(int length)
{
    DuTupleObject *ob;
    size_t size = sizeof(DuTupleObject) + (length-1)*sizeof(DuObject *);
    ob = (DuTupleObject *)stm_allocate(size);
    ob->ob_base.type_id = DUTYPE_TUPLE;
    ob->ob_count = length;
    ob->ob_capacity = length;
    return ob;
}

int overallocated_size(int size)
{
    return size + (size >> 3) + (size < 9 ? 3 : 6);
}

void _list_append(DuListObject *ob, DuObject *x)
{
    _du_read1(ob);
    DuTupleObject *olditems = ob->ob_tuple;

    _du_read1(olditems);
    int i, newcount = olditems->ob_count + 1;

    if (newcount <= olditems->ob_capacity) {
        _du_write1(olditems);
        olditems->ob_items[newcount-1] = x;
        olditems->ob_count = newcount;
    } else {                    /* allocate new one */
        _du_save3(ob, x, olditems);
        DuTupleObject *newitems = DuTuple_New(overallocated_size(newcount));
        newitems->ob_count = newcount;
        _du_restore3(ob, x, olditems);

        _du_write1(ob);

        for (i=0; i<newcount-1; i++)
            newitems->ob_items[i] = olditems->ob_items[i];
        newitems->ob_items[newcount-1] = x;

        ob->ob_tuple = newitems;
    }
}

void DuList_Append(DuObject *ob, DuObject *item)
{
    DuList_Ensure("DuList_Append", ob);
    _list_append((DuListObject *)ob, item);
}

int DuList_Size(DuObject *ob)
{
    DuList_Ensure("DuList_Size", ob);
    return list_length((DuListObject *)ob);
}

DuObject *_list_getitem(DuListObject *ob, int index)
{
    _du_read1(ob);
    DuTupleObject *p = ob->ob_tuple;

    _du_read1(p);
    if (index < 0 || index >= p->ob_count)
        Du_FatalError("list_get: index out of range");
    return p->ob_items[index];
}

DuObject *DuList_GetItem(DuObject *ob, int index)
{
    DuList_Ensure("DuList_GetItem", ob);
    return _list_getitem((DuListObject *)ob, index);
}

void _list_setitem(DuListObject *ob, int index, DuObject *newitem)
{
    _du_read1(ob);
    DuTupleObject *p = ob->ob_tuple;

    _du_write1(p);
    if (index < 0 || index >= p->ob_count)
        Du_FatalError("list_set: index out of range");
    p->ob_items[index] = newitem;
}

void DuList_SetItem(DuObject *list, int index, DuObject *newobj)
{
    DuList_Ensure("DuList_SetItem", list);
    _list_setitem((DuListObject *)list, index, newobj);
}

DuObject *_list_pop(DuListObject *ob, int index)
{
    _du_read1(ob);
    DuTupleObject *p = ob->ob_tuple;

    _du_write1(p);
    if (index < 0 || index >= p->ob_count)
        Du_FatalError("list_pop: index out of range");
    DuObject *result = p->ob_items[index];
    int i;
    p->ob_count--;
    for (i=index; i<p->ob_count; i++)
        p->ob_items[i] = p->ob_items[i+1];
    return result;
}

DuObject *DuList_Pop(DuObject *list, int index)
{
    DuList_Ensure("DuList_Pop", list);
    return _list_pop((DuListObject *)list, index);
}

DuType DuTuple_Type = {    /* "tuple" is mostly an internal type here */
    "tuple",
    DUTYPE_TUPLE,
    0,    /* dt_size */
    (trace_fn)tuple_trace,
    (print_fn)NULL,
    (eval_fn)NULL,
    (len_fn)NULL,
    (len_fn)NULL,
    (bytesize_fn)tuple_bytesize,
};

DuType DuList_Type = {
    "list",
    DUTYPE_LIST,
    sizeof(DuListObject),
    (trace_fn)list_trace,
    (print_fn)list_print,
    (eval_fn)NULL,
    (len_fn)NULL,
    (len_fn)list_length,
};

static DuTupleObject *du_empty_tuple;

void init_prebuilt_list_objects(void)
{
    du_empty_tuple = (DuTupleObject *)
        _stm_allocate_old(sizeof(DuTupleObject));
    du_empty_tuple->ob_base.type_id = DUTYPE_TUPLE;
    du_empty_tuple->ob_count = 0;
    du_empty_tuple->ob_capacity = 0;
    _du_save1(du_empty_tuple);
}

DuObject *DuList_New()
{
    DuListObject *ob = (DuListObject *)DuObject_New(&DuList_Type);
    ob->ob_tuple = du_empty_tuple;
    return (DuObject *)ob;
}

void DuList_Ensure(char *where, DuObject *ob)
{
    if (!DuList_Check(ob))
        Du_FatalError("%s: expected 'list' argument, got '%s'",
                      where, Du_TYPE(ob)->dt_name);
}
