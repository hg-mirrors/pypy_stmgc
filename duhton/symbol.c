#include <string.h>
#include <stdlib.h>
#include "duhton.h"

typedef struct _Du_Symbol {
    DuOBJECT_HEAD
    int myid;
    char *name;
    struct _Du_Symbol *next;
} DuSymbolObject;

static DuSymbolObject _Du_AllSymbols = {
    DuOBJECT_HEAD_INIT(DUTYPE_SYMBOL),
    "",
    NULL};


void symbol_trace(DuSymbolObject *ob, void visit(gcptr *))
{
    visit((gcptr *)&ob->next);
}

void symbol_print(DuSymbolObject *ob)
{
    _du_read1(ob);
    printf("'%s'", ob->name);
}

DuObject *symbol_eval(DuObject *ob, DuObject *locals)
{
    _du_save1(ob);
    DuObject *res = DuFrame_GetSymbol(locals, ob);
    _du_restore1(ob);
    if (res == NULL) {
        _du_save1(ob);
        res = DuFrame_GetSymbol(Du_Globals, ob);
        _du_restore1(ob);
        if (res == NULL)
            Du_FatalError("symbol not defined as a variable: '%s'",
                          DuSymbol_AsString(ob));
    }
    return res;
}

DuType DuSymbol_Type = {
    "symbol",
    DUTYPE_SYMBOL,
    sizeof(DuSymbolObject),
    (trace_fn)symbol_trace,
    (print_fn)symbol_print,
    (eval_fn)symbol_eval,
};


static int next_id = 1;

DuObject *DuSymbol_FromString(const char *name)
{
    DuSymbolObject *p, *head = &_Du_AllSymbols;
    for (p=head; p != NULL; p=p->next) {
        _du_read1(p);
        if (strcmp(name, p->name) == 0) {
            return (DuObject *)p;
        }
    }
    p = (DuSymbolObject *)DuObject_New(&DuSymbol_Type);
    p->name = strdup(name);
    p->myid = __sync_fetch_and_add(&next_id, 1);

    _du_write1(head);
    p->next = head->next;
    head->next = p;

    return (DuObject *)p;
}

char *DuSymbol_AsString(DuObject *ob)
{
    DuSymbol_Ensure("DuSymbol_AsString", ob);
    _du_read1(ob);
    return ((DuSymbolObject *)ob)->name;
}

int DuSymbol_Id(DuObject *ob)
{
    DuSymbol_Ensure("DuSymbol_Id", ob);
    return ((DuSymbolObject *)ob)->id;
}

void DuSymbol_Ensure(char *where, DuObject *ob)
{
    if (!DuSymbol_Check(ob))
        Du_FatalError("%s: expected 'symbol' argument, got '%s'",
                      where, Du_TYPE(ob)->dt_name);
}
