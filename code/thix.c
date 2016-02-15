/* thix.c: Threads Manager for Posix threads
 *
 *  $Id$
 *  Copyright (c) 2001-2014 Ravenbrook Limited.  See end of file for license.
 *
 * .purpose: This is a pthreads implementation of the threads manager.
 * This implements <code/th.h>.
 *
 * .design: See <design/thread-manager/>.
 *
 * .thread.id: The thread id is used to identify the current thread.
 *
 * ASSUMPTIONS
 *
 * .error.resume: PThreadextResume is assumed to succeed unless the
 * thread has been terminated.
 * .error.suspend: PThreadextSuspend is assumed to succeed unless the
 * thread has been terminated.
 *
 * .stack.full-descend:  assumes full descending stack.
 * i.e. stack pointer points to the last allocated location;
 * stack grows downwards.
 *
 * .stack.below-bottom: it's legal for the stack pointer to be at a
 * higher address than the registered bottom of stack. This might
 * happen if the stack of another thread doesn't contain any frames
 * belonging to the client language. In this case, the stack should
 * not be scanned.
 *
 * .stack.align: assume roots on the stack are always word-aligned,
 * but don't assume that the stack pointer is necessarily
 * word-aligned at the time of reading the context of another thread.
 */

#include "prmcix.h"
#include "mpm.h"

#include <pthread.h>
#include "pthrdext.h"

SRCID(thix, "$Id$");


/* ThreadStruct -- thread descriptor */

typedef struct mps_thr_s {       /* PThreads thread structure */
  Sig sig;                       /* <design/sig/> */
  Serial serial;                 /* from arena->threadSerial */
  Arena arena;                   /* owning arena */
  RingStruct arenaRing;          /* threads attached to arena */
  Bool alive;                    /* thread believed to be alive? */
  PThreadextStruct thrextStruct; /* PThreads extension */
  pthread_t id;                  /* Pthread object of thread */
  MutatorFaultContext mfc;       /* Context if suspended, NULL if not */
} ThreadStruct;


/* ThreadCheck -- check a thread */

Bool ThreadCheck(Thread thread)
{
  CHECKS(Thread, thread);
  CHECKU(Arena, thread->arena);
  CHECKL(thread->serial < thread->arena->threadSerial);
  CHECKD_NOSIG(Ring, &thread->arenaRing);
  CHECKL(BoolCheck(thread->alive));
  CHECKD(PThreadext, &thread->thrextStruct);
  return TRUE;
}

Bool ThreadCheckSimple(Thread thread)
{
  CHECKS(Thread, thread);
  return TRUE;
}


/* ThreadRegister -- register a thread with an arena */

Res ThreadRegister(Thread *threadReturn, Arena arena)
{
  Res res;
  Thread thread;
  void *p;

  AVER(threadReturn != NULL);
  AVERT(Arena, arena);

  res = ControlAlloc(&p, arena, sizeof(ThreadStruct),
                     /* withReservoirPermit */ FALSE);
  if(res != ResOK)
    return res;
  thread = (Thread)p;

  thread->id = pthread_self();

  RingInit(&thread->arenaRing);

  thread->sig = ThreadSig;
  thread->serial = arena->threadSerial;
  ++arena->threadSerial;
  thread->arena = arena;
  thread->alive = TRUE;
  thread->mfc = NULL;

  PThreadextInit(&thread->thrextStruct, thread->id);

  AVERT(Thread, thread);

  RingAppend(ArenaThreadRing(arena), &thread->arenaRing);

  *threadReturn = thread;
  return ResOK;
}


/* ThreadDeregister -- deregister a thread from an arena */

void ThreadDeregister(Thread thread, Arena arena)
{
  AVERT(Thread, thread);
  AVERT(Arena, arena);

  RingRemove(&thread->arenaRing);

  thread->sig = SigInvalid;

  RingFinish(&thread->arenaRing);

  PThreadextFinish(&thread->thrextStruct);

  ControlFree(arena, thread, sizeof(ThreadStruct));
}


/* mapThreadRing -- map over threads on ring calling a function on
 * each one except the current thread.
 *
 * Threads that are found to be dead (that is, if func returns FALSE)
 * are moved to deadRing, in order to implement
 * design.thread-manager.sol.thread.term.attempt.
 */

static void mapThreadRing(Ring threadRing, Ring deadRing, Bool (*func)(Thread))
{
  Ring node, next;
  pthread_t self;

  AVERT(Ring, threadRing);
  AVERT(Ring, deadRing);
  AVER(FUNCHECK(func));

  self = pthread_self();
  RING_FOR(node, threadRing, next) {
    Thread thread = RING_ELT(Thread, arenaRing, node);
    AVERT(Thread, thread);
    AVER(thread->alive);
    if (!pthread_equal(self, thread->id) /* .thread.id */
        && !(*func)(thread))
    {
      thread->alive = FALSE;
      RingRemove(&thread->arenaRing);
      RingAppend(deadRing, &thread->arenaRing);
    }
  }
}


