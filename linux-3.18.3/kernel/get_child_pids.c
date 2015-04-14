#include <linux/linkage.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/uaccess.h>
#include <linux/rwlock.h>
#include <linux/slab.h>
#include <linux/gfp.h>

asmlinkage long sys_get_child_pids(pid_t *list, size_t limit,
                                   size_t* num_childrens)
{
    size_t child_nr = 0;
    size_t i = 0;

    struct task_struct* current_child;
    size_t *pid_list = NULL, *pid_list_start = NULL;

    if (list == NULL || num_childrens == NULL)
    {
        return -EFAULT;
    }

    pid_list = pid_list_start = kmalloc(sizeof(pid_t) * limit, GFP_KERNEL);

    if (pid_list == NULL)
    {
        return -ENOMEM;
    }

    read_lock(&tasklist_lock);

    list_for_each_entry (current_child, &current->children, sibling)
    {
        child_nr++;

        if (child_nr > limit)
        {
            //No place to store pids so just count additionnal children
            continue;
        }

        *pid_list = current_child->pid;

        pid_list++;
    }

    read_unlock(&tasklist_lock);

    for (i = 0; i < min(child_nr, limit); i++)
    {
        if (put_user(pid_list_start[i], &list[i]) != 0)
        {
            kfree(pid_list_start);
            return -EFAULT;
        }
    }

    kfree(pid_list_start);

    if (put_user(child_nr, num_childrens) != 0)
    {
        return -EFAULT;
    }

    return (child_nr > limit) ? -ENOBUFS : 0;
}
