
/* LGE_CHANGE_S
 *
 * do read/mmap profiling during booting
 * in order to use the data as readahead args
 *
 * matia.kim@lge.com 20130403
 */
#include "mount.h"
#include "ext4/ext4.h"
#include "sreadahead_prof.h"

static struct sreadahead_prof prof_buf;

#ifdef CONFIG_VM_EVENT_COUNTERS
static unsigned long vm_chk_jiffies;
#endif

static DECLARE_WAIT_QUEUE_HEAD(prof_state_wait);

static void prof_buf_free_work(struct work_struct *data)
{
	mutex_lock(&prof_buf.ulock);
	if (prof_buf.state == PROF_DONE) {
		mutex_unlock(&prof_buf.ulock);
		return;
	}

	prof_buf.state = PROF_NOT;
	wake_up_interruptible(&prof_state_wait);
	vfree(prof_buf.data);
	prof_buf.data = NULL;
	_DBG("mem of prof_buf is freed by vfree()");
	mutex_unlock(&prof_buf.ulock);
}

static void prof_timer_handler(unsigned long arg)
{
	_DBG("profiling state - %d\n", prof_buf.state);
	schedule_work(&prof_buf.free_work);
}


static ssize_t sreadahead_dbgfs_read(
		struct file *file,
		char __user *buff,
		size_t buff_count,
		loff_t *ppos)
{
	struct sreadahead_profdata data;

	mutex_lock(&prof_buf.ulock);
	if (prof_buf.data == NULL) {
		mutex_unlock(&prof_buf.ulock);
		return 0;
	}

	if (prof_buf.read_cnt >= prof_buf.file_cnt) {
		vfree(prof_buf.data);
		prof_buf.data = NULL;
		mutex_unlock(&prof_buf.ulock);
		return 0;
	}

	data = prof_buf.data[prof_buf.read_cnt++];
	mutex_unlock(&prof_buf.ulock);

	_DBG("%s:%lld:%lld#%s -- read_cnt:%d",
		data.name, data.pos[0],
		data.len, data.procname, prof_buf.read_cnt);

	if (copy_to_user(buff, &data, sizeof(struct sreadahead_profdata)))
		return 0;

	(*ppos) = 0;
	return 1;
}

static ssize_t sreadaheadflag_dbgfs_read(
		struct file *file,
		char __user *buff,
		size_t buff_count,
		loff_t *ppos)
{
	int ret = 0;

	DECLARE_WAITQUEUE(wait, current);
	add_wait_queue(&prof_state_wait, &wait);
	__set_current_state(TASK_INTERRUPTIBLE);

	do {
		if (prof_buf.state == PROF_DONE || prof_buf.state == PROF_NOT) {
			break;
		}
		schedule();
	} while(1);

	if (copy_to_user(buff, &prof_buf.state, sizeof(int))) {
		ret = 0;
	} else {
		ret = sizeof(int);
	}

	(*ppos) = 0;

	__set_current_state(TASK_RUNNING);
	remove_wait_queue(&prof_state_wait, &wait);

	return ret;
}

static ssize_t sreadaheadflag_dbgfs_write(
		struct file *file,
		const char __user *buff,
		size_t count,
		loff_t *ppos)
{
	int state;

	if (copy_from_user(&state, buff, sizeof(int)))
		return 0;

	if (state == PROF_INIT) {
		mutex_lock(&prof_buf.ulock);
		if (prof_buf.state != PROF_NOT) {
			mutex_unlock(&prof_buf.ulock);
			return 0;
		}
		_DBG("PROF_INT");
		prof_buf.state = state;

		_DBG("add timer");
		prof_buf.timer.expires = get_jiffies_64() + (PROF_TIMEOUT * HZ);
		add_timer(&prof_buf.timer);
		mutex_unlock(&prof_buf.ulock);
	} else if (state == PROF_DONE) {
		mutex_lock(&prof_buf.ulock);
		if (prof_buf.state != PROF_RUN) {
			mutex_unlock(&prof_buf.ulock);
			return 0;
		}
		_DBG("PROF_DONE by user daemon(boot_completed)");
		prof_buf.state = state;
		wake_up_interruptible(&prof_state_wait);

		_DBG("del timer");
		del_timer(&prof_buf.timer);
		mutex_unlock(&prof_buf.ulock);
	}

	(*ppos) = 0;
	return sizeof(int);
}

