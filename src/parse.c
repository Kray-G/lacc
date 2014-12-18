#include "lcc.h"
#include "symbol.h"
#include "ir.h"

#include <stdlib.h>
#include <string.h>

/* Tokenization interface and helper functions */
static struct token peek_value;
static int has_value;
static int eof;

static struct token
readtoken()
{
    struct token t;
    if (has_value) {
        has_value = 0;
        return peek_value;
    }
    eof = !get_token(&t);
    return t;
}

static enum token_type
peek()
{
    if (!has_value) {
        peek_value = readtoken();
        has_value = 1;
    }
    return peek_value.type;
}

static void
consume(enum token_type expected)
{
    struct token t = readtoken();
    if (t.type != expected) {
        error("Unexpected token %s, aborting\n", t.value);
        exit(1);
    }
}

static block_t *declaration();
static typetree_t *declaration_specifiers();
static typetree_t *declarator(typetree_t *, const char **);
static typetree_t *pointer(const typetree_t *);
static typetree_t *direct_declarator(typetree_t *, const char **);
static typetree_t *parameter_list(const typetree_t *);
static block_t *block(block_t *);
static block_t *statement(block_t *);

static const symbol_t *identifier();

/* expression nodes that are called in high level rules */
static const symbol_t *expression(block_t *);
static const symbol_t *constant_expression(block_t *);
static const symbol_t *assignment_expression(block_t *);

/* External interface */
void
compile()
{
    push_scope();

    do {
        block_t *fun = declaration();
        if (fun != NULL) {
            output_block(fun);
            puts("");
        }
        peek();
    } while (!eof);

    pop_scope();
}

/* Return either an initialized declaration, or a function definition.
 * Forward declarations are just registered in the symbol table. 
 */
static block_t *
declaration()
{
    block_t *function;
    const symbol_t *symbol;
    typetree_t *type, *base = declaration_specifiers();
    int i;

    while (1) {
        const char *name = NULL;
        type = declarator(base, &name);
        symbol = sym_add(name, type);

        switch (peek()) {
            case ';':
                consume(';');
                return NULL;
            case '=':
                consume('=');
                /* Assignment expression for external declaration must be a 
                 * constant value computable at compile time. */
                assignment_expression(NULL); /* todo: store value in symtab */
                if (peek() != ',') {
                    consume(';');
                    return NULL;
                }
                break;
            case '{':
                if (type->type != FUNCTION || symbol->depth > 0) {
                    error("Invalid function definition, aborting");
                    exit(1);
                }
                function = block_init(name);
                push_scope();
                for (i = 0; i < type->n_args; ++i) {
                    if (type->params[i] == NULL) {
                        error("Missing parameter name at position %d, aborting", i + 1);
                        exit(1);
                    }
                    sym_add(type->params[i], type->args[i]);
                }
                block(function); /* generate code */
                pop_scope();
                return function;
            default:
                break;
        }
        consume(',');
    }
}

static typetree_t *
declaration_specifiers()
{
    int end = 0;
    typetree_t *type = NULL; 
    int flags = 0x0;
    while (1) {
        switch (peek()) {
            case AUTO: case REGISTER: case STATIC: case EXTERN: case TYPEDEF:
                /* todo: something about storage class, maybe do it before this */
                break;
            case CHAR:
                type = type_init(CHAR_T);
                break;
            case SHORT:
            case INT:
            case LONG:
            case SIGNED:
            case UNSIGNED:
                type = type_init(INT64_T);
                break;
            case FLOAT:
            case DOUBLE:
                type = type_init(DOUBLE_T);
                break;
            case VOID:
                type = type_init(VOID_T);
                break;
            case CONST:
                flags |= CONST_Q;
                break;
            case VOLATILE:
                flags |= VOLATILE_Q;
                break;
            default:
                end = 1;
        }
        if (end) break;
        consume(peek());
    }
    if (type == NULL) {
        error("Missing type specifier, aborting");
        exit(1);
    }
    type->flags = flags;
    return type;
}

static typetree_t *
declarator(typetree_t *base, const char **symbol)
{
    while (peek() == '*') {
        base = pointer(base);
    }
    return direct_declarator(base, symbol);
}

