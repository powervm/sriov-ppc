/*
 *  cpuidle-powernv - idle state cpuidle driver.
 *  Adapted from drivers/cpuidle/cpuidle-pseries
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/moduleparam.h>
#include <linux/cpuidle.h>
#include <linux/cpu.h>
#include <linux/notifier.h>
#include <linux/clockchips.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <asm/machdep.h>
#include <asm/firmware.h>
#include <asm/opal.h>
#include <asm/runlatch.h>
#include <asm/cpuidle.h>

/*
 * Expose only those Hardware idle states via the cpuidle framework
 * that have latency value below POWERNV_THRESHOLD_LATENCY_NS.
 */
#define POWERNV_THRESHOLD_LATENCY_NS 200000

struct cpuidle_driver powernv_idle_driver = {
	.name             = "powernv_idle",
	.owner            = THIS_MODULE,
};

static int max_idle_state;
static struct cpuidle_state *cpuidle_state_table;

struct stop_psscr_table {
	u64 val;
	u64 mask;
};

static struct stop_psscr_table stop_psscr_table[CPUIDLE_STATE_MAX];

static u64 snooze_timeout;
static bool snooze_timeout_en;

static int snooze_loop(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index)
{
	u64 snooze_exit_time;

	local_irq_enable();
	set_thread_flag(TIF_POLLING_NRFLAG);

	snooze_exit_time = get_tb() + snooze_timeout;
	ppc64_runlatch_off();
	while (!need_resched()) {
		HMT_low();
		HMT_very_low();
		if (snooze_timeout_en && get_tb() > snooze_exit_time)
			break;
	}

	HMT_medium();
	ppc64_runlatch_on();
	clear_thread_flag(TIF_POLLING_NRFLAG);
	smp_mb();
	return index;
}

static int nap_loop(struct cpuidle_device *dev,
			struct cpuidle_driver *drv,
			int index)
{
	ppc64_runlatch_off();
	power7_idle();
	ppc64_runlatch_on();
	return index;
}

/* Register for fastsleep only in oneshot mode of broadcast */
#ifdef CONFIG_TICK_ONESHOT
static int fastsleep_loop(struct cpuidle_device *dev,
				struct cpuidle_driver *drv,
				int index)
{
	unsigned long old_lpcr = mfspr(SPRN_LPCR);
	unsigned long new_lpcr;

	if (unlikely(system_state < SYSTEM_RUNNING))
		return index;

	new_lpcr = old_lpcr;
	/* Do not exit powersave upon decrementer as we've setup the timer
	 * offload.
	 */
	new_lpcr &= ~LPCR_PECE1;

	mtspr(SPRN_LPCR, new_lpcr);
	power7_sleep();

	mtspr(SPRN_LPCR, old_lpcr);

	return index;
}
#endif

static int stop_loop(struct cpuidle_device *dev,
		     struct cpuidle_driver *drv,
		     int index)
{
	ppc64_runlatch_off();
	power9_idle_stop(stop_psscr_table[index].val,
			 stop_psscr_table[index].mask);
	ppc64_runlatch_on();
	return index;
}

/*
 * States for dedicated partition case.
 */
static struct cpuidle_state powernv_states[CPUIDLE_STATE_MAX] = {
	{ /* Snooze */
		.name = "snooze",
		.desc = "snooze",
		.exit_latency = 0,
		.target_residency = 0,
		.enter = snooze_loop },
};

static int powernv_cpuidle_add_cpu_notifier(struct notifier_block *n,
			unsigned long action, void *hcpu)
{
	int hotcpu = (unsigned long)hcpu;
	struct cpuidle_device *dev =
				per_cpu(cpuidle_devices, hotcpu);

	if (dev && cpuidle_get_driver()) {
		switch (action) {
		case CPU_ONLINE:
		case CPU_ONLINE_FROZEN:
			cpuidle_pause_and_lock();
			cpuidle_enable_device(dev);
			cpuidle_resume_and_unlock();
			break;

		case CPU_DEAD:
		case CPU_DEAD_FROZEN:
			cpuidle_pause_and_lock();
			cpuidle_disable_device(dev);
			cpuidle_resume_and_unlock();
			break;

		default:
			return NOTIFY_DONE;
		}
	}
	return NOTIFY_OK;
}

static struct notifier_block setup_hotplug_notifier = {
	.notifier_call = powernv_cpuidle_add_cpu_notifier,
};

/*
 * powernv_cpuidle_driver_init()
 */
