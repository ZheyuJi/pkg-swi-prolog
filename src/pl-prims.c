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

/*#define O_DEBUG 1*/
#include "pl-incl.h"
#include "os/pl-ctype.h"
#include "pl-inline.h"
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

#undef LD
#define LD LOCAL_LD

static bool	unify_with_occurs_check(Word t1, Word t2,
					occurs_check_t mode ARG_LD);


		 /*******************************
		 *	   CYCLIC TERMS		*
		 *******************************/

#if O_CYCLIC

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Cyclic term unification. The algorithm has been  described to me by Bart
Demoen. Here it is (translated from dutch):

I created my own variation. You only need it during general unification.
Here is a short description:  suppose  you   unify  2  terms  f(...) and
f(...), which are represented on the heap (=global stack) as:

     +-----+          and     +-----+
     | f/3 |                  | f/3 |
     +-----+                  +-----+
      args                     args'

Before working on args and args', change  this into the structure below,
using a reference pointer pointing from functor  of the one to the other
term.

     +-----+          and      +-----+
     | ----+----------------->| f/3 |
     +-----+                  +-----+
      args                     args'

If, during this unification you  find  a   compound  whose  functor is a
reference to the term at the right hand you know you hit a cycle and the
terms are the same.

Of course functor_t must be different from ref. Overwritten functors are
collected in a stack and  reset   regardless  of whether the unification
succeeded or failed.

Note that we need to  dereference  the   functors  both  left and right.
References at the right are rare, but possible. The trick is to use both
sharing and cycles, where the cycles at the left are shorter:

t :-
	X = s(X),       Y = y(X,X),
	A = s(s(s(A))), B = y(A,A),
	Y = B.

While unifying the first argument of y/2, the left-walker crosses to the
right after the first cycle  and  creates   references  in  A, which are
processed by the right-walker when entering the second argumet of y/2.

Initial measurements show a performance degradation for deep unification
of approx. 30%. On the other hand,  if subterms appear multiple times in
a term unification can be much faster. As only a small percentage of the
unifications of a realistic program are   covered by unify() and involve
deep unification the overall impact of performance is small (< 3%).
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static void
initvisited(ARG1_LD)
{ LD->cycle.vstack.unit_size = sizeof(Word);
}


static int
empty_visited(ARG1_LD)
{ return LD->cycle.vstack.count == 0;
}


static inline int
visitedWord(Word p ARG_LD)
{ if ( is_marked(p) )
    succeed;
  set_marked(p);
  pushSegStack(&LD->cycle.vstack, &p);
  fail;
}


static inline int
visited(Functor f ARG_LD)
{ Word p = &f->definition;

  return visitedWord(p PASS_LD);
}


static void
unvisit(ARG1_LD)
{ Word p;

  while( popSegStack(&LD->cycle.vstack, &p) )
  { clear_marked(p);
  }
}


static void
popVisited(ARG1_LD)
{ Word p;

  popSegStack(&LD->cycle.vstack, &p);
  clear_marked(p);
}


static inline void
initCyclic(ARG1_LD)
{ LD->cycle.lstack.unit_size = sizeof(Word);
}


static inline void
linkTermsCyclic(Functor f1, Functor f2 ARG_LD)
{ Word p1 = (Word)&f1->definition;
  Word p2 = (Word)&f2->definition;

  *p1 = makeRefG(p2);
  pushSegStack(&LD->cycle.lstack, &p1);
}


static inline void
exitCyclic(ARG1_LD)
{ Word p;

  while( popSegStack(&LD->cycle.lstack, &p) )
  { *p = *unRef(*p);
  }
}

#else /*O_CYCLIC*/

static inline visited(Functor f ARG_LD) { fail; }
static inline unvisit(Word *base ARG_LD) { }
static inline void initCyclic(ARG1_LD) {}
static inline void exitCyclic(ARG1_LD) {}
static inline void linkTermsCyclic(Functor f1, Functor f2 ARG_LD) {}

#endif /*O_CYCLIC*/

#define HAVE_VISITED
#define AC_TERM_WALK_LR 1
#define AC_TERM_WALK_LRS 1
#include "pl-termwalk.c"

		/********************************
		*          UNIFICATION          *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Unify is the general unification procedure. This raw routine should only
be called by interpret as it  does   not  undo  bindings made during the
unification in case the unification fails. pl_unify() (implementing =/2)
does undo bindings and should be used   by  foreign predicates. See also
unify_ptrs().

Unification depends on the datatypes available in the system and will in
general need updating if new types are added.  It should be  noted  that
unify()  is  not  the only place were unification happens.  Other points
are:

  - various of the virtual machine instructions
  - various macros, for example APPENDLIST and CLOSELIST
  - unifyAtomic(): unification of atomic data.
  - various builtin predicates. They should be flagged some way.

Returns one of:

  - FALSE:		terms cannot unify.  Note that this routine does not
			rollback changes it made!
  - TRUE:		Unification has completed sucessfully
  - GLOBAL_OVERFLOW:	Unification cannot be completed due to lack
			of global-space.
  - TRAIL_OVERFLOW:	Unification cannot be completed due to lack
			of trail-space.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
do_unify(Word t1, Word t2 ARG_LD)
{
  word w1;
  word w2;

right_recursion:
  w1 = *t1;
  w2 = *t2;

  while(isRef(w1))			/* this is deRef() */
  { t1 = unRef(w1);
    w1 = *t1;
  }
  while(isRef(w2))
  { t2 = unRef(w2);
    w2 = *t2;
  }

  SECURE(assert(w1 != ATOM_garbage_collected);
	 assert(w2 != ATOM_garbage_collected));

  if ( isVar(w1) )
  { if ( unlikely(tTop+1 >= tMax) )
      return TRAIL_OVERFLOW;

    if ( isVar(w2) )
    { if ( t1 < t2 )			/* always point downwards */
      { Trail(t2, makeRef(t1));
	return TRUE;
      }
      if ( t1 == t2 )
	succeed;
      Trail(t1, makeRef(t2));
      return TRUE;
    }
#ifdef O_ATTVAR
    if ( isAttVar(w2 ) )
      w2 = makeRef(t2);
#endif
    Trail(t1, w2);
    return TRUE;
  }
  if ( isVar(w2) )
  { if ( unlikely(tTop+1 >= tMax) )
      return TRAIL_OVERFLOW;
#ifdef O_ATTVAR
    if ( isAttVar(w1) )
      w1 = makeRef(t1);
#endif
    Trail(t2, w1);
    return TRUE;
  }

#ifdef O_ATTVAR
  if ( isAttVar(w1) )
  { if ( !hasGlobalSpace(0) )
      return overflowCode(0);
    assignAttVar(t1, t2 PASS_LD);
    return TRUE;
  }
  if ( isAttVar(w2) )
  { if ( !hasGlobalSpace(0) )
      return overflowCode(0);
    assignAttVar(t2, t1 PASS_LD);
    return TRUE;
  }
#endif

  if ( w1 == w2 )
    succeed;
  if ( tag(w1) != tag(w2) )
    fail;

  switch(tag(w1))
  { case TAG_ATOM:
      fail;
    case TAG_INTEGER:
      if ( storage(w1) == STG_INLINE ||
	   storage(w2) == STG_INLINE )
	fail;
    case TAG_STRING:
    case TAG_FLOAT:
      return equalIndirect(w1, w2);
    case TAG_COMPOUND:
    { Functor f1 = valueTerm(w1);
      Functor f2 = valueTerm(w2);
      Word e;

#if O_CYCLIC
      while ( isRef(f1->definition) )
	f1 = (Functor)unRef(f1->definition);
      while ( isRef(f2->definition) )
	f2 = (Functor)unRef(f2->definition);
      if ( f1 == f2 )
	succeed;
#endif

      if ( f1->definition != f2->definition )
	fail;

      t1 = f1->arguments;
      t2 = f2->arguments;
      e  = t1+arityFunctor(f1->definition)-1; /* right-recurse on last */
      linkTermsCyclic(f1, f2 PASS_LD);

      for(; t1 < e; t1++, t2++)
      { int rc;

	if ( (rc=do_unify(t1, t2 PASS_LD)) <= 0 )
	  return rc;
      }
      goto right_recursion;
    }
  }

  succeed;
}


static int
raw_unify_ptrs(Word t1, Word t2 ARG_LD)
{ switch(LD->prolog_flag.occurs_check)
  { case OCCURS_CHECK_FALSE:
    { int rc;

      initCyclic(PASS_LD1);
      rc = do_unify(t1, t2 PASS_LD);
      exitCyclic(PASS_LD1);

      return rc;
    }
    case OCCURS_CHECK_TRUE:
      return unify_with_occurs_check(t1, t2, OCCURS_CHECK_TRUE PASS_LD);
    case OCCURS_CHECK_ERROR:
      return unify_with_occurs_check(t1, t2, OCCURS_CHECK_ERROR PASS_LD);
    default:
      assert(0);
      fail;
  }
}


static
PRED_IMPL("=", 2, unify, 0)
{ PRED_LD

  return PL_unify(A1, A2);
}


static
PRED_IMPL("\\=", 2, not_unify, 0)
{ PRED_LD
  Word p0 = valTermRef(A1);
  term_t ex = 0;

  if ( can_unify(p0, p0+1, &ex) )
    return FALSE;
  if ( ex )
    return PL_raise_exception(ex);
  return TRUE;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Public unification procedure for `raw' data.   See also PL_unify(). Note
that we can have an exception due   to occurs-check errors. In this case
we must undo carefully to preserve the exception term.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

bool
unify_ptrs(Word t1, Word t2, int flags ARG_LD)
{ for(;;)
  { int rc;

    rc = raw_unify_ptrs(t1, t2 PASS_LD);
    if ( rc >= 0 )
    { return rc;
    } else				/* Stack overflow */
    { int rc2;

      PushPtr(t1); PushPtr(t2);
      rc2 = makeMoreStackSpace(rc, flags);
      PopPtr(t2); PopPtr(t1);
      if ( !rc2 )
	return FALSE;
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
can_unify(t1, t2, &ex, flags) succeeds if   two  terms *can* be unified,
without actually doing so.  This  is   basically  a  stripped version of
unify() above. See this function for  comments.   Note  that  we have to
execute delayed goals and these may raise an exception. If this happens,
ex is a reference to the exception term.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

bool
can_unify(Word t1, Word t2, term_t *ex)
{ GET_LD
  fid_t fid;

  if ( (fid = PL_open_foreign_frame()) )
  { if ( unify_ptrs(t1, t2, ALLOW_GC|ALLOW_SHIFT PASS_LD) &&
	 foreignWakeup(ex PASS_LD) )
    { PL_discard_foreign_frame(fid);
      return TRUE;
    }

    PL_discard_foreign_frame(fid);
  }

  return FALSE;
}


		 /*******************************
		 *	   OCCURS-CHECK		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
int var_occurs_in(Word v, Word t)
    Succeeds of the term `v' occurs in `t'.  v must be dereferenced on
    entry.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static bool
var_occurs_in(Word v, Word t)
{ GET_LD

right_recursion:
  deRef(t);
  if ( v == t )
    succeed;

  if ( isTerm(*t) )
  { Functor f = valueTerm(*t);
    int arity = arityFunctor(f->definition);

    t = f->arguments;
    for( ; --arity > 0; t++)
    { if ( var_occurs_in(v, t) )
	succeed;
    }
    goto right_recursion;
  }

  fail;
}


static int
failed_unify_with_occurs_check(Word t1, Word t2, occurs_check_t mode ARG_LD)
{ int rc;

  if ( mode == OCCURS_CHECK_TRUE )
    fail;

  blockGC(0 PASS_LD);
  rc = PL_error(NULL, 0, NULL, ERR_OCCURS_CHECK, t1, t2);
  unblockGC(0 PASS_LD);

  return rc;
}


static int
unify_with_occurs_check(Word t1, Word t2, occurs_check_t mode ARG_LD)
{ word w1;
  word w2;

right_recursion:
  w1 = *t1;
  w2 = *t2;

  while(isRef(w1))			/* this is deRef() */
  { t1 = unRef(w1);
    w1 = *t1;
  }
  while(isRef(w2))
  { t2 = unRef(w2);
    w2 = *t2;
  }

  if ( t1 == t2 )
    succeed;

  if ( isVar(w1) )
  { if ( tTop+1 > tMax )
      return TRAIL_OVERFLOW;

    if ( isVar(w2) )
    { if ( t1 < t2 )			/* always point downwards */
      { Trail(t2, makeRef(t1));
	return TRUE;
      }
      Trail(t1, makeRef(t2));
      return TRUE;
    }
    if ( onStack(global, t1) && var_occurs_in(t1, t2) )
      return failed_unify_with_occurs_check(t1, t2, mode PASS_LD);
#ifdef O_ATTVAR
    if ( isAttVar(w2) )
      w2 = makeRef(t2);
#endif
    Trail(t1, w2);
    return TRUE;
  }
  if ( isVar(w2) )
  { if ( tTop+1 > tMax )
      return TRAIL_OVERFLOW;

    if ( onStack(global, t2) && var_occurs_in(t2, t1) )
      return failed_unify_with_occurs_check(t2, t1, mode PASS_LD);

#ifdef O_ATTVAR
    if ( isAttVar(w1) )
      w1 = makeRef(t1);
#endif
    Trail(t2, w1);
    return TRUE;
  }

#ifdef O_ATTVAR
  if ( isAttVar(w1) )
  { if ( var_occurs_in(t1, t2) )
      return failed_unify_with_occurs_check(t1, t2, mode PASS_LD);
    if ( !hasGlobalSpace(0) )
      return GLOBAL_OVERFLOW;
    assignAttVar(t1, t2 PASS_LD);
    return TRUE;
  }
  if ( isAttVar(w2) )
  { if ( var_occurs_in(t2, t1) )
      return failed_unify_with_occurs_check(t2, t1, mode PASS_LD);
    if ( !hasGlobalSpace(0) )
      return GLOBAL_OVERFLOW;
    assignAttVar(t2, t1 PASS_LD);
    return TRUE;
  }
#endif

  if ( w1 == w2 )
    succeed;
  if ( tag(w1) != tag(w2) )
    fail;

  switch(tag(w1))
  { case TAG_ATOM:
      fail;
    case TAG_INTEGER:
      if ( storage(w1) == STG_INLINE ||
	   storage(w2) == STG_INLINE )
	fail;
    case TAG_STRING:
    case TAG_FLOAT:
      return equalIndirect(w1, w2);
    case TAG_COMPOUND:
    { int arity;
      Functor f1 = valueTerm(w1);
      Functor f2 = valueTerm(w2);

      if ( f1->definition != f2->definition )
	fail;

      arity = arityFunctor(f1->definition);
      t1 = f1->arguments;
      t2 = f2->arguments;

      for(; --arity > 0; t1++, t2++)
      { int rc;

	if ( (rc=unify_with_occurs_check(t1, t2, mode PASS_LD)) <= 0 )
	  return rc;
      }
      goto right_recursion;
    }
  }

  succeed;
}


static
PRED_IMPL("unify_with_occurs_check", 2, unify_with_occurs_check, 0)
{ PRED_LD

  for(;;)
  { mark m;
    int rc;
    Word p1 = valTermRef(A1);
    Word p2 = valTermRef(A2);

    Mark(m);
    rc = unify_with_occurs_check(p1, p2, OCCURS_CHECK_TRUE PASS_LD);
    if ( rc == TRUE )			/* Terms unified */
    { DiscardMark(m);
      return rc;
    } else if ( rc == FALSE )		/* Terms did not unify */
    { if ( !exception_term )		/* Check for occurs error */
	Undo(m);
      DiscardMark(m);
      return rc;
    } else				/* Stack overflow */
    { Undo(m);
      DiscardMark(m);
      if ( !makeMoreStackSpace(rc, ALLOW_GC|ALLOW_SHIFT) )
	return FALSE;
    }
  }
}


		/********************************
		*         TYPE CHECKING         *
		*********************************/

static
PRED_IMPL("nonvar", 1, nonvar, 0)
{ PRED_LD
  return PL_is_variable(A1) ? FALSE : TRUE;
}

static
PRED_IMPL("var", 1, var, 0)
{ PRED_LD
  return PL_is_variable(A1);
}

static
PRED_IMPL("integer", 1, integer, 0)
{ return PL_is_integer(A1);
}

static
PRED_IMPL("float", 1, float, 0)
{ return PL_is_float(A1);
}

static
PRED_IMPL("rational", 1, rational, 0)
{ return PL_is_rational(A1);
}


#if O_STRING
static
PRED_IMPL("string", 1, string, 0)
{ return PL_is_string(A1);
}
#endif /* O_STRING */

static
PRED_IMPL("number", 1, number, 0)
{ return PL_is_number(A1);
}

static
PRED_IMPL("atom", 1, atom, 0)
{ PRED_LD
  return PL_is_atom(A1);
}

static
PRED_IMPL("atomic", 1, atomic, 0)
{ PRED_LD
  return PL_is_atomic(A1);
}


		 /*******************************
		 *	       GROUND		*
		 *******************************/

typedef enum
{ ph_mark,
  ph_unmark
} phase;

static inline int
ph_visitedWord(Word p, phase ph ARG_LD)
{ switch(ph)
  { case ph_mark:
      if ( is_marked(p) )
	succeed;
      set_marked(p);
      break;
    case ph_unmark:
      if ( !is_marked(p) )
	succeed;
      clear_marked(p);
  }
  fail;
}

static inline int
ph_visited(Functor f, phase ph ARG_LD)
{ Word p = &f->definition;

  return ph_visitedWord(p, ph PASS_LD);
}

/* missing optimisations:

   i  recurse only when at least another nontrivial case is open

   ii programmed stack
*/

static int
ph_ground(Word p, phase ph ARG_LD) /* Phase 1 marking */
{ int arity;
  Functor f;

last:
  deRef(p);

  if ( canBind(*p) )		/* attributed variables are not ground */
    fail;
  if ( !isTerm(*p) )
    succeed;

  f = valueTerm(*p);
  arity = arityFunctor(f->definition);
  p = f->arguments;
  if ( ph_visited(f, ph PASS_LD) )	/* already been here, so it must be ground */
    succeed;

  for(; --arity > 0; p++)
  { if ( !ph_ground(p, ph PASS_LD) )
      fail;
  }

  goto last;
}


int
ground__LD(Word p ARG_LD)
{ int rc1, rc2;

  startCritical;
  rc1 = ph_ground(p, ph_mark PASS_LD);  /* mark functors */
  rc2 = ph_ground(p, ph_unmark PASS_LD);  /* unmark the very same functors */
  if ( !endCritical )
    return FALSE;
  assert(rc1 == rc2);
  return rc1;
}


int
PL_is_ground(term_t t)
{ GET_LD

  return ground__LD(valTermRef(t) PASS_LD);
}


static
PRED_IMPL("ground", 1, ground, 0)
{ PRED_LD

  return ground__LD(valTermRef(A1) PASS_LD);
}


static
PRED_IMPL("compound", 1, compound, 0)
{ return PL_is_compound(A1);
}


static
PRED_IMPL("callable", 1, callable, 0)
{ return PL_is_callable(A1);
}


		 /*******************************
		 *	     CYCLIC		*
		 *******************************/

static inline int
ph1_markedWord(Word p ARG_LD)
{ if ( is_marked(p) )
    succeed;
  fail;
}

static inline int
ph1_marked(Functor f ARG_LD)
{ Word p = &f->definition;

  return ph1_markedWord(p PASS_LD);
}

static inline int
ph1_visitedWord(Word p ARG_LD)
{ if ( is_marked(p) )
    succeed;
  set_marked(p);
  fail;
}

static inline int
ph1_visited(Functor f ARG_LD)
{ Word p = &f->definition;

  return ph1_visitedWord(p PASS_LD);
}

static inline int
ph1_firstvisitedWord(Word p ARG_LD)
{ if ( is_first(p) )
    succeed;
  set_first(p);
  fail;
}

static inline int
ph1_firstvisited(Functor f ARG_LD)
{ Word p = &f->definition;

  return ph1_firstvisitedWord(p PASS_LD);
}

static int
ph1_is_acyclic(Word firstp ARG_LD)
{ Functor f;
  int l = 0 ; /* number of last arguments to process */
  Word p;

  deRef(firstp);
  p = firstp;

  for(;;)
  { int arity, i;
    if ( !isTerm(*p) )
      break;
    f = valueTerm(*p);
    arity = arityFunctor(f->definition);
    p = f->arguments;
    if ( ph1_marked(f PASS_LD) ) /* already acyclic */
      break;
    if ( ph1_firstvisited(f PASS_LD) )	/* Got a cycle! */
      fail;

    for(i = 0; i < arity-1; i++)
    { if ( !ph1_is_acyclic((p+i) PASS_LD) )
	fail;
    }
    l++; /* remember to mark later */
    p = p + arity-1;
    deRef(p);
  }
  /* mark all last arguments as ph1_visited */

  if (l > 0) /* there are l structures to mark */
  { Word p = firstp;
    int arity;

    for(;;)
    { SECURE(assert(isTerm(*p)));
      f = valueTerm(*p);
      if (ph1_visited(f PASS_LD) )
	{assert(0);} /* Impossible: someone else did it */
      if (--l==0)
	break;
      arity = arityFunctor(f->definition);
      p = f-> arguments + arity -1; /* next last argument */
      deRef(p);
    }
  }
  succeed;
}


static inline int
ph2_avisitedWord(Word p ARG_LD)
{ if ( !is_first(p) )
    succeed;
  clear_both(p);
  fail;
}


static inline int
ph2_avisited(Functor f ARG_LD)
{ Word p = &f->definition;

  return ph2_avisitedWord(p PASS_LD);
}


static void
ph2_is_acyclic(Word p ARG_LD)
{ int arity, i;
  Functor f;

 top:
  deRef(p);
  if ( !isTerm(*p) )
    return;

  f = valueTerm(*p);
  arity = arityFunctor(f->definition);
  p = f->arguments;
  if ( ph2_avisited(f PASS_LD) )	/* Got a cycle! */
    return;

  for(i = 0; i < arity-1; i++)
    ph2_is_acyclic((p+i) PASS_LD);

  p = p+i;
  goto top;
}


static int
is_acyclic(Word p ARG_LD)
{ int rc1;

  startCritical;
  rc1 = ph1_is_acyclic(p PASS_LD);
  ph2_is_acyclic(p PASS_LD);
  if ( !endCritical )
    return FALSE;

  return rc1;
}


int
PL_is_acyclic(term_t t)
{ GET_LD

  return is_acyclic(valTermRef(t) PASS_LD);
}


static
PRED_IMPL("acyclic_term", 1, acyclic_term, 0)
{ PRED_LD

  return is_acyclic(valTermRef(A1) PASS_LD);
}


static
PRED_IMPL("cyclic_term", 1, cyclic_term, 0)
{ PRED_LD

  return is_acyclic(valTermRef(A1) PASS_LD) ? FALSE : TRUE;
}


		 /*******************************
		 *	     FACTORIZE		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Factorizing a term  based  on  the   internal  sharing.  This  takes the
following steps:

  1. scan_shared() walks the term and
     a. Set MARK_MASK on all visited terms and FIRST_MASK on those find
        twice.
     b. Create a list Var=Term for all terms found twice.
     c. Returns the count of places that must be shared.
  2. reverse_factor_pointers() walks through the created list, placing
     the functor in Var and creating a reference from the location of
     the original functor.
  3. link_shared() walks the term and
     a. If the functor is a reference, follow the reference-chain to
        find the functor.  Link the term into the reference-chain.
     b. If the functor is marked, unmark it.
  4. restore_shared_functors() finishes the job by following the
     variable-list and putting all functors from the variable back
     into the term and setting the variable to be a real variable.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
scan_shared(Word t, Word vart, size_t *count ARG_LD)
{ term_agenda agenda;
  size_t shared = 0;
  Word p;

  initTermAgenda(&agenda, t);
  while( (p=nextTermAgenda(&agenda)) )
  { if ( isTerm(*p) )
    { Functor f = valueTerm(*p);
      Word d = &f->definition;

      if ( is_marked(d) )
      { if ( !is_first(d) )
	{ Word v;

	  if ( !(v=allocGlobalNoShift(6)) )
	    return GLOBAL_OVERFLOW;

	  v[0] = FUNCTOR_dot2;
	  v[1] = consPtr(&v[3], TAG_COMPOUND|STG_GLOBAL);
	  v[2] = ATOM_nil;
	  v[3] = FUNCTOR_equals2;
	  v[4] = 0;			/* For now */
	  v[5] = consPtr(d, TAG_COMPOUND|STG_GLOBAL);

	  *vart = consPtr(&v[0], TAG_COMPOUND|STG_GLOBAL);
	  vart = &v[2];

	  set_first(d);
	}
	shared++;
      } else
      { int arity = arityFunctor(f->definition);

	pushWorkAgenda(&agenda, arity, f->arguments);
	set_marked(d);
      }
    }
  }
  clearTermAgenda(&agenda);
  *count = shared;

  return TRUE;
}


