// SPDX-License-Identifier: GPL-2.0-only
/*
 * Fan driver for the HP ENVY x360 Convertible 15 (AMD, board 876F).
 *
 * The EC drives two fans. Extended EC RAM holds each fan's drive level in
 * percent (0x8102, 0x8129) and its measured speed in RPM / 100 (0x8104,
 * 0x812b). A drive level written there takes effect at once; the EC's own
 * control only catches up after a while and then ramps it slowly back towards
 * its own choice.
 *
 * Nothing else found on this board moves the fans: hp-wmi's fan commands
 * return -EINVAL, HP's thermal-profile write (WMI 0x4c) hands SMM the wrong
 * buffer and always fails, FWPM in the EC's shared memory is ignored, and the
 * EC does not read back the RAM copy of its step table.
 *
 * This driver lays a fan curve over the EC's own control. Once a second it
 * raises any fan whose drive level is below the curve's level for the CPU
 * temperature. It never lowers one, so the EC can always run a fan faster and
 * the fans never run slower than the EC would run them. Rising temperatures
 * apply at once; falling ones decay over ~8 s, after which the EC ramps the
 * fans down itself.
 *
 *   \_TZ.TSZ0.FRSP                 fan 1 speed, RPM (checked against 0x8104)
 *   \_TZ.TSZ0._TMP                 CPU temperature from the EC, deci-kelvin
 *   \_SB.PCI0.LPC0.EC0.FANG(i)     read extended EC RAM byte i
 *   \_SB.PCI0.LPC0.EC0.FANW(i, v)  write it
 */

#include <linux/acpi.h>
#include <linux/cleanup.h>
#include <linux/dmi.h>
#include <linux/hwmon.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/platform_profile.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

#define DRVNAME			"hp_envy_fan"

#define TZ_PATH			"\\_TZ.TSZ0"
#define EC_PATH			"\\_SB.PCI0.LPC0.EC0"
#define WMID_PATH		"\\_SB.WMID"

#define FAN_COUNT		2
#define DUTY_MAX		100

static const u16 fan_duty_idx[FAN_COUNT] = { 0x8102, 0x8129 };	/* percent */
static const u16 fan_speed_idx[FAN_COUNT] = { 0x8104, 0x812b };	/* RPM / 100 */

/* HP thermal policy numbers, as GTPS and STPS take them */
static const enum platform_profile_option policy_profile[] = {
	PLATFORM_PROFILE_PERFORMANCE,
	PLATFORM_PROFILE_BALANCED,
	PLATFORM_PROFILE_COOL,
	PLATFORM_PROFILE_QUIET,
};

/*
 * HP's EC reports faults by forcing the hidden thermal zone \_TZ.TSZ2 to its
 * _HOT temperature, which Windows answers with a shutdown and Linux ignores.
 * The flags behind it: FFAL/THSD/PDPN in CMOS, the rest named in TSZ2. Reading
 * TSZ2._TMP clears BOTP/TZTP/FFVO, so check the flags themselves, as soon as
 * the EC notifies the zone and every few seconds besides.
 */
static const struct {
	const char *path;
	u32 bit;
} emergency_flags[] = {
	{ "\\FFAL", BIT(0) },
	{ "\\THSD", BIT(1) },
	{ "\\_TZ.TSZ2.BOTP", BIT(1) },
	{ "\\PDPN", BIT(2) },
	{ "\\_TZ.TSZ2.TZTP", BIT(3) },
	{ "\\_TZ.TSZ2.FFVO", BIT(4) },
};

static const char * const emergency_names[] = {
	"fan-failure", "battery-overheat", "power-fault", "thermal-trip", "fan-fault",
};

#define EMERGENCY_INTERVAL	(5 * HZ)

#define CURVE_MAX_POINTS	8
#define CURVE_INTERVAL		HZ
#define CURVE_DECAY		8	/* falling temperature: 1/8 of the gap per tick */
#define CURVE_SMOOTH_MAX	30	/* longest spike the curve can be told to ignore, seconds */

/* Debug writes are limited to the EC's fan table block. */
#define XRAM_WRITE_MIN		0x8100
#define XRAM_WRITE_MAX		0x814f

static char *def_curve = "60:40 70:55 80:75 90:100";
module_param_named(curve, def_curve, charp, 0444);
MODULE_PARM_DESC(curve, "Fan curve, \"temp:percent ...\" in degrees C, at most 8 points");

static bool def_curve_enable = true;
module_param_named(curve_enable, def_curve_enable, bool, 0444);
MODULE_PARM_DESC(curve_enable, "Apply the fan curve from load (default: yes)");

static u8 def_smooth;
module_param_named(curve_smooth, def_smooth, byte, 0444);
MODULE_PARM_DESC(curve_smooth, "Seconds of temperature spike to ignore before speeding up (default: 0, react at once)");

static bool emergency_shutdown;
module_param(emergency_shutdown, bool, 0444);
MODULE_PARM_DESC(emergency_shutdown, "Shut down when HP's EC reports an emergency (default: log and notify only)");

static bool debug;
module_param(debug, bool, 0444);
MODULE_PARM_DESC(debug, "Add xram (EC RAM dump) and xram_set (fan table writes)");

