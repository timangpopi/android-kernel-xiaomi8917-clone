/*
 * Author: Alucard_24@XDA
 *
 * Copyright 2012 Alucard_24@XDA
 *
 * Copyright 2018 pascua28@XDA
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/tick.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/module.h>
#include <linux/slab.h>

#if defined(CONFIG_POWERSUSPEND)
#include <linux/powersuspend.h>
#elif defined(CONFIG_HAS_EARLYSUSPEND)
#include <linux/earlysuspend.h>
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

struct hotplug_cpuinfo {
#ifndef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
	u64 prev_cpu_wall;
	u64 prev_cpu_idle;
#endif
	unsigned int up_load;
	unsigned int down_load;
	unsigned int up_freq;
	unsigned int down_freq;
	unsigned int up_rq;
	unsigned int down_rq;
	unsigned int up_rate;
	unsigned int down_rate;
	unsigned int cur_up_rate;
	unsigned int cur_down_rate;
};

static DEFINE_PER_CPU(struct hotplug_cpuinfo, od_hotplug_cpuinfo);

static struct workqueue_struct *alucardhp_wq;

static struct delayed_work alucard_hotplug_work;

static struct hotplug_tuners {
	unsigned int hotplug_sampling_rate;
	unsigned int hotplug_enable;
	unsigned int min_cpus_online;
	unsigned int maxcoreslimit;
	unsigned int hp_io_is_busy;
#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	unsigned int hotplug_suspend;
	bool suspended;
	struct mutex alu_hotplug_mutex;
#endif
} hotplug_tuners_ins = {
	.hotplug_sampling_rate = 65,
	.hotplug_enable = 1,
	.min_cpus_online = 2,
	.maxcoreslimit = 4,
	.hp_io_is_busy = 1,
#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	.hotplug_suspend = 1,
	.suspended = false,
#endif
};

#define DOWN_INDEX		(0)
#define UP_INDEX		(1)

struct runqueue_data {
	unsigned int nr_run_avg;
	int64_t last_time;
	int64_t total_time;
	spinlock_t lock;
};

static struct runqueue_data *rq_data;

static void init_rq_avg_stats(void)
{
	rq_data->nr_run_avg = 0;
	rq_data->last_time = 0;
	rq_data->total_time = 0;
}

static int __init init_rq_avg(void)
{
	rq_data = kzalloc(sizeof(struct runqueue_data), GFP_KERNEL);
	if (rq_data == NULL) {
		pr_err("%s cannot allocate memory\n", __func__);
		return -ENOMEM;
	}
	spin_lock_init(&rq_data->lock);

	return 0;
}

static void exit_rq_avg(void)
{
	kfree(rq_data);
}

static unsigned int get_nr_run_avg(void)
{
	int64_t time_diff = 0;
	int64_t nr_run = 0;
	unsigned long flags = 0;
	int64_t cur_time;
	unsigned int nr_run_avg;

	cur_time = ktime_to_ns(ktime_get());

	spin_lock_irqsave(&rq_data->lock, flags);

	if (rq_data->last_time == 0)
		rq_data->last_time = cur_time;
	if (rq_data->nr_run_avg == 0)
		rq_data->total_time = 0;

	nr_run = nr_running() * 100;
	time_diff = cur_time - rq_data->last_time;
	do_div(time_diff, 1000 * 1000);

	if (time_diff != 0 && rq_data->total_time != 0) {
		nr_run = (nr_run * time_diff) +
			(rq_data->nr_run_avg * rq_data->total_time);
		do_div(nr_run, rq_data->total_time + time_diff);
	}
	rq_data->nr_run_avg = nr_run;
	rq_data->total_time += time_diff;
	rq_data->last_time = cur_time;

	nr_run_avg = rq_data->nr_run_avg;
	rq_data->nr_run_avg = 0;

	spin_unlock_irqrestore(&rq_data->lock, flags);

	return nr_run_avg;
}

typedef enum {IDLE, ON, OFF} HOTPLUG_STATUS;

static void __ref hotplug_work_fn(struct work_struct *work)
{
	unsigned int upmaxcoreslimit = 0;
	unsigned int min_cpus_online = hotplug_tuners_ins.min_cpus_online;
	unsigned int cpu = 0;
	int online_cpu = 0;
	int offline_cpu = 0;
	int online_cpus = 0;
	unsigned int rq_avg;

	HOTPLUG_STATUS hotplug_onoff[NR_CPUS] = {IDLE, IDLE, IDLE, IDLE};
	int delay;
	int io_busy = hotplug_tuners_ins.hp_io_is_busy;

	rq_avg = get_nr_run_avg();
	upmaxcoreslimit = hotplug_tuners_ins.maxcoreslimit;

#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	if (hotplug_tuners_ins.suspended)
		return;
#endif

	get_online_cpus();
	online_cpus = num_online_cpus();
	for_each_online_cpu(cpu) {
		struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);
		unsigned int upcpu = (cpu + 1);
#ifndef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
		u64 cur_wall_time, cur_idle_time;
		unsigned int wall_time, idle_time;
#endif
		int cur_load = -1;
		unsigned int cur_freq = 0;
		bool check_up = false, check_down = false;

#ifdef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
		cur_load = cpufreq_quick_get_util(cpu);
#else
		cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, io_busy);

		wall_time = (unsigned int)
				(cur_wall_time -
					pcpu_info->prev_cpu_wall);
		pcpu_info->prev_cpu_wall = cur_wall_time;

		idle_time = (unsigned int)
				(cur_idle_time -
					pcpu_info->prev_cpu_idle);
		pcpu_info->prev_cpu_idle = cur_idle_time;

		/* if wall_time < idle_time, evaluate cpu load next time */
		if (wall_time >= idle_time) {
			/*
			 * if wall_time is equal to idle_time,
			 * cpu_load is equal to 0
			 */
			cur_load = wall_time > idle_time ? (100 *
				(wall_time - idle_time)) / wall_time : 0;
		}