/* Needed to restore if we run out of stack
*/

static int
unscan_shared(Word t ARG_LD)
{ term_agenda agenda;
  Word p;

  initTermAgenda(&agenda, t);
  while( (p=nextTermAgenda(&agenda)) )
  { if ( isTerm(*p) )
    { Functor f = valueTerm(*p);
      Word d = &f->definition;

      if ( is_marked(d) )
      { int arity;

	clear_marked(d);
	clear_first(d);
	arity = arityFunctor(f->definition);
	pushWorkAgenda(&agenda, arity, f->arguments);
      }
    }
  }
  clearTermAgenda(&agenda);

  return TRUE;
}


static void
reverse_factor_pointers(Word vars ARG_LD)
{ while(*vars != ATOM_nil)
  { Word v = (Word)valueTerm(*vars);
    Functor t = valueTerm(v[5]);

    v[4] = t->definition & ~(MARK_MASK|FIRST_MASK); /* v[4] is the variable */
    t->definition = makeRefG(&v[4])|MARK_MASK|FIRST_MASK;

    vars = &v[2];
  }
}


static void
restore_shared_functors(Word vars ARG_LD)
{ while(*vars != ATOM_nil)
  { Word v = (Word)valueTerm(*vars);
    Functor t = valueTerm(v[5]);
    Word p = &v[4];

    deRef(p);
    t->definition = *p;
    setVar(*p);

    vars = &v[2];
  }
}


static int
link_shared(Word t, Word vars ARG_LD)
{ term_agenda agenda;
  Word p;

  initTermAgenda(&agenda, t);
  while( (p=nextTermAgenda(&agenda)) )
  { if ( isTerm(*p) )
    { Functor f = valueTerm(*p);
      Word d = &f->definition;

      if ( isRef(*d) )			/* shared term */
      { Word v;

	v = unRef(*d & ~(FIRST_MASK|MARK_MASK));
	deRef(v);

	if ( is_marked(d) )
	{ int arity = arityFunctor(*v);
	  pushWorkAgenda(&agenda, arity, f->arguments);
	}

	if ( v < p )
	{ TrailAssignment(p);
	  *p = makeRefG(v);
	} else
	{ TrailAssignment(p);
	  *p = *v;
	  *v = makeRefG(p);
	}
      } else if ( is_marked(d) )
      { int arity;
	word fun = f->definition & ~(FIRST_MASK|MARK_MASK);

	clear_marked(d);
	arity = arityFunctor(fun);
	pushWorkAgenda(&agenda, arity, f->arguments);
      }
    }
  }
  clearTermAgenda(&agenda);

  return TRUE;
}


static
PRED_IMPL("$factorize_term", 2, factorize_term, 0)
{ PRED_LD
  fid_t fid;
  term_t vars;
  Word t;
  size_t count;
  int rc;

  for(;;)
  { fid = PL_open_foreign_frame();

    vars = PL_new_term_ref();
    PL_put_nil(vars);
    t = valTermRef(A1);

    SECURE(checkStacks(NULL));
    startCritical;
    switch( (rc=scan_shared(t, valTermRef(vars), &count PASS_LD)) )
    { case TRUE:
	if ( tTop + 2*count > tMax )
	  rc = TRAIL_OVERFLOW;
        else if ( gTop + count > gMax )
	  rc = GLOBAL_OVERFLOW;
        else
	  break;
	/*FALLTHROUGH*/
      default:
	unscan_shared(t PASS_LD);
	PL_discard_foreign_frame(fid);
	if ( !endCritical ||
	     !makeMoreStackSpace(rc, ALLOW_GC|ALLOW_SHIFT) )
	  return FALSE;
	continue;
    }

    break;
  }

  reverse_factor_pointers(valTermRef(vars) PASS_LD);
  link_shared(t, valTermRef(vars) PASS_LD);
  restore_shared_functors(valTermRef(vars) PASS_LD);
  PL_close_foreign_frame(fid);
  SECURE(checkStacks(NULL));

  if ( !endCritical )
    return FALSE;

  return PL_unify(A2, vars);
}


		 /*******************************
		 *	 META-CALL SUPPORT	*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
deterministic(-Bool)

Bool = true if no choicepoint has been created in the current clause.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static
PRED_IMPL("deterministic", 1, deterministic, 0)
{ PRED_LD
  LocalFrame FR  = environment_frame->parent;
  Choice     BFR = LD->choicepoints;

  for( ; BFR; BFR = BFR->parent)
  { switch(BFR->type)
    { case CHP_CLAUSE:
	if ( BFR->frame == FR )
	  return PL_unify_atom(A1, ATOM_true);
      case CHP_JUMP:
	if ( (void *)BFR > (void *)FR )
	  return PL_unify_atom(A1, ATOM_false);
        else
	  return PL_unify_atom(A1, ATOM_true);
      default:
	continue;
    }
  }

  return PL_unify_atom(A1, ATOM_true);
}


#ifdef O_TERMHASH
		 /*******************************
		 *	    TERM-HASH		*
		 *******************************/

static bool
termHashValue(word term, long depth, unsigned int *hval ARG_LD)
{ for(;;)
  { switch(tag(term))
    { case TAG_VAR:
      case TAG_ATTVAR:
	fail;
      case TAG_ATOM:
      { *hval = MurmurHashAligned2(&atomValue(term)->hash_value,
				   sizeof(unsigned int), *hval);
        succeed;
      }
      case TAG_STRING:
      { size_t len;
	char *s;

	s = getCharsString(term, &len);
	*hval = MurmurHashAligned2(s, len, *hval);

        succeed;
      }
      case TAG_INTEGER:
	if ( storage(term) == STG_INLINE )
	{ int64_t v = valInt(term);

	  *hval = MurmurHashAligned2(&v, sizeof(v), *hval);

	  succeed;
	}
      /*FALLTHROUGH*/
      case TAG_FLOAT:
	{ Word p = addressIndirect(term);
	  size_t n = wsizeofInd(*p);

	  *hval = MurmurHashAligned2(p+1, n*sizeof(word), *hval);

	  succeed;
	}
      case TAG_COMPOUND:
      { Functor t = valueTerm(term);
	FunctorDef fd;
	int arity;
	Word p;
	unsigned int atom_hashvalue;

	if ( visited(t PASS_LD) )
	{ *hval = MurmurHashAligned2(hval, sizeof(*hval), *hval);
	  succeed;
	}

	fd = valueFunctor(t->definition);
	arity = fd->arity;

	atom_hashvalue = atomValue(fd->name)->hash_value + arity;
	*hval = MurmurHashAligned2(&atom_hashvalue,
				   sizeof(atom_hashvalue),
				   *hval);

	if ( --depth != 0 )
	{ for(p = t->arguments; arity-- > 0; p++)
	  { if ( !termHashValue(*p, depth, hval PASS_LD) )
	    { popVisited(PASS_LD1);
	      fail;
	    }
	  }
	}

	popVisited(PASS_LD1);
	succeed;
      }
      case TAG_REFERENCE:
      { term = *unRef(term);
	continue;
      }
      default:
	assert(0);
    }
  }
}


/* term_hash(+Term, +Depth, +Range, -HashKey) */

static
PRED_IMPL("term_hash", 4, term_hash4, 0)
{ PRED_LD
  Word p = valTermRef(A1);
  unsigned int hraw = MURMUR_SEED;
  long depth;
  int range;
  int rc = TRUE;

  if ( !PL_get_long_ex(A2, &depth) )
    fail;
  if ( depth < -1 )
    return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_not_less_than_zero, A2);

  if ( !PL_get_integer_ex(A3, &range) )
    fail;
  if ( range < 1 )
    return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_not_less_than_one, A2);

  if ( depth != 0 )
  { initvisited(PASS_LD1);
    rc = termHashValue(*p, depth, &hraw PASS_LD);
    assert(empty_visited(PASS_LD1));
  }

  if ( rc )
  { hraw = hraw % range;

    return PL_unify_integer(A4, hraw);
  }

  succeed;
}

#endif /*O_TERMHASH*/


		/********************************
		*        STANDARD ORDER         *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
There are atoms of different  type.   We  only define comparison between
atoms of the same type, except for mixed ISO Latin-1 and UCS atoms.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
compareAtoms(atom_t w1, atom_t w2)
{ Atom a1 = atomValue(w1);
  Atom a2 = atomValue(w2);

  if ( a1->type == a2->type )
  { if ( a1->type->compare )
    { return (*a1->type->compare)(w1, w2);
    } else
    { size_t l = (a1->length <= a2->length ? a1->length : a2->length);
      int v;

      if ( (v=memcmp(a1->name, a2->name, l)) != 0 )
	return v < 0 ? CMP_LESS : CMP_GREATER;
      return a1->length == a2->length ? CMP_EQUAL :
	     a1->length < a2->length ? CMP_LESS : CMP_GREATER;
    }
  } else if ( true(a1->type, PL_BLOB_TEXT) &&
	      true(a2->type, PL_BLOB_TEXT) )
  { PL_chars_t t1, t2;
    size_t len;

    get_atom_text(w1, &t1);
    get_atom_text(w2, &t2);
    len = t1.length > t2.length ? t1.length : t2.length;

    return PL_cmp_text(&t1, 0, &t2, 0, len);
  } else
  { return a1->type->rank == a2->type->rank ? CMP_EQUAL :
           a1->type->rank < a2->type->rank ? CMP_LESS : CMP_GREATER;
  }
}


static int
compareStrings(word w1, word w2 ARG_LD)
{ PL_chars_t t1, t2;
  size_t len;

  get_string_text(w1, &t1 PASS_LD);
  get_string_text(w2, &t2 PASS_LD);
  len = (t1.length > t2.length ? t1.length : t2.length);

  return PL_cmp_text(&t1, 0, &t2, 0, len);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
compareStandard(Word p1, Word p2, int eq)

    Rules:

    Var @< AttVar @< Number @< Atom @< String < Term

    OldVar < NewVar	(not relyable)
    Atom:	alphabetically
    Strings:	alphabetically
    number:	value
    Term:	arity / alphabetically / recursive

If eq == TRUE, only test for equality. In this case expensive inequality
tests (alphabetical order) are skipped and the call returns NOTEQ.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
do_compare(term_agendaLR *agenda, int eq ARG_LD)
{ Word p1, p2;

  while( nextTermAgendaLR(agenda, &p1, &p2) )
  { word t1, t2;
    word w1 = *p1;
    word w2 = *p2;

    if ( w1 == w2 )
    { if ( isVar(w1) )
	goto cmpvars;
      continue;
    }

    t1 = tag(w1);
    t2 = tag(w2);

    if ( t1 != t2 )
    { if ( !truePrologFlag(PLFLAG_ISO) && !eq )
      { if ( (t1 == TAG_INTEGER && t2 == TAG_FLOAT) ||
	     (t1 == TAG_FLOAT && t2 == TAG_INTEGER) )
	{ number left, right;
	  int rc;

	  get_number(w1, &left PASS_LD);
	  get_number(w2, &right PASS_LD);
	  rc = cmpNumbers(&left, &right);
	  clearNumber(&left);
	  clearNumber(&right);

	  if ( rc == CMP_EQUAL )
	    return (t1 == TAG_FLOAT) ? CMP_LESS : CMP_GREATER;
	  return rc;
	}
      }

      if ( t1 > TAG_ATTVAR || t2 > TAG_ATTVAR )
	return t1 < t2 ? CMP_LESS : CMP_GREATER;
    }

    switch(t1)
    { case TAG_VAR:
      case TAG_ATTVAR:
      cmpvars:
	if ( p1 == p2 )
	  continue;
	return p1 < p2 ? CMP_LESS : CMP_GREATER;
      case TAG_INTEGER:
      { number n1, n2;
	int rc;

	get_integer(w1, &n1);
	get_integer(w2, &n2);
	if ( eq && (n1.type != n2.type) )
	  return CMP_NOTEQ;
	rc = cmpNumbers(&n1, &n2);
	clearNumber(&n1);
	clearNumber(&n2);

	if ( rc == CMP_EQUAL )
	  continue;
	return rc;
      }
      case TAG_FLOAT:
      { double f1 = valFloat(w1);
	double f2 = valFloat(w2);

	if ( f1 == f2 )
	  continue;
	return f1 < f2 ? CMP_LESS : CMP_GREATER;
      }
      case TAG_ATOM:
	return eq ? CMP_NOTEQ : compareAtoms(w1, w2);
      case TAG_STRING:
      { int rc = compareStrings(w1, w2 PASS_LD);

	if ( rc == CMP_EQUAL )
	  continue;
	return rc;
      }
      case TAG_COMPOUND:
      { Functor f1 = (Functor)valPtr(w1);
	Functor f2 = (Functor)valPtr(w2);

  #if O_CYCLIC
	while ( isRef(f1->definition) )
	  f1 = (Functor)unRef(f1->definition);
	while ( isRef(f2->definition) )
	  f2 = (Functor)unRef(f2->definition);
	if ( f1 == f2 )
	  continue;
  #endif

	if ( f1->definition != f2->definition )
	{ FunctorDef fd1 = valueFunctor(f1->definition);
	  FunctorDef fd2 = valueFunctor(f2->definition);

	  if ( eq )
	    return CMP_NOTEQ;

	  if ( fd1->arity != fd2->arity )
	    return fd1->arity > fd2->arity ? CMP_GREATER : CMP_LESS;

	  return compareAtoms(fd1->name, fd2->name);
	} else
	{ int arity = arityFunctor(f1->definition);

	  linkTermsCyclic(f1, f2 PASS_LD);
	  if ( !pushWorkAgendaLR(agenda, arity, f1->arguments, f2->arguments) )
	  { PL_error(NULL, 0, NULL, ERR_RESOURCE, ATOM_memory);
	    return CMP_ERROR;
	  }
	  continue;
	}
      }
      default:
	assert(0);
	return CMP_ERROR;
    }
  }

  return CMP_EQUAL;
}


int
compareStandard(Word p1, Word p2, int eq ARG_LD)
{ term_agendaLR agenda;
  int rc;

  initCyclic(PASS_LD1);
  initTermAgendaLR(&agenda, p1, p2);
  rc = do_compare(&agenda, eq PASS_LD);
  clearTermAgendaLR(&agenda);
  exitCyclic(PASS_LD1);

  return rc;
}


/* compare(-Diff, +T1, +T2) */

