#include <string.h>
#include "duhton.h"

typedef struct {
    DuOBJECT_HEAD
    int ob_count;
    DuObject **ob_items;
} DuListObject;

void list_free(DuListObject *ob)
{
    int i;
    for (i=0; i<ob->ob_count; i++) {
        Du_DECREF(ob->ob_items[i]);
#ifdef Du_DEBUG
        ob->ob_items[i] = (DuObject *)0xDD;
#endif
    }
    free(ob->ob_items);
    free(ob);
}

void list_print(DuListObject *ob)
{
    int i;
    Du_AME_INEVITABLE(ob);
    if (ob->ob_count == 0) {
        printf("[]");
    }
    else {
        printf("[ ");
        for (i=0; i<ob->ob_count; i++) {
            Du_Print(ob->ob_items[i], 0);
            printf(" ");
        }
        printf("]");
    }
}

int list_length(DuListObject *ob)
{
    int length;
    Du_AME_READ(ob, (length = ob->ob_count));
    return length;
}

void list_ame_copy(DuListObject *ob)
{
    DuObject **globitems = ob->ob_items;
    int count = ob->ob_count;
    ob->ob_items = malloc(sizeof(DuObject*) * count);
    assert(ob->ob_items);
    memcpy((char*)ob->ob_items, globitems, sizeof(DuObject*) * count);
    /* XXX either this ob_items or the original one is never freed */
}

void _list_append(DuListObject *ob, DuObject *x)
{
    Du_AME_WRITE(ob);
    int i, newcount = ob->ob_count + 1;
    DuObject **olditems = ob->ob_items;
    DuObject **newitems = malloc(sizeof(DuObject*) * newcount);
    assert(newitems);
    for (i=0; i<newcount-1; i++)
        newitems[i] = olditems[i];
    Du_INCREF(x);
    newitems[newcount-1] = x;
    ob->ob_items = newitems;
    ob->ob_count = newcount;
    free(olditems);
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
    DuObject *result;
    int length;
    DuObject **items;
    Du_AME_READ(ob, (length = ob->ob_count, items = ob->ob_items));
    if (index < 0 || index >= length)
        Du_FatalError("list_get: index out of range");
    result = items[index];
    Du_INCREF(result);
    return result;
}

DuObject *DuList_GetItem(DuObject *ob, int index)
{
    DuList_Ensure("DuList_GetItem", ob);
    return _list_getitem((DuListObject *)ob, index);
}

void _list_setitem(DuListObject *ob, int index, DuObject *newitem)
{
    Du_AME_WRITE(ob);
    if (index < 0 || index >= ob->ob_count)
        Du_FatalError("list_set: index out of range");
    DuObject *prev = ob->ob_items[index];
    Du_INCREF(newitem);
    ob->ob_items[index] = newitem;
    Du_DECREF(prev);
}

void DuList_SetItem(DuObject *list, int index, DuObject *newobj)
{
    DuList_Ensure("DuList_SetItem", list);
    _list_setitem((DuListObject *)list, index, newobj);
}

DuObject *_list_pop(DuListObject *ob, int index)
{
    int i;
    Du_AME_WRITE(ob);
    if (index < 0 || index >= ob->ob_count)
        Du_FatalError("list_pop: index out of range");
    DuObject *result = ob->ob_items[index];
    ob->ob_count--;
    for (i=index; i<ob->ob_count; i++)
        ob->ob_items[i] = ob->ob_items[i+1];
    return result;
}

DuObject *DuList_Pop(DuObject *list, int index)
{
    DuList_Ensure("DuList_Pop", list);
    return _list_pop((DuListObject *)list, index);
}

DuTypeObject DuList_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "list",
    sizeof(DuListObject),
    (destructor_fn)list_free,
    (print_fn)list_print,
    (eval_fn)NULL,
    (len_fn)NULL,
    (len_fn)list_length,
    (ame_copy_fn)list_ame_copy,
};

DuObject *DuList_New()
{
    DuListObject *ob = (DuListObject *)DuObject_New(&DuList_Type);
    ob->ob_count = 0;
    ob->ob_items = NULL;
    return (DuObject *)ob;
}

void DuList_Ensure(char *where, DuObject *ob)
{
    if (!DuList_Check(ob))
        Du_FatalError("%s: expected 'list' argument, got '%s'",
                      where, ob->ob_type->dt_name);
}
