/*
 * Exynos Core Sparing Governor - Exynos Mobile Scheduler
 *
 * Copyright (C) 2019 Samsung Electronics Co., Ltd
 * Choonghoon Park <choong.park@samsung.com>
 */

#include <dt-bindings/soc/samsung/exynos-ecs.h>

#include <trace/events/ems.h>
#include <trace/events/ems_debug.h>

#include "../sched.h"
#include "ems.h"

struct ecs_governor {
	struct list_head	*modes;
	struct list_head	*domains;

	unsigned int		enabled;

	struct ecs_mode		*cur_mode;

	struct cpumask		heavy_cpus;
	struct cpumask		busy_cpus;
	struct cpumask		cpus;

	/* members for sysfs */
	struct kobject		kobj;
	struct mutex		attrib_lock;
} ecs_gov;

static LIST_HEAD(default_mode_list);
static LIST_HEAD(default_domain_list);

static DEFINE_SPINLOCK(ecs_lock);
static DEFINE_RAW_SPINLOCK(ecs_load_lock);
/**********************************************************************************
 *				   External API					  *
 **********************************************************************************/
static void __ecs_update_system_status(struct ecs_domain *domain)
{
	int cpu;
	unsigned int util_sum = 0, util_avg, nr_running_sum = 0, num_of_cpus = 0;

	for_each_cpu_and(cpu, &domain->cpus, cpu_online_mask) {
		util_sum += ml_cpu_util(cpu);
		num_of_cpus++;
		nr_running_sum += cpu_rq(cpu)->nr_running;
	}

	if (!num_of_cpus)
		return;

	util_avg = util_sum / num_of_cpus;

	if (util_avg >= domain->domain_util_avg_thr)
		for_each_cpu_and(cpu, &domain->cpus, cpu_online_mask)
			cpumask_set_cpu(cpu, &ecs_gov.busy_cpus);

	if (nr_running_sum >= domain->domain_nr_running_thr)
		for_each_cpu_and(cpu, &domain->cpus, cpu_online_mask)
			cpumask_set_cpu(cpu, &ecs_gov.busy_cpus);
}

static bool ecs_update_system_status(void)
{
	struct ecs_domain *domain;
	struct cpumask prev_heavy_cpus, prev_busy_cpus;
	struct list_head *domlist = ecs_gov.domains;

	cpumask_copy(&prev_heavy_cpus, &ecs_gov.heavy_cpus);
	cpumask_copy(&prev_busy_cpus, &ecs_gov.busy_cpus);

	cpumask_clear(&ecs_gov.heavy_cpus);
	cpumask_clear(&ecs_gov.busy_cpus);

	list_for_each_entry(domain, domlist, list)
		__ecs_update_system_status(domain);

	if (cpumask_equal(&prev_busy_cpus, &ecs_gov.busy_cpus) &&
		cpumask_equal(&prev_heavy_cpus, &ecs_gov.heavy_cpus))
		return 0;

	return 1;
}

static struct ecs_mode* ecs_get_base_mode(void)
{
	return list_first_entry(ecs_gov.modes, struct ecs_mode, list);
}

static struct ecs_mode* ecs_get_mode(void)
{
	struct ecs_mode *mode, *target_mode = NULL;
	struct list_head *modlist = ecs_gov.modes;

	/* Modes are in ascending order */
	list_for_each_entry_reverse(mode, modlist, list) {
		if (mode->mode & SAVING) {
			if (cpumask_weight(&mode->cpus) <= cpumask_weight(&ecs_gov.busy_cpus))
				continue;
		}

		target_mode = mode;
		break;
	}

	if (!target_mode)
		target_mode = ecs_get_base_mode();

	return target_mode;
}

static int ecs_update_spared_cpus(struct ecs_mode *target_mode)
{
	unsigned long flags;

	spin_lock_irqsave(&ecs_lock, flags);

	if (!ecs_gov.enabled)
		goto unlock;

	ecs_gov.cur_mode = target_mode;
	cpumask_copy(&ecs_gov.cpus, &target_mode->cpus);

unlock:
	spin_unlock_irqrestore(&ecs_lock, flags);

	return 0;
}