struct envy_fan {
	struct device *dev;
	acpi_handle tz;
	acpi_handle ec;
	acpi_handle wmid;		/* HP thermal policy, NULL if absent */
	struct device *ppdev;		/* platform profile device, NULL if absent */
	struct mutex lock;		/* everything below */
	struct delayed_work work;
	bool curve_on;
	bool resume_curve;
	bool resume_policy;		/* put saved_policy back on resume */
	u32 saved_policy;		/* HP thermal policy as of suspend */
	bool dying;
	int npoints;
	int curve_temp[CURVE_MAX_POINTS];	/* degrees C, increasing */
	int curve_duty[CURVE_MAX_POINTS];	/* percent, non-decreasing */
	u8 floor;			/* curve drive level at eff_temp */
	long eff_temp;			/* millidegrees */
	u8 smooth;			/* ignore rises shorter than this many ticks */
	u8 hist_pos;			/* next slot in hist */
	bool hist_primed;
	long hist[CURVE_SMOOTH_MAX];	/* recent temperatures, millidegrees */
	u16 xram_base;
	u16 xram_len;
	char *eval_result;		/* debug acpi_eval output */
	struct device *hwmon;
	acpi_handle tz2;
	bool tz2_notify;
	u32 emergency;			/* EC fault bits seen since load */
	bool shutting_down;
	struct delayed_work emergency_work;
	/* originals of bytes written through xram_set, restored on exit */
	int xram_saved;
	u16 xram_saved_idx[32];
	u8 xram_saved_val[32];
};

static int envy_eval(acpi_handle handle, char *method, const u64 *args, int nargs,
		     unsigned long long *out)
{
	union acpi_object params[2] = {};
	struct acpi_object_list input = { .count = nargs, .pointer = params };
	unsigned long long val;
	int i;

	for (i = 0; i < nargs; i++) {
		params[i].type = ACPI_TYPE_INTEGER;
		params[i].integer.value = args[i];
	}

	if (ACPI_FAILURE(acpi_evaluate_integer(handle, method, nargs ? &input : NULL, &val)))
		return -EIO;
	if (out)
		*out = val;
	return 0;
}

static int envy_xram_read(struct envy_fan *fan, u16 idx, u8 *val)
{
	u64 args[] = { idx };
	unsigned long long v;
	int ret;

	ret = envy_eval(fan->ec, "FANG", args, 1, &v);
	if (ret)
		return ret;
	if (v > U8_MAX)
		return -EIO;
	*val = v;
	return 0;
}

static int envy_xram_write(struct envy_fan *fan, u16 idx, u8 val)
{
	u64 args[] = { idx, val };

	return envy_eval(fan->ec, "FANW", args, 2, NULL);
}

/* _TMP is 2732 + 10 * degrees C */
static int envy_read_temp(struct envy_fan *fan, long *mdeg)
{
	unsigned long long dk;
	int ret;

	ret = envy_eval(fan->tz, "_TMP", NULL, 0, &dk);
	if (ret)
		return ret;
	*mdeg = ((long)dk - 2732) * 100;
	return 0;
}

static int envy_read_rpm(struct envy_fan *fan, char *method, long *rpm)
{
	unsigned long long v;
	int ret;

	ret = envy_eval(fan->tz, method, NULL, 0, &v);
	if (ret)
		return ret;
	*rpm = v;
	return 0;
}

/* Curve drive level at @mdeg, percent rounded up; 0 below the first point. */
static u8 envy_curve_duty(struct envy_fan *fan, long mdeg)
{
	long t0, t1, duty;
	int i;

	if (!fan->npoints || mdeg < fan->curve_temp[0] * 1000L)
		return 0;

	for (i = 1; i < fan->npoints; i++)
		if (mdeg < fan->curve_temp[i] * 1000L)
			break;

	if (i == fan->npoints)
		return fan->curve_duty[i - 1];

	t0 = fan->curve_temp[i - 1] * 1000L;
	t1 = fan->curve_temp[i] * 1000L;
	duty = fan->curve_duty[i - 1] * (t1 - t0) +
	       (fan->curve_duty[i] - fan->curve_duty[i - 1]) * (mdeg - t0);
	return DIV_ROUND_UP(duty, t1 - t0);
}

static int envy_curve_tick_locked(struct envy_fan *fan)
{
	u8 duty;
	long t, rise;
	int i, ret;

	ret = envy_read_temp(fan, &t);
	if (ret)
		return ret;

	/*
	 * Speeding up follows the lowest temperature of the last fan->smooth
	 * seconds, so a spike shorter than that window never reaches the fans,
	 * while a real load is followed in full, just that many seconds late.
	 * smooth = 0 follows the reading itself. Slowing down is unchanged, and a
	 * spike holds eff_temp where it is rather than decaying it.
	 */
	if (!fan->hist_primed) {
		for (i = 0; i < CURVE_SMOOTH_MAX; i++)
			fan->hist[i] = t;
		fan->hist_pos = 0;
		fan->hist_primed = true;
	}
	fan->hist[fan->hist_pos] = t;
	rise = t;
	for (i = 0; i < fan->smooth; i++)
		rise = min(rise, fan->hist[(fan->hist_pos + CURVE_SMOOTH_MAX - i) % CURVE_SMOOTH_MAX]);
	fan->hist_pos = (fan->hist_pos + 1) % CURVE_SMOOTH_MAX;

	if (rise >= fan->eff_temp)
		fan->eff_temp = rise;
	else if (t < fan->eff_temp)
		fan->eff_temp -= DIV_ROUND_UP(fan->eff_temp - t, CURVE_DECAY);
	fan->floor = envy_curve_duty(fan, fan->eff_temp);

	for (i = 0; i < FAN_COUNT; i++) {
		ret = envy_xram_read(fan, fan_duty_idx[i], &duty);
		if (ret)
			return ret;
		if (duty < fan->floor) {
			ret = envy_xram_write(fan, fan_duty_idx[i], fan->floor);
			if (ret)
				return ret;
		}
	}
	return 0;
}

