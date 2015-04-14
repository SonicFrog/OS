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
    dummy_rq->root = RB_ROOT;
}

static inline struct task_struct *dummy_task_of(struct sched_dummy_entity *dummy_se)
{
    return container_of(dummy_se, struct task_struct, dummy_se);
}

/**
 * Gets the aged dummy prio for the given sched_entity
 * @param se a pointer to a sched_dummy_entity
 * @returns The total aged priority for the given entity
 **/
static inline int get_dummy_prio(struct sched_dummy_entity* se)
{
    return max(se->prio_base + se->prio_dyn, (u64) DUMMY_PRIO_BASE);
}

/**
 * Increases the dummy priority of an entity
 **/
static inline void inc_dummy_prio(struct sched_dummy_entity* se)
{
    se->prio_dyn--;
}

/**
 * Resets the priority of a dummy to its base value
 **/
static inline void reset_dummy_prio(struct sched_dummy_entity* se)
{
    se->prio_dyn = 0;
}

/**
 * Ages the dummy priority of the given entity
 **/
static inline void age_dummy_prio(struct sched_dummy_entity* se)
{
    if (get_dummy_prio(se) < DUMMY_PRIO_HIGH) {
        inc_dummy_prio(se);
    }
}

/**
 * Inserts a new node in the rbtree while rebalancing
 * @param root The root of the tree we are adding to
 * @param se The scheduling entity we are adding to the tree
 **/
static void weighted_rbtree_insert(struct rb_root *root, struct sched_dummy_entity* se)
{
    struct rb_node **new = &(root->rb_node), *parent = NULL;

    /* Figure out where to put new node */
    while (*new) {
        struct sched_dummy_entity *this =
            container_of(*new, struct sched_dummy_entity, node);
        struct sched_dummy_entity *insert = se;
        int prio_diff = get_dummy_prio(insert) - get_dummy_prio(this);

        parent = *new;

        if (prio_diff < 0) {
            new = &((*new)->rb_left);
        }
        else if (prio_diff > 0) {
            new = &((*new)->rb_right);
        }
        else { //Same priority => order by last jiffies
            if (insert->last_jiffies > this->last_jiffies) {
                new = &((*new)->rb_right);
            } else {
                new = &((*new)->rb_left);
            }
        }
    }

    /* Add new node and rebalance tree. */
    rb_link_node(&se->node, parent, new);
    rb_insert_color(&se->node, root);
}

/**
 * Traverses the rb tree and ages priority for the tasks that have not run in a
 * while.
 **/
static void dummy_age_tree(struct rq* rq, struct rb_root *root)
{
    struct sched_dummy_entity *pos, *n;
    u64 now = rq_clock(rq);

    rbtree_postorder_for_each_entry_safe(pos, n, root, node) {
        if (now - pos->last_jiffies > NS_TO_JIFFIES(get_age_threshold())) {
            age_dummy_prio(pos);
            //Remove and re-insert the current node to rebalance tree
            rb_erase(&pos->node, &rq->dummy.root);
            weighted_rbtree_insert(&rq->dummy.root, pos);
        }
    }
}

/**
 * Gets the leftmost node of rbtree containing sched_dummy_entities.
 * The leftmost node is the node with the highest priority that is ready to run!
 **/
static struct sched_dummy_entity* rb_get_leftmost(struct rb_root* root)
{
    struct rb_node *node = root->rb_node, *prev;
    struct sched_dummy_entity *left = NULL;

    while(node) {
        prev = node;
        node = node->rb_left;
    }

    left = container_of(prev, struct sched_dummy_entity, node);

    return left;
}

static inline void _enqueue_task_dummy(struct rq *rq, struct task_struct *p)
{
    struct sched_dummy_entity *dummy_se = &p->dummy_se;

    weighted_rbtree_insert(&rq->dummy.root, dummy_se);
}

