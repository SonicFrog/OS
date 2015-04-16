## Dummy Scheduler

# Levels of priority

To handle the 5 different levels of priority;
We chose to implement a linked list for each level of priority.
As such, when a new task is recieved it is inserted in the list of appropriate
level of priority. (`enqueue_task_dummy(...)`)

# Aging

All task in list will age on 'Ticking' (`task_tick_dummy(...)`). If a task has 
aged too much it will be moved at the tail of the next more prioritized 
task list. (lower priority value, if it was in priority list 4 it will go to 
priority list 3).

# Picking the next task

Going from the most prioritized task list to the least (so in increasing
priority value order) the scheduler checks if the task list is not empty, if it
is it looks in the next task list. If it's not then the first task of the list
will become the current task. (`pick_next_task_dummy(...)`)