static void envy_curve_start_locked(struct envy_fan *fan)
{
	fan->eff_temp = 0;
	fan->hist_primed = false;
	fan->curve_on = true;
	mod_delayed_work(system_wq, &fan->work, 0);
}

/* The EC ramps the fans back down from wherever the curve left them. */
static void envy_curve_stop_locked(struct envy_fan *fan)
{
	fan->curve_on = false;
	fan->floor = 0;
	cancel_delayed_work(&fan->work);
}

static void envy_curve_work(struct work_struct *work)
{
	struct envy_fan *fan = container_of(to_delayed_work(work), struct envy_fan, work);
	int ret;

	guard(mutex)(&fan->lock);
	if (!fan->curve_on)
		return;

	ret = envy_curve_tick_locked(fan);
	if (ret) {
		dev_err(fan->dev, "EC access failed (%d), fan curve off\n", ret);
		envy_curve_stop_locked(fan);
		return;
	}
	schedule_delayed_work(&fan->work, CURVE_INTERVAL);
}

static int envy_parse_curve(const char *buf, int *temp, int *duty)
{
	char *s __free(kfree) = kstrdup(buf, GFP_KERNEL);
	char *p = s, *tok;
	int n = 0;

	if (!s)
		return -ENOMEM;

	while ((tok = strsep(&p, " \t\n,"))) {
		if (!*tok)
			continue;
		if (n == CURVE_MAX_POINTS)
			return -EINVAL;
		if (sscanf(tok, "%d:%d", &temp[n], &duty[n]) != 2 ||
		    temp[n] < 0 || temp[n] > 110 || duty[n] < 0 || duty[n] > DUTY_MAX ||
		    (n && (temp[n] <= temp[n - 1] || duty[n] < duty[n - 1])))
			return -EINVAL;
		n++;
	}
	return n ?: -EINVAL;
}

static umode_t envy_is_visible(const void *data, enum hwmon_sensor_types type,
			       u32 attr, int channel)
{
	return 0444;
}

static int envy_read(struct device *dev, enum hwmon_sensor_types type,
		     u32 attr, int channel, long *val)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	u8 v;
	int ret;

	switch (type) {
	case hwmon_temp:
		return envy_read_temp(fan, val);
	case hwmon_fan:
		scoped_guard(mutex, &fan->lock)
			ret = envy_xram_read(fan, fan_speed_idx[channel], &v);
		if (ret)
			return ret;
		*val = v * 100;
		return 0;
	case hwmon_pwm:
		scoped_guard(mutex, &fan->lock)
			ret = envy_xram_read(fan, fan_duty_idx[channel], &v);
		if (ret)
			return ret;
		*val = DIV_ROUND_CLOSEST(min(v, DUTY_MAX) * 255, DUTY_MAX);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

static int envy_read_string(struct device *dev, enum hwmon_sensor_types type,
			    u32 attr, int channel, const char **str)
{
	*str = "CPU";
	return 0;
}

static const struct hwmon_ops envy_ops = {
	.is_visible = envy_is_visible,
	.read = envy_read,
	.read_string = envy_read_string,
};

static const struct hwmon_channel_info * const envy_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL),
	HWMON_CHANNEL_INFO(fan, HWMON_F_INPUT, HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(pwm, HWMON_PWM_INPUT, HWMON_PWM_INPUT),
	NULL
};

static const struct hwmon_chip_info envy_chip_info = {
	.ops = &envy_ops,
	.info = envy_info,
};

static void envy_emergency_check(struct envy_fan *fan)
{
	unsigned long long v;
	u32 seen = 0, new;
	int i;

	for (i = 0; i < ARRAY_SIZE(emergency_flags); i++)
		if (!envy_eval(NULL, (char *)emergency_flags[i].path, NULL, 0, &v) && v)
			seen |= emergency_flags[i].bit;

	scoped_guard(mutex, &fan->lock) {
		new = seen & ~fan->emergency;
		fan->emergency |= seen;
	}
	if (!new)
		return;

	for (i = 0; i < ARRAY_SIZE(emergency_names); i++)
		if (new & BIT(i))
			dev_crit(fan->dev, "HP EC reports %s\n", emergency_names[i]);
	if (fan->hwmon)
		sysfs_notify(&fan->hwmon->kobj, NULL, "emergency");

	if (emergency_shutdown && !fan->shutting_down) {
		fan->shutting_down = true;
		dev_emerg(fan->dev, "shutting down on HP EC emergency\n");
		orderly_poweroff(true);
	}
}

