#pragma once


typedef void (*builtin_handler_t)(int argc, char **argv);

typedef struct {
    const char *name;
    builtin_handler_t handler;
} builtin_t;