static int powernv_cpuidle_driver_init(void)
{
	int idle_state;
	struct cpuidle_driver *drv = &powernv_idle_driver;

	drv->state_count = 0;

	for (idle_state = 0; idle_state < max_idle_state; ++idle_state) {
		/* Is the state not enabled? */
		if (cpuidle_state_table[idle_state].enter == NULL)
			continue;

		drv->states[drv->state_count] =	/* structure copy */
			cpuidle_state_table[idle_state];

		drv->state_count += 1;
	}

	/*
	 * On PowerNV platform cpu_present may be less that cpu_possible
	 * in cases where firmware detects the cpu, but it is not available
	 * for OS.  Such CPUs are not hotplugable at runtime on PowerNV
	 * platform and hence sysfs files are not created for those.
	 * Generic topology_init() would skip creating sysfs directories
	 * for cpus that are not present and not hotplugable later at
	 * runtime.
	 *
	 * drv->cpumask defaults to cpu_possible_mask in __cpuidle_driver_init().
	 * This breaks cpuidle on powernv where sysfs files are not created for
	 * cpus in cpu_possible_mask that cannot be hot-added.
	 *
	 * Hence at runtime sysfs nodes are present for cpus only in
	 * cpu_present_mask. Trying cpuidle_register_device() on cpu without
	 * sysfs node is incorrect.
	 *
	 * Hence pass correct cpu mask to generic cpuidle driver.
	 */

	drv->cpumask = (struct cpumask *)cpu_present_mask;

	return 0;
}

static inline void add_powernv_state(int index, const char *name,
				     unsigned int flags,
				     int (*idle_fn)(struct cpuidle_device *,
						    struct cpuidle_driver *,
						    int),
				     unsigned int target_residency,
				     unsigned int exit_latency,
				     u64 psscr_val, u64 psscr_mask)
{
	strlcpy(powernv_states[index].name, name, CPUIDLE_NAME_LEN);
	strlcpy(powernv_states[index].desc, name, CPUIDLE_NAME_LEN);
	powernv_states[index].flags = flags;
	powernv_states[index].target_residency = target_residency;
	powernv_states[index].exit_latency = exit_latency;
	powernv_states[index].enter = idle_fn;
	stop_psscr_table[index].val = psscr_val;
	stop_psscr_table[index].mask = psscr_mask;
}