void ecs_update(void)
{
	unsigned long flags;
	struct ecs_mode *target_mode;
	bool updated;

	if (!ecs_gov.enabled)
		return;

	if (!raw_spin_trylock_irqsave(&ecs_load_lock, flags))
		return;

	/* update cpumask to judge needs to spare cpus */
	updated = ecs_update_system_status();
	if (!updated)
		goto unlock;

	/* get mode */
	target_mode = ecs_get_mode();

	/* request mode */
	if (ecs_gov.cur_mode != target_mode)
		ecs_update_spared_cpus(target_mode);

unlock:
	trace_ecs_update(*(unsigned int *)&ecs_gov.heavy_cpus,
			 *(unsigned int *)&ecs_gov.busy_cpus,
			 *(unsigned int *)&ecs_gov.cpus);

	raw_spin_unlock_irqrestore(&ecs_load_lock, flags);
}

int ecs_is_sparing_cpu(int cpu)
{
	if (!ecs_gov.enabled)
		return false;

	return !cpumask_test_cpu(cpu, &ecs_gov.cpus);
}

struct cpumask *ecs_cpus_allowed(void)
{
	return &ecs_gov.cpus;
}

/****************************************************************/
/*		   emstune mode update notifier			*/
/****************************************************************/
static int ecs_mode_update_callback(struct notifier_block *nb,
				unsigned long val, void *v)
{
	struct emstune_set *cur_set = (struct emstune_set *)v;

	ecs_gov.modes = cur_set->ecs.p_mode_list;
	ecs_gov.domains = cur_set->ecs.p_domain_list;

	return NOTIFY_OK;
}

static struct notifier_block ecs_mode_update_notifier = {
	.notifier_call = ecs_mode_update_callback,
};

/**********************************************************************************
 *					SYSFS					  *
 **********************************************************************************/
#define to_ecs_gov(k) container_of(k, struct ecs_governor, kobj)
#define to_domain(k) container_of(k, struct ecs_domain, kobj)
#define to_mode(k) container_of(k, struct ecs_mode, kobj)

#define ecs_show(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, char *buf)					\
{									\
	struct ecs_governor *ecs = to_ecs_gov(kobj);			\
									\
	return sprintf(buf, "%u\n", ecs->object);			\
}

#define ecs_store(file_name, object)					\
static ssize_t store_##file_name					\
(struct kobject *kobj, const char *buf, size_t count)			\
{									\
	int ret;							\
	unsigned int val;						\
	struct ecs_governor *ecs = to_ecs_gov(kobj);			\
									\
	ret = kstrtoint(buf, 10, &val);					\
	if (ret)							\
		return -EINVAL;						\
									\
	ecs->object = val;						\
									\
	return ret ? ret : count;					\
}

#define ecs_domain_show(file_name, object)				\
static ssize_t show_##file_name						\
(struct kobject *kobj, char *buf)					\
{									\
	struct ecs_domain *domain = to_domain(kobj);			\
									\
	return sprintf(buf, "%u\n", domain->object);			\
}

#define ecs_domain_store(file_name, object)				\
static ssize_t store_##file_name					\
(struct kobject *kobj, const char *buf, size_t count)			\
{									\
	int ret;							\
	unsigned int val;						\
	struct ecs_domain *domain = to_domain(kobj);			\
									\
	ret = kstrtoint(buf, 10, &val);					\
	if (ret)							\
		return -EINVAL;						\
									\
	domain->object = val;						\
									\
	return ret ? ret : count;					\
}

#define ecs_mode_show(file_name, object)				\
static ssize_t show_##file_name						\
(struct kobject *kobj, char *buf)					\
{									\
	struct ecs_mode *mode = to_mode(kobj);				\
									\
	return sprintf(buf, "%u\n", mode->object);			\
}

#define ecs_mode_store(file_name, object)				\
static ssize_t store_##file_name					\
(struct kobject *kobj, const char *buf, size_t count)			\
{									\
	int ret;							\
	unsigned int val;						\
	struct ecs_mode *mode = to_mode(kobj);				\
									\
	ret = kstrtoint(buf, 10, &val);					\
	if (ret)							\
		return -EINVAL;						\
									\
	mode->object = val;						\
									\
	return ret ? ret : count;					\
}

ecs_show(enabled, enabled);

