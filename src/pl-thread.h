/*  $Id$

    Part of SWI-Prolog

    Author:        Jan Wielemaker
    E-mail:        J.Wielemaker@cs.vu.nl
    WWW:           http://www.swi-prolog.org
    Copyright (C): 1985-2009, University of Amsterdam

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Lesser General Public
    License as published by the Free Software Foundation; either
    version 2.1 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public
    License along with this library; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#ifndef PL_THREAD_H_DEFINED
#define PL_THREAD_H_DEFINED

#ifdef O_PLMT
#include <pthread.h>

#ifdef HAVE_SEMA_INIT
#include <synch.h>
#endif

#ifndef __WINDOWS__
#define SIG_FORALL SIGUSR1
#define SIG_RESUME SIG_FORALL		/* these can be shared */

#define SIG_ALERT  SIGUSR2
#endif

typedef enum
{ LDATA_IDLE = 0,
  LDATA_SIGNALLED,
  LDATA_ANSWERING,
  LDATA_ANSWERED
} ldata_status_t;

#ifdef __WINDOWS__
enum
{ SIGNAL     = 0,
  BROADCAST  = 1,
  MAX_EVENTS = 2
} win32_event_t;

typedef struct
{ HANDLE events[MAX_EVENTS];		/* events to be signalled */
  int    waiters;			/* # waiters */
} win32_cond_t;
#endif

typedef struct _PL_thread_info_t
{ int		    pl_tid;		/* Prolog thread id */
  size_t	    local_size;		/* Stack sizes */
  size_t	    global_size;
  size_t	    trail_size;
  size_t	    stack_size;		/* system (C-) stack */
  int		    (*cancel)(int id);	/* cancel function */
  int		    open_count;		/* for PL_thread_detach_engine() */
  bool		    detached;		/* detached thread */
  int		    status;		/* PL_THREAD_* */
  pthread_t	    tid;		/* Thread identifier */
  int		    has_tid;		/* TRUE: tid = valid */
#ifdef __linux__
  pid_t		    pid;		/* for identifying */
#endif
#ifdef __WINDOWS__
  unsigned long	    w32id;		/* Win32 thread HANDLE */
#endif
  struct PL_local_data  *thread_data;	/* The thread-local data  */
  module_t	    module;		/* Module for starting goal */
  record_t	    goal;		/* Goal to start thread */
  record_t	    return_value;	/* Value (term) returned */
  atom_t	    name;		/* Name of the thread */
  ldata_status_t    ldata_status;	/* status of forThreadLocalData() */
} PL_thread_info_t;

#define QTYPE_THREAD	0
#define QTYPE_QUEUE	1

typedef struct message_queue
{ simpleMutex	       mutex;		/* Message queue mutex */
#ifdef __WINDOWS__
  win32_cond_t	       cond_var;
  win32_cond_t	       drain_var;
#else
  pthread_cond_t       cond_var;	/* condvar for reading */
  pthread_cond_t       drain_var;	/* condvar for writing */
#endif
  struct thread_message   *head;		/* Head of message queue */
  struct thread_message   *tail;		/* Tail of message queue */
  uint64_t	       sequence_next;	/* next for sequence id */
  word		       id;		/* Id of the queue */
  long		       size;		/* # terms in queue */
  long		       max_size;	/* Max # terms in queue */
  int		       waiting;		/* # waiting threads */
  int		       waiting_var;	/* # waiting with unbound */
  int		       wait_for_drain;	/* # threads waiting for write */
  unsigned	destroyed : 1;		/* Thread is being destroyed */
  unsigned	type : 2;		/* QTYPE_* */
} message_queue;

typedef struct pl_mutex
{ pthread_mutex_t mutex;		/* the system mutex */
  int count;				/* lock count */
  int owner;				/* integer id of owner */
  word id;				/* id of the mutex */
} pl_mutex;

#define PL_THREAD_MAGIC 0x2737234f

#define PL_THREAD_UNUSED	0	/* no thread on this slot */
#define PL_THREAD_RUNNING	1	/* a normally running one */
#define PL_THREAD_EXITED	2	/* died with thread_exit/1 */
#define PL_THREAD_SUCCEEDED	3	/* finished with Yes */
#define PL_THREAD_FAILED	4	/* finished with No */
#define PL_THREAD_EXCEPTION	5	/* finished with exception */
#define PL_THREAD_NOMEM		6	/* couldn't start due no-memory */
#define	PL_THREAD_CREATED	7	/* just created */
#define	PL_THREAD_SUSPENDED	8	/* suspended */
#define PL_THREAD_RESUMING      9	/* about to resume */