static typetree_t *
pointer(const typetree_t *base)
{
    typetree_t *type = type_init(POINTER);
    type->next = base;
    base = type;
    consume('*');
    while (peek() == CONST || peek() == VOLATILE) {
        type->flags |= (readtoken().type == CONST) ? CONST_Q : VOLATILE_Q;
    }
    return type;
}

static long
get_symbol_constant_value(const symbol_t *symbol, long *out)
{
    if (symbol->type->type == INT64_T && symbol->is_immediate) {
        *out = symbol->immediate.longval;
        return 1;
    }
    return 0;
}

/* Consume [s0][s1][s2]..[sn] in array declarations, returning type
 * <symbol> :: [s0] [s1] [s2] .. [sn] (base)
 */
static typetree_t *
direct_declarator_array(typetree_t *base)
{
    if (peek() == '[') {
        typetree_t *root;
        const symbol_t *expr;
        long length;

        consume('[');
        if (peek() != ']') {
            block_t *throwaway = block_init(NULL);
            expr = constant_expression(throwaway);
            if (!get_symbol_constant_value(expr, &length)) {
                error("Array declaration must be a compile time constant, aborting");
                exit(1);
            }
            if (length < 1) {
                error("Invalid array size %ld, aborting");
                exit(1);
            }
        } else {
            /* special value for unspecified array size */
            length = 0;
        }
        consume(']');
        
        base = direct_declarator_array(base);
        root = type_init(ARRAY);

        root->next = base;
        root->length = length;
        root->size = (base->type == ARRAY) ? base->size * base->length : base->size;
        base = root;
    }
    return base;
}

static typetree_t *
direct_declarator(typetree_t *base, const char **symbol)
{
    typetree_t *type = base;
    switch (peek()) {
        case IDENTIFIER: 
            *symbol = readtoken().value;
            break;
        case '(':
            consume('(');
            type = declarator(base, symbol);
            consume(')');
            break;
        default: break;
    }
    /* left-recursive declarations like 'int foo[10][5];' */
    while (peek() == '[' || peek() == '(') {
        switch (peek()) {
            case '[':
                type = direct_declarator_array(base);
                /*type = type_init(ARRAY);
                type->d.arr.of = base;
                consume('[');
                if (peek() != ']') {
                    symbol_t *expr = constant_expression();
                    long size;
                    if (!get_symbol_constant_value(expr, &size)) {
                        error("Array declaration must be a compile time constant, aborting");
                        exit(1);
                    }
                    if (size < 1) {
                        error("Invalid array size %ld, aborting");
                        exit(1);
                    }
                    type->d.arr.size = size;
                }
                consume(']');*/
                break;
            case '(': {
                consume('(');
                type = parameter_list(base);
                consume(')');
                break;
            }
            default: break; /* impossible */
        }
        base = type;
    }
    return type;
}

/* FOLLOW(parameter-list) = { ')' }, peek to return empty list;
 * even though K&R require at least specifier: (void)
 * Set parameter-type-list = parameter-list, including the , ...
 */
static typetree_t *
parameter_list(const typetree_t *base)
{
    typetree_t *type = type_init(FUNCTION);
    const typetree_t **args = NULL;
    const char **params = NULL;
    int nargs = 0;

    while (peek() != ')') {
        const char *symbol = NULL;
        typetree_t *decl = declaration_specifiers();
        decl = declarator(decl, &symbol);

        nargs++;
        args = realloc(args, sizeof(typetree_t *) * nargs);
        params = realloc(params, sizeof(char *) * nargs);
        args[nargs - 1] = decl;
        params[nargs - 1] = symbol;

        if (peek() != ',') break;
        consume(',');
        if (peek() == ')') {
            error("Trailing comma in parameter list, aborting");
            exit(1);
        }
        if (peek() == DOTS) {
            consume(DOTS); /* todo: add vararg type */
            break;
        }
    }
    
    type->next = base;
    type->n_args = nargs;
    type->args = args;
    type->params = params;
    return type;
}

/* Treat statements and declarations equally, allowing declarations in between
 * statements as in modern C. Called compound-statement in K&R.
 */
static block_t *
block(block_t *parent)
{
    consume('{');
    while (peek() != '}') {
        parent = statement(parent);
    }
    consume('}');
    return parent;
}

/* Create or expand a block of code. Consecutive statements without branches
 * are stored as a single block, passed as parent. Statements with branches
 * generate new blocks. Returns the current block of execution after the
 * statement is done. For ex: after an if statement, the empty fallback is
 * returned. Caller must keep handles to roots, only the tail is returned. */