static
PRED_IMPL("compare", 3, compare, 0)
{ PRED_LD
  Word p1 = valTermRef(A2);
  Word p2 = p1+1;
  int val;
  atom_t a;

  if ( !PL_is_variable(A1) )
  { if ( !PL_get_atom_ex(A1, &a) )
      fail;

    if ( a == ATOM_equals )
      return compareStandard(p1, p2, TRUE PASS_LD) == CMP_EQUAL ? TRUE : FALSE;

    if ( a != ATOM_smaller && a != ATOM_larger )
      return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_order, A1);
  } else
  { a = 0;
  }

  if ( (val = compareStandard(p1, p2, FALSE PASS_LD)) == CMP_ERROR )
    return FALSE;

  if ( a )
  { if ( a == ATOM_smaller )
      return val < 0;
    else
      return val > 0;
  }

  return PL_unify_atom(A1, val < 0 ? ATOM_smaller :
		           val > 0 ? ATOM_larger :
		                     ATOM_equals);
}


static
PRED_IMPL("@<", 2, std_lt, 0)
{ PRED_LD
  Word p1 = valTermRef(A1);
  Word p2 = p1+1;
  int rc;

  if ( (rc=compareStandard(p1, p2, FALSE PASS_LD)) == CMP_ERROR )
    return FALSE;

  return rc < 0 ? TRUE : FALSE;
}


static
PRED_IMPL("@=<", 2, std_leq, 0)
{ PRED_LD
  Word p1 = valTermRef(A1);
  Word p2 = p1+1;
  int rc;

  if ( (rc=compareStandard(p1, p2, FALSE PASS_LD)) == CMP_ERROR )
    return FALSE;

  return rc <= 0 ? TRUE : FALSE;
}


static
PRED_IMPL("@>", 2, std_gt, 0)
{ PRED_LD
  Word p1 = valTermRef(A1);
  Word p2 = p1+1;
  int rc;

  if ( (rc=compareStandard(p1, p2, FALSE PASS_LD)) == CMP_ERROR )
    return FALSE;

  return rc > 0 ? TRUE : FALSE;
}


static
PRED_IMPL("@>=", 2, std_geq, 0)
{ PRED_LD
  Word p1 = valTermRef(A1);
  Word p2 = p1+1;
  int rc;

  if ( (rc=compareStandard(p1, p2, FALSE PASS_LD)) == CMP_ERROR )
    return FALSE;

  return rc >= 0 ? TRUE : FALSE;
}

		/********************************
		*           EQUALITY            *
		*********************************/

static
PRED_IMPL("==", 2, equal, 0)
{ PRED_LD
  Word p1 = valTermRef(A1);
  Word p2 = p1+1;
  int rc;

  if ( (rc=compareStandard(p1, p2, TRUE PASS_LD)) == CMP_ERROR )
    return FALSE;

  return rc == CMP_EQUAL ? TRUE : FALSE;
}


static
PRED_IMPL("\\==", 2, nonequal, 0)
{ PRED_LD
  Word p1 = valTermRef(A1);
  Word p2 = p1+1;
  int rc;

  if ( (rc=compareStandard(p1, p2, TRUE PASS_LD)) == CMP_ERROR )
    return FALSE;

  return rc == CMP_EQUAL ? FALSE : TRUE;
}


		/********************************
		*	     VARIANT	        *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Variant(A,B) is true iff there exists a  renaming of variables in A that
is (==) equivalent to B  and  a  renaming   of  variables  in  B that is
equivalent to A.

The implementation went through quite a few cycles. The current one uses
the following:

    * Term-walking and cycle-detection follows structure pointers as
      = and ==.
    * In addition, there are two pointers that walk A and B and do not
      follow the links created by the cycle detection.
    * For each variable pair encountered, we use the labeling mechanism
      described by Kuniaki Mukai:
	- We use a triple.  The variable is set to refer to the
	  triple.  The triple contains =v=, pointing to the variable for
	  reset, =l=, containing the labeling seen from the left and =r=
	  containing the labeling seen from the right.  The labeling is
	  a pointer to the variable at the other side.
	- For each pair of variables:
	    - Left is ok if no =l= label or =l= points to right
	    - Right is ok if no =r= label or =r= points to left
	    - If ok, ensure there is a triple and assign the label
	      if not already present.
    * If a shared subterm is found, we call number_common_subterm().
      Each variable encountered must either be labeled with itself
      from left and right or be unlabeled.  Unlabeled variables are
      labeled to itself.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct v_var
{ Word  v;				/* The variable */
  Word	l;				/* left label */
  Word	r;				/* rigth label */
} v_var;


#define consVar(n) (((word)(n)<<LMASK_BITS) | TAG_VAR | FIRST_MASK)
#define valVar(w)  ((intptr_t)(w) >> LMASK_BITS)
#define FMPLAIN(w) ((intptr_t)(w) & ~(FIRST_MASK|MARK_MASK))

static inline v_var *
get_v_entry(Word p, TmpBuffer buf)
{ if ( *p )
  { int id = valVar(*p);

    return &fetchBuffer(buf, id, v_var);
  }

  return NULL;
}


static void
pop_variant_mark(Functor l, Functor r, void *data)
{ uintptr_t d = (uintptr_t)data;

  if ( !(d & FIRST_MASK) )
    l->definition &= ~FIRST_MASK;
  if ( !(d & MARK_MASK) )
    r->definition &= ~MARK_MASK;
}


static bool
variant(term_agendaLRS *agenda, TmpBuffer buf ARG_LD)
{ Word l, r;

  while( nextTermAgendaLRS(agenda, &l, &r) )
  { word wl, wr;

  attvar:
    wl = *l;
    wr = *r;

    if ( tag(wl) != tag(wr) )
      fail;
    if ( tag(wl) == TAG_VAR )
    { v_var *vl = get_v_entry(l, buf);
      v_var *vr = get_v_entry(r, buf);

      if ( vl && vl->r && vl->r != r )
	fail;
      if ( vr && vr->l && vr->l != l )
	fail;

      if ( !vl )
      { v_var new;
	size_t id = entriesBuffer(buf, v_var);

	new.v = l;
	new.r = r;
	if ( l != r )
	{ new.l = NULL;
	  addBuffer(buf, new, v_var);
	  *l = consVar(id);
	} else
	{ new.l = l;
	  addBuffer(buf, new, v_var);
	  *l = consVar(id);

	  continue;
	}
      } else if ( !vl->r )
      { vl->r = r;
      }

      if ( !vr )
      { v_var new;
	size_t id = entriesBuffer(buf, v_var);

	new.v = r;
	new.l = l;
	new.r = NULL;
	addBuffer(buf, new, v_var);
	*r = consVar(id);
      } else if ( !vr->l )
      { vr->l = l;
      }

      continue;
    }

    if ( wl == wr && !isTerm(wl) )
      continue;

    switch(tag(wl))
    { case TAG_ATOM:
	fail;
      case TAG_ATTVAR:
      { l = valPAttVar(wl);
	r = valPAttVar(wr);

	goto attvar;
      }
      case TAG_INTEGER:
	if ( storage(wl) == STG_INLINE ||
	     storage(wr) == STG_INLINE )
	  return FALSE;
      case TAG_STRING:
      case TAG_FLOAT:
	if ( equalIndirect(wl, wr) )
	  continue;
        return FALSE;
      case TAG_COMPOUND:
      { Functor fl = valueTerm(wl);
	Functor fr = valueTerm(wr);
	intptr_t d;

	if ( FMPLAIN(fl->definition) != FMPLAIN(fr->definition) )
	  return FALSE;
	if ( (fl->definition & FIRST_MASK) &&
	     (fr->definition & MARK_MASK) )
	  continue;

	d = (fl->definition & FIRST_MASK) | (fr->definition & MARK_MASK);
	fl->definition |= FIRST_MASK;
	fr->definition |= MARK_MASK;

	pushWorkAgendaLRS(agenda, fl, fr, (void*)d);
	continue;
      }
    }
  }

  return TRUE;
}


static
PRED_IMPL("=@=", 2, variant, 0)
{ GET_LD
  term_agendaLRS agenda;
  bool rval;
  tmp_buffer buf;
  v_var *r;
  Word p1 = valTermRef(A1);
  Word p2 = valTermRef(A2);
  Functor t1, t2;

  deRef(p1);
  deRef(p2);

  if ( *p1 == *p2 )			/* same term */
    succeed;
  if ( tag(*p1) != tag(*p2) )		/* different type */
    fail;
again:
  switch(tag(*p1))			/* quick tests */
  { case TAG_VAR:
      succeed;
    case TAG_ATTVAR:
      p1 = valPAttVar(*p1);
      p2 = valPAttVar(*p2);
      goto again;
    case TAG_ATOM:
      fail;
    case TAG_INTEGER:
      if ( !(isIndirect(*p1) && isIndirect(*p2)) )
	fail;
      /*FALLTHROUGH*/
    case TAG_FLOAT:
    case TAG_STRING:
      return equalIndirect(*p1, *p2);
    case TAG_COMPOUND:
      t1 = valueTerm(*p1);
      t2 = valueTerm(*p2);
      if ( t1->definition != t2->definition )
	fail;
      break;
    default:
      assert(0);
      fail;
  }

  startCritical;
  initBuffer(&buf);			/* can be faster! */
  initTermAgendaLRS(&agenda, t1, t2, pop_variant_mark, NULL);
  rval = variant(&agenda, &buf PASS_LD);
  clearTermAgendaLRS(&agenda);
  for(r = baseBuffer(&buf, v_var); r < topBuffer(&buf, v_var); r++)
  { setVar(*r->v);
  }
  discardBuffer(&buf);
  SECURE(checkStacks(NULL));
  if ( !endCritical )
    return FALSE;

  return rval;
}


static
PRED_IMPL("\\=@=", 2, not_variant, 0)
{ return pl_variant2_va(PL__t0, PL__ac, PL__ctx) ? FALSE : TRUE;
}


/** ?=(@X, @Y) is semidet.

True if we can decide for now and forever  that X and Y are either equal
or non-equal. I.e. X and Y are equal or they cannot unify.
*/

static
PRED_IMPL("?=", 2, can_compare, 0)
{ PRED_LD
  fid_t fid = PL_open_foreign_frame();
  int rc;

  rc = PL_unify(A1, A2);
  if ( rc )
  { FliFrame fr = (FliFrame) valTermRef(fid);

    assert(fr->magic == FLI_MAGIC);
    if ( fr->mark.trailtop != tTop )
      rc = FALSE;
  } else if ( exception_term )
  { PL_close_foreign_frame(fid);	/* keep exception */
    return FALSE;
  } else
  { rc = TRUE;				/* could not unify */
  }

  PL_discard_foreign_frame(fid);
  return rc;
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
same_term(@T1, @T2) is semidet.

True if T1 and T2 is really  the   same  term,  so setarg/3 affects both
terms.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
PL_same_term__LD(term_t T1, term_t T2 ARG_LD)
{ Word t1 = valTermRef(T1);
  Word t2 = valTermRef(T2);

  deRef(t1);
  deRef(t2);

  if ( isVar(*t1) )
    return t1 == t2;
  if ( *t1 == *t2 )
    succeed;
  if ( isIndirect(*t1) && isIndirect(*t2) )
    return equalIndirect(*t1, *t2);

  fail;
}

static
PRED_IMPL("same_term", 2, same_term, 0)
{ PRED_LD

  return PL_same_term(A1, A2);
}


		/********************************
		*         TERM HACKING          *
		*********************************/

/* functor(+Term, -Name, -Arity) */
/* functor(-Term, +Name, +Arity) */

PRED_IMPL("functor", 3, functor, 0)
{ PRED_LD
  int arity;
  atom_t name;
  Word p = valTermRef(A1);

  deRef(p);

  if ( isTerm(*p) )
  { FunctorDef fd = valueFunctor(functorTerm(*p));
    if ( !PL_unify_atom(A2, fd->name) ||
	 !PL_unify_integer(A3, fd->arity) )
      fail;

    succeed;
  }
  if ( isAtomic(*p) )
  { if ( !PL_unify(A2, A1) ||
	 !PL_unify_integer(A3, 0) )
      fail;

    succeed;
  }
  if ( !PL_is_atomic(A2) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_atomic, A2);

  if ( !PL_get_integer_ex(A3, &arity) )
    fail;
  if ( arity == 0 )
    return PL_unify(A1, A2);
  if ( arity < 0 )
    return PL_error(NULL, 0, NULL, ERR_DOMAIN,
		    ATOM_not_less_than_zero, A3);
  if ( PL_get_atom_ex(A2, &name) )
    return PL_unify_functor(A1, PL_new_functor(name, arity));

  fail;
}


static
PRED_IMPL("arg", 3, arg, PL_FA_NONDETERMINISTIC)
{ PRED_LD
  atom_t name;
  int arity;

  term_t n    = A1;
  term_t term = A2;
  term_t arg  = A3;

  switch( CTX_CNTRL )
  { case FRG_FIRST_CALL:
    { int idx;
      Word p = valTermRef(term);

      deRef(p);
      if ( isTerm(*p) )
	arity = arityTerm(*p);
      else if ( isTextAtom(*p) && !truePrologFlag(PLFLAG_ISO) )
	arity = 0;
      else
	return PL_error("arg", 3, NULL, ERR_TYPE, ATOM_compound, term);

      if ( PL_get_integer(n, &idx) )
      { if ( idx > 0 && idx <= arity )
	{ Word ap = argTermP(*p, idx-1);

	  return unify_ptrs(valTermRef(arg), ap, ALLOW_GC|ALLOW_SHIFT PASS_LD);
	}
	if ( idx < 0 )
	  return PL_error("arg", 3, NULL, ERR_DOMAIN,
			  ATOM_not_less_than_zero, n);
	fail;
      }
      if ( PL_is_variable(n) )
      { int argn = 1;
	term_t a = PL_new_term_ref();

	for(argn=1; argn <= arity; argn++)
	{ _PL_get_arg(argn, term, a);
	  if ( PL_unify(arg, a) )
	  { PL_unify_integer(n, argn);
	    if ( argn == arity )
	      succeed;
	    ForeignRedoInt(argn);
	  }
	  if ( exception_term )
	    return FALSE;
	}
	fail;
      }
      return PL_error("arg", 3, NULL, ERR_TYPE, ATOM_integer, n);
    }
    case FRG_REDO:
    { int argn = (int)CTX_INT + 1;
      term_t a = PL_new_term_ref();

      if ( !PL_get_name_arity(term, &name, &arity) )
	sysError("arg/3: PL_get_name_arity() failed");

      for(; argn <= arity; argn++)
      { _PL_get_arg(argn, term, a);
	if ( PL_unify(arg, a) )
	{ PL_unify_integer(n, argn);
	  if ( argn == arity )
	    succeed;
	  ForeignRedoInt(argn);
	}
	if ( exception_term )
	  return FALSE;
      }

      fail;
    }
    default:
      succeed;
  }
}


#define SETARG_BACKTRACKABLE    0x1
#define SETARG_LINK		0x2


static word
setarg(term_t n, term_t term, term_t value, int flags)
{ GET_LD
  int arity, argn;
  atom_t name;
  Word a, v;

  if ( !PL_get_integer_ex(n, &argn) )
    fail;
  if ( argn <= 0 )
  { if ( argn < 0 )
      return PL_error(NULL, 0, NULL, ERR_DOMAIN,
		      ATOM_not_less_than_zero, n);
    else
      fail;
  }
  if ( !PL_get_name_arity(term, &name, &arity) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_compound, term);

  if ( argn > arity )
    fail;

  if ( (flags & SETARG_BACKTRACKABLE) )
  { a = valTermRef(term);
    deRef(a);
    a = argTermP(*a, argn-1);

    if ( isVar(*a) )
    { return unify_ptrs(valTermRef(value), a, ALLOW_GC|ALLOW_SHIFT PASS_LD);
    } else
    { if ( !hasGlobalSpace(0) )
      { int rc;

	if ( (rc=ensureGlobalSpace(0, ALLOW_GC)) != TRUE )
	  return raiseStackOverflow(rc);
	a = valTermRef(term);
	deRef(a);
	a = argTermP(*a, argn-1);
      }

      TrailAssignment(a);
    }
  } else
  { v = valTermRef(value);
    deRef(v);

    if ( storage(*v) == STG_GLOBAL )
    { if ( !(flags & SETARG_LINK) )
      { term_t copy = PL_new_term_ref();

	if ( !duplicate_term(value, copy PASS_LD) )
	  fail;
	value = copy;
      }

      freezeGlobal(PASS_LD1);
    }

    a = valTermRef(term);		/* duplicate may shift stacks */
    deRef(a);
    a = argTermP(*a, argn-1);
  }
					/* this is unify(), but the */
					/* assignment must *not* be trailed */
  v = valTermRef(value);
  deRef(v);

  if ( isVar(*v) )
  { if ( v < a )
    { *a = makeRef(v);
    } else if ( a < v )
    { setVar(*a);
      *v = makeRef(a);
    } else
      setVar(*a);
  } else if ( isAttVar(*v) )
  { *a = makeRef(v);
  } else
    *a = *v;

  succeed;
}


static
PRED_IMPL("setarg", 3, setarg, 0)
{ return setarg(A1, A2, A3, SETARG_BACKTRACKABLE);
}


static
PRED_IMPL("nb_setarg", 3, nb_setarg, 0)
{ return setarg(A1, A2, A3, 0);
}


static
PRED_IMPL("nb_linkarg", 3, nb_linkarg, 0)
{ return setarg(A1, A2, A3, SETARG_LINK);
}


intptr_t
skip_list(Word l, Word *tailp ARG_LD)
{ intptr_t length = 0;
  Word s; /* slow */

  deRef(l);
  s = l;

  if ( isList(*l) )
  { intptr_t power = 1, lam = 0;
    do
    { if ( power == lam )
      { s = l;
	power *= 2;
	lam = 0;
      }
      lam++;
      length++;
      l = TailList(l); deRef(l);
    } while ( *l != *s && isList(*l) );
  }
  *tailp = l;

  return length;
}

/** '$skip_list'(-Length, +Xs0, -Xs) is det.

Xs0, Xs is a pair of list differences. Xs0   is the input list and Xs is
the minimal remaining list. Examination of   Xs  permits to classify the
list Xs0:

	Xs        | list type of Xs0   | Length
	[]    ... | well formed        | length
	Var   ... | partial            | elements skipped
	[_|_] ... | infinite           | upper bound for cycle
	Term  ... | malformed          | elements skipped
*/

PRED_IMPL("$skip_list", 3, skip_list, 0)
{ PRED_LD
  Word tail;
  intptr_t len;

  len = skip_list(valTermRef(A2), &tail PASS_LD);
  if ( unify_ptrs(valTermRef(A3), tail, ALLOW_GC|ALLOW_SHIFT PASS_LD) &&
       PL_unify_integer(A1, len) )
    return TRUE;

  return FALSE;
}


/*  Determine the length of a list.  Returns:

	len >=  0 if list is proper
	len == -1 if list is not a list
	len == -2 if list is incomplete (i.e. tail is unbound)

 ** Mon Apr 18 16:29:01 1988  jan@swivax.UUCP (Jan Wielemaker)  */

intptr_t
lengthList(term_t list, int errors)
{ GET_LD
  intptr_t length = 0;
  Word l = valTermRef(list);
  Word tail;

  length = skip_list(l, &tail PASS_LD);

  if ( isNil(*tail) )
    return length;

  if ( errors )
    PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_list, wordToTermRef(l));

  return isVar(*tail) ? -2 : -1;
}