/* ThreadRingSuspend -- suspend all threads on a ring, except the
 * current one.
 */

static Bool threadSuspend(Thread thread)
{
  /* .error.suspend: if PThreadextSuspend fails, we assume the thread
   * has been terminated. */
  Res res;
  AVER(thread->mfc == NULL);
  res = PThreadextSuspend(&thread->thrextStruct, &thread->mfc);
  AVER(res == ResOK);
  AVER(thread->mfc != NULL);
  /* design.thread-manager.sol.thread.term.attempt */
  return res == ResOK;
}



void ThreadRingSuspend(Ring threadRing, Ring deadRing)
{
  mapThreadRing(threadRing, deadRing, threadSuspend);
}


/* ThreadRingResume -- resume all threads on a ring (expect the current one) */


static Bool threadResume(Thread thread)
{
  Res res;
  /* .error.resume: If PThreadextResume fails, we assume the thread
   * has been terminated. */
  AVER(thread->mfc != NULL);
  res = PThreadextResume(&thread->thrextStruct);
  AVER(res == ResOK);
  thread->mfc = NULL;
  /* design.thread-manager.sol.thread.term.attempt */
  return res == ResOK;
}

void ThreadRingResume(Ring threadRing, Ring deadRing)
{
  mapThreadRing(threadRing, deadRing, threadResume);
}


/* ThreadRingThread -- return the thread at the given ring element */

Thread ThreadRingThread(Ring threadRing)
{
  Thread thread;
  AVERT(Ring, threadRing);
  thread = RING_ELT(Thread, arenaRing, threadRing);
  AVERT(Thread, thread);
  return thread;
}


/* ThreadArena -- get the arena of a thread
 *
 * Must be thread-safe. See <design/interface-c/#check.testt>.
 */

Arena ThreadArena(Thread thread)
{
  AVER(TESTT(Thread, thread));
  return thread->arena;
}


/* ThreadScan -- scan the state of a thread (stack and regs) */

Res ThreadScan(ScanState ss, Thread thread, Word *stackBot,
	       mps_area_scan_t scan_area,
	       void *closure, size_t closure_size)
{
  pthread_t self;
  Res res;

  AVERT(Thread, thread);
  self = pthread_self();
  if(pthread_equal(self, thread->id)) {
    /* scan this thread's stack */
    AVER(thread->alive);
    res = StackScan(ss, stackBot, scan_area, closure, closure_size);
    if(res != ResOK)
      return res;
  } else if (thread->alive) {
    MutatorFaultContext mfc;
    Word *stackBase, *stackLimit;
    Addr stackPtr;

    mfc = thread->mfc;
    AVER(mfc != NULL);

    stackPtr = MutatorFaultContextSP(mfc);
    /* .stack.align */
    stackBase  = (Word *)AddrAlignUp(stackPtr, sizeof(Addr));
    stackLimit = stackBot;
    if (stackBase >= stackLimit)
      return ResOK;    /* .stack.below-bottom */

    /* scan stack inclusive of current sp and exclusive of
     * stackBot (.stack.full-descend)
     */
    res = TraceScanArea(ss, stackBase, stackLimit,
			scan_area, closure, closure_size);
    if(res != ResOK)
      return res;

    /* scan the registers in the mutator fault context */
    res = MutatorFaultContextScan(ss, mfc, scan_area, closure, closure_size);
    if(res != ResOK)
      return res;
  }

  return ResOK;
}


/* ThreadDescribe -- describe a thread */

Res ThreadDescribe(Thread thread, mps_lib_FILE *stream, Count depth)
{
  Res res;

  res = WriteF(stream, depth,
               "Thread $P ($U) {\n", (WriteFP)thread, (WriteFU)thread->serial,
               "  arena $P ($U)\n",
               (WriteFP)thread->arena, (WriteFU)thread->arena->serial,
               "  alive $S\n", WriteFYesNo(thread->alive),
               "  id $U\n",          (WriteFU)thread->id,
               "} Thread $P ($U)\n", (WriteFP)thread, (WriteFU)thread->serial,
               NULL);
  if(res != ResOK)
    return res;

  return ResOK;
}


/* C. COPYRIGHT AND LICENSE
 *
 * Copyright (C) 2001-2014 Ravenbrook Limited <http://www.ravenbrook.com/>.
 * All rights reserved.  This is an open source license.  Contact
 * Ravenbrook for commercial licensing options.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 * 1. Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * 
 * 2. Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in the
 * documentation and/or other materials provided with the distribution.
 * 
 * 3. Redistributions in any form must be accompanied by information on how
 * to obtain complete source code for this software and any accompanying
 * software that uses this software.  The source code must either be
 * included in the distribution or be available for no more than the cost
 * of distribution plus a nominal fee, and must be freely redistributable
 * under reasonable conditions.  For an executable file, complete source
 * code means the source code for all modules it contains. It does not
 * include source code for modules or files that typically accompany the
 * major components of the operating system on which the executable file
 * runs.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
 * IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR
 * PURPOSE, OR NON-INFRINGEMENT, ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS AND CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF
 * USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