#endif

		/* if cur_load < 0, evaluate cpu load next time */
		if (cur_load >= 0) {
			/* get the cpu current frequency */
			/* cur_freq = acpuclk_get_rate(cpu); */
			cur_freq = cpufreq_quick_get(cpu);

			if (pcpu_info->cur_up_rate > pcpu_info->up_rate)
				pcpu_info->cur_up_rate = 1;

			if (pcpu_info->cur_down_rate > pcpu_info->down_rate)
				pcpu_info->cur_down_rate = 1;

			check_up = (pcpu_info->cur_up_rate % pcpu_info->up_rate == 0);
			check_down = (pcpu_info->cur_down_rate % pcpu_info->down_rate == 0);

			if (cpu > 0 
				&& ((online_cpus - offline_cpu) > upmaxcoreslimit)) {
					hotplug_onoff[cpu] = OFF;
					pcpu_info->cur_up_rate = 1;
					pcpu_info->cur_down_rate = 1;
					++offline_cpu;
					continue;
			} else if ((online_cpus + online_cpu) < min_cpus_online) {

					if (upcpu < upmaxcoreslimit) {
						if (!cpu_online(upcpu)) {
							hotplug_onoff[upcpu] = ON;
							pcpu_info->cur_up_rate = 1;
							pcpu_info->cur_down_rate = 1;
							++online_cpu;
						}
					}
					continue;
			}

			if (upcpu > 0
				&& upcpu < upmaxcoreslimit
				&& (!cpu_online(upcpu))
				&& (online_cpus + online_cpu) < upmaxcoreslimit
 			    && cur_load >= pcpu_info->up_load 
				&& cur_freq >= pcpu_info->up_freq 
				&& rq_avg > pcpu_info->up_rq) {
					++pcpu_info->cur_up_rate;
					if (check_up) {
#if 0
						pr_info("CPU[%u], UPCPU[%u], cur_freq[%u], cur_load[%u], rq_avg[%u], up_rate[%u]\n", cpu, upcpu, cur_freq, cur_load, rq_avg, pcpu_info->cur_up_rate);
#endif
						hotplug_onoff[upcpu] = ON;
						pcpu_info->cur_up_rate = 1;
						pcpu_info->cur_down_rate = 1;
						++online_cpu;
					}
			} else if (cpu >= min_cpus_online
					   && (cur_load < pcpu_info->down_load 
						   || (cur_freq <= pcpu_info->down_freq
						       && rq_avg <= pcpu_info->down_rq))) {
							++pcpu_info->cur_down_rate;
							if (check_down) {
#if 0
								pr_info("CPU[%u], cur_freq[%u], cur_load[%u], rq_avg[%u], down_rate[%u]\n", cpu, cur_freq, cur_load, rq_avg, pcpu_info->cur_down_rate);
#endif
								hotplug_onoff[cpu] = OFF;
								pcpu_info->cur_up_rate = 1;
								pcpu_info->cur_down_rate = 1;
								++offline_cpu;
							}
			} else {
				pcpu_info->cur_up_rate = 1;
				pcpu_info->cur_down_rate = 1;
			}
		}
	}
	put_online_cpus();

	for (cpu = 1; cpu < NR_CPUS; cpu++) {
		if (hotplug_onoff[cpu] == ON)
			cpu_up(cpu);
		else if (hotplug_onoff[cpu] == OFF)
			cpu_down(cpu);
	}

	delay = msecs_to_jiffies(hotplug_tuners_ins.hotplug_sampling_rate);

