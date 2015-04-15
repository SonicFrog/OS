/*
 * Dummy scheduling class, mapped to range of 5 levels of SCHED_NORMAL policy
 */

#include "sched.h"

/*
 * Timeslice and age threshold are repsented in jiffies. Default timeslice
 * is 100ms. Both parameters can be tuned from /proc/sys/kernel.
 */

#define DUMMY_TIMESLICE         (100 * HZ / 1000)
#define DUMMY_AGE_THRESHOLD     (3 * DUMMY_TIMESLICE)

#define DUMMY_PRIO_BASE         130
#define DUMMY_PRIO_HIGH         135


#define __DEBUG


#ifdef __DEBUG
#define dprintk(fmt, ...) printk(KERN_DEBUG "%s:%d " fmt, \
                                 __FILE__, __LINE__, ##__VA_ARGS__)
#else
#define dprintk(fmt, ...) do { } while(0)
#endif

unsigned int sysctl_sched_dummy_timeslice = DUMMY_TIMESLICE;
static inline unsigned int get_timeslice(void)
{
    return sysctl_sched_dummy_timeslice;
}

unsigned int sysctl_sched_dummy_age_threshold = DUMMY_AGE_THRESHOLD;
static inline unsigned int get_age_threshold(void)
{
    return sysctl_sched_dummy_age_threshold;
}

void init_dummy_rq(struct dummy_rq *dummy_rq, struct rq *qr)
{
    int i;

    for (i = 0; i < DUMMY_PRIO_COUNT; i++) {
        INIT_LIST_HEAD(&dummy_rq->queues[i]);
    }
}

static inline struct task_struct *dummy_task_of(struct sched_dummy_entity *dummy_se)
{
    return container_of(dummy_se, struct task_struct, dummy_se);
}

/**
 * Returns the prio for the task in the range 1-5
 **/
static inline int dummy_task_prio(struct sched_dummy_entity* se)
{
    struct task_struct* task = dummy_task_of(se);

    return task->prio - DUMMY_PRIO_BASE;
}

static inline int dummy_needs_aging (struct sched_dummy_entity *dummy_se)
{
    return dummy_se->jiffies_since_last > NS_TO_JIFFIES(get_age_threshold());
}

static inline struct list_head* dummy_queue_for_prio (struct rq* rq, int prio)
{
    struct list_head* result;

    result = rq->dummy.queues + prio - DUMMY_PRIO_BASE - 1;

    return result;
}

static inline struct list_head* dummy_queue_from_prio (struct rq* rq,
                                                       struct sched_dummy_entity* se)
{
    struct list_head *result;

    result = rq->dummy.queues + dummy_task_prio(se) - 1;

    return result;
}

static inline void _queue_dummy_task(struct rq* rq, struct task_struct* task)
{
    struct list_head* target_head;

    target_head = dummy_queue_from_prio(rq, &task->dummy_se);

    list_move_tail(&task->dummy_se.run_list, target_head);
}


/**
 * Returns a pointer to the sched_dummy_entity with the highest priority
 **/
static inline struct sched_dummy_entity *dummy_highest_prio(struct rq* rq)
{
    int i;
    struct list_head *current_head;
    struct list_head *entry;
    struct sched_dummy_entity *se;

    for (i = 0; i < DUMMY_PRIO_COUNT; i++) {
        current_head = dummy_queue_for_prio(rq, dummy_task_of(se)->prio);

        if (!list_empty(current_head)) {
            list_for_each (entry, current_head) {
                se = list_entry (entry, struct sched_dummy_entity, run_list);
                return se;
            }
        }
    }

    return NULL;
}

/**
 * Increases the dummy priority of an entity
 **/
static inline void inc_dummy_prio(struct rq* rq, struct sched_dummy_entity* se)
{
    struct list_head *target_head = dummy_queue_from_prio(rq, se)
        - sizeof(struct list_head);

    list_move_tail(&se->run_list, target_head);
}

/**
 * Resets the dummy prio of the given entity
 **/
static inline void reset_dummy_prio(struct rq* rq, struct sched_dummy_entity* se)
{
    struct list_head *target_head = dummy_queue_from_prio(rq, se);

    list_move_tail(&se->run_list, target_head);
}

static inline void start_dummy_run(struct sched_dummy_entity* se) {
    se->jiffies_since_last = se->jiffies_count = 0;
}

/**
 * Traverses the rb tree and increases priority for task that have not run for
 * more than age_threshold.
 * @param rq The runqueue that needs to be checked for aging processes
 **/
static void dummy_age_tree(struct rq* rq)
{
    int i;
    struct list_head *current_head, *prev_head = NULL;
    struct list_head *pos;
    struct sched_dummy_entity *entry;

    for (i = 0; i < DUMMY_PRIO_HIGH; i++) {
        current_head = dummy_queue_for_prio(rq, i + DUMMY_PRIO_BASE);

        if (!list_empty(current_head)) {
            list_for_each (pos, current_head) {
                entry = list_entry(pos, struct sched_dummy_entity, run_list);

                if (dummy_needs_aging(entry)) {
                    entry->jiffies_since_last = 0;
                    if (prev_head) {
                        list_move_tail(&entry->run_list, prev_head);
                    }
                }
            }
        }
        prev_head = current_head;
    }
}

static inline void _enqueue_task_dummy(struct rq *rq, struct task_struct *p)
{
    struct sched_dummy_entity *se = &p->dummy_se;
    struct list_head *head = dummy_queue_from_prio(rq, se);

    list_add_tail(&se->run_list, head);
}

