#include <ros/arch/membar.h>
#include <parlib.h>
#include <vcore.h>
#include <uthread.h>
#include <event.h>

/* Which operations we'll call for the 2LS.  Will change a bit with Lithe.  For
 * now, there are no defaults.  2LSs can override sched_ops. */
struct schedule_ops default_2ls_ops = {0};
struct schedule_ops *sched_ops __attribute__((weak)) = &default_2ls_ops;

__thread struct uthread *current_uthread = 0;

/* static helpers: */
static int __uthread_allocate_tls(struct uthread *uthread);
static void __uthread_free_tls(struct uthread *uthread);

/* Gets called once out of uthread_create().  Can also do this in a ctor. */
static int uthread_init(void)
{
	/* Init the vcore system */
	assert(!vcore_init());
	/* Bug if vcore init was called with no 2LS */
	assert(sched_ops->sched_init);
	/* Get thread 0's thread struct (2LS allocs it) */
	struct uthread *uthread = sched_ops->sched_init();
	/* Save a pointer to thread0's tls region (the glibc one) into its tcb */
	uthread->tls_desc = get_tls_desc(0);
	/* Save a pointer to the uthread in its own TLS */
	current_uthread = uthread;
	/* Change temporarily to vcore0s tls region so we can save the newly created
	 * tcb into its current_uthread variable and then restore it.  One minor
	 * issue is that vcore0's transition-TLS isn't TLS_INITed yet.  Until it is
	 * (right before vcore_entry(), don't try and take the address of any of
	 * its TLS vars. */
	extern void** vcore_thread_control_blocks;
	set_tls_desc(vcore_thread_control_blocks[0], 0);
	current_uthread = uthread;
	set_tls_desc(uthread->tls_desc, 0);
	assert(!in_vcore_context());
	/* don't forget to enable notifs on vcore0.  if you don't, the kernel will
	 * restart your _S with notifs disabled, which is a path to confusion. */
	enable_notifs(0);
	/* Get ourselves into _M mode */
	while (num_vcores() < 1) {
		vcore_request(1);
		/* TODO: consider blocking */
		cpu_relax();
	}
	return 0;
}

/* 2LSs shouldn't call uthread_vcore_entry directly */
void __attribute__((noreturn)) uthread_vcore_entry(void)
{
	uint32_t vcoreid = vcore_id();

	struct preempt_data *vcpd = &__procdata.vcore_preempt_data[vcoreid];

	/* Should always have notifications disabled when coming in here. */
	assert(vcpd->notif_enabled == FALSE);
	assert(in_vcore_context());

	check_preempt_pending(vcoreid);
	handle_events(vcoreid);
	assert(in_vcore_context());	/* double check, in case and event changed it */
	assert(sched_ops->sched_entry);
	sched_ops->sched_entry();
	/* 2LS sched_entry should never return */
	assert(0);
}

/* Creates a uthread.  Will pass udata to sched_ops's thread_create.  For now,
 * the vcore/default 2ls code handles start routines and args.  Mostly because
 * this is used when initing a utf, which is vcore specific for now. */
struct uthread *uthread_create(void (*func)(void), void *udata)
{
	/* First time through, init the uthread code (which makes a uthread out of
	 * thread0 / the current code.  Could move this to a ctor. */
	static bool first = TRUE;
	if (first) {
		assert(!uthread_init());
		first = FALSE;
	}
	assert(!in_vcore_context());
	assert(sched_ops->thread_create);
	struct uthread *new_thread = sched_ops->thread_create(func, udata);
	/* Get a TLS */
	assert(!__uthread_allocate_tls(new_thread));
	/* Switch into the new guys TLS and let it know who it is */
	struct uthread *caller = current_uthread;
	assert(caller);
	/* Don't migrate this thread to another vcore, since it depends on being on
	 * the same vcore throughout. */
	caller->dont_migrate = TRUE;
	wmb();
	/* Note the first time we call this, we technically aren't on a vcore */
	uint32_t vcoreid = vcore_id();
	/* Save the new_thread to the new uthread in that uthread's TLS */
	set_tls_desc(new_thread->tls_desc, vcoreid);
	current_uthread = new_thread;
	/* Switch back to the caller */
	set_tls_desc(caller->tls_desc, vcoreid);
	/* Okay to migrate now. */
	wmb();
	caller->dont_migrate = FALSE;
	return new_thread;
}