static inline void _dequeue_task_dummy(struct rq* rq, struct task_struct *p)
{
    struct sched_dummy_entity *dummy_se = &p->dummy_se;

    rb_erase(&dummy_se->node, &rq->dummy.root);
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
    reset_dummy_prio(&rq->curr->dummy_se);
    resched_curr(rq);
}

/**
 * Checks if the running task has a low enough priority to be preempted
 **/
static void check_preempt_curr_dummy(struct rq *rq, struct task_struct *p, int flags)
{
    struct task_struct *curr = rq->curr;
    struct sched_dummy_entity *se = &curr->dummy_se, *pse = &p->dummy_se;

    if (unlikely(se == pse)) {
        //We need not do anything since the running has highest prio
        return;
    }

    // The current task has already been marked for reschedule
    if (test_tsk_need_resched(curr)) {
        return;
    }

    if (get_dummy_prio(se) < get_dummy_prio(pse)) {
        resched_curr(rq);
    }
}

/**
 * Puts the previously running task back into the runqueue
 **/
static void put_prev_task_dummy(struct rq *rq, struct task_struct *prev)
{
    struct task_struct *p = rq->curr;
    struct sched_dummy_entity* se;
    struct rb_root* root = &rq->dummy.root;

    if (unlikely(!p)) {
        return;
    }

    se = &p->dummy_se;

    reset_dummy_prio(se);
    rb_erase(&se->node, root);
    weighted_rbtree_insert(root, se);
}


/**
 * Computes which task is scheduled to run next
 * @param rq the runqueue for the current CPU
 * @param prev the task currently running
 * @returns a pointer to the task which will run next
 **/
static struct task_struct *pick_next_task_dummy(struct rq *rq, struct task_struct* prev)
{
    struct sched_dummy_entity* se = rb_get_leftmost(&rq->dummy.root);
    struct task_struct* next;

    if (unlikely(!se)) {
        return NULL;
    }

    se->last_jiffies = get_jiffies_64();

    next = dummy_task_of(se);

    if (next != prev) {
        put_prev_task_dummy(rq, prev);
    }

    return dummy_task_of(se);
}


static void set_curr_task_dummy(struct rq *rq)
{
    struct task_struct *p = rq->curr;
    struct sched_dummy_entity *se;

    if (unlikely(!p)) {
        return;
    }

    se = &p->dummy_se;

    se->last_jiffies = get_jiffies_64();
    _enqueue_task_dummy(rq, p);
}

/**
 * Function executed by the periodic scheduler at each tick
 * @param rq the runqueue for the current CPU
 * @param curr A pointer to the running task
 * @param queued ???
 **/
static void task_tick_dummy(struct rq *rq, struct task_struct *curr, int queued)
{
    struct sched_dummy_entity* se = &curr->dummy_se;
    struct sched_dummy_entity* top_prio;

    dummy_age_tree(rq, &rq->dummy.root);

    top_prio = rb_get_leftmost(&rq->dummy.root);

    if (top_prio != se) {
        resched_curr(rq);
    }
}

static void switched_from_dummy(struct rq *rq, struct task_struct *p)
{
    p->dummy_se.last_jiffies = get_jiffies_64();
}

static void switched_to_dummy(struct rq *rq, struct task_struct *p)
{
    p->dummy_se.last_jiffies = get_jiffies_64();
}

static void prio_changed_dummy(struct rq *rq, struct task_struct *p, int oldprio)
{
    struct sched_dummy_entity *se;

    if (unlikely(!p)) {
        return;
    }

    se = &p->dummy_se;

    se->prio_base = p->prio;

    reset_dummy_prio(se);

    if (oldprio < p->prio) {
        //If the priority was higher before change we need to reschedule
        rb_erase(&se->node, &rq->dummy.root);
        weighted_rbtree_insert(&rq->dummy.root, se);
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

    se->last_jiffies = get_jiffies_64();
    reset_dummy_prio(se);
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
