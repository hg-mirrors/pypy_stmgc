#include "duhton.h"
#include <stdint.h>

struct dictentry {
    revision_t symbol_id;
    DuObject *symbol;
    DuObject *value;
    eval_fn builtin_macro;
    DuObject *func_arglist;
    DuObject *func_progn;
};

typedef struct {
    DuOBJECT_HEAD
    int ob_count;
    struct dictentry ob_items[1];
} DuFrameNodeObject;

void framenode_trace(DuFrameNodeObject *ob, void visit(gcptr *))
{
    int i;
    for (i=ob->ob_count-1; i>=0; i--) {
        struct dictentry *e = &ob->ob_items[i];
        visit(&e->symbol);
        visit(&e->value);
        visit(&e->func_arglist);
        visit(&e->func_progn);
    }
}

size_t framenode_bytesize(DuFrameNodeObject *ob)
{
    return (sizeof(DuFrameNodeObject) +
            (ob->ob_count - 1) * sizeof(struct dictentry));
}


typedef struct {
    DuOBJECT_HEAD
    DuFrameNodeObject *ob_nodes;
} DuFrameObject;

static DuFrameNodeObject du_empty_framenode = {
    DuOBJECT_HEAD_INIT(DUTYPE_FRAMENODE),
    0,
};

DuFrameObject Du_GlobalsFrame = {
    DuOBJECT_HEAD_INIT(DUTYPE_FRAME),
    &du_empty_framenode,
};

DuObject *_Du_GetGlobals()
{
    return (DuObject *)&Du_GlobalsFrame;
}

DuObject *DuFrame_New()
{
    DuFrameObject *ob = (DuFrameObject *)DuObject_New(&DuFrame_Type);
    ob->ob_nodes = &du_empty_framenode;
    return (DuObject *)ob;
}

#if 0
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
#endif

void frame_trace(DuFrameObject *ob, void visit(gcptr *))
{
    visit((gcptr *)&ob->ob_nodes);
}

void frame_print(DuFrameObject *ob)
{
    printf("<frame>");
}

static struct dictentry *
find_entry(DuFrameObject *frame, DuObject *symbol, int write_mode)
{
    _du_read1(frame);
    DuFrameNodeObject *ob = frame->ob_nodes;

    _du_read1(ob);
    int left = 0;
    int right = ob->ob_count;
    struct dictentry *entries = ob->ob_items;
    revision_t search_id = stm_id(symbol);

#if 0
#ifdef _GC_DEBUG
    int j;
    for (j = 0; j < right; j++) {
        dprintf(("\t%p\n", (gcptr)entries[j].symbol_id));
    }
#endif
#endif

    while (right > left) {
        int middle = (left + right) / 2;
        revision_t found_id = entries[middle].symbol_id;
        if (search_id < found_id)
            right = middle;
        else if (search_id == found_id) {
            if (write_mode) {
                _du_write1(ob);
                entries = ob->ob_items;
            }
            return entries + middle;
        }
        else
            left = middle + 1;
    }

    if (!write_mode) {
        return NULL;
    }
    else {
        int i;
        size_t size = (sizeof(DuFrameNodeObject) +
                       (ob->ob_count + 1 - 1)*sizeof(struct dictentry));
        DuFrameNodeObject *newob;

        _du_save3(ob, symbol, frame);
        newob = (DuFrameNodeObject *)stm_allocate(size, DUTYPE_FRAMENODE);
        _du_restore3(ob, symbol, frame);

        newob->ob_count = ob->ob_count + 1;
        struct dictentry *newentries = newob->ob_items;
        entries = ob->ob_items;

        for (i=0; i<left; i++)
            newentries[i] = entries[i];

        DuSymbol_Ensure("find_entry", symbol);
        dprintf(("NEW ENTRY ADDED WITH search_id = %p\n", (gcptr)search_id));
        newentries[left].symbol_id = search_id;
        newentries[left].symbol = symbol;
        newentries[left].value = NULL;
        newentries[left].builtin_macro = NULL;
        newentries[left].func_arglist = NULL;
        newentries[left].func_progn = NULL;

        for (i=left+1; i<newob->ob_count; i++)
            newentries[i] = entries[i-1];

        _du_write1(frame);
        frame->ob_nodes = newob;

        return newentries + left;
    }
}