void uthread_runnable(struct uthread *uthread)
{
	/* Allow the 2LS to make the thread runnable, and do whatever. */
	assert(sched_ops->thread_runnable);
	sched_ops->thread_runnable(uthread);
	/* Ask the 2LS how many vcores it wants, and put in the request. */
	assert(sched_ops->vcores_wanted);
	vcore_request(sched_ops->vcores_wanted());
}

/* Need to have this as a separate, non-inlined function since we clobber the
 * stack pointer before calling it, and don't want the compiler to play games
 * with my hart.
 *
 * TODO: combine this 2-step logic with uthread_exit() */
static void __attribute__((noinline, noreturn)) 
__uthread_yield(struct uthread *uthread)
{
	assert(in_vcore_context());
	/* TODO: want to set this to FALSE once we no longer depend on being on this
	 * vcore.  Though if we are using TLS, we are depending on the vcore.  Since
	 * notifs are disabled and we are in a transition context, we probably
	 * shouldn't be moved anyway.  It does mean that a pthread could get jammed.
	 * If we do this after putting it on the active list, we'll have a race on
	 * dont_migrate. */
	uthread->dont_migrate = FALSE;
	assert(sched_ops->thread_yield);
	/* 2LS will save the thread somewhere for restarting.  Later on, we'll
	 * probably have a generic function for all sorts of waiting. */
	sched_ops->thread_yield(uthread);
	/* Leave the current vcore completely */
	current_uthread = NULL; // this might be okay, even with a migration
	/* Go back to the entry point, where we can handle notifications or
	 * reschedule someone. */
	uthread_vcore_entry();
}

/* Calling thread yields.  TODO: combine similar code with uthread_exit() (done
 * like this to ease the transition to the 2LS-ops */
void uthread_yield(void)
{
	struct uthread *uthread = current_uthread;
	volatile bool yielding = TRUE; /* signal to short circuit when restarting */
	/* TODO: (HSS) Save silly state */
	// save_fp_state(&t->as);
	assert(!in_vcore_context());
	/* Don't migrate this thread to another vcore, since it depends on being on
	 * the same vcore throughout (once it disables notifs). */
	uthread->dont_migrate = TRUE;
	wmb();
	uint32_t vcoreid = vcore_id();
	printd("[U] Uthread %08p is yielding on vcore %d\n", uthread, vcoreid);
	struct preempt_data *vcpd = &__procdata.vcore_preempt_data[vcoreid];
	/* once we do this, we might miss a notif_pending, so we need to enter vcore
	 * entry later.  Need to disable notifs so we don't get in weird loops with
	 * save_ros_tf() and pop_ros_tf(). */
	disable_notifs(vcoreid);
	/* take the current state and save it into t->utf when this pthread
	 * restarts, it will continue from right after this, see yielding is false,
	 * and short ciruit the function. */
	save_ros_tf(&uthread->utf);
	if (!yielding)
		goto yield_return_path;
	yielding = FALSE; /* for when it starts back up */
	/* Change to the transition context (both TLS and stack). */
	extern void** vcore_thread_control_blocks;
	set_tls_desc(vcore_thread_control_blocks[vcoreid], vcoreid);
	assert(current_uthread == uthread);	
	assert(in_vcore_context());	/* technically, we aren't fully in vcore context */
	/* After this, make sure you don't use local variables.  Note the warning in
	 * pthread_exit() */
	set_stack_pointer((void*)vcpd->transition_stack);
	/* Finish exiting in another function. */
	__uthread_yield(current_uthread);
	/* Should never get here */
	assert(0);
	/* Will jump here when the pthread's trapframe is restarted/popped. */
yield_return_path:
	printd("[U] Uthread %08p returning from a yield!\n", uthread);
}

/* Need to have this as a separate, non-inlined function since we clobber the
 * stack pointer before calling it, and don't want the compiler to play games
 * with my hart. */
static void __attribute__((noinline, noreturn)) 
__uthread_exit(struct uthread *uthread)
{
	assert(in_vcore_context());
	/* we alloc and manage the TLS, so lets get rid of it */
	__uthread_free_tls(uthread);
	/* 2LS specific cleanup */
	assert(sched_ops->thread_exit);
	sched_ops->thread_exit(uthread);
	current_uthread = NULL;
	/* Go back to the entry point, where we can handle notifications or
	 * reschedule someone. */
	uthread_vcore_entry();
}