static
PRED_IMPL("=..", 2, univ, PL_FA_ISO)
{ GET_LD
  term_t t = A1;
  term_t list = A2;
  int arity;
  atom_t name;
  int n;

  if ( PL_is_variable(t) )
  { term_t tail = PL_copy_term_ref(list);
    term_t head = PL_new_term_ref();

    if ( !PL_get_list(tail, head, tail) )
    { if ( PL_get_nil(tail) )
	return PL_error(NULL, 0, NULL, ERR_DOMAIN,
			ATOM_non_empty_list, tail);
      return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_list, tail);
    }

    if ( PL_get_nil(tail) )		/* A =.. [H] */
      return PL_unify(t, head);
    if ( !PL_get_atom_ex(head, &name) )
      fail;

    if ( (arity = (int)lengthList(tail, FALSE)) < 0 ) /* TBD: check MAXINT */
    { if ( arity == -1 )
	return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_list, list);
      else
	return PL_error(NULL, 0, NULL, ERR_INSTANTIATION);
    }

    if ( !PL_unify_functor(t, PL_new_functor(name, arity)) )
      fail;

    for(n=1; PL_get_list(tail, head, tail); n++)
    { if ( !PL_unify_arg(n, t, head) )
	fail;
    }

    succeed;
  }

					/* 1st arg is term or atom */
  if ( PL_get_name_arity(t, &name, &arity) )
  { term_t head = PL_new_term_ref();
    term_t l = PL_new_term_ref();

    if ( !PL_unify_list_ex(list, head, l) )
      fail;
    if ( !PL_unify_atom(head, name) )
      fail;

    for(n = 1; n <= arity; n++)
    { if ( !PL_unify_list_ex(l, head, l) ||
	   !PL_unify_arg(n, t, head) )
	fail;
    }

    return PL_unify_nil_ex(l);
  }

  if ( PL_is_atomic(t) )		/* 3 =.. X, 3.4 =.. X, "foo" =.. X */
  { term_t head = PL_new_term_ref();
    term_t l = PL_new_term_ref();

    if ( PL_unify_list_ex(list, head, l) &&
	 PL_unify(head, t) &&
	 PL_unify_nil_ex(l) )
      succeed;
  }

  fail;
}


		 /*******************************
		 *	     NUMBERVARS		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Returns	>= 0: Number for next variable variable
	  -1: Error
	< -1: Out of stack error
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
do_number_vars(Word p, nv_options *options, int n, mark *m ARG_LD)
{
start:
  if ( n < 0 )
    return n;				/* error */

  deRef(p);

  if ( canBind(*p) )
  { Word a;
    word v;

    if ( isAttVar(*p) )
    { switch(options->on_attvar)
      { case AV_SKIP:
	  return n;
	case AV_ERROR:
	  return -1;
	case AV_BIND:
	  break;
      }
    }

    if ( !hasGlobalSpace(2) )
      return overflowCode(2);

    a = gTop;
    a[0] = options->functor;
    if ( options->singletons )
    { a[1] = ATOM_anonvar;
    } else
    { a[1] = consInt(n);
      assert(valInt(a[1]) == n);
      n++;
    }
    gTop += 2;

    v = consPtr(a, TAG_COMPOUND|STG_GLOBAL);
    bindConst(p, v);
  } else if ( isTerm(*p) )
  { Functor f = valueTerm(*p);
    int arity;

    if ( options->singletons &&
	 f->definition == options->functor &&
	 (Word)f >= m->globaltop )	/* new one we created ourselves */
    { Word p = &f->arguments[0];

      if ( *p == ATOM_anonvar )
      { *p = consInt(n);		/* stack can't hold enough vars */
        n++;				/* to averflow this */
      }
    }

    if ( !options->singletons && visited(f PASS_LD) )
      return n;

    arity = arityFunctor(f->definition);

    for(p=argTermP(*p, 0); --arity > 0; p++)
    { if ( (n=do_number_vars(p, options, n, m PASS_LD)) < 0 )
	return n;
    }
    goto start;				/* right-argument recursion */
  }

  return n;				/* anything else */
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Returns	>= 0: Number for next variable variable
	  -1: Error
	< -1: Out of stack error
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

int
numberVars(term_t t, nv_options *options, int n ARG_LD)
{ for(;;)
  { mark m;
    int rc;

    Mark(m);
    initvisited(PASS_LD1);
    rc = do_number_vars(valTermRef(t), options, n, &m PASS_LD);
    unvisit(PASS_LD1);
    if ( rc >= 0 )			/* all ok */
    { DiscardMark(m);
      return rc;
    } else if ( rc == -1 )		/* error */
    { DiscardMark(m);
      return rc;
    } else				/* stack overflow */
    { int rc2;

      Undo(m);
      DiscardMark(m);
      if ( !(rc2 = makeMoreStackSpace(rc, ALLOW_GC|ALLOW_SHIFT)) )
	return rc;
    }
  }
}


static const opt_spec numbervar_options[] =
{ { ATOM_attvar,	    OPT_ATOM },
  { ATOM_functor_name,	    OPT_ATOM },
  { ATOM_singletons,	    OPT_BOOL },
  { NULL_ATOM,		    0 }
};


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
numbervars(+Term, +Start, -End, +Options)
numbervars(+Term, +Functor, +Start, -End)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static
PRED_IMPL("numbervars", 4, numbervars, 0)
{ GET_LD
  int n;
  atom_t name = ATOM_isovar;		/* '$VAR' */
  atom_t av = ATOM_error;
  term_t t, end, options;
  nv_options opts;

  opts.singletons = FALSE;

  t = PL_copy_term_ref(A1);

  if ( !PL_get_integer(A2, &n) )
  { if ( PL_get_atom(A2, &name) &&
	 PL_get_integer(A3, &n)	)	/* old calling conventions */
    { end = A4;
      options = 0;
    } else
    { return PL_get_integer_ex(A2, &n);
    }
  } else
  { end = A3;
    options = A4;
  }

  if ( options &&
       !scan_options(options, 0, ATOM_numbervar_option, numbervar_options,
		     &av, &name, &opts.singletons) )
    fail;

  if ( av == ATOM_error )
    opts.on_attvar = AV_ERROR;
  else if ( av == ATOM_skip )
    opts.on_attvar = AV_SKIP;
  else if ( av == ATOM_bind )
    opts.on_attvar = AV_BIND;
  else
    return PL_error(NULL, 0, NULL, ERR_DOMAIN, ATOM_numbervar_option, options);

  if ( opts.singletons )		/* Hack */
  { if ( !is_acyclic(valTermRef(A1) PASS_LD) )
      opts.singletons = FALSE;
  }

  opts.functor = PL_new_functor(name, 1);
  n = numberVars(t, &opts, n PASS_LD);
  if ( n == -1 )
    return PL_error(NULL, 0, NULL,
		    ERR_TYPE, ATOM_free_of_attvar, A1);
  else if ( n < 0 )
    return raiseStackOverflow(n);

  return PL_unify_integer(end, n);
}


		 /*******************************
		 *	   TERM-VARIABLES	*
		 *******************************/

#define TV_ATTVAR 0x1
#define TV_EXCEPTION ((size_t)-1)
#define TV_NOSPACE   ((size_t)-2)

static size_t
term_variables_loop(term_agenda *agenda, size_t maxcount, int flags ARG_LD)
{ Word p;
  size_t count = 0;

  while( (p=nextTermAgenda(agenda)) )
  { word w;

  again:
    w = *p;

    if ( canBind(w) )
    { term_t v;

      if ( visitedWord(p PASS_LD) )
	continue;

      if ( flags & TV_ATTVAR )
      { if ( isAttVar(w) )
	{ Word p2 = valPAttVar(w);

	  if ( ++count > maxcount )
	    return count;
	  if ( !(v = PL_new_term_ref_noshift()) )
	    return TV_NOSPACE;
	  *valTermRef(v) = makeRef(p);

	  deRef2(p2, p);
	  goto again;
	}
      } else
      { if ( ++count > maxcount )
	  return count;
	if ( !(v = PL_new_term_ref_noshift()) )
	  return TV_NOSPACE;
	*valTermRef(v) = makeRef(p);
      }
    } else if ( isTerm(w) )
    { pushTermAgendaIfNotVisited(agenda, w);
    }
  }

  return count;
}


static size_t
term_variables_to_termv(term_t t, term_t *vp, size_t maxcount, int flags ARG_LD)
{ term_agenda agenda;
  term_t v0   = PL_new_term_refs(0);
  size_t count;

  startCritical;
  initvisited(PASS_LD1);
  initTermAgenda(&agenda, valTermRef(t));
  count = term_variables_loop(&agenda, maxcount, flags PASS_LD);
  clearTermAgenda(&agenda);
  unvisit(PASS_LD1);
  if ( !endCritical )
    return TV_EXCEPTION;

  *vp = v0;
  return count;
}



static int
term_variables(term_t t, term_t vars, term_t tail, int flags ARG_LD)
{ term_t head = PL_new_term_ref();
  term_t list = PL_copy_term_ref(vars);
  term_t v0;
  size_t i, maxcount, count;

  if ( !(!tail && PL_skip_list(vars, 0, &maxcount) == PL_LIST) )
    maxcount = ~0;

  for(;;)
  { count = term_variables_to_termv(t, &v0, maxcount, flags PASS_LD);
    if ( count == TV_EXCEPTION )
      return FALSE;
    if ( count == TV_NOSPACE )
    { PL_reset_term_refs(v0);
      if ( !makeMoreStackSpace(LOCAL_OVERFLOW, ALLOW_SHIFT) )
	return FALSE;			/* GC doesn't help */
      continue;
    }
    if ( count > maxcount )
      return FALSE;
    break;
  }

  for(i=0; i<count; i++)
  { if ( !PL_unify_list(list, head, list) ||
	 !PL_unify(head, v0+i) )
      fail;
  }
  PL_reset_term_refs(v0);

  if ( tail )
    return PL_unify(list, tail);
  else
    return PL_unify_nil(list);
}



static
PRED_IMPL("term_variables", 2, term_variables2, 0)
{ PRED_LD

  return term_variables(A1, A2, 0, 0 PASS_LD);
}


static
PRED_IMPL("term_variables", 3, term_variables3, 0)
{ PRED_LD

  return term_variables(A1, A2, A3, 0 PASS_LD);
}


static
PRED_IMPL("term_attvars", 2, term_attvars, 0)
{ PRED_LD

  return term_variables(A1, A2, 0, TV_ATTVAR PASS_LD);
}


		 /*******************************
		 *	      SUBSUMES		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
subsumes is defined as

subsumes(General, Specific) :-
	term_variables(Specific, SVars),
	General = Specific,
	term_variables(SVars, SVars).

Below is the implementation, but we keep  the array of variables instead
of creating an array and we check whether these are all unique variables
by scanning the array.  This saves both time and space.

We tried to do this using   a one-sided unification implementation. Most
of this is fairly trivial, but  we  must   make  sure  we know when left
argument (general) becomes a pointer  in   the  specific term. There are
three cases for this to happen. One  is following a cycle-reference, two
is following a previously bound term and  three is following a reference
pointer from a variable that  was   shared  between general and specific
before the entry of subsumes/2. The first  two are easily fixed. I don't
know how to fix the latter without a   complete  scan on specific. If we
need to do that anyway,  we  can  just   as  well  use  the below simple
algorithm.

We can enhance on this by combining this with the one-sided unification.
We could delay scanning specific until we  bind the first variable. This
will not have any significant  inpact   on  performance for a succeeding
subsumes check, but can result in early failure and avoiding the scan of
specific. This works because  the   one-sided  unification algorithm can
only succeed in places where it should fail.

The latest version of the old algorithm is in the GIT commit

	f68eb71a9d5d0b9b6055483842d9654c30e29550
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
subsumes(term_t general, term_t specific ARG_LD)
{ term_t v0;
  size_t i, n;
  term_t ex = 0;
  int rc;
  int omode;

  n = term_variables_to_termv(specific, &v0, ~0, 0 PASS_LD);
  omode = LD->prolog_flag.occurs_check;
  LD->prolog_flag.occurs_check = OCCURS_CHECK_FALSE;
  rc = PL_unify(general, specific);
  LD->prolog_flag.occurs_check = omode;

  if ( rc && foreignWakeup(&ex PASS_LD) )
  { int rc = TRUE;

    startCritical;
    initvisited(PASS_LD1);
    for(i=0; i<n; i++)
    { Word p = valTermRef(v0+i);
      deRef(p);

      if ( !isVar(*p) || visitedWord(p PASS_LD) )
      { rc = FALSE;
	break;
      }
    }
    unvisit(PASS_LD1);
    if ( !endCritical )
      return FALSE;
    return rc;
  }

  if ( ex )
    return PL_raise_exception(ex);

  fail;
}


static
PRED_IMPL("subsumes_term", 2, subsumes_term, 0)
{ PRED_LD
  int rc;
  fid_t fid;

  fid = PL_open_foreign_frame();
  rc = subsumes(A1, A2 PASS_LD);
  PL_discard_foreign_frame(fid);

  return rc;
}


/** '$e_free_variables'(V0^V1^Term, Vars)

Used by setof/3 and bagof/3 to determine  the free variables in the goal
that have not been existentially bound. The implementation is very close
to term_variables/2, but  while  traversing  the   lefthand  of  ^,  the
variable is marked but not added to the list.
*/

static size_t
free_variables_loop(Word t, term_t l, size_t n, int existential ARG_LD)
{
right_recursion:
  deRef(t);

  if ( canBind(*t) )
  { term_t v;

    if ( !visitedWord(t PASS_LD) && !existential )
    { if ( !(v = PL_new_term_ref_noshift()) )
	return TV_NOSPACE;
      *valTermRef(v) = makeRef(t);

      n++;
    }

    return n;
  }
  if ( isTerm(*t) )
  { int arity;
    Functor f = valueTerm(*t);
    word fd = f->definition;

    if ( visited(f PASS_LD) )
      return n;

    t = f->arguments;
    if ( fd == FUNCTOR_hat2 )
    { n = free_variables_loop(t, l, n, TRUE PASS_LD); /* cannot overflow */
      t++;
    } else
    { arity = arityFunctor(f->definition);
      for(; --arity > 0; t++)
      { n = free_variables_loop(t, l, n, existential PASS_LD);
	if ( n == TV_NOSPACE )
	  return n;
      }
    }

    goto right_recursion;
  }

  return n;
}


