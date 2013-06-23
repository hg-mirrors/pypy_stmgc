#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "duhton.h"


DuObject *_Du_Parse(FILE *f, int level, int stop_after_newline)
{
    DuObject *cons, *list;
    int c, i;

    list = DuList_New();
    if (level == 0) {
        DuObject *item = DuSymbol_FromString("progn");
        DuList_Append(list, item);
        Du_DECREF(item);
    }
    c = fgetc(f);
    while (1) {
        DuObject *item;

        switch (c) {

        case EOF:
            if (level > 0)
                Du_FatalError("more '(' than ')'");
            if (stop_after_newline) {
                Du_DECREF(list);
                return NULL;
            }
            goto done;

        case '(':
            item = _Du_Parse(f, level + 1, 0);
            c = fgetc(f);
            break;

        case ')':
            if (level == 0)
                Du_FatalError("more ')' than '('");
            goto done;

        case '\n':
            if (stop_after_newline)
                goto done;
            c = fgetc(f);
            continue;

        case ';':
            while (c != '\n' && c != EOF)
                c = fgetc(f);
            continue;

        default:
            if (isspace(c)) {
                c = fgetc(f);
                continue;
            }
            else {
                char token[201];
                char *p = token;
                char *end;
                int number;
                do {
                    *p++ = c;
                    c = fgetc(f);
                } while (!(isspace(c) || c == '(' || c == ')' || c == EOF));
                *p = '\0';
                number = strtol(token, &end, 0);
                if (*end == '\0') {
                    item = DuInt_FromInt(number);
                }
                else {
                    item = DuSymbol_FromString(token);
                }
                break;
            }
        }
        DuList_Append(list, item);
        Du_DECREF(item);
    }

 done:
    Du_INCREF(Du_None);
    cons = Du_None;
    for (i = DuList_Size(list) - 1; i >= 0; i--) {
        DuObject *item = DuList_GetItem(list, i);
        DuObject *newcons = DuCons_New(item, cons);
        Du_DECREF(cons);
        Du_DECREF(item);
        cons = newcons;
    }
    Du_DECREF(list);
    return cons;
}


DuObject *Du_Compile(char *filename, int stop_after_newline)
{
    FILE *f;
    if (strcmp(filename, "-") == 0)
        f = stdin;
    else
        f = fopen(filename, "r");
    if (!f) Du_FatalError("cannot open '%s'", filename);
    DuObject *cons = _Du_Parse(f, 0, stop_after_newline);
    if (f != stdin)
        fclose(f);
    return cons;
}