ecs_domain_store(domain_util_avg_thr, domain_util_avg_thr);
ecs_domain_store(domain_nr_running_thr, domain_nr_running_thr);
ecs_domain_show(domain_util_avg_thr, domain_util_avg_thr);
ecs_domain_show(domain_nr_running_thr, domain_nr_running_thr);

static int ecs_set_enable(bool enable)
{
	struct ecs_mode *base_mode = ecs_get_base_mode();
	unsigned long flags;

	spin_lock_irqsave(&ecs_lock, flags);

	if (enable == ecs_gov.enabled)
		goto unlock;

	ecs_gov.enabled = enable;
	ecs_gov.cur_mode = base_mode;
	cpumask_copy(&ecs_gov.cpus, &base_mode->cpus);

unlock:
	spin_unlock_irqrestore(&ecs_lock, flags);

	return 0;
}

static ssize_t store_enabled(struct kobject *kobj,
			const char *buf, size_t count)
{
	int ret;
	unsigned int val;

	ret = kstrtoint(buf, 10, &val);
	if (ret)
		return -EINVAL;

	if (val > 0)
		ret = ecs_set_enable(true);
	else
		ret = ecs_set_enable(false);

	return ret ? ret : count;
}

struct ecs_attr {
	struct attribute attr;
	ssize_t (*show)(struct kobject *, char *);
	ssize_t (*store)(struct kobject *, const char *, size_t count);
};

