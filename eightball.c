/**************************************************************************/
/* EightBall                                                              */
/*                                                                        */
/* The Eight Bit Algorithmic Language                                     */
/* For Apple IIe/c/gs (64K), Commodore 64, VIC-20 +32K RAM expansion      */
/* (also builds for Linux as 32 bit executable (gcc -m32) only)           */
/*                                                                        */
/* Compiles with cc65 v2.15 for VIC-20, C64, Apple II                     */
/* and gcc 7.3 for Linux                                                  */
/*                                                                        */
/* Note that this code assumes that sizeof(int) = sizeof(int*), which is  */
/* true for 6502 (16 bits each) and i686 (32 bits each) - but not amd64   */
/*                                                                        */
/* cc65: Define symbol VIC20 to build for Commodore VIC-20 + 32K.         */
/*       Define symbol C64 to build for Commodore 64.                     */
/*       Define symbol A2E to build for Apple //e.                        */
/*                                                                        */
/* Copyright Bobbi Webber-Manners 2016, 2017, 2018                        */
/*                                                                        */
/* Formatted using indent -kr -nut                                        */
/*                                                                        */
/**************************************************************************/

/**************************************************************************/
/*  GNU PUBLIC LICENCE v3 OR LATER                                        */
/*                                                                        */
/*  This program is free software: you can redistribute it and/or modify  */
/*  it under the terms of the GNU General Public License as published by  */
/*  the Free Software Foundation, either version 3 of the License, or     */
/*  (at your option) any later version.                                   */
/*                                                                        */
/*  This program is distributed in the hope that it will be useful,       */
/*  but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/*  GNU General Public License for more details.                          */
/*                                                                        */
/*  You should have received a copy of the GNU General Public License     */
/*  along with this program.  If not, see <http://www.gnu.org/licenses/>. */
/*                                                                        */
/**************************************************************************/

//#include <stdio.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <unistd.h>

#include "eightballutils.h"
#include "eightballvm.h"

/* Define EXTMEM to enable extended memory support for source code.
 * Define EXTMEMCODE to enable extended memory support for object code.
 * Works for Apple //e only at present, but easy to extend.
 * EXTMEMCODE can only be enabled if EXTMEM is also enabled.
 */ 
#ifdef A2E
#define EXTMEM      /* Enable/disable extended memory for source code */
#define EXTMEMCODE  /* Enable/disable extended memory for object code */
#endif

/* Shortcut define CC65 makes code clearer */
#if defined(VIC20) || defined(C64) || defined(A2E)
#define CC65
#endif

/* Shortcut define CBM is useful! */
#if defined(VIC20) || defined(C64)
#define CBM
#endif

#ifdef CC65
#ifdef CBM
/* Commodore headers */
#include <cbm.h>
#include <peekpoke.h>
#endif
#if defined(A2E)
/* Apple //e headers */
#include <apple2enh.h>
#include <fcntl.h>              /* For open(), close() */
#include <unistd.h>             /* For read(), write() */
#include <conio.h>              /* For clrscr() */
#include <stdio.h>              /* For fopen(), fclose() */
#include <peekpoke.h>
#include <em.h>
#endif
#endif

#ifdef __GNUC__
#include <stdio.h>              /* For FILE */
#endif

//#define TEST
//#define DEBUG_READFILE

//#define EXIT(arg) {printf("%d\n",__LINE__); exit(arg);}
#define EXIT(arg) exit(arg)

#define VARNUMCHARS 4           /* First 4 chars of variable name are significant */
#define SUBRNUMCHARS 8          /* First 8 chars of variable name are significant */

/*
 ***************************************************************************
 * Lightweight functions (lighter than the cc65 library for our use case)
 ***************************************************************************
 */

/*
 * This should use less memory than the table-driven version of isalpha()
 * provided by cc65
 */
#define isalphach(ch) ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z'))

/*
 * This should use less memory than the table-driven version of isdigit()
 * provided by cc65
 */
#define isdigitch(ch) (ch >= '0' && ch <= '9')

/*
 * Implement this ourselves for cc65 - x^y
 */
int _pow(int x, int y)
{
    int i;
    int ret = 1;

    for (i = 0; i < y; i++) {
        ret *= x;
    }
    return ret;
}

/*
 ***************************************************************************
 * Prototypes
 ***************************************************************************
 */
unsigned char P(void);
unsigned char E(void);
unsigned char eval(unsigned char checkNoMore, int *val);
unsigned char parseint(int *);
unsigned char parsehexint(int *);
unsigned char getintvar(char *, int, int *, unsigned char *, unsigned char);
unsigned char openfile(unsigned char);
unsigned char readfile(void);
unsigned char writefile(void);
void list(unsigned int, unsigned int);
void run(unsigned char);
void new(void);
unsigned char parseline(void);
unsigned char docall(void);
unsigned char doreturn(int retvalue);
void emit(enum bytecode code);
void emit_imm(enum bytecode code, int word);
void emitprmsg(void);
void linksubs(void);
void copyfromaux(char *auxptr, unsigned char len);

#define emitldi(x) emit_imm(VM_LDIMM, x)

/*
 ***************************************************************************
 * Globals
 ***************************************************************************
 */

char compile = 0;               /* 0 means interpret, 1 means compile          */
char compilingsub = 0;          /* 1 when compiling subroutine, 0 otherwise    */
char onlyconstants = 0;         /* 0 is normal, 1 means only allow const exprs */
char compiletimelookup = 0;     /* When set to 1, getintvar() will do lookup   */
                                /* rather than code generation                 */

#define FILENAMELEN 15

char readbuf[255];              /* Buffer for reading from file                */
char lnbuf[255];                /* Input text line buffer                      */
char filename[FILENAMELEN+1];   /* Name of bytecode file                       */
char *txtPtr;                   /* Pointer to next character to read in lnbuf  */

#define STACKSZ 16              /* Size of expression stacks   */
#define RETSTACKSZ 64           /* Size of return stack        */

int operand_stack[STACKSZ];     /* Operand stack - grows down  */
unsigned char operator_stack[STACKSZ];  /* Operator stack - grows down */
int return_stack[RETSTACKSZ];   /* Return stack - grows down   */

unsigned char operatorSP;       /* Operator stack pointer      */
unsigned char operandSP;        /* Operand stack pointer       */
unsigned char returnSP;         /* Return stack pointer        */

jmp_buf jumpbuf;                /* For setjmp()/longjmp()      */

#ifndef CBM
FILE *fd;                       /* File descriptor             */
#endif

/*
 * Definitions for the EightBall VM - compilation target
 */

unsigned int rtPC;              /* Program counter when compiling      */
unsigned int rtSP;              /* Stack pointer when compiling        */
unsigned int rtFP;              /* Frame pointer when compiling        */
unsigned int rtPCBeforeEval;    /* Stashed copy of program counter     */
unsigned char *codeptr;         /* Pointer to write VM code to memory  */

#ifdef EXTMEMCODE
unsigned char *codestart;       /* Start address of VM code in ext mem */
#endif

/*
 * Represents a line of EightBall code.
 * The string itself is stored adjacent in regular memory or, if EXTMEM is
 * defined, in extended memory (aux RAM on Apple //e.)
 */
struct lineofcode {
    char *line;
    struct lineofcode *next;
#ifdef EXTMEM
    unsigned char len;
#endif
};

/*
 * Pointer to current line
 */
struct lineofcode *current = NULL;

/*
 * Used as a line number counter
 */
int counter;

/*
 * Holds the return value from a subroutine / function
 * (for the interpreter only)
 */
int retregister = 0;

/*
 ***************************************************************************
 * Token table for expression parser
 ***************************************************************************
 */

/*
 * Single character binary operators - order must match binaryops string.
 * and must be sequential.
 */
#ifdef CBM
const char binaryops[] = "^/%*+-><&#!";
#else
const char binaryops[] = "^/%*+-><&|!";
#endif

#define TOK_POW     245         /* Binary ^               */
#define TOK_DIV     246         /* Binary /               */
#define TOK_MOD     247         /* Binary %               */
#define TOK_MUL     248         /* Binary *               */
#define TOK_ADD     249         /* Binary +               */
#define TOK_SUB     250         /* Binary -               */
#define TOK_GT      251         /* Binary >               */
#define TOK_LT      252         /* Binary <               */
#define TOK_BITAND  253         /* Binary &               */
#define TOK_BITOR   254         /* Binary | (# on CBM)    */
#define TOK_BITXOR  255         /* Binary ! (^ in C code) */

/*
 * Macro to determine if a token is a binary operator with one character.
 */
#define IS1CHBINARY(tok) ((tok >= TOK_POW) && (tok <= TOK_BITXOR))

/*
 * Two character binary operators - order must match binaryops1/2 strings.
 * and must be sequential.
 */
#ifdef CBM
const char binaryops1[] = "=!><&#<>";   /* 1st char */
const char binaryops2[] = "====&#<>";   /* 2nd char */
#else
const char binaryops1[] = "=!><&|<>";   /* 1st char */
const char binaryops2[] = "====&|<>";   /* 2nd char */
#endif

#define TOK_EQL     237         /* Binary ==             */
#define TOK_NEQL    238         /* Binary !=             */
#define TOK_GTE     239         /* Binary >=             */
#define TOK_LTE     240         /* Binary <=             */
#define TOK_AND     241         /* Binary &&             */
#define TOK_OR      242         /* Binary || (## on CBM) */
#define TOK_LSH     243         /* Binary <<             */
#define TOK_RSH     244         /* Binary >>             */

/*
 * Macro to determine if a token is a binary operator with two characters.
 */
#define IS2CHBINARY(tok) ((tok >= TOK_EQL) && (tok <= TOK_RSH))

/*
 * Unary operators - order must match unaryops string and must be sequential.
 * All unary operators are single character.
 */
#ifdef CBM
const char unaryops[] = "-+!.*^";
#else
const char unaryops[] = "-+!~*^";
#endif

#define TOK_UNM     231         /* Unary -                  */
#define TOK_UNP     232         /* Unary +                  */
#define TOK_NOT     233         /* Unary !                  */
#define TOK_BITNOT  234         /* Unary ~ (. on CBM)       */
#define TOK_STAR    235         /* Unary * (word deref.)    */
#define TOK_CARET   236         /* Unary ^ (byte deref.)    */

/*
 * Macro to determine if a token is a unary operator.
 */
#define ISUNARY(tok) ((tok >= TOK_UNM) && (tok <= TOK_CARET))

/*
 * Special token to mark end of stack
 */
#define SENTINEL    50

/*
 * Special token for illegal operator or statement.
 */
#define ILLEGAL     100

/*
 * Error codes
 */
#define ERR_FIRST   101         /* FIRST ERROR NUM    */
#define ERR_NOIF    101         /* No IF              */
#define ERR_NOFOR   102         /* No FOR             */
#define ERR_NOWHILE 103         /* No WHILE           */
#define ERR_NOSUB   104         /* No SUB             */
#define ERR_STACK   105         /* No stack           */
#define ERR_COMPLEX 106         /* Too complex        */
#define ERR_VAR     107         /* Variable expected  */
#define ERR_REDEF   108         /* Variable redefined */
#define ERR_EXPECT  109         /* Expected character */
#define ERR_EXTRA   110         /* Unexpected extra   */
#define ERR_DIM     111         /* Bad dimension      */
#define ERR_SUBSCR  112         /* Bad subscript      */
#define ERR_RUNSUB  113         /* Ran into sub       */
#define ERR_STR     114         /* Bad string         */
#define ERR_FILE    115         /* File error         */
#define ERR_LINE    116         /* Bad line number    */
#define ERR_EXPR    117         /* Invalid expr       */
#define ERR_NUM     118         /* Invalid number     */
#define ERR_ARG     119         /* Argument error     */
#define ERR_TYPE    120         /* Type error         */
#define ERR_DIVZERO 121         /* Divide by zero     */
#define ERR_VALUE   122         /* Bad value          */
#define ERR_CONST   123         /* Const value reqd   */
#define ERR_STCONST 124         /* Const value reqd   */
#define ERR_TOOLONG 125         /* Initializer too lng */
#define ERR_LINK    126         /* Linkage error      */

char *errmsgs[] = {
    "no if",                    /* ERR_NOIF    */
    "no for",                   /* ERR_NOFOR   */
    "no while",                 /* ERR_NOWHILE */
    "no sub",                   /* ERR_NOSUB   */
    "stack",                    /* ERR_STACK   */
    "complex",                  /* ERR_COMPLEX */
    "expect var",               /* ERR_VAR     */
    "redef",                    /* ERR_REDEF   */
    "expected ",                /* ERR_EXPECT  */
    "extra",                    /* ERR_EXTRA   */
    "bad dim",                  /* ERR_DIM     */
    "bad idx",                  /* ERR_SUBSCR  */
    "ran into sub",             /* ERR_RUNSUB  */
    "bad str",                  /* ERR_STR     */
    "file",                     /* ERR_FILE    */
    "bad line#",                /* ERR_LINE    */
    "bad expr",                 /* ERR_EXPR    */
    "bad num",                  /* ERR_NUM     */
    "arg",                      /* ERR_ARG     */
    "type",                     /* ERR_TYPE    */
    "div/0",                    /* ERR_DIVZERO */
    "bad val",                  /* ERR_VALUE   */
    "not const",                /* ERR_CONST   */
    "const",                    /* ERR_STCONST */
    "too long",                 /* ERR_TOOLONG */
    "link"                      /* ERR_LINK    */
};

/*
 * Error reporting
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void error(unsigned char errcode)
{
    printchar('?');
    print(errmsgs[errcode - ERR_FIRST]);
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Based on C's precedence rules.  Higher return value means higher precedence.
 * Ref: http://en.cppreference.com/w/c/language/operator_precedence
 * TODO: Code will probably be smaller if we use a table.
 */
unsigned char getprecedence(int token)
{
    switch (token) {
    case TOK_UNP:
    case TOK_UNM:
    case TOK_STAR:
    case TOK_CARET:
    case TOK_NOT:
    case TOK_BITNOT:
        return 11;
    case TOK_POW:
    case TOK_DIV:
    case TOK_MUL:
    case TOK_MOD:
        return 10;
    case TOK_ADD:
    case TOK_SUB:
        return 9;
    case TOK_LSH:
    case TOK_RSH:
        return 8;
    case TOK_GT:
    case TOK_GTE:
    case TOK_LT:
    case TOK_LTE:
        return 7;
    case TOK_EQL:
    case TOK_NEQL:
        return 6;
    case TOK_BITAND:
        return 5;
    case TOK_BITXOR:
        return 4;
    case TOK_BITOR:
        return 3;
    case TOK_AND:
        return 2;
    case TOK_OR:
        return 1;
    case SENTINEL:
        return 0;  /* Must be lowest precedence! */
    default:
        /* Should never happen */
        EXIT(99);
    }
}

/*
 * Operator stack routines
 */

void push_operator_stack(unsigned char operator)
{
    operator_stack[operatorSP] = operator;
    if (!operatorSP) {
        /* Warm start */
        error(ERR_COMPLEX);
        longjmp(jumpbuf, 1);
    }
    --operatorSP;
}

unsigned char pop_operator_stack()
{
    if (operatorSP == STACKSZ - 1) {
        /* Warm start */
        longjmp(jumpbuf, 1);
    }
    ++operatorSP;
    return operator_stack[operatorSP];
}

#define top_operator_stack() operator_stack[operatorSP + 1]

/*
 * Operand stack routines
 */

void push_operand_stack(int operand)
{
    if (compile) {
        emitldi(operand);
        return;
    }
    operand_stack[operandSP] = operand;
    if (!operandSP) {
        /* Warm start */
        error(ERR_COMPLEX);
        longjmp(jumpbuf, 1);
    }
    --operandSP;
}

int pop_operand_stack()
{
    if (compile) {
        return 0;
    }
    if (operandSP == STACKSZ - 1) {
        /* Warm start */
        longjmp(jumpbuf, 1);
    }
    ++operandSP;
    return operand_stack[operandSP];
}

#define top_operand_stack() operand_stack[operandSP + 1]

/*
 ***************************************************************************
 * Parser proper ...
 ***************************************************************************
 */

/*
 * Binary operators
 *
 * Examines the character at *txtPtr, and if not NULL the character at
 * *(txtPtr+1).  If a two character binary operator is found, return the
 * token, otherwise if a one character binary operator is found return
 * the token.  If neither of these, return ILLEGAL.
 *
 * Uses the global tables binaryops (for single char operators) and
 * binaryops1 / binaryops2 (for two character operators).
 */
unsigned char binary()
{
    unsigned char tok;
    unsigned char idx = 0;

    /*
     * First two char ops (don't try if first char is NULL though!)
     */

    if (*txtPtr) {
        tok = TOK_EQL;
        while (binaryops1[idx]) {
            if (binaryops1[idx] == *txtPtr) {
                if (binaryops2[idx] == *(txtPtr + 1)) {
                    return tok;
                }
            }
            ++idx;
            ++tok;
        }
    }

    /*
     * Now single char ops
     */
    idx = 0;
    tok = TOK_POW;
    while (binaryops[idx]) {
        if (binaryops[idx] == *txtPtr) {
            return tok;
        }
        ++idx;
        ++tok;
    }

    return ILLEGAL;
}

/*
 * Unary operators
 *
 * Examines the character at *txtPtr.  If it is one of the unary operators
 * (which are all single character), returns the token value, otherwise
 * returns ILLEGAL.
 *
 * Uses the global tables unaryops (for single char operators).
 */
