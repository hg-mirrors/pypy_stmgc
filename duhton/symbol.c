#include <string.h>
#include "duhton.h"

typedef struct _Du_Symbol {
    DuOBJECT_HEAD
    char *name;
    struct _Du_Symbol *next;
} DuSymbolObject;

DuSymbolObject *_Du_AllSymbols = NULL;

void symbol_print(DuSymbolObject *ob)
{
    printf("'%s'", ob->name);
}

DuObject *symbol_eval(DuObject *ob, DuObject *locals)
{
    DuObject *res = DuFrame_GetSymbol(locals, ob);
    if (res == NULL) {
        res = DuFrame_GetSymbol(Du_Globals, ob);
        if (res == NULL)
            Du_FatalError("symbol not defined as a variable: '%s'",
                          DuSymbol_AsString(ob));
    }
    return res;
}

DuTypeObject DuSymbol_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "symbol",
    sizeof(DuSymbolObject),
    (destructor_fn)NULL,
    (print_fn)symbol_print,
    (eval_fn)symbol_eval,
};

DuObject *DuSymbol_FromString(char *name)
{
    DuSymbolObject *p;
    for (p=_Du_AllSymbols; p != NULL; p=p->next) {
        if (strcmp(name, p->name) == 0) {
            Du_INCREF(p);
            return (DuObject *)p;
        }
    }
    p = (DuSymbolObject *)DuObject_New(&DuSymbol_Type);
    _Du_BecomeImmortal((DuObject *)p);    /* make the symbol object immortal */
    p->name = strdup(name);
    p->next = _Du_AllSymbols;
    _Du_AllSymbols = p;
    Du_INCREF(p);
    return (DuObject *)p;
}

char *DuSymbol_AsString(DuObject *ob)
{
    DuSymbol_Ensure("DuSymbol_AsString", ob);
    return ((DuSymbolObject *)ob)->name;
}

void DuSymbol_Ensure(char *where, DuObject *ob)
{
    if (!DuSymbol_Check(ob))
        Du_FatalError("%s: expected 'symbol' argument, got '%s'",
                      where, ob->ob_type->dt_name);
}
