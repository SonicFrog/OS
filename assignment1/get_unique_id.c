#include <linux/linkage.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <asm/atomic.h>

asmlinkage long sys_get_unique_id(int *uuid)
{
    static atomic_t counter = ATOMIC_INIT(0);
    int current_value;

    current_value = atomic_inc_return(&counter);

    if (put_user(current_value, uuid) != 0)
    {
        atomic_dec(&counter);
        return -EFAULT;
    }

    return 0;
}
