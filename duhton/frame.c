#include "duhton.h"
#include <stdint.h>

struct dictentry {
    DuObject *symbol;
    DuObject *value;
    eval_fn builtin_macro;
    DuObject *func_arglist;
    DuObject *func_progn;
};

typedef struct {
    DuOBJECT_HEAD
    int entry_count;
    struct dictentry *entries;
} DuFrameObject;

DuObject *DuFrame_New()
{
    DuFrameObject *ob = (DuFrameObject *)DuObject_New(&DuFrame_Type);
    ob->entry_count = 0;
    ob->entries = NULL;
    return (DuObject *)ob;
}

DuObject *DuFrame_Copy(DuObject *frame)
{
    DuFrame_Ensure("DuFrame_Copy", frame);
    int i;
    DuFrameObject *src = (DuFrameObject *)frame;
    DuFrameObject *dst = (DuFrameObject *)DuFrame_New();
    dst->entry_count = src->entry_count;
    dst->entries = malloc(sizeof(struct dictentry) * src->entry_count);
    assert(dst->entries);
    for (i=0; i<src->entry_count; i++) {
        struct dictentry *e = &src->entries[i];
        Du_INCREF(e->symbol);
        if (e->value        != NULL) Du_INCREF(e->value       );
        if (e->func_arglist != NULL) Du_INCREF(e->func_arglist);
        if (e->func_progn   != NULL) Du_INCREF(e->func_progn  );
        dst->entries[i] = *e;
    }
    return (DuObject *)dst;
}

void frame_free(DuFrameObject *ob)
{
    int i;
    for (i=0; i<ob->entry_count; i++) {
        struct dictentry *e = &ob->entries[i];
        Du_DECREF(e->symbol);
        if (e->value        != NULL) Du_DECREF(e->value       );
        if (e->func_arglist != NULL) Du_DECREF(e->func_arglist);
        if (e->func_progn   != NULL) Du_DECREF(e->func_progn  );
    }
    free(ob->entries);
    free(ob);
}

void frame_print(DuFrameObject *ob)
{
    printf("<frame>");
}

static struct dictentry *
find_entry(DuFrameObject *frame, DuObject *symbol, int add_if_missing)
{
    int left = 0;
    int right = frame->entry_count;
    struct dictentry *entries = frame->entries;
    while (right > left) {
        int middle = (left + right) / 2;
        DuObject *found = entries[middle].symbol;
        if ((intptr_t)found < (intptr_t)symbol)
            right = middle;
        else if (found == symbol)
            return entries + middle;
        else
            left = middle + 1;
    }
    if (add_if_missing) {
        int i;
        int newcount = frame->entry_count + 1;
        struct dictentry *newentries = malloc(sizeof(struct dictentry) *
                                              newcount);
        for (i=0; i<left; i++)
            newentries[i] = entries[i];
        DuSymbol_Ensure("find_entry", symbol);
        newentries[left].symbol = symbol; Du_INCREF(symbol);
        newentries[left].value = NULL;
        newentries[left].builtin_macro = NULL;
        newentries[left].func_arglist = NULL;
        newentries[left].func_progn = NULL;
        for (i=left+1; i<newcount; i++)
            newentries[i] = entries[i-1];
        frame->entries = newentries;
        frame->entry_count = newcount;
        free(entries);
        return newentries + left;
    }
    else
        return NULL;
}

void DuFrame_SetBuiltinMacro(DuObject *frame, char *name, eval_fn func)
{
    DuFrame_Ensure("DuFrame_SetBuiltinMacro", frame);
    DuObject *sym = DuSymbol_FromString(name);
    struct dictentry *e = find_entry((DuFrameObject *)frame, sym, 1);
    e->builtin_macro = func;
    Du_DECREF(sym);
}