unsigned char unary()
{
    unsigned char idx = 0;
    unsigned char tok = TOK_UNM;

    while (unaryops[idx]) {
        if (unaryops[idx] == *txtPtr) {
            return tok;
        }
        ++idx;
        ++tok;
    }
    return ILLEGAL;
}

/*
 * Pop an operator from the operator stack, pop the operands from the
 * operand stack and apply the operator to the operands.
 * Returns 0 if successful, 1 on error
 */
unsigned char pop_operator()
{
    int operand2;
    int result;
    int token = pop_operator_stack();
    int operand1 = pop_operand_stack();

    if (!ISUNARY(token)) {

        /*
         * Evaluate binary operator
         * (Apply the operator token to operand1, operand2)
         */
        operand2 = pop_operand_stack();

        switch (token) {
        case TOK_POW:
            result = _pow(operand2, operand1);
            break;
        case TOK_MUL:
            if (compile) {
                emit(VM_MUL);
                return 0;
            }
            result = operand2 * operand1;
            break;
        case TOK_DIV:
            if (compile) {
                emit(VM_DIV);
                return 0;
            }
            if (operand1 == 0) {
                error(ERR_DIVZERO);
                return 1;
            } else {
                result = operand2 / operand1;
            }
            break;
        case TOK_MOD:
            if (compile) {
                emit(VM_MOD);
                return 0;
            }
            if (operand1 == 0) {
                error(ERR_DIVZERO);
                return 1;
            } else {
                result = operand2 % operand1;
            }
            break;
        case TOK_ADD:
            if (compile) {
                emit(VM_ADD);
                return 0;
            }
            result = operand2 + operand1;
            break;
        case TOK_SUB:
            if (compile) {
                emit(VM_SUB);
                return 0;
            }
            result = operand2 - operand1;
            break;
        case TOK_GT:
            if (compile) {
                emit(VM_GT);
                return 0;
            }
            result = operand2 > operand1;
            break;
        case TOK_GTE:
            if (compile) {
                emit(VM_GTE);
                return 0;
            }
            result = operand2 >= operand1;
            break;
        case TOK_LT:
            if (compile) {
                emit(VM_LT);
                return 0;
            }
            result = operand2 < operand1;
            break;
        case TOK_LTE:
            if (compile) {
                emit(VM_LTE);
                return 0;
            }
            result = operand2 <= operand1;
            break;
        case TOK_EQL:
            if (compile) {
                emit(VM_EQL);
                return 0;
            }
            result = operand2 == operand1;
            break;
        case TOK_NEQL:
            if (compile) {
                emit(VM_NEQL);
                return 0;
            }
            result = operand2 != operand1;
            break;
        case TOK_AND:
            if (compile) {
                emit(VM_AND);
                return 0;
            }
            result = operand2 && operand1;
            break;
        case TOK_OR:
            if (compile) {
                emit(VM_OR);
                return 0;
            }
            result = operand2 || operand1;
            break;
        case TOK_BITAND:
            if (compile) {
                emit(VM_BITAND);
                return 0;
            }
            result = operand2 & operand1;
            break;
        case TOK_BITOR:
            if (compile) {
                emit(VM_BITOR);
                return 0;
            }
            result = operand2 | operand1;
            break;
        case TOK_BITXOR:
            if (compile) {
                emit(VM_BITXOR);
                return 0;
            }
            result = operand2 ^ operand1;
            break;
        case TOK_LSH:
            if (compile) {
                emit(VM_LSH);
                return 0;
            }
            result = operand2 << operand1;
            break;
        case TOK_RSH:
            if (compile) {
                emit(VM_RSH);
                return 0;
            }
            result = operand2 >> operand1;
            break;
        default:
            /* Should never happen */
            EXIT(99);
        }

    } else {

        /*
         * Evaluate unary operator
         * (Apply the operator token to operand1)
         */

        switch (token) {
        case TOK_UNM:
            if (compile) {
                emit(VM_NEG);
                return 0;
            }
            result = -operand1;
            break;
        case TOK_UNP:
            if (compile) {
                return 0;
            }
            result = operand1;
            break;
        case TOK_NOT:
            if (compile) {
                emit(VM_NOT);
                return 0;
            }
            result = !operand1;
            break;
        case TOK_BITNOT:
            if (compile) {
                emit(VM_BITNOT);
                return 0;
            }
            result = ~operand1;
            break;
        case TOK_STAR:
            if (compile) {
                emit(VM_LDAWORD);
                return 0;
            }
            result = *((int *) operand1);
            break;
        case TOK_CARET:
            if (compile) {
                emit(VM_LDABYTE);
                return 0;
            }
            result = *((unsigned char *) operand1);
            break;
        default:
            /* Should never happen */
            EXIT(99);
        }
    }
    push_operand_stack(result);
    return 0;
}

/*
 * Returns 0 if successful, 1 on error
 */
unsigned char push_operator(int operator_token)
{
    /* Handles operator precedence here */
    while (getprecedence(top_operator_stack()) >=
           getprecedence(operator_token)) {

        if (pop_operator()) {
            return 1;
        }
    }
    push_operator_stack(operator_token);
    return 0;
}


#define CALLFRAME   0xfffe      /* Magic number for CALL stack frame           */
#define IFFRAME     0xfffd      /* Magic number for IF stack frame             */
#define FORFRAME_B  0xfffc      /* Magic number for FOR stack frame - byte var */
#define FORFRAME_W  0xfffb      /* Magic number for FOR stack frame - word var */
#define WHILEFRAME  0xfffa      /* Magic number for WHILE stack frame          */

/*
 * Push line number (or other int) to return stack.
 */
void push_return(int linenum)
{
    return_stack[returnSP] = linenum;
    if (!returnSP) {
        error(ERR_STACK);
        longjmp(jumpbuf, 1);
    }
    --returnSP;
}

/*
 * Pop line number (or other int) from return stack.
 */
int pop_return()
{
    if (returnSP == RETSTACKSZ - 1) {
        error(ERR_STACK);
        longjmp(jumpbuf, 1);
    }
    ++returnSP;
    return return_stack[returnSP];
}

/*
 * Consume any space characters at txtPtr
 * Macro so it inlines on 6502 for speed.
 */
#define eatspace() \
while (*txtPtr == ' ') { \
    ++txtPtr; \
}

/*
 * Returns 0 on success, 1 if error
 * This is only ever invoked for single character tokens
 * (which allows some simplification)
 */
unsigned char expect(unsigned char token)
{
    if (*txtPtr == token) {
        ++txtPtr;               // expect() only called for one char tokens
        eatspace();
        return 0;
    } else {
        error(ERR_EXPECT);
        printchar(token);
        return 1;
    }
}

/*
 * Handles an expression
 * Returns 0 on success, 1 on error
 */
unsigned char E()
{
    int op;

    if (P()) {
        return 1;
    }

    while ((op = binary()) != ILLEGAL) {
        if (push_operator(op)) {
            return 1;
        }
        if (IS1CHBINARY(op)) {
            ++txtPtr;
        } else {
            txtPtr += 2;
        }
        if (P()) {
            return 1;
        }
    }

    while (top_operator_stack() != SENTINEL) {
        if (pop_operator()) {
            return 1;
        }
    }
    return 0;
}

/*
 * Parse array subscript
 * Returns 0 if '[expr]' is found, 1 otherwise
 */
unsigned char subscript(int *idx)
{
    /* Start a new subexpression */
    push_operator_stack(SENTINEL);

    if (expect('[')) {
        return 1;
    }
    if (eval(0, idx)) {
        return 1;
    }
    if (expect(']')) {
        return 1;
    }

    /* Throw away SENTINEL */
    pop_operator_stack();

    return 0;
}

/*
 * Handles a predicate
 * Returns 0 on success, 1 on error
 * If the global variable onlyconstants is set then only allow constant predicates.
 */
unsigned char P()
{
    struct lineofcode *oldcurrent;
    int oldcounter;
    char key[VARNUMCHARS];
    int idx;
    char *writePtr;
    unsigned char addressmode;  /* Set to 1 if there is '&' */
    int arg = 0;
    unsigned char type;

    eatspace();

    if (!(*txtPtr)) {
        error(ERR_EXPR);
        return 1;
    }

    if ((*txtPtr == '&') || (isalphach(*txtPtr))) {

        addressmode = 0;

        /*
         * Handle address-of operator
         */
        if (*txtPtr == '&') {
            addressmode = 1;
            ++txtPtr;
            if (!isalphach(*txtPtr)) {
                error(ERR_VAR);
                return 1;
            }
        }

        /*
         * Handle variables
         */
        writePtr = readbuf;
        while (isalphach(*txtPtr) || isdigitch(*txtPtr)) {
            if (arg < VARNUMCHARS) {
                key[arg++] = *txtPtr;
            }
            *writePtr = *txtPtr;
            ++txtPtr;
            ++writePtr;
        }
        if (arg < VARNUMCHARS) {
            key[arg] = '\0';
        }
        *writePtr = '\0';

        idx = -1;
        if (*txtPtr == '[') {
            idx = 0;
            if (subscript(&idx) == 1) {
                error(ERR_SUBSCR);
                return 1;
            }

        } else if (*txtPtr == '(') {

            /*
             * Function invokation
             */

            if (onlyconstants) {
                error(ERR_CONST);
                return 1;
            }

            /* No taking address of functions thank you! */
            if (addressmode) {
                error(ERR_VAR);
                return 1;
            }

            if (compile) {
                push_operator_stack(SENTINEL);
                if (docall()) {
                    return 1;
                }

                pop_operator_stack();

            } else {

                push_operator_stack(SENTINEL);

                oldcurrent = current;
                oldcounter = counter;

                /*
                 * For CALL, stack frame is just the
                 * magic number, the return line (-2 in this case)
                 * and the txtPtr pointer (-1 again).
                 *
                 * We create this fake CALLFRAME so that the call to 
                 * run() below terminates after hitting a return
                 * statement in the sub being called.
                 */
                push_return(CALLFRAME);
                push_return(-2);        /* Magic number for function */
                push_return(-1);

                /*
                 * Function call - sets up current, counter and txtPtr
                 * to first line of subroutine being called.
                 */
                if (docall()) {
                    return 1;
                }

                /*
                 * Run the function.  When the function returns it 
                 * is treated as immediate mode, so it comes back
                 * here.  txtPtr is restored to immediately after
                 * the call automatically.
                 */
                run(1);

                current = oldcurrent;
                counter = oldcounter;

#ifdef EXTMEM
                // Restore embuf, which is trashed by the call to run() above
                copyfromaux(current->line, current->len);
#endif
                /*
                 * Throw away our CALLFRAME.
                 */
                pop_return();
                pop_return();
                pop_return();

                /* Throw away the sentinel */
                pop_operator_stack();

                push_operand_stack(retregister);
            }
            goto skip_var;      // MESSY!!
        }

        if (compile) {
            compiletimelookup = 1;
            if (getintvar(key, idx, &arg, &type, addressmode)) {
                return 1;
            }

            if (type & 0x20) {
                push_operand_stack(arg);
                goto skip_var;
            }
        }

        if (getintvar(key, idx, &arg, &type, addressmode)) {
            return 1;
        }

        /* If onlyconstants is set then only allow const variables */
        if (onlyconstants && !(type & 0x20)) {
            error(ERR_CONST);
            return 1;
        }

        if (!compile) {
            push_operand_stack(arg);
        }

      skip_var:
        eatspace();

    } else if (isdigitch(*txtPtr)) {
        /*
         * Handle integer constants
         */
        if (parseint(&arg)) {
            error(ERR_NUM);
            return 1;
        }
        push_operand_stack(arg);
        eatspace();

    } else if (*txtPtr == '$') {
        /*
         * Handle hex constants
         */
        ++txtPtr;               /* Eat the $ */
        if (parsehexint(&arg)) {
            error(ERR_NUM);
            return 1;
        }
        push_operand_stack(arg);
        eatspace();

    } else if (*txtPtr == '\'') {
        /*
         * Handle character constants
         */
        ++txtPtr;               /* Eat the ' */
        arg = *txtPtr;
        ++txtPtr;
        if (*txtPtr != '\'') {
            error(ERR_NUM);
            return 1;
        }
        ++txtPtr;               /* Eat the ' */
        push_operand_stack(arg);
        eatspace();

    } else if (*txtPtr == '(') {
        /*
         * Handle subexpressions in parenthesis
         */
        ++txtPtr;
        push_operator_stack(SENTINEL);
        if (E()) {
            return 1;
        }
        if (expect(')')) {
            return 1;
        }
        pop_operator_stack();

    } else if ((arg = unary()) != ILLEGAL) {
        /*
         * Handle unary operator
         */
        push_operator_stack(arg);
        ++txtPtr;
        if (P()) {
            return 1;
        }

    } else {
        /*
         * Otherwise error
         */
        error(ERR_EXTRA);
        printchar(' ');
        printchar(*txtPtr);
        return 1;
    }
    return 0;
}

/*
 * Evaluate expression at txtPtr
 * If checkNoMore is 1 then check there is no extra input to be consumed.
 * eval() is basically a wrapper around the expression parser routine E().
 * Result is returned via argument val.
 * Returns 0 if successful, 1 on error.
 */
unsigned char eval(unsigned char checkNoMore, int *val)
{

    eatspace();

    if (!(*txtPtr)) {
        error(ERR_EXPR);
        return 1;
    }
    if (E()) {
        return 1;
    }
    if (checkNoMore == 1) {
        if (*txtPtr == ';') {
            goto doret;
        }

        if (*txtPtr) {
            error(ERR_EXTRA);
            printchar(' ');
            print(txtPtr);
            return 1;
        }
    }
  doret:
    *val = pop_operand_stack();

    return 0;
}

/*
 * Everything above this line is the expression parser.
 * Everything below is the rest of the language implementation.
 */

unsigned char *heap1Ptr;        /* Arena 1: top-down stack */
unsigned char *heap2PtrTop;     /* Arena 2: top-down stack */
unsigned char *heap2PtrBttm;    /* Arena 2: bottom-up heap */

#ifdef A2E
unsigned char *auxmemPtrBttm;   /* Auxiliary memory: bottom up heap */
#endif

#ifdef A2E

/*
 * Apple II Enhanced
 *
 * Code starts at 0x0800.  Top of memory is 0xbf00.
 * Stack is 2K immediately below 0xbfff. (0xb800-0xbfff) ??
 *
 * Heap usage:
 *   Heap 1: Variables
 *   Heap 2: Linked list of pointers to lines of program text
 *   Auxiliary memory is used to store program text
 */
#define HEAP1TOP (char*)0xb7ff
#define HEAP1LIM (char*)0x9800

#define HEAP2TOP (char*)(HEAP1LIM - 1)
#ifdef EXTMEM
#define HEAP2LIM (char*)0x8000
#else
#define HEAP2LIM (char*)0x6f00
#endif
                                 /* HEAP2LIM HAS TO BE ADJUSTED TO NOT
                                  * TRASH THE CODE, WHICH LOADS FROM $0800 UP
                                  * USE THE MAPFILE! */

#define AUXMEMTOP (char*)(192*256)  /* Amount of aux memory available */
#define AUXMEMBTTM (char*)2048      /* Bottom 2K of aux mem is used for 80 cols */

#elif defined(C64)

/*
 * C64
 *
 * Here we have a continuous block of RAM from the top of the executable
 * to 0xbfff.  I retain the heap 1 / heap 2 convention of the VIC-20 for now.
 * For now I assign 8K to heap 1 and whatever is left for heap 2.
 *
 * Heap usage:
 *   Heap 1: Variables
 *   Heap 2: Program text
 */
#define HEAP1TOP (char*)0xbfff  /* Leaves 2K for stack, and 2K for ??? */
#define HEAP1LIM (char*)0xa000

#define HEAP2TOP (char*)0x9fff - 0x0400 /* Leave $800 for the C stack */
#define HEAP2LIM (char*)0x6f00
                                 /* HEAP2LIM HAS TO BE ADJUSTED TO NOT
                                  * TRASH THE CODE, WHICH LOADS FROM $0800 UP
                                  * USE THE MAPFILE! */


#elif defined(VIC20)

/*
 * VIC-20:
 *
 * We have two heaps because we have two discontinuous blocks of free memory
 * Heap 1: one using all of BLK5 (8KB)
 * Heap 2: growing down from the top of BLK3 (27.5K less size of executable)
 * The executable is around 19K at the time of writing.
 *
 * Heap usage:
 *   Heap 1: Variables
 *   Heap 2: Program text
 */
//#define HEAP1TOP (char*)0xbfff
//#define HEAP1LIM (char*)0xa000

//#define HEAP2TOP (char*)0x7fff - 0x0400 /* Leave $400 for the C stack */
//#define HEAP2LIM (char*)0x7600  /* HEAP2LIM HAS TO BE ADJUSTED TO NOT
//                                 * TRASH THE CODE, WHICH LOADS FROM $1200 UP
//                                 * USE THE MAPFILE! */

// Everything in BLK5 for now
// BLK3 is almost totally full of code!
// Man ... we really need more memory!!
#define HEAP1TOP (char*)0xbfff
#define HEAP1LIM (char*)0xb000
#define HEAP2TOP (char*)0xafff
#define HEAP2LIM (char*)0xa000


#endif

#ifdef __GNUC__

#define HEAP1SZ 1024*16
unsigned char heap1[HEAP1SZ];
#define HEAP1TOP (heap1 + HEAP1SZ - 1)
#define HEAP1LIM heap1

#endif

/*
 * When compiling, generated code will be stored from start of HEAP 1.
 * Symbol tables grow down from the top of HEAP1
 */
#define CODESTART      HEAP1LIM

/*
 * Clears heap 1.  Must call this before using alloc1().
 */
#define CLEARHEAP1() heap1Ptr = HEAP1TOP