static block_t *
statement(block_t *parent)
{
    block_t *node;

    /* Store reference to top of loop, for resolving break and continue. Use
     * call stack to keep track of depth, backtracking to the old value. */
    static block_t *break_target, *continue_target;
    block_t *old_break_target, *old_continue_target;

    enum token_type t = peek();

    switch (t) {
        case ';':
            consume(';');
            node = parent;
            break;
        case '{':
            push_scope();
            node = block(parent); /* execution continues  */
            pop_scope();
            break;
        case SWITCH:
        case IF:
        {
            block_t *right = block_init(NULL), *next = block_init(NULL);
            readtoken();
            consume('(');

            /* node becomes a branch, store the expression as condition
             * variable and append code to compute the value. */
            parent->expr = expression(parent);
            consume(')');

            parent->jump[0] = next;
            parent->jump[1] = right;

            /* The order is important here: Send right as head in new statement
             * graph, and store the resulting tail as new right, hooking it up
             * to the fallback of the if statement. */
            right = statement(right);
            right->jump[0] = next;

            if (peek() == ELSE) {
                block_t *left = block_init(NULL);
                consume(ELSE);

                /* Again, order is important: Set left as new jump target for
                 * false if branch, then invoke statement to get the
                 * (potentially different) tail. */
                parent->jump[0] = left;
                left = statement(left);

                left->jump[0] = next;
            }
            node = next;
            break;
        }
        case WHILE:
        case DO:
        {
            block_t *top = block_init(NULL), *body = block_init(NULL), *next = block_init(NULL);
            parent->jump[0] = top; /* Parent becomes unconditional jump. */

            /* Enter a new loop, store reference for break and continue target. */
            old_break_target = break_target;
            old_continue_target = continue_target;
            break_target = next;
            continue_target = top;

            readtoken();

            if (t == WHILE) {
                consume('(');
                top->expr = expression(top);
                consume(')');
                top->jump[0] = next;
                top->jump[1] = body;

                /* Generate statement, and get tail end of body to loop back */
                body = statement(body);
                body->jump[0] = top;
            } else if (t == DO) {

                /* Generate statement, and get tail end of body */
                body = statement(top);
                consume(WHILE);
                consume('(');
                body->expr = expression(body); /* Tail becomes branch. (nb: wrong if tail is return?!) */
                body->jump[0] = next;
                body->jump[1] = top;
                consume(')');
            }

            /* Restore previous nested loop */
            break_target = old_break_target;
            continue_target = old_continue_target;

            node = next;
            break;
        }
        case FOR:
        {
            block_t *top = block_init(NULL), *body = block_init(NULL), *increment = block_init(NULL), *next = block_init(NULL);

            /* Enter a new loop, store reference for break and continue target. */
            old_break_target = break_target;
            old_continue_target = continue_target;
            break_target = next;
            continue_target = top;

            consume(FOR);
            consume('(');
            if (peek() != ';') {
                expression(parent);
            }
            consume(';');
            if (peek() != ';') {
                parent->jump[0] = top;
                top->expr = expression(top);
                top->jump[0] = next;
                top->jump[1] = body;
            } else {
                /* Infinite loop */
                free(top);
                parent->jump[0] = body;
                top = body;
            }
            consume(';');
            if (peek() != ')') {
                expression(increment);
                increment->jump[0] = top;
            }
            consume(')');
            body = statement(body);
            body->jump[0] = increment;

            /* Restore previous nested loop */
            break_target = old_break_target;
            continue_target = old_continue_target;

            node = next;
            break;
        }
        case GOTO:
            consume(GOTO);
            identifier();
            /* todo */
            consume(';');
            break;
        case CONTINUE:
        case BREAK:
            readtoken();
            parent->jump[0] = (t == CONTINUE) ? 
                continue_target :
                break_target;
            consume(';');
            /* Return orphan node, which is dead code unless there is a label
             * and a goto statement. Dead code elimination done in another pass. */
            node = block_init(NULL); 
            break;
        case RETURN:
            consume(RETURN);
            if (peek() != ';')
                parent->expr = expression(parent);
            consume(';');
            node = block_init(NULL); /* orphan */
            break;
        case CASE:
        case DEFAULT:
            /*readtoken();
            if (peek() == ':') {
                consume(':');
                addchild(node, statement());
            } else {
                constant_expression(block);
                consume(':');
                addchild(node, statement());
            }*/
            break;
        case IDENTIFIER: /* also part of label statement, need 2 lookahead */
        case INTEGER: /* todo: any constant value */
        case STRING:
        case '(':
            expression(parent);
            consume(';');
            node = parent;
            break;
        default:
            declaration();
            node = parent;
    }
    return node;
}