static void envy_emergency_work(struct work_struct *work)
{
	struct envy_fan *fan = container_of(to_delayed_work(work), struct envy_fan,
					    emergency_work);

	envy_emergency_check(fan);
	schedule_delayed_work(&fan->emergency_work, EMERGENCY_INTERVAL);
}

static void envy_tz2_notify(acpi_handle handle, u32 event, void *data)
{
	envy_emergency_check(data);
}

static ssize_t emergency_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	int i, len = 0;
	u32 e;

	scoped_guard(mutex, &fan->lock)
		e = fan->emergency;
	if (!e)
		return sysfs_emit(buf, "none\n");
	for (i = 0; i < ARRAY_SIZE(emergency_names); i++)
		if (e & BIT(i))
			len += sysfs_emit_at(buf, len, "%s%s", len ? " " : "", emergency_names[i]);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}
static DEVICE_ATTR_RO(emergency);

/* Battery details the EC keeps and ACPI's _BIX leaves out. */
static int envy_ec_field(struct envy_fan *fan, char *name, unsigned long long *v)
{
	return envy_eval(fan->ec, name, NULL, 0, v);
}

static ssize_t battery_cycles_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned long long lo, hi;

	if (envy_ec_field(fan, "CYC0", &lo) || envy_ec_field(fan, "CYC1", &hi))
		return -EIO;
	return sysfs_emit(buf, "%llu\n", (hi & 0xff) << 8 | (lo & 0xff));
}
static DEVICE_ATTR_RO(battery_cycles);

/* Cell voltages in mV; 0 means no cell in that position. */
static ssize_t battery_cells_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static char * const cells[] = { "BCV1", "BCV2", "BCV3", "BCV4" };
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned long long mv;
	int i, len = 0;

	for (i = 0; i < ARRAY_SIZE(cells); i++) {
		if (envy_ec_field(fan, cells[i], &mv))
			return -EIO;
		if (mv)
			len += sysfs_emit_at(buf, len, "%s%llu", len ? " " : "", mv);
	}
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}
static DEVICE_ATTR_RO(battery_cells);

/* Smart Battery date format: (year - 1980) << 9 | month << 5 | day */
static ssize_t battery_manufactured_show(struct device *dev, struct device_attribute *attr,
					 char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned long long d;

	if (envy_ec_field(fan, "BMD0", &d) || !d)
		return -EIO;
	return sysfs_emit(buf, "%04llu-%02llu-%02llu\n", 1980 + (d >> 9), (d >> 5) & 0xf, d & 0x1f);
}
static DEVICE_ATTR_RO(battery_manufactured);

/* Full-charge capacity as a percentage of design capacity. */
static ssize_t battery_health_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned long long full, design;

	if (envy_ec_field(fan, "BFC0", &full) || envy_ec_field(fan, "BDC0", &design) || !design)
		return -EIO;
	return sysfs_emit(buf, "%llu\n", DIV_ROUND_CLOSEST(full * 100, design));
}
static DEVICE_ATTR_RO(battery_health);

static ssize_t curve_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	int i, len = 0;

	guard(mutex)(&fan->lock);
	for (i = 0; i < fan->npoints; i++)
		len += sysfs_emit_at(buf, len, "%s%d:%d", i ? " " : "",
				     fan->curve_temp[i], fan->curve_duty[i]);
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t curve_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	int temp[CURVE_MAX_POINTS], duty[CURVE_MAX_POINTS];
	int n;

	n = envy_parse_curve(buf, temp, duty);
	if (n < 0)
		return n;

	guard(mutex)(&fan->lock);
	memcpy(fan->curve_temp, temp, sizeof(temp));
	memcpy(fan->curve_duty, duty, sizeof(duty));
	fan->npoints = n;
	return count;
}
static DEVICE_ATTR_RW(curve);

static ssize_t curve_enable_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);

	guard(mutex)(&fan->lock);
	return sysfs_emit(buf, "%d\n", fan->curve_on);
}

static ssize_t curve_enable_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	bool on;
	int ret;

	ret = kstrtobool(buf, &on);
	if (ret)
		return ret;

	guard(mutex)(&fan->lock);
	if (fan->dying)
		return -ENODEV;
	if (on && !fan->curve_on)
		envy_curve_start_locked(fan);
	else if (!on)
		envy_curve_stop_locked(fan);
	return count;
}
static DEVICE_ATTR_RW(curve_enable);

/* The curve's drive level right now, percent; 0 when off or below the first point. */
static ssize_t curve_floor_show(struct device *dev, struct device_attribute *attr,
				char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);

	guard(mutex)(&fan->lock);
	return sysfs_emit(buf, "%d\n", fan->curve_on ? fan->floor : 0);
}
static DEVICE_ATTR_RO(curve_floor);

/*
 * Seconds of temperature spike to sit through before speeding the fans up, so
 * that opening something does not send them to full for a moment. 0 reacts at
 * once. Slowing down is unaffected.
 */