static const struct file_operations sreadaheadflag_dbgfs_fops = {
	.read = sreadaheadflag_dbgfs_read,
	.write = sreadaheadflag_dbgfs_write,
};

static const struct file_operations sreadahead_dbgfs_fops = {
	.read = sreadahead_dbgfs_read,
};

static int __init sreadahead_init(void)
{
	struct dentry *dbgfs_dir;

	/* state init */
	prof_buf.state = PROF_NOT;

	/* lock init */
	mutex_init(&prof_buf.ulock);

	/* timer init */
	init_timer(&prof_buf.timer);
	prof_buf.timer.function = prof_timer_handler;

	/* work struct init */
	INIT_WORK(&prof_buf.free_work, prof_buf_free_work);

	/* debugfs init for sreadahead */
	dbgfs_dir = debugfs_create_dir("sreadahead", NULL);
	if (!dbgfs_dir)
		return -ENOENT;
	debugfs_create_file("profilingdata",
			0444, dbgfs_dir, NULL,
			&sreadahead_dbgfs_fops);
	debugfs_create_file("profilingflag",
			0644, dbgfs_dir, NULL,
			&sreadaheadflag_dbgfs_fops);
	return 0;
}

device_initcall(sreadahead_init);

static int get_absolute_path(unsigned char *buf, int buflen, struct file *filp)
{
	unsigned char tmpstr[buflen];
	struct dentry *tmpdentry = 0;
	struct mount *tmpmnt;
	struct mount *tmpoldmnt;
	tmpmnt = real_mount(filp->f_path.mnt);

	tmpdentry = filp->f_path.dentry->d_parent;
	do {
		tmpoldmnt = tmpmnt;
		while (!IS_ROOT(tmpdentry)) {
			strlcpy(tmpstr, buf, buflen);
			/* byungchul.park@lge.com */
			/* make codes robust */
			strlcpy(buf, tmpdentry->d_name.name, buflen);
			buf[buflen - 1] = '\0';
			if (strlen(buf) + strlen("/") > buflen - 1)
				return -ENOMEM;
			else
				strlcat(buf, "/", buflen);

			if (strlen(buf) + strlen(tmpstr) > (buflen - 1))
				return -ENOMEM;
			else
				strlcat(buf, tmpstr, buflen);

			tmpdentry = tmpdentry->d_parent;
		}
		tmpdentry = tmpmnt->mnt_mountpoint;
		tmpmnt = tmpmnt->mnt_parent;
	} while (tmpmnt != tmpoldmnt);
	strlcpy(tmpstr, buf, buflen);
	strlcpy(buf, "/", 2);
	/* byungchul.park@lge.com */
	/* make codes robust */
	if (strlen(buf) + strlen(tmpstr) > (buflen - 1))
		return -ENOMEM;
	strlcat(buf, tmpstr, buflen);

	return 0;
}

static int is_system_partition(unsigned char *fn) {
	return strncmp((const char*)fn, "/system/", 8) == 0 ? 1 : 0;
}

#ifdef CONFIG_VM_EVENT_COUNTERS
static int check_vm_pgsteal_events(void)
{
	int cpu;
	int i;

	for_each_online_cpu(cpu) {
		struct vm_event_state *this = &per_cpu(vm_event_states, cpu);

		for (i = PGSTEAL_DIRECT_MOVABLE; i > PGREFILL_MOVABLE; i--) {
			if (this->event[i] > 0) {
				return 1;
			}
		}
	}

	return 0;
}

static int check_vm_events(void)
{
	int ret;

	get_online_cpus();
	ret = check_vm_pgsteal_events();
	put_online_cpus();

	return ret;
}
#endif