/*	if (num_online_cpus() > 1) {
#if 0
		pr_info("NR_CPUS[%u], jiffies[%ld], delay[%u]\n", num_online_cpus(), jiffies, delay);
#endif
		delay -= jiffies % delay;
	}
*/
	queue_delayed_work_on(0, alucardhp_wq, &alucard_hotplug_work,
							  delay);
}

static void wakeup_boost()
{
	unsigned int cpu;
	for_each_possible_cpu(cpu) {
		if (!cpu_online(cpu))
			cpu_up(cpu);
	}

	msleep(2500);
	INIT_DELAYED_WORK(&alucard_hotplug_work, hotplug_work_fn);
	queue_delayed_work_on(0, alucardhp_wq, &alucard_hotplug_work,
			msecs_to_jiffies(hotplug_tuners_ins.hotplug_sampling_rate));

}



#if defined(CONFIG_POWERSUSPEND) || defined(CONFIG_HAS_EARLYSUSPEND)
#ifdef CONFIG_POWERSUSPEND
static void __ref alucard_hotplug_suspend(struct power_suspend *handler)
#else
static void __ref alucard_hotplug_early_suspend(struct early_suspend *handler)
#endif
{
	unsigned int cpu;
	if (hotplug_tuners_ins.hotplug_enable == 1
		&& hotplug_tuners_ins.hotplug_suspend == 1) { 
			mutex_lock(&hotplug_tuners_ins.alu_hotplug_mutex);
			hotplug_tuners_ins.suspended = true;
			mutex_unlock(&hotplug_tuners_ins.alu_hotplug_mutex);

			cancel_delayed_work_sync(&alucard_hotplug_work);

			for_each_possible_cpu(cpu)
				if (cpu != 0 && cpu_online(cpu))
					cpu_down(cpu);

			cpu_up(1);
	}

}

#ifdef CONFIG_POWERSUSPEND
static void __ref alucard_hotplug_resume(struct power_suspend *handler)
#else
static void __ref alucard_hotplug_late_resume(
				struct early_suspend *handler)
#endif
{
	if (hotplug_tuners_ins.hotplug_enable == 1
		&& hotplug_tuners_ins.hotplug_suspend == 1) {
			mutex_lock(&hotplug_tuners_ins.alu_hotplug_mutex);
			hotplug_tuners_ins.suspended = false;
			// wake up everyone
			mutex_unlock(&hotplug_tuners_ins.alu_hotplug_mutex);
			wakeup_boost();
	}
}

#ifdef CONFIG_POWERSUSPEND
static struct power_suspend alucard_hotplug_power_suspend_driver = {
	.suspend = alucard_hotplug_suspend,
	.resume = alucard_hotplug_resume,
};
#else
static struct early_suspend alucard_hotplug_early_suspend_driver = {
	.level = EARLY_SUSPEND_LEVEL_DISABLE_FB + 10,
	.suspend = alucard_hotplug_early_suspend,
	.resume = alucard_hotplug_late_resume,
};
#endif
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

