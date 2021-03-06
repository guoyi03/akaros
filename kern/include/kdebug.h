#ifndef ROS_KERN_KDEBUG_H
#define ROS_KERN_KDEBUG_H

#include <ros/common.h>
#include <arch/kdebug.h>

struct symtab_entry {
	char *name;
	uintptr_t addr;
};

void backtrace(void);
void backtrace_frame(uintptr_t pc, uintptr_t fp);

/* Arch dependent, listed here for ease-of-use */
static inline uintptr_t get_caller_pc(void);

/* Returns a null-terminated string with the function name for a given PC /
 * instruction pointer.  kfree() the result. */
char *get_fn_name(uintptr_t pc);

/* Returns the address of sym, or 0 if it does not exist */
uintptr_t get_symbol_addr(char *sym);

#endif /* ROS_KERN_KDEBUG_H */
