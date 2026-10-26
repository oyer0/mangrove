/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../userspace/fetch/fetch_args.h"

static fetch_arguments_result_t parse(int argc, char **argv,
                                      fetch_arguments_t *arguments,
                                      const char **bad)
{
    return fetch_arguments_parse(argc, argv, arguments, bad);
}

int main(void)
{
    fetch_arguments_t arguments;
    const char *bad = NULL;
    char *grouped[] = {(char *)"fetch", (char *)"-qopage.html",
                       (char *)"example.com"};
    assert(parse(3, grouped, &arguments, &bad) == FETCH_ARGUMENTS_OK);
    assert(arguments.quiet && !strcmp(arguments.output_path, "page.html"));
    assert(arguments.url_count == 1U &&
           !strcmp(arguments.urls[0], "example.com"));
    fetch_arguments_destroy(&arguments);

    char *long_options[] = {(char *)"fetch", (char *)"--quiet",
        (char *)"--output=page.html", (char *)"--",
        (char *)"-leading.example"};
    assert(parse(5, long_options, &arguments, &bad) == FETCH_ARGUMENTS_OK);
    assert(arguments.quiet && arguments.url_count == 1U);
    assert(!strcmp(arguments.output_path, "page.html"));
    assert(!strcmp(arguments.urls[0], "-leading.example"));
    fetch_arguments_destroy(&arguments);

    char *separated[] = {(char *)"fetch", (char *)"-i", (char *)"urls.txt"};
    assert(parse(3, separated, &arguments, &bad) == FETCH_ARGUMENTS_OK);
    assert(!strcmp(arguments.input_file, "urls.txt") &&
           arguments.url_count == 0U);
    fetch_arguments_destroy(&arguments);

    char *output_separated[] = {(char *)"fetch", (char *)"-o",
        (char *)"page.html", (char *)"example.com"};
    assert(parse(4, output_separated, &arguments, &bad) == FETCH_ARGUMENTS_OK);
    assert(!strcmp(arguments.output_path, "page.html") &&
           arguments.url_count == 1U);
    fetch_arguments_destroy(&arguments);

    char *output_long_separated[] = {(char *)"fetch",
        (char *)"--output", (char *)"page.html",
        (char *)"example.com"};
    assert(parse(4, output_long_separated, &arguments, &bad) ==
           FETCH_ARGUMENTS_OK);
    assert(!strcmp(arguments.output_path, "page.html"));
    fetch_arguments_destroy(&arguments);

    char *long_input[] = {(char *)"fetch", (char *)"--input-file=urls.txt"};
    assert(parse(2, long_input, &arguments, &bad) == FETCH_ARGUMENTS_OK);
    assert(!strcmp(arguments.input_file, "urls.txt"));
    fetch_arguments_destroy(&arguments);

    char *multiple[] = {(char *)"fetch", (char *)"one.example",
                        (char *)"two.example", (char *)"-q"};
    assert(parse(4, multiple, &arguments, &bad) == FETCH_ARGUMENTS_OK);
    assert(arguments.quiet && arguments.url_count == 2U);
    fetch_arguments_destroy(&arguments);

    char *missing[] = {(char *)"fetch", (char *)"-o"};
    assert(parse(2, missing, &arguments, &bad) ==
           FETCH_ARGUMENTS_MISSING_VALUE);
    assert(bad && !strcmp(bad, "-o"));
    char *unknown[] = {(char *)"fetch", (char *)"--overwrite"};
    assert(parse(2, unknown, &arguments, &bad) ==
           FETCH_ARGUMENTS_UNKNOWN_OPTION);
    char *unexpected[] = {(char *)"fetch", (char *)"--quiet=yes"};
    assert(parse(2, unexpected, &arguments, &bad) ==
           FETCH_ARGUMENTS_UNEXPECTED_VALUE);
    char *duplicate[] = {(char *)"fetch", (char *)"-oone",
                         (char *)"-otwo", (char *)"host"};
    assert(parse(4, duplicate, &arguments, &bad) ==
           FETCH_ARGUMENTS_DUPLICATE_OPTION);
    char *ambiguous[] = {(char *)"fetch", (char *)"-o", (char *)"out",
                         (char *)"-i", (char *)"urls.txt"};
    assert(parse(5, ambiguous, &arguments, &bad) == FETCH_ARGUMENTS_OK);
    fetch_arguments_destroy(&arguments);

    usize count = 2048U;
    char **many = (char **)calloc(count + 1U, sizeof(*many));
    assert(many);
    many[0] = (char *)"fetch";
    for (usize index = 1; index <= count; index++) many[index] = (char *)"host.example";
    assert(parse((int)(count + 1U), many, &arguments, &bad) ==
           FETCH_ARGUMENTS_OK);
    assert(arguments.url_count == count);
    fetch_arguments_destroy(&arguments);
    free(many);
    puts("fetch options: grouped/attached/separated forms, --, input and large URL lists passed");
    return 0;
}
