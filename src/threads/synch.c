/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void
sema_init (struct semaphore *sema, unsigned value) 
{
  ASSERT (sema != NULL);

  sema->value = value;
  list_init (&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
void
sema_down (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  while (sema->value == 0) 
  {
      list_push_back (&sema->waiters, &thread_current ()->elem);
      thread_block ();
  }
  sema->value--;
  intr_set_level (old_level);
}

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool
sema_try_down (struct semaphore *sema) 
{
  enum intr_level old_level;
  bool success;

  ASSERT (sema != NULL);

  old_level = intr_disable ();
  if (sema->value > 0) 
    {
      sema->value--;
      success = true; 
    }
  else
    success = false;
  intr_set_level (old_level);

  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
void
sema_up (struct semaphore *sema) 
{
  enum intr_level old_level;

  ASSERT (sema != NULL);

  old_level = intr_disable ();

  struct thread *t = NULL ;
  if (!list_empty (&sema->waiters))
  {
	  // Get the thread with maximum priority waiting for the semaphore and unblock it
	  struct list_elem *e = list_max ( &sema->waiters, min_priority, NULL ) ;
	  list_remove(e) ;

	  t = list_entry(e, struct thread, elem ) ;
	  thread_unblock (t) ;
  }
  sema->value++;

  // If the thread unblocked has a higher priority, then schedule it
  // It is IMPORTANT to check if the thread is not running inside interrupt before yielding. This is given in the comment for this function
  if ( !intr_context() )
	  if ( t != NULL && thread_current()->priority < t->priority )
		  thread_yield() ;

  intr_set_level (old_level);
}

static void sema_test_helper (void *sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void
sema_self_test (void) 
{
  struct semaphore sema[2];
  int i;

  printf ("Testing semaphores...");
  sema_init (&sema[0], 0);
  sema_init (&sema[1], 0);
  thread_create ("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) 
    {
      sema_up (&sema[0]);
      sema_down (&sema[1]);
    }
  printf ("done.\n");
}

/* Thread function used by sema_self_test(). */
static void
sema_test_helper (void *sema_) 
{
  struct semaphore *sema = sema_;
  int i;

  for (i = 0; i < 10; i++) 
    {
      sema_down (&sema[0]);
      sema_up (&sema[1]);
    }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void
lock_init (struct lock *lock)
{
  ASSERT (lock != NULL);

  lock->holder = NULL;
  sema_init (&lock->semaphore, 1);
}

// Function to implement nested donations as an recursive function
// Each thread will update the priority of the thread which currently has the lock, with new priority
// INTERUPPTS are disabled while running this function
static void nested_donate ( struct thread *t )
{
	// Base Case: If the thread is not waiting on any lock, then return
	if ( t->lock == NULL )
		return ;

	// Transfer the new priority to the thread on which 't' is waiting and recurse on that thread
	struct thread *donee = t->lock->holder ;
	donee->priority = t->priority ;
	nested_donate ( donee ) ;

	return ;
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
lock_acquire (struct lock *lock)
{
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (!lock_held_by_current_thread (lock));

  enum intr_level old_level = intr_disable() ;

  // Check if the lock has been acquired. If so, then you have to donate priority
  if ( lock->holder != NULL )
  {
	  // Donate the priority
	  struct thread *cur = thread_current() ;
	  struct thread *donee = lock->holder ;

	  // Add the list_elem of the current thread to the list of donors in the donee thread
	  cur->lock = lock ;
	  list_push_back ( &donee->donors, &cur->pri_elem ) ;
	  donee->priority = cur->priority ;

	  // Recursive function to handle nested donation.
	  // i.e this function is used to transfer the new priority to the thread on which this thread is waiting
	  nested_donate ( donee ) ;
  }

  sema_down (&lock->semaphore);
  lock->holder = thread_current ();

  intr_set_level(old_level) ;
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool
lock_try_acquire (struct lock *lock)
{
  bool success;

  ASSERT (lock != NULL);
  ASSERT (!lock_held_by_current_thread (lock));

  success = sema_try_down (&lock->semaphore);
  if (success)
    lock->holder = thread_current ();
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void
lock_release (struct lock *lock) 
{
  ASSERT (lock != NULL);
  ASSERT (lock_held_by_current_thread (lock));

  lock->holder = NULL;

  enum intr_level old_level = intr_disable() ;

  struct thread *cur = thread_current() ;

  // Release the donation acquired for this lock
  // For all the threads which are currently waiting on LOCK, remove their entry in DONORS list.
  if ( !list_empty(&cur->donors) )
  {
	  struct list_elem *e, *next ;
	  for ( e =  list_begin(&cur->donors) ; e != list_end(&cur->donors) ; e = next )
	  {
		  next = list_next(e) ;

		  // Check if this list entry is waiting for the lock which was just released
		  struct thread *t = list_entry( e, struct thread, pri_elem ) ;
		  if ( t->lock == lock )
		  {
			  list_remove(e) ;
			  t->lock = NULL ;
		  }
	  }
  }

  // Check if the current thread still has any donors
  // If TRUE, then reset its priority
  // Else, set the priority to the highest in the donors list
  if ( list_empty(&cur->donors) )
  {
	  cur->priority = cur->real_priority ;
  }
  else
  {
	  struct list_elem *e = list_max ( &cur->donors, min_priority, NULL ) ;
	  int max = (list_entry( e, struct thread, pri_elem))->priority ;

	  // If the current priority is higher than the MAX of the donors, retain current_priority
	  if ( max > cur->real_priority )
		  cur->priority = max ;
	  else
		  cur->priority = cur->real_priority ;
  }

  sema_up (&lock->semaphore);

  intr_set_level(old_level) ;
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool
lock_held_by_current_thread (const struct lock *lock) 
{
  ASSERT (lock != NULL);

  return lock->holder == thread_current ();
}

/* One semaphore in a list. */
struct semaphore_elem 
{
    struct list_elem elem;              /* List element. */
    struct semaphore semaphore;         /* This semaphore. */
	struct thread *t ;					/* The thread to which this belongs to */
};

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void
cond_init (struct condition *cond)
{
  ASSERT (cond != NULL);

  list_init (&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void
cond_wait (struct condition *cond, struct lock *lock) 
{
  struct semaphore_elem waiter;

  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));
  
  waiter.t = thread_current() ;
  sema_init (&waiter.semaphore, 0);
  list_push_back (&cond->waiters, &waiter.elem);
  lock_release (lock);
  sema_down (&waiter.semaphore);
  lock_acquire (lock);
}

// Own functon to find the thread with maximum priority waiting for the semaphore
static struct list_elem * cond_list_max ( struct list *list )
{
  struct list_elem *max = list_begin (list);
  struct thread *tmax, *t ;
  if (max != list_end (list))
  {
	  tmax = (list_entry(max, struct semaphore_elem, elem))->t ;
      struct list_elem *e ;
      for (e = list_next (max); e != list_end (list); e = list_next (e))
      {
		  t = (list_entry(e, struct semaphore_elem, elem))->t ;
		  if ( tmax->priority < t->priority )
		  {
			  tmax = t ;
			  max = e ;
		  }
      }
  }
  return max;
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_signal (struct condition *cond, struct lock *lock UNUSED) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);
  ASSERT (!intr_context ());
  ASSERT (lock_held_by_current_thread (lock));

  // Wake up i.e call sema_up on the thread with highest priority among the threads in the cond->waiters list
  if (!list_empty (&cond->waiters))
  {
	  struct list_elem *e = cond_list_max ( &cond->waiters ) ;
	  list_remove(e) ;
	  sema_up (&list_entry (e, struct semaphore_elem, elem)->semaphore);
  }
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void
cond_broadcast (struct condition *cond, struct lock *lock) 
{
  ASSERT (cond != NULL);
  ASSERT (lock != NULL);

  while (!list_empty (&cond->waiters))
    cond_signal (cond, lock);
}