#define ecs_attr_ro(_name)						\
static struct ecs_attr _name =						\
__ATTR(_name, 0444, show_##_name, NULL)

#define ecs_attr_rw(_name)						\
static struct ecs_attr _name =						\
__ATTR(_name, 0644, show_##_name, store_##_name)

#define to_attr(a) container_of(a, struct ecs_attr, attr)

static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct ecs_attr *hattr = to_attr(attr);
	ssize_t ret;

	mutex_lock(&ecs_gov.attrib_lock);
	ret = hattr->show(kobj, buf);
	mutex_unlock(&ecs_gov.attrib_lock);

	return ret;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		     const char *buf, size_t count)
{
	struct ecs_attr *hattr = to_attr(attr);
	ssize_t ret = -EINVAL;

	mutex_lock(&ecs_gov.attrib_lock);
	ret = hattr->store(kobj, buf, count);
	mutex_unlock(&ecs_gov.attrib_lock);

	return ret;
}

ecs_attr_rw(enabled);

ecs_attr_rw(domain_util_avg_thr);
ecs_attr_rw(domain_nr_running_thr);

static struct attribute *ecs_attrs[] = {
	&enabled.attr,
	NULL
};

static struct attribute *ecs_domain_attrs[] = {
	&domain_util_avg_thr.attr,
	&domain_nr_running_thr.attr,
	NULL
};

static struct attribute *ecs_mode_attrs[] = {
	NULL
};

static const struct sysfs_ops ecs_sysfs_ops = {
	.show	= show,
	.store	= store,
};

static struct kobj_type ktype_domain = {
	.sysfs_ops	= &ecs_sysfs_ops,
	.default_attrs	= ecs_attrs,
};

static struct kobj_type ktype_ecs_domain = {
	.sysfs_ops	= &ecs_sysfs_ops,
	.default_attrs	= ecs_domain_attrs,
};

static struct kobj_type ktype_ecs_mode = {
	.sysfs_ops	= &ecs_sysfs_ops,
	.default_attrs	= ecs_mode_attrs,
};
/**********************************************************************************
 *				Initialization					  *
 **********************************************************************************/
static int __init ecs_parse_mode(struct device_node *dn)
{
	struct ecs_mode *mode;
	const char *buf;

	mode = kzalloc(sizeof(struct ecs_mode), GFP_KERNEL);
	if (!mode)
		return -ENOBUFS;

	if (of_property_read_string(dn, "cpus", &buf))
		goto free;
	if (cpulist_parse(buf, &mode->cpus))
		goto free;

	if (of_property_read_u32(dn, "mode", &mode->mode))
		goto free;

	list_add_tail(&mode->list, ecs_gov.modes);

	return 0;
free:
	pr_warn("ECS: failed to parse ecs mode\n");
	kfree(mode);
	return -EINVAL;
}

static int __init ecs_parse_domain(struct device_node *dn)
{
	struct ecs_domain *domain;
	const char *buf;

	domain = kzalloc(sizeof(struct ecs_domain), GFP_KERNEL);
	if (!domain)
		return -ENOBUFS;

	if (of_property_read_string(dn, "cpus", &buf))
		goto free;
	if (cpulist_parse(buf, &domain->cpus))
		goto free;

	if (of_property_read_u32(dn, "role", &domain->role))
		goto free;

	if (of_property_read_u32(dn, "domain-util-avg-thr", &domain->domain_util_avg_thr))
		goto free;

	if (of_property_read_u32(dn, "domain-nr-running-thr", &domain->domain_nr_running_thr))
		domain->domain_nr_running_thr = UINT_MAX;

	list_add_tail(&domain->list, ecs_gov.domains);

	return 0;
free:
	pr_warn("ECS: failed to parse ecs domain\n");
	kfree(domain);
	return -EINVAL;
}

static int __init ecs_parse_dt(void)
{
	struct device_node *root, *dn, *child;
	unsigned int temp;

	root = of_find_node_by_path("/ems/ecs");
	if (!root)
		return -EINVAL;

	if (of_property_read_u32(root, "enabled", &temp))
		goto failure;
	if (!temp)  {
		pr_info("ECS: ECS disabled\n");
		return -1;
	}

	/* parse ecs modes */
	dn = of_find_node_by_name(root, "modes");
	if (!dn)
		goto failure;
	for_each_child_of_node(dn, child)
		if (ecs_parse_mode(child))
			goto failure;

	/* parse ecs domains */
	dn = of_find_node_by_name(root, "domains");
	if (!dn)
		goto failure;
	for_each_child_of_node(dn, child)
		if (ecs_parse_domain(child))
			goto failure;

	return 0;

failure:
	return -EINVAL;
}

static int __init __ecs_sysfs_init(void)
{
	int ret;

	ret = kobject_init_and_add(&ecs_gov.kobj, &ktype_domain,
				   ems_kobj, "ecs");
	if (ret) {
		pr_err("ECS: failed to init ecs.kobj: %d\n", ret);
		return -EINVAL;
	}

	return ret;
}

static int __init __ecs_domain_sysfs_init(struct ecs_domain *domain, int num)
{
	int ret;

	ret = kobject_init_and_add(&domain->kobj, &ktype_ecs_domain,
				   &ecs_gov.kobj, "domain%u", num);
	if (ret) {
		pr_err("ECS: failed to init domain->kobj: %d\n", ret);
		return -EINVAL;
	}

	return 0;
}

static int __init __ecs_mode_sysfs_init(struct ecs_mode *mode, int num)
{
	int ret;

	ret = kobject_init_and_add(&mode->kobj, &ktype_ecs_mode,
				   &ecs_gov.kobj, "mode%u", num);
	if (ret) {
		pr_err("ECS: failed to init mode->kobj: %d\n", ret);
		return -EINVAL;
	}

	return 0;
}

static int __init ecs_sysfs_init(void)
{
	struct ecs_domain *domain;
	struct ecs_mode *mode;
	int i = 0;

	/* init attrb_lock */
	mutex_init(&ecs_gov.attrib_lock);

	/* init ecs sysfs */
	if (__ecs_sysfs_init())
		goto failure;

	/* init ecs domain sysfs */
	list_for_each_entry(domain, ecs_gov.domains, list)
		if (__ecs_domain_sysfs_init(domain, i++))
			goto failure;

	/* init ecs mode sysfs */
	i = 0;
	list_for_each_entry(mode, ecs_gov.modes, list)
		if (__ecs_mode_sysfs_init(mode, i++))
			goto failure;
	return 0;

failure:
	return -1;
}

static int __init init_core_sparing(void)
{
	/* Explicit assignment for ecs_gov */
	ecs_gov.enabled = 0;
	cpumask_copy(&ecs_gov.cpus, cpu_active_mask);

	ecs_gov.modes = &default_mode_list;
	ecs_gov.domains = &default_domain_list;

	/* parse dt */
	ecs_parse_dt();

	/* init sysfs */
	if (ecs_sysfs_init())
		goto failure;

	emstune_register_mode_update_notifier(&ecs_mode_update_notifier);

	ecs_gov.enabled = 1;

	return 0;

failure:
	return 0;
} late_initcall(init_core_sparing);