static int hotplug_start(void)
{
	unsigned int cpu;
	int ret = 0;

	alucardhp_wq = alloc_workqueue("alucardhp_wq", WQ_HIGHPRI, 0);

	if (!alucardhp_wq) {
		printk(KERN_ERR "Failed to create alucard hotplug workqueue\n");
		return -EFAULT;
	}

	ret = init_rq_avg();
	if (ret) {
		destroy_workqueue(alucardhp_wq);
		return ret;
	}

#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	hotplug_tuners_ins.suspended = false;
#endif

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

#ifndef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
		pcpu_info->prev_cpu_idle = get_cpu_idle_time(cpu,
				&pcpu_info->prev_cpu_wall, hotplug_tuners_ins.hp_io_is_busy);
#endif
		pcpu_info->cur_up_rate = 1;
		pcpu_info->cur_down_rate = 1;
	}
	put_online_cpus();

	init_rq_avg_stats();
	INIT_DELAYED_WORK(&alucard_hotplug_work, hotplug_work_fn);
	queue_delayed_work_on(0, alucardhp_wq, &alucard_hotplug_work,
						msecs_to_jiffies(hotplug_tuners_ins.hotplug_sampling_rate));

#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	mutex_init(&hotplug_tuners_ins.alu_hotplug_mutex);
#endif

#if defined(CONFIG_POWERSUSPEND)
	register_power_suspend(&alucard_hotplug_power_suspend_driver);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	register_early_suspend(&alucard_hotplug_early_suspend_driver);
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

	return 0;
}

static void hotplug_stop(void)
{
#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	mutex_destroy(&hotplug_tuners_ins.alu_hotplug_mutex);
#endif

#if defined(CONFIG_POWERSUSPEND)
	unregister_power_suspend(&alucard_hotplug_power_suspend_driver);
#elif defined(CONFIG_HAS_EARLYSUSPEND)
	unregister_early_suspend(&alucard_hotplug_early_suspend_driver);
#endif  /* CONFIG_POWERSUSPEND || CONFIG_HAS_EARLYSUSPEND */

	cancel_delayed_work_sync(&alucard_hotplug_work);

	exit_rq_avg();

	destroy_workqueue(alucardhp_wq);
}

#define show_one(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	return sprintf(buf, "%d\n", \
			hotplug_tuners_ins.object);			\
}

show_one(hotplug_sampling_rate, hotplug_sampling_rate);
show_one(hotplug_enable, hotplug_enable);
show_one(min_cpus_online, min_cpus_online);
show_one(maxcoreslimit, maxcoreslimit);
show_one(hp_io_is_busy, hp_io_is_busy);
#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
show_one(hotplug_suspend, hotplug_suspend);
#endif

#define show_pcpu_param(file_name, var_name, num_core)		\
static ssize_t show_##file_name		\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
	struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, num_core - 1); \
	return sprintf(buf, "%u\n", \
			pcpu_info->var_name);		\
}

show_pcpu_param(hotplug_freq_1_1, up_freq, 1);
show_pcpu_param(hotplug_freq_2_1, up_freq, 2);
show_pcpu_param(hotplug_freq_3_1, up_freq, 3);
show_pcpu_param(hotplug_freq_2_0, down_freq, 2);
show_pcpu_param(hotplug_freq_3_0, down_freq, 3);
show_pcpu_param(hotplug_freq_4_0, down_freq, 4);

show_pcpu_param(hotplug_load_1_1, up_load, 1);
show_pcpu_param(hotplug_load_2_1, up_load, 2);
show_pcpu_param(hotplug_load_3_1, up_load, 3);
show_pcpu_param(hotplug_load_2_0, down_load, 2);
show_pcpu_param(hotplug_load_3_0, down_load, 3);
show_pcpu_param(hotplug_load_4_0, down_load, 4);