/* Exits from the uthread */
void uthread_exit(void)
{
	assert(!in_vcore_context());
	struct uthread *uthread = current_uthread;
	/* Don't migrate this thread to anothe vcore, since it depends on being on
	 * the same vcore throughout. */
	uthread->dont_migrate = TRUE; // won't set to false later, since he is dying
	wmb();
	uint32_t vcoreid = vcore_id();
	struct preempt_data *vcpd = &__procdata.vcore_preempt_data[vcoreid];
	printd("[U] Uthread %08p is exiting on vcore %d\n", uthread, vcoreid);
	/* once we do this, we might miss a notif_pending, so we need to enter vcore
	 * entry later. */
	disable_notifs(vcoreid);
	/* Change to the transition context (both TLS and stack). */
	extern void** vcore_thread_control_blocks;
	set_tls_desc(vcore_thread_control_blocks[vcoreid], vcoreid);
	assert(current_uthread == uthread);	
	/* After this, make sure you don't use local variables.  Also, make sure the
	 * compiler doesn't use them without telling you (TODO).
	 *
	 * In each arch's set_stack_pointer, make sure you subtract off as much room
	 * as you need to any local vars that might be pushed before calling the
	 * next function, or for whatever other reason the compiler/hardware might
	 * walk up the stack a bit when calling a noreturn function. */
	set_stack_pointer((void*)vcpd->transition_stack);
	/* Finish exiting in another function.  Ugh. */
	__uthread_exit(current_uthread);
}

/* Runs whatever thread is vcore's current_uthread */
void run_current_uthread(void)
{
	uint32_t vcoreid = vcore_id();
	struct preempt_data *vcpd = &__procdata.vcore_preempt_data[vcoreid];
	assert(current_uthread);
	printd("[U] Vcore %d is restarting uthread %d\n", vcoreid, uthread->id);
	clear_notif_pending(vcoreid);
	set_tls_desc(current_uthread->tls_desc, vcoreid);
	/* Pop the user trap frame */
	pop_ros_tf(&vcpd->notif_tf, vcoreid);
	assert(0);
}

/* Launches the uthread on the vcore.  Don't call this on current_uthread. */
void run_uthread(struct uthread *uthread)
{
	assert(uthread != current_uthread);
	/* Save a ptr to the pthread running in the transition context's TLS */
	uint32_t vcoreid = vcore_id();
	struct preempt_data *vcpd = &__procdata.vcore_preempt_data[vcoreid];
	printd("[U] Vcore %d is starting uthread %d\n", vcoreid, uthread->id);
	current_uthread = uthread;
	clear_notif_pending(vcoreid);
	set_tls_desc(uthread->tls_desc, vcoreid);
	/* Load silly state (Floating point) too.  For real */
	/* TODO: (HSS) */
	/* Pop the user trap frame */
	pop_ros_tf(&uthread->utf, vcoreid);
	assert(0);
}

/* Deals with a pending preemption (checks, responds).  If the 2LS registered a
 * function, it will get run.  Returns true if you got preempted.  Called
 * 'check' instead of 'handle', since this isn't an event handler.  It's the "Oh
 * shit a preempt is on its way ASAP".  While it is isn't too involved with
 * uthreads, it is tied in to sched_ops. */
bool check_preempt_pending(uint32_t vcoreid)
{
	bool retval = FALSE;
	if (__procinfo.vcoremap[vcoreid].preempt_pending) {
		retval = TRUE;
		if (sched_ops->preempt_pending)
			sched_ops->preempt_pending();
		/* this tries to yield, but will pop back up if this was a spurious
		 * preempt_pending. */
		sys_yield(TRUE);
	}
	return retval;
}

/* TLS helpers */
static int __uthread_allocate_tls(struct uthread *uthread)
{
	assert(!uthread->tls_desc);
	uthread->tls_desc = allocate_tls();
	if (!uthread->tls_desc) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

static void __uthread_free_tls(struct uthread *uthread)
{
	free_tls(uthread->tls_desc);
	uthread->tls_desc = NULL;
}