void DuFrame_SetBuiltinMacro(DuObject *frame, char *name, eval_fn func)
{
    DuFrame_Ensure("DuFrame_SetBuiltinMacro", frame);

    _du_save1(frame);
    DuObject *sym = DuSymbol_FromString(name);
    _du_restore1(frame);

    struct dictentry *e = find_entry((DuFrameObject *)frame, sym, 1);
    e->builtin_macro = func;
}

static void
_parse_arguments(DuObject *symbol, DuObject *arguments,
                 DuObject *formallist, DuObject *caller, DuObject *callee)
{
    while (DuCons_Check(formallist)) {
        if (!DuCons_Check(arguments))
            Du_FatalError("call to '%s': not enough arguments",
                          DuSymbol_AsString(symbol));

        _du_read1(arguments);
        DuObject *arg = _DuCons_CAR(arguments);
        DuObject *argumentsnext = _DuCons_NEXT(arguments);

        _du_save3(symbol, argumentsnext, caller);
        _du_save2(formallist, callee);
        DuObject *obj = Du_Eval(arg, caller);
        _du_restore2(formallist, callee);

        _du_read1(formallist);
        DuObject *sym = _DuCons_CAR(formallist);
        DuObject *formallistnext = _DuCons_NEXT(formallist);

        _du_save2(formallistnext, callee);
        DuFrame_SetSymbol(callee, sym, obj);
        _du_restore2(formallistnext, callee);
        _du_restore3(symbol, argumentsnext, caller);

        formallist = formallistnext;
        arguments = argumentsnext;
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
        DuObject *func = e->func_progn;
        _du_save1(func);
        _du_save3(frame, symbol, rest);
        DuObject *callee_frame = DuFrame_New();
        _du_restore3(frame, symbol, rest);

        _du_save1(callee_frame);
        _parse_arguments(symbol, rest, e->func_arglist, frame, callee_frame);
        _du_restore1(callee_frame);
        _du_restore1(func);

        if (execute_now) {
            return Du_Progn(func, callee_frame);
        }
        else {
            Du_TransactionAdd(func, callee_frame);
            return NULL;
        }
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
    return e ? e->value : NULL;
}

void DuFrame_SetSymbol(DuObject *frame, DuObject *symbol, DuObject *value)
{
    struct dictentry *e;
    DuFrame_Ensure("DuFrame_SetSymbol", frame);

    _du_save1(value);
    e = find_entry((DuFrameObject *)frame, symbol, 1);
    _du_restore1(value);

    e->value = value;
}

void DuFrame_SetSymbolStr(DuObject *frame, char *name, DuObject *value)
{
    _du_save2(frame, value);
    DuObject *sym = DuSymbol_FromString(name);
    _du_restore2(frame, value);

    DuFrame_SetSymbol(frame, sym, value);
}

void DuFrame_SetUserFunction(DuObject *frame, DuObject *symbol,
                             DuObject *arglist, DuObject *progn)
{
    struct dictentry *e;
    DuFrame_Ensure("DuFrame_SetUserFunction", frame);

    _du_save2(arglist, progn);
    e = find_entry((DuFrameObject *)frame, symbol, 1);
    _du_restore2(arglist, progn);

    e->func_arglist = arglist;
    e->func_progn = progn;
}

void DuFrame_Ensure(char *where, DuObject *ob)
{
    if (!DuFrame_Check(ob))
        Du_FatalError("%s: expected 'frame' argument, got '%s'",
                      where, Du_TYPE(ob)->dt_name);
}

DuType DuFrameNode_Type = {    /* internal type */
    "framenode",
    DUTYPE_FRAMENODE,
    0,    /* dt_size */
    (trace_fn)framenode_trace,
    (print_fn)NULL,
    (eval_fn)NULL,
    (len_fn)NULL,
    (len_fn)NULL,
    (bytesize_fn)framenode_bytesize,
};

DuType DuFrame_Type = {
    "frame",
    DUTYPE_FRAME,
    sizeof(DuFrameObject),
    (trace_fn)frame_trace,
    (print_fn)frame_print,
};