static int powernv_add_idle_states(void)
{
	struct device_node *power_mgt;
	int nr_idle_states = 1; /* Snooze */
	int dt_idle_states;
	u32 latency_ns[CPUIDLE_STATE_MAX];
	u32 residency_ns[CPUIDLE_STATE_MAX];
	u32 flags[CPUIDLE_STATE_MAX];
	u64 psscr_val[CPUIDLE_STATE_MAX];
	u64 psscr_mask[CPUIDLE_STATE_MAX];
	const char *names[CPUIDLE_STATE_MAX];
	u32 has_stop_states = 0;
	int i, rc;

	/* Currently we have snooze statically defined */

	power_mgt = of_find_node_by_path("/ibm,opal/power-mgt");
	if (!power_mgt) {
		pr_warn("opal: PowerMgmt Node not found\n");
		goto out;
	}

	/* Read values of any property to determine the num of idle states */
	dt_idle_states = of_property_count_u32_elems(power_mgt, "ibm,cpu-idle-state-flags");
	if (dt_idle_states < 0) {
		pr_warn("cpuidle-powernv: no idle states found in the DT\n");
		goto out;
	}

	/*
	 * Since snooze is used as first idle state, max idle states allowed is
	 * CPUIDLE_STATE_MAX -1
	 */
	if (dt_idle_states > CPUIDLE_STATE_MAX - 1) {
		pr_warn("cpuidle-powernv: discovered idle states more than allowed");
		dt_idle_states = CPUIDLE_STATE_MAX - 1;
	}

	if (of_property_read_u32_array(power_mgt,
			"ibm,cpu-idle-state-flags", flags, dt_idle_states)) {
		pr_warn("cpuidle-powernv : missing ibm,cpu-idle-state-flags in DT\n");
		goto out;
	}

	if (of_property_read_u32_array(power_mgt,
		"ibm,cpu-idle-state-latencies-ns", latency_ns,
		dt_idle_states)) {
		pr_warn("cpuidle-powernv: missing ibm,cpu-idle-state-latencies-ns in DT\n");
		goto out;
	}
	if (of_property_read_string_array(power_mgt,
		"ibm,cpu-idle-state-names", names, dt_idle_states) < 0) {
		pr_warn("cpuidle-powernv: missing ibm,cpu-idle-state-names in DT\n");
		goto out;
	}

	/*
	 * If the idle states use stop instruction, probe for psscr values
	 * and psscr mask which are necessary to specify required stop level.
	 */
	has_stop_states = (flags[0] &
			   (OPAL_PM_STOP_INST_FAST | OPAL_PM_STOP_INST_DEEP));
	if (has_stop_states) {
		if (of_property_read_u64_array(power_mgt,
		    "ibm,cpu-idle-state-psscr", psscr_val, dt_idle_states)) {
			pr_warn("cpuidle-powernv: missing ibm,cpu-idle-state-psscr in DT\n");
			goto out;
		}

		if (of_property_read_u64_array(power_mgt,
					       "ibm,cpu-idle-state-psscr-mask",
						psscr_mask, dt_idle_states)) {
			pr_warn("cpuidle-powernv:Missing ibm,cpu-idle-state-psscr-mask in DT\n");
			goto out;
		}
	}

	rc = of_property_read_u32_array(power_mgt,
		"ibm,cpu-idle-state-residency-ns", residency_ns, dt_idle_states);

	for (i = 0; i < dt_idle_states; i++) {
		unsigned int exit_latency, target_residency;
		/*
		 * If an idle state has exit latency beyond
		 * POWERNV_THRESHOLD_LATENCY_NS then don't use it
		 * in cpu-idle.
		 */
		if (latency_ns[i] > POWERNV_THRESHOLD_LATENCY_NS)
			continue;
		/*
		 * Firmware passes residency and latency values in ns.
		 * cpuidle expects it in us.
		 */
		exit_latency = latency_ns[i] / 1000;
		if (!rc)
			target_residency = residency_ns[i] / 1000;
		else
			target_residency = 0;

		if (has_stop_states) {
			int err = validate_psscr_val_mask(&psscr_val[i],
							  &psscr_mask[i],
							  flags[i]);
			if (err) {
				report_invalid_psscr_val(psscr_val[i], err);
				continue;
			}
		}

		/*
		 * For nap and fastsleep, use default target_residency
		 * values if f/w does not expose it.
		 */
		if (flags[i] & OPAL_PM_NAP_ENABLED) {
			if (!rc)
				target_residency = 100;
			/* Add NAP state */
			add_powernv_state(nr_idle_states, "Nap",
					  CPUIDLE_FLAG_NONE, nap_loop,
					  target_residency, exit_latency, 0, 0);
		} else if ((flags[i] & OPAL_PM_STOP_INST_FAST) &&
				!(flags[i] & OPAL_PM_TIMEBASE_STOP)) {
			add_powernv_state(nr_idle_states, names[i],
					  CPUIDLE_FLAG_NONE, stop_loop,
					  target_residency, exit_latency,
					  psscr_val[i], psscr_mask[i]);
		}

		/*
		 * All cpuidle states with CPUIDLE_FLAG_TIMER_STOP set must come
		 * within this config dependency check.
		 */
#ifdef CONFIG_TICK_ONESHOT
		if (flags[i] & OPAL_PM_SLEEP_ENABLED ||
			flags[i] & OPAL_PM_SLEEP_ENABLED_ER1) {
			if (!rc)
				target_residency = 300000;
			/* Add FASTSLEEP state */
			add_powernv_state(nr_idle_states, "FastSleep",
					  CPUIDLE_FLAG_TIMER_STOP,
					  fastsleep_loop,
					  target_residency, exit_latency, 0, 0);
		} else if ((flags[i] & OPAL_PM_STOP_INST_DEEP) &&
				(flags[i] & OPAL_PM_TIMEBASE_STOP)) {
			add_powernv_state(nr_idle_states, names[i],
					  CPUIDLE_FLAG_TIMER_STOP, stop_loop,
					  target_residency, exit_latency,
					  psscr_val[i], psscr_mask[i]);
		}
#endif
		nr_idle_states++;
	}
out:
	return nr_idle_states;
}

/*
 * powernv_idle_probe()
 * Choose state table for shared versus dedicated partition
 */
static int powernv_idle_probe(void)
{
	if (cpuidle_disable != IDLE_NO_OVERRIDE)
		return -ENODEV;

	if (firmware_has_feature(FW_FEATURE_OPAL)) {
		cpuidle_state_table = powernv_states;
		/* Device tree can indicate more idle states */
		max_idle_state = powernv_add_idle_states();
		if (max_idle_state > 1) {
			snooze_timeout_en = true;
			snooze_timeout = powernv_states[1].target_residency *
					 tb_ticks_per_usec;
		}
 	} else
 		return -ENODEV;

	return 0;
}

static int __init powernv_processor_idle_init(void)
{
	int retval;

	retval = powernv_idle_probe();
	if (retval)
		return retval;

	powernv_cpuidle_driver_init();
	retval = cpuidle_register(&powernv_idle_driver, NULL);
	if (retval) {
		printk(KERN_DEBUG "Registration of powernv driver failed.\n");
		return retval;
	}

	register_cpu_notifier(&setup_hotplug_notifier);
	printk(KERN_DEBUG "powernv_idle_driver registered\n");
	return 0;
}

device_initcall(powernv_processor_idle_init);
