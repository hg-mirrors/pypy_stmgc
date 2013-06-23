#ifndef _DUHTON_H_
#define _DUHTON_H_


#include <stdio.h>
#include <stdlib.h>
#include <assert.h>


#define Du_AME     /* must always be on for now */

#if defined(Du_DEBUG) || defined(Du_AME)
#  define Du_TRACK_REFS
#endif

#ifdef Du_AME
typedef long owner_version_t;
#endif


typedef struct _DuObject {
    int ob_refcnt;
    struct _DuTypeObject *ob_type;
#ifdef Du_TRACK_REFS
    struct _DuObject *ob_debug_prev, *ob_debug_next;
#endif
#ifdef Du_AME
    owner_version_t ob_version;
#endif
} DuObject;

#ifdef Du_TRACK_REFS
#  define _DuObject_HEAD_EXTRA  NULL, NULL
#else
#  define _DuObject_HEAD_EXTRA  /* nothing */
#endif

#define DuOBJECT_HEAD   DuObject ob_base;

#define DuOBJECT_HEAD_INIT(type)   { -1, type, _DuObject_HEAD_EXTRA }


#ifdef __GNUC__
#  define NORETURN  __attribute__((noreturn))
#else
#  define NORETURN  /* nothing */
#endif


typedef void(*destructor_fn)(DuObject *);
typedef void(*print_fn)(DuObject *);
typedef DuObject *(*eval_fn)(DuObject *, DuObject *);
typedef int(*len_fn)(DuObject *);
typedef void(*ame_copy_fn)(DuObject *);

typedef struct _DuTypeObject {
    DuOBJECT_HEAD
    const char *dt_name;
    int dt_size;
    destructor_fn dt_destructor;
    print_fn dt_print;
    eval_fn dt_eval;
    len_fn dt_is_true;
    len_fn dt_length;
    ame_copy_fn dt_ame_copy;
} DuTypeObject;

DuObject *DuObject_New(DuTypeObject *tp);
void _Du_Dealloc(DuObject *ob);
int DuObject_IsTrue(DuObject *ob);
int DuObject_Length(DuObject *ob);

#ifdef Du_TRACK_REFS
void _Du_NewReference(DuObject *ob);
void _Du_ForgetReference(DuObject *ob);
#else
#define _Du_NewReference(ob)       /* nothing */
#define _Du_ForgetReference(ob)    /* nothing */
#endif
void _Du_BecomeImmortal(DuObject *ob);


#define Du_AME_GLOBAL(ob)                       \
    (((DuObject*)(ob))->ob_refcnt < 0)

#define Du_INCREF(ob)                           \
    do {                                        \
        if (!Du_AME_GLOBAL(ob))                 \
            ++((DuObject*)(ob))->ob_refcnt;     \
    } while (0)

#define Du_DECREF(ob)                                   \
    do {                                                \
        if (((DuObject*)(ob))->ob_refcnt > 1)           \
            --((DuObject*)(ob))->ob_refcnt;             \
        else if (!Du_AME_GLOBAL(ob))                    \
            _Du_Dealloc((DuObject*)(ob));               \
    } while (0)


extern DuObject _Du_NoneStruct;
#define Du_None (&_Du_NoneStruct)

extern DuTypeObject DuType_Type;
extern DuTypeObject DuInt_Type;
extern DuTypeObject DuList_Type;
extern DuTypeObject DuContainer_Type;
extern DuTypeObject DuCons_Type;
extern DuTypeObject DuSymbol_Type;
extern DuTypeObject DuFrame_Type;

#define DuType_Check(ob)      (((DuObject*)(ob))->ob_type == &DuType_Type)
#define DuInt_Check(ob)       (((DuObject*)(ob))->ob_type == &DuInt_Type)
#define DuList_Check(ob)      (((DuObject*)(ob))->ob_type == &DuList_Type)
#define DuContainer_Check(ob) (((DuObject*)(ob))->ob_type == &DuContainer_Type)
#define DuCons_Check(ob)      (((DuObject*)(ob))->ob_type == &DuCons_Type)
#define DuSymbol_Check(ob)    (((DuObject*)(ob))->ob_type == &DuSymbol_Type)
#define DuFrame_Check(ob)     (((DuObject*)(ob))->ob_type == &DuFrame_Type)

void DuType_Ensure(char *where, DuObject *ob);
void DuInt_Ensure(char *where, DuObject *ob);
void DuList_Ensure(char *where, DuObject *ob);
void DuContainer_Ensure(char *where, DuObject *ob);
void DuCons_Ensure(char *where, DuObject *ob);
void DuSymbol_Ensure(char *where, DuObject *ob);
void DuFrame_Ensure(char *where, DuObject *ob);

DuObject *DuInt_FromInt(int value);
int DuInt_AsInt(DuObject *ob);

DuObject *DuList_New(void);
void DuList_Append(DuObject *list, DuObject *item);
int DuList_Size(DuObject *list);
DuObject *DuList_GetItem(DuObject *list, int index);
void DuList_SetItem(DuObject *list, int index, DuObject *newobj);
DuObject *DuList_Pop(DuObject *list, int index);

DuObject *DuContainer_New(void);
DuObject *DuContainer_GetRef(DuObject *container);
void DuContainer_SetRef(DuObject *container, DuObject *newobj);

DuObject *DuSymbol_FromString(char *name);
char *DuSymbol_AsString(DuObject *ob);

DuObject *DuCons_New(DuObject *car, DuObject *cdr);
DuObject *DuCons_Car(DuObject *cons);
DuObject *DuCons_Cdr(DuObject *cons);
DuObject *_DuCons_CAR(DuObject *cons);
DuObject *_DuCons_NEXT(DuObject *cons);

void Du_FatalError(char *msg, ...) NORETURN;
DuObject *Du_Compile(char *filename, int stop_after_newline);
void Du_Print(DuObject *ob, int newline);

DuObject *Du_Eval(DuObject *ob, DuObject *locals);
DuObject *Du_Progn(DuObject *cons, DuObject *locals);

DuObject *DuFrame_New();
DuObject *DuFrame_Copy(DuObject *frame);
DuObject *DuFrame_GetSymbol(DuObject *frame, DuObject *symbol);
void DuFrame_SetSymbol(DuObject *frame, DuObject *symbol, DuObject *value);
void DuFrame_SetSymbolStr(DuObject *frame, char *name, DuObject *value);
void DuFrame_SetBuiltinMacro(DuObject *frame, char *name, eval_fn func);
void DuFrame_SetUserFunction(DuObject *frame, DuObject *symbol,
                             DuObject *arglist, DuObject *progn);
DuObject *_DuFrame_EvalCall(DuObject *frame, DuObject *symbol,
                            DuObject *rest, int execute_now);

void Du_Initialize(void);
void Du_Finalize(void);
void _Du_InitializeObjects(void);
void _Du_FinalizeObjects(void);
#ifdef Du_TRACK_REFS
void _Du_MakeImmortal(void);
#endif
extern DuObject *Du_Globals;

void Du_TransactionAdd(DuObject *code, DuObject *frame);
void Du_TransactionRun(void);


#include "stm/ame.h"

#endif  /* _DUHTON_H_ */