/*
 * Clears heap 2 top-down stack.  Must call this before using alloc2top().
 */
#ifdef CC65
#define CLEARHEAP2TOP() heap2PtrTop = HEAP2TOP
#endif

/*
 * Clears heap 2 bottom-up heap.  Must call this before using alloc2bttm().
 */
#ifdef CC65
#define CLEARHEAP2BTTM() heap2PtrBttm = HEAP2LIM
#endif

/*
 * Clears aux mem bottom-up heap.  Must call this before using allocauxmem().
 */
#ifdef CC65
#ifdef EXTMEM
#define CLEARAUXMEM() auxmemPtrBttm = AUXMEMBTTM;
#endif
#endif

/*
 * Clears runtime call stack (target system when compiling)
 * Called before compilation begins.
 */
#ifdef EXTMEMCODE
#define CLEARRTCALLSTACK() rtSP = RTCALLSTACKTOP; rtFP = rtSP; rtPC = RTPCSTART; codeptr = auxmemPtrBttm; codestart = codeptr;
#else
#define CLEARRTCALLSTACK() rtSP = RTCALLSTACKTOP; rtFP = rtSP; rtPC = RTPCSTART; codeptr = CODESTART;
#endif

/*
 * Allocate bytes on heap 1.
 */
void *alloc1(unsigned int bytes)
{
    if ((heap1Ptr - bytes) < HEAP1LIM) {
        print("No mem (1)!\n");
        longjmp(jumpbuf, 1);
    }
    heap1Ptr -= bytes;
    return heap1Ptr;
}

/*
 * Free bytes on heap 1.
 */
void free1(unsigned int bytes)
{
    heap1Ptr += bytes;
}

/*
 * Allocate bytes on target's call stack
 * Starts at RTCALLSTACKTOP and grows down.
 *
 * To have this track the VM's stack pointer, be sure
 * to emit stack push instuctions (VM_PSHWORD / VM_PSHBYTE)
 * instructions that match calls to
 * rt_push_callstack().
 */
unsigned int rt_push_callstack(unsigned int bytes)
{
    if ((rtSP - bytes) < RTCALLSTACKLIM) {
        print("No tgt mem!\n");
        longjmp(jumpbuf, 1);
    }
    rtSP -= bytes;
    return rtSP;
}

/*
 * Free bytes on target's call stack
 *
 * To have this track the VM's stack pointer, be sure
 * to emit VM_POPWORD / VM_POPBYTE / VM_RTS instructions
 * that match calls to rt_pop_callstack().
 *
 * Note that local variables are freed on function end
 * using the FPSP instruction.
 */
void rt_pop_callstack(unsigned int bytes)
{
    rtSP += bytes;
}

/*
 * Allocate bytes on the stack at the top of heap 2.
 */
void *alloc2top(unsigned int bytes)
{
#ifdef __GNUC__
    void *p = malloc(bytes);
    if (!p) {
        print("No mem (2)!\n");
        longjmp(jumpbuf, 1);
    }
    return p;
#else
    if ((heap2PtrTop - bytes) < heap2PtrBttm) {
        print("No mem (2)!\n");
        longjmp(jumpbuf, 1);
    }
    heap2PtrTop -= bytes;
    return heap2PtrTop;
#endif
}

/*
 * Allocate bytes on the heap at the bottom of heap 2.
 */
void *alloc2bttm(unsigned int bytes)
{
#ifdef __GNUC__
    void *p = malloc(bytes);
    if (!p) {
        print("No mem (2)!\n");
        longjmp(jumpbuf, 1);
    }
    return p;
#else
    void *p = heap2PtrBttm;
    if ((heap2PtrBttm + bytes) > heap2PtrTop) {
        print("No mem (2)!\n");
        longjmp(jumpbuf, 1);
    }
    heap2PtrBttm += bytes;
    return p;
#endif
}

/*
 * Return the total amount of free space on heap 1.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
int getfreespace1()
{
    return (heap1Ptr - HEAP1LIM + 1);
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Return total amount of space in heap 1.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
int gettotalspace1()
{
    return (HEAP1TOP - HEAP1LIM + 1);
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Return the total amount of free space on heap 2.
 * This is the space between the bottom of the downwards-growning
 * stack at the top and the top of the upwards-growing heap at the
 * bottom.
 */
#ifdef CC65
#ifdef A2E
#pragma code-name (push, "LC")
#endif
int getfreespace2()
{
    return (heap2PtrTop - heap2PtrBttm + 1);
}
#ifdef A2E
#pragma code-name (pop)
#endif
#endif

/*
 * Return total amount of space in heap 2.
 */
#ifdef CC65
#ifdef A2E
#pragma code-name (push, "LC")
#endif
int gettotalspace2()
{
    return (HEAP2TOP - HEAP2LIM + 1);
}
#ifdef A2E
#pragma code-name (pop)
#endif
#endif

#ifdef CC65
#ifdef EXTMEM
/*
 * This is used to keep track of allocations in Apple II auxiliary memory.
 * This memory is accessed using cc65's extended memory (EM) driver.
 */
void *allocauxmem(unsigned int bytes) {
    void *p = auxmemPtrBttm;
    if ((auxmemPtrBttm + bytes) > AUXMEMTOP) {
        print("No aux mem!\n");
        longjmp(jumpbuf, 1);
    }
    auxmemPtrBttm += bytes;
    return p;
}

/*
 * Returns the number of bytes of aux mem free
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
int getfreeauxmem()
{
    return (AUXMEMTOP - auxmemPtrBttm + 1);
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Returns total number of usable bytes of aux mem available
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
int gettotalauxmem()
{
    return (AUXMEMTOP - AUXMEMBTTM + 1);
}
#ifdef A2E
#pragma code-name (pop)
#endif

#endif
#endif

#ifdef EXTMEM
struct em_copy emcopy;

char embuf[255];
char embuf2[255];

char b;
extern char *addrptr;
#pragma zpsym ("addrptr");

/*
 * This inline assembler version avoids the memory corruption
 * which results from using em_copyto() in this situation.
 */
void copybytetoaux(char *auxptr, char byte) {
    addrptr = auxptr; /* addrptr is in zero page */
    addrptr += 0x200; /* BASE address offset */
    b = byte;
    __asm__("sta $c005"); /* Write to aux mem */
    __asm__("lda %v", b);
    __asm__("sta (%v)", addrptr); /* 65C02 instruction */
    __asm__("sta $c004"); /* Back to normal */
#if 0 
    b = byte;
    emcopy.buf = &b;
    emcopy.count = 1;
    emcopy.offs = (unsigned char)auxptr;
    emcopy.page = (unsigned int)auxptr >> 8;
    em_copyto(&emcopy);
#endif
}

void copybytefromaux(char *auxptr) {
    emcopy.buf = embuf;
    emcopy.count = 1;
    emcopy.offs = (unsigned char)auxptr;
    emcopy.page = (unsigned int)auxptr >> 8;
    em_copyfrom(&emcopy);
}

void copytoaux(char *auxptr, char *line) {
    emcopy.buf = line;
    emcopy.count = strlen(line) + 1;
    emcopy.offs = (unsigned char)auxptr;
    emcopy.page = (unsigned int)auxptr >> 8;
    em_copyto(&emcopy);
}

void copyfromaux(char *auxptr, unsigned char len) {
    emcopy.buf = embuf;
    emcopy.count = len + 1; /* Remember the NULL */
    emcopy.offs = (unsigned char)auxptr;
    emcopy.page = (unsigned int)auxptr >> 8;
    em_copyfrom(&emcopy);
}

void copyfromaux2(char *auxptr, unsigned char len) {
    emcopy.buf = embuf2;
    emcopy.count = len + 1; /* Remember the NULL */
    emcopy.offs = (unsigned char)auxptr;
    emcopy.page = (unsigned int)auxptr >> 8;
    em_copyfrom(&emcopy);
}
#endif

/*
 * Compiler: Emit simple one byte code
 * Used for everything except immediate mode opcodes
 * Stores using codeptr.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void emit(enum bytecode code)
{
/*
    unsigned char c = code;
*/

#ifdef EXTMEMCODE
    copybytetoaux(codeptr++, code);
#else
    *codeptr++ = code;
#endif