show_pcpu_param(hotplug_rq_1_1, up_rq, 1);
show_pcpu_param(hotplug_rq_2_1, up_rq, 2);
show_pcpu_param(hotplug_rq_3_1, up_rq, 3);
show_pcpu_param(hotplug_rq_2_0, down_rq, 2);
show_pcpu_param(hotplug_rq_3_0, down_rq, 3);
show_pcpu_param(hotplug_rq_4_0, down_rq, 4);

show_pcpu_param(hotplug_rate_1_1, up_rate, 1);
show_pcpu_param(hotplug_rate_2_1, up_rate, 2);
show_pcpu_param(hotplug_rate_3_1, up_rate, 3);
show_pcpu_param(hotplug_rate_2_0, down_rate, 2);
show_pcpu_param(hotplug_rate_3_0, down_rate, 3);
show_pcpu_param(hotplug_rate_4_0, down_rate, 4);

#define store_pcpu_param(file_name, var_name, num_core)		\
static ssize_t store_##file_name		\
(struct kobject *kobj, struct attribute *attr,				\
	const char *buf, size_t count)					\
{									\
	unsigned int input;						\
	struct hotplug_cpuinfo *pcpu_info; 		\
	int ret;									\
													\
	ret = sscanf(buf, "%u", &input);					\
	if (ret != 1)											\
		return -EINVAL;										\
																\
	pcpu_info = &per_cpu(od_hotplug_cpuinfo, num_core - 1); 	\
															\
	if (input == pcpu_info->var_name) {		\
		return count;						\
	}								\
										\
	pcpu_info->var_name = input;			\
	return count;							\
}

store_pcpu_param(hotplug_freq_1_1, up_freq, 1);
store_pcpu_param(hotplug_freq_2_1, up_freq, 2);
store_pcpu_param(hotplug_freq_3_1, up_freq, 3);
store_pcpu_param(hotplug_freq_2_0, down_freq, 2);
store_pcpu_param(hotplug_freq_3_0, down_freq, 3);
store_pcpu_param(hotplug_freq_4_0, down_freq, 4);

store_pcpu_param(hotplug_load_1_1, up_load, 1);
store_pcpu_param(hotplug_load_2_1, up_load, 2);
store_pcpu_param(hotplug_load_3_1, up_load, 3);
store_pcpu_param(hotplug_load_2_0, down_load, 2);
store_pcpu_param(hotplug_load_3_0, down_load, 3);
store_pcpu_param(hotplug_load_4_0, down_load, 4);

store_pcpu_param(hotplug_rq_1_1, up_rq, 1);
store_pcpu_param(hotplug_rq_2_1, up_rq, 2);
store_pcpu_param(hotplug_rq_3_1, up_rq, 3);
store_pcpu_param(hotplug_rq_2_0, down_rq, 2);
store_pcpu_param(hotplug_rq_3_0, down_rq, 3);
store_pcpu_param(hotplug_rq_4_0, down_rq, 4);

store_pcpu_param(hotplug_rate_1_1, up_rate, 1);
store_pcpu_param(hotplug_rate_2_1, up_rate, 2);
store_pcpu_param(hotplug_rate_3_1, up_rate, 3);
store_pcpu_param(hotplug_rate_2_0, down_rate, 2);
store_pcpu_param(hotplug_rate_3_0, down_rate, 3);
store_pcpu_param(hotplug_rate_4_0, down_rate, 4);

define_one_global_rw(hotplug_freq_1_1);
define_one_global_rw(hotplug_freq_2_0);
define_one_global_rw(hotplug_freq_2_1);
define_one_global_rw(hotplug_freq_3_0);
define_one_global_rw(hotplug_freq_3_1);
define_one_global_rw(hotplug_freq_4_0);

define_one_global_rw(hotplug_load_1_1);
define_one_global_rw(hotplug_load_2_0);
define_one_global_rw(hotplug_load_2_1);
define_one_global_rw(hotplug_load_3_0);
define_one_global_rw(hotplug_load_3_1);
define_one_global_rw(hotplug_load_4_0);