static const symbol_t *
identifier()
{
    struct token name = readtoken();
    const symbol_t *sym = sym_lookup(name.value);
    if (sym == NULL) {
        error("Undefined symbol '%s', aborting", name.value);
        exit(0);
    }
    return sym;
}

static const symbol_t *conditional_expression(block_t *block);
static const symbol_t *logical_expression(block_t *block);
static const symbol_t *or_expression(block_t *block);
static const symbol_t *and_expression(block_t *block);
static const symbol_t *equality_expression(block_t *block);
static const symbol_t *relational_expression(block_t *block);
static const symbol_t *shift_expression(block_t *block);
static const symbol_t *additive_expression(block_t *block);
static const symbol_t *multiplicative_expression(block_t *block);
static const symbol_t *cast_expression(block_t *block);
static const symbol_t *postfix_expression(block_t *block);
static const symbol_t *unary_expression(block_t *block);
static const symbol_t *primary_expression(block_t *block);

static const symbol_t *
expression(block_t *block)
{
    return assignment_expression(block);
}

static const symbol_t *
assignment_expression(block_t *block)
{
    op_t op;
    const symbol_t *l = conditional_expression(block), *r;
    if (peek() == '=') {
        consume('=');
        /* todo: node must be unary-expression or lower (l-value) */
        r = assignment_expression(block);

        op.type = IR_ASSIGN;
        op.a = l;
        op.b = r;
        ir_append(block, op);
    } 
    return l;
}

static const symbol_t *
constant_expression(block_t *block)
{
    return conditional_expression(block);
}

static const symbol_t *
conditional_expression(block_t *block)
{
    const symbol_t *sym = logical_expression(block);
    if (peek() == '?') {
        consume('?');
        expression(block);
        consume(':');
        conditional_expression(block);
    }
    return sym;
}

/* merge AND/OR */
static const symbol_t *
logical_expression(block_t *block)
{
    op_t op;
    const symbol_t *l, *r, *res;
    l = or_expression(block);
    while (peek() == LOGICAL_OR || peek() == LOGICAL_AND) {
        optype_t optype = (readtoken().type == LOGICAL_AND) ? IR_OP_LOGICAL_AND : IR_OP_LOGICAL_OR;

        r = and_expression(block);
        res = sym_mktemp(type_combine(l->type, r->type));

        op.type = optype;
        op.a = res;
        op.b = l;
        op.c = r;
        ir_append(block, op);

        l = res;
    }
    return l;
}

/* merge OR/XOR */
static const symbol_t *
or_expression(block_t *block)
{
    op_t op;
    const symbol_t *l, *r, *res;
    l = and_expression(block);
    while (peek() == '|' || peek() == '^') {
        optype_t optype = (readtoken().type == '|') ? IR_OP_BITWISE_OR : IR_OP_BITWISE_XOR;

        r = and_expression(block);
        res = sym_mktemp(type_combine(l->type, r->type));

        op.type = optype;
        op.a = res;
        op.b = l;
        op.c = r;
        ir_append(block, op);

        l = res;
    }
    return l;
}

static const symbol_t *
and_expression(block_t *block)
{
    op_t op;
    const symbol_t *l, *r, *res;
    l = equality_expression(block);
    while (peek() == '&') {
        consume('&');
        r = and_expression(block);
        res = sym_mktemp(type_combine(l->type, r->type));

        op.type = IR_OP_BITWISE_AND;
        op.a = res;
        op.b = l;
        op.c = r;
        ir_append(block, op);

        l = res;
    }
    return l;
}

static const symbol_t *
equality_expression(block_t *block)
{
    return relational_expression(block);
}

static const symbol_t *
relational_expression(block_t *block)
{
    return shift_expression(block);
}

static const symbol_t *
shift_expression(block_t *block)
{
    return additive_expression(block);
}