static ssize_t curve_smooth_show(struct device *dev, struct device_attribute *attr,
				 char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);

	guard(mutex)(&fan->lock);
	return sysfs_emit(buf, "%u\n", fan->smooth);
}

static ssize_t curve_smooth_store(struct device *dev, struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	u8 secs;
	int ret;

	ret = kstrtou8(buf, 10, &secs);
	if (ret)
		return ret;
	if (secs > CURVE_SMOOTH_MAX)
		return -EINVAL;

	guard(mutex)(&fan->lock);
	fan->smooth = secs;
	return count;
}
static DEVICE_ATTR_RW(curve_smooth);

static ssize_t xram_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	int i, len = 0;
	u8 v;

	guard(mutex)(&fan->lock);
	for (i = 0; i < fan->xram_len; i++) {
		if (i % 16 == 0)
			len += sysfs_emit_at(buf, len, "%04x:", fan->xram_base + i);
		if (envy_xram_read(fan, fan->xram_base + i, &v))
			len += sysfs_emit_at(buf, len, " ??");
		else
			len += sysfs_emit_at(buf, len, " %02x", v);
		if (i % 16 == 15 || i == fan->xram_len - 1)
			len += sysfs_emit_at(buf, len, "\n");
	}
	return len;
}

/* "<base> <len>", len up to 256 */
static ssize_t xram_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned int base, len;

	if (sscanf(buf, "%i %i", &base, &len) != 2 || base > U16_MAX || !len || len > 256 ||
	    base + len > U16_MAX + 1)
		return -EINVAL;

	guard(mutex)(&fan->lock);
	fan->xram_base = base;
	fan->xram_len = len;
	return count;
}
static DEVICE_ATTR_RW(xram);

/* "<idx> <val>"; the first write to a byte records its original value */
static ssize_t xram_set_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned int idx, val;
	u8 orig;
	int i, ret;

	if (sscanf(buf, "%i %i", &idx, &val) != 2 || idx < XRAM_WRITE_MIN ||
	    idx > XRAM_WRITE_MAX || val > U8_MAX)
		return -EINVAL;

	guard(mutex)(&fan->lock);
	for (i = 0; i < fan->xram_saved; i++)
		if (fan->xram_saved_idx[i] == idx)
			break;
	if (i == fan->xram_saved) {
		if (i == ARRAY_SIZE(fan->xram_saved_idx))
			return -ENOSPC;
		ret = envy_xram_read(fan, idx, &orig);
		if (ret)
			return ret;
		fan->xram_saved_idx[i] = idx;
		fan->xram_saved_val[i] = orig;
		fan->xram_saved++;
	}

	ret = envy_xram_write(fan, idx, val);
	if (ret)
		return ret;

	dev_info(fan->dev, "xram 0x%04x set to 0x%02x (was 0x%02x)\n", idx, val,
		 fan->xram_saved_val[i]);
	return count;
}
static DEVICE_ATTR_WO(xram_set);

/*
 * HP's thermal policy. The WMI write path (WRCF case 0x4c) hands STPS the whole
 * WMI buffer instead of its data word, so SMM always rejects it; STPS itself
 * works. Both go through an SMM mailbox that WMAA guards with \\MSMT, so take
 * that too. HWRC is the mailbox's return code.
 */
static int envy_policy_call(struct envy_fan *fan, int set, u32 *policy)
{
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object arg = { .integer = { .type = ACPI_TYPE_INTEGER, .value = *policy } };
	struct acpi_object_list input = { .count = 1, .pointer = &arg };
	union acpi_object *obj, *buf;
	unsigned long long hwrc;
	acpi_status status;
	int ret = -EIO;

	if (ACPI_FAILURE(acpi_acquire_mutex(NULL, "\\MSMT", 1000)))
		return -EBUSY;

	if (set)
		status = acpi_evaluate_object(fan->wmid, "STPS", &input, &out);
	else
		status = acpi_evaluate_object(fan->wmid, "GTPS", NULL, &out);
	obj = out.pointer;

	if (ACPI_SUCCESS(status) && !envy_eval(fan->wmid, "HWRC", NULL, 0, &hwrc)) {
		if (hwrc) {
			ret = -EIO;
		} else if (set) {
			ret = 0;
		} else if (obj && obj->type == ACPI_TYPE_PACKAGE && obj->package.count == 3) {
			/*
			 * SMM writes only the first byte; the rest of the mailbox
			 * still holds whatever the previous call left there.
			 */
			buf = &obj->package.elements[2];
			if (buf->type == ACPI_TYPE_BUFFER && buf->buffer.length >= 1) {
				*policy = buf->buffer.pointer[0];
				ret = *policy < ARRAY_SIZE(policy_profile) ? 0 : -EINVAL;
			}
		}
	}

	acpi_release_mutex(NULL, "\\MSMT");
	kfree(obj);
	return ret;
}

static int envy_profile_probe(void *drvdata, unsigned long *choices)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(policy_profile); i++)
		set_bit(policy_profile[i], choices);
	return 0;
}

static int envy_profile_get(struct device *dev, enum platform_profile_option *profile)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	u32 policy = 0;
	int ret;

	guard(mutex)(&fan->lock);
	ret = envy_policy_call(fan, 0, &policy);
	if (ret)
		return ret;
	*profile = policy_profile[policy];
	return 0;
}