define_one_global_rw(hotplug_rq_1_1);
define_one_global_rw(hotplug_rq_2_0);
define_one_global_rw(hotplug_rq_2_1);
define_one_global_rw(hotplug_rq_3_0);
define_one_global_rw(hotplug_rq_3_1);
define_one_global_rw(hotplug_rq_4_0);

define_one_global_rw(hotplug_rate_1_1);
define_one_global_rw(hotplug_rate_2_0);
define_one_global_rw(hotplug_rate_2_1);
define_one_global_rw(hotplug_rate_3_0);
define_one_global_rw(hotplug_rate_3_1);
define_one_global_rw(hotplug_rate_4_0);

static void cpus_hotplugging(int status) {
	int ret = 0;

	if (status) {
		ret = hotplug_start();
		if (ret)
			status = 0;
	} else {
		hotplug_stop();
	}

	hotplug_tuners_ins.hotplug_enable = status;
}

/* hotplug_sampling_rate */
static ssize_t store_hotplug_sampling_rate(struct kobject *a,
				struct attribute *b,
				const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input, 10);

	if (input == hotplug_tuners_ins.hotplug_sampling_rate)
		return count;

	hotplug_tuners_ins.hotplug_sampling_rate = input;

	return count;
}

/* hotplug_enable */
static ssize_t store_hotplug_enable(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0;

	if (hotplug_tuners_ins.hotplug_enable == input)
		return count;

	if (input > 0) {
		cpus_hotplugging(1);
		hotplug_tuners_ins.hotplug_enable = 1;
	} else {
		cpus_hotplugging(0);
	}
	return count;
}

/* min_cpus_online */
static ssize_t store_min_cpus_online(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1);

	if (hotplug_tuners_ins.min_cpus_online == input)
		return count;

	hotplug_tuners_ins.min_cpus_online = input;

	return count;
}

/* maxcoreslimit */
static ssize_t store_maxcoreslimit(struct kobject *a, struct attribute *b,
				  const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = max(input > NR_CPUS ? NR_CPUS : input, 1);

	if (hotplug_tuners_ins.maxcoreslimit == input)
		return count;

	hotplug_tuners_ins.maxcoreslimit = input;

	return count;
}

/* hp_io_is_busy */
static ssize_t store_hp_io_is_busy(struct kobject *a, struct attribute *b,
				   const char *buf, size_t count)
{
	unsigned int input, j;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	if (input > 1)
		input = 1;

	if (input == hotplug_tuners_ins.hp_io_is_busy)
		return count;

	hotplug_tuners_ins.hp_io_is_busy = !!input;
#ifndef CONFIG_ALUCARD_HOTPLUG_USE_CPU_UTIL
	/* we need to re-evaluate prev_cpu_idle */
	if (hotplug_tuners_ins.hotplug_enable == 1) {
		for_each_online_cpu(j) {
			struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, j);
			pcpu_info->prev_cpu_idle = get_cpu_idle_time(j,
					&pcpu_info->prev_cpu_wall, hotplug_tuners_ins.hp_io_is_busy);
		}
	}
#endif
	return count;
}

/*
 * hotplug_suspend control
 * if set = 1 hotplug will sleep,
 * if set = 0, then hoplug will be active all the time.
 */
#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
static ssize_t store_hotplug_suspend(struct kobject *a,
				struct attribute *b,
				const char *buf, size_t count)
{
	int input;
	int ret;

	ret = sscanf(buf, "%u", &input);
	if (ret != 1)
		return -EINVAL;

	input = input > 0;

	if (hotplug_tuners_ins.hotplug_suspend == input)
		return count;

	if (input > 0)
		hotplug_tuners_ins.hotplug_suspend = 1;
	else {
		hotplug_tuners_ins.hotplug_suspend = 0;
		hotplug_tuners_ins.suspended = false;
	}

	return count;
}
#endif

define_one_global_rw(hotplug_sampling_rate);
define_one_global_rw(hotplug_enable);
define_one_global_rw(min_cpus_online);
define_one_global_rw(maxcoreslimit);
define_one_global_rw(hp_io_is_busy);
#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
define_one_global_rw(hotplug_suspend);
#endif

