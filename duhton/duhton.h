#ifndef _DUHTON_H_
#define _DUHTON_H_

/* #undef USE_GIL */            /* forces "gil-c7" instead of "c7" */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#ifdef USE_GIL
#  include "../gil-c7/stmgc.h"
#else
#  include "../c7/stmgc.h"
#endif


extern __thread stm_thread_local_t stm_thread_local;

struct DuObject_s {
    struct object_s header;
    uint32_t type_id;
};
typedef TLPREFIX struct DuObject_s DuObject;


#define DuOBJECT_HEAD1   struct DuObject_s ob_base;


#ifdef __GNUC__
#  define NORETURN  __attribute__((noreturn))
#else
#  define NORETURN  /* nothing */
#endif


typedef void(*trace_fn)(struct DuObject_s *, void visit(object_t **));
typedef size_t(*bytesize_fn)(struct DuObject_s *);
typedef void(*print_fn)(DuObject *);
typedef DuObject *(*eval_fn)(DuObject *, DuObject *);
typedef int(*len_fn)(DuObject *);

typedef struct {
    const char *dt_name;
    int dt_typeindex;
    int dt_size;
    trace_fn dt_trace;
    print_fn dt_print;
    eval_fn dt_eval;
    len_fn dt_is_true;
    len_fn dt_length;
    bytesize_fn dt_bytesize;
} DuType;

/* keep this list in sync with object.c's Du_Types[] */
#define DUTYPE_INVALID       0
#define DUTYPE_NONE          1
#define DUTYPE_INT           2
#define DUTYPE_SYMBOL        3
#define DUTYPE_CONS          4
#define DUTYPE_LIST          5
#define DUTYPE_TUPLE         6
#define DUTYPE_FRAME         7
#define DUTYPE_FRAMENODE     8
#define DUTYPE_CONTAINER     9
#define _DUTYPE_TOTAL       10

extern DuType DuNone_Type;
extern DuType DuInt_Type;
extern DuType DuSymbol_Type;
extern DuType DuCons_Type;
extern DuType DuList_Type;
extern DuType DuTuple_Type;
extern DuType DuFrame_Type;
extern DuType DuFrameNode_Type;
extern DuType DuContainer_Type;

extern DuType *Du_Types[_DUTYPE_TOTAL];

#define ROUND_UP(size)  ((size) < 16 ? 16 : ((size) + 7) & ~7)


DuObject *DuObject_New(DuType *tp);
int DuObject_IsTrue(DuObject *ob);
int DuObject_Length(DuObject *ob);


extern DuObject *Du_None;

#define _DuObject_TypeNum(ob) (((DuObject*)(ob))->type_id)
#define Du_TYPE(ob)           (Du_Types[_DuObject_TypeNum(ob)])
#define DuInt_Check(ob)       (_DuObject_TypeNum(ob) == DUTYPE_INT)
#define DuSymbol_Check(ob)    (_DuObject_TypeNum(ob) == DUTYPE_SYMBOL)
#define DuCons_Check(ob)      (_DuObject_TypeNum(ob) == DUTYPE_CONS)
#define DuList_Check(ob)      (_DuObject_TypeNum(ob) == DUTYPE_LIST)
#define DuFrame_Check(ob)     (_DuObject_TypeNum(ob) == DUTYPE_FRAME)
#define DuContainer_Check(ob) (_DuObject_TypeNum(ob) == DUTYPE_CONTAINER)

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

DuObject *DuContainer_New(DuObject *obj);
DuObject *DuContainer_GetRef(DuObject *container);
void DuContainer_SetRef(DuObject *container, DuObject *newobj);

DuObject *DuSymbol_FromString(const char *name);
char *DuSymbol_AsString(DuObject *ob);
int DuSymbol_Id(DuObject *ob);

typedef TLPREFIX struct DuConsObject_s {
    DuOBJECT_HEAD1
    DuObject *car, *cdr;
} DuConsObject;

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

void Du_Initialize(int);
void Du_Finalize(void);
extern DuObject *Du_Globals;

void Du_TransactionAdd(DuObject *code, DuObject *frame);
void Du_TransactionRun(void);


#define _du_save1(p1)           (_push_root((DuObject *)(p1)))
#define _du_save2(p1,p2)        (_push_root((DuObject *)(p1)),  \
                                 _push_root((DuObject *)(p2)))
#define _du_save3(p1,p2,p3)     (_push_root((DuObject *)(p1)),  \
                                 _push_root((DuObject *)(p2)),  \
                                 _push_root((DuObject *)(p3)))
#define _du_save4(p1,p2,p3,p4)  (_push_root((DuObject *)(p1)), \
                                 _push_root((DuObject *)(p2)),          \
                                 _push_root((DuObject *)(p3)),          \
                                 _push_root((DuObject *)(p4)))


#define _du_restore1(p1)        (p1 = (typeof(p1))_pop_root())
#define _du_restore2(p1,p2)     (p2 = (typeof(p2))_pop_root(),  \
                                 p1 = (typeof(p1))_pop_root())
#define _du_restore3(p1,p2,p3)  (p3 = (typeof(p3))_pop_root(),  \
                                 p2 = (typeof(p2))_pop_root(),  \
                                 p1 = (typeof(p1))_pop_root())
#define _du_restore4(p1,p2,p3,p4)(p4 = (typeof(p4))_pop_root(), \
                                  p3 = (typeof(p3))_pop_root(),         \
                                  p2 = (typeof(p2))_pop_root(),         \
                                  p1 = (typeof(p1))_pop_root())


#define _du_read1(p1)           stm_read((object_t *)(p1))
#define _du_write1(p1)          stm_write((object_t *)(p1))

#define INIT_PREBUILT(p)       ((typeof(p))stm_setup_prebuilt((object_t *)(p)))


#ifndef NDEBUG
# define _check_not_free(ob)                                    \
    assert(_DuObject_TypeNum(ob) > DUTYPE_INVALID && \
           _DuObject_TypeNum(ob) < _DUTYPE_TOTAL)
#endif

static inline void _push_root(DuObject *ob) {
    #ifndef NDEBUG
    if (ob) _check_not_free(ob);
    #endif
    STM_PUSH_ROOT(stm_thread_local, ob);
}
static inline object_t *_pop_root(void) {
    object_t *ob;
    STM_POP_ROOT(stm_thread_local, ob);
    #ifndef NDEBUG
    if (ob) _check_not_free(ob);
    #endif
    return ob;
}

extern pthread_t *all_threads;
extern int all_threads_count;

//extern __thread DuObject *stm_thread_local_obj;  /* XXX temp */
#endif  /* _DUHTON_H_ */
