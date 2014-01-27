#include <string.h>
#include <stdlib.h>
#include "duhton.h"

typedef TLPREFIX struct DuSymbolObject_s DuSymbolObject;

struct DuSymbolObject_s {
    DuOBJECT_HEAD1
    int myid;
    char *name;
    DuSymbolObject *next;
};

static DuSymbolObject *_Du_AllSymbols;


void symbol_trace(struct DuSymbolObject_s *ob, void visit(object_t **))
{
    visit((object_t **)&ob->next);
}

void symbol_print(DuSymbolObject *ob)
{
    /* _du_read1(ob); IMMUTABLE name */
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

void init_prebuilt_symbol_objects(void)
{
    _Du_AllSymbols = (DuSymbolObject *)
        stm_allocate_prebuilt(sizeof(DuSymbolObject));
    _Du_AllSymbols->ob_base.type_id = DUTYPE_SYMBOL;
    _Du_AllSymbols->myid = 0;
    _Du_AllSymbols->name = "";
    _Du_AllSymbols->next = NULL;
}

DuObject *DuSymbol_FromString(const char *name)
{
    DuSymbolObject *p, *head = _Du_AllSymbols;
    _du_read1(head);
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
    /* _du_read1(ob); IMMUTABLE name */
    return ((DuSymbolObject *)ob)->name;
}

int DuSymbol_Id(DuObject *ob)
{
    DuSymbol_Ensure("DuSymbol_Id", ob);
    return ((DuSymbolObject *)ob)->myid;
}

void DuSymbol_Ensure(char *where, DuObject *ob)
{
    if (!DuSymbol_Check(ob))
        Du_FatalError("%s: expected 'symbol' argument, got '%s'",
                      where, Du_TYPE(ob)->dt_name);
}
