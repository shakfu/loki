/* choice.c - A one-key choice on the message line (agent permission prompts)
 *
 * The editor keeps running while a choice is pending: background work and
 * redraws continue, and the next key answers. 1-9 picks an option; Esc or
 * Ctrl-C cancels. Other keys are swallowed so a stray keystroke cannot edit
 * the buffer while the user is being asked something.
 */

#include <stdio.h>
#include <string.h>
#include "internal.h"
#include "choice.h"

static struct {
    int active;
    int count;
    char prompt[512];
    choice_fn fn;
    void *ud;
} choice;

int choice_begin(const char *question, const char *const *labels, int count,
                 choice_fn fn, void *ud) {
    if (choice.active || count < 1 || count > CHOICE_MAX) return -1;
    size_t n = (size_t)snprintf(choice.prompt, sizeof(choice.prompt), "%s", question);
    for (int i = 0; i < count && n < sizeof(choice.prompt); i++)
        n += (size_t)snprintf(choice.prompt + n, sizeof(choice.prompt) - n,
                              "  [%d] %s", i + 1, labels[i]);
    if (n < sizeof(choice.prompt))
        snprintf(choice.prompt + n, sizeof(choice.prompt) - n, "  [Esc] cancel");
    choice.active = 1;
    choice.count = count;
    choice.fn = fn;
    choice.ud = ud;
    return 0;
}

const char *choice_prompt(void) {
    return choice.active ? choice.prompt : NULL;
}

static void finish(int index) {
    choice_fn fn = choice.fn;
    void *ud = choice.ud;
    choice.active = 0;
    choice.fn = NULL;
    if (fn) fn(index, ud);  /* may start the next choice */
}

int choice_handle_key(int key) {
    if (!choice.active) return 0;
    if (key >= '1' && key < '1' + choice.count) finish(key - '1');
    else if (key == ESC || key == CTRL_C) finish(-1);
    return 1;
}

void choice_cancel(void) {
    if (choice.active) finish(-1);
}