extern counting_mutex _PL_mutexes[];	/* Prolog mutexes */

#define L_MISC		0
#define L_ALLOC		1
#define L_ATOM		2
#define L_FLAG	        3
#define L_FUNCTOR	4
#define L_RECORD	5
#define L_THREAD	6
#define L_PREDICATE	7
#define L_MODULE	8
#define L_TABLE		9
#define L_BREAK	       10
#define L_FILE	       11
#define L_PLFLAG      12
#define L_OP	       13
#define L_INIT	       14
#define L_TERM	       15
#define L_GC	       16
#define L_AGC	       17
#define L_FOREIGN      18
#define L_OS	       19

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The IF_MT(id, g) macro  is  used  to   bypass  mutexes  if  threading  is
disabled. We cannot do this for the L_THREAD mutex however as we need to
control when threads can be created.

We  assume  id  ==  L_THREAD  is  optimized  away  if  id  is  known  at
compile-time
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define IF_MT(id, g) if ( id == L_THREAD || GD->thread.enabled ) g

#ifdef O_CONTENTION_STATISTICS
#define countingMutexLock(cm) \
	do \
	{ if ( pthread_mutex_trylock(&(cm)->mutex) == EBUSY ) \
	  { (cm)->collisions++; \
	    pthread_mutex_lock(&(cm)->mutex); \
	  } \
	  (cm)->count++; \
	} while(0)
#else
#define countingMutexLock(cm) \
	do \
	{ simpleMutexLock(&(cm)->mutex); \
	  (cm)->count++; \
	} while(0)
#endif
#define countingMutexUnlock(cm) \
	do \
	{ (cm)->unlocked++; \
	  assert((cm)->unlocked <= (cm)->count); \
	  simpleMutexUnlock(&(cm)->mutex); \
	} while(0)


