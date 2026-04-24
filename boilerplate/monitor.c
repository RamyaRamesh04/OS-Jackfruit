#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/timer.h>
#include <linux/sched/signal.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>

#define DEVICE_NAME "monitor"
#define CLASS_NAME  "mon"

#define MONITOR_SET_LIMITS _IOW('m', 1, struct monitor_limits)

struct monitor_limits {
    char container_id[32];
    pid_t target_pid;
    unsigned long soft_limit;
    long hard_limit;
};

static int major;
static struct class* mon_class  = NULL;
static struct device* mon_device = NULL;
static struct timer_list monitor_timer;

struct container_mon {
    char id[32];
    pid_t pid;
    unsigned long soft_limit;
    long hard_limit;
    struct list_head list;
};

static LIST_HEAD(monitored_containers);
static DEFINE_MUTEX(mon_lock);

static long get_rss_bytes(pid_t pid) {
    struct task_struct *task;
    struct pid *pid_struct;
    long rss = 0;

    rcu_read_lock();
    // Use find_vpid and pid_task instead of find_task_by_vpid
    pid_struct = find_vpid(pid);
    if (pid_struct) {
        task = pid_task(pid_struct, PIDTYPE_PID);
        if (task && task->mm) {
            rss = get_mm_rss(task->mm) << PAGE_SHIFT;
        }
    }
    rcu_read_unlock();
    return rss;
}

static void timer_callback(struct timer_list *t) {
    struct container_mon *pos;
    mutex_lock(&mon_lock);
    list_for_each_entry(pos, &monitored_containers, list) {
        long usage = get_rss_bytes(pos->pid);
        if (pos->hard_limit > 0 && usage > pos->hard_limit) {
            struct pid *pid_struct = find_vpid(pos->pid);
            if (pid_struct) {
                kill_pid(pid_struct, SIGKILL, 1);
            }
        }
    }
    mutex_unlock(&mon_lock);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
}

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg) {
    if (cmd == MONITOR_SET_LIMITS) {
        struct monitor_limits req;
        struct container_mon *new_mon;
        if (copy_from_user(&req, (void __user *)arg, sizeof(req))) return -EFAULT;
        new_mon = kmalloc(sizeof(*new_mon), GFP_KERNEL);
        if (!new_mon) return -ENOMEM;
        strncpy(new_mon->id, req.container_id, 31);
        new_mon->pid = req.target_pid;
        new_mon->soft_limit = req.soft_limit;
        new_mon->hard_limit = req.hard_limit;
        mutex_lock(&mon_lock);
        list_add(&new_mon->list, &monitored_containers);
        mutex_unlock(&mon_lock);
        return 0;
    }
    return -EINVAL;
}

static const struct file_operations fops = {
    .unlocked_ioctl = monitor_ioctl,
};

static int __init monitor_init(void) {
    major = register_chrdev(0, DEVICE_NAME, &fops);
    if (major < 0) return major;
    
    mon_class = class_create(CLASS_NAME);
    if (IS_ERR(mon_class)) {
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(mon_class);
    }

    mon_device = device_create(mon_class, NULL, MKDEV(major, 0), NULL, DEVICE_NAME);
    if (IS_ERR(mon_device)) {
        class_destroy(mon_class);
        unregister_chrdev(major, DEVICE_NAME);
        return PTR_ERR(mon_device);
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + msecs_to_jiffies(1000));
    return 0;
}

static void __exit monitor_exit(void) {
    struct container_mon *pos, *tmp;
    
    // Kernel 6.1+ uses timer_delete_sync
    timer_delete_sync(&monitor_timer);
    
    mutex_lock(&mon_lock);
    // list iteration for Kernel 6.x
    list_for_each_entry_safe(pos, tmp, &monitored_containers, list) {
        list_del(&pos->list);
        kfree(pos);
    }
    mutex_unlock(&mon_lock);
    
    device_destroy(mon_class, MKDEV(major, 0));
    class_destroy(mon_class);
    unregister_chrdev(major, DEVICE_NAME);
}

module_init(monitor_init);
module_exit(monitor_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Memory monitor for lightweight container engine");