static int sreadahead_prof_RUN(struct file *filp, size_t len, loff_t pos)
{
	int i;
	int buflen;
	struct sreadahead_profdata data;

	memset(&data, 0x00, sizeof(struct sreadahead_profdata));
	data.len = (long long)len;
	data.pos[0] = pos;
	data.pos[1] = 0;
	data.procname[0] = '\0';
	get_task_comm(data.procname, current);

	buflen = FILE_PATH_LEN;
	if (get_absolute_path(data.name, buflen, filp) < 0)
		return -ENOENT;
	strlcat(data.name, filp->f_path.dentry->d_name.name, buflen);

	if (is_system_partition(data.name) == 0)
		return 0;

	mutex_lock(&prof_buf.ulock);

	/* vfree called or profiling is already done */
	if (prof_buf.data == NULL || prof_buf.state != PROF_RUN) {
		mutex_unlock(&prof_buf.ulock);
		return -EADDRNOTAVAIL;
	}

	for (i = 0; i < prof_buf.file_cnt; ++i) {
		if (strncmp(prof_buf.data[i].name, data.name, FILE_PATH_LEN) == 0)
			break;
	}
	/* add a new entry */
	if (i == prof_buf.file_cnt && i < PROF_BUF_SIZE) {
		strlcpy(prof_buf.data[i].procname, data.procname, PROC_NAME_LEN);
		prof_buf.data[i].procname[PROC_NAME_LEN - 1] = '\0';
		strlcpy(prof_buf.data[i].name, data.name, FILE_PATH_LEN);
		prof_buf.data[i].name[FILE_PATH_LEN - 1] = '\0';
		prof_buf.data[i].pos[0] = prof_buf.data[i].pos[1]
			= ALIGNPAGECACHE(data.pos[0]);
		prof_buf.data[i].pos[1] +=
			E_ALIGNPAGECACHE((long long)data.len);
		prof_buf.data[i].len = prof_buf.data[i].pos[1]
			- prof_buf.data[i].pos[0];
		prof_buf.file_cnt++;

		_DBG("New Entry - %s:%lld:%lld#%s -- cnt:%d",
				prof_buf.data[i].name,
				prof_buf.data[i].pos[0],
				prof_buf.data[i].len,
				prof_buf.data[i].procname,
				prof_buf.file_cnt);
	}

	if (prof_buf.file_cnt >= PROF_BUF_SIZE) {
		_DBG("PROF_DONE by kernel(file_cnt) & del timer");
		prof_buf.state = PROF_DONE;
		wake_up_interruptible(&prof_state_wait);
		del_timer(&prof_buf.timer);
	}

#ifdef CONFIG_VM_EVENT_COUNTERS
	if (time_after(jiffies, vm_chk_jiffies + VM_CHK_INTERVAL)) {
		vm_chk_jiffies = jiffies;
		if (check_vm_events()) {
			_DBG("PROF_DONE by pgsteal");
			prof_buf.state = PROF_DONE;
			wake_up_interruptible(&prof_state_wait);
			del_timer(&prof_buf.timer);
		}
	}
#endif

	mutex_unlock(&prof_buf.ulock);

	return 0;
}

static int sreadahead_profdata_init(void)
{
	mutex_lock(&prof_buf.ulock);
	if (prof_buf.state != PROF_INIT) {
		mutex_unlock(&prof_buf.ulock);
		return 0;
	}

	prof_buf.data = (struct sreadahead_profdata *)
		vmalloc(sizeof(struct sreadahead_profdata) * PROF_BUF_SIZE);

	if (prof_buf.data == NULL)
		return -EADDRNOTAVAIL;

	memset(prof_buf.data, 0x00,
		sizeof(struct sreadahead_profdata) * PROF_BUF_SIZE);
	prof_buf.state = PROF_RUN;

#ifdef CONFIG_VM_EVENT_COUNTERS
	vm_chk_jiffies = jiffies;
#endif
	mutex_unlock(&prof_buf.ulock);

	return 0;
}

int sreadahead_prof(struct file *filp, size_t len, loff_t pos)
{
	if (prof_buf.state == PROF_NOT || prof_buf.state == PROF_DONE)
		return 0;
	if (prof_buf.state == PROF_INIT)
		sreadahead_profdata_init();
	if (prof_buf.state == PROF_RUN) {
		if (filp->f_op == &ext4_file_operations)
			sreadahead_prof_RUN(filp, len, pos);
	}
	return 0;
}
/* LGE_CHANGE_E */