static
PRED_IMPL("$e_free_variables", 2, e_free_variables, 0)
{ GET_LD

  for(;;)
  { Word t2 = valTermRef(A1);
    term_t v0 = PL_new_term_refs(0);
    size_t i, n;

    startCritical;
    initvisited(PASS_LD1);
    n = free_variables_loop(t2, v0, 0, FALSE PASS_LD);
    unvisit(PASS_LD1);
    if ( !endCritical )
      return FALSE;
    if ( n == TV_NOSPACE )
    { PL_reset_term_refs(v0);
      if ( !makeMoreStackSpace(LOCAL_OVERFLOW, ALLOW_SHIFT) )
	return FALSE;
      continue;
    }

    if ( PL_unify_functor(A2, PL_new_functor(ATOM_v, (int)n)) )
    { for(i=0; i<n; i++)
      { if ( !PL_unify_arg((int)i+1, A2, v0+i) )
	  return FALSE;
      }

      return TRUE;
    }
    return FALSE;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
unifiable(@X, @Y, -Substitution)

If X can be unified to Y, unify   Substitution with a list of Variable =
value for the substitutions that must be made to make X and Y identical.

The implementation extracts the substitutions  from the trail, rewinding
the trail at the same  time.  This   is  fairly  trivial, except for the
assignments of attributed variables (assignAttVar()). The last operation
of assignAttVar() is a trailed assignment  replacing the attvar with its
value. Before that it performs two trailed  actions to update the wakeup
list. These two must be skipped.

Unfortunately, if a value is unified to   a  local stack variable (which
can only be the case if one of the arguments is a plain variable) things
get very complicated. Therefore we test   these  cases before going into
the trouble. Note that unifying attributed   variables  is no problem as
these always live on the global stack.

(*) Unfortunately, we cannot handle  shift/GC   during  this process. In
particular, if we  need  space  for   the  result-list,  we  cannot call
allocGlobal(), because the resulting  GC  will   do  early-reset  on the
trailed variables and thus invalidate our nice   and clean trail. So, if
there is no space we rewind and retry the whole process.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
unifiable_occurs_check(term_t t1, term_t t2 ARG_LD)
{ switch(LD->prolog_flag.occurs_check)
  { case OCCURS_CHECK_FALSE:
      return TRUE;
    case OCCURS_CHECK_TRUE:
    case OCCURS_CHECK_ERROR:
    { Word p1 = valTermRef(t1);
      Word p2 = valTermRef(t2);

      deRef(p1);
      if ( !var_occurs_in(p1, p2) )
	return TRUE;

      return failed_unify_with_occurs_check(p1, p2,
					    LD->prolog_flag.occurs_check
					    PASS_LD);
    }
    default:
      assert(0);
      fail;
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Same as unify_ptrs(), but ensures that   all  assignments are trailed by
setting LD->mark_bar to the top  of   the  memory. Note that NO_MARK_BAR
also needs support in garbageCollect() and growStacks().
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static bool
unify_all_trail_ptrs(Word t1, Word t2 ARG_LD)
{ for(;;)
  { mark m;
    int rc;

    Mark(m);
    LD->mark_bar = NO_MARK_BAR;
    rc = raw_unify_ptrs(t1, t2 PASS_LD);
    if ( rc == TRUE )			/* Terms unified */
    { DiscardMark(m);
      return rc;
    } else if ( rc == FALSE )		/* Terms did not unify */
    { if ( !exception_term )		/* Check for occurs error */
	Undo(m);
      DiscardMark(m);
      return rc;
    } else				/* Stack overflow */
    { int rc2;

      Undo(m);
      DiscardMark(m);
      PushPtr(t1); PushPtr(t2);
      rc2 = makeMoreStackSpace(rc, ALLOW_GC|ALLOW_SHIFT);
      PopPtr(t2); PopPtr(t1);
      if ( !rc2 )
	return FALSE;
    }
  }
}


static ssize_t
unifiable(term_t t1, term_t t2, term_t subst ARG_LD)
{ fid_t fid;

  if ( PL_is_variable(t1) )
  { if ( PL_compare(t1, t2) == 0 )
    { return PL_unify_atom(subst, ATOM_nil);
    } else
    { if ( !unifiable_occurs_check(t1, t2 PASS_LD) )
	fail;

      return PL_unify_term(subst,
			   PL_FUNCTOR, FUNCTOR_dot2,
			     PL_FUNCTOR, FUNCTOR_equals2,
			       PL_TERM, t1,
			       PL_TERM, t2,
			     PL_ATOM, ATOM_nil);
    }
  }
  if ( PL_is_variable(t2) )
  { if ( !unifiable_occurs_check(t2, t1 PASS_LD) )
      fail;

    return PL_unify_term(subst,
			 PL_FUNCTOR, FUNCTOR_dot2,
			   PL_FUNCTOR, FUNCTOR_equals2,
			     PL_TERM, t2,
			     PL_TERM, t1,
			   PL_ATOM, ATOM_nil);
  }

  if ( !(fid = PL_open_foreign_frame()) )
    return FALSE;

retry:
  if ( unify_all_trail_ptrs(valTermRef(t1),	/* can do shift/gc */
			    valTermRef(t2) PASS_LD) )
  { FliFrame fr = (FliFrame)valTermRef(fid);
    TrailEntry tt = tTop;
    TrailEntry mt = fr->mark.trailtop;

    if ( tt > mt )
    { ssize_t needed = (tt-mt)*6+1;
      Word list, gp, tail;

      if ( !hasGlobalSpace(needed) )	/* See (*) */
      { int rc = overflowCode(needed);

	PL_rewind_foreign_frame(fid);
	rc = makeMoreStackSpace(rc, ALLOW_GC|ALLOW_SHIFT);
	if ( rc )
	  goto retry;
	return FALSE;
      }

      tail = list = gTop;
      gp = list+1;

      *list = ATOM_nil;
      while(--tt >= mt)
      { Word p = tt->address;

	*tail = consPtr(&gp[0], TAG_COMPOUND|STG_GLOBAL);
	gp[0] = FUNCTOR_dot2;
	gp[1] = consPtr(&gp[3], TAG_COMPOUND|STG_GLOBAL);
	gp[2] = ATOM_nil;
	tail = &gp[2];
	gp[3] = FUNCTOR_equals2;
	if ( isTrailVal(p) )
	{ Word p2 = tt[-1].address;
	  gp[4] = makeRef(p2);
	  gp[5] = *p2;
	} else
	{ gp[5] = *p;
	  assert(onGlobalArea(p));
	  gp[4] = makeRefG(p);
	  setVar(*p);
	}
	gp += 6;

	if ( isTrailVal(p) )
	{ assert(isAttVar(trailVal(p)));

	  tt--;				/* re-insert the attvar */
	  *tt->address = trailVal(p);

	  tt--;				/* restore tail of wakeup list */
	  p = tt->address;
	  if ( isTrailVal(p) )
	  { tt--;
	    *tt->address = trailVal(p);
	  } else
	  { setVar(*p);
	  }

	  tt--;				/* restore head of wakeup list */
	  p = tt->address;
	  if ( isTrailVal(p) )
	  { tt--;
	    *tt->address = trailVal(p);
	  } else
	  { setVar(*p);
	  }

	  assert(tt>=mt);
	}
      }
      gTop = gp;			/* may not have used all space */
      tTop = fr->mark.trailtop;

      PL_close_foreign_frame(fid);
      return PL_unify(wordToTermRef(list), subst);
    } else
    { PL_close_foreign_frame(fid);
      return PL_unify_atom(subst, ATOM_nil);
    }
  } else
  { PL_close_foreign_frame(fid);
    return FALSE;
  }
}


static
PRED_IMPL("unifiable", 3, unifiable, 0)
{ PRED_LD

  return unifiable(A1, A2, A3 PASS_LD);
}

#if O_CYCLIC

		 /*******************************
		 *	       CYCLES		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
term_cycles(+Term, -Cycles)
is_cycle(+SubTerm, +Cycles)

term_cycles(+Term, -Cycles) returns a binary tree  holding the cycles of
Term sorted by address. The tree   is  represented as cycle(Cycle, Left,
Right) where [] represents terminals.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */



#endif /*O_CYCLIC*/

		 /*******************************
		 *	    COPY TERM		*
		 *******************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
copy_term(+Term, -Copy)

Copy a term, renaming its  variables.   Attributed  variables have their
attributed copied, so futher modification of the attribute list does not
affect the copy  and  visa  versa.   The  algorithm  deals  with  shared
variables as well as cyclic  terms.  It   works,  like  unify for cyclic
terms, by creating references from  the   original  to the reference and
restoring the references using a stack.

There are three types of references between the original and the copy:

	* For variables we set the new variable to VAR_MARK and
	  make a reference to it.  This means that if we find a
	  reference to a variable VAR_MARK we must create a reference
	  to the same address.
	* For attributed variables we create an TAG_ATTVAR link to the
	  copy.  If we find a TAG_ATTVAR pointing to a TAG_ATTVAR we
	  know we found a copy.  Unfortunately just trailing the old
	  location doesn't suffice as we must recreate the link to
	  the old address, so we push this one first.
	* Compounds use the old trick to make the functor a reference
	  to the copy.

do_copy_term() returns TRUE if the term can   be shared and FALSE if not
(i.e. it is a variable or attributed variable). If, in sharing mode, the
copying routine copied a shareable term it   discards the copy and links
the original.

We cannot affort the stacks  to  be   shifted  during  the  execution of
copy_term/2. If the copy  fails  due   to  lack  of space do_copy_term()
returns with *_OVERFLOW. We then use   exitCyclic()  to undo the damage,
reset gTop, get more space and try again.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

#define VAR_MARK (0x1<<LMASK_BITS|TAG_VAR)

#define COPY_SHARE 0x01			/* Allow sharing ground terms */
#define COPY_ATTRS 0x02			/* do copy attributes */


static inline void
initCyclicCopy(ARG1_LD)
{ LD->cycle.lstack.unit_size = sizeof(Word);
}


static inline void
TrailCyclic(Word p ARG_LD)
{ pushSegStack(&LD->cycle.lstack, &p);
}


static inline void
exitCyclicCopy(size_t count, int flags ARG_LD)
{ while(LD->cycle.lstack.count > count)
  { Word p;

    popSegStack(&LD->cycle.lstack, &p);

    if ( isRef(*p) )
    { Word p2 = unRef(*p);

      if ( *p2 == VAR_MARK )		/* sharing variables */
      { setVar(*p2);
	setVar(*p);
      } else
      { *p = *p2;			/* cyclic terms */
      }
    } else if ( is_first(p) )		/* shared ground term */
    { clear_first(p);
    } else
    { Word old;

      popSegStack(&LD->cycle.lstack, &old);

      if ( !(flags&COPY_ATTRS) )
      { Word p2 = valPAttVar(*p);

	assert(*p2 == VAR_MARK);
	setVar(*p2);
      }

      *p = consPtr(old, STG_GLOBAL|TAG_ATTVAR);
    }
  }
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
FALSE: term cannot be shared
TRUE:  term can be shared (ground)
*_OVERFLOW: not enough space on the stack
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static int
do_copy_term(Word from, Word to, int flags ARG_LD)
{

again:
  switch(tag(*from))
  { case TAG_REFERENCE:
    { Word p2 = unRef(*from);

      if ( *p2 == VAR_MARK )
      { *to = makeRef(p2);
        return FALSE;
      } else
      { from = p2;
	goto again;
      }
    }
    case TAG_VAR:
      *to = VAR_MARK;
      *from = makeRef(to);
      TrailCyclic(from PASS_LD);
      return FALSE;
    case TAG_ATTVAR:
    { Word p = valPAttVar(*from);

      if ( !(flags & COPY_ATTRS) )
      { if ( *p == VAR_MARK )
	{ *to = makeRef(p);
	  return FALSE;
	} else
	{ *to = VAR_MARK;
	  *from = consPtr(to, STG_GLOBAL|TAG_ATTVAR);
	  TrailCyclic(p PASS_LD);
	  TrailCyclic(from PASS_LD);
	  return FALSE;
	}
      }

      if ( isAttVar(*p) )		/* already copied */
      { *to = makeRefG(p);
        return FALSE;
      } else
      { Word attr;			/* the new attributes */
	int rc;

	if ( !onGlobalArea(to) )
	{ Word t;

	  if ( !(t = allocGlobalNoShift(1)) )
	    return GLOBAL_OVERFLOW;

	  *to = makeRefG(t);
	  to = t;
	}
	if ( !(attr = allocGlobalNoShift(1)) )
	  return GLOBAL_OVERFLOW;
	TrailCyclic(p PASS_LD);
	TrailCyclic(from PASS_LD);
	*from = consPtr(to, STG_GLOBAL|TAG_ATTVAR);
	*to = consPtr(attr, STG_GLOBAL|TAG_ATTVAR);

					/* copy attribute value */
	flags &= ~COPY_SHARE;
	if ( (rc=do_copy_term(p, attr, flags PASS_LD)) < 0 )
	  return rc;
	return FALSE;
      }
    }
    case TAG_ATOM:
    case TAG_FLOAT:
    case TAG_INTEGER:
    case TAG_STRING:
      *to = *from;
      return TRUE;
    case TAG_COMPOUND:
    { Functor f1 = valueTerm(*from);

      if ( isRef(f1->definition) )
      { *to = consPtr(unRef(f1->definition), TAG_COMPOUND|STG_GLOBAL);
        return FALSE;			/* Cyclic */
      } else if ( f1->definition & FIRST_MASK )
      { *to = *from;
	return TRUE;
      } else
      { int arity = arityFunctor(f1->definition);
	Word oldtop = gTop;
	Word to0 = to;
	Word from0 = from;
	Functor f2;
	int ground = TRUE;
	size_t count = LD->cycle.lstack.count;

	if ( !(f2 = (Functor)allocGlobalNoShift(arity+1)) )
	  return GLOBAL_OVERFLOW;

	f2->definition = f1->definition;
	f1->definition = makeRefG((Word)f2);
	TrailCyclic(&f1->definition PASS_LD);
	*to = consPtr(unRef(f1->definition), TAG_COMPOUND|STG_GLOBAL);

	from = &f1->arguments[0];
	to   = &f2->arguments[0];
	while(--arity > 0)
	{ int rc = do_copy_term(from++, to++, flags PASS_LD);
	  if ( rc < 0 )
	    return rc;
	  ground &= rc;
	}

	if ( (flags & COPY_SHARE) )
	{ int rc = do_copy_term(from, to, flags PASS_LD);

	  if ( rc < 0 )
	    return rc;

	  ground &= rc;
	  if ( ground )
	  { exitCyclicCopy(count, flags PASS_LD);
	    gTop = oldtop;
	    *to0 = *from0;
	    f1->definition |= FIRST_MASK;
	    TrailCyclic(&f1->definition PASS_LD);
	    DEBUG(2, Sdprintf("Shared\n"));
	    return TRUE;
	  } else
	    return FALSE;
	} else
	  goto again;
      }
    }
    default:
      assert(0);
      return FALSE;
  }
}


static int
copy_term_refs(term_t from, term_t to, int flags ARG_LD)
{ for(;;)
  { fid_t fid;
    int rc;
    Word dest;

    if ( !(fid = PL_open_foreign_frame()) )
      return FALSE;			/* no space */

    if ( !(dest = allocGlobal(1)) )	/* make a variable on the global */
      return FALSE;			/* stack */
    setVar(*dest);
    *valTermRef(to) = makeRef(dest);

    initCyclicCopy(PASS_LD1);
    rc = do_copy_term(valTermRef(from), dest, flags PASS_LD);
    exitCyclicCopy(0, flags PASS_LD);

    if ( rc < 0 )			/* no space for copy */
    { PL_discard_foreign_frame(fid);
      PL_put_variable(to);
      if ( !makeMoreStackSpace(rc, ALLOW_SHIFT|ALLOW_GC) )
	return FALSE;
    } else
    { PL_close_foreign_frame(fid);
      return TRUE;		/* if do_copy_term() == FALSE --> not-ground */
    }
  }
}


static
PRED_IMPL("copy_term", 2, copy_term, 0)
{ PRED_LD
  term_t copy = PL_new_term_ref();

  if ( copy_term_refs(A1, copy, COPY_SHARE|COPY_ATTRS PASS_LD) )
    return PL_unify(copy, A2);

  fail;
}


int
duplicate_term(term_t in, term_t copy ARG_LD)
{ return copy_term_refs(in, copy, COPY_ATTRS PASS_LD);
}


static
PRED_IMPL("duplicate_term", 2, duplicate_term, 0)
{ PRED_LD

  if ( PL_is_atomic(A1) )
  { return PL_unify(A1, A2);
  } else
  { term_t copy = PL_new_term_ref();

    if ( duplicate_term(A1, copy PASS_LD) )
      return PL_unify(copy, A2);

    fail;
  }
}


static
PRED_IMPL("copy_term_nat", 2, copy_term_nat, 0)
{ PRED_LD
  term_t copy = PL_new_term_ref();

  if ( copy_term_refs(A1, copy, COPY_SHARE PASS_LD) )
    return PL_unify(copy, A2);

  fail;
}

		 /*******************************
		 *	       ATOMS		*
		 *******************************/

static
PRED_IMPL("atom_length", 2, atom_length, PL_FA_ISO)
{ PRED_LD
  int flags;
  PL_chars_t txt;

  if ( truePrologFlag(PLFLAG_ISO) )
    flags = CVT_ATOM|CVT_STRING|CVT_EXCEPTION;	/* strings are not known to ISO */
  else
    flags = CVT_ALL|CVT_EXCEPTION;

  if ( PL_get_text(A1, &txt, flags) )
    return PL_unify_int64_ex(A2, txt.length);

  fail;
}


#define X_AUTO   0x00
#define X_ATOM   0x01
#define X_NUMBER 0x02
#define X_MASK   0x0f
#define X_CHARS  0x10

static int
x_chars(const char *pred, term_t atom, term_t string, int how ARG_LD)
{ PL_chars_t atext, stext;
  int arg1;
  int flags2 = CVT_STRING|CVT_LIST|CVT_EXCEPTION;

  arg1 = PL_get_text(atom, &atext,
		     (how & X_NUMBER) ? CVT_NUMBER : CVT_ATOMIC);

  if ( arg1 )
  { int ok;
    fid_t fid = PL_open_foreign_frame();

    ok = PL_unify_text(string, 0, &atext,
		       (how & X_CHARS) ? PL_CHAR_LIST : PL_CODE_LIST);

    if ( ok || !(how & X_NUMBER) )
    { PL_close_foreign_frame(fid);
      return ok;
    }
    flags2 |= CVT_VARNOFAIL;
    PL_discard_foreign_frame(fid);
  } else if ( !PL_is_variable(atom) )
  { return PL_error(pred, 2, NULL, ERR_TYPE,
		    (how & X_NUMBER) ? ATOM_number : ATOM_atom,
		    atom);
  }

  if ( PL_get_text(string, &stext, flags2) != TRUE )
    return FALSE;

  how &= X_MASK;

  switch(how)
  { case X_ATOM:
    case_atom:
      return PL_unify_text(atom, 0, &stext, PL_ATOM);
    case X_NUMBER:
    case X_AUTO:
    { strnumstat rc = NUM_ERROR;

      if ( stext.encoding == ENC_ISO_LATIN_1 )
      { unsigned char *q, *s = (unsigned char *)stext.text.t;
	number n;
	AR_CTX;

	if ( how == X_NUMBER )
	{ if ( s )				/* ISO: number_codes(X, "  42") */
	  { while(*s && isBlankW(*s))
	      s++;
	  }
	}

	AR_BEGIN();
	if ( s && (rc=str_number(s, &q, &n, FALSE)) == NUM_OK )
	{ if ( *q == EOS )
	  { int rc = PL_unify_number(atom, &n);
	    clearNumber(&n);
	    AR_END();
	    return rc;
	  }
	  clearNumber(&n);
	}
	AR_END();
      }

      if ( how == X_AUTO )
	goto case_atom;
      else
	return PL_error(pred, 2, NULL, ERR_SYNTAX, str_number_error(rc));
    }
    default:
      assert(0);
      return FALSE;
  }
}


static
PRED_IMPL("name", 2, name, 0)
{ PRED_LD
  return x_chars("name", A1, A2, X_AUTO PASS_LD);
}


static
PRED_IMPL("atom_chars", 2, atom_chars, PL_FA_ISO)
{ PRED_LD
  return x_chars("atom_chars", A1, A2, X_ATOM|X_CHARS PASS_LD);
}


static
PRED_IMPL("atom_codes", 2, atom_codes, PL_FA_ISO)
{ PRED_LD
  return x_chars("atom_codes", A1, A2, X_ATOM PASS_LD);
}


static
PRED_IMPL("number_chars", 2, number_chars, PL_FA_ISO)
{ PRED_LD
  return x_chars("number_chars", A1, A2, X_NUMBER|X_CHARS PASS_LD);
}


static
PRED_IMPL("number_codes", 2, number_codes, PL_FA_ISO)
{ PRED_LD
  return x_chars("number_chars", A1, A2, X_NUMBER PASS_LD);
}


static
PRED_IMPL("char_code", 2, char_code, PL_FA_ISO)
{ PRED_LD
  PL_chars_t txt;
  int n;
  term_t atom = A1;
  term_t chr  = A2;
  int vatom = PL_is_variable(atom);
  int vchr  = PL_is_variable(chr);
  int achr = -1;
  int cchr = -1;

  if ( vatom && vchr )
    return PL_error(NULL, 0, NULL, ERR_INSTANTIATION);

  if ( !vatom )
  { if ( PL_get_text(atom, &txt, CVT_ATOM|CVT_STRING) && txt.length == 1 )
    { if ( txt.encoding == ENC_WCHAR )
	achr = txt.text.w[0];
      else
	achr = txt.text.t[0]&0xff;
    } else
    { return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_character, atom);
    }
  }

  if ( !vchr )
  { if ( !PL_get_integer_ex(chr, &n) )
      fail;

    if ( n >= 0 && n <= 0x10ffff )
      cchr = n;
    else
      return PL_error(NULL, 0, NULL, ERR_REPRESENTATION, ATOM_character_code);
  }

  if ( achr == cchr )
    succeed;
  if ( vatom )
    return PL_unify_atom(atom, codeToAtom(cchr));
  else
    return PL_unify_integer(chr, achr);
}


static
PRED_IMPL("atom_number", 2, atom_number, 0)
{ PRED_LD
  AR_CTX
  char *s;
  size_t len;

  if ( PL_get_nchars(A1, &len, &s, CVT_ATOM|CVT_STRING) )
  { number n;
    unsigned char *q;
    strnumstat rc;

    AR_BEGIN();

    if ( (rc=str_number((unsigned char *)s, &q, &n, FALSE) == NUM_OK) )
    { if ( *q == EOS )
      { int rc = PL_unify_number(A2, &n);
        clearNumber(&n);

        AR_END();
        return rc;
      }
      else
      { clearNumber(&n);
        AR_END();
        return PL_error(NULL, 0, NULL, ERR_SYNTAX, "illegal_number");
      }
    } else
    { AR_END();
      return PL_error(NULL, 0, NULL, ERR_SYNTAX, str_number_error(rc));
    }
  } else if ( PL_get_nchars(A2, &len, &s, CVT_NUMBER) )
  { return PL_unify_atom_nchars(A1, len, s);
  }

  if ( !PL_is_variable(A2) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_number, A2);
  else
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_atom, A1);
}


static
PRED_IMPL("collation_key", 2, collation_key, 0)
{
#ifdef HAVE_WCSXFRM
  wchar_t *s;
  size_t len;
  wchar_t buf[256];
  size_t buflen = sizeof(buf)/sizeof(wchar_t);
  wchar_t *o = buf;
  size_t n;

  if ( !PL_get_wchars(A1, &len, &s, CVT_ATOM|CVT_STRING|CVT_EXCEPTION) )
    fail;
  for(;;)
  { if ( (n=wcsxfrm(o, s, buflen)) < buflen )
    { int rc = PL_unify_wchars(A2, PL_STRING, n, o);

      if ( o != buf )
	PL_free(o);

      return rc;
    } else
    { assert(o == buf);
      buflen = n+1;
      o = PL_malloc(buflen*sizeof(wchar_t));
    }
  }
#else
  GET_LD
  return PL_unify(A1, A2);
#endif
}

static word
concat(term_t a1, term_t a2, term_t a3,
       int bidirectional,		/* FALSE: only mode +,+,- */
       control_t ctx,
       int accept,			/* CVT_* */
       int otype ARG_LD)		/* PL_ATOM or PL_STRING */
{ PL_chars_t t1, t2, t3;
  int rc;
  int inmode = bidirectional ? CVT_VARNOFAIL : 0;

#define L1 t1.length
#define L2 t2.length
#define L3 t3.length

  if ( ForeignControl(ctx) == FRG_CUTTED )
    succeed;

  t1.text.t = t2.text.t = t3.text.t = NULL;

  if ( !PL_get_text(a1, &t1, accept|inmode|CVT_EXCEPTION) ||
       !PL_get_text(a2, &t2, accept|inmode|CVT_EXCEPTION) ||
       !PL_get_text(a3, &t3, accept|CVT_EXCEPTION|CVT_VARNOFAIL) )
    fail;

  if ( t1.text.t && t2.text.t )
  { if ( t3.text.t )
    { rc = ( t1.length + t2.length == t3.length &&
	     PL_cmp_text(&t1, 0, &t3, 0, t1.length) == 0 &&
	     PL_cmp_text(&t2, 0, &t3, t1.length, t2.length) == 0 );
      goto out;
    } else
    { PL_chars_t c;
      PL_chars_t *v[2];

      v[0] = &t1;
      v[1] = &t2;

      PL_concat_text(2, v, &c);

      rc = PL_unify_text(a3, 0, &c, otype);
      PL_free_text(&c);
      goto out;
    }
  }

  if ( !t3.text.t )
    return PL_error(NULL, 0, NULL, ERR_INSTANTIATION);

  if ( t1.text.t )			/* +, -, + */
  { if ( L1 <= L3 &&
	 PL_cmp_text(&t1, 0, &t3, 0, L1) == 0 )
      return PL_unify_text_range(a2, &t3, L1, L3-L1, otype);
    fail;
  } else if ( t2.text.t )		/* -, +, + */
  { if ( L2 <= L3 &&
	 PL_cmp_text(&t2, 0, &t3, L3-L2, L2) == 0 )
      return PL_unify_text_range(a1, &t3, 0, L3-L2, otype);
    fail;
  } else				/* -, -, + */
  { size_t at_n;

    switch ( ForeignControl(ctx) )
    { case FRG_FIRST_CALL:
        if ( PL_same_term(a1, a2) )	/* sharing variables */
	{ if ( L3 % 2 )
	  { rc = FALSE;
	    goto out;
	  } else
	  { at_n = L3/2;
	    if ( PL_cmp_text(&t3, 0, &t3, at_n, at_n) == 0 )
	    { PL_unify_text_range(a1, &t3, 0, at_n, otype);
	      rc = TRUE;
	    } else
	    { rc = FALSE;
	    }
	    goto out;
	  }
	}
	at_n = 0;
        break;
      case FRG_REDO:
	at_n = ForeignContextInt(ctx);
        break;
      default:
	succeed;
    }

    PL_unify_text_range(a2, &t3, at_n, L3-at_n, otype);
    PL_unify_text_range(a1, &t3, 0,    at_n, otype);
    if ( at_n < L3 )
      ForeignRedoInt(at_n+1);

    rc = TRUE;
  }

out:
  if ( t1.text.t ) PL_free_text(&t1);
  if ( t2.text.t ) PL_free_text(&t2);
  if ( t3.text.t ) PL_free_text(&t3);

#undef L1
#undef L2
#undef L3

  return rc;
}


static
PRED_IMPL("atom_concat", 3, atom_concat, PL_FA_NONDETERMINISTIC|PL_FA_ISO)
{ PRED_LD

  return concat(A1, A2, A3, TRUE, PL__ctx, CVT_ATOMIC, PL_ATOM PASS_LD);
}


static
PRED_IMPL("atomic_concat", 3, atomic_concat, PL_FA_ISO)
{ PRED_LD

  return concat(A1, A2, A3, FALSE, PL__ctx, CVT_ATOMIC, PL_ATOM PASS_LD);
}


static int
split_atom(term_t list, term_t sep, term_t atom ARG_LD)
{ PL_chars_t st, at;
  size_t i, last;
  term_t tail = PL_copy_term_ref(list);
  term_t head = PL_new_term_ref();

  if ( !sep )
    return -1;
  if ( !PL_get_text(atom, &at, CVT_ATOMIC) ||
       !PL_get_text(sep, &st, CVT_ATOMIC) )
    return -1;

  for(last=i=0; (ssize_t)i<=(ssize_t)(at.length-st.length); )
  { if ( PL_cmp_text(&st, 0, &at, i, st.length) == 0 )
    { if ( !PL_unify_list(tail, head, tail) ||
	   !PL_unify_text_range(head, &at, last, i-last, PL_ATOM) )
	fail;
      i += st.length;
      last = i;
    } else
      i++;
  }

  if ( !PL_unify_list(tail, head, tail) ||
       !PL_unify_text_range(head, &at, last, at.length-last, PL_ATOM) )
    fail;

  return PL_unify_nil(tail);
}


static void
append_text_to_buffer(Buffer b, PL_chars_t *txt, IOENC *enc)
{ if ( txt->encoding == *enc )
  { if ( txt->encoding == ENC_ISO_LATIN_1 )
    { addMultipleBuffer(b, txt->text.t, txt->length, char);
    } else
    { addMultipleBuffer(b, txt->text.w, txt->length, pl_wchar_t);
    }
  } else if ( txt->encoding == ENC_ISO_LATIN_1 )
  { const unsigned char *s = (const unsigned char*)txt->text.t;
    const unsigned char *e = &s[txt->length];

    for( ;s<e; s++)
    { pl_wchar_t chr = *s;

      addBuffer(b, chr, pl_wchar_t);
    }
  } else				/* promote our buffer */
  { size_t len = entriesBuffer(b, char);
    unsigned char *tmp = PL_malloc(len);
    const unsigned char *s = tmp;
    const unsigned char *e = &s[len];

    memcpy(tmp, baseBuffer(b, char), len);
    discardBuffer(b);
    initBuffer(b);

    for( ;s<e; s++)
    { pl_wchar_t chr = *s;

      addBuffer(b, chr, pl_wchar_t);
    }

    PL_free(tmp);
    *enc = ENC_WCHAR;
					/* and add new text */
    addMultipleBuffer(b, txt->text.w, txt->length, pl_wchar_t);
  }
}


static foreign_t
atomic_list_concat(term_t list, term_t sep, term_t atom ARG_LD)
{ term_t l = PL_copy_term_ref(list);
  term_t head = PL_new_term_ref();
  IOENC enc = ENC_ISO_LATIN_1;
  tmp_buffer b;
  PL_chars_t st;			/* separator text */
  int ntxt = 0;

  if ( sep && !PL_get_text(sep, &st, CVT_ATOMIC) )
    return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_text, sep);

  initBuffer(&b);
  while( PL_get_list(l, head, l) )
  { PL_chars_t txt;

    if ( !PL_get_text(head, &txt, CVT_ATOMIC) )
    { discardBuffer(&b);
      switch(split_atom(list, sep, atom PASS_LD))
      { case -1:
	  return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_text, head);
	case 0:
	  fail;
	default:
	  succeed;
      }
    }

    if ( ntxt > 0 && sep )
      append_text_to_buffer((Buffer)&b, &st, &enc);

    append_text_to_buffer((Buffer)&b, &txt, &enc);
    PL_free_text(&txt);
    ntxt++;
  }

  if ( PL_get_nil(l) )
  { PL_chars_t sum;
    int rc;

    sum.encoding  = enc;
    sum.storage   = PL_CHARS_HEAP;
    sum.canonical = TRUE;

    if ( enc == ENC_ISO_LATIN_1 )
    { sum.text.t = baseBuffer(&b, char);
      sum.length = entriesBuffer(&b, char);
    } else
    { sum.text.w = baseBuffer(&b, pl_wchar_t);
      sum.length = entriesBuffer(&b, pl_wchar_t);
    }

    rc = PL_unify_text(atom, 0, &sum, PL_ATOM);
    discardBuffer(&b);

    return rc;
  }

  discardBuffer(&b);
  switch(split_atom(list, sep, atom PASS_LD))
  { case -1:
      return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_list, l);
    case 0:
      fail;
    default:
      succeed;
  }
}


