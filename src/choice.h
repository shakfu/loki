/* choice.h - A one-key choice on the message line */

#ifndef LOKI_CHOICE_H
#define LOKI_CHOICE_H

#define CHOICE_MAX 9

/* index is 0-based, or -1 when cancelled. */
typedef void (*choice_fn)(int index, void *ud);

/* Show question with numbered labels until a key answers. Returns -1 if a
 * choice is already pending or count is not 1..CHOICE_MAX. */
int choice_begin(const char *question, const char *const *labels, int count,
                 choice_fn fn, void *ud);

/* The text to show while a choice is pending, else NULL. */
const char *choice_prompt(void);

/* Answer or swallow key if a choice is pending; returns 1 if consumed. */
int choice_handle_key(int key);

/* Cancel a pending choice (its callback gets -1). */
void choice_cancel(void);

#endif /* LOKI_CHOICE_H */