/*
    printhex(rtPC);
    print(": ");
    printhexbyte(c);
    print("       : ");
    print(bytecodenames[c]);
    printchar('\n');
*/
    ++rtPC;
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Compiler: Emit opcode and 16 bit word argument
 * Stores using codeptr.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void emit_imm(enum bytecode code, int word)
{
    unsigned char *p = (unsigned char *) &word;

#ifdef EXTMEMCODE
    copybytetoaux(codeptr++, code);
    copybytetoaux(codeptr++, *p++);
    copybytetoaux(codeptr++, *p);
#else
    *codeptr++ = code;
    *codeptr++ = *p++;
    *codeptr++ = *p;
#endif
    rtPC += 3;
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Compiler: Emit PRMSG and string argument.
 * String is in readbuf
 * String is zero-terminated.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void emitprmsg(void)
{
    char *p = readbuf;
#ifdef EXTMEMCODE
    copybytetoaux(codeptr++, VM_PRMSG);
    ++rtPC;
    while (*p) {
        copybytetoaux(codeptr++, *p++);
        ++rtPC;
    }
    copybytetoaux(codeptr++, 0);
    ++rtPC;
#else
    emit(VM_PRMSG);
    ++rtPC;
    while (*p) {
        *codeptr++ = *p++;
        ++rtPC;
    }
    *codeptr++ = 0;
    /* TODO: For some reason I don't need ++rtPC here */
#endif
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Emit fixup for address.
 * The compiler uses this to go back and fill in the address for forward
 * jumps, once it discovers where the destination is.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void emit_fixup(int address, int word)
{
#ifdef EXTMEMCODE
    unsigned char *ptr = (unsigned char *) (codestart + address - RTPCSTART);
    unsigned char *p = (unsigned char *) &word;
    copybytetoaux(ptr++, *p++);
    copybytetoaux(ptr, *p);
#else
    unsigned char *ptr = (unsigned char *) (CODESTART + address - RTPCSTART);
    unsigned char *p = (unsigned char *) &word;
    *ptr++ = *p++;
    *ptr = *p;
#endif

/*
    printhex(address);
    print(": ");
    printhex(word);
    print("                 ; Fixup\n");
*/
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Write code to file.
 * Call this after compilation is done.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void writebytecode()
{
    unsigned char *end = codeptr;
    unsigned char *p;
    unsigned char *q;
#ifdef EXTMEMCODE
    p = (unsigned char *) codestart;
#else
    p = (unsigned char *) CODESTART;
#endif
    strcpy(readbuf, filename);
    printchar('\n');
    openfile(1);
    print("...\n");
    while (p < end) {
#ifdef EXTMEMCODE
        copybytefromaux(p);
        q = embuf;
#else
        q = p;
#endif
#ifdef CBM
        cbm_write(1, q, 1);
#else
        fwrite(q, 1, 1, fd);
#endif
        ++p;
    }
#ifdef CBM
    cbm_close(1);
#else
    fclose(fd);
#endif
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Values:
 *  0 not editing program
 *  1 editing program
 *  2 editing program - insert first line
 */
unsigned char editmode = 0;

/*
 * Pointer to first line of code.
 */
struct lineofcode *program = NULL;

/*
 * skipFlag is set to one when we enter a body of code which we are not
 * executing (for example because a while loop condition was false.)  When
 * skipFlag is one, the parser will only process certain loop control tokens
 * - all others are ignored.
 */
unsigned char skipFlag;

/*
 * Append a line to the program
 * The new line will be appended after current
 * and current will be moved forward to point to the newly added line.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void appendline(char *line)
{
    struct lineofcode *loc = alloc2bttm(sizeof(struct lineofcode));

#ifdef EXTMEM
    loc->line = allocauxmem(sizeof(char) * strlen(line) + 1);
    copytoaux(loc->line, line);
    loc->len = strlen(line);
#else
    loc->line = alloc2bttm(sizeof(char) * (strlen(line) + 1));
    strcpy(loc->line, line);
#endif
    loc->next = current->next;
    current->next = loc;
    current = loc;
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Insert new first line (special case)
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void insertfirstline(char *line)
{
    struct lineofcode *loc = alloc2bttm(sizeof(struct lineofcode));

#ifdef EXTMEM
    loc->line = allocauxmem(sizeof(char) * strlen(line) + 1);
    copytoaux(loc->line, line);
    loc->len = strlen(line);
#else
    loc->line = alloc2bttm(sizeof(char) * (strlen(line) + 1));
    strcpy(loc->line, line);
#endif
    loc->next = program;
    program = loc;
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Make current point to the line with number linenum
 * (or NULL if not found).
 * Line numbers start from one in this routine.
 */
void findline(int linenum)
{
    counter = 1;
    current = program;
    while (current) {
        if (counter == linenum) {
            return;
        }
        current = current->next;
        ++counter;
    }
}

/*
 * Delete line(s)
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void deleteline(int startline, int endline)
{
    int linesToDel = endline - startline + 1;
    struct lineofcode *prev = NULL;

    counter = 1;
    if (endline < startline) {
        return;
    }
    current = program;
    while (current && linesToDel) {
        if (counter == startline) {
            if (prev) {
                prev->next = current->next;
            } else {
                program = current->next;
            }
#ifdef __GNUC__
            free(current->line);
            free(current);
#endif
            current = current->next;    /* ILLEGAL BUT WORKS FOR NOW */
            --linesToDel;
            continue;
        }
        prev = current;
        current = current->next;
        ++counter;
    }
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Replace line pointed to by current
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void changeline(char *line)
{
#ifdef __GNUC__
    free(current->line);
#endif

#ifdef EXTMEM
    current->line = allocauxmem(sizeof(char) * (strlen(line) + 1));
    copytoaux(current->line, line);
#else
    current->line = alloc2bttm(sizeof(char) * (strlen(line) + 1));
    strcpy(current->line, line);
#endif
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Delete program, free memory.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void new()
{
#ifdef __GNUC__
    struct lineofcode *l = program;
    struct lineofcode *l2;

    while (l) {
        l2 = l->next;
        free(l->line);
        free(l);
        l = l2;
    }
#else
    /* No need to iterate and free them all, just dump the heap */
    CLEARHEAP2TOP();
    CLEARHEAP2BTTM();
#ifdef EXTMEM
    CLEARAUXMEM();
#endif
#endif
    program = NULL;
    current = NULL;
}
#ifdef A2E
#pragma code-name (pop)
#endif

/* 0 is the top level, 1 is first level sub call etc. */
int calllevel;

/*
 * Entry in the variable table
 * name: first VARNUMCHARS characters as key
 * type: encodes the type in the least significant 4 bits (bits 3:0) encode
 *       the data type (TYPE_WORD or TYPE_BYTE).  The next least significant
 *       bit (bit 4) is 0 for a scalar value and 1 for an array.  Bit 5 is
 *       0 for a normal variable and 1 for a constant.
 * next: pointer to next vartabent.
 */
struct vartabent {
    char name[VARNUMCHARS];
    unsigned char type;         /* See above */
    struct vartabent *next;
};

typedef struct vartabent var_t;

var_t *varsbegin;               /* First table entry */
var_t *varsend;                 /* Last table entry  */
var_t *varslocal;               /* Local stack frame */

/*
 * Entry in the subroutine table.  This is used by the compiler only.
 * name: first SUBRNUMCHARS characters as key
 * addr: address of entry point in compiled code.
 */
struct subtabent {
    char name[SUBRNUMCHARS];
    unsigned int addr;
    struct subtabent *next;
};

typedef struct subtabent sub_t;

sub_t *subsbegin;               /* Entry points of compiled subroutines - first */
sub_t *subsend;                 /* Entry points of compiled subroutines - last  */
sub_t *callsbegin;              /* Subroutine calls - first */
sub_t *callsend;                /* Subroutine calls - end */

#define getptrtoscalarword(v) (int*)((char*)v + sizeof(var_t))
#define getptrtoscalarbyte(v) (unsigned char*)((char*)v + sizeof(var_t))

/*
 * Find integer variable
 * local - pointer to unsigned char.  If this contains 1 on entry then
 * only local variables will be searched.  The value returned in this
 * field can be used to determine if variable found was local (1) or
 * global (0).
 */
var_t *findintvar(char *name, unsigned char *local)
{
    var_t *ptr;

    /* Search locals */
    ptr = varslocal;
    while (ptr) {
        if (!strncmp(name, ptr->name, VARNUMCHARS)) {
            *local = 1;
            return ptr;
        }
        ptr = ptr->next;
    }

    if (*local == 1) {
        return NULL;
    }

    /* Search globals */
    ptr = varsbegin;
    while (ptr && (ptr->name[0] != '-')) {
        if (!strncmp(name, ptr->name, VARNUMCHARS)) {
            *local = 0;
            return ptr;
        }
        ptr = ptr->next;
    }
    return NULL;                /* Not found */
}

/*
 * Clear all variables
 */
void clearvars()
{
/* No need to iterate and free them all, just dump the heap */
    CLEARHEAP1();
    varsbegin = NULL;
    varsend = NULL;
    varslocal = NULL;
}

enum types {
    TYPE_CONST,                 /* Stored as TYPE_WORD     */
    TYPE_WORD,                  /* Word variable - 16 bits */
    TYPE_BYTE                   /* Byte variable - 8 bits  */
};

/*
 * Print all variables as a table
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void printvars()
{
    var_t *v = varsbegin;

    while (v) {
        printchar(v->name[0] ? v->name[0] : ' ');
        printchar(v->name[1] ? v->name[1] : ' ');
        printchar(v->name[2] ? v->name[2] : ' ');
        printchar(v->name[3] ? v->name[3] : ' ');
        if (v->type & 0x10) {
            printchar('[');
            printdec(*(int *)
                     ((unsigned char *) v + sizeof(var_t) + sizeof(int)));
            printchar(']');
        }
        printchar(' ');
        printchar(((v->type & 0x0f) == TYPE_WORD) ? 'w' : 'b');
        printchar((v->type & 0x20) ? 'c' : ' ');
        printchar(' ');
        if ((v->type & 0x10) == 0) {
            if (v->type == TYPE_WORD) {
                printdec(*getptrtoscalarword(v));
            } else {
                printdec(*getptrtoscalarbyte(v));
            }
        }
        printchar('\n');
        v = v->next;
    }
}
#ifdef A2E
#pragma code-name (pop)
#endif

/* Factored out to save a few bytes
 * Used by createintvar() only.
 */
void civ_st_rel_word(unsigned int i)
{
    emitldi(rtSP - rtFP + 2 * i);
    emit(VM_STRWORD);
}

/* Factored out to save a few bytes
 * Used by createintvar() only.
 */
void civ_st_rel_byte(unsigned int i)
{
    emitldi(rtSP - rtFP + i);
    emit(VM_STRBYTE);
}

#define STRG_INIT 0
#define LIST_INIT 1
/*
 * Create new integer variable (either word or byte, scalar or array)
 *
 * name is the variable name
 * type specifies if it is a word (TYPE_WORD) variable, a byte variable
 * (TYPE_BYTE), or a constant (TYPE_CONST).
 * isarray is 0 for scalar variable, 1 for array variable
 * sz is the size (for an array only)
 * value is the initializer (for a scalar only)  TODO: Can save a word of arguments here!!!!!
 * bodyptr is used when allocating arrays.  If bodyptr is null then the
 * function will allocate space for the array data block, following the
 * array header.  If, on the other hand, a non-null pointer is passed then
 * only the array header will be allocated and the pointer will be stored
 * as the pointer to the array data block.  This allows array pass-by-reference
 * semantics.
 * 
 * Return 0 on success, 1 if error.
 *
 * New variable is appended to table, which adds it to the innermost scope.
 */
unsigned char createintvar(char *name,
                           enum types type,
                           unsigned char isarray,
                           int sz, int value, int bodyptr)
{
    int i;
    int val;
    var_t *v;
    unsigned char arrinitmode;  /* STRG_INIT means string initializer, LIST_INIT means list initializer */
    unsigned char local = 1;
    unsigned char isconst = 0;

    v = findintvar(name, &local);       /* local = 1, so only search local scope */

    if (v) {
        error(ERR_REDEF);
        return 1;
    }

    if (sz < 1) {
        error(ERR_DIM);
        return 1;
    }

    if (type == TYPE_CONST) {
        isconst = 1;
        type = TYPE_WORD;
    }

    if (!isarray) {
        /*
         * Scalar variables
         */
        if (compile) {
            /*
             * When compiling we store the address of the variable in
             * the target VM where the value normally goes.
             *
             * For local variables this is RELATIVE to the frame pointer but
             * for globals it is an ABSOLUTE address.
             */
            v = alloc1(sizeof(var_t) + sizeof(int));
            if (isconst) {
                /* Store value of const.  No code generation. */
                *getptrtoscalarword(v) = value;
            } else if (type == TYPE_WORD) {
                /* Relative if compiling sub, absolute otherwise */
                *getptrtoscalarword(v) = (compilingsub ? (rt_push_callstack(2) - rtFP) : (rt_push_callstack(2) + 1));
                emit(VM_PSHWORD);
            } else {
                /* Relative if compiling sub, absolute otherwise */
                *getptrtoscalarword(v) = (compilingsub ? (rt_push_callstack(1) - rtFP) : (rt_push_callstack(1) + 1));
                emit(VM_PSHBYTE);
            }
        } else {
            if (type == TYPE_WORD) {
                v = alloc1(sizeof(var_t) + sizeof(int));
                *getptrtoscalarword(v) = value;
            } else {
                v = alloc1(sizeof(var_t) + sizeof(unsigned char));
                *getptrtoscalarbyte(v) = value;
            }
        }
    } else {
        /*
         * Array variables.
         *
         * Here we allocate two words of space as follows:
         *  WORD1: Pointer to payload
         *  WORD2: to record the single dimensions of the 1D array.
         * The payload follows these two words.  This scheme is
         * designed to be extensible to more dimensions.
         */
        if (bodyptr) {

            /*
             * Should work for both interpreter and compiler
             * (although only used in interpreter right now)
             */
            v = alloc1(sizeof(var_t) + 2 * sizeof(int));

        } else {
            /*
             * For arrays we parse the initializer here.
             */
            if (isarray) {
                if (*txtPtr == '"') {
                    arrinitmode = STRG_INIT;
                    ++txtPtr;
#ifdef CBM
                } else if (*txtPtr == '[') {
#else
                } else if (*txtPtr == '{') {
#endif
                    arrinitmode = LIST_INIT;
                    ++txtPtr;
                }
            }

            if (compile) {

                v = alloc1(sizeof(var_t) + 2 * sizeof(int));
                if (type == TYPE_WORD) {
                    /* Relative if compiling sub, absolute otherwise */
                    bodyptr = (compilingsub ? (rt_push_callstack(sz * 2) - rtFP) : (rt_push_callstack(sz * 2) + 1));
                } else {
                    /* Relative if compiling sub, absolute otherwise */
                    bodyptr = (compilingsub ? (rt_push_callstack(sz) - rtFP) : (rt_push_callstack(sz) + 1));
                }

                /*
                 * The following generates code to allocate the array
                 * TODO: This is not very efficient. Need a VM instruction to allocate a block.
                 */
                emitldi(sz);
                emit(VM_DEC);
                emit(VM_DUP);
                emitldi(0);     /* Value to fill with */
                emit((type == TYPE_WORD) ? VM_PSHWORD : VM_PSHBYTE);
                emitldi(0);
                emit(VM_NEQL);
                emit_imm(VM_BRNCHIMM, rtPC - 10);
                emit(VM_DROP);

                /*
                 * Initialize array
                 * arrinitmode STRG_INIT is for string initializer "like this"
                 * arrinitmode LIST_INIT is for list initializer {123, 456, 789 ...}
                 */
                if (arrinitmode == STRG_INIT) {
                    --sz;       /* Hack to leave space for final null */
                }
                for (i = 0; i < sz; ++i) {
                    if (arrinitmode == STRG_INIT) {
                        emitldi((*txtPtr == '"') ? 0 : *txtPtr);
                        ((type == TYPE_WORD) ? civ_st_rel_word(i) : civ_st_rel_byte(i));
                        if (*txtPtr == '"') {
                            break;
                        }
                        ++txtPtr;
                    } else {
#ifdef CBM
                        if (*txtPtr == ']')
#else
                        if (*txtPtr == '}')
#endif
                        {
                            break;
                        }
                        if (eval(0, &val)) {
                            return 1;
                        }
                        ((type == TYPE_WORD) ? civ_st_rel_word(i) : civ_st_rel_byte(i));
                        eatspace();
                        if (*txtPtr == ',') {
                            ++txtPtr;
                        }
                        eatspace();
                    }
                }
            } else {
                if (type == TYPE_WORD) {
                    v = alloc1(sizeof(var_t) + (sz + 2) * sizeof(int));
                } else {
                    v = alloc1(sizeof(var_t) + 2 * sizeof(int) + sz * sizeof(unsigned char));
                }
                bodyptr = (int) ((unsigned char *) v + sizeof(var_t) + 2 * sizeof(int));

                /*
                 * Initialize array
                 * arrinitmode STRG_INIT is for string initializer "like this"
                 * arrinitmode LIST_INIT is for list initializer {123, 456, 789 ...}
                 */
                if (arrinitmode == STRG_INIT) {
                    --sz;       /* Hack to leave space for final null */
                }
                for (i = 0; i < sz; ++i) {
                    if (arrinitmode == STRG_INIT) {
                        if (*txtPtr == '"') {
                            val = 0;
                        } else {
                            val = *txtPtr;
                            ++txtPtr;
                        }
                    } else {
#ifdef CBM
                        if (*txtPtr == ']')
#else
                        if (*txtPtr == '}')
#endif
                        {
                            val = 0;
                        } else {
                            if (eval(0, &val)) {
                                return 1;
                            }
                            eatspace();
                            if (*txtPtr == ',') {
                                ++txtPtr;
                            }
                            eatspace();
                        }
                    }
                    if (type == TYPE_WORD) {
                        *((int *) bodyptr + i) = val;
                    } else {
                        *((unsigned char *) bodyptr + i) = val;
                    }
                }
            }
            if (arrinitmode == STRG_INIT) {
                ++sz;           /* Reverse the hack we perpetuated above */
                if (*txtPtr == '"') {
                    ++txtPtr;
                } else {
                    error(ERR_TOOLONG);
                    return 1;
                }
            } else {
#ifdef CBM
                if (*txtPtr == ']')
#else
                if (*txtPtr == '}')
#endif
                {
                    ++txtPtr;
                } else {
                    error(ERR_TOOLONG);
                    return 1;
                }
            }
        }

        /* Store pointer to payload */
        *(int *) ((unsigned char *) v + sizeof(var_t)) = (int) bodyptr;

        /* Store size */
        *(int *) ((unsigned char *) v + sizeof(var_t) + sizeof(int)) = sz;
    }

    strncpy(v->name, name, VARNUMCHARS);
    v->type = (isconst << 5) | (isarray << 4) | type;
    v->next = NULL;

    if (varsend) {
        varsend->next = v;
    }
    varsend = v;
    if (!varsbegin) {
        varsbegin = v;
        varslocal = v;
    }
    return 0;
}

/*
 * Mark variable table when we enter a subroutine.
 * We use a fake variable entry for this, with illegal name '----'
 * Records pointer to the fake entry in varslocal.
 */
void vars_markcallframe()
{
    ++calllevel;
    varslocal = alloc1(sizeof(var_t) + sizeof(int));
    strncpy(varslocal->name, "----", VARNUMCHARS);
    varslocal->type = TYPE_WORD;
    varslocal->next = NULL;
    *(getptrtoscalarword(varslocal)) = (int) varsend;   /* Store pointer to previous in value */
    if (varsend) {
        varsend->next = varslocal;
    }
    varsend = varslocal;
    if (!varsbegin) {
        varsbegin = varslocal;
    }
}

/*
 * Release local variables on return from a subroutine.
 */
void vars_deletecallframe()
{
    var_t *newend = (void *) *(getptrtoscalarword(varslocal));  /* Recover pointer */
    var_t *v = varslocal;

    /* Free the local variables */
    if (!newend) {
        CLEARHEAP1();
    } else {
        free1((int) newend - (int) varsend);
    }

    if (newend) {
        newend->next = NULL;
    } else {
        varsbegin = NULL;
    }
    varsend = newend;
    --calllevel;

    /* Set varslocal to previous stack frame or NULL if none */
    varslocal = NULL;
    v = varsbegin;
    while (v) {
        if (v->name[0] == '-') {
            varslocal = v;
        }
        v = v->next;
    }
}

/* Factored out to save a few bytes
 * Used by setintvar() only.
 */
void siv_st_abs(unsigned char type)
{
    ((type == TYPE_WORD) ? emit(VM_STAWORD) : emit(VM_STABYTE));
}

/* Factored out to save a few bytes
 * Used by setintvar() only.
 */
void siv_st_rel(unsigned char type)
{
    ((type == TYPE_WORD) ? emit(VM_STRWORD) : emit(VM_STRBYTE));
}

/* Factored out to save a few bytes
 * Used by setintvar() only.
 */
void siv_st_abs_imm(unsigned int addr, unsigned char type)
{
    emit_imm(((type & 0x0f) == TYPE_WORD) ? VM_STAWORDIMM : VM_STABYTEIMM, addr);
}

/* Factored out to save a few bytes
 * Used by setintvar() only.
 */
void siv_st_rel_imm(unsigned int addr, unsigned char type)
{
    emit_imm(((type & 0x0f) == TYPE_WORD) ? VM_STRWORDIMM : VM_STRBYTEIMM, addr);
}

/*
 * Set existing integer variable
 * name is the variable name
 * idx is the index into an array. -1 means subscript not given.
 * value is the value to set
 * Return 0 if successful, 1 on error
 *
 * Sets matching local variable.  If no local exists then return the
 * matching global.  Otherwise error.
 */
unsigned char setintvar(char *name, int idx, int value)
{
    unsigned char isarray;
    unsigned char type;
    void *bodyptr;
    unsigned char local = 0;

    var_t *ptr = findintvar(name, &local);

    if (!ptr) {
        error(ERR_VAR);
        return 1;
    }
    isarray = (ptr->type & 0x10) >> 4;
    type = ptr->type & 0x0f;

    /* Error if try to set const */
    if (ptr->type & 0x20) {
        error(ERR_STCONST);
        return 1;
    }

    if (!isarray) {
        /*
         * Scalars
         */
        if (idx != -1) {
            /* Means [..] subscript was provided */
            error(ERR_SUBSCR);
            return 1;
        }
        if (compile) {
            /*
             * When we are at the top level scope (global scope), all
             * variables are globals and we use ABSOLUTE addressing.
             * When we are at function scope, globals still use
             * ABSOLUTE addressing, but locals are addressed RELATIVE
             * to the frame pointer.
             */
            if (local && compilingsub) {
                siv_st_rel_imm(*getptrtoscalarword(ptr), type);
            } else {
                siv_st_abs_imm(*getptrtoscalarword(ptr), type);
            }
        } else {
            if (type == TYPE_WORD) {
                *getptrtoscalarword(ptr) = value;
            } else {
                *getptrtoscalarbyte(ptr) = value;
            }
        }
    } else {
        /*
         * Arrays
         */
        if (idx == -1) {
            /* Means [..] subscript was never provided */
            error(ERR_SUBSCR);
            return 1;
        }
        bodyptr = (void *) *(int *) ((unsigned char *) ptr + sizeof(var_t));

        if (compile) {
            /* *** Index is on the stack (X) */
            emit(VM_SWAP);
            if (type == TYPE_WORD) {
                emitldi(1);
                emit(VM_LSH);
            }
            emitldi((int) ((int *) bodyptr));
            /*
             * If the array size field is -1, this means the bodyptr is a
             * pointer to a pointer to the body (rather than pointer to
             * the body), so it needs to be dereferenced one more time.
             */
            if (*(int *) ((unsigned char *) ptr + sizeof(var_t) + sizeof(int)) == -1) {
                emit(VM_LDRWORD);
            }
            emit(VM_ADD);
            if (local && compilingsub) {
                if (*(int *) ((unsigned char *) ptr + sizeof(var_t) + sizeof(int)) == -1) {
                    siv_st_abs(type);
                } else {
                    siv_st_rel(type);
                }
            } else {
                siv_st_abs(type);
            }
        } else {
            if ((idx < 0) || (idx >= *(int *) ((unsigned char *) ptr + sizeof(var_t) + sizeof(int)))) {
                error(ERR_SUBSCR);
                return 1;
            }
            if (type == TYPE_WORD) {
                *((int *) bodyptr + idx) = value;
            } else {
                *((unsigned char *) bodyptr + idx) = value;
            }
        }
    }

    return 0;
}

/* Factored out to save a few bytes
 * Used by getintvar() only.
 */
void giv_ld_abs(unsigned char type)
{
    (((type & 0x0f) == TYPE_WORD) ? emit(VM_LDAWORD) : emit(VM_LDABYTE));
}

/* Factored out to save a few bytes
 * Used by getintvar() only.
 */
void giv_ld_rel(unsigned char type)
{
    (((type & 0x0f) == TYPE_WORD) ? emit(VM_LDRWORD) : emit(VM_LDRBYTE));
}

/* Factored out to save a few bytes
 * Used by getintvar() only.
 */
void giv_ld_abs_imm(unsigned int addr, unsigned char type)
{
    emit_imm(((type & 0x0f) == TYPE_WORD) ? VM_LDAWORDIMM : VM_LDABYTEIMM, addr);
}

/* Factored out to save a few bytes
 * Used by getintvar() only.
 */
void giv_ld_rel_imm(unsigned int addr, unsigned char type)
{
    emit_imm(((type & 0x0f) == TYPE_WORD) ? VM_LDRWORDIMM : VM_LDRBYTEIMM, addr);
}

/*
 * Get existing integer variable
 * name is the variable name
 * idx is the index into an array. -1 means subscript not given.
 * Returns the value (or the address) in val.
 * Return the type TYPE_BYTE or TYPE_WORD in type.
 * address if set to 1 then address is returned, not value
 * Return 0 if successful, 1 on error
 *
 * Returns matching local variable.  If no local exists then return the
 * matching global.  Otherwise error.
 */
unsigned char getintvar(char *name,
                        int idx,
                        int *val,
                        unsigned char *type, unsigned char address)
{
    unsigned char isarray;
    void *bodyptr;
    unsigned char local = 0;

    var_t *ptr = findintvar(name, &local);

    if (!ptr) {
        error(ERR_VAR);
        return 1;
    }
    isarray = (ptr->type & 0x10) >> 4;
    *type = ptr->type;

    if (compiletimelookup) {
        /*
         * Special hack to allow lookup (rather than code
         * generation) during compilation.
         */
        *val = *getptrtoscalarword(ptr);
        compiletimelookup = 0;
        return 0;
    }

    if (!isarray) {
        /*
         * Scalars
         */
        if (idx != -1) {
            /* Means [..] subscript was provided */
            error(ERR_SUBSCR);
            return 1;
        }
        if (compile) {
            /*
             * When we are at the top level scope (global scope), all
             * variables are globals and we use ABSOLUTE addressing.
             * When we are at function scope, globals still use
             * ABSOLUTE addressing, but locals are addressed RELATIVE
             * to the frame pointer.
             */
            if (address) {
                emitldi(*getptrtoscalarword(ptr));
                if (local && compilingsub) {
                    emit(VM_RTOA);
                }
            } else {
                if (local && compilingsub) {
                    giv_ld_rel_imm(*getptrtoscalarword(ptr), *type);
                } else {
                    giv_ld_abs_imm(*getptrtoscalarword(ptr), *type);
                }
            }
        } else {
            if ((*type & 0x0f) == TYPE_WORD) {
                if (address) {
                    *val = (int) getptrtoscalarword(ptr);
                } else {
                    *val = *getptrtoscalarword(ptr);
                }
            } else {
                if (address) {
                    *val = (int) getptrtoscalarbyte(ptr);
                } else {
                    *val = *getptrtoscalarbyte(ptr);
                }
            }
        }
    } else {
        /*
         * Arrays
         * Note the special cases, for an array A:
         * 1) &A is the same as &A[0]
         * 2) A is the same as &A[0]
         * This second case is needed to make the eval() work propertly
         * for array pass-by-reference.
         */
        if (idx == -1) {
            /* Means [..] subscript was never provided */
            address = 1;
            idx = 0;
            if (compile) {
                emitldi(0);
            }
        }
        bodyptr =
            (void *) *(int *) ((unsigned char *) ptr + sizeof(var_t));

        if (compile) {
            /* *** Index is on the stack (X) *** */
            if ((*type & 0x0f) == TYPE_WORD) {
                emitldi(1);
                emit(VM_LSH);
            }
            emitldi((int) ((int *) bodyptr));
            /*
             * If the array size field is -1, this means the bodyptr is a
             * pointer to a pointer to the body (rather than pointer to
             * the body), so it needs to be dereferenced one more time.
             */
            if (*(int *) ((unsigned char *) ptr + sizeof(var_t) + sizeof(int)) == -1) {
                emit(VM_LDRWORD);
            }
            emit(VM_ADD);
            if (!address) {
                if (local && compilingsub) {
                    if (*(int *) ((unsigned char *) ptr + sizeof(var_t) + sizeof(int)) == -1) {
                        giv_ld_abs(*type);
                    } else {
                        giv_ld_rel(*type);
                    }
                } else {
                    giv_ld_abs(*type);
                }
            } else {
                if (local && compilingsub) {
                    if (*(int *) ((unsigned char *) ptr + sizeof(var_t) + sizeof(int)) != -1) {
                        /* Convert to absolute address */
                        emit(VM_RTOA);
                    }
                }
            }
        } else {
            if ((idx < 0) || (idx >= *(int *) ((unsigned char *) ptr + sizeof(var_t) + sizeof(int)))) {
                error(ERR_SUBSCR);
                return 1;
            }

            if ((*type & 0x0f) == TYPE_WORD) {
                if (address) {
                    *val = (int) ((int *) bodyptr + idx);
                } else {
                    *val = *((int *) bodyptr + idx);
                }
            } else {
                if (address) {
                    *val = (int) ((unsigned char *) bodyptr + idx);
                } else {
                    *val = *((unsigned char *) bodyptr + idx);
                }
            }
        }
    }

    return 0;
}


/*
 * Handy defines for return codes
 */
#define RET_SUCCESS      0      /* Successful */
#define RET_ERROR        1      /* Error */

/*************************************************************************/
/* IF / THEN / ELSE                                                      */
/*************************************************************************/

/*
 * Handles if statement.
 */
void doif(unsigned char arg)
{

    /*
     * Place the following on the return stack when interpreting:
     *   - Magic value IFFRAME to indicate IF loop stack frame
     *   - Status value as follows:
     *     0: skipFlag was already set so not evaluating my argument
     *     1: skipFlag was clear and I set it (condition false)
     *     2: skipFlag was clear and I left it clear (condition true)
     *   - Dummy value 0
     *
     * When compiling:
     *   - Magic value IFFRAME
     *   - Address of the branch destination operand for when condition
     *     is false.  This will be filled in later and may point to the
     *     ELSE block (if present) or end of the IF block otherwise.
     *   - Space to store the branch destination operand which will be
     *     used to skip over the ELSE block if the IF block runs.  This
     *     will also be filled in later.
     */
    push_return(IFFRAME);

    if (compile) {
        /* **** Value of IF expression is on the eval stack **** */
        emit(VM_NOT);
        push_return(rtPC + 1);
        emit_imm(VM_BRNCHIMM, 0xffff);        /* To be filled in later */
        push_return(0);
    } else {
        if (skipFlag) {
            push_return(0);
        } else {
            if (!arg) {
                skipFlag = 1;
                push_return(1);
            } else {
                push_return(2);
            }
        }
        push_return(0);         /* Dummy */
    }
}

/*
 * Handles else statement.
 * Returns RET_SUCCESS if no error, RET_ERROR if error
 */
unsigned char doelse()
{
    if (return_stack[returnSP + 3] != IFFRAME) {
        error(ERR_NOIF);
        return RET_ERROR;
    }

    if (compile) {
        /*
         * Code to jump over ELSE block when IF condition is true
         */
        return_stack[returnSP + 1] = rtPC + 1;
        emit_imm(VM_JMPIMM, 0xffff); /* To be filled in later */

        /*
         * Fixup the dummy destination address initialized by
         * doif() to point to ELSE
         */
        emit_fixup(return_stack[returnSP + 2], rtPC);
        return_stack[returnSP + 2] = 0;
    } else {
        /*
         * If the matching IF statement had condition true then the
         * value at return_stack[returnSP + 1] will be 2.  If the
         * matching IF statement had condition false then the value
         * will be 1.
         */
        if (return_stack[returnSP + 2] == 2) {
            skipFlag = 1;
        } else if (return_stack[returnSP + 2] == 1) {
            skipFlag = 0;
        }
    }
    return RET_SUCCESS;
}

/*
 * Handles endif statement.
 * Returns RET_SUCCESS if no error, RET_ERROR if error
 */
unsigned char doendif()
{
    if (return_stack[returnSP + 3] != IFFRAME) {
        error(ERR_NOIF);
        return RET_ERROR;
    }

    if (compile) {
        /*
         * Fixup the dummy destination address initialized by
         * doif() to point to ENDIF. (Only do this if it hasn't
         * already been updated to point to the ELSE.)
         */
        if (return_stack[returnSP + 2]) {
            emit_fixup(return_stack[returnSP + 2], rtPC);
        }
        /*
         * Fixup the dummy destination address initialized by
         * doelse() to point to ENDIF. (Only do this if there was
         * actually an ELSE.)
         */
        if (return_stack[returnSP + 1]) {
            emit_fixup(return_stack[returnSP + 1], rtPC);
        }
    } else {
        /*
         * If skipFlag was false when we hit the matching IF
         * statement, then the value at return_stack[returnSP + 2]
         * will be 1 or 2. In this case, clear skipFlag.
         */
        if (return_stack[returnSP + 2]) {
            skipFlag = 0;
        }
    }

    pop_return();
    pop_return();
    pop_return();
    return RET_SUCCESS;
}

/*************************************************************************/
/* FOR LOOPS                                                             */
/*************************************************************************/

/*
 * Routine handles five cases, each of which looks like variable assignment.
 * Doing these all together here makes code easier to maintain and smaller.
 */
#define WORD_MODE  0
#define BYTE_MODE  1
#define CONST_MODE 2
#define LET_MODE   3
#define FOR_MODE   4

/*
 * Handles four cases, according to value of mode:
 *  - WORD_MODE  - declaration of word variable 
 *  - BYTE_MODE  - declaration of byte variable 
 *  - CONST_MODE - declaration of constant
 *  - LET_MODE   - assignment to existing variable
 *  - FOR_MODE   - entry to for loop
 *
 * Handles parsing the following text (mode == WORD_MODE/BYTE_MODE) either:
 *     "var = expr"
 * or, "var[expr1] = expr2"
 * or (mode == CONST_MODE), just:
 *     "var = expr"
 * or (mode == FOR_MODE):
 *     "var = expr2 : expr3"
 * or, "var[expr1] = expr2 : expr3"
 *
 * Returns RET_SUCCESS if no error, RET_ERROR if error
 */
unsigned char assignorcreate(unsigned char mode)
{
    int j;
    int k;
    unsigned char type;
    char name[VARNUMCHARS];
    int i = 0;
    unsigned char isarray = 0;
    unsigned char local = 0;
    unsigned char oldcompile = compile;

    if (!txtPtr || !isalphach(*txtPtr)) {
        error(ERR_VAR);
        return RET_ERROR;
    }
    while (*txtPtr && (isalphach(*txtPtr) || isdigitch(*txtPtr))) {
        if (i < VARNUMCHARS) {
            name[i++] = *txtPtr;
        }
        ++txtPtr;
    }
    if (i < VARNUMCHARS) {
        name[i] = '\0';
    }

    i = 0;
    if (*txtPtr == '[') {
        isarray = 1;
        switch (mode) {
        case WORD_MODE:
        case BYTE_MODE:
            onlyconstants = 1;  /* Only parse constants - no variables  */
            compile = 0;        /* Use subscript() to eval, not codegen */
            if (subscript(&i) == 1) {
                onlyconstants = 0;
                compile = oldcompile;
                return RET_ERROR;
            }
            onlyconstants = 0;  /* Back to normal service */
            compile = oldcompile;
            break;
        default:
            if (subscript(&i) == 1) {
                return RET_ERROR;
            }
        }
    }

    eatspace();

    if (expect('=')) {
        return RET_ERROR;
    }

    eatspace();

    if (mode == CONST_MODE) {
        compile = 0;            /* Eval, not codegen */
    }

    /*
     * If it is LET or FOR, evaluate the single argument.
     * If it is declaration, only evaluate single argument for scalars.
     * For arrays, the initializer is evaluated inside createintvar().
     */
    if (!isarray || (mode == LET_MODE) || (mode == FOR_MODE)) {
        if (eval((mode != FOR_MODE), &j)) {
            compile = 1;
            return RET_ERROR;
        }
    }
    compile = oldcompile;

    switch (mode) {

    case WORD_MODE:
    case BYTE_MODE:
    case CONST_MODE:
        if (i == 0) {
            ++i;
        }
        if (createintvar(name,
                         ((mode == CONST_MODE) ? TYPE_CONST : ((mode == WORD_MODE) ? TYPE_WORD : TYPE_BYTE)),
                         isarray, i, j, 0)) {
            return RET_ERROR;
        }
        break;

    case LET_MODE:
    case FOR_MODE:
        if (!isarray) {
            i = -1;
        }
        if (setintvar(name, i, j)) {
            return RET_ERROR;
        }
        break;
    }

    if (mode != FOR_MODE) {
        return RET_SUCCESS;
    }

    /*
     * The remaining code is to handle entry to FOR
     * mode == FOR_MODE
     */

    if (expect(':')) {
        return RET_ERROR;
    }

    if (eval(1, &k)) {
        return RET_ERROR;
    }

    /*
     * Place the following on the return stack when interpreting:
     * - Magic value FORFRAME_B or FORFRAME_W - to indicate FOR loop stack
     *   frame for byte or word variable respectively.
     * - Line counter for the for statement (here) (int)
     * - txtPtr (int*)
     * - Loop limit (int)
     * - Pointer to loop control variable (int*)
     *
     * When compiling:
     *
     * - Magic value FORFRAME_B or FORFRAME_W.
     * - 0 if absolute addressing, 1 if relative addressing
     * - Runtime PC
     * - Pointer to loop control variable.
     * - Dummy word
     */

    /* Get the address of the variable */
    if (compile) {
        compiletimelookup = 1;
    }
    if (getintvar(name, i, &j, &type, 1)) {
        return RET_ERROR;
    }

    push_return(((type & 0x0f) == TYPE_WORD) ? FORFRAME_W : FORFRAME_B);

    if (compile) {
        /* Find out if it is a local or a global */
        findintvar(name, &local);
        push_return(local && compilingsub);     /* 0: absolute, 1: relative addr */

        /* Loop limit k should be on the runtime eval stack, move it to call stack */
        emit(VM_PSHWORD);

        push_return(rtPC);      /* Store PC so we know where to come back to */
        push_return(j);
        push_return(0);         /* Dummy */
    } else {
        push_return(counter);
        push_return((int) txtPtr);
        push_return(k);
        push_return(j);
    }

    return RET_SUCCESS;
}

/*
 * Go back to the start of a loop or return after end of subroutine.
 * (Used for FOR and WHILE loops and for subroutine CALL/RETURN).
 * - linenum is the line number of the start of the loop or the call
 *   statement, or -1 in immediate mode.
 * - oldTxtPtr is the stashed text pointer, which is expected to point
 *   to the code immediately after the opening loop statement.
 * This is used by the interpreter only.
 */
void backtotop(int linenum, char *oldTxtPtr)
{

    if (linenum == -1) {
        /* Return to immediate mode */
        counter = -1;
        current = NULL;
    } else {
        /*
         * If not immediate mode, then reload the line containing
         * the opening statement of the loop, or the call statement
         */
        findline(linenum + 1);
        --counter;              /* Findline uses 1-based linenums */
        if (!current) {
            /* Should never get here! */
            EXIT(99);
        }
    }

#ifdef EXTMEM
    copyfromaux(current->line, current->len);
#endif

    /* This should also work with extended memory */
    txtPtr = oldTxtPtr;
}

/*
 * Handle iterating or exiting the FOR loop.
 * Expects to find pointer to loop variable, loop limit
 * and line counter on the return stack.
 * Returns RET_SUCCESS on success, RET_ERROR on error
 */
unsigned char doendfor()
{
    int val;
    unsigned char type = 0xff;

    if (return_stack[returnSP + 5] == FORFRAME_W) {
        type = TYPE_WORD;
        if (!compile) {
            val = *(int *) (return_stack[returnSP + 1]);
        }
    } else if (return_stack[returnSP + 5] == FORFRAME_B) {
        type = TYPE_BYTE;
        if (!compile) {
            val = *(unsigned char *) (return_stack[returnSP + 1]);
        }
    }
    if (type == 0xff) {
        error(ERR_NOFOR);
        return RET_ERROR;
    }

    if (compile) {
        /* **** Loop limit is on the call stack **** */
        emit(VM_POPWORD);
        emit(VM_DUP);
        emit(VM_PSHWORD);

        //emitldi(return_stack[returnSP + 2]); /* Pointer to loop variable */
        if (return_stack[returnSP + 4]) {    /* Rel or abs */
            /* Pointer to loop var */
            emit_imm((type == TYPE_WORD) ? VM_LDRWORDIMM : VM_LDRBYTEIMM, return_stack[returnSP + 2]);
        } else {
            /* Pointer to loop var */
            emit_imm((type == TYPE_WORD) ? VM_LDAWORDIMM : VM_LDABYTEIMM, return_stack[returnSP + 2]);
        }

        /* Increment and store loop variable */
        emit(VM_INC);
        emit(VM_DUP);
        if (return_stack[returnSP + 4]) {
            emit_imm((type == TYPE_WORD) ? VM_STRWORDIMM : VM_STRBYTEIMM, return_stack[returnSP + 2]);
        } else {
            emit_imm((type == TYPE_WORD) ? VM_STAWORDIMM : VM_STABYTEIMM, return_stack[returnSP + 2]);
        }

        /* Compare with loop limit already on eval stack */
        emit(VM_GTE);
        emit_imm(VM_BRNCHIMM, return_stack[returnSP + 3]); /* Branch destination */

        /* Drop loop limit from call stack */
        emit(VM_POPWORD);
        emit(VM_DROP);
        goto unwind;
    }

    /*
     * Compare loop control variable and limit
     */
    if (val < return_stack[returnSP + 2]) {

        /*
         * If loop not done, increment loop control var, jump back
         * to line after FOR
         */
        if (type == TYPE_WORD) {
            ++(*(int *) (return_stack[returnSP + 1]));
        } else {
            ++(*(unsigned char *) (return_stack[returnSP + 1]));
        }

        backtotop(return_stack[returnSP + 4], (char *) return_stack[returnSP + 3]);

        return RET_SUCCESS;
    }

  unwind:
    /* Done looping, unwind stack */
    pop_return();
    pop_return();
    pop_return();
    pop_return();
    pop_return();
    return RET_SUCCESS;
}

/*************************************************************************/
/* WHILE LOOPS                                                           */
/*************************************************************************/

/*
 * Handles entry into a while loop.
 * startTxtPtr should point to the text of the WHILE statement itself.
 * arg is the evaluated value of the argument to the WHILE.
 */
void dowhile(char *startTxtPtr, unsigned char arg)
{

    /*
     * Place the following on the return stack when interpreting:
     *   - Magic value WHILEFRAME to indicate WHILE loop stack frame
     *   - Status value as follows:
     *     0: skipFlag was already set so not evaluating my argument
     *     1: skipFlag was clear and I set it (condition false)
     *     2: skipFlag was clear and I left it clear (condition true)
     *   - Line number for the WHILE line (here)
     *   - txtPtr (int*)
     *
     * When compiling:
     *   - Magic value WHILEFRAME
     *   - Runtime PC prior to evaluating WHILE expression
     *   - Runtime PC for patching up the branch
     *   - Dummy value
     *
     */
    push_return(WHILEFRAME);

    if (compile) {
        push_return(rtPCBeforeEval);
        /* **** Value of WHILE expression is on the eval stack **** */
        emit(VM_NOT);
        push_return(rtPC + 1);  /* Address of dummy 0xffff */
        emit_imm(VM_BRNCHIMM, 0xffff);
        push_return(0);         /* Dummy */
    } else {
        if (skipFlag) {
            push_return(0);
        } else {
            if (!arg) {
                skipFlag = 1;
                push_return(1);
            } else {
                push_return(2);
            }
        }
        push_return(counter);
        push_return((int) startTxtPtr);
    }
}

/*
 * Handles endwhile statement.
 * Returns RET_SUCCESS on success, RET_ERROR on error
 */
unsigned char doendwhile()
{

    if (return_stack[returnSP + 4] != WHILEFRAME) {
        error(ERR_NOWHILE);
        return RET_ERROR;
    }

    if (compile) {
        /*
         * Jump back and re-evaluate the WHILE argument.
         */
        emit_imm(VM_JMPIMM, return_stack[returnSP + 3]);

        /*
         * Fixup the dummy destination address initialized by
         * dowhile() to point to the ENDWHILE.
         */
        emit_fixup(return_stack[returnSP + 2], rtPC);
    } else {
        switch (return_stack[returnSP + 3]) {

        case 0:

            /*
             * If skipFlag was true when we hit the
             * matching WHILE statement, the the value
             * at return_stack[returnSP+3] will be 0.
             */

            goto doret;

        case 1:

            /*
             * If skipFlag was false when we hit the
             * matching WHILE statement, then the value
             * at return_stack[returnSP + 3] will be 1
             * (condition false) or 2 (condition true).
             * If the WHILE was false, then just set
             * clear skipFlag, pop the stack and keep
             * going.  If the WHILE was true, pop the
             * stack and jump back to the WHILE test
             * again.
             */

            skipFlag = 0;
            goto doret;

        case 2:

            /*
             * skipFlag was true when we hit the
             * matching WHILE. Having executed the
             * loop body, now we loop back
             * to the WHILE statement.
             */
            backtotop(return_stack[returnSP + 2],
                      (char *) return_stack[returnSP + 1]);

            goto doret;

        default:
            /* Should never get here! */
            exit(99);
        }
    }

  doret:
    pop_return();
    pop_return();
    pop_return();
    pop_return();

    return RET_SUCCESS;
}

/*
 * Compare two strings up to terminator character.
 * This function takes two char pointers and compares them character
 * by character, up until a terminator character c (or space), which MUST
 * appear in s1.  Returns 0 if equal, 1 if unequal.
 */
unsigned char compareUntil(char *s1, char *s2, char term)
{
    while (*s1 == *s2) {
        if (*s1 == 0) {
            return 1;
        }
        ++s1;
        ++s2;
    }
    /* s2 is allowed to have extra trailing junk */
    if ((*s1 == term) || (*s1 == ' ')) {
        return 0;
    }
    return 1;
}

/*
 * Handle subroutine declaration.
 * This is really only used by the compiler.
 */
unsigned char dosubr()
{
    unsigned char type;
    char name[VARNUMCHARS];
    unsigned char j;
    unsigned char arraymode;
    var_t *v;
    sub_t *s;

    if (compile) {

        compilingsub = 1;

        print("\n[");
        print(readbuf);
        print("]");

        /*
         * Create entry in subroutine table
         * Allocate this on the top-down stack in arena 2.  This grows down towards the
         * source code, which is growing up from the bottom of arena 2.
         */
        s = alloc2top(sizeof(sub_t));
        strncpy(s->name, readbuf, SUBRNUMCHARS);
        s->addr = rtPC;
        s->next = NULL;

        if (subsend) {
            subsend->next = s;
        }
        subsend = s;
        if (!subsbegin) {
            subsbegin = s;
        }

        vars_markcallframe();

        /* Update frame pointer */
        emit(VM_SPTOFP);
        rtFP = rtSP;

        if (expect('(')) {
            return RET_ERROR;
        }

        for (;;) {
            eatspace();
            if (*txtPtr == ')') {
                break;
            }
            if (!strncmp(txtPtr, "word ", 5)) {
                type = TYPE_WORD;
            } else if (!strncmp(txtPtr, "byte ", 5)) {
                type = TYPE_BYTE;
            } else {
                error(ERR_ARG);
                return RET_ERROR;
            }
            txtPtr += 5;
            eatspace();
            for (j = 0; j < VARNUMCHARS; ++j) {
                name[j] = 0;
            }
            j = 0;
            while (txtPtr && (isalphach(*txtPtr) || isdigitch(*txtPtr))) {
                if (j < VARNUMCHARS) {
                    name[j] = *txtPtr;
                }
                ++j;
                ++txtPtr;
            }
            /*
             * If argument is followed by '[]'
             * then switch to pass array by ref
             * mode.
             */
            arraymode = 0;
            if (*txtPtr == '[') {
                ++txtPtr;
                if (*txtPtr == ']') {
                    ++txtPtr;
                    arraymode = 1;
                } else {
                    error(ERR_ARG);
                    return RET_ERROR;
                }
            }

            /*
             * Set up the variables for the formal parameters,
             * pointing back to the storage already allocated on
             * the eval stack by the caller.  Each time we add
             * a parameter, adjust the relative addresses of all
             * the previously handled parameters.
             */
            v = varslocal;
            while (v) {
                if (v->name[0] != '-') {
                    if (arraymode || (type == TYPE_WORD)) {
                        *(int *) ((unsigned char *) v + sizeof(var_t)) +=
                            2;
                    } else {
                        *(int *) ((unsigned char *) v + sizeof(var_t)) +=
                            1;
                    }
                }
                v = v->next;
            }

            if (arraymode) {
                v = alloc1(sizeof(var_t) + 2 * sizeof(int));
            } else {
                v = alloc1(sizeof(var_t) + sizeof(int));
            }

            *(int *) ((unsigned char *) v + sizeof(var_t)) = 4; // Skip over return address and frame pointer
            strncpy(v->name, name, VARNUMCHARS);
            v->type = (arraymode << 4) | type;
            v->next = NULL;

            if (arraymode) {
                /*
                 * Array pass-by-reference.
                 *
                 * In this case the pointer to the array body was pushed to the
                 * call stack by the caller, and the var_t record records the
                 * pointer to this pointer!
                 *
                 * Array size is not used in compiled code, so set it to -1 to
                 * indicate array-pass-by-reference.  Code in setintvar() and
                 * getintvar() uses this to work out that it has to do an extra
                 * dereference.
                 */
                *(int *) ((unsigned char *) v + sizeof(var_t) +
                          sizeof(int)) = -1;
            }

            if (varsend) {
                varsend->next = v;
            }
            varsend = v;
            if (!varsbegin) {
                varsbegin = v;
                varslocal = v;
            }

            eatspace();
            if (*txtPtr == ',') {
                ++txtPtr;       /* Eat the comma */
            }
        }
        if (expect(')')) {
            return RET_ERROR;
        }

    } else {
        /* Error if we just run into this line! */
        error(ERR_RUNSUB);
        return RET_ERROR;
    }
    return RET_SUCCESS;
}

/*
 * Handle endsub
 */
unsigned char doendsubr()
{
    if (compile) {
        rtSP = rtFP;
        compilingsub = 0;
        vars_deletecallframe();
        emitldi(0);
    }
    doreturn(0);
    return RET_SUCCESS;
}

/*
 * Perform call instruction
 * Expects sub name to call in readbuf
 * Return RET_SUCCESS if successful, RET_ERROR on error
 */
unsigned char docall()
{
    unsigned char type;
    char *p;
    int arg;
    char name[VARNUMCHARS];
    char name2[VARNUMCHARS];
    unsigned char j;
    unsigned char arraymode;
    var_t *oldvarslocal;
    var_t *newvarslocal;
    var_t *array;
    sub_t *s;
    unsigned char argbytes = 0;
    struct lineofcode *l = program;
    int origcounter = counter;
    unsigned char local = 0;

    /*
     * Do this before evaluating arguments, which overwrites readbuf
     */
    if (compile) {
        /*
         * Allocate this on the top-down stack in arena 2.  This grows down
         * towards the source code, which is growing up from the bottom of
         * arena 2.
         */
        s = alloc2top(sizeof(sub_t));
        strncpy(s->name, readbuf, SUBRNUMCHARS);
    }

    if (!compile) {
        counter = -1;
    }
    while (l) {
#ifdef EXTMEM
        copyfromaux2(l->line, l->len);
        p = embuf2;
#else
        p = l->line;
#endif
        if (!compile) {
            ++counter;
        }

        skipFlag = 0;

        while (p && (*p == ' ')) {
            ++p;
        }
        if (!strncmp(p, "sub ", 4)) {
            p += 4;
            while (p && (*p == ' ')) {
                ++p;
            }

            if (!compareUntil(p, readbuf, '(')) {

                /*
                 * Here we are parsing two lines at a time:
                 * The call (at *txtPtr as usual) and the
                 * sub being called (at *p).
                 */

                /* Eat the subroutine name at *p */
                while (p && (*p != '(')) {
                    ++p;
                }
                if (!p) {
                    error(ERR_EXPECT);
                    printchar('(');
                    return RET_ERROR;
                }
                ++p;            /* Eat the '(' */

                /*
                 * Set up txtPtr to start passing the argument
                 * list of the call
                 */
                eatspace();
                if (expect('(')) {
                    counter = origcounter;
                    return RET_ERROR;
                }

                if (!compile) {
                    /*
                     * Will need this later for looking up
                     * things in the 'old' frame
                     */
                    oldvarslocal = varslocal;

                    /*
                     * For CALL, stack frame is:
                     *  - CALLFRAME magic number
                     *  - line number of CALL
                     *  - Pointer to just after the call statement
                     *    (set further down in the code)
                     */
                    push_return(CALLFRAME);
                    push_return(origcounter);

                    vars_markcallframe();

                    newvarslocal = varslocal;
                }

                /*
                 * Iterate through the formal parameter
                 * list of the sub (at *p).
                 *
                 * For word and byte scalar parameters, we
                 * instantiate a local of the appropriate
                 * type, evaluate the corresponding expression
                 * in the call and store the result in this
                 * new local.
                 *
                 * For arrays we copy the header, leaving
                 * the pointer to the original global data
                 * intact.  Effectively, this gives arrays
                 * pass by reference semantics (similar to C).
                 * This trick works when passing literal
                 * arrays only.
                 */
                for (;;) {
                    while (p && (*p == ' ')) {
                        ++p;
                    }
                    if (!p) {
                        error(ERR_ARG);
                        return RET_ERROR;
                    }
                    if (*p == ')') {
                        break;
                    }
                    if (!strncmp(p, "word ", 5)) {
                        type = TYPE_WORD;
                    } else if (!strncmp(p, "byte ", 5)) {
                        type = TYPE_BYTE;
                    } else {
                        error(ERR_ARG);
                        return RET_ERROR;
                    }
                    p += 5;
                    while (p && (*p == ' ')) {
                        ++p;
                    }
                    if (!p) {
                        error(ERR_ARG);
                        return RET_ERROR;
                    }
                    for (j = 0; j < VARNUMCHARS; ++j) {
                        name[j] = 0;
                    }
                    j = 0;
                    while (p && (isalphach(*p) || isdigitch(*p))) {
                        if (j < VARNUMCHARS) {
                            name[j] = *p;
                        }
                        ++j;
                        ++p;
                    }
                    /*
                     * If argument is followed by '[]'
                     * then switch to pass array by ref
                     * mode.
                     */
                    arraymode = 0;
                    if (p && (*p == '[')) {
                        ++p;
                        if (p && (*p == ']')) {
                            ++p;
                            arraymode = 1;
                        } else {
                            error(ERR_ARG);
                            return RET_ERROR;
                        }
                    }

                    /*
                     * Now we go back to looking at the 
                     * call arguments
                     */

                    /* If end of line, error */
                    if (!(*txtPtr)) {
                        counter = origcounter;
                        error(ERR_ARG);
                        return RET_ERROR;
                    }
                    if (*txtPtr == ')') {
                        counter = origcounter;
                        error(ERR_ARG);
                        return RET_ERROR;
                    }
                    if (!arraymode) {
                        /*
                         * Pass scalar value
                         */
                        if (!compile) {
                            /* Back to old frame for lookup */
                            varslocal = oldvarslocal;
                        }
                        if (eval(0, &arg)) {
                            /* No expression found */
                            counter = origcounter;
                            error(ERR_ARG);
                            return RET_ERROR;
                        }
#ifdef EXTMEM
                        // Recover embuf2, which has been trashed by eval() above
                        copyfromaux2(l->line, l->len);
#endif
                        if (compile) {
                            if (type == TYPE_WORD) {
                                emit(VM_PSHWORD);
                                argbytes += 2;
                            } else {
                                emit(VM_PSHBYTE);
                                ++argbytes;
                            }
                        } else {
                            /* Back to new frame to create var */
                            varslocal = newvarslocal;
                            createintvar(name, type, 0, 1, arg, 0);
                        }
                    } else {
                        /*
                         * Array pass-by-reference
                         */
                        if (!compile) {
                            for (j = 0; j < VARNUMCHARS; ++j) {
                                name2[j] = 0;
                            }
                            j = 0;
                            while (txtPtr && (isalphach(*txtPtr) || isdigitch(*txtPtr))) {
                                if (j < VARNUMCHARS) {
                                    name2[j] = *txtPtr;
                                }
                                ++txtPtr;
                                ++j;
                            }
                            /* Back to old frame for lookup */
                            varslocal = oldvarslocal;
                            array = findintvar(name2, &local);
                            if (!array) {
                                counter = origcounter;
                                error(ERR_VAR);
                                return RET_ERROR;
                            }
                            /* j holds number of dimensions */
                            j = (array->type & 0xf0) >> 4;
                            if (((array->type & 0x0f) != type) || (j == 0)) {
                                counter = origcounter;
                                error(ERR_TYPE);
                                return RET_ERROR;
                            }
                            /* Back to new frame to create var */
                            varslocal = newvarslocal;
                            createintvar(name,
                                         type,
                                         j,
                                         *(getptrtoscalarword(array) + 1),
                                         0, *getptrtoscalarword(array));

                        } else {
                            if (eval(0, &arg)) {
                                /* No expression found */
                                counter = origcounter;
                                error(ERR_ARG);
                                return RET_ERROR;
                            }
                            emit(VM_PSHWORD);
                            argbytes += 2;
                        }
                    }
                    eatspace();
                    if (*txtPtr == ',') {
                        ++txtPtr;
                    }
                    eatspace();

                    while (p && (*p == ' ')) {
                        ++p;
                    }

                    if (!p) {
                        error(ERR_ARG);
                        return RET_ERROR;
                    }

                    if (*p == ',') {
                        ++p;    /* Eat the comma */
                    }
                }

                eatspace();
                if (expect(')')) {
                    counter = origcounter;
                    return RET_ERROR;
                }

                if (compile) {

                    emit_imm(VM_JSRIMM, 0xffff);

                    /*
                     * Create entry in call table
                     */
                    s->addr = rtPC - 2;
                    s->next = NULL;

                    if (callsend) {
                        callsend->next = s;
                    }
                    callsend = s;
                    if (!callsbegin) {
                        callsbegin = s;
                    }

                    /* Caller must drop the arguments
                     * pushed to call stack above */
                    if (argbytes) {
                        emitldi(argbytes);
                        emit(VM_DISCARD);
                    }
                } else {
                    /* Stash pointer to just after the call stmt */
                    push_return((int) txtPtr);

                    /*
                     * Set up parser to start executing first
                     * line of subroutine
                     */
                    current = l->next;
                    ++counter;
#ifdef EXTMEM
                    copyfromaux(current->line, current->len);
                    txtPtr = embuf;
#else
                    txtPtr = current->line;
#endif
                }
                return RET_SUCCESS;
            }
        }
        l = l->next;
    }
    counter = origcounter;
    error(ERR_NOSUB);
    return RET_ERROR;
}

/*
 * Handle return from subroutine.
 * Parameter retvalue is the value to be returned to the caller.
 * Returns RET_SUCCESS on success, RET_ERROR on error
 */
unsigned char doreturn(int retvalue)
{
    if (compile) {

        /*
         * Return value is already on evaluation stack
         */

        /* Update stack pointer to drop local variables */
        emit(VM_FPTOSP);

        /* And done! */
        emit(VM_RTS);

    } else {

        /*
         * Search the stack to find the first CALLFRAME.  This allows us
         * to unwind any inner stackframes (for example where we return
         * from within a FOR loop or IF statement.)
         */

        int p = returnSP + 1;

        while (p <= RETSTACKSZ - 1) {
            if (return_stack[p] == CALLFRAME) {
                /*
                 * Unwind the stack.
                 */
                returnSP = p;
                goto found;
            }
            ++p;
        }

        error(ERR_STACK);
        return RET_ERROR;

      found:
        /* Stash the return value */
        retregister = retvalue;

        vars_deletecallframe();

        backtotop(return_stack[p - 1], (char *) return_stack[p - 2]);
    }
    return RET_SUCCESS;
}

/*************************************************************************/

/*
 * Parse a decimal integer constant.
 * The text to parse is pointed to by txtPtr.
 * The result is placed in val.
 * If successful returns 0, otherwise 1.
 */
unsigned char parseint(int *val)
{
    *val = 0;
    if (!(*txtPtr)) {
        return 1;
    }
    if (!isdigitch(*txtPtr)) {
        return 1;
    }
    do {
        *val *= 10;
        *val += *txtPtr - '0';
        ++txtPtr;
    } while (isdigitch(*txtPtr));
    return 0;
}

/*
 * Return value of hex char
 */
unsigned char hexchar2val(char c)
{
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return c - '0';
}

/*
 * Parse a hexadecimal integer constant.
 * The text to parse is pointed to by txtPtr.
 * The result is placed in val.
 * If successful returns 0, otherwise 1.
 */
unsigned char parsehexint(int *val)
{
    *val = 0;
    if (!(isdigitch(*txtPtr) || ((*txtPtr >= 'a') && (*txtPtr <= 'f')))) {
        return 1;
    }
    do {
        *val *= 16;
        *val += hexchar2val(*txtPtr);
        ++txtPtr;
    } while (isdigitch(*txtPtr) || ((*txtPtr >= 'a') && (*txtPtr <= 'f')));
    return 0;
}

/*
 * Statement tokens - must be in order with no gaps in sequence
 */
#define TOK_COMM     150        /* (comment) must be lowest-numbered token */
#define TOK_PRDEC    151        /* pr.dec        */
#define TOK_PRDEC_S  152        /* pr.dec.s      */
#define TOK_PRHEX    153        /* pr.hex        */
#define TOK_PRMSG    154        /* pr.msg        */
#define TOK_PRNL     155        /* pr.nl         */
#define TOK_PRSTR    156        /* pr.str        */
#define TOK_PRCH     157        /* pr.ch         */
#define TOK_KBDCH    158        /* kbd.ch        */
#define TOK_KBDLN    159        /* kbd.ln        */
#define TOK_QUIT     160        /* quit          */
#define TOK_CLEAR    161        /* clear         */
#define TOK_VARS     162        /* vars          */
#define TOK_WORD     163        /* word          */
#define TOK_BYTE     164        /* byte          */
#define TOK_CONST    165        /* byte          */
#define TOK_RUN      166        /* run           */
#define TOK_COMPILE  167        /* comp          */
#define TOK_NEW      168        /* new           */
#define TOK_SUBR     169        /* sub           */
#define TOK_ENDSUBR  170        /* endsub        */
#define TOK_IF       171        /* if            */
#define TOK_ELSE     172        /* else          */
#define TOK_ENDIF    173        /* endif         */
#define TOK_FREE     174        /* free          */
#define TOK_CALL     175        /* call sub      */
#define TOK_RET      176        /* return        */
#define TOK_FOR      177        /* for           */
#define TOK_ENDFOR   178        /* endfor        */
#define TOK_WHILE    179        /* while         */
#define TOK_ENDW     180        /* endwhile      */
#define TOK_END      181        /* end           */
#define TOK_MODE     182        /* mode          */

/*
 * All the following tokens do not require trailing whitespace
 * Careful - the ordering matters!
 */
#define TOK_POKEWORD 183        /* poke word (*) */
#define TOK_POKEBYTE 184        /* poke byte (^) */

/* Line editor commands */
#define TOK_LOAD    185         /* Editor: load        */
#define TOK_SAVE    186         /* Editor: save        */
#define TOK_LIST    187         /* Editor: list        */
#define TOK_CHANGE  188         /* Editor: modify line */
#define TOK_APP     189         /* Editor: append line */
#define TOK_INS     190         /* Editor: insert line */
#define TOK_DEL     191         /* Editor: delete line */

/*
 * Used for the stmnttabent type field.  Code in parseline() uses this
 * value to determine how to handle parameters for the statement.
 *
 *  FULLLINE: full-line statement where the entire line 'belongs' to the
 *            statement (comments, sub and lbl are like this.)
 *  NOARGS: no arguments permitted.
 *  ONEARG: one expression is expected and evaluated.  No further arguments
 *          permitted.
 *  TWOARGS: two expressions are expected, separated by a comma
 *  INITIALARG: one expression is evaluated.  Any subsequent arguments may be
 *              evaluated by custom code for each statement.
 *  ONESTRARG: a string constant in quotes is expected
 *  INITIALNAMEARG: a single name is evaluated.  Any subsequent arguments may
 *                  be evaluated by custom code for each statement.  The name
 *                  must start with alpha character and has no spaces.
 *  CUSTOM: the statement has its own custom code to handle parameters
 */
enum stmnttype {
    FULLLINE,
    NOARGS,
    ONEARG,
    TWOARGS,
    INITIALARG,
    ONESTRARG,
    INITIALNAMEARG,
    CUSTOM
};

/*
 * Represents an entry in the statement table
 */
struct stmnttabent {
    char *name;
    unsigned char token;
    enum stmnttype type;
};

/*
 * Number of statements - must be updated to match the table
 */
#define NUMSTMNTS 42

/*
 * Statement table
 * Must be in order of sequentially increasing token value, so that
 * stmnttabent[t - TOK_COMM] is the line matching token t
 *
 * Also note that if a one name is a prefix of another, the longer one
 * must be first (so println goes before print).
 */
struct stmnttabent stmnttab[] = {

    /* Statements */
    {"\'", TOK_COMM, FULLLINE},         /* 1 */
    {"pr.dec", TOK_PRDEC, ONEARG},      /* 2 */
    {"pr.dec.s", TOK_PRDEC_S, ONEARG},  /* 3 */
    {"pr.hex", TOK_PRHEX, ONEARG},      /* 4 */
    {"pr.msg", TOK_PRMSG, ONESTRARG},   /* 5 */
    {"pr.nl", TOK_PRNL, NOARGS},        /* 6 */
    {"pr.str", TOK_PRSTR, ONEARG},      /* 7 */
    {"pr.ch", TOK_PRCH, ONEARG},        /* 8 */
    {"kbd.ch", TOK_KBDCH, ONEARG},      /* 9 */
    {"kbd.ln", TOK_KBDLN, TWOARGS},     /* 10 */
    {"quit", TOK_QUIT, NOARGS},         /* 11 */
    {"clear", TOK_CLEAR, NOARGS},       /* 12 */
    {"vars", TOK_VARS, NOARGS},         /* 13 */
    {"word", TOK_WORD, CUSTOM},         /* 14 */
    {"byte", TOK_BYTE, CUSTOM},         /* 15 */
    {"const", TOK_CONST, CUSTOM},       /* 16 */
    {"run", TOK_RUN, NOARGS},           /* 17 */
    {"comp", TOK_COMPILE, ONESTRARG},   /* 18 */
    {"new", TOK_NEW, NOARGS},           /* 19 */
    {"sub", TOK_SUBR, INITIALNAMEARG},  /* 20 */
    {"endsub", TOK_ENDSUBR, NOARGS},    /* 21 */
    {"if", TOK_IF, ONEARG},             /* 22 */
    {"else", TOK_ELSE, NOARGS},         /* 23 */
    {"endif", TOK_ENDIF, NOARGS},       /* 24 */
    {"free", TOK_FREE, NOARGS},         /* 25 */
    {"call", TOK_CALL, INITIALNAMEARG}, /* 26 */
    {"return", TOK_RET, ONEARG},        /* 27 */
    {"for", TOK_FOR, CUSTOM},           /* 28 */
    {"endfor", TOK_ENDFOR, NOARGS},     /* 29 */
    {"while", TOK_WHILE, ONEARG},       /* 30 */
    {"endwhile", TOK_ENDW, NOARGS},     /* 31 */
    {"end", TOK_END, NOARGS},           /* 32 */
    {"mode", TOK_MODE, ONEARG},         /* 33 */
    {"*", TOK_POKEWORD, INITIALARG},    /* 34 */
    {"^", TOK_POKEBYTE, INITIALARG},    /* 35 */

    /* Editor commands */
    {":r", TOK_LOAD, ONESTRARG},        /* 36 */
    {":w", TOK_SAVE, ONESTRARG},        /* 37 */
    {":l", TOK_LIST, CUSTOM},           /* 38 */
    {":c", TOK_CHANGE, INITIALARG},     /* 39 */
    {":a", TOK_APP, ONEARG},            /* 40 */
    {":i", TOK_INS, ONEARG},            /* 41 */
    {":d", TOK_DEL, INITIALARG}         /* 42 - set NUMSTMNTS to this value */
};

/*
 * Attempt to find statement keyword
 * Returns the token or ILLEGAL
 *
 * Uses table stmnttab, returning the token corresponding to the first
 * matching name.  Also checks for either a space or end of line following
 * the keyword, or will not declare a match.
 */
unsigned char matchstatement()
{
    unsigned char i;
    unsigned char len;
    char c;
    struct stmnttabent *s;

    for (i = 0; i < NUMSTMNTS; ++i) {
        s = &(stmnttab[i]);
        len = strlen(s->name);
        if (!strncmp(txtPtr, s->name, len)) {
            /*
             * Do not check for whitespace for tokens >= TOK_POKEWORD
             * Also do not check for whitespace for tokens <= TOK_COMM.
             */
            if ((s->token >= TOK_POKEWORD) || (s->token <= TOK_COMM)) {
                return s->token;
            }
            c = *(txtPtr + len);
            if ((c == 0) || (c == ' ') || (c == ';')) {
                return s->token;
            }
        }
    }
    return ILLEGAL;
}

/*
 * Used to check no arguments are passed to statements that do not take them
 * Returns 0 if end of line or semicolon next, 1 otherwise.
 */
unsigned char checkNoMoreArgs()
{
    eatspace();
    if (*txtPtr && (*txtPtr != ';')) {
        error(ERR_EXTRA);
        printchar(' ');
        print(txtPtr);
        return 1;
    }
    return 0;
}

#ifdef A2E
#pragma code-name (push, "LC")
#endif
void showfreespace() {
    print("free:\n");
#ifdef CC65
#ifdef EXTMEM
    print("Blk1: ");
    printdec(getfreespace1());
    print(" / ");
    printdec(gettotalspace1());
#ifdef EXTMEMCODE
    print(" vars\n");
#else
    print(" bytecode,vars\n");
#endif
    print("Blk2: ");
    printdec(getfreespace2());
    print(" / ");
    printdec(gettotalspace2());
    print(" lists,linkage\n");
    print("Aux:  ");
    printdec(getfreeauxmem());
    print(" / ");
    printdec(gettotalauxmem());
#ifdef EXTMEMCODE
    print(" source,bytecode");
#else
    print(" source");
#endif
#else
    print("Blk1: ");
    printdec(getfreespace1());
    print(" / ");
    printdec(gettotalspace1());
    print(" bytecode,vars\n");
    print("Blk2: ");
    printdec(getfreespace2());
    print(" / ");
    printdec(gettotalspace2());
    print(" source,linkage");
#endif
#else
    printdec(getfreespace1());
    print(" / ");
    printdec(gettotalspace1());
    print(" bytecode,vars\n");
    print("unlimited source,linkage");
#endif
}
#ifdef A2E
#pragma code-name (pop)
#endif

/* Parse a line from the input buffer
 * Handles statements
 * Starts reading from location of txtPtr
 * Returns:
 *   0: Keep executing
 *   1: Normal stop
 *   2: Error stop
 *   3: User stop / escape
 */
unsigned char parseline()
{
    int token;
    int arg;
    int arg2;
    char *p;
    char *startTxtPtr;
    struct stmnttabent *s;

    for (;;) {

        /* See if user requested stop */
        if (checkInterrupted()) {
            return 3;
        }

        eatspace();

        while (*txtPtr == ';') {
            ++txtPtr;
            if (!(*txtPtr)) {
                return 0;
            }
            eatspace();
        }

        if (!(*txtPtr)) {
            return 0;
        }

        startTxtPtr = txtPtr;

        token = matchstatement();

        /*
         * If skipFlag is set, then only process those tokens that
         * manipulate skipFlag:
         *   'if / else / endif'
         *   'while / endw'
         * Skip all others.
         */
        if (skipFlag) {
            if ((token != TOK_IF) &&
                (token != TOK_ELSE) &&
                (token != TOK_ENDIF) &&
                (token != TOK_WHILE) && (token != TOK_ENDW)) {

                /*
                 * Eat the statement up to semicolon or the
                 * end.
                 */
                while (*txtPtr && (*txtPtr != ';')) {
                    ++txtPtr;
                }
                continue;
            }
        }

        if (token == ILLEGAL) {

#ifdef CBM
            /*
             * If the first character is a digit then treat
             * this as an editor 'change line' command.  This
             * allows the VIC-20/C64 screen editor to work the
             * same way as in BASIC.
             */

            if (isdigitch(*txtPtr)) {

                token = TOK_CHANGE;
                s = &(stmnttab[token - TOK_COMM]);

            } else {
#endif
                /*
                 * Variable assignment winds up here
                 */
                if (assignorcreate(LET_MODE)) {
                    return 2;   /* Error */
                }
                continue;
#ifdef CBM
            }
#endif

        } else {

            s = &(stmnttab[token - TOK_COMM]);

            /* Eat the keyword */
            txtPtr += strlen(s->name);

            eatspace();
        }

        /*
         * If we are compiling it is good to keep a copy of the
         * VM program counter just before we begin argument 
         * handling.  This is useful for re-evaluating WHILE loop
         * guards, for example!
         */
        rtPCBeforeEval = rtPC;

        /*
         * Generic parameter handling based on statement type.
         */
        switch (s->type) {

        case NOARGS:
            /* Check end of input */
            arg = checkNoMoreArgs();
            if (arg) {
                return 2;
            }
            break;
        case ONEARG:
            /* Evaluate one arg and check end of input */
            if (eval(1, &arg)) {
                return 2;
            }
            break;
        case TWOARGS:
            /* Evaluate one arg don't check end of input */
            if (eval(0, &arg)) {
                return 2;
            }
            eatspace();
            if (expect(',')) {
                return 2;
            }
            /* Evaluate second arg, don't check end of input */
            if (eval(0, &arg2)) {
                return 2;
            }
            break;
        case INITIALARG:
            /* Evaluate one arg, don't check end of input */
            if (eval(0, &arg)) {
                return 2;
            }
            break;
        case ONESTRARG:
            /* Parse quoted string, place it in readbuf */
            if (!(*txtPtr == '"')) {
                return 2;
            }
            ++txtPtr;
            p = readbuf;
            while (*txtPtr && (*txtPtr != '"')) {
                *(p++) = *(txtPtr++);
            }
            *p = '\0';
            if (*txtPtr == '"') {
                ++txtPtr;
            } else {
                error(ERR_STR);
                return 2;
            }
            arg = checkNoMoreArgs();
            if (arg) {
                return 2;
            }
            break;
        case INITIALNAMEARG:
            /* Evaluate name, place in readbuf */
            /* Don't check end of input */
            if (!isalpha(*txtPtr)) {
                return 2;
            }
            p = readbuf;
            while (*txtPtr && (isalphach(*txtPtr) || isdigitch(*txtPtr))) {
                *(p++) = *(txtPtr++);
            }
            *p = '\0';
            break;
        case FULLLINE:
            /* Eat the line */
            while (*txtPtr) {
                ++txtPtr;
            }
            break;
#ifdef __GNUC__
        case CUSTOM:
            break;
#endif
        }

        /*
         * Code for individual statements.
         */
        switch (token) {
        case TOK_COMM:
            break;
        case TOK_QUIT:
#ifdef C64
            /* Restore normal NMI vector on C64 */
            POKE(808, 237);
#elif defined(VIC20)
            /* Restore normal NMI vector on VIC20 */
            POKE(808, 112);
#endif

            print("Bye!\n");
            EXIT(0);
        case TOK_PRDEC:
            if (compile) {
                emit(VM_PRDEC);
            } else {
                printdec(arg);
            }
            break;
        case TOK_PRDEC_S:
            if (compile) {
                emit(VM_DUP);   /* Preserve arg on the stack */
                emitldi(0x8000);
                emit(VM_BITAND);
                emit(VM_NOT);
                emit_imm(VM_BRNCHIMM, rtPC + 9);      /* Jump over printing of '-' */
                emitldi('-');
                emit(VM_PRCH);
                emit(VM_NEG);
                emit(VM_PRDEC);
            }
            if (arg < 0) {
                printchar('-');
                arg = -arg;
            }
            printdec(arg);
            break;
        case TOK_PRHEX:
            if (compile) {
                emit(VM_PRHEX);
            } else {
                printhex(arg);
            }
            break;
        case TOK_PRMSG:
            if (compile) {
                emitprmsg();
            } else {
                print(readbuf);
            }
            break;
        case TOK_PRNL:
            if (compile) {
#ifdef CBM
                emitldi(13);
#else
                emitldi(10);
#endif
                emit(VM_PRCH);
            } else {
                printchar('\n');
            }
            break;
        case TOK_PRSTR:
            if (compile) {
                emit(VM_PRSTR);
            } else {
                print((char *) arg);
            }
            break;
        case TOK_PRCH:
            if (compile) {
                emit(VM_PRCH);
            } else {
                printchar(arg);
            }
            break;
        case TOK_KBDCH:
            if (compile) {
                /* Address should be on the eval stack already */
                emit(VM_KBDCH);
                /* Now the keycode is pushed to the eval stack also */
                emit(VM_SWAP);
                emit(VM_STABYTE);
            } else {
#ifdef A2E
                /* Loop until we get a keypress */
                while (!(arg2 = getkey()));
                *(char *) arg = arg2;
#elif defined(CBM)
                /* Loop until we get a keypress */
                while (!(*(char *) arg = cbm_k_getin()));
#else
                print("kbd.ch unimplemented on Linux\n");
#endif
            }
            break;
        case TOK_KBDLN:
            if (compile) {
                /* Address and length should both be on the eval stack */
                emit(VM_KBDLN);
            } else {
                getln((char *) arg, arg2);
            }
            break;
        case TOK_CLEAR:
            clearvars();
            break;
        case TOK_VARS:
            printvars();
            break;
        case TOK_WORD:
            if (assignorcreate(WORD_MODE)) {
                return 2;
            }
            break;
        case TOK_BYTE:
            if (assignorcreate(BYTE_MODE)) {
                return 2;
            }
            break;
        case TOK_CONST:
            if (assignorcreate(CONST_MODE)) {
                return 2;
            }
            break;
        case TOK_RUN:
            run(0);             /* Start from beginning */
            break;
        case TOK_COMPILE:
            strncpy(filename, readbuf, FILENAMELEN);
            filename[FILENAMELEN] = 0; /* Just in case not terminated */
            compile = 1;
            subsbegin = subsend = NULL;
            callsbegin = callsend = NULL;
            CLEARRTCALLSTACK();
            run(0);
            if (compile) {
                emit(VM_END);
                linksubs();
                writebytecode();
                compile = 0;
            }
#ifndef __GNUC__
            CLEARHEAP2TOP();    /* Clear the linkage table */
#endif
            break;
        case TOK_NEW:
            new();
            break;
        case TOK_SUBR:
            if (dosubr()) {
                return 2;
            }
            break;
        case TOK_ENDSUBR:
            if (doendsubr()) {
                return 2;
            }
            break;
        case TOK_CALL:
            if (docall()) {
                return 2;
            }
            if (compile) {
                /* Drop the return value */
                emit(VM_DROP);
            } else {
                /* If we were called from immediate mode ... */
                /* Switch to run mode and continue */
                if (return_stack[returnSP + 2] == -1) {
                    run(1);
                }
            }
            break;
        case TOK_RET:
            if (doreturn(arg)) {
                /* Error */
                return 2;
            }

            /*
             * If this was a function invocation, just
             * return and let P() continue with its job!
             */
            if (return_stack[returnSP + 2] == -2) {
                return 1;
            }

            break;
        case TOK_IF:
            doif(arg);
            break;
        case TOK_ELSE:
            if (doelse()) {
                return 2;
            }
            break;
        case TOK_ENDIF:
            if (doendif()) {
                return 2;
            }
            break;
        case TOK_FOR:
            if (assignorcreate(FOR_MODE)) {
                return 2;
            }
            break;
        case TOK_ENDFOR:
            if (doendfor()) {
                return 2;
            }
            break;
        case TOK_WHILE:
            dowhile(startTxtPtr, arg);
            break;
        case TOK_ENDW:
            if (doendwhile()) {
                return 2;
            }
            break;
        case TOK_END:
            if (compile) {
                emit(VM_END);
            } else {
                return 1;       /* Normal stop */
            }
            break;
        case TOK_MODE:
#ifdef A2E
            if (arg == 40) {
                videomode(VIDEOMODE_40COL);
            } else if (arg == 80) {
                videomode(VIDEOMODE_80COL);
            } else {
                error(ERR_VALUE);
                return 2;
            }
#endif
            break;
        case TOK_FREE:
            showfreespace();
            break;
        case TOK_POKEWORD:
            eatspace();
            if (expect('=')) {
                return 2;
            }
            if (eval(1, &arg2)) {
                return 2;
            }
            if (compile) {
                emit(VM_SWAP);
                emit(VM_STAWORD);
                return 0;
            }
            *(int *) arg = arg2;
            break;
        case TOK_POKEBYTE:
            eatspace();
            if (expect('=')) {
                return 2;
            }
            if (eval(1, &arg2)) {
                return 2;
            }
            if (compile) {
                emit(VM_SWAP);
                emit(VM_STABYTE);
                return 0;
            }
            *(unsigned char *) arg = arg2;
            break;
        case TOK_APP:
            findline(arg);
            if (!current) {
                error(ERR_LINE);
                break;
            }
            editmode = 1;
            break;
        case TOK_INS:
            if (arg <= 1) {
                editmode = 2;   /* Special mode for insert
                                   first line */
            } else {
                findline(arg - 1);
                if (!current) {
                    error(ERR_LINE);
                    break;
                }
                editmode = 1;
            }
            break;
        case TOK_DEL:
            eatspace();
            if (!(*txtPtr)) {
                deleteline(arg, arg);   /* One arg */
                break;
            }
            if (expect(',')) {
                return 2;
            }
            if (eval(1, &arg2)) {
                return 2;
            }
            deleteline(arg, arg2);      /* Two args */
            break;
        case TOK_CHANGE:
            eatspace();
            if (expect(':')) {
                return 2;
            }
            findline(arg);
            if (!current) {
                error(ERR_LINE);
                break;
            }
            changeline(txtPtr);
            /* Don't execute the changed code yet! */
            return 0;
            break;
        case TOK_LIST:
            if (!(*txtPtr)) {
                list(1, 32767); /* No args */
                break;
            }
            if (eval(0, &arg)) {
                return 2;
            }
            eatspace();
            if (!(*txtPtr)) {
                list(arg, 32767);       /* One arg */
                break;
            }
            if (expect(',')) {
                return 2;
            }
            if (eval(1, &arg2)) {
                return 2;
            }
            list(arg, arg2);    /* Two args */
            break;
        case TOK_LOAD:
            if (readfile()) {
                return 2;       /* Error */
            }
            /* Because readfile() trashes lnbuf ... */
            return 0;
            break;
        case TOK_SAVE:
            if (writefile()) {
                return 2;       /* Error */
            }
            break;
        default:
            /* Should never get here */
            EXIT(99);
        }
    }
    return 0;
}

/*
 * Expects filename in readbuf.
 * If writemode = 0 then it opens file for reading, otherwise for writing.
 * Returns 0 if OK, 1 if error.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
unsigned char openfile(unsigned char writemode)
{
    char *readPtr = readbuf;

    if (writemode) {
        print("Writing ");
    } else {
        print("Reading ");
    }
    print(readPtr);
    printchar(':');
    while (*readPtr) {
        ++readPtr;
    }
#ifdef CBM
    /* Commodore only, append ',s' for SEQ file */
    *(readPtr++) = ',';
    *(readPtr++) = 's';
#endif
    *readPtr = '\0';

    readPtr = readbuf;

#ifdef CBM
    /* Commodore */
    if (cbm_open(1, 8, (writemode ? CBM_WRITE : CBM_READ), readPtr)) {
        error(ERR_FILE);
        return 1;
    }
#else

#ifdef A2E
    _filetype = 4;              /* Text file */
#endif

    /* POSIX */
    fd = fopen(readPtr, (writemode ? "w" : "r"));
    if (fd == NULL) {
        error(ERR_FILE);
        return 1;
    }
#endif

    return 0;

}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Load program from file.
 * Expects filename in readbuf.
 * Returns 0 if OK, 1 if error.
 * NOTE: Trashes lnbuf !!
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
unsigned char readfile()
{
    unsigned char i;
    unsigned char j;
    unsigned int bytes;
    unsigned char foundEOL;
    unsigned char bytesInBuf = 0;
    char *readPtr = readbuf;
    int donereading = 0;
    int count = 0;

    if (openfile(0)) {
        return 1;
    }

    clearvars();
    new();

    readPtr = readbuf;
    do {
        if (!donereading) {
#ifdef DEBUG_READFILE
            print("About to read ");
            printdec(255 - bytesInBuf);
            print(" bytes\n");
#endif

#ifdef CBM
            /* Commodore */
            bytes = cbm_read(1, readPtr, 255 - bytesInBuf);
#else
            /* POSIX */
            bytes = fread(readPtr, 1, 255 - bytesInBuf, fd);

#endif
            if (bytes == -1U) {
                error(ERR_FILE);
#ifdef CBM
                /* Commodore */
                cbm_close(1);
#else
                /* POSIX */
                fclose(fd);
#endif
                return 1;
            }
            if (!bytes) {
                donereading = 1;
            }

            readPtr += bytes;
            bytesInBuf += bytes;

#ifdef DEBUG_READFILE
            print("Read ");
            printdec(bytes);
            print(" bytes\nBuf[");
            for (i = 0; i < bytesInBuf; ++i) {
                printchar(readbuf[i]);
            }
            print("]\n");
#endif
        }

        foundEOL = 0;
        for (i = 0; i < bytesInBuf; ++i) {
            if (readbuf[i] == 10 || readbuf[i] == 13) {
                strncpy(lnbuf, readbuf, i);
                lnbuf[i] = 0;
                for (j = i + 1; j < bytesInBuf; ++j) {
                    readbuf[j - i - 1] = readbuf[j];
                }
                readPtr = readbuf + bytesInBuf - i - 1;
                bytesInBuf -= (i + 1);
                foundEOL = 1;
                break;
            }
        }

        if (foundEOL == 1) {
            if (!count) {
                insertfirstline(lnbuf);
                findline(1);
            } else {
                appendline(lnbuf);
            }
            ++count;
        } else {
            if (bytesInBuf == 255) {
                error(ERR_FILE);
#ifdef CBM
                /* Commodore */
                cbm_close(1);
#else
                /* Apple II and POSIX */
                fclose(fd);
#endif
                return 1;
            }
            /* Handle last line with missing CRLF */
            if (donereading == 1 && bytesInBuf) {
                readbuf[bytesInBuf] = '\0';
                appendline(readbuf);
                ++count;
                break;
            }
        }

    } while (bytes || bytesInBuf);

#ifdef CBM
    /* Commodore */
    cbm_close(1);
#else
    /* Apple II and POSIX */
    fclose(fd);
#endif

    printdec(count);
    print(" lines\n");
    return 0;
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Save program to file.
 * Expects filename in readbuf.
 * Returns 0 if OK, 1 if error.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
unsigned char writefile()
{
    unsigned int bytes;
    unsigned int index;

    if (openfile(1)) {
        return 1;
    }

    current = program;
    while (current) {
        index = 0;

#ifdef EXTMEM
        copyfromaux(current->line, current->len);
        for (index = 0; index < strlen(embuf); ++index) {
#else
        for (index = 0; index < strlen(current->line); ++index) {
#endif

#ifdef CBM
            /* Commodore */
            bytes = cbm_write(1, current->line + index, 1);

#elif defined(EXTMEM)
            /* Apple II, using extended memory driver */
            bytes = fwrite(embuf + index, 1, 1, fd);
#else
            /* POSIX and Apple II without extended memory */
            bytes = fwrite(current->line + index, 1, 1, fd);
#endif
            if (!bytes) {
                goto error;
            }
        }
#ifdef CBM
        /* Commodore */
        bytes += cbm_write(1, "\n", 1);
#elif defined(A2E)
        /* Apple II */
        bytes += fwrite("\r", 1, 1, fd);
#else
        /* POSIX */
        bytes += fwrite("\n", 1, 1, fd);
#endif
        if (!bytes) {
            goto error;
        }
        current = current->next;
    }

#ifdef CBM
    /* Commodore */
    cbm_close(1);
#else
    /* POSIX */
    fclose(fd);
#endif
    print("OK\n");
    return 0;

  error:
#ifdef CBM
    /* Commodore */
    cbm_close(1);
#else
    /* POSIX */
    fclose(fd);
#endif
    error(ERR_FILE);
    return 1;
}
#ifdef A2E
#pragma code-name (pop)
#endif


void run(unsigned char cont)
{
    int status = 0;

    calllevel = 0;
    skipFlag = 0;
    if (cont == 0) {
        counter = 0;
        clearvars();
        returnSP = RETSTACKSZ - 1;
        current = program;
    }
    while (current && !status) {
        if (compile) {
            printchar('.');
        }
#ifdef EXTMEM
        copyfromaux(current->line, current->len);
        txtPtr = embuf;
#else
        txtPtr = current->line;
#endif
        status = parseline();
        /* parseline() can set current to NULL when return is to
         * immediate mode */
        if (!current) {
            break;
        }
        current = current->next;
        ++counter;
    }
    switch (status) {
    case 2:
        print(" err at ");
        printdec(counter);
        printchar('\n');
        returnSP = (RETSTACKSZ - 1);
        skipFlag = 0;
        compile = 0;
        break;
    case 3:
        print("\nBrk at ");
        printdec(counter);
        printchar('\n');
        returnSP = (RETSTACKSZ - 1);
        skipFlag = 0;
        compile = 0;
        break;
    }
}

/*
 * Perform linkage.
 * The subroutine definitions are in the list that starts with subsbegin.
 * The subroutine calls are in the list that starts with callsbegin.
 */
#ifdef A2E
#pragma code-name (push, "LC")
#endif
void linksubs()
{
    sub_t *call;
    sub_t *sub;
    call = callsbegin;
    while (call) {
        sub = subsbegin;

        while (strncmp(sub->name, call->name, SUBRNUMCHARS)) {
            sub = sub->next;
            if (!sub) {
                error(ERR_LINK);
                return;
            }
        }
        emit_fixup(call->addr, sub->addr);
        call = call->next;
    }
}
#ifdef A2E
#pragma code-name (pop)
#endif

#ifdef A2E
#pragma code-name (push, "LC")
#endif
void list(unsigned int startline, unsigned int endline)
{
    unsigned int count = 1;

    current = program;
    while (current) {
        if ((count >= startline) && (count <= endline)) {
#ifdef CBM
            printchar(28);      /* Red */
            printchar(18);      /* Reverse On */
#elif defined(A2E)
            revers(1);
#endif
            printdec(count);
#ifdef CBM
            printchar(':');     /* To make scrn editor work */
            printchar(144);     /* Black */
            printchar(146);     /* Reverse Off */
#elif defined(A2E)
            revers(0);
#endif

#ifdef EXTMEM
            copyfromaux(current->line, current->len);
            print(embuf);
#else
            print(current->line);
#endif
            printchar('\n');
        }
        ++count;
        current = current->next;
    }
}
#ifdef A2E
#pragma code-name (pop)
#endif

/*
 * Clear the operator and operand stacks prior to evaluating expression.
 */
#define clearexprstacks() \
    operandSP = STACKSZ - 1; \
    operatorSP = STACKSZ - 1; \
    push_operator_stack(SENTINEL);


/*
 * Entry point.
 */
#ifdef __GNUC__
int
#else
void
#endif
main()
{

#ifdef EXTMEM
    unsigned char emhandle;
#endif
#ifdef A2E
    clrscr();
#elif defined(VIC20)
    POKE(0x900f, 254);          /* Nice color scheme */
#elif defined(C64)
    char *border = (char *) 0xd020;
    char *background = (char *) 0xd021;
    *border = 6;
    *background = 7;
#endif

#ifdef CBM
    printchar(147);             /* Clear */
    printchar(28);              /* Red */
    printchar(18);              /* Reverse On */

    /* Disable RUNSTOP/RESTORE */
    POKE(808, 100);
#endif

    calllevel = 1;
    returnSP = RETSTACKSZ - 1;
    varsbegin = NULL;
    varsend = NULL;
    varslocal = NULL;
    program = NULL;
    current = NULL;

#ifdef A2E
    videomode(VIDEOMODE_80COL);
    revers(1);
    print("      ***    EIGHTBALL V" VERSIONSTR "   ***     \n");
    print("      ***    (C)BOBBI, 2018    ***     \n\n");
    revers(0);
#ifdef EXTMEM
    emhandle = em_load_driver("a2e.auxmem.emd");
    if (emhandle != EM_ERR_OK) {
        print("Unable to load EM driver a2e.auxmem.emd\n");
        return;
    }
#endif
#elif defined(C64)
    print("      ***    EightBall v" VERSIONSTR "   ***      ");
    print("      ***    (c)Bobbi, 2018    ***      \n\n");
#elif defined(VIC20)
    /* Looks great in 22 cols! */
    print("*** EightBall v" VERSIONSTR "****** (c)Bobbi, 2017 ***\n\n");
#else
    print("      ***    EightBall v" VERSIONSTR "   ***     \n");
    print("      ***    (c)Bobbi, 2018    ***     \n\n");
#endif

#ifdef CBM
    printchar(144);             /* Black */
    printchar(146);             /* Reverse Off */
#endif
    print("Free Software.\n");
    print("Licenced under GPL.\n\n");

    CLEARHEAP1();
#ifdef CC65
    CLEARHEAP2TOP();
    CLEARHEAP2BTTM();
#ifdef EXTMEM
    CLEARAUXMEM();
#endif
#endif

    showfreespace();
    print("\n\n");

    /* Warm reset goes here */
    if (setjmp(jumpbuf) == 1) {
        print("Restart\n");
    }

    for (;;) {
        clearexprstacks();
        if (editmode) {
#ifdef CBM
            printchar(30);      /* Green */
            printchar(18);      /* Reverse On */
#endif
            printchar('>');
#ifdef CBM
            printchar(144);     /* Black */
            printchar(146);     /* Reverse Off */
#endif
        }

        compile = 0;
        getln(lnbuf, 255);

        switch (editmode) {
        case 0:                /* Not editing - immediate mode execute */
            txtPtr = lnbuf;
            current = NULL;
            counter = -1;
            switch (parseline()) {
            case 0:
            case 1:
                printchar('\n');
                break;
            case 2:
                print(" err\n");
                returnSP = (RETSTACKSZ - 1);
                skipFlag = 0;
                break;
            case 3:
                print("Brk\n");
                returnSP = (RETSTACKSZ - 1);
                skipFlag = 0;
                break;
            }
            if (returnSP != (RETSTACKSZ - 1)) {
                error(ERR_STACK);
                returnSP = (RETSTACKSZ - 1);
            }
            skipFlag = 0;
            break;
        case 1:                /* Editing the program, period to escape */
            if (lnbuf[0] == '.') {
                editmode = 0;
            } else {
                appendline(lnbuf);
            }
            break;
        case 2:                /* Special case for insert first line */
            insertfirstline(lnbuf);
            findline(1);
            editmode = 1;
            break;
        }
    }
}