static inline void _dequeue_task_dummy(struct rq* rq, struct task_struct *p)
{
    struct sched_dummy_entity *se = &p->dummy_se;
    list_del_init(&se->run_list);
}

/*
 * Scheduling class functions to implement
 */

static void enqueue_task_dummy(struct rq *rq, struct task_struct *p, int flags)
{
    _enqueue_task_dummy(rq, p);
    add_nr_running(rq,1);
}

static void dequeue_task_dummy(struct rq *rq, struct task_struct *p, int flags)
{
    _dequeue_task_dummy(rq, p);
    sub_nr_running(rq,1);
}

/**
 * Yields the CPU from the currently running task
 * @param rq the runqueue for the current CPU
 **/
static void yield_task_dummy(struct rq *rq)
{
    struct sched_dummy_entity *se = &rq->curr->dummy_se;
    struct list_head* head = dummy_queue_from_prio(rq, se);
    struct list_head* entry = &se->run_list;

    list_del_init(entry);
    list_add_tail(entry, head);
}

/**
 * Checks if the running task has a low enough priority to be preempted
 **/
static void check_preempt_curr_dummy(struct rq *rq, struct task_struct *p, int flags)
{
    struct task_struct *curr = rq->curr;
    struct sched_dummy_entity *se = &curr->dummy_se, *pse = &p->dummy_se;

    dprintk("Preempt check for %p on %p\n", p, rq);

    if (unlikely(se == pse)) {
        return;
    }

    // The current task has already been marked for reschedule
    if (test_tsk_need_resched(curr)) {
        return;
    }
}

/**
 * Puts the previously running task back into the runqueue
 **/
static void put_prev_task_dummy(struct rq *rq, struct task_struct *prev)
{
    dprintk("Putting prev task %p on %p\n", prev, rq);

    if (unlikely(!prev)) {
        return;
    }

    reset_dummy_prio(rq, &prev->dummy_se);
}


/**
 * Computes which task is scheduled to run next
 * @param rq the runqueue for the current CPU
 * @param prev the task currently running
 * @returns a pointer to the task which will run next
 **/
static struct task_struct *pick_next_task_dummy(struct rq *rq, struct task_struct* prev)
{
    struct task_struct* next;

    dprintk("Picking next task while %p runs\n", prev);

    next = dummy_task_of(dummy_highest_prio(rq));

    dprintk("Next to run is %p\n", next);

    if (unlikely(!next)) {
        return prev;
    }

    if (next != prev) {
        put_prev_task_dummy(rq, prev);
    }

    return next;
}

static void set_curr_task_dummy(struct rq *rq)
{
    /* TODO: what is this supposed to do ? */
    dprintk("Set curr_task\n");
}

/**
 * Function executed by the periodic scheduler at each tick
 * @param rq the runqueue for the current CPU
 * @param curr A pointer to the running task
 * @param queued ???
 **/
static void task_tick_dummy(struct rq *rq, struct task_struct *curr, int queued)
{
    struct sched_dummy_entity *se = &curr->dummy_se;

    dummy_age_tree(rq);

    se->jiffies_count++;

    if (se->jiffies_count > NS_TO_JIFFIES(get_timeslice())) {
        resched_curr(rq);
    }
}

static void switched_from_dummy(struct rq *rq, struct task_struct *p)
{
}

static void switched_to_dummy(struct rq *rq, struct task_struct *p)
{
    struct sched_dummy_entity *se = &p->dummy_se;

    start_dummy_run(se);
}

static void prio_changed_dummy(struct rq *rq, struct task_struct *p, int oldprio)
{
    struct sched_dummy_entity *se = &p->dummy_se;

    if (likely(oldprio != p->prio)) {
        reset_dummy_prio(rq, se);
    }
}

static unsigned int get_rr_interval_dummy(struct rq* rq, struct task_struct *p)
{
    return get_timeslice();
}

#ifdef CONFIG_SMP
/*
 * SMP related functions
 */

static inline int select_task_rq_dummy(struct task_struct *p, int cpu, int sd_flags, int wake_flags)
{
    int new_cpu = smp_processor_id();

    return new_cpu; //set assigned CPU to zero
}


static void set_cpus_allowed_dummy(struct task_struct *p,  const struct cpumask *new_mask)
{
}
#endif

/*
 * Scheduling class
 */
static void update_curr_dummy(struct rq *rq)
{
    struct task_struct* p = rq->curr;
    struct sched_dummy_entity *se;

    if (unlikely(!p)) {
        return;
    }

    se = &p->dummy_se;

    reset_dummy_prio(rq, se);
}

const struct sched_class dummy_sched_class = {
    .next                   = &idle_sched_class,
    .enqueue_task           = enqueue_task_dummy,
    .dequeue_task           = dequeue_task_dummy,
    .yield_task             = yield_task_dummy,

    .check_preempt_curr     = check_preempt_curr_dummy,

    .pick_next_task         = pick_next_task_dummy,
    .put_prev_task          = put_prev_task_dummy,

#ifdef CONFIG_SMP
    .select_task_rq         = select_task_rq_dummy,
    .set_cpus_allowed       = set_cpus_allowed_dummy,
#endif

    .set_curr_task          = set_curr_task_dummy,
    .task_tick              = task_tick_dummy,

    .switched_from          = switched_from_dummy,
    .switched_to            = switched_to_dummy,
    .prio_changed           = prio_changed_dummy,

    .get_rr_interval        = get_rr_interval_dummy,
    .update_curr            = update_curr_dummy,
};
