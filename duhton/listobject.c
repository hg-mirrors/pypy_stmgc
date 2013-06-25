#include <string.h>
#include "duhton.h"


/* 'tuple' objects are only used internally as the current items
   of 'list' objects
*/
typedef struct {
    DuOBJECT_HEAD
    int ob_count;
    DuObject *ob_items[1];
} DuTupleObject;

typedef struct {
    DuOBJECT_HEAD
    DuTupleObject *ob_tuple;
} DuListObject;


void tuple_trace(DuTupleObject *ob, void visit(gcptr *))
{
    int i;
    for (i=ob->ob_count-1; i>=0; i--) {
        visit(&ob->ob_items[i]);
    }
}

void list_trace(DuListObject *ob, void visit(gcptr *))
{
    visit((gcptr *)&ob->ob_tuple);
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
    ob = (DuTupleObject *)stm_allocate(size, DUTYPE_TUPLE);
    ob->ob_count = length;
    return ob;
}

void _list_append(DuListObject *ob, DuObject *x)
{
    _du_write1(ob);
    DuTupleObject *olditems = ob->ob_tuple;

    _du_read1(olditems);
    int i, newcount = olditems->ob_count + 1;
    DuTupleObject *newitems = DuTuple_New(newcount);

    for (i=0; i<newcount-1; i++)
        newitems->ob_items[i] = olditems->ob_items[i];
    newitems->ob_items[newcount-1] = x;

    ob->ob_tuple = newitems;
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

size_t _DuTuple_ByteSize(DuObject *tuple)
{
    DuTupleObject *t = (DuTupleObject *)tuple;
    return sizeof(DuTupleObject) + (t->ob_count - 1) * sizeof(DuObject *);
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

static DuTupleObject du_empty_tuple = {
    DuOBJECT_HEAD_INIT(DUTYPE_TUPLE),
    0,
};

DuObject *DuList_New()
{
    DuListObject *ob = (DuListObject *)DuObject_New(&DuList_Type);
    ob->ob_tuple = &du_empty_tuple;
    return (DuObject *)ob;
}

void DuList_Ensure(char *where, DuObject *ob)
{
    if (!DuList_Check(ob))
        Du_FatalError("%s: expected 'list' argument, got '%s'",
                      where, Du_TYPE(ob)->dt_name);
}