static
PRED_IMPL("atomic_list_concat", 3, atomic_list_concat, 0)
{ PRED_LD
  return atomic_list_concat(A1, A2, A3 PASS_LD);
}


static
PRED_IMPL("atomic_list_concat", 2, atomic_list_concat, 0)
{  PRED_LD
  return atomic_list_concat(A1, 0, A2 PASS_LD);
}


word
pl_apropos_match(term_t a1, term_t a2)
{ char *s1=NULL, *s2=NULL;
  pl_wchar_t *w1=NULL, *w2=NULL;
  size_t l1, l2;

  if ( PL_get_chars(a1, &s1, CVT_ALL|BUF_RING) &&
       PL_get_chars(a2, &s2, CVT_ALL) )
  { char *s, *q;

    for (; *s2; s2++)
    { for(q=s1, s=s2; *q && *s; q++, s++)
      { if ( *q != *s && *q != toLower(*s) )
	  break;
      }
      if ( *q == EOS )
	succeed;
    }
    fail;
  }
  if ( PL_get_wchars(a1, &l1, &w1, CVT_ALL|BUF_RING) &&
       PL_get_wchars(a2, &l2, &w2, CVT_ALL) )
  { pl_wchar_t *s, *q;
    pl_wchar_t *eq = &w1[l1];
    pl_wchar_t *es = &w2[l2];
    unsigned int i2;

    for (i2=0; i2<l2; i2++)
    { for(q=w1, s=w2+i2; q<eq && s<es; q++, s++)
      { if ( *q != *s && *q != (pl_wchar_t)towlower(*s) )
	  break;
      }
      if ( q == eq )
	succeed;
    }
    fail;
  }

  return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_text, (s1||w1) ? a2 : a1);
}


/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
ISO compliant hacking  into  atoms.  The   state  is  represented  by  a
`redo-int', of which we use the first 15   bits for the `before' and the
second 15 bits for the `after'.

There are many possibilities (think the semantics are a bit overloaded).

    * sub is given
        + if len conflicts: fail
	+ if before or after given: test deterministically
	+ otherwise: search (non-deterministic)
    * two of the integers are given
        + generate (deterministic)
    * before is given:
        + split the remainder (non-deterministic)
    * len is given:
        + enumerate breaks (non-deterministic)
    * after is given:
        + split the remainder (non-deterministic)
    * non given:
        + enumerate using before and len (non-deterministic)
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

enum sub_type
{ SUB_SEARCH,				/* sub given, but no position */
  SUB_SPLIT_TAIL,			/* before given, split tail */
  SUB_SPLIT_HEAD,			/* after given, split head */
  SUB_SPLIT_LEN,			/* len given, move it */
  SUB_ENUM				/* all free */
};

typedef struct
{ enum sub_type type;			/* Type of enumeration */
  size_t n1;				/* 1-st state id */
  size_t n2;				/* 2-nd state id */
  size_t n3;
} sub_state;


static int
get_positive_integer_or_unbound(term_t t, ssize_t *v ARG_LD)
{ long i;

  if ( PL_get_long(t, &i) )		/* TBD: should be ssize_t */
  { if ( i < 0 )
      PL_error(NULL, 0, NULL, ERR_DOMAIN,
	       ATOM_not_less_than_zero, t);
    *v = i;

    return TRUE;
  }

  if ( PL_is_variable(t) )
    return TRUE;

  return PL_error(NULL, 0, NULL, ERR_TYPE, ATOM_integer, t);
}



static foreign_t
sub_text(term_t atom,
	 term_t before, term_t len, term_t after,
	 term_t sub,
	 control_t h,
	 int type			/* PL_ATOM or PL_STRING */
	 ARG_LD)
{ PL_chars_t ta, ts;			/* the strings */
  ssize_t b = -1, l = -1, a = -1;	/* the integers */
  sub_state *state;			/* non-deterministic state */
  atom_t expected = (type == PL_STRING ? ATOM_string : ATOM_atom);
  int match;
  fid_t fid;

#define la ta.length
#define ls ts.length

  switch( ForeignControl(h) )
  { case FRG_FIRST_CALL:
    { if ( !PL_get_text(atom, &ta, CVT_ATOMIC) )
	return PL_error(NULL, 0, NULL, ERR_TYPE, expected, atom);

      if ( !get_positive_integer_or_unbound(before, &b PASS_LD) ||
	   !get_positive_integer_or_unbound(len, &l PASS_LD) ||
	   !get_positive_integer_or_unbound(after, &a PASS_LD) )
	fail;

      if ( !PL_get_text(sub, &ts, CVT_ATOMIC) )
      { if ( !PL_is_variable(sub) )
	  return PL_error(NULL, 0, NULL, ERR_TYPE, expected, sub);
	ts.text.t = NULL;
      }

      if ( ts.text.t )			/* `sub' given */
      { if ( l >= 0 && (int)ls != l )	/* len conflict */
	  fail;
	if ( b >= 0 )			/* before given: test */
	{ if ( PL_cmp_text(&ta, b, &ts, 0, ls) == 0 )
	  { return (PL_unify_integer(len, ls) &&
		    PL_unify_integer(after, la-ls-b)) ? TRUE : FALSE;
	  }
	  fail;
	}
	if ( a >= 0 )			/* after given: test */
	{ ssize_t off = la-a-ls;

	  if ( off >= 0 && PL_cmp_text(&ta, (unsigned)off, &ts, 0, ls) == 0 )
	  { return (PL_unify_integer(len, ls) &&
		    PL_unify_integer(before, off)) ? TRUE : FALSE;
	  }
	  fail;
	}
	state = allocHeap(sizeof(*state));
	state->type = SUB_SEARCH;
	state->n1   = 0;
	state->n2   = la;
	state->n3   = ls;
	break;
      }

      if ( b >= 0 )			/* before given */
      { if ( b > (int)la )
	  fail;

	if ( l >= 0 )			/* len given */
	{ if ( b+l <= (int)la )		/* deterministic fit */
	  { if ( PL_unify_integer(after, la-b-l) &&
		 PL_unify_text_range(sub, &ta, b, l, type) )
	      succeed;
	  }
	  fail;
	}
	if ( a >= 0 )			/* after given */
	{ if ( (l = la-a-b) >= 0 )
	  { if ( PL_unify_integer(len, l) &&
		 PL_unify_text_range(sub, &ta, b, l, type) )
	      succeed;
	  }

	  fail;
	}
	state = allocHeap(sizeof(*state));
	state->type = SUB_SPLIT_TAIL;
	state->n1   = 0;		/* len of the split */
	state->n2   = la;		/* length of the atom */
	state->n3   = b;		/* length before */
	break;
      }

      if ( l >= 0 )			/* no before, len given */
      { if ( l > (int)la )
	  fail;

	if ( a >= 0 )			/* len and after */
	{ if ( (b = la-a-l) >= 0 )
	  { if ( PL_unify_integer(before, b) &&
		 PL_unify_text_range(sub, &ta, b, l, type) )
	      succeed;
	  }

	  fail;
	}
	state = allocHeap(sizeof(*state));
	state->type = SUB_SPLIT_LEN;
	state->n1   = 0;		/* before */
	state->n2   = l;		/* length */
	state->n3   = la;
	break;
      }

      if ( a >= 0 )			/* only after given */
      { if ( a > (int)la )
	  fail;

	state = allocHeap(sizeof(*state));
	state->type = SUB_SPLIT_HEAD;
	state->n1   = 0;		/* before */
	state->n2   = la;
	state->n3   = a;
	break;
      }

      state = allocHeap(sizeof(*state));
      state->type = SUB_ENUM;
      state->n1	= 0;			/* before */
      state->n2 = 0;			/* len */
      state->n3 = la;			/* total length */
      break;
    }
    case FRG_REDO:
      state = ForeignContextPtr(h);
      PL_get_text(atom, &ta, CVT_ATOMIC);
      break;
    case FRG_CUTTED:
    exit_succeed:
      state = ForeignContextPtr(h);
      if ( state )
	freeHeap(state, sizeof(*state));
      succeed;
    default:
      assert(0);
      fail;
  }

  fid = PL_open_foreign_frame();
