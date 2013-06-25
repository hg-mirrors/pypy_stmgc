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
        _du_save1(list);
        DuObject *item = DuSymbol_FromString("progn");
        _du_restore1(list);
        _du_save1(list);
        DuList_Append(list, item);
        _du_restore1(list);
    }
    c = fgetc(f);
    while (1) {
        DuObject *item;

        switch (c) {

        case EOF:
            if (level > 0)
                Du_FatalError("more '(' than ')'");
            if (stop_after_newline) {
                return NULL;
            }
            goto done;

        case '(':
            _du_save1(list);
            item = _Du_Parse(f, level + 1, 0);
            _du_restore1(list);
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
                _du_save1(list);
                if (*end == '\0') {
                    item = DuInt_FromInt(number);
                }
                else {
                    item = DuSymbol_FromString(token);
                }
                _du_restore1(list);
                break;
            }
        }
        _du_save1(list);
        DuList_Append(list, item);
        _du_restore1(list);
    }

 done:
    cons = Du_None;
    for (i = DuList_Size(list) - 1; i >= 0; i--) {
        DuObject *item = DuList_GetItem(list, i);
        _du_save1(list);
        cons = DuCons_New(item, cons);
        _du_restore1(list);
    }
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