#ifdef O_DEBUG_MT
#define PL_LOCK(id) \
	do { Sdprintf("[%s] %s:%d: LOCK(%s)\n", \
		      threadName(0), \
		      __BASE_FILE__, __LINE__, #id); \
             IF_MT(id, countingMutexLock(&_PL_mutexes[id])); \
	   } while(0)
#define PL_UNLOCK(id) \
	do { Sdprintf("[%s] %s:%d: UNLOCK(%s)\n", \
		      threadName(0), \
		      __BASE_FILE__, __LINE__, #id); \
	     IF_MT(id, countingMutexUnlock(&_PL_mutexes[id])); \
	   } while(0)
#else
#define PL_LOCK(id)   IF_MT(id, countingMutexLock(&_PL_mutexes[id]))
#define PL_UNLOCK(id) IF_MT(id, countingMutexUnlock(&_PL_mutexes[id]))
#endif

#define LOCKDEF(def) \
	if ( GD->thread.enabled ) \
	{ if ( def->mutex ) \
	  { countingMutexLock(def->mutex); \
	  } else if ( false(def, DYNAMIC) ) \
	  { countingMutexLock(&_PL_mutexes[L_PREDICATE]); \
	  } \
	}

#define UNLOCKDEF(def) \
	if ( GD->thread.enabled ) \
	{ if ( def->mutex ) \
	  { countingMutexUnlock(def->mutex); \
	  } else if ( false(def, DYNAMIC) ) \
	  { countingMutexUnlock(&_PL_mutexes[L_PREDICATE]); \
	  } \
	}

#define LOCKDYNDEF(def) \
	if ( GD->thread.enabled && def->mutex ) countingMutexLock(def->mutex)
#define UNLOCKDYNDEF(def) \
	if ( GD->thread.enabled && def->mutex ) countingMutexUnlock(def->mutex)

#define LOCKMODULE(module)   countingMutexLock((module)->mutex)
#define UNLOCKMODULE(module) countingMutexUnlock((module)->mutex)


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
				Thread-local data

All  thread-local  data  is  combined  in    one  structure  defined  in
pl-global.h. If Prolog is compiled for single-threading this is a simple
global variable and the macro LD is defined   to  pick up the address of
this variable. In multithreaded context,  POSIX pthread_getspecific() is
used to get separate versions for each  thread. Functions uisng LD often
may wish to write:

<header>
{ GET_LD
#undef LD
#define LD LOCAL_LD
  ...

#undef LD
#define LD GLOBAL_LD
}
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

		 /*******************************
		 *   NATIVE THREAD-LOCAL DATA	*
		 *******************************/

#ifdef __WINDOWS__
typedef DWORD	TLD_KEY;

#define TLD_alloc(p)	(*(p) = TlsAlloc())
#define TLD_get(p)	TlsGetValue((p))
#define TLD_set(p, v)	TlsSetValue((p), (v))
#define TLD_free(p)	TlsFree(p);

#else
typedef pthread_key_t TLD_KEY;

#define TLD_alloc(p)	pthread_key_create(p, NULL)
#define TLD_get(p)	pthread_getspecific(p)
#define TLD_set(p, v)	pthread_setspecific((p), (v))
#define TLD_free(p)	pthread_key_delete(p)
#endif

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
If available, use GCC's __attribute((const)) to tell the compiler it may
choose to store the result of LD is a local variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

extern TLD_KEY PL_ldata;		/* key to local data */

#define GLOBAL_LD ((PL_local_data_t *)TLD_get(PL_ldata))


		 /*******************************
		 *	       WINDOWS		*
		 *******************************/

#define WM_SIGNALLED (WM_USER+4201)	/* how to select a good number!? */


		 /*******************************
		 *	    FUNCTIONS		*
		 *******************************/

extern int		exitPrologThreads(void);
extern bool		aliasThread(int tid, atom_t name);
extern word		pl_thread_create(term_t goal, term_t id,
					 term_t options);
extern word		pl_thread_join(term_t thread, term_t retcode);
extern word		pl_thread_exit(term_t retcode);
extern foreign_t	pl_thread_signal(term_t thread, term_t goal);

extern foreign_t	pl_thread_at_exit(term_t goal);
extern int		PL_thread_self(void);

extern foreign_t	pl_mutex_destroy(term_t mutex);
extern foreign_t	pl_mutex_lock(term_t mutex);
extern foreign_t	pl_mutex_trylock(term_t mutex);
extern foreign_t	pl_mutex_unlock(term_t mutex);
extern foreign_t	pl_mutex_unlock_all(void);

const char *		threadName(int id);
void			executeThreadSignals(int sig);
foreign_t		pl_attach_xterm(term_t in, term_t out);
size_t			threadLocalHeapUsed(void);
int			attachConsole(void);
Definition		localiseDefinition(Definition def);
LocalDefinitions	new_ldef_vector(void);
int			PL_mutex_lock(struct pl_mutex *m);
int			PL_mutex_unlock(struct pl_mutex *m);
int			PL_thread_raise(int tid, int sig);
void			cleanupThreads();
intptr_t			system_thread_id(PL_thread_info_t *info);

		 /*******************************
		 *	 GLOBAL GC SUPPORT	*
		 *******************************/

void		forThreadLocalData(void (*func)(struct PL_local_data *),
				   unsigned flags);
void		resumeThreads(void);
void		markAtomsThreads(void);

#define PL_THREAD_SUSPEND_AFTER_WORK	0x1 /* forThreadLocalData() */

#else /*O_PLMT, end of threading-stuff */

		 /*******************************
		 *	 NON-THREAD STUFF	*
		 *******************************/

#ifdef O_MULTIPLE_ENGINES

#define GLOBAL_LD PL_current_engine_ptr

#else /*O_MULTIPLE_ENGINES*/

#define GLOBAL_LD (&PL_local_data)

#endif /*O_MULTIPLE_ENGINES*/

#define PL_LOCK(id)
#define PL_UNLOCK(id)

#define LOCKDEF(def)
#define UNLOCKDEF(def)
#define LOCKDYNDEF(def)
#define UNLOCKDYNDEF(def)
#define LOCKMODULE(module)
#define UNLOCKMODULE(module)

#endif /*O_PLMT*/

		 /*******************************
		 *	       COMMON		*
		 *******************************/

extern void		initPrologThreads(void);

#endif /*PL_THREAD_H_DEFINED*/