static const symbol_t *
additive_expression(block_t *block)
{
    op_t op;
    const symbol_t *l, *r, *res;
    l = multiplicative_expression(block);
    while (peek() == '+' || peek() == '-') {
        optype_t optype = (readtoken().type == '+') ? IR_OP_ADD : IR_OP_SUB;

        r = multiplicative_expression(block);
        res = sym_mktemp(type_combine(l->type, r->type));

        op.type = optype;
        op.a = res;
        op.b = l;
        op.c = r;
        ir_append(block, op);

        l = res;
    }
    return l;
}

static const symbol_t *
multiplicative_expression(block_t *block)
{
    op_t op;
    const symbol_t *l, *r, *res;
    l = cast_expression(block);
    while (peek() == '*' || peek() == '/' || peek() == '%') {
        struct token tok = readtoken();
        optype_t optype = (tok.type == '*') ?
            IR_OP_MUL : (tok.type == '/') ?
                IR_OP_DIV : IR_OP_MOD;

        r = cast_expression(block);
        res = sym_mktemp(type_combine(l->type, r->type));

        op.type = optype;
        op.a = res;
        op.b = l;
        op.c = r;
        ir_append(block, op);

        l = res;
    }
    return l;
}

static const symbol_t *
cast_expression(block_t *block)
{
    return unary_expression(block);
}

static const symbol_t *
unary_expression(block_t *block)
{
    return postfix_expression(block);
}

/* This rule is left recursive, build tree bottom up
 */
static const symbol_t *
postfix_expression(block_t *block)
{
    const symbol_t *root = primary_expression(block);

    while (peek() == '[' || peek() == '(' || peek() == '.') {
        switch (peek()) {
            /* Parse and emit ir for general array indexing
             *  - From K&R: an array is not a variable, and cannot be assigned or modified.
             *    Referencing an array always converts the first rank to pointer type,
             *    e.g. int foo[3][2][1]; a = foo; assignment has the type int (*)[2][1].
             *  - Functions return and pass pointers to array. First index not necessary to
             *    specify in array (pointer) parameters: int (*foo(int arg[][3][2][1]))[3][2][1]
             */
            case '[':
                consume('[');
                {
                    op_t mul, add;
                    const symbol_t *res, *l, *r;
                    l = expression(block);
                    r = sym_mkimmediate_long((long) root->type->size);
                    res = sym_mktemp(type_combine(l->type, r->type));

                    mul.type = IR_OP_MUL;
                    mul.a = res;
                    mul.b = l;
                    mul.c = r;
                    ir_append(block, mul);

                    r = sym_mktemp(type_combine(root->type, res->type));
                    add.type = IR_OP_ADD;
                    add.a = r;
                    add.b = root;
                    add.c = res;
                    ir_append(block, add);

                    root = r;
                }
                consume(']');

                if (root->type->next->type == ARRAY) {
                    ((symbol_t *)root)->type = type_deref(root->type);
                } else {
                    op_t deref;
                    const symbol_t *res;
                    if (root->type->type != POINTER) {
                        error("Cannot dereference non-pointer, aborting");
                        exit(0);
                    }
                    res = sym_mktemp(root->type->next);
                    deref.type = IR_DEREF;
                    deref.a = res;
                    deref.b = root;
                    ir_append(block, deref);

                    root = res;
                }
                break;
            /*case '(':
                addchild(parent, argument_expression_list()); 
                consume('(');
                consume(')');
                break;
            case '.':
                parent->token = readtoken();
                addchild(parent, identifier());
                break;*/
            default:
                error("Unexpected token '%s', not a valid postfix expression", readtoken().value);
                exit(0);
        }
    }
    return root;
}

static const symbol_t *
primary_expression(block_t *block)
{
    const symbol_t *symbol;
    struct token token = readtoken();
    switch (token.type) {
        case IDENTIFIER:
            symbol = sym_lookup(token.value);
            if (symbol == NULL) {
                error("Undefined symbol '%s', aborting", token.value);
                exit(0);
            }
            break;
        case INTEGER:
            symbol = sym_mkimmediate(INT64_T, token.value);
            break;
        case '(':
            symbol = expression(block);
            consume(')');
            break;
        default:
            error("Unexpected token '%s', not a valid primary expression", token.value);
            exit(0);
    }
    return symbol;
}