static int envy_profile_set(struct device *dev, enum platform_profile_option profile)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	u32 policy;

	for (policy = 0; policy < ARRAY_SIZE(policy_profile); policy++) {
		if (policy_profile[policy] == profile) {
			guard(mutex)(&fan->lock);
			return envy_policy_call(fan, 1, &policy);
		}
	}
	return -EOPNOTSUPP;
}

static const struct platform_profile_ops envy_profile_ops = {
	.probe = envy_profile_probe,
	.profile_get = envy_profile_get,
	.profile_set = envy_profile_set,
};

/* Debug: the raw policy number and the mailbox's last return code. */
static ssize_t tps_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned long long hwrc;
	u32 policy = 0;
	int ret;

	guard(mutex)(&fan->lock);
	ret = envy_policy_call(fan, 0, &policy);
	if (envy_eval(fan->wmid, "HWRC", NULL, 0, &hwrc))
		return -EIO;
	return sysfs_emit(buf, "policy=%u ret=%d hwrc=%llu\n", policy, ret, hwrc);
}

static ssize_t tps_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	u32 policy;
	int ret;

	ret = kstrtou32(buf, 0, &policy);
	if (ret)
		return ret;
	if (policy >= ARRAY_SIZE(policy_profile))
		return -EINVAL;

	guard(mutex)(&fan->lock);
	ret = envy_policy_call(fan, 1, &policy);
	dev_info(fan->dev, "thermal policy %u: %d\n", policy, ret);
	return ret ?: count;
}
static DEVICE_ATTR_RW(tps);

/* Power limits and fan cap the EC currently hands to the SMU and uses itself. */
static ssize_t ec_limits_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	static char * const names[] = { "MINL", "FPPT", "SPPT", "SAPU", "FNMX" };
	struct envy_fan *fan = dev_get_drvdata(dev);
	unsigned long long v;
	int i, len = 0;

	guard(mutex)(&fan->lock);
	for (i = 0; i < ARRAY_SIZE(names); i++) {
		if (envy_eval(fan->ec, names[i], NULL, 0, &v))
			len += sysfs_emit_at(buf, len, "%s=? ", names[i]);
		else
			len += sysfs_emit_at(buf, len, "%s=%llu ", names[i], v);
	}
	buf[len - 1] = '\n';
	return len;
}
static DEVICE_ATTR_RO(ec_limits);

static int envy_fmt_obj(char *buf, int len, int size, const union acpi_object *o, int depth)
{
	int i;

	switch (o->type) {
	case ACPI_TYPE_INTEGER:
		return len + scnprintf(buf + len, size - len, "0x%llx ", o->integer.value);
	case ACPI_TYPE_STRING:
		return len + scnprintf(buf + len, size - len, "\"%.*s\" ", o->string.length, o->string.pointer);
	case ACPI_TYPE_BUFFER:
		len += scnprintf(buf + len, size - len, "buf[%u]:", o->buffer.length);
		for (i = 0; i < o->buffer.length && i < 128; i++)
			len += scnprintf(buf + len, size - len, "%02x", o->buffer.pointer[i]);
		return len + scnprintf(buf + len, size - len, " ");
	case ACPI_TYPE_PACKAGE:
		if (depth > 3)
			return len + scnprintf(buf + len, size - len, "pkg ");
		len += scnprintf(buf + len, size - len, "{ ");
		for (i = 0; i < o->package.count; i++)
			len = envy_fmt_obj(buf, len, size, &o->package.elements[i], depth + 1);
		return len + scnprintf(buf + len, size - len, "} ");
	default:
		return len + scnprintf(buf + len, size - len, "type%u ", o->type);
	}
}

/*
 * Debug: "<path> [int ...]" evaluates an ACPI object with up to three integer
 * arguments; reading shows the result. For HP's getters and EC registers.
 */
static ssize_t acpi_eval_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct envy_fan *fan = dev_get_drvdata(dev);

	guard(mutex)(&fan->lock);
	return sysfs_emit(buf, "%s\n", fan->eval_result ?: "");
}

static ssize_t acpi_eval_store(struct device *dev, struct device_attribute *attr,
			       const char *buf, size_t count)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	struct acpi_buffer out = { ACPI_ALLOCATE_BUFFER, NULL };
	union acpi_object args[3] = {};
	struct acpi_object_list input = { .pointer = args };
	char path[64];
	unsigned long long v[3];
	acpi_status status;
	bool wmid;
	char *res;
	int n, len;

	n = sscanf(buf, "%63s %lli %lli %lli", path, &v[0], &v[1], &v[2]);
	if (n < 1)
		return -EINVAL;
	for (input.count = 0; input.count < n - 1; input.count++) {
		args[input.count].type = ACPI_TYPE_INTEGER;
		args[input.count].integer.value = v[input.count];
	}

	res = kzalloc(PAGE_SIZE, GFP_KERNEL);
	if (!res)
		return -ENOMEM;

	guard(mutex)(&fan->lock);
	/* HP's WMI methods share an SMM mailbox with WMAA, which holds \\MSMT. */
	wmid = !strncmp(path, "\\_SB.WMID.", 10);
	if (wmid && ACPI_FAILURE(acpi_acquire_mutex(NULL, "\\MSMT", 1000))) {
		kfree(res);
		return -EBUSY;
	}
	status = acpi_evaluate_object(NULL, path, input.count ? &input : NULL, &out);
	if (wmid)
		acpi_release_mutex(NULL, "\\MSMT");

	if (ACPI_FAILURE(status))
		len = scnprintf(res, PAGE_SIZE - 1, "%s: %s", path, acpi_format_exception(status));
	else if (!out.pointer)
		len = scnprintf(res, PAGE_SIZE - 1, "%s: (no value)", path);
	else
		len = envy_fmt_obj(res, scnprintf(res, PAGE_SIZE - 1, "%s: ", path), PAGE_SIZE - 1,
				   out.pointer, 0);
	res[len] = 0;
	kfree(out.pointer);
	kfree(fan->eval_result);
	fan->eval_result = res;
	return count;
}
static DEVICE_ATTR(acpi_eval, 0600, acpi_eval_show, acpi_eval_store);

