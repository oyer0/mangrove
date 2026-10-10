#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Production mutex and scheduler wake decision; pthread-backed park fixture.
This covers SMP waiter bookkeeping, not guest scheduler/IRQ implementation.
Optional repository argument runs the deterministic old lost-wakeup assertion.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
ROOT = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]
source = (ROOT / 'kernel/src/mutex.c').read_text()
source = re.sub(r'^#include .*\n', '', source, flags=re.M)
source = re.sub(r'static u64 mutex_irq_save\(void\).*?\n}', 'static u64 mutex_irq_save(void) {return 0;}', source, count=1, flags=re.S)
source = re.sub(r'static void mutex_irq_restore\(u64 flags\).*?\n}', 'static void mutex_irq_restore(u64 flags) {(void)flags;}', source, count=1, flags=re.S)
sched = (ROOT / 'kernel/src/scheduler.c').read_text()
m = re.search(r'^bool scheduler_mutex_wake\(.*?\n}', sched, re.M | re.S)
wake = m.group(0) if m else ''
pre = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
typedef uint64_t u64;
typedef pthread_mutex_t spinlock_t;
typedef struct kernel_mutex mutex_t;
typedef struct kernel_thread {
 unsigned state; bool mutex_wake_pending;
 mutex_t *waiting_mutex; struct kernel_thread *mutex_wait_next;
 pthread_cond_t condition;
} kernel_thread_t;
struct kernel_mutex {spinlock_t state_lock;kernel_thread_t *owner,*wait_head,*wait_tail;};
enum {THREAD_STATE_RUNNING,THREAD_STATE_BLOCKED,THREAD_STATE_READY,THREAD_STATE_TERMINATED};
static spinlock_t scheduler_lock=PTHREAD_MUTEX_INITIALIZER;
static _Thread_local kernel_thread_t *self;
static _Thread_local bool scheduler_held;
static bool inject_race;
static kernel_thread_t *thread_current(void){return self;}
static void spinlock_init(spinlock_t*p){assert(!pthread_mutex_init(p,NULL));}
static void spin_lock(spinlock_t*p){assert(!pthread_mutex_lock(p));}
static void spin_unlock(spinlock_t*p){assert(!pthread_mutex_unlock(p));}
static u64 spin_lock_irqsave(spinlock_t*p){spin_lock(p);scheduler_held=true;return 0;}
static void spin_unlock_irqrestore(spinlock_t*p,u64 f){(void)f;scheduler_held=false;spin_unlock(p);}
static bool scheduler_unblock_locked(kernel_thread_t*t){assert(scheduler_held);if(t->state!=THREAD_STATE_BLOCKED)return false;t->state=THREAD_STATE_READY;pthread_cond_signal(&t->condition);return true;}
static bool scheduler_unblock(kernel_thread_t*t) __attribute__((unused));
static bool scheduler_unblock(kernel_thread_t*t){u64 f=spin_lock_irqsave(&scheduler_lock);bool r=scheduler_unblock_locked(t);spin_unlock_irqrestore(&scheduler_lock,f);return r;}
static void pending_store(bool*p,bool v,int order){(void)order;
 /* Old handoff has already read RUNNING without the scheduler lock. CPU 1
    can finish parking immediately before CPU 0 publishes pending=true. */
 if(inject_race && v && !scheduler_held)self->state=THREAD_STATE_BLOCKED;
 __atomic_store_n(p,v,__ATOMIC_RELEASE);
}
#define __atomic_store_n(p,v,o) pending_store(p,v,o)
static bool scheduler_block(void){
 u64 f=spin_lock_irqsave(&scheduler_lock);
 if(__atomic_exchange_n(&self->mutex_wake_pending,false,__ATOMIC_ACQ_REL)){
  spin_unlock_irqrestore(&scheduler_lock,f);return true;
 }
 self->state=THREAD_STATE_BLOCKED;
 while(self->state==THREAD_STATE_BLOCKED)pthread_cond_wait(&self->condition,&scheduler_lock);
 self->state=THREAD_STATE_RUNNING;spin_unlock_irqrestore(&scheduler_lock,f);return true;
}
'''
post = r'''
static mutex_t shared;
static unsigned counter;
static void *worker(void *arg){kernel_thread_t *t=arg;self=t;
 for(unsigned n=0;n<10000;n++){assert(mutex_lock(&shared));counter++;assert(mutex_unlock(&shared));}
 return NULL;
}
int main(void){
 kernel_thread_t waiter={.state=THREAD_STATE_RUNNING,.condition=PTHREAD_COND_INITIALIZER};
 mutex_t m;mutex_init(&m);m.wait_head=m.wait_tail=&waiter;waiter.waiting_mutex=&m;
 self=&waiter;inject_race=true;
 assert(mutex_handoff_locked(&m));
 assert(waiter.state!=THREAD_STATE_BLOCKED && waiter.mutex_wake_pending);
 assert(scheduler_block());assert(waiter.state!=THREAD_STATE_BLOCKED);
 inject_race=false;assert(mutex_lock(&m));assert(!waiter.waiting_mutex);
 assert(!mutex_lock(&m));assert(mutex_unlock(&m));
 /* Blocked, preempted READY, terminated, cancellation and FIFO handoff. */
 kernel_thread_t a={.state=THREAD_STATE_BLOCKED,.condition=PTHREAD_COND_INITIALIZER};
 kernel_thread_t b={.state=THREAD_STATE_READY,.condition=PTHREAD_COND_INITIALIZER};
 kernel_thread_t dead={.state=THREAD_STATE_TERMINATED};
 m.owner=&waiter;m.wait_head=&dead;m.wait_tail=&b;
 dead.waiting_mutex=a.waiting_mutex=b.waiting_mutex=&m;
 dead.mutex_wait_next=&a;a.mutex_wait_next=&b;
 assert(mutex_unlock(&m));assert(m.owner==&a && a.state==THREAD_STATE_READY);
 assert(!dead.waiting_mutex && m.wait_head==&b);
 mutex_cancel_waiter(&a);assert(m.owner==&b && b.mutex_wake_pending);
 mutex_cancel_waiter(&b);assert(!m.owner && !m.wait_head && !m.wait_tail);
 mutex_init(&shared);kernel_thread_t ts[4]={0};pthread_t ps[4];
 for(unsigned i=0;i<4;i++){ts[i].state=THREAD_STATE_RUNNING;pthread_cond_init(&ts[i].condition,NULL);assert(!pthread_create(&ps[i],NULL,worker,&ts[i]));}
 for(unsigned i=0;i<4;i++)assert(!pthread_join(ps[i],NULL));
 assert(counter==40000 && !shared.owner && !shared.wait_head && !shared.wait_tail);
 puts("mutex: production wake/handoff/cancellation and 40,000 contended handoffs passed");
}
'''
with tempfile.TemporaryDirectory(prefix='mangrove-mutex-') as d:
    c = Path(d) / 'test.c'
    c.write_text(pre + wake + '\n' + source + post)
    exe = Path(d) / 'test'
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined', str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)