again:
  switch(state->type)
  { case SUB_SEARCH:
    { PL_get_text(sub, &ts, CVT_ATOMIC);
      la = state->n2;
      ls = state->n3;

      for( ; state->n1+ls <= la; state->n1++ )
      { if ( PL_cmp_text(&ta, state->n1, &ts, 0, ls) == 0 )
	{ match = (PL_unify_integer(before, state->n1) &&
		   PL_unify_integer(len,    ls) &&
		   PL_unify_integer(after,  la-ls-state->n1));

	  state->n1++;
	  goto next;
	}
      }
      goto exit_fail;
    }
    case SUB_SPLIT_TAIL:		/* before given, rest unbound */
    { la = state->n2;
      b  = state->n3;
      l  = state->n1++;

      match = (PL_unify_integer(len, l) &&
	       PL_unify_integer(after, la-b-l));
    out:
      match = (match && PL_unify_text_range(sub, &ta, b, l, type));
      if ( b+l < (int)la )
	goto next;
      else if ( match )
	goto exit_succeed;
      else
	goto exit_fail;
    }
    case SUB_SPLIT_LEN:
    { b  = state->n1++;
      l  = state->n2;
      la = state->n3;

      match = (PL_unify_integer(before, b) &&
	       PL_unify_integer(after, la-b-l));
      goto out;
    }
    case SUB_SPLIT_HEAD:
    { b  = state->n1++;
      la = state->n2;
      a  = state->n3;
      l  = la - a - b;

      match = (PL_unify_integer(before, b) &&
	       PL_unify_integer(len, l) &&
	       PL_unify_text_range(sub, &ta, b, l, type));
      if ( l > 0 )
	goto next;
      else if ( match )
	goto exit_succeed;
      else
	goto exit_fail;
    }
    case SUB_ENUM:
    { b  = state->n1;
      l  = state->n2++;
      la = state->n3;
      a  = la-b-l;

      match = (PL_unify_integer(before, b) &&
	       PL_unify_integer(len, l) &&
	       PL_unify_integer(after, a) &&
	       PL_unify_text_range(sub, &ta, b, l, type));
      if ( a == 0 )
      { if ( b == (int)la )
	{ if ( match )
	    goto exit_succeed;
	  else
	    goto exit_fail;
	}
	state->n2 = 0;
	state->n1++;
      }
      goto next;
    }
  }

exit_fail:
  freeHeap(state, sizeof(*state));
  fail;

next:
  if ( match )
  { ForeignRedoPtr(state);
  } else
  { PL_rewind_foreign_frame(fid);
    goto again;
  }

#undef la
#undef ls
}


foreign_t
pl_sub_atom(term_t atom,
	    term_t before, term_t len, term_t after,
	    term_t sub,
	    control_t h)
{ GET_LD
  return sub_text(atom, before, len, after, sub, h, PL_ATOM PASS_LD);
}


#if O_STRING
/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Provisional String manipulation functions.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static
PRED_IMPL("string_length", 2, string_length, 0)
{ PRED_LD
  PL_chars_t t;

  if ( PL_get_text(A1, &t, CVT_ALL|CVT_EXCEPTION) )
  { int rc = PL_unify_int64_ex(A2, t.length);

    PL_free_text(&t);

    return rc;
  }

  fail;
}


static
PRED_IMPL("string_concat", 3, string_concat, PL_FA_NONDETERMINISTIC)
{ PRED_LD

  return concat(A1, A2, A3, TRUE, PL__ctx, CVT_ATOMIC, PL_STRING PASS_LD);
}


static
PRED_IMPL("string_to_atom", 2, string_to_atom, 0)
{ PRED_LD
  term_t str = A1;
  term_t a = A2;
  PL_chars_t t;
  int rc;

  if ( PL_get_text(str, &t, CVT_ALL) )
    rc = PL_unify_text(a, 0, &t, PL_ATOM);
  else if ( PL_get_text(a, &t, CVT_ALL) )
    rc = PL_unify_text(str, 0, &t, PL_STRING);
  else
    return PL_error(NULL, 0, NULL, ERR_INSTANTIATION);

  PL_free_text(&t);

  return rc;
}


static
PRED_IMPL("string_to_list", 2, string_to_list, 0)
{ PRED_LD
  term_t str = A1;
  term_t list = A2;
  PL_chars_t t;
  int rc;

  if ( PL_get_text(str, &t, CVT_ALL) )
    rc = PL_unify_text(list, 0, &t, PL_CODE_LIST);
  else if ( PL_get_text(list, &t, CVT_STRING|CVT_LIST) ) /* -, [] */
    rc = PL_unify_text(str, 0, &t, PL_STRING);
  else if ( PL_get_text(list, &t, CVT_ALL) )
    rc = PL_unify_text(str, 0, &t, PL_STRING);
  else return PL_error(NULL, 0, NULL, ERR_INSTANTIATION);

  PL_free_text(&t);

  return rc;
}


foreign_t
pl_sub_string(term_t atom,
	      term_t before, term_t len, term_t after,
	      term_t sub,
	      control_t h)
{ GET_LD
  return sub_text(atom, before, len, after, sub, h, PL_STRING PASS_LD);
}

#endif /* O_STRING */


		/********************************
		*            CONTROL            *
		*********************************/

word
pl_repeat(control_t h)
{ switch( ForeignControl(h) )
  { case FRG_FIRST_CALL:
    case FRG_REDO:
      ForeignRedoInt(2L);
    case FRG_CUTTED:
    default:
      succeed;
  }
}

word
pl_fail()		/* just to define it */
{ fail;
}

word
pl_true()		/* just to define it */
{ succeed;
}

word
pl_halt(term_t code)
{ int status;

#ifdef O_PLMT
  if ( PL_thread_self() != 1 )
  { GET_LD
    term_t t = PL_new_term_ref();

    pl_thread_self(t);
    return PL_error("halt", 1, "Only from thread `main'",
		    ERR_PERMISSION,
		    ATOM_halt, ATOM_thread, t);
  }
#endif

  if ( !PL_get_integer_ex(code, &status) )
    fail;

  PL_halt(status);
  /*NOTREACHED*/
  fail;
}

#ifdef O_LIMIT_DEPTH

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
The    predicates    below    provide      the     infrastructure    for
call_with_depth_limit/3. This predicate was included on request by Steve
Moyle, for improving the implementation of a theorem prover.

The implementation of call_with_depth_limit/3 in pl-prims.pl is

================================================================
call_with_depth_limit(G, Limit, Result) :-
	$depth_limit(Limit, OLimit, OReached),
	(   catch(G, E, depth_limit_except(OLimit, OReached, E)),
	    $depth_limit_true(Limit, OLimit, OReached, Result, Cut),
	    Cut
	;   $depth_limit_false(OLimit, OReached, Result)
	).
================================================================

$depth_limit/3 sets the new limit and fetches the old values so they can
be restored by the other calls.   '$depth_limit_true'/5 restores the old
limits, and unifies Result with  the   maximum  depth reached during the
proof. Cut is unified  with  !   if  G  succeeded deterministically, and
`true' otherwise and  ensures  the   wrapper  maintains  the determistic
properties of G. It can be debated whether this is worthwhile ...

Finally, '$depth_limit_false'/4 checks for a depth-overflow, and unifies
result with `depth_limit_exceeded' if an overflow  has occurred and just
fails otherwise. Of course it always restores the outer environment.

Note that call_with_depth_limit/3 cannot be written  as a simple foreign
call using PL_open_query(), etc, as   the non-deterministic predicate is
not allowed to return to  the   parent  environment  without closing the
query.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

/* $depth_limit(+Limit, -OldLimit, -DepthReached)
*/

static
PRED_IMPL("$depth_limit", 3, pl_depth_limit, 0)
{ GET_LD
  long levels;
  long clevel = levelFrame(environment_frame) - 1;

  if ( PL_get_long_ex(A1, &levels) )
  { if ( PL_unify_integer(A2, depth_limit) &&
	 PL_unify_integer(A3, depth_reached) )
    { depth_limit   = clevel + levels + 1; /* 1 for the catch/3 */
      depth_reached = clevel;

      updateAlerted(LD);
      succeed;
    }
  }

  fail;
}


static
PRED_IMPL("$depth_limit_true", 5, pl_depth_limit_true, PL_FA_NONDETERMINISTIC)
{ term_t limit = A1;
  term_t olimit = A2;
  term_t oreached = A3;
  term_t res = A4;
  term_t cut = A5;

  switch( CTX_CNTRL )
  { case FRG_FIRST_CALL:
    { GET_LD
      long l, ol, or;

      if ( PL_get_long_ex(limit, &l) &&
	   PL_get_long_ex(olimit, &ol) &&
	   PL_get_long_ex(oreached, &or) )
      { intptr_t clevel = levelFrame(environment_frame) - 1;
	intptr_t used = depth_reached - clevel - 1;
	Choice ch;

	depth_limit   = ol;
	depth_reached = or;
	updateAlerted(LD);

	if ( used < 1 )
	  used = 1;
	if ( !PL_unify_integer(res, used) )
	  fail;

	for(ch=LD->choicepoints; ch; ch = ch->parent)
	{ if ( ch->frame == environment_frame )
	    continue;			/* choice from I_FOPENNDET */
	  switch(ch->type)
	  { case CHP_CATCH:
	    case CHP_DEBUG:
	      continue;
	    default:
	      break;
	  }
	  break;
	}

	if ( ch && ch->frame == environment_frame->parent )
	{ DEBUG(1, Sdprintf("CUT\n"));
	  return PL_unify_atom(cut, ATOM_cut);
	} else
	{ if ( PL_unify_atom(cut, ATOM_true) )
	    ForeignRedoInt(1);
	}
      }

      break;
    }
    case FRG_REDO:
    { GET_LD
      long levels;
      long clevel = levelFrame(environment_frame) - 1;

      PL_get_long_ex(limit, &levels);
      depth_limit   = clevel + levels + 1; /* 1 for catch/3 */
      depth_reached = clevel;
      updateAlerted(LD);

      fail;				/* backtrack to goal */
    }
    case FRG_CUTTED:
      succeed;
  }

  fail;
}


static
PRED_IMPL("$depth_limit_false", 3, depth_limit_false, 0)
{ PRED_LD
  long ol, or;

  if ( PL_get_long_ex(A1, &ol) &&
       PL_get_long_ex(A2, &or) )
  { int exceeded = (depth_reached > depth_limit);

    depth_limit   = ol;
    depth_reached = or;
    updateAlerted(LD);

    if ( exceeded )
      return PL_unify_atom(A3, ATOM_depth_limit_exceeded);
  }

  fail;
}


static
PRED_IMPL("$depth_limit_except", 3, depth_limit_except, 0)
{ PRED_LD
  long ol, or;

  if ( PL_get_long_ex(A1, &ol) &&
       PL_get_long_ex(A2, &or) )
  { depth_limit   = ol;
    depth_reached = or;
    updateAlerted(LD);

    return PL_raise_exception(A3);
  }

  fail;
}

#endif /*O_LIMIT_DEPTH*/


		/********************************
		*          STATISTICS           *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
Fetch runtime statistics. There are two standards  here. One is based on
old C-Prolog compatibility, exended as required   by  SWI-Prolog and the
other  is  defined  by  Quintus/SICStus.  The   latter  is  included  if
QP_STATISTICS is defined. The compatibility   is pretty complete, except
the `atoms' key that is defined by both and this ambiguous.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

static size_t
heapUsed(void)
{ size_t heap = GD->statistics.heap;	/* Big allocations */

  heap += GD->alloc_pool.allocated;	/* global small allocations */
#ifdef O_PLMT
  heap += threadLocalHeapUsed();	/* thread-local small allocations */
#endif

  return heap;
}

static size_t
CStackSize(PL_local_data_t *ld)
{
#ifdef O_PLMT
  if ( ld->thread.info->pl_tid != 1 )
  { DEBUG(1, Sdprintf("Thread-stack: %ld\n", ld->thread.info->stack_size));
    return ld->thread.info->stack_size;
  }
#endif
#ifdef HAVE_GETRLIMIT
{ struct rlimit rlim;

  if ( getrlimit(RLIMIT_STACK, &rlim) == 0 )
  { DEBUG(1, Sdprintf("Stack: %ld\n", rlim.rlim_cur));
    return rlim.rlim_cur;
  }
}
#endif

  return 0;
}

#define QP_STATISTICS 1

#ifdef QP_STATISTICS

static int
#ifdef O_PLMT
qp_statistics__LD(atom_t key, int64_t v[], PL_local_data_t *LD)
#else
qp_statistics__LD(atom_t key, int64_t v[], PL_local_data_t *ld)
#endif
{ int vn;

  if ( key == ATOM_runtime )		/* compat: exclude gc-time */
  { v[0] = (int64_t)((LD->statistics.user_cputime -
		      gc_status.time -
		      GD->atoms.gc_time) * 1000.0);
    v[1] = v[0] - LD->statistics.last_cputime;
    LD->statistics.last_cputime = (intptr_t)v[0];
    vn = 2;
  } else if ( key == ATOM_system_time )
  { v[0] = (int64_t)(LD->statistics.system_cputime * 1000.0);
    v[1] = v[0] - LD->statistics.last_systime;
    LD->statistics.last_systime = (intptr_t)v[0];
    vn = 2;
  } else if ( key == ATOM_real_time )
  { v[0] = (int64_t)WallTime();
    v[1] = v[0] - LD->statistics.last_real_time;
    LD->statistics.last_real_time = (intptr_t)v[0];
    vn = 2;
  } else if ( key == ATOM_walltime )
  { double wt = WallTime();
    if ( !LD->statistics.last_walltime )
      LD->statistics.last_walltime = GD->statistics.start_time;
    v[0] = (int64_t)((wt - GD->statistics.start_time) * 1000.0);
    v[1] = (int64_t)((wt - LD->statistics.last_walltime) * 1000.0);
    LD->statistics.last_walltime = wt;
    vn = 2;
  } else if ( key == ATOM_memory || key == ATOM_core )
  { v[0] = UsedMemory();
    v[1] = FreeMemory();
    vn = 2;
  } else if ( key == ATOM_stacks )
  { v[0] = usedStack(global);
    v[1] = usedStack(local);
    vn = 2;
  } else if ( key == ATOM_global_stack )
  { v[0] = usedStack(global);
    v[1] = limitStack(global) - v[0];
    vn = 2;
  } else if ( key == ATOM_local_stack )
  { v[0] = usedStack(local);
    v[1] = limitStack(local) - v[0];
    vn = 2;
  } else if ( key == ATOM_trail )
  { v[0] = usedStack(trail);
    v[1] = 0;
    vn = 2;
  } else if ( key == ATOM_program )
  { v[0] = GD->statistics.heap;
    v[1] = 0;
    vn = 2;
  } else if ( key == ATOM_garbage_collection )
  { vn=0;

    v[vn++] = gc_status.collections;
    v[vn++] = gc_status.trail_gained + gc_status.global_gained;
    v[vn++] = (int64_t)(gc_status.time * 1000.0);
    v[vn++] = gc_status.trail_left + gc_status.global_left;

  } else if ( key == ATOM_stack_shifts )
  {
    v[0] = LD->shift_status.global_shifts;
    v[1] = LD->shift_status.local_shifts;
    v[2] = (int64_t)(LD->shift_status.time * 1000.0);
    vn = 3;
  } else if ( key == ATOM_atoms )
  { v[0] = GD->statistics.atoms;
    v[1] = GD->statistics.atomspace;
    v[2] = 0;
    vn = 3;
  } else if ( key == ATOM_atom_garbage_collection )
  {
#ifdef O_ATOMGC
    v[0] = GD->atoms.gc;
    v[1] = GD->statistics.atomspacefreed;
    v[2] = (int64_t)(GD->atoms.gc_time * 1000.0);
    vn = 3;
#else
    vn = 0;				/* no values */
#endif
  } else
    vn = -1;				/* unknown key */

  return vn;
}

#endif /*QP_STATISTICS*/