static void envy_xram_restore_locked(struct envy_fan *fan)
{
	int i;

	for (i = fan->xram_saved - 1; i >= 0; i--)
		if (envy_xram_write(fan, fan->xram_saved_idx[i], fan->xram_saved_val[i]))
			dev_err(fan->dev, "failed to restore xram 0x%04x\n",
				fan->xram_saved_idx[i]);
	fan->xram_saved = 0;
}

static struct attribute *envy_attrs[] = {
	&dev_attr_curve.attr,
	&dev_attr_curve_enable.attr,
	&dev_attr_curve_floor.attr,
	&dev_attr_curve_smooth.attr,
	&dev_attr_emergency.attr,
	&dev_attr_battery_cycles.attr,
	&dev_attr_battery_cells.attr,
	&dev_attr_battery_manufactured.attr,
	&dev_attr_battery_health.attr,
	&dev_attr_xram.attr,
	&dev_attr_xram_set.attr,
	&dev_attr_tps.attr,
	&dev_attr_ec_limits.attr,
	&dev_attr_acpi_eval.attr,
	NULL
};

static umode_t envy_attr_visible(struct kobject *kobj, struct attribute *attr, int n)
{
	struct envy_fan *fan = dev_get_drvdata(kobj_to_dev(kobj));

	if ((attr == &dev_attr_xram.attr || attr == &dev_attr_xram_set.attr ||
	     attr == &dev_attr_ec_limits.attr || attr == &dev_attr_acpi_eval.attr) && !debug)
		return 0;
	if (attr == &dev_attr_tps.attr && (!debug || !fan->wmid))
		return 0;
	return attr->mode;
}

static const struct attribute_group envy_group = {
	.attrs = envy_attrs,
	.is_visible = envy_attr_visible,
};
__ATTRIBUTE_GROUPS(envy);

static void envy_release(struct envy_fan *fan, bool dying, bool for_resume)
{
	if (dying) {
		cancel_delayed_work_sync(&fan->emergency_work);
		if (fan->tz2_notify) {
			acpi_remove_notify_handler(fan->tz2, ACPI_DEVICE_NOTIFY, envy_tz2_notify);
			fan->tz2_notify = false;
		}
	}
	scoped_guard(mutex, &fan->lock) {
		fan->dying = dying;
		fan->resume_curve = for_resume && fan->curve_on;
		envy_curve_stop_locked(fan);
		envy_xram_restore_locked(fan);
	}
	cancel_delayed_work_sync(&fan->work);
}