static void
_parse_arguments(DuObject *symbol, DuObject *arguments,
                 DuObject *formallist, DuObject *caller, DuObject *callee)
{
    while (DuCons_Check(formallist)) {
        if (!DuCons_Check(arguments))
            Du_FatalError("call to '%s': not enough arguments",
                          DuSymbol_AsString(symbol));
        DuObject *sym = _DuCons_CAR(formallist);
        DuObject *obj = Du_Eval(_DuCons_CAR(arguments), caller);
        DuFrame_SetSymbol(callee, sym, obj);
        Du_DECREF(obj);
        formallist = _DuCons_NEXT(formallist);
        arguments = _DuCons_NEXT(arguments);
    }
    if (arguments != Du_None)
        Du_FatalError("call to '%s': too many arguments",
                      DuSymbol_AsString(symbol));
}

DuObject *_DuFrame_EvalCall(DuObject *frame, DuObject *symbol,
                            DuObject *rest, int execute_now)
{
    struct dictentry *e;
    DuFrame_Ensure("_DuFrame_EvalCall", frame);

    e = find_entry((DuFrameObject *)frame, symbol, 0);
    if (!e) {
        e = find_entry((DuFrameObject *)Du_Globals, symbol, 0);
        if (!e) {
            if (!DuSymbol_Check(symbol)) {
                printf("_DuFrame_EvalCall: ");
                Du_Print(symbol, 1);
                Du_FatalError("expected a symbol to execute");
            }
            else
                goto not_defined;
        }
    }
    if (e->func_progn) {
        DuObject *callee_frame = DuFrame_New();
        DuObject *res;
        _parse_arguments(symbol, rest, e->func_arglist, frame, callee_frame);
        if (execute_now) {
            res = Du_Progn(e->func_progn, callee_frame);
        }
        else {
            Du_TransactionAdd(e->func_progn, callee_frame);
            res = NULL;
        }
        Du_DECREF(callee_frame);
        return res;
    }
    if (e->builtin_macro) {
        if (!execute_now)
            Du_FatalError("symbol refers to a macro: '%s'",
                          DuSymbol_AsString(symbol));
        return e->builtin_macro(rest, frame);
    }
 not_defined:
    Du_FatalError("symbol not defined as a function: '%s'",
                  DuSymbol_AsString(symbol));
}

DuObject *DuFrame_GetSymbol(DuObject *frame, DuObject *symbol)
{
    struct dictentry *e;
    DuFrame_Ensure("DuFrame_GetSymbol", frame);

    e = find_entry((DuFrameObject *)frame, symbol, 0);
    if (e && e->value) {
        Du_INCREF(e->value);
        return e->value;
    }
    else
        return NULL;
}

void DuFrame_SetSymbol(DuObject *frame, DuObject *symbol, DuObject *value)
{
    struct dictentry *e;
    DuFrame_Ensure("DuFrame_SetSymbol", frame);

    e = find_entry((DuFrameObject *)frame, symbol, 1);
    if (e->value) Du_DECREF(e->value);
    e->value = value; Du_INCREF(value);
}

void DuFrame_SetSymbolStr(DuObject *frame, char *name, DuObject *value)
{
    DuObject *sym = DuSymbol_FromString(name);
    DuFrame_SetSymbol(frame, sym, value);
    Du_DECREF(sym);
}

void DuFrame_SetUserFunction(DuObject *frame, DuObject *symbol,
                             DuObject *arglist, DuObject *progn)
{
    struct dictentry *e;
    DuFrame_Ensure("DuFrame_SetUserFunction", frame);

    e = find_entry((DuFrameObject *)frame, symbol, 1);
    if (e->func_arglist) Du_DECREF(e->func_arglist);
    if (e->func_progn)   Du_DECREF(e->func_progn);
    e->func_arglist = arglist; Du_INCREF(arglist);
    e->func_progn   = progn;   Du_INCREF(progn);
}

void DuFrame_Ensure(char *where, DuObject *ob)
{
    if (!DuFrame_Check(ob))
        Du_FatalError("%s: expected 'frame' argument, got '%s'",
                      where, ob->ob_type->dt_name);
}

DuTypeObject DuFrame_Type = {
    DuOBJECT_HEAD_INIT(&DuType_Type),
    "frame",
    sizeof(DuFrameObject),
    (destructor_fn)frame_free,
    (print_fn)frame_print,
};