static struct attribute *alucard_hotplug_attributes[] = {
	&hotplug_sampling_rate.attr,
	&hotplug_enable.attr,
	&hotplug_freq_1_1.attr,
	&hotplug_freq_2_0.attr,
	&hotplug_freq_2_1.attr,
	&hotplug_freq_3_0.attr,
	&hotplug_freq_3_1.attr,
	&hotplug_freq_4_0.attr,
	&hotplug_load_1_1.attr,
	&hotplug_load_2_0.attr,
	&hotplug_load_2_1.attr,
	&hotplug_load_3_0.attr,
	&hotplug_load_3_1.attr,
	&hotplug_load_4_0.attr,
	&hotplug_rq_1_1.attr,
	&hotplug_rq_2_0.attr,
	&hotplug_rq_2_1.attr,
	&hotplug_rq_3_0.attr,
	&hotplug_rq_3_1.attr,
	&hotplug_rq_4_0.attr,
	&hotplug_rate_1_1.attr,
	&hotplug_rate_2_0.attr,
	&hotplug_rate_2_1.attr,
	&hotplug_rate_3_0.attr,
	&hotplug_rate_3_1.attr,
	&hotplug_rate_4_0.attr,
	&min_cpus_online.attr,
	&maxcoreslimit.attr,
	&hp_io_is_busy.attr,
#if defined(CONFIG_POWERSUSPEND) || \
	defined(CONFIG_HAS_EARLYSUSPEND)
	&hotplug_suspend.attr,
#endif
	NULL
};

static struct attribute_group alucard_hotplug_attr_group = {
	.attrs = alucard_hotplug_attributes,
	.name = "alucard_hotplug",
};

static int __init alucard_hotplug_init(void)
{
	int ret;
	unsigned int cpu;
	unsigned int hotplug_freq[NR_CPUS][2] = {
		{0, 1094400},
		{800000, 998400},
		{800000, 998400},
		{800000, 0}
	};
	unsigned int hotplug_load[NR_CPUS][2] = {
		{0, 60},
		{30, 70},
		{40, 70},
		{40, 0}
	};
	unsigned int hotplug_rq[NR_CPUS][2] = {
		{0, 100},
		{100, 200},
		{200, 300},
		{300, 0}
	};
	unsigned int hotplug_rate[NR_CPUS][2] = {
		{1, 1},
		{4, 1},
		{4, 1},
		{4, 1}
	};

	ret = sysfs_create_group(kernel_kobj, &alucard_hotplug_attr_group);
	if (ret) {
		printk(KERN_ERR "failed at(%d)\n", __LINE__);
		return ret;
	}

	/* INITIALIZE PCPU VARS */
	for_each_possible_cpu(cpu) {
		struct hotplug_cpuinfo *pcpu_info = &per_cpu(od_hotplug_cpuinfo, cpu);

		pcpu_info->up_freq = hotplug_freq[cpu][UP_INDEX];
		pcpu_info->down_freq = hotplug_freq[cpu][DOWN_INDEX];
		pcpu_info->up_load = hotplug_load[cpu][UP_INDEX];
		pcpu_info->down_load = hotplug_load[cpu][DOWN_INDEX];
		pcpu_info->up_rq = hotplug_rq[cpu][UP_INDEX];
		pcpu_info->down_rq = hotplug_rq[cpu][DOWN_INDEX];
		pcpu_info->up_rate = hotplug_rate[cpu][UP_INDEX];
		pcpu_info->down_rate = hotplug_rate[cpu][DOWN_INDEX];
	}

	if (hotplug_tuners_ins.hotplug_enable == 1) {
		hotplug_start();
	}

	return ret;
}

static void __exit alucard_hotplug_exit(void)
{
	if (hotplug_tuners_ins.hotplug_enable == 1) {
		hotplug_stop();
	}

	sysfs_remove_group(kernel_kobj, &alucard_hotplug_attr_group);
}
MODULE_AUTHOR("Alucard_24@XDA");
MODULE_DESCRIPTION("'alucard_hotplug' - A cpu hotplug driver for "
	"capable processors");
MODULE_LICENSE("GPL");

late_initcall(alucard_hotplug_init);