static int __init envy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct envy_fan *fan;
	struct device *hwmon;
	u8 duty[FAN_COUNT], speed;
	long rpm;
	int i, ret;

	fan = devm_kzalloc(dev, sizeof(*fan), GFP_KERNEL);
	if (!fan)
		return -ENOMEM;
	fan->dev = dev;
	fan->xram_base = 0x8100;
	fan->xram_len = 0x50;

	if (ACPI_FAILURE(acpi_get_handle(NULL, TZ_PATH, &fan->tz)) ||
	    ACPI_FAILURE(acpi_get_handle(NULL, EC_PATH, &fan->ec)) ||
	    !acpi_has_method(fan->tz, "FRSP") ||
	    !acpi_has_method(fan->tz, "_TMP") || !acpi_has_method(fan->ec, "FANG") ||
	    !acpi_has_method(fan->ec, "FANW"))
		return dev_err_probe(dev, -ENODEV, "EC fan methods not found\n");

	if (ACPI_FAILURE(acpi_get_handle(NULL, WMID_PATH, &fan->wmid)) ||
	    !acpi_has_method(fan->wmid, "STPS") || !acpi_has_method(fan->wmid, "GTPS"))
		fan->wmid = NULL;

	ret = devm_mutex_init(dev, &fan->lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&fan->work, envy_curve_work);

	/* Check the EC RAM layout against the DSDT's own fan reading before writing to it. */
	ret = envy_read_rpm(fan, "FRSP", &rpm);
	if (!ret)
		ret = envy_xram_read(fan, fan_speed_idx[0], &speed);
	for (i = 0; !ret && i < FAN_COUNT; i++)
		ret = envy_xram_read(fan, fan_duty_idx[i], &duty[i]);
	if (ret)
		return dev_err_probe(dev, ret, "cannot read fan state\n");
	if (abs(rpm - speed * 100) > 1000 || duty[0] > DUTY_MAX || duty[1] > DUTY_MAX)
		return dev_err_probe(dev, -ENODEV,
				     "unexpected EC fan layout (rpm %ld, speed %u, duty %u/%u)\n",
				     rpm, speed, duty[0], duty[1]);

	ret = envy_parse_curve(def_curve, fan->curve_temp, fan->curve_duty);
	if (ret < 0)
		dev_warn(dev, "invalid curve \"%s\", fan left to the EC\n", def_curve);
	else
		fan->npoints = ret;

	fan->smooth = min_t(u8, def_smooth, CURVE_SMOOTH_MAX);

	platform_set_drvdata(pdev, fan);
	hwmon = devm_hwmon_device_register_with_info(dev, DRVNAME, fan, &envy_chip_info,
						     envy_groups);
	if (IS_ERR(hwmon))
		return PTR_ERR(hwmon);
	fan->hwmon = hwmon;

	INIT_DELAYED_WORK(&fan->emergency_work, envy_emergency_work);
	if (ACPI_SUCCESS(acpi_get_handle(NULL, "\\_TZ.TSZ2", &fan->tz2)) &&
	    ACPI_SUCCESS(acpi_install_notify_handler(fan->tz2, ACPI_DEVICE_NOTIFY,
						     envy_tz2_notify, fan)))
		fan->tz2_notify = true;
	else
		dev_warn(dev, "EC emergencies will only be polled\n");
	schedule_delayed_work(&fan->emergency_work, 0);

	if (fan->wmid) {
		struct device *pp = devm_platform_profile_register(dev, "hp-envy-fan", fan,
								   &envy_profile_ops);
		if (IS_ERR(pp))
			dev_warn(dev, "no platform profile: %ld\n", PTR_ERR(pp));
		else
			fan->ppdev = pp;
	}

	if (def_curve_enable && fan->npoints) {
		guard(mutex)(&fan->lock);
		envy_curve_start_locked(fan);
	}
	return 0;
}

static void envy_remove(struct platform_device *pdev)
{
	struct envy_fan *fan = platform_get_drvdata(pdev);

	envy_release(fan, true, false);
	kfree(fan->eval_result);
	fan->eval_result = NULL;
}

static int envy_suspend(struct device *dev)
{
	struct envy_fan *fan = dev_get_drvdata(dev);

	scoped_guard(mutex, &fan->lock)
		fan->resume_policy = fan->wmid &&
				     !envy_policy_call(fan, 0, &fan->saved_policy);
	envy_release(fan, false, true);
	return 0;
}

/*
 * The EC comes back on HP's default policy, so put the thermal mode back where
 * it was. Userspace caches the platform profile (power-profiles-daemon reads it
 * once at startup), so tell it to look again whatever the outcome: after a
 * successful restore its value is right again, and after a failed one it at
 * least stops advertising a mode the laptop is not in.
 */
static int envy_resume(struct device *dev)
{
	struct envy_fan *fan = dev_get_drvdata(dev);
	bool moved = false;
	u32 policy;

	scoped_guard(mutex, &fan->lock) {
		if (fan->resume_curve)
			envy_curve_start_locked(fan);

		if (fan->resume_policy &&
		    (envy_policy_call(fan, 0, &policy) || policy != fan->saved_policy)) {
			policy = fan->saved_policy;
			if (envy_policy_call(fan, 1, &policy))
				dev_warn(fan->dev, "could not restore thermal policy %u\n",
					 fan->saved_policy);
			moved = true;
		}
	}

	if (moved && fan->ppdev)
		platform_profile_notify(fan->ppdev);
	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(envy_pm, envy_suspend, envy_resume);

static struct platform_driver envy_driver = {
	.driver = {
		.name = DRVNAME,
		.pm = pm_sleep_ptr(&envy_pm),
	},
	.remove = envy_remove,
	.shutdown = envy_remove,
};

static const struct dmi_system_id envy_dmi_table[] __initconst = {
	{
		.ident = "HP ENVY x360 Convertible 15 (AMD)",
		.matches = {
			DMI_MATCH(DMI_BOARD_VENDOR, "HP"),
			DMI_EXACT_MATCH(DMI_BOARD_NAME, "876F"),
		},
	},
	{}
};
MODULE_DEVICE_TABLE(dmi, envy_dmi_table);

static struct platform_device *envy_pdev;

static int __init envy_init(void)
{
	if (!dmi_check_system(envy_dmi_table))
		return -ENODEV;

	envy_pdev = platform_create_bundle(&envy_driver, envy_probe, NULL, 0, NULL, 0);
	return PTR_ERR_OR_ZERO(envy_pdev);
}

static void __exit envy_exit(void)
{
	platform_device_unregister(envy_pdev);
	platform_driver_unregister(&envy_driver);
}

module_init(envy_init);
module_exit(envy_exit);

MODULE_DESCRIPTION("HP ENVY x360 15 (board 876F) EC fan curve driver");
MODULE_LICENSE("GPL");