static int
#ifdef O_PLMT
swi_statistics__LD(atom_t key, Number v, PL_local_data_t *LD)
#else
swi_statistics__LD(atom_t key, Number v, PL_local_data_t *ld)
#endif
{ v->type = V_INTEGER;			/* most of them */

  if      (key == ATOM_cputime)				/* time */
  { v->type = V_FLOAT;
    v->value.f = LD->statistics.user_cputime;
  } else if (key == ATOM_inferences)			/* inferences */
    v->value.i = LD->statistics.inferences;
  else if (key == ATOM_stack)
    v->value.i = GD->statistics.stack_space;
  else if (key == ATOM_local)				/* local stack */
    v->value.i = sizeStack(local);
  else if (key == ATOM_localused)
    v->value.i = usedStack(local);
  else if (key == ATOM_locallimit)
    v->value.i = limitStack(local);
  else if (key == ATOM_heaplimit)			/* heap */
    fail;
  else if (key == ATOM_heap)
    fail;
  else if (key == ATOM_heapused)			/* heap usage */
    v->value.i = heapUsed();
  else if (key == ATOM_trail)				/* trail */
    v->value.i = sizeStack(trail);
  else if (key == ATOM_trailused)
    v->value.i = usedStack(trail);
  else if (key == ATOM_traillimit)
    v->value.i = limitStack(trail);
  else if (key == ATOM_global)				/* global */
    v->value.i = sizeStack(global);
  else if (key == ATOM_globalused )
    v->value.i = usedStack(global);
  else if (key == ATOM_globallimit)
    v->value.i = limitStack(global);
  else if (key == ATOM_argumentlimit)
    v->value.i = limitStack(argument);
  else if (key == ATOM_c_stack)
    v->value.i = CStackSize(LD);
  else if (key == ATOM_atoms)				/* atoms */
    v->value.i = GD->statistics.atoms;
  else if (key == ATOM_functors)			/* functors */
    v->value.i = GD->statistics.functors;
  else if (key == ATOM_predicates)			/* predicates */
    v->value.i = GD->statistics.predicates;
  else if (key == ATOM_modules)				/* modules */
    v->value.i = GD->statistics.modules;
  else if (key == ATOM_codes)				/* codes */
    v->value.i = GD->statistics.codes;
  else if (key == ATOM_gctime)
  { v->type = V_FLOAT;
    v->value.f = gc_status.time;
  } else if (key == ATOM_collections)
    v->value.i = gc_status.collections;
  else if (key == ATOM_collected)
    v->value.i = gc_status.trail_gained + gc_status.global_gained;
#ifdef O_ATOMGC
  else if (key == ATOM_agc)
    v->value.i = GD->atoms.gc;
  else if (key == ATOM_agc_gained)
    v->value.i = GD->atoms.collected;
  else if (key == ATOM_agc_time)
  { v->type = V_FLOAT;
    v->value.f = GD->atoms.gc_time;
  }
#endif
  else if (key == ATOM_global_shifts)
    v->value.i = LD->shift_status.global_shifts;
  else if (key == ATOM_local_shifts)
    v->value.i = LD->shift_status.local_shifts;
  else if (key == ATOM_trail_shifts)
    v->value.i = LD->shift_status.trail_shifts;
#ifdef O_PLMT
  else if ( key == ATOM_threads )
    v->value.i = GD->statistics.threads_created -
		 GD->statistics.threads_finished;
  else if ( key == ATOM_threads_created )
    v->value.i = GD->statistics.threads_created;
  else if ( key == ATOM_thread_cputime )
  { v->type = V_FLOAT;
    v->value.f = GD->statistics.thread_cputime;
  }
#endif
  else
    return -1;				/* unknown key */

  succeed;
}


int
pl_statistics_ld(term_t k, term_t value, PL_local_data_t *ld ARG_LD)
{ number result;			/* make compiler happy */
  atom_t key;
  int rc;
#ifdef QP_STATISTICS
  int64_t v[4];
#endif

  if ( !PL_get_atom_ex(k, &key) )
    fail;

  if ( !PL_is_list(value) )
  { switch(swi_statistics__LD(key, &result, ld))
    { case TRUE:
	return PL_unify_number(value, &result);
      case FALSE:
	fail;
      case -1:
	break;
    }
  }

#ifdef QP_STATISTICS
  if ( (rc=qp_statistics__LD(key, v, ld)) >= 0 )
  { int64_t *p;
    term_t tail = PL_copy_term_ref(value);
    term_t head = PL_new_term_ref();

    for(p = v; rc-- > 0; p++)
    { if ( !PL_unify_list(tail, head, tail) )
      { if ( PL_unify_nil(tail) )
	  succeed;
	fail;
      }
      if ( !PL_unify_int64(head, *p) )
	fail;
    }

    return PL_unify_nil(tail);
  }
#endif /*QP_STATISTICS*/

  return PL_error("statistics", 2, NULL, ERR_DOMAIN,
		  PL_new_atom("statistics_key"), k);
}


static
PRED_IMPL("statistics", 2, statistics, 0)
{ GET_LD
  atom_t k;

  if ( PL_get_atom(A1, &k) )
  { if ( k == ATOM_cputime || k == ATOM_runtime )
      LD->statistics.user_cputime = CpuTime(CPU_USER);
    else if ( k == ATOM_system_time )
      LD->statistics.system_cputime = CpuTime(CPU_SYSTEM);
  }

  return pl_statistics_ld(A1, A2, LD PASS_LD);
}


#ifdef O_MEMSTATS

#ifndef O_MYALLOC
#define O_MYALLOC 1
#endif

#if O_MYALLOC
static int
addNameValue(term_t list, const char *name, term_t val)
{ GET_LD
  term_t head = PL_new_term_ref();

  if ( !PL_unify_list(list, head, list) )
    return FALSE;
  if ( !PL_unify_term(head, PL_FUNCTOR, FUNCTOR_equals2,
		      PL_CHARS, name, PL_TERM, val) )
    return FALSE;

  PL_reset_term_refs(head);

  return TRUE;
}
#endif

static int
addNameInteger(term_t list, const char *name, intptr_t val)
{ GET_LD
  term_t head = PL_new_term_ref();

  if ( !PL_unify_list(list, head, list) )
    return FALSE;
  if ( !PL_unify_term(head, PL_FUNCTOR, FUNCTOR_equals2,
		      PL_CHARS, name, PL_INTPTR, val) )
    return FALSE;

  PL_reset_term_refs(head);

  return TRUE;
}


/** memory_statistics(-Stats) is det.

Provide statistics on memory  allocation,   and  particularly  on wasted
memory due to not-reused freed memory. Stats   is  a list of Name=Value.
Provided values are:

    * local_pool  = pool(FreeCount1, FreeCount2, ...)
    * global_pool = pool(FreeCount1, FreeCount2, ...)

Where  FreeCount1  is  the  number   of    not-reused   chunks  of  size
sizeof(double); FreeCount2 of size 2*sizeof(double), etc.

If the system provides mallinfo(),  the   values  of  this structure are
added to the list. See "info mallinfo" for a the defined names and their
meaning. Unused values are not included.
*/

static
PRED_IMPL("memory_statistics", 1, memory_statistics, 0)
{ PRED_LD
  term_t tail = PL_copy_term_ref(A1);

#if O_MYALLOC
  term_t val = PL_new_term_ref();

  PL_put_variable(val);
  unifyFreeStatsPool(val, &LD->alloc_pool);
  if ( !addNameValue(tail, "local_pool", val) )
    return FALSE;

  PL_put_variable(val);
  PL_LOCK(L_ALLOC);
  unifyFreeStatsPool(val, &GD->alloc_pool);
  PL_UNLOCK(L_ALLOC);
  if ( !addNameValue(tail, "global_pool", val) )
    return FALSE;
#endif

#ifdef HAVE_MALLINFO
  { struct mallinfo info = mallinfo();

    addNameInteger(tail, "arena", info.arena);
    addNameInteger(tail, "ordblks", info.ordblks);
    addNameInteger(tail, "hblks", info.hblks);
    addNameInteger(tail, "hblkhd", info.hblkhd);
    addNameInteger(tail, "uordblks", info.uordblks);
    addNameInteger(tail, "fordblks", info.fordblks);
    addNameInteger(tail, "keepcost", info.keepcost);
  }
#endif

  return PL_unify_nil(tail);
}
#endif


		/********************************
		*            OPTIONS            *
		*********************************/

/* - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -
$option/3, provides access to the option  structure from Prolog. This is
halfway a generic structure package ... Anyway, it is better then direct
coded access, as the  indirect  approach   allows  us  to  enumerate the
options and generalise the option processing from the saved-states.

See also pl-main.c, which exploits set_pl_option()  to parse the options
resource  member.  Please  note  this   code   doesn't   use   atoms  as
set_pl_option() is called before the Prolog system is initialised.

This code should be moved into another file.
- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - */

typedef struct
{ const char   *name;
  int		type;
  void	       *address;
} optdef, *OptDef;

#define CMDOPT_LONG   0
#define CMDOPT_STRING 1

static const optdef optdefs[] =
{ { "local",		CMDOPT_LONG,	&GD->options.localSize },
  { "global",		CMDOPT_LONG,	&GD->options.globalSize },
  { "trail",		CMDOPT_LONG,	&GD->options.trailSize },

  { "goal",		CMDOPT_STRING,	&GD->options.goal },
  { "toplevel",		CMDOPT_STRING,	&GD->options.topLevel },
  { "init_file",	CMDOPT_STRING,	&GD->options.initFile },
  { "system_init_file",	CMDOPT_STRING,	&GD->options.systemInitFile },
  { "script_file",	CMDOPT_STRING,	&GD->options.scriptFile },
  { "compileout",	CMDOPT_STRING,	&GD->options.compileOut },
  { "class",		CMDOPT_STRING,  &GD->options.saveclass },
  { "home",		CMDOPT_STRING,	&GD->defaults.home },

  { NULL,		0,		NULL }
};


static
PRED_IMPL("$option", 3, option, PL_FA_NONDETERMINISTIC)
{ PRED_LD
  char *k;

  term_t key = A1;
  term_t old = A2;
  term_t new = A3;

  switch( CTX_CNTRL )
  { int index;

    case FRG_FIRST_CALL:
      if ( PL_is_variable(key) )
      { index = 0;

      next:
	for( ; optdefs[index].name; index++ )
	{ switch( optdefs[index].type )
	  { case CMDOPT_LONG:
	    { intptr_t *val = optdefs[index].address;

	      if ( !PL_unify_integer(old, *val) )
		continue;
	      break;
	    }
	    case CMDOPT_STRING:
	    { char **val = optdefs[index].address;

	      if ( !PL_unify_atom_chars(old, *val) )
		continue;
	      break;
	    }
	  }
	  if ( !PL_unify_atom_chars(key, optdefs[index].name) )
	    return FALSE;
	  ForeignRedoInt(index+1);
	}

	fail;
      }
      break;
    case FRG_REDO:
      index = (int)CTX_INT;
      goto next;
    case FRG_CUTTED:
      succeed;
  }

  if ( PL_get_atom_chars(key, &k) )
  { OptDef d = (OptDef)optdefs;

    for( ; d->name; d++ )
    { if ( streq(k, d->name) )
      { switch(d->type)
	{ case CMDOPT_LONG:
	  { long *val = d->address;
	    long newval;

	    if ( !PL_unify_integer(old, *val) ||
		 !PL_get_long(new, &newval) )
	      fail;
	    *val = newval;

	    succeed;
	  }
	  case CMDOPT_STRING:
	  { char **val = d->address;
	    char *newval;

	    if ( !PL_unify_atom_chars(old, *val) ||
		 !PL_get_atom_chars(new, &newval) )
	      fail;

	    if ( !streq(*val, newval) )
	    { remove_string(*val);
	      *val = store_string(newval);
	    }

	    succeed;
	  }
	}
      }
    }
  }

  fail;
}


int
set_pl_option(const char *name, const char *value)
{ OptDef d = (OptDef)optdefs;

  for( ; d->name; d++ )
  { if ( streq(name, d->name) )
    { switch(d->type)
      { case CMDOPT_LONG:
	{ intptr_t *val = d->address;
	  number n;
	  unsigned char *q;

	  if ( str_number((unsigned char *)value, &q, &n, FALSE) == NUM_OK &&
	       *q == EOS &&
	       intNumber(&n) )
	  { *val = (intptr_t)n.value.i;
	    succeed;
	  }
	  fail;
	}
	case CMDOPT_STRING:
	{ char **val = d->address;

	  *val = store_string(value);
	  succeed;
	}
      }
    }
  }

  fail;
}


		/********************************
		*         STYLE CHECK           *
		*********************************/

static
PRED_IMPL("$style_check", 2, style_check, 0)
{ PRED_LD
  int n;

  term_t old = A1;
  term_t new = A2;

  if ( PL_unify_integer(old, debugstatus.styleCheck) &&
       PL_get_integer(new, &n) )
  { debugstatus.styleCheck = n;
    systemMode(n & DOLLAR_STYLE);

    succeed;
  }

  fail;
}


		 /*******************************
		 *	       THROW		*
		 *******************************/

static
PRED_IMPL("throw", 1, throw, 0)
{ PRED_LD

  if ( PL_is_variable(A1) )
    return PL_error(NULL, 0, NULL, ERR_INSTANTIATION);

  return PL_raise_exception(A1);
}


		 /*******************************
		 *      PUBLISH PREDICATES	*
		 *******************************/

BeginPredDefs(prims)
  PRED_DEF("=", 2, unify, PL_FA_ISO)
  PRED_DEF("\\=", 2, not_unify, PL_FA_ISO)
  PRED_DEF("unify_with_occurs_check", 2, unify_with_occurs_check, PL_FA_ISO)
  PRED_DEF("subsumes_term", 2, subsumes_term, 0)
  PRED_DEF("nonvar", 1, nonvar, PL_FA_ISO)
  PRED_DEF("var", 1, var, PL_FA_ISO)
  PRED_DEF("integer", 1, integer, PL_FA_ISO)
  PRED_DEF("float", 1, float, PL_FA_ISO)
  PRED_DEF("rational", 1, rational, 0)
  PRED_DEF("number", 1, number, PL_FA_ISO)
  PRED_DEF("arg", 3, arg, PL_FA_NONDETERMINISTIC|PL_FA_ISO)
  PRED_DEF("atomic", 1, atomic, PL_FA_ISO)
  PRED_DEF("atom", 1, atom, PL_FA_ISO)
  PRED_DEF("string", 1, string, 0)
  PRED_DEF("ground", 1, ground, PL_FA_ISO)
  PRED_DEF("acyclic_term", 1, acyclic_term, 0)
  PRED_DEF("cyclic_term", 1, cyclic_term, 0)
  PRED_DEF("$factorize_term", 2, factorize_term, 0)
  PRED_DEF("compound", 1, compound, PL_FA_ISO)
  PRED_DEF("callable", 1, callable, PL_FA_ISO)
  PRED_DEF("==", 2, equal, PL_FA_ISO)
  PRED_DEF("\\==", 2, nonequal, PL_FA_ISO)
  PRED_DEF("compare", 3, compare, PL_FA_ISO)
  PRED_DEF("@<", 2, std_lt, PL_FA_ISO)
  PRED_DEF("@=<", 2, std_leq, PL_FA_ISO)
  PRED_DEF("@>", 2, std_gt, PL_FA_ISO)
  PRED_DEF("@>=", 2, std_geq, PL_FA_ISO)
  PRED_DEF("=@=", 2, variant, 0)
  PRED_DEF("\\=@=", 2, not_variant, 0)
  PRED_DEF("?=", 2, can_compare, 0)
  PRED_DEF("same_term", 2, same_term, 0)
  PRED_DEF("functor", 3, functor, PL_FA_ISO)
  PRED_DEF("=..", 2, univ, PL_FA_ISO)
  PRED_DEF("numbervars", 4, numbervars, 0)
  PRED_DEF("term_variables", 2, term_variables2, 0)
  PRED_DEF("term_variables", 3, term_variables3, 0)
  PRED_DEF("term_attvars", 2, term_attvars, 0)
  PRED_DEF("$e_free_variables", 2, e_free_variables, 0)
  PRED_DEF("unifiable", 3, unifiable, 0)
#ifdef O_TERMHASH
  PRED_DEF("term_hash", 4, term_hash4, 0)
#endif
  PRED_DEF("copy_term", 2, copy_term, PL_FA_ISO)
  PRED_DEF("duplicate_term", 2, duplicate_term, 0)
  PRED_DEF("copy_term_nat", 2, copy_term_nat, 0)
#ifdef O_LIMIT_DEPTH
  PRED_DEF("$depth_limit_except", 3, depth_limit_except, 0)
  PRED_DEF("$depth_limit_false",  3, depth_limit_false, 0)
  PRED_DEF("$depth_limit", 3, pl_depth_limit, 0)
  PRED_DEF("$depth_limit_true", 5, pl_depth_limit_true, PL_FA_NONDETERMINISTIC)
#endif
  PRED_DEF("atom_length", 2, atom_length, PL_FA_ISO)
  PRED_DEF("name", 2, name, 0)
  PRED_DEF("atom_chars", 2, atom_chars, PL_FA_ISO)
  PRED_DEF("atom_codes", 2, atom_codes, PL_FA_ISO)
  PRED_DEF("atom_concat", 3, atom_concat, PL_FA_NONDETERMINISTIC|PL_FA_ISO)
  PRED_DEF("atomic_concat", 3, atomic_concat, 0)
  PRED_DEF("number_chars", 2, number_chars, PL_FA_ISO)
  PRED_DEF("number_codes", 2, number_codes, PL_FA_ISO)
  PRED_DEF("char_code", 2, char_code, PL_FA_ISO)
  PRED_DEF("atom_number", 2, atom_number, 0)
  PRED_DEF("collation_key", 2, collation_key, 0)
  PRED_DEF("atomic_list_concat", 3, atomic_list_concat, 0)
  PRED_DEF("atomic_list_concat", 2, atomic_list_concat, 0)
  PRED_DEF("string_concat", 3, string_concat, PL_FA_NONDETERMINISTIC)
  PRED_DEF("string_length", 2, string_length, 0)
  PRED_DEF("string_to_atom", 2, string_to_atom, 0)
  PRED_DEF("string_to_list", 2, string_to_list, 0)
  PRED_DEF("statistics", 2, statistics, 0)
  PRED_DEF("$option", 3, option, PL_FA_NONDETERMINISTIC)
  PRED_DEF("$style_check", 2, style_check, 0)
  PRED_DEF("deterministic", 1, deterministic, 0)
  PRED_DEF("setarg", 3, setarg, 0)
  PRED_DEF("nb_setarg", 3, nb_setarg, 0)
  PRED_DEF("nb_linkarg", 3, nb_linkarg, 0)
  PRED_DEF("$skip_list", 3, skip_list, 0)
  PRED_DEF("throw", 1, throw, PL_FA_ISO)
#ifdef O_MEMSTATS
  PRED_DEF("memory_statistics", 1, memory_statistics, 0)
#endif
EndPredDefs