/* Copyright (c) 2012-2015, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/pm_runtime.h>
#include <linux/ratelimit.h>
#include <linux/interrupt.h>
#include <linux/ioport.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_gpio.h>
#include <linux/list.h>
#include <linux/uaccess.h>
#include <linux/usb/ch9.h>
#include <linux/usb/gadget.h>
#include <linux/usb/of.h>
#include <linux/usb/msm_hsusb.h>
#include <linux/regulator/consumer.h>
#include <linux/pm_wakeup.h>
#include <linux/power_supply.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/clk/msm-clk.h>
#include <linux/msm-bus.h>
#include <linux/irq.h>

#include "power.h"
#include "core.h"
#include "gadget.h"
#include "dbm.h"
#include "debug.h"
#include "xhci.h"
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
#include <soc/qcom/lge/board_lge.h>
#include <soc/qcom/lge/lge_cable_detection.h>
#include <linux/qpnp/qpnp-adc.h>
#define DWC3_USB30_CHG_CURRENT 900
#define DEFAULT_DCP_CHG_MAX    1800
#endif

#ifdef CONFIG_LGE_PM
#define DWC3_IDEV_CHG_MAX  1800
#define DWC3_HVDCP_CHG_MAX 2000
#else
#define DWC3_IDEV_CHG_MAX 1500
#define DWC3_HVDCP_CHG_MAX 1800
#endif

/* time out to wait for USB cable status notification (in ms)*/
#define SM_INIT_TIMEOUT 30000

/* AHB2PHY register offsets */
#define PERIPH_SS_AHB2PHY_TOP_CFG 0x10

/* AHB2PHY read/write waite value */
#define ONE_READ_WRITE_WAIT 0x11

/* cpu to fix usb interrupt */
static int cpu_to_affin;
module_param(cpu_to_affin, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(cpu_to_affin, "affin usb irq to this cpu");

static int override_phy_init;
module_param(override_phy_init, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(override_phy_init, "Override HSPHY Init Seq");

/* Max current to be drawn for HVDCP charger */
static int hvdcp_max_current = DWC3_HVDCP_CHG_MAX;
module_param(hvdcp_max_current, int, S_IRUGO|S_IWUSR);
MODULE_PARM_DESC(hvdcp_max_current, "max current drawn for HVDCP charger");

/* Max current to be drawn for DCP charger */
int dcp_max_current = DWC3_IDEV_CHG_MAX;
module_param(dcp_max_current, int, S_IRUGO | S_IWUSR);
MODULE_PARM_DESC(dcp_max_current, "max current drawn for DCP charger");

/* XHCI registers */
#define USB3_HCSPARAMS1		(0x4)
#define USB3_PORTSC		(0x420)

/**
 *  USB QSCRATCH Hardware registers
 *
 */
#define QSCRATCH_REG_OFFSET	(0x000F8800)
#define QSCRATCH_GENERAL_CFG	(QSCRATCH_REG_OFFSET + 0x08)
#define CGCTL_REG		(QSCRATCH_REG_OFFSET + 0x28)
#define PWR_EVNT_IRQ_STAT_REG    (QSCRATCH_REG_OFFSET + 0x58)
#define PWR_EVNT_IRQ_MASK_REG    (QSCRATCH_REG_OFFSET + 0x5C)

#define PWR_EVNT_POWERDOWN_IN_P3_MASK		BIT(2)
#define PWR_EVNT_POWERDOWN_OUT_P3_MASK		BIT(3)
#define PWR_EVNT_LPM_IN_L2_MASK			BIT(4)
#define PWR_EVNT_LPM_OUT_L2_MASK		BIT(5)
#define PWR_EVNT_LPM_OUT_L1_MASK		BIT(13)

/* QSCRATCH_GENERAL_CFG register bit offset */
#define PIPE_UTMI_CLK_SEL	BIT(0)
#define PIPE3_PHYSTATUS_SW	BIT(3)
#define PIPE_UTMI_CLK_DIS	BIT(8)

struct dwc3_msm_req_complete {
	struct list_head list_item;
	struct usb_request *req;
	void (*orig_complete)(struct usb_ep *ep,
			      struct usb_request *req);
};

enum dwc3_id_state {
	DWC3_ID_GROUND = 0,
	DWC3_ID_FLOAT,
};

/* Input bits to state machine (mdwc->inputs) */

#define ID			0
#define B_SESS_VLD		1
#define B_SUSPEND		2

/*
 * USB chargers
 *
 * DWC3_INVALID_CHARGER		Invalid USB charger.
 * DWC3_SDP_CHARGER		Standard downstream port. Refers to a
 *				downstream port on USB compliant host/hub.
 * DWC3_DCP_CHARGER		Dedicated charger port(AC charger/ Wall charger)
 * DWC3_CDP_CHARGER		Charging downstream port. Enumeration can happen
 *				and IDEV_CHG_MAX can be drawn irrespective of
 *				USB state.
 * DWC3_PROPRIETARY_CHARGER	A non-standard charger that pulls DP/DM to
 *				specific voltages between 2.0-3.3v for
 *				identification.
 * DWC3_FLOATED_CHARGER		Non standard charger whose data lines are
 *				floating.
 */
enum dwc3_chg_type {
	DWC3_INVALID_CHARGER = 0,
	DWC3_SDP_CHARGER,
	DWC3_DCP_CHARGER,
	DWC3_CDP_CHARGER,
	DWC3_PROPRIETARY_CHARGER,
	DWC3_FLOATED_CHARGER,
};

#ifdef CONFIG_LGE_USB_MAXIM_EVP
enum hvdcp_chg_type {
	HVDCP_NONE = 0,
	HVDCP_LEGACY_DCP,
	HVDCP_EVP_DYNAMIC,
	HVDCP_EVP_SIMPLE,
	HVDCP_QC20,
	INVALID_HVDCP,
};
#endif

struct dwc3_msm {
	struct device *dev;
	void __iomem *base;
	void __iomem *ahb2phy_base;
	struct platform_device	*dwc3;
	const struct usb_ep_ops *original_ep_ops[DWC3_ENDPOINTS_NUM];
	struct list_head req_complete_list;
	struct clk		*xo_clk;
	struct clk		*core_clk;
	long			core_clk_rate;
	struct clk		*iface_clk;
	struct clk		*sleep_clk;
	struct clk		*utmi_clk;
	unsigned int		utmi_clk_rate;
	struct clk		*utmi_clk_src;
	struct clk		*bus_aggr_clk;
	struct clk		*cfg_ahb_clk;
	struct regulator	*dwc3_gdsc;

	struct usb_phy		*hs_phy, *ss_phy;

	struct dbm		*dbm;

	/* VBUS regulator for host mode */
#ifndef CONFIG_LGE_USB_TYPE_C
	struct regulator	*vbus_reg;
#endif
	int			vbus_retry_count;
	bool			resume_pending;
	atomic_t                pm_suspended;
	int			hs_phy_irq;
	struct delayed_work	resume_work;
	struct work_struct	restart_usb_work;
	bool			in_restart;
	struct workqueue_struct *dwc3_wq;
	struct delayed_work	sm_work;
	unsigned long		inputs;
	struct completion	dwc3_xcvr_vbus_init;
	enum dwc3_chg_type	chg_type;
	unsigned		max_power;
	bool			charging_disabled;
	enum usb_otg_state	otg_state;
	enum usb_chg_state	chg_state;
	int			pmic_id_irq;
	struct work_struct	id_work;
	u8			dcd_retries;
	struct work_struct	bus_vote_w;
	unsigned int		bus_vote;
	u32			bus_perf_client;
	struct msm_bus_scale_pdata	*bus_scale_table;
	struct power_supply	usb_psy;
	struct power_supply	*ext_vbus_psy;
	unsigned int		online;
	bool			in_host_mode;
	unsigned int		voltage_max;
	unsigned int		current_max;
	unsigned int		health_status;
	unsigned int		tx_fifo_size;
	bool			vbus_active;
#ifdef CONFIG_LGE_USB_TYPE_C
	bool			vbus_active_pending;
	unsigned int		dp_dm;
#ifdef CONFIG_LGE_ALICE_FRIENDS
	int			alice_friends;
#endif
#endif
	bool			suspend;
	bool			ext_inuse;
	bool			disable_host_mode_pm;
	enum dwc3_id_state	id_state;
	unsigned long		lpm_flags;
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	struct delayed_work     cable_adc_work;
	struct qpnp_vadc_chip   *vadc_id_dev;
	struct qpnp_vadc_chip	*vadc_dev;
	void (*read_cable_adc)  (struct dwc3_msm *mdwc, bool start);
	bool                    adc_read_complete;

#endif
#define MDWC3_SS_PHY_SUSPEND		BIT(0)
#define MDWC3_ASYNC_IRQ_WAKE_CAPABILITY	BIT(1)
#define MDWC3_POWER_COLLAPSE		BIT(2)

	bool power_collapse; /* power collapse on cable disconnect */
	bool power_collapse_por; /* perform POR sequence after power collapse */

#ifdef CONFIG_LGE_PM
	bool xo_vote_for_charger;
#endif

	unsigned int		irq_to_affin;
	struct notifier_block	dwc3_cpu_notifier;

	int  pwr_event_irq;
	atomic_t                in_p3;
	unsigned int		lpm_to_suspend_delay;
	bool			init;
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	bool				evp_detect;
	bool				pwr_event_irq_disabled;
	enum hvdcp_chg_type	evp_sts;
	struct delayed_work	evp_connect_work;
	atomic_t			evp_detecting;
#endif
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
	struct hrtimer		floated_chg_hrtimer;
	struct delayed_work	floated_chg_work;
	bool			apsd_rerun_need;
	bool			after_apsd_rerun;
#endif
};

#define USB_HSPHY_3P3_VOL_MIN		3050000 /* uV */
#define USB_HSPHY_3P3_VOL_MAX		3300000 /* uV */
#define USB_HSPHY_3P3_HPM_LOAD		16000	/* uA */

#define USB_HSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_HSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_HSPHY_1P8_HPM_LOAD		19000	/* uA */

#define USB_SSPHY_1P8_VOL_MIN		1800000 /* uV */
#define USB_SSPHY_1P8_VOL_MAX		1800000 /* uV */
#define USB_SSPHY_1P8_HPM_LOAD		23000	/* uA */

#define DSTS_CONNECTSPD_SS		0x4


static struct usb_ext_notification *usb_ext;

static void dwc3_pwr_event_handler(struct dwc3_msm *mdwc);
static int dwc3_msm_gadget_vbus_draw(struct dwc3_msm *mdwc, unsigned mA);

#if defined (CONFIG_LGE_TOUCH_CORE)
void touch_notify_connect(int value);
#endif

#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
#define FLOATED_CHG_CHECK_DELAY (msecs_to_jiffies(1000))
#define FLOATED_CHG_CURRENT_MAX        1500 * 1000
#define FLOATED_CHG_DEFAULT_CURRENT    500 * 1000
static bool apsd_retried = false;

static void dwc3_floated_chg_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, floated_chg_work.work);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	if (lge_get_boot_mode() == LGE_BOOT_MODE_CHARGERLOGO) {
		pr_info("%s(): floated chg detected\n", __func__);
		mdwc->usb_psy.is_floated_charger = 1;
		power_supply_set_current_limit(&mdwc->usb_psy,
				FLOATED_CHG_DEFAULT_CURRENT);
		power_supply_changed(&mdwc->usb_psy);
	} else {
		pr_info("%s(): retry apsd\n", __func__);
		pr_info("%s(): vbus_active(%d), softconnect(%d)\n", __func__, mdwc->vbus_active, dwc->softconnect);
		if (mdwc->vbus_active && dwc->softconnect) {
			pr_info("%s(): put phy into non-drive mode\n", __func__);
			usb_phy_set_nondrive_mode(mdwc->hs_phy);
		}
		apsd_retried = true;
		mdwc->apsd_rerun_need = 1;
		power_supply_set_current_limit(&mdwc->usb_psy,
				FLOATED_CHG_CURRENT_MAX);
		power_supply_changed(&mdwc->usb_psy);
	}
}

static enum hrtimer_restart floated_chg_hrtimer_func(struct hrtimer *hrtimer)
{
	struct dwc3_msm *mdwc = container_of(hrtimer, struct dwc3_msm, floated_chg_hrtimer);

	pr_info("%s(): floated_chg_hrtimer expired\n", __func__);
	schedule_delayed_work(&mdwc->floated_chg_work, 0);

	return HRTIMER_NORESTART;
}
#endif

#ifdef CONFIG_LGE_USB_MAXIM_EVP
static int dwc3_otg_start_peripheral(struct dwc3_msm *mdwc, int on);

static void dwc3_evp_status(struct dwc3_msm *mdwc, unsigned evp_sts)
{
	enum hvdcp_chg_type hvdcp_type;

	if (!(evp_sts & EVP_STS_DCP)) {
		dev_info(mdwc->dev, "%s: Cable is not DCP.\n", __func__);
		hvdcp_type = HVDCP_NONE;
	} else if (evp_sts & EVP_STS_QC20) {
		dev_info(mdwc->dev, "%s: Cable is QC2.0\n", __func__);
		hvdcp_type = HVDCP_QC20;
	} else if (!(evp_sts & EVP_STS_EVP)) {
		dev_info(mdwc->dev, "%s: Cable is legacy DCP.\n", __func__);
		hvdcp_type = HVDCP_LEGACY_DCP;
	} else if (evp_sts & EVP_STS_DYNAMIC) {
		dev_info(mdwc->dev, "%s: Cable is EVP-dynamic.\n", __func__);
		hvdcp_type = HVDCP_EVP_DYNAMIC;
	} else if (evp_sts & EVP_STS_SIMPLE) {
		dev_info(mdwc->dev, "%s: Cable is EVP-simple.\n", __func__);
		hvdcp_type = HVDCP_EVP_SIMPLE;
	} else {
		dev_err(mdwc->dev, "%s: INVALID HVDCP TYPE.\n", __func__);
		hvdcp_type = INVALID_HVDCP;
	}
	mdwc->evp_sts = hvdcp_type;
	power_supply_changed(&mdwc->usb_psy);
}

static void dwc3_evp_event_notify(struct dwc3_msm *mdwc)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	if (!dwc) {
		dev_err(mdwc->dev, "%s: dwc driver data get failed.\n", __func__);
		return;
	}

	if (!(dwc->gadget.evp_sts & EVP_STS_DCP)) {
		dev_err(mdwc->dev, "XCVR: Invalid EVP detection request.\n");
		return;
	}

	if (dwc->gadget.evp_sts & EVP_STS_DETGO) {
		dev_err(mdwc->dev, "XCVR: Already EVP detection request received.\n");
		return;
	}

	if (mdwc->evp_detect) {
		dev_info(mdwc->dev, "XCVR: EVP detection start.\n");
		dwc->gadget.evp_sts |= EVP_STS_DETGO;
		schedule_delayed_work(&mdwc->sm_work, 0);
	} else {
		dwc->gadget.evp_sts |= EVP_STS_QC20;
		dwc3_evp_status(mdwc, dwc->gadget.evp_sts);
		dev_info(mdwc->dev, "XCVR: QC2.0 detected, keep dwc3 in LPM(%d).\n",
					atomic_read(&dwc->in_lpm));
	}
}

static void dwc3_otg_evp_connect_work(struct work_struct *w)
{
	struct power_supply *batt_psy;
	union power_supply_propval prop;

	batt_psy = power_supply_get_by_name("battery");
	if (!batt_psy) {
		pr_err("%s battery psy get failed.\n", __func__);
		return;
	}

	prop.intval = 1;

#ifdef CONFIG_LGE_PM_MAXIM_EVP_CONTROL
	batt_psy->set_property(batt_psy, POWER_SUPPLY_PROP_ENABLE_EVP_CHG, &prop);
#endif
	pr_info("%s EVP connected.\n", __func__);
}

static int dwc3_otg_evp_connect(struct dwc3_msm *mdwc, bool evp_connect)
{
	if (evp_connect)
		schedule_delayed_work(&mdwc->evp_connect_work, 0);
	else
		cancel_delayed_work(&mdwc->evp_connect_work);

	return 0;
}

void dwc_dcp_check_work(struct work_struct *w)
{
	struct dwc3 *dwc = container_of(w, struct dwc3, dcp_check_work.work);
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	int ret = 0;
	pr_info("%s %s connected.\n",
				__func__, (dwc->gadget.evp_sts & EVP_STS_EVP) ? "EVP" : "DCP");
	if (dwc->gadget.evp_sts & EVP_STS_EVP) {
		/*If erratic error happens while EVP enumeration, err count clear here*/
		dwc->evp_usbctrl_err_cnt = 0;
	}

	if (dwc->gadget.evp_sts & EVP_STS_DYNAMIC) {
		/*Dynamic mode*/
		pr_info("%s : EVP-dynamic mode\n", __func__);
	} else if (dwc->gadget.evp_sts & EVP_STS_SIMPLE) {
		/*Simple mode
		 *Dwc3 going to LPM, but keep DP pullup.
		 */
		pr_info("%s : EVP-simple mode, dwc3 into LPM.\n", __func__);
		dwc->gadget.evp_sts |= EVP_STS_SLEEP;
		if (!test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dwc->gadget.evp_sts &= ~EVP_STS_SLEEP;
			pr_err("%s : EVP unplugged, sm_work handle suspending. %u\n",
				__func__, dwc->gadget.evp_sts);
			return;
		}
		ret = pm_runtime_put_sync(mdwc->dev);
		if (ret)
			pr_err("%s : pm_runtime_put_sync result = %d.\n", __func__, ret);
		dbg_event(0xFF, "Dcpchk sim",
				atomic_read(&mdwc->dev->power.usage_count));
	} else if ((mdwc->otg_state == OTG_STATE_B_PERIPHERAL)
				&& (dwc->gadget.evp_sts & EVP_STS_DCP)) {
		pr_info("%s : DCP, dwc3 into LPM.\n", __func__);
		dwc3_otg_start_peripheral(mdwc, 0);
		ret = pm_runtime_put_sync(mdwc->dev);
		if (ret)
			pr_err("%s : pm_runtime_put_sync result = %d.\n", __func__, ret);
		mdwc->otg_state = OTG_STATE_B_IDLE;
		mdwc->chg_type = DWC3_INVALID_CHARGER;
		dbg_event(0xFF, "Dcpchk dcp",
				atomic_read(&mdwc->dev->power.usage_count));
	}
	if ((ret == -EBUSY) && atomic_read(&mdwc->dev->power.child_count)) {
		pr_info("%s : child count exists, and retry suspend\n",	__func__);
		pm_runtime_idle(dwc->dev);
		usleep_range(1000, 1200);
		pm_runtime_idle(mdwc->dev);
	}
	dwc3_evp_status(mdwc, dwc->gadget.evp_sts);
}
#endif

#ifdef CONFIG_LGE_PM_CABLE_DETECTION
static const char *chg_to_string(enum dwc3_chg_type chg_type);

static void dwc3_cable_adc_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
		cable_adc_work.work);

	if (IS_ERR_OR_NULL(mdwc->vadc_id_dev)) {
		 mdwc->vadc_id_dev = qpnp_get_vadc(mdwc->dev, "dwc");
		 if (IS_ERR(mdwc->vadc_id_dev)) {
			 if (PTR_ERR(mdwc->vadc_id_dev) == -EPROBE_DEFER) {
				 dev_err(mdwc->dev, "%s: qpnp vadc not yet "
					"probed.\n",  __func__);
				 schedule_delayed_work(&mdwc->cable_adc_work,
					msecs_to_jiffies(200));
				 return;
			 }
		 }
	}

	dev_dbg(mdwc->dev, "%s: charger type: %s\n", __func__,
			chg_to_string(mdwc->chg_type));

	lge_pm_read_cable_info(mdwc->vadc_id_dev);
	mdwc->adc_read_complete = true;
}

static void dwc3_read_cable_adc(struct dwc3_msm *mdwc, bool start)
{
	if (start) {
		cancel_delayed_work_sync(&mdwc->cable_adc_work);
		mdwc->adc_read_complete = false;
		schedule_delayed_work(&mdwc->cable_adc_work, 0);
	} else {
		cancel_delayed_work_sync(&mdwc->cable_adc_work);
		mdwc->adc_read_complete = false;
	}
}
#endif

/**
 *
 * Read register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg(void *base, u32 offset)
{
	u32 val = ioread32(base + offset);
	return val;
}

/**
 * Read register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 *
 * @return u32
 */
static inline u32 dwc3_msm_read_reg_field(void *base,
					  u32 offset,
					  const u32 mask)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 val = ioread32(base + offset);
	val &= mask;		/* clear other bits */
	val >>= shift;
	return val;
}

/**
 *
 * Write register with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg(void *base, u32 offset, u32 val)
{
	iowrite32(val, base + offset);
}

/**
 * Write register masked field with debug info.
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask.
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_reg_field(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 shift = find_first_bit((void *)&mask, 32);
	u32 tmp = ioread32(base + offset);

	tmp &= ~mask;		/* clear written bits */
	val = tmp | (val << shift);
	iowrite32(val, base + offset);
}

/**
 * Write register and read back masked value to confirm it is written
 *
 * @base - DWC3 base virtual address.
 * @offset - register offset.
 * @mask - register bitmask specifying what should be updated
 * @val - value to write.
 *
 */
static inline void dwc3_msm_write_readback(void *base, u32 offset,
					    const u32 mask, u32 val)
{
	u32 write_val, tmp = ioread32(base + offset);

	tmp &= ~mask;		/* retain other bits */
	write_val = tmp | val;

	iowrite32(write_val, base + offset);

	/* Read back to see if val was written */
	tmp = ioread32(base + offset);
	tmp &= mask;		/* clear other bits */

	if (tmp != val)
		pr_err("%s: write: %x to QSCRATCH: %x FAILED\n",
			__func__, val, offset);
}

static bool dwc3_msm_is_host_superspeed(struct dwc3_msm *mdwc)
{
	int i, num_ports;
	u32 reg;

	reg = dwc3_msm_read_reg(mdwc->base, USB3_HCSPARAMS1);
	num_ports = HCS_MAX_PORTS(reg);

	for (i = 0; i < num_ports; i++) {
		reg = dwc3_msm_read_reg(mdwc->base, USB3_PORTSC + i*0x10);
		if ((reg & PORT_PE) && DEV_SUPERSPEED(reg))
			return true;
	}

	return false;
}

static inline bool dwc3_msm_is_dev_superspeed(struct dwc3_msm *mdwc)
{
	u8 speed;

	speed = dwc3_msm_read_reg(mdwc->base, DWC3_DSTS) & DWC3_DSTS_CONNECTSPD;
	return !!(speed & DSTS_CONNECTSPD_SS);
}

static inline bool dwc3_msm_is_superspeed(struct dwc3_msm *mdwc)
{
	if (mdwc->in_host_mode)
		return dwc3_msm_is_host_superspeed(mdwc);

	return dwc3_msm_is_dev_superspeed(mdwc);
}

/**
 * Configure the DBM with the BAM's data fifo.
 * This function is called by the USB BAM Driver
 * upon initialization.
 *
 * @ep - pointer to usb endpoint.
 * @addr - address of data fifo.
 * @size - size of data fifo.
 *
 */
int msm_data_fifo_config(struct usb_ep *ep, phys_addr_t addr,
			 u32 size, u8 dst_pipe_idx)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	dev_dbg(mdwc->dev, "%s\n", __func__);

	return	dbm_data_fifo_config(mdwc->dbm, dep->number, addr, size,
						dst_pipe_idx);
}


/**
* Cleanups for msm endpoint on request complete.
*
* Also call original request complete.
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
*
* @return int - 0 on success, negative on error.
*/
static void dwc3_msm_req_complete_func(struct usb_ep *ep,
				       struct usb_request *request)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete = NULL;

	/* Find original request complete function and remove it from list */
	list_for_each_entry(req_complete, &mdwc->req_complete_list, list_item) {
		if (req_complete->req == request)
			break;
	}
	if (!req_complete || req_complete->req != request) {
		dev_err(dep->dwc->dev, "%s: could not find the request\n",
					__func__);
		return;
	}
	list_del(&req_complete->list_item);

	/*
	 * Release another one TRB to the pool since DBM queue took 2 TRBs
	 * (normal and link), and the dwc3/gadget.c :: dwc3_gadget_giveback
	 * released only one.
	 */
	dep->busy_slot++;

	/* Unconfigure dbm ep */
	dbm_ep_unconfig(mdwc->dbm, dep->number);

	/*
	 * If this is the last endpoint we unconfigured, than reset also
	 * the event buffers; unless unconfiguring the ep due to lpm,
	 * in which case the event buffer only gets reset during the
	 * block reset.
	 */
	if (0 == dbm_get_num_of_eps_configured(mdwc->dbm) &&
		!dbm_reset_ep_after_lpm(mdwc->dbm))
			dbm_event_buffer_config(mdwc->dbm, 0, 0, 0);

	/*
	 * Call original complete function, notice that dwc->lock is already
	 * taken by the caller of this function (dwc3_gadget_giveback()).
	 */
	request->complete = req_complete->orig_complete;
	if (request->complete)
		request->complete(ep, request);

	kfree(req_complete);
}


/**
* Helper function
*
* Reset  DBM endpoint.
*
* @mdwc - pointer to dwc3_msm instance.
* @dep - pointer to dwc3_ep instance.
*
* @return int - 0 on success, negative on error.
*/
static int __dwc3_msm_dbm_ep_reset(struct dwc3_msm *mdwc, struct dwc3_ep *dep)
{
	int ret;

	dev_dbg(mdwc->dev, "Resetting dbm endpoint %d\n", dep->number);

	/* Reset the dbm endpoint */
	ret = dbm_ep_soft_reset(mdwc->dbm, dep->number, true);
	if (ret) {
		dev_err(mdwc->dev, "%s: failed to assert dbm ep reset\n",
				__func__);
		return ret;
	}

	/*
	 * The necessary delay between asserting and deasserting the dbm ep
	 * reset is based on the number of active endpoints. If there is more
	 * than one endpoint, a 1 msec delay is required. Otherwise, a shorter
	 * delay will suffice.
	 */
	if (dbm_get_num_of_eps_configured(mdwc->dbm) > 1)
		usleep_range(1000, 1200);
	else
		udelay(10);
	ret = dbm_ep_soft_reset(mdwc->dbm, dep->number, false);
	if (ret) {
		dev_err(mdwc->dev, "%s: failed to deassert dbm ep reset\n",
				__func__);
		return ret;
	}

	return 0;
}

/**
* Reset the DBM endpoint which is linked to the given USB endpoint.
*
* @usb_ep - pointer to usb_ep instance.
*
* @return int - 0 on success, negative on error.
*/

int msm_dwc3_reset_dbm_ep(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	return __dwc3_msm_dbm_ep_reset(mdwc, dep);
}
EXPORT_SYMBOL(msm_dwc3_reset_dbm_ep);


/**
* Helper function.
* See the header of the dwc3_msm_ep_queue function.
*
* @dwc3_ep - pointer to dwc3_ep instance.
* @req - pointer to dwc3_request instance.
*
* @return int - 0 on success, negative on error.
*/
static int __dwc3_msm_ep_queue(struct dwc3_ep *dep, struct dwc3_request *req)
{
	struct dwc3_trb *trb;
	struct dwc3_trb *trb_link;
	struct dwc3_gadget_ep_cmd_params params;
	u32 cmd;
	int ret = 0;

	/* We push the request to the dep->req_queued list to indicate that
	 * this request is issued with start transfer. The request will be out
	 * from this list in 2 cases. The first is that the transfer will be
	 * completed (not if the transfer is endless using a circular TRBs with
	 * with link TRB). The second case is an option to do stop stransfer,
	 * this can be initiated by the function driver when calling dequeue.
	 */
	req->queued = true;
	list_add_tail(&req->list, &dep->req_queued);

	/* First, prepare a normal TRB, point to the fake buffer */
	trb = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb, 0, sizeof(*trb));

	req->trb = trb;
	trb->bph = DBM_TRB_BIT | DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb->size = DWC3_TRB_SIZE_LENGTH(req->request.length);
	trb->ctrl = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_HWO |
		DWC3_TRB_CTRL_CHN | (req->direction ? 0 : DWC3_TRB_CTRL_CSP);
	req->trb_dma = dwc3_trb_dma_offset(dep, trb);

	/* Second, prepare a Link TRB that points to the first TRB*/
	trb_link = &dep->trb_pool[dep->free_slot & DWC3_TRB_MASK];
	dep->free_slot++;
	memset(trb_link, 0, sizeof *trb_link);

	trb_link->bpl = lower_32_bits(req->trb_dma);
	trb_link->bph = DBM_TRB_BIT |
			DBM_TRB_DMA | DBM_TRB_EP_NUM(dep->number);
	trb_link->size = 0;
	trb_link->ctrl = DWC3_TRBCTL_LINK_TRB | DWC3_TRB_CTRL_HWO;

	/*
	 * Now start the transfer
	 */
	memset(&params, 0, sizeof(params));
	params.param0 = 0; /* TDAddr High */
	params.param1 = lower_32_bits(req->trb_dma); /* DAddr Low */

	/* DBM requires IOC to be set */
	cmd = DWC3_DEPCMD_STARTTRANSFER | DWC3_DEPCMD_CMDIOC;
	ret = dwc3_send_gadget_ep_cmd(dep->dwc, dep->number, cmd, &params);
	if (ret < 0) {
		dev_dbg(dep->dwc->dev,
			"%s: failed to send STARTTRANSFER command\n",
			__func__);

		list_del(&req->list);
		return ret;
	}
	dep->flags |= DWC3_EP_BUSY;
	dep->resource_index = dwc3_gadget_ep_get_transfer_index(dep->dwc,
		dep->number);

	return ret;
}

/**
* Queue a usb request to the DBM endpoint.
* This function should be called after the endpoint
* was enabled by the ep_enable.
*
* This function prepares special structure of TRBs which
* is familiar with the DBM HW, so it will possible to use
* this endpoint in DBM mode.
*
* The TRBs prepared by this function, is one normal TRB
* which point to a fake buffer, followed by a link TRB
* that points to the first TRB.
*
* The API of this function follow the regular API of
* usb_ep_queue (see usb_ep_ops in include/linuk/usb/gadget.h).
*
* @usb_ep - pointer to usb_ep instance.
* @request - pointer to usb_request instance.
* @gfp_flags - possible flags.
*
* @return int - 0 on success, negative on error.
*/
static int dwc3_msm_ep_queue(struct usb_ep *ep,
			     struct usb_request *request, gfp_t gfp_flags)
{
	struct dwc3_request *req = to_dwc3_request(request);
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct dwc3_msm_req_complete *req_complete;
	unsigned long flags;
	int ret = 0, size;
	u8 bam_pipe;
	bool producer;
	bool disable_wb;
	bool internal_mem;
	bool ioc;
	bool superspeed;

	if (!(request->udc_priv & MSM_SPS_MODE)) {
		/* Not SPS mode, call original queue */
		dev_vdbg(mdwc->dev, "%s: not sps mode, use regular queue\n",
					__func__);

		return (mdwc->original_ep_ops[dep->number])->queue(ep,
								request,
								gfp_flags);
	}

	if (!dep->endpoint.desc) {
		dev_err(mdwc->dev,
			"%s: trying to queue request %p to disabled ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	}

	if (dep->number == 0 || dep->number == 1) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %p to control ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	}


	if (dep->busy_slot != dep->free_slot || !list_empty(&dep->request_list)
					 || !list_empty(&dep->req_queued)) {
		dev_err(mdwc->dev,
			"%s: trying to queue dbm request %p tp ep %s\n",
			__func__, request, ep->name);
		return -EPERM;
	} else {
		dep->busy_slot = 0;
		dep->free_slot = 0;
	}

	/* HW restriction regarding TRB size (8KB) */
	if (req->request.length < 0x2000) {
		dev_err(mdwc->dev, "%s: Min TRB size is 8KB\n", __func__);
		return -EINVAL;
	}

	/*
	 * Override req->complete function, but before doing that,
	 * store it's original pointer in the req_complete_list.
	 */
	req_complete = kzalloc(sizeof(*req_complete), gfp_flags);
	if (!req_complete) {
		dev_err(mdwc->dev, "%s: not enough memory\n", __func__);
		return -ENOMEM;
	}
	req_complete->req = request;
	req_complete->orig_complete = request->complete;
	list_add_tail(&req_complete->list_item, &mdwc->req_complete_list);
	request->complete = dwc3_msm_req_complete_func;

	/*
	 * Configure the DBM endpoint
	 */
	bam_pipe = request->udc_priv & MSM_PIPE_ID_MASK;
	producer = ((request->udc_priv & MSM_PRODUCER) ? true : false);
	disable_wb = ((request->udc_priv & MSM_DISABLE_WB) ? true : false);
	internal_mem = ((request->udc_priv & MSM_INTERNAL_MEM) ? true : false);
	ioc = ((request->udc_priv & MSM_ETD_IOC) ? true : false);

	ret = dbm_ep_config(mdwc->dbm, dep->number, bam_pipe, producer,
				disable_wb, internal_mem, ioc);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling dbm_ep_config\n", ret);
		return ret;
	}

	dev_vdbg(dwc->dev, "%s: queing request %p to ep %s length %d\n",
			__func__, request, ep->name, request->length);
	size = dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTSIZ(0));
	dbm_event_buffer_config(mdwc->dbm,
		dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTADRLO(0)),
		dwc3_msm_read_reg(mdwc->base, DWC3_GEVNTADRHI(0)),
		DWC3_GEVNTSIZ_SIZE(size));

	/*
	 * We must obtain the lock of the dwc3 core driver,
	 * including disabling interrupts, so we will be sure
	 * that we are the only ones that configure the HW device
	 * core and ensure that we queuing the request will finish
	 * as soon as possible so we will release back the lock.
	 */
	spin_lock_irqsave(&dwc->lock, flags);
	ret = __dwc3_msm_ep_queue(dep, req);
	spin_unlock_irqrestore(&dwc->lock, flags);
	if (ret < 0) {
		dev_err(mdwc->dev,
			"error %d after calling __dwc3_msm_ep_queue\n", ret);
		return ret;
	}

	superspeed = dwc3_msm_is_dev_superspeed(mdwc);
	dbm_set_speed(mdwc->dbm, (u8)superspeed);

	return 0;
}


/**
 * Configure MSM endpoint.
 * This function do specific configurations
 * to an endpoint which need specific implementaion
 * in the MSM architecture.
 *
 * This function should be called by usb function/class
 * layer which need a support from the specific MSM HW
 * which wrap the USB3 core. (like DBM specific endpoints)
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negetive on error.
 */
int msm_ep_config(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *new_ep_ops;


	/* Save original ep ops for future restore*/
	if (mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] already configured as msm endpoint\n",
			ep->name, dep->number);
		return -EPERM;
	}
	mdwc->original_ep_ops[dep->number] = ep->ops;

	/* Set new usb ops as we like */
	new_ep_ops = kzalloc(sizeof(struct usb_ep_ops), GFP_ATOMIC);
	if (!new_ep_ops) {
		dev_err(mdwc->dev,
			"%s: unable to allocate mem for new usb ep ops\n",
			__func__);
		return -ENOMEM;
	}
	(*new_ep_ops) = (*ep->ops);
	new_ep_ops->queue = dwc3_msm_ep_queue;
	ep->ops = new_ep_ops;

	/*
	 * Do HERE more usb endpoint configurations
	 * which are specific to MSM.
	 */

	return 0;
}
EXPORT_SYMBOL(msm_ep_config);

/**
 * Un-configure MSM endpoint.
 * Tear down configurations done in the
 * dwc3_msm_ep_config function.
 *
 * @ep - a pointer to some usb_ep instance
 *
 * @return int - 0 on success, negative on error.
 */
int msm_ep_unconfig(struct usb_ep *ep)
{
	struct dwc3_ep *dep = to_dwc3_ep(ep);
	struct dwc3 *dwc = dep->dwc;
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	struct usb_ep_ops *old_ep_ops;

	/* Restore original ep ops */
	if (!mdwc->original_ep_ops[dep->number]) {
		dev_err(mdwc->dev,
			"ep [%s,%d] was not configured as msm endpoint\n",
			ep->name, dep->number);
		return -EINVAL;
	}
	old_ep_ops = (struct usb_ep_ops	*)ep->ops;
	ep->ops = mdwc->original_ep_ops[dep->number];
	mdwc->original_ep_ops[dep->number] = NULL;
	kfree(old_ep_ops);

	/*
	 * Do HERE more usb endpoint un-configurations
	 * which are specific to MSM.
	 */

	return 0;
}
EXPORT_SYMBOL(msm_ep_unconfig);
static void dwc3_resume_work(struct work_struct *w);

static void dwc3_restart_usb_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
						restart_usb_work);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	enum dwc3_chg_type chg_type;
	unsigned timeout = 50;

	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (atomic_read(&dwc->in_lpm) || !dwc->is_drd) {
		dev_err(mdwc->dev, "%s failed!!!\n", __func__);
		return;
	}

	/* guard against concurrent VBUS handling */
	mdwc->in_restart = true;

	if (!mdwc->vbus_active) {
		dev_dbg(mdwc->dev, "%s bailing out in disconnect\n", __func__);
		dwc->err_evt_seen = false;
		mdwc->in_restart = false;
		return;
	}

	dbg_event(0xFF, "RestartUSB", 0);
	chg_type = mdwc->chg_type;

	/* Reset active USB connection */
	dwc3_resume_work(&mdwc->resume_work.work);

	/* Make sure disconnect is processed before sending connect */
	while (--timeout && !pm_runtime_suspended(mdwc->dev))
		msleep(20);

	if (!timeout) {
		dev_warn(mdwc->dev, "Not in LPM after disconnect, forcing suspend...\n");
		pm_runtime_suspend(mdwc->dev);
	}

	/* Force reconnect only if cable is still connected */
	if (mdwc->vbus_active) {
		mdwc->chg_type = chg_type;
		mdwc->in_restart = false;
		dwc3_resume_work(&mdwc->resume_work.work);
	}

	dwc->err_evt_seen = false;
}

/**
 * Reset USB peripheral connection
 * Inform OTG for Vbus LOW followed by Vbus HIGH notification.
 * This performs full hardware reset and re-initialization which
 * might be required by some DBM client driver during uninit/cleanup.
 */
void msm_dwc3_restart_usb_session(struct usb_gadget *gadget)
{
	struct dwc3 *dwc = container_of(gadget, struct dwc3, gadget);
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	if (!mdwc)
		return;

	dev_dbg(mdwc->dev, "%s\n", __func__);
	schedule_work(&mdwc->restart_usb_work);
}
EXPORT_SYMBOL(msm_dwc3_restart_usb_session);

/**
 * msm_register_usb_ext_notification: register for event notification
 * @info: pointer to client usb_ext_notification structure. May be NULL.
 *
 * @return int - 0 on success, negative on error
 */
int msm_register_usb_ext_notification(struct usb_ext_notification *info)
{
	pr_debug("%s usb_ext: %p\n", __func__, info);

	if (info) {
		if (usb_ext) {
			pr_err("%s: already registered\n", __func__);
			return -EEXIST;
		}

		if (!info->notify) {
			pr_err("%s: notify is NULL\n", __func__);
			return -EINVAL;
		}
	}

	usb_ext = info;
	return 0;
}
EXPORT_SYMBOL(msm_register_usb_ext_notification);

/*
 * Check whether the DWC3 requires resetting the ep
 * after going to Low Power Mode (lpm)
 */
bool msm_dwc3_reset_ep_after_lpm(struct usb_gadget *gadget)
{
	struct dwc3 *dwc = container_of(gadget, struct dwc3, gadget);
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);

	return dbm_reset_ep_after_lpm(mdwc->dbm);
}
EXPORT_SYMBOL(msm_dwc3_reset_ep_after_lpm);

/*
 * Config Global Distributed Switch Controller (GDSC)
 * to support controller power collapse
 */
static int dwc3_msm_config_gdsc(struct dwc3_msm *mdwc, int on)
{
	int ret;

	if (IS_ERR_OR_NULL(mdwc->dwc3_gdsc))
		return -EPERM;

	if (on) {
		ret = regulator_enable(mdwc->dwc3_gdsc);
		if (ret) {
			dev_err(mdwc->dev, "unable to enable usb3 gdsc\n");
			return ret;
		}
	} else {
		ret = regulator_disable(mdwc->dwc3_gdsc);
		if (ret) {
			dev_err(mdwc->dev, "unable to disable usb3 gdsc\n");
			return ret;
		}
	}

	return ret;
}

static int dwc3_msm_link_clk_reset(struct dwc3_msm *mdwc, bool assert)
{
	int ret = 0;

	if (assert) {
		if (mdwc->pwr_event_irq)
			disable_irq(mdwc->pwr_event_irq);
		/* Using asynchronous block reset to the hardware */
		dev_dbg(mdwc->dev, "block_reset ASSERT\n");
		clk_disable_unprepare(mdwc->utmi_clk);
		clk_disable_unprepare(mdwc->sleep_clk);
		clk_disable_unprepare(mdwc->core_clk);
		clk_disable_unprepare(mdwc->iface_clk);
		ret = clk_reset(mdwc->core_clk, CLK_RESET_ASSERT);
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk assert failed\n");
	} else {
		dev_dbg(mdwc->dev, "block_reset DEASSERT\n");
		ret = clk_reset(mdwc->core_clk, CLK_RESET_DEASSERT);
		ndelay(200);
		clk_prepare_enable(mdwc->iface_clk);
		clk_prepare_enable(mdwc->core_clk);
		clk_prepare_enable(mdwc->sleep_clk);
		clk_prepare_enable(mdwc->utmi_clk);
		if (ret)
			dev_err(mdwc->dev, "dwc3 core_clk deassert failed\n");
		if (mdwc->pwr_event_irq)
			enable_irq(mdwc->pwr_event_irq);
	}

	return ret;
}

static void dwc3_msm_update_ref_clk(struct dwc3_msm *mdwc)
{
	u32 guctl, gfladj = 0;

	guctl = dwc3_msm_read_reg(mdwc->base, DWC3_GUCTL);
	guctl &= ~DWC3_GUCTL_REFCLKPER;

	/* GFLADJ register is used starting with revision 2.50a */
	if (dwc3_msm_read_reg(mdwc->base, DWC3_GSNPSID) >= DWC3_REVISION_250A) {
		gfladj = dwc3_msm_read_reg(mdwc->base, DWC3_GFLADJ);
		gfladj &= ~DWC3_GFLADJ_REFCLK_240MHZDECR_PLS1;
		gfladj &= ~DWC3_GFLADJ_REFCLK_240MHZ_DECR;
		gfladj &= ~DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj &= ~DWC3_GFLADJ_REFCLK_FLADJ;
	}

	/* Refer to SNPS Databook Table 6-55 for calculations used */
	switch (mdwc->utmi_clk_rate) {
	case 19200000:
		guctl |= 52 << __ffs(DWC3_GUCTL_REFCLKPER);
		gfladj |= 12 << __ffs(DWC3_GFLADJ_REFCLK_240MHZ_DECR);
		gfladj |= DWC3_GFLADJ_REFCLK_240MHZDECR_PLS1;
		gfladj |= DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj |= 200 << __ffs(DWC3_GFLADJ_REFCLK_FLADJ);
		break;
	case 24000000:
		guctl |= 41 << __ffs(DWC3_GUCTL_REFCLKPER);
		gfladj |= 10 << __ffs(DWC3_GFLADJ_REFCLK_240MHZ_DECR);
		gfladj |= DWC3_GFLADJ_REFCLK_LPM_SEL;
		gfladj |= 2032 << __ffs(DWC3_GFLADJ_REFCLK_FLADJ);
		break;
	default:
		dev_warn(mdwc->dev, "Unsupported utmi_clk_rate: %u\n",
				mdwc->utmi_clk_rate);
		break;
	}

	dwc3_msm_write_reg(mdwc->base, DWC3_GUCTL, guctl);
	if (gfladj)
		dwc3_msm_write_reg(mdwc->base, DWC3_GFLADJ, gfladj);
}

/* Initialize QSCRATCH registers for HSPHY and SSPHY operation */
static void dwc3_msm_qscratch_reg_init(struct dwc3_msm *mdwc)
{
	if (dwc3_msm_read_reg(mdwc->base, DWC3_GSNPSID) < DWC3_REVISION_250A)
		/* On older cores set XHCI_REV bit to specify revision 1.0 */
		dwc3_msm_write_reg_field(mdwc->base, QSCRATCH_GENERAL_CFG,
					 BIT(2), 1);

	/*
	 * Enable master clock for RAMs to allow BAM to access RAMs when
	 * RAM clock gating is enabled via DWC3's GCTL. Otherwise issues
	 * are seen where RAM clocks get turned OFF in SS mode
	 */
	dwc3_msm_write_reg(mdwc->base, CGCTL_REG,
		dwc3_msm_read_reg(mdwc->base, CGCTL_REG) | 0x18);

}

static void dwc3_msm_notify_event(struct dwc3 *dwc, unsigned event)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dwc->dev->parent);
	u32 reg;
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	static DEFINE_RATELIMIT_STATE(rl, 10*HZ, 1);
#endif

	if (dwc->revision < DWC3_REVISION_230A)
		return;

	switch (event) {
	case DWC3_CONTROLLER_ERROR_EVENT:
#ifdef CONFIG_LGE_USB_MAXIM_EVP
		if (__ratelimit(&rl))
#endif
		dev_info(mdwc->dev,
			"DWC3_CONTROLLER_ERROR_EVENT received, irq cnt %lu, evp err cnt %u, evp sts %u\n",
			dwc->irq_cnt, dwc->evp_usbctrl_err_cnt, dwc->gadget.evp_sts);
#ifdef CONFIG_LGE_USB_MAXIM_EVP
		if (dwc->gadget.evp_sts & EVP_STS_DCP) {
			if (dwc->evp_usbctrl_err_cnt > 500) {
				/*
				 * Some erratic error could happens during EVP enumeration.
				 * We need to block reset if too many erratic error occurs.
				 * If not, it will be cause watchdong bite.
				 */
				dwc->evp_usbctrl_err_cnt = 0;
			} else {
				dwc->err_evt_seen = 0;
				break;
			}
		}
#endif
		dwc3_gadget_disable_irq(dwc);

		/* prevent core from generating interrupts until recovery */
		reg = dwc3_msm_read_reg(mdwc->base, DWC3_GCTL);
		reg |= DWC3_GCTL_CORESOFTRESET;
		dwc3_msm_write_reg(mdwc->base, DWC3_GCTL, reg);

		/* restart USB which performs full reset and reconnect */
		schedule_work(&mdwc->restart_usb_work);
		break;
	case DWC3_CONTROLLER_RESET_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_RESET_EVENT received\n");
		/* HS & SSPHYs get reset as part of core soft reset */
		dwc3_msm_qscratch_reg_init(mdwc);
		break;
	case DWC3_CONTROLLER_POST_RESET_EVENT:
		dev_dbg(mdwc->dev,
				"DWC3_CONTROLLER_POST_RESET_EVENT received\n");

		/*
		 * Below sequence is used when controller is working without
		 * having ssphy and only USB high speed is supported.
		 */
		if (dwc->maximum_speed == USB_SPEED_HIGH) {
			dwc3_msm_write_reg(mdwc->base, QSCRATCH_GENERAL_CFG,
				dwc3_msm_read_reg(mdwc->base,
				QSCRATCH_GENERAL_CFG)
				| PIPE_UTMI_CLK_DIS);

			usleep_range(2, 5);


			dwc3_msm_write_reg(mdwc->base, QSCRATCH_GENERAL_CFG,
				dwc3_msm_read_reg(mdwc->base,
				QSCRATCH_GENERAL_CFG)
				| PIPE_UTMI_CLK_SEL
				| PIPE3_PHYSTATUS_SW);

			usleep_range(2, 5);

			dwc3_msm_write_reg(mdwc->base, QSCRATCH_GENERAL_CFG,
				dwc3_msm_read_reg(mdwc->base,
				QSCRATCH_GENERAL_CFG)
				& ~PIPE_UTMI_CLK_DIS);
		}

		dwc3_msm_update_ref_clk(mdwc);
		dwc->tx_fifo_size = mdwc->tx_fifo_size;
		break;
	case DWC3_CONTROLLER_CONNDONE_EVENT:
		dev_info(mdwc->dev, "DWC3_CONTROLLER_CONNDONE_EVENT received\n");
		/*
		 * Add power event if the dbm indicates coming out of L1 by
		 * interrupt
		 */
		if (mdwc->dbm && dbm_l1_lpm_interrupt(mdwc->dbm))
			dwc3_msm_write_reg_field(mdwc->base,
					PWR_EVNT_IRQ_MASK_REG,
					PWR_EVNT_LPM_OUT_L1_MASK, 1);

		atomic_set(&dwc->in_lpm, 0);
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
		if (mdwc->usb_psy.is_floated_charger == 0) {
			pr_info("%s(): cancel floated_chg_hrtimer\n", __func__);
			hrtimer_cancel(&mdwc->floated_chg_hrtimer);
		} else {
			pr_info("%s(): sdp connected while in is_floated_charger\n", __func__);
			cancel_delayed_work(&mdwc->floated_chg_work);

			mdwc->usb_psy.is_floated_charger = 0;
			power_supply_changed(&mdwc->usb_psy);
		}
#endif
		break;
	case DWC3_CONTROLLER_NOTIFY_OTG_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_NOTIFY_OTG_EVENT received\n");
		if (dwc->enable_bus_suspend) {
			mdwc->suspend = dwc->b_suspend;
#ifdef CONFIG_LGE_USB_G_ANDROID
			cancel_delayed_work(&mdwc->resume_work);
#endif
			queue_delayed_work(mdwc->dwc3_wq,
					&mdwc->resume_work, 0);
		}
		break;
	case DWC3_CONTROLLER_SET_CURRENT_DRAW_EVENT:
		dev_dbg(mdwc->dev, "DWC3_CONTROLLER_SET_CURRENT_DRAW_EVENT received\n");
#ifdef CONFIG_LGE_USB_MAXIM_EVP
		if (dwc->gadget.evp_sts & EVP_STS_DCP) {
			dev_dbg(mdwc->dev, "DWC3_CONTROLLER_SET_CURRENT_DRAW_EVENT ignored\n");
			break;
		}
#endif
		dwc3_msm_gadget_vbus_draw(mdwc, dwc->vbus_draw);
		break;
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	case DWC3_EVP_CONNECT_EVENT:
		dev_dbg(mdwc->dev, "DWC3_EVP_CONNECT_EVENT received : %d\n", dwc->evp_connect);
		dwc3_otg_evp_connect(mdwc, dwc->evp_connect);
		break;
#endif
	default:
		dev_dbg(mdwc->dev, "unknown dwc3 event\n");
		break;
	}
}

static void dwc3_msm_block_reset(struct dwc3_msm *mdwc, bool core_reset)
{
	int ret  = 0;

	if (core_reset) {
		ret = dwc3_msm_link_clk_reset(mdwc, 1);
		if (ret)
			return;

		usleep_range(1000, 1200);
		ret = dwc3_msm_link_clk_reset(mdwc, 0);
		if (ret)
			return;

		usleep_range(10000, 12000);
	}

	if (mdwc->dbm) {
		/* Reset the DBM */
		dbm_soft_reset(mdwc->dbm, 1);
		usleep_range(1000, 1200);
		dbm_soft_reset(mdwc->dbm, 0);

		/*enable DBM*/
		dwc3_msm_write_reg_field(mdwc->base, QSCRATCH_GENERAL_CFG,
			DBM_EN_MASK, 0x1);
		dbm_enable(mdwc->dbm);
	}
}

static const char *chg_to_string(enum dwc3_chg_type chg_type)
{
	switch (chg_type) {
	case DWC3_SDP_CHARGER:		return "USB_SDP_CHARGER";
	case DWC3_DCP_CHARGER:		return "USB_DCP_CHARGER";
	case DWC3_CDP_CHARGER:		return "USB_CDP_CHARGER";
	case DWC3_PROPRIETARY_CHARGER:	return "USB_PROPRIETARY_CHARGER";
	case DWC3_FLOATED_CHARGER:	return "USB_FLOATED_CHARGER";
	default:			return "UNKNOWN_CHARGER";
	}
}

static void dwc3_msm_power_collapse_por(struct dwc3_msm *mdwc)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	u32 val;

	/* Configure AHB2PHY for one wait state read/write */
	if (mdwc->ahb2phy_base) {
		clk_prepare_enable(mdwc->cfg_ahb_clk);
		val = readl_relaxed(mdwc->ahb2phy_base +
				PERIPH_SS_AHB2PHY_TOP_CFG);
		if (val != ONE_READ_WRITE_WAIT) {
			writel_relaxed(ONE_READ_WRITE_WAIT,
				mdwc->ahb2phy_base + PERIPH_SS_AHB2PHY_TOP_CFG);
			/* complete above write before configuring USB PHY. */
			mb();
		}
		clk_disable_unprepare(mdwc->cfg_ahb_clk);
	}

	dwc3_core_init(dwc);
	/* Re-configure event buffers */
	dwc3_event_buffers_setup(dwc);
	dwc3_msm_notify_event(dwc, DWC3_CONTROLLER_POST_INITIALIZATION_EVENT);
}

static int dwc3_msm_prepare_suspend(struct dwc3_msm *mdwc)
{
	unsigned long timeout;
	u32 reg = 0;
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
#endif

	if ((mdwc->in_host_mode || mdwc->vbus_active)
			&& dwc3_msm_is_superspeed(mdwc)) {
#ifdef CONFIG_LGE_USB_MAXIM_EVP
		if (!(dwc->gadget.evp_sts & EVP_STS_DCP)
				&& (mdwc->chg_type != DWC3_DCP_CHARGER))
#endif
		if (!atomic_read(&mdwc->in_p3)) {
			dev_err(mdwc->dev, "Not in P3,aborting LPM sequence\n");
			return -EBUSY;
		}
	}

	/* Clear previous L2 events */
	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG,
		PWR_EVNT_LPM_IN_L2_MASK | PWR_EVNT_LPM_OUT_L2_MASK);

	/* Prepare HSPHY for suspend */
	reg = dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0));
	dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
		reg | DWC3_GUSB2PHYCFG_ENBLSLPM | DWC3_GUSB2PHYCFG_SUSPHY);

	/* Wait for PHY to go into L2 */
	timeout = jiffies + msecs_to_jiffies(5);
	while (!time_after(jiffies, timeout)) {
		reg = dwc3_msm_read_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG);
		if (reg & PWR_EVNT_LPM_IN_L2_MASK)
			break;
	}
	if (!(reg & PWR_EVNT_LPM_IN_L2_MASK))
		dev_err(mdwc->dev, "could not transition HS PHY to L2\n");

	/* Clear L2 event bit */
	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG,
		PWR_EVNT_LPM_IN_L2_MASK);

	return 0;
}

static void dwc3_msm_wake_interrupt_enable(struct dwc3_msm *mdwc, bool on)
{
	u32 irq_mask, irq_stat;
	u32 wakeup_events = PWR_EVNT_POWERDOWN_OUT_P3_MASK |
		PWR_EVNT_LPM_OUT_L2_MASK;

	irq_stat = dwc3_msm_read_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG);

	/* clear pending interrupts */
	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG, irq_stat);

	irq_mask = dwc3_msm_read_reg(mdwc->base, PWR_EVNT_IRQ_MASK_REG);

	if (on) /* Enable P3 and L2 OUT events */
		irq_mask |= wakeup_events;
	else /* Disable P3 and L2 OUT events */
		irq_mask &= ~wakeup_events;

	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_MASK_REG, irq_mask);
}

static void dwc3_msm_bus_vote_w(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, bus_vote_w);
	int ret;

	ret = msm_bus_scale_client_update_request(mdwc->bus_perf_client,
			mdwc->bus_vote);
	if (ret)
		dev_err(mdwc->dev, "Failed to reset bus bw vote %d\n", ret);
}

static int dwc3_msm_suspend(struct dwc3_msm *mdwc)
{
	int ret, i;
	bool can_suspend_ssphy;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dbg_event(0xFF, "Ctl Sus", atomic_read(&dwc->in_lpm));

	if (atomic_read(&dwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already suspended\n", __func__);
		return 0;
	}

	if (!mdwc->in_host_mode) {
		/* pending device events unprocessed */
		for (i = 0; i < dwc->num_event_buffers; i++) {
			struct dwc3_event_buffer *evt = dwc->ev_buffs[i];
			if ((evt->flags & DWC3_EVENT_PENDING)) {
				dev_dbg(mdwc->dev,
				"%s: %d device events pending, abort suspend\n",
				__func__, evt->count / 4);
				dbg_print_reg("PENDING DEVICE EVENT",
						*(u32 *)(evt->buf + evt->lpos));
				return -EBUSY;
			}
		}
	}

	if (!mdwc->vbus_active && dwc->is_drd &&
		mdwc->otg_state == OTG_STATE_B_PERIPHERAL) {
		/*
		 * In some cases, the pm_runtime_suspend may be called by
		 * usb_bam when there is pending lpm flag. However, if this is
		 * done when cable was disconnected and otg state has not
		 * yet changed to IDLE, then it means OTG state machine
		 * is running and we race against it. So cancel LPM for now,
		 * and OTG state machine will go for LPM later, after completing
		 * transition to IDLE state.
		*/
		dev_dbg(mdwc->dev,
			"%s: cable disconnected while not in idle otg state\n",
			__func__);
		return -EBUSY;
	}

#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	cancel_delayed_work_sync(&mdwc->cable_adc_work);
#endif

	/*
	 * Check if device is not in CONFIGURED state
	 * then check controller state of L2 and break
	 * LPM sequence. Check this for device bus suspend case.
	 */
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	if (!(dwc->gadget.evp_sts & EVP_STS_DCP)) {
#endif
	if ((dwc->is_drd && mdwc->otg_state == OTG_STATE_B_SUSPEND) &&
		(dwc->gadget.state != USB_STATE_CONFIGURED)) {
		pr_err("%s(): Trying to go in LPM with state:%d\n",
					__func__, dwc->gadget.state);
		pr_err("%s(): LPM is not performed.\n", __func__);
		return -EBUSY;
	}
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	}
#endif
	ret = dwc3_msm_prepare_suspend(mdwc);
	if (ret)
		return ret;

	/* Initialize variables here */
	can_suspend_ssphy = !(mdwc->in_host_mode &&
				dwc3_msm_is_host_superspeed(mdwc));

	/* Disable core irq */
	if (dwc->irq)
		disable_irq(dwc->irq);

	/* Enable wakeup from LPM */
	if (mdwc->pwr_event_irq) {
		disable_irq(mdwc->pwr_event_irq);
		dwc3_msm_wake_interrupt_enable(mdwc, true);
		enable_irq_wake(mdwc->pwr_event_irq);
	}
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	if (dwc->gadget.evp_sts & EVP_STS_EVP)
		dwc3_gadget_disable_irq(dwc);
#endif

	/* Suspend HS PHY */
	usb_phy_set_suspend(mdwc->hs_phy, 1);

	/* Suspend SS PHY */
	if (can_suspend_ssphy) {
		usb_phy_set_suspend(mdwc->ss_phy, 1);
		mdwc->lpm_flags |= MDWC3_SS_PHY_SUSPEND;
	}

	/* make sure above writes are completed before turning off clocks */
	wmb();

	/* Disable clocks */
	if (mdwc->bus_aggr_clk)
		clk_disable_unprepare(mdwc->bus_aggr_clk);
	clk_disable_unprepare(mdwc->utmi_clk);

	clk_set_rate(mdwc->core_clk, 19200000);
	clk_disable_unprepare(mdwc->core_clk);
	/*
	 * Disable iface_clk only after core_clk as core_clk has FSM
	 * depedency on iface_clk. Hence iface_clk should be turned off
	 * after core_clk is turned off.
	 */
	clk_disable_unprepare(mdwc->iface_clk);
	/* USB PHY no more requires TCXO */
#ifdef CONFIG_LGE_PM
	if (lge_get_boot_mode() != LGE_BOOT_MODE_CHARGERLOGO) {
		if (!mdwc->xo_vote_for_charger) {
			clk_disable_unprepare(mdwc->xo_clk);
			dev_err(mdwc->dev, "%s unvote for TCXO buffer\n",
					__func__);
		}
	} else
		clk_disable_unprepare(mdwc->xo_clk);
#else
	clk_disable_unprepare(mdwc->xo_clk);
#endif

	/* Perform controller power collapse */
#if defined(CONFIG_LGE_USB_MAXIM_EVP) && defined(CONFIG_LGE_USB_FLOATED_CHARGER_DETECT)
	if ( (!mdwc->in_host_mode &&
	     (!mdwc->vbus_active || mdwc->after_apsd_rerun || mdwc->alice_friends) &&
	     mdwc->power_collapse) ||
	      ((dwc->gadget.evp_sts & EVP_STS_DCP) &&
	       !(dwc->gadget.evp_sts & EVP_STS_SIMPLE)) ) {
#else
	if (!mdwc->in_host_mode && !mdwc->vbus_active && mdwc->power_collapse) {
#endif
		mdwc->lpm_flags |= MDWC3_POWER_COLLAPSE;
		dev_info(mdwc->dev, "%s: power collapse\n", __func__);
		dwc3_msm_config_gdsc(mdwc, 0);
		clk_disable_unprepare(mdwc->sleep_clk);
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
		if (mdwc->after_apsd_rerun)
			mdwc->after_apsd_rerun = 0;
#endif
	}

	/* Remove bus voting */
	if (mdwc->bus_perf_client) {
		mdwc->bus_vote = 0;
		schedule_work(&mdwc->bus_vote_w);
	}

	/*
	 * release wakeup source with timeout to defer system suspend to
	 * handle case where on USB cable disconnect, SUSPEND and DISCONNECT
	 * event is received.
	 */
	if (mdwc->lpm_to_suspend_delay) {
		dev_dbg(mdwc->dev, "defer suspend with %d(msecs)\n",
					mdwc->lpm_to_suspend_delay);
		pm_wakeup_event(mdwc->dev, mdwc->lpm_to_suspend_delay);
	} else {
		pm_relax(mdwc->dev);
	}

	/*
	 * with DCP or during cable disconnect, we dont require wakeup
	 * using HS_PHY_IRQ. Hence enable wakeup only in case of host
	 * bus suspend and device bus suspend.
	 */
	if (mdwc->hs_phy_irq && (mdwc->vbus_active || mdwc->in_host_mode)) {
		enable_irq_wake(mdwc->hs_phy_irq);
		mdwc->lpm_flags |= MDWC3_ASYNC_IRQ_WAKE_CAPABILITY;
	}

	atomic_set(&dwc->in_lpm, 1);

	if (mdwc->pwr_event_irq)
		enable_irq(mdwc->pwr_event_irq);

	dev_info(mdwc->dev, "DWC3 in low power mode\n");
	return 0;
}

static int dwc3_msm_resume(struct dwc3_msm *mdwc)
{
	int ret;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(mdwc->dev, "%s: exiting lpm\n", __func__);

	if (!atomic_read(&dwc->in_lpm)) {
		dev_dbg(mdwc->dev, "%s: Already resumed\n", __func__);
		return 0;
	}

	pm_stay_awake(mdwc->dev);

	/* Enable bus voting */
	if (mdwc->bus_perf_client) {
		mdwc->bus_vote = 1;
		schedule_work(&mdwc->bus_vote_w);
	}

	/* Vote for TCXO while waking up USB HSPHY */
#ifdef CONFIG_LGE_PM
	if (lge_get_boot_mode() != LGE_BOOT_MODE_CHARGERLOGO) {
		if (!mdwc->xo_vote_for_charger) {
			ret = clk_prepare_enable(mdwc->xo_clk);
			if (ret)
				dev_err(mdwc->dev, "%s failed to vote TCXO buffer%d\n",
						__func__, ret);
			else
				dev_err(mdwc->dev, "%s vote for TCXO buffer\n",
						__func__);
		} else
			dev_err(mdwc->dev, "%s xo_vote_for_charger = %d\n",
					__func__, mdwc->xo_vote_for_charger);
	} else {
		ret = clk_prepare_enable(mdwc->xo_clk);
		if (ret)
			dev_err(mdwc->dev, "%s failed to vote TCXO buffer%d\n",
					__func__, ret);
	}
#else
	ret = clk_prepare_enable(mdwc->xo_clk);
	if (ret)
		dev_err(mdwc->dev, "%s failed to vote TCXO buffer%d\n",
						__func__, ret);
#endif

	/* Restore controller power collapse */
	if (mdwc->lpm_flags & MDWC3_POWER_COLLAPSE) {
		dev_dbg(mdwc->dev, "%s: exit power collapse\n", __func__);
		dwc3_msm_config_gdsc(mdwc, 1);

		clk_reset(mdwc->core_clk, CLK_RESET_ASSERT);
		/* HW requires a short delay for reset to take place properly */
		usleep_range(1000, 1200);
		clk_reset(mdwc->core_clk, CLK_RESET_DEASSERT);
		clk_prepare_enable(mdwc->sleep_clk);
	}

	/* Resume SS PHY */
	if (mdwc->lpm_flags & MDWC3_SS_PHY_SUSPEND) {
		usb_phy_set_suspend(mdwc->ss_phy, 0);
		mdwc->lpm_flags &= ~MDWC3_SS_PHY_SUSPEND;
	}

	/* Resume HS PHY */
	usb_phy_set_suspend(mdwc->hs_phy, 0);

	/*
	 * Enable clocks
	 * Turned ON iface_clk before core_clk due to FSM depedency.
	 */
	clk_prepare_enable(mdwc->iface_clk);
	clk_set_rate(mdwc->core_clk, mdwc->core_clk_rate);
	clk_prepare_enable(mdwc->core_clk);
	clk_prepare_enable(mdwc->utmi_clk);
	if (mdwc->bus_aggr_clk)
		clk_prepare_enable(mdwc->bus_aggr_clk);

	/* Recover from controller power collapse */
	if (mdwc->lpm_flags & MDWC3_POWER_COLLAPSE) {
		dev_dbg(mdwc->dev, "%s: exit power collapse (POR=%d)\n",
			__func__, mdwc->power_collapse_por);

		if (mdwc->power_collapse_por)
			dwc3_msm_power_collapse_por(mdwc);

		/* Re-enable IN_P3 event */
		dwc3_msm_write_reg_field(mdwc->base, PWR_EVNT_IRQ_MASK_REG,
					PWR_EVNT_POWERDOWN_IN_P3_MASK, 1);

		mdwc->lpm_flags &= ~MDWC3_POWER_COLLAPSE;
	}

	atomic_set(&dwc->in_lpm, 0);

	/* disable wakeup from LPM */
	if (mdwc->pwr_event_irq) {
		disable_irq_wake(mdwc->pwr_event_irq);
		dwc3_msm_wake_interrupt_enable(mdwc, false);
	}

	/* Disable HSPHY auto suspend */
	dwc3_msm_write_reg(mdwc->base, DWC3_GUSB2PHYCFG(0),
		dwc3_msm_read_reg(mdwc->base, DWC3_GUSB2PHYCFG(0)) &
				~(DWC3_GUSB2PHYCFG_ENBLSLPM |
					DWC3_GUSB2PHYCFG_SUSPHY));

	/* Disable wakeup capable for HS_PHY IRQ, if enabled */
	if (mdwc->hs_phy_irq &&
			(mdwc->lpm_flags & MDWC3_ASYNC_IRQ_WAKE_CAPABILITY)) {
		disable_irq_wake(mdwc->hs_phy_irq);
		mdwc->lpm_flags &= ~MDWC3_ASYNC_IRQ_WAKE_CAPABILITY;
	}

	dev_info(mdwc->dev, "DWC3 exited from low power mode\n");

	/* Enable core irq */
	if (dwc->irq)
		enable_irq(dwc->irq);

	/*
	 * Handle other power events that could not have been handled during
	 * Low Power Mode
	 */
	dwc3_pwr_event_handler(mdwc);

	dbg_event(0xFF, "Ctl Res", atomic_read(&dwc->in_lpm));

	return 0;
}

/**
 * dwc3_ext_event_notify - callback to handle events from external transceiver
 *
 * Returns 0 on success
 */
static void dwc3_ext_event_notify(struct dwc3_msm *mdwc)
{
#ifdef CONFIG_LGE_USB_G_ANDROID
	pr_info("%s\n", __func__);
#endif

	/* Flush processing any pending events before handling new ones */
	if (mdwc->init)
		flush_delayed_work(&mdwc->sm_work);

	if (mdwc->id_state == DWC3_ID_FLOAT) {
		dev_dbg(mdwc->dev, "XCVR: ID set\n");
		dbg_event(0xFF, "XCVR:ID Set", 1);
		set_bit(ID, &mdwc->inputs);
	} else {
		dev_dbg(mdwc->dev, "XCVR: ID clear\n");
		dbg_event(0xFF, "XCVR:ID Clear", 0);
		clear_bit(ID, &mdwc->inputs);
	}

	if (mdwc->vbus_active && !mdwc->in_restart) {
		dev_dbg(mdwc->dev, "XCVR: BSV set\n");
		set_bit(B_SESS_VLD, &mdwc->inputs);
	} else {
		dev_dbg(mdwc->dev, "XCVR: BSV clear\n");
		clear_bit(B_SESS_VLD, &mdwc->inputs);
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
		if (!mdwc->in_restart) {
			pr_info("%s(): cancel floated_chg_hrtimer\n", __func__);
			hrtimer_cancel(&mdwc->floated_chg_hrtimer);
		}
		cancel_delayed_work(&mdwc->floated_chg_work);
		mdwc->usb_psy.is_floated_charger = 0;
		power_supply_changed(&mdwc->usb_psy);
		if (mdwc->apsd_rerun_need) {
			mdwc->apsd_rerun_need = 0;
			mdwc->after_apsd_rerun = 1;
		}
#endif
	}

	if (mdwc->suspend) {
		dev_dbg(mdwc->dev, "XCVR: SUSP set\n");
		set_bit(B_SUSPEND, &mdwc->inputs);
	} else {
		dev_dbg(mdwc->dev, "XCVR: SUSP clear\n");
		clear_bit(B_SUSPEND, &mdwc->inputs);
	}

	if (!mdwc->init) {
		mdwc->init = true;
		pm_runtime_set_autosuspend_delay(mdwc->dev, 1000);
		pm_runtime_use_autosuspend(mdwc->dev);
		if (!work_busy(&mdwc->sm_work.work))
			schedule_delayed_work(&mdwc->sm_work, 0);

		complete(&mdwc->dwc3_xcvr_vbus_init);
		dev_dbg(mdwc->dev, "XCVR: BSV init complete\n");
		return;
	}


	schedule_delayed_work(&mdwc->sm_work, 0);
}

static void dwc3_resume_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm,
							resume_work.work);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_info(mdwc->dev, "%s: dwc3 resume work\n", __func__);

	/*
	 * exit LPM first to meet resume timeline from device side.
	 * resume_pending flag would prevent calling
	 * dwc3_msm_resume() in case we are here due to system
	 * wide resume without usb cable connected. This flag is set
	 * only in case of power event irq in lpm.
	 */
	if (mdwc->resume_pending) {
		dwc3_msm_resume(mdwc);
		mdwc->resume_pending = false;
	}

	if (atomic_read(&mdwc->pm_suspended)) {
		dbg_event(0xFF, "RWrk PMSus", 0);
		/* let pm resume kick in resume work later */
		return;
	}

	dbg_event(0xFF, "RWrk", dwc->is_drd);
	dwc3_ext_event_notify(mdwc);
}

static void dwc3_pwr_event_handler(struct dwc3_msm *mdwc)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	u32 irq_stat, irq_clear = 0;

	irq_stat = dwc3_msm_read_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG);
	dev_dbg(mdwc->dev, "%s irq_stat=%X\n", __func__, irq_stat);

	/* Check for P3 events */
	if ((irq_stat & PWR_EVNT_POWERDOWN_OUT_P3_MASK) &&
			(irq_stat & PWR_EVNT_POWERDOWN_IN_P3_MASK)) {
		/* Can't tell if entered or exit P3, so check LINKSTATE */
		u32 ls = dwc3_msm_read_reg_field(mdwc->base,
				DWC3_GDBGLTSSM, DWC3_GDBGLTSSM_LINKSTATE_MASK);
		dev_dbg(mdwc->dev, "%s link state = 0x%04x\n", __func__, ls);
		atomic_set(&mdwc->in_p3, ls == DWC3_LINK_STATE_U3);

		irq_stat &= ~(PWR_EVNT_POWERDOWN_OUT_P3_MASK |
				PWR_EVNT_POWERDOWN_IN_P3_MASK);
		irq_clear |= (PWR_EVNT_POWERDOWN_OUT_P3_MASK |
				PWR_EVNT_POWERDOWN_IN_P3_MASK);
	} else if (irq_stat & PWR_EVNT_POWERDOWN_OUT_P3_MASK) {
		atomic_set(&mdwc->in_p3, 0);
		irq_stat &= ~PWR_EVNT_POWERDOWN_OUT_P3_MASK;
		irq_clear |= PWR_EVNT_POWERDOWN_OUT_P3_MASK;
	} else if (irq_stat & PWR_EVNT_POWERDOWN_IN_P3_MASK) {
		atomic_set(&mdwc->in_p3, 1);
		irq_stat &= ~PWR_EVNT_POWERDOWN_IN_P3_MASK;
		irq_clear |= PWR_EVNT_POWERDOWN_IN_P3_MASK;
	}

	/* Clear L2 exit */
	if (irq_stat & PWR_EVNT_LPM_OUT_L2_MASK) {
		irq_stat &= ~PWR_EVNT_LPM_OUT_L2_MASK;
#ifdef CONFIG_LGE_USB_G_ANDROID
		irq_clear |= PWR_EVNT_LPM_OUT_L2_MASK;
#else
		irq_stat |= PWR_EVNT_LPM_OUT_L2_MASK;
#endif
	}

	/* Handle exit from L1 events */
	if (irq_stat & PWR_EVNT_LPM_OUT_L1_MASK) {
		dev_dbg(mdwc->dev, "%s: handling PWR_EVNT_LPM_OUT_L1_MASK\n",
				__func__);
		if (usb_gadget_wakeup(&dwc->gadget))
			dev_err(mdwc->dev, "%s failed to take dwc out of L1\n",
					__func__);
		irq_stat &= ~PWR_EVNT_LPM_OUT_L1_MASK;
		irq_clear |= PWR_EVNT_LPM_OUT_L1_MASK;
	}

	/* Unhandled events */
	if (irq_stat)
		dev_dbg(mdwc->dev, "%s: unexpected PWR_EVNT, irq_stat=%X\n",
			__func__, irq_stat);

	dwc3_msm_write_reg(mdwc->base, PWR_EVNT_IRQ_STAT_REG, irq_clear);
}

static irqreturn_t msm_dwc3_pwr_irq_thread(int irq, void *_mdwc)
{
	struct dwc3_msm *mdwc = _mdwc;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(mdwc->dev, "%s\n", __func__);

	if (atomic_read(&dwc->in_lpm))
		dwc3_resume_work(&mdwc->resume_work.work);
	else
		dwc3_pwr_event_handler(mdwc);

	dbg_event(0xFF, "PWR IRQ", atomic_read(&dwc->in_lpm));

	return IRQ_HANDLED;
}

static irqreturn_t msm_dwc3_hs_phy_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;

	dev_dbg(mdwc->dev, "%s HS PHY IRQ handled\n", __func__);

	return IRQ_HANDLED;
}
static irqreturn_t msm_dwc3_pwr_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dwc->t_pwr_evt_irq = ktime_get();
	dev_dbg(mdwc->dev, "%s received\n", __func__);
	/*
	 * When in Low Power Mode, can't read PWR_EVNT_IRQ_STAT_REG to acertain
	 * which interrupts have been triggered, as the clocks are disabled.
	 * Resume controller by waking up pwr event irq thread.After re-enabling
	 * clocks, dwc3_msm_resume will call dwc3_pwr_event_handler to handle
	 * all other power events.
	 */
	if (atomic_read(&dwc->in_lpm)) {
		/* set this to call dwc3_msm_resume() */
		mdwc->resume_pending = true;
		return IRQ_WAKE_THREAD;
	}

	dwc3_pwr_event_handler(mdwc);
	return IRQ_HANDLED;
}

#ifdef CONFIG_LGE_PM_CABLE_DETECTION
static int
get_prop_usbin_voltage_now(struct dwc3_msm *mdwc)
{
	int rc = 0;
	struct qpnp_vadc_result results;

	if (IS_ERR_OR_NULL(mdwc->vadc_dev)) {
		mdwc->vadc_dev = qpnp_get_vadc(mdwc->dev, "usbin");
		if (IS_ERR(mdwc->vadc_dev))
			return PTR_ERR(mdwc->vadc_dev);
	}

	rc = qpnp_vadc_read(mdwc->vadc_dev, USBIN, &results);
	if (rc) {
		pr_err("Unable to read usbin rc=%d\n", rc);
		return 0;
	} else {
		return results.physical;
	}
}
#endif

#if defined CONFIG_LGE_USB_MAXIM_EVP && defined CONFIG_LGE_PM_MAXIM_EVP_CONTROL
static int dwc3_msm_gadget_func_io(struct power_supply *psy,
					int *val, bool rw)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	if (!dwc || !dwc->gadget.ops) {
		dev_err(mdwc->dev, "%s: dwc or usb gadget driver data get failed.\n", __func__);
		return -EINVAL;
	}

	if (dwc->gadget.ops->gadget_func_io)
		dwc->gadget.ops->gadget_func_io(&dwc->gadget, "evp", val, rw);
	else
		pr_err("%s gadget_func_io is not exists in gadget.\n", __func__);

	return 0;
}
#endif

static int dwc3_msm_power_get_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  union power_supply_propval *val)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);
	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		val->intval = mdwc->voltage_max;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		val->intval = mdwc->current_max;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
		if (mdwc->usb_psy.is_floated_charger) {
			val->intval = 1;
		}
		else
#endif
		val->intval = mdwc->vbus_active;
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		val->intval = mdwc->online;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		val->intval = psy->type;
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		val->intval = mdwc->health_status;
		break;
	case POWER_SUPPLY_PROP_USB_OTG:
		val->intval = !mdwc->id_state;
		break;
#if defined CONFIG_LGE_USB_MAXIM_EVP
#if defined CONFIG_LGE_PM_MAXIM_EVP_CONTROL
	case POWER_SUPPLY_PROP_EVP_VOL:
		dwc3_msm_gadget_func_io(psy, &val->intval, false);
		break;
#endif
	case POWER_SUPPLY_PROP_HVDCP_TYPE:
		val->intval = (int)mdwc->evp_sts;
		break;
#endif
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
	case POWER_SUPPLY_PROP_APSD_RERUN_NEED:
		val->intval = mdwc->apsd_rerun_need;
		break;
#endif
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		pr_info("%s: POWER_SUPPLY_PROP_VOLTAGE_NOW\n", __func__);
		val->intval = get_prop_usbin_voltage_now(mdwc);
		break;
#endif
#ifdef CONFIG_LGE_USB_TYPE_C
	case POWER_SUPPLY_PROP_DP_DM:
		val->intval = mdwc->dp_dm;
		break;
#endif

	default:
		return -EINVAL;
	}
	return 0;
}

static int dwc3_msm_power_set_property_usb(struct power_supply *psy,
				  enum power_supply_property psp,
				  const union power_supply_propval *val)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm,
								usb_psy);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
#if defined CONFIG_LGE_USB_MAXIM_EVP && defined CONFIG_LGE_PM_MAXIM_EVP_CONTROL
	static int evp_vol;
#endif

	switch (psp) {
	case POWER_SUPPLY_PROP_USB_OTG:
		/* Let OTG know about ID detection */
		mdwc->id_state = val->intval ? DWC3_ID_GROUND : DWC3_ID_FLOAT;
		dbg_event(0xFF, "id_state", mdwc->id_state);
		if (dwc->is_drd && !mdwc->ext_inuse)
#ifdef CONFIG_LGE_USB_G_ANDROID
		{
			cancel_delayed_work(&mdwc->resume_work);
			queue_delayed_work(mdwc->dwc3_wq,
					&mdwc->resume_work, 12);
		}
#else
			queue_delayed_work(mdwc->dwc3_wq,
					&mdwc->resume_work, 0);
#endif

		break;
	/* PMIC notification for DP_DM state */
	case POWER_SUPPLY_PROP_DP_DM:
#ifdef CONFIG_LGE_USB_TYPE_C
		mdwc->dp_dm = val->intval;
#endif
		usb_phy_change_dpdm(mdwc->hs_phy, val->intval);
		break;
	/* Process PMIC notification in PRESENT prop */
	case POWER_SUPPLY_PROP_PRESENT:
#ifdef CONFIG_LGE_USB_TYPE_C
		if (val->intval && psy->type == POWER_SUPPLY_TYPE_UNKNOWN) {
			mdwc->vbus_active_pending = true;
			break;
		} else {
			mdwc->vbus_active_pending = false;
		}
#endif

#ifdef CONFIG_LGE_USB_G_ANDROID
		dev_info(mdwc->dev, "%s: notify xceiv event with val:%d, vbus_active:%d\n",
				__func__, val->intval, mdwc->vbus_active);
#else
		dev_dbg(mdwc->dev, "%s: notify xceiv event with val:%d\n",
							__func__, val->intval);
#endif
#ifdef CONFIG_LGE_USB_MAXIM_EVP
		atomic_set(&mdwc->evp_detecting, 0);
#endif
		/*
		 * Now otg_sm_work() state machine waits for USB cable status.
		 * Hence here it makes sure that schedule resume work only if
		 * there is change in USB cable also if there is no USB cable
		 * notification.
		 */
		if (mdwc->otg_state == OTG_STATE_UNDEFINED) {
			mdwc->vbus_active = val->intval;
			dwc3_ext_event_notify(mdwc);
			break;
		}

		if (mdwc->vbus_active == val->intval)
			break;

		mdwc->vbus_active = val->intval;
		if (dwc->is_drd && !mdwc->ext_inuse && !mdwc->in_restart) {
			/*
			 * Set debouncing delay to 120ms. Otherwise battery
			 * charging CDP complaince test fails if delay > 120ms.
			 */
			dbg_event(0xFF, "Q RW (vbus)", val->intval);
#ifdef CONFIG_LGE_USB_G_ANDROID
			cancel_delayed_work(&mdwc->resume_work);
			{
			int rc = queue_delayed_work(mdwc->dwc3_wq,
					&mdwc->resume_work, 12);
			dev_info(mdwc->dev, "%s: Q RW return %d\n", __func__, rc);
			dbg_event(0xFF, "Q RW (rc)", rc);
			}
#else
			queue_delayed_work(mdwc->dwc3_wq,
					&mdwc->resume_work, 12);
#endif
		}
		break;
	case POWER_SUPPLY_PROP_ONLINE:
		mdwc->online = val->intval;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		mdwc->voltage_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_CURRENT_MAX:
		mdwc->current_max = val->intval;
		break;
	case POWER_SUPPLY_PROP_TYPE:
		psy->type = val->intval;

		switch (psy->type) {
		case POWER_SUPPLY_TYPE_USB:
			mdwc->chg_type = DWC3_SDP_CHARGER;
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
			if (!lge_get_factory_boot() && mdwc->id_state != DWC3_ID_GROUND) {
				if (apsd_retried) {
					pr_info("%s(): floated chg detected finally\n", __func__);
					mdwc->usb_psy.is_floated_charger = 1;
					power_supply_set_current_limit(&mdwc->usb_psy,
							FLOATED_CHG_DEFAULT_CURRENT);
					power_supply_changed(&mdwc->usb_psy);
					apsd_retried = false;
				} else {
					pr_info("%s(): start floated_chg_hrtimer\n", __func__);
					hrtimer_cancel(&mdwc->floated_chg_hrtimer);
					hrtimer_start(&mdwc->floated_chg_hrtimer,
							ktime_set(6, 0),
							HRTIMER_MODE_REL);
				}
			} else {
				pr_info("%s: factory or otg cable connected. do not start floated_chg_hrtimer\n", __func__);
			}
#endif
			break;
		case POWER_SUPPLY_TYPE_USB_DCP:
			mdwc->chg_type = DWC3_DCP_CHARGER;
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
			if (psy->is_floated_charger == 1) {
				psy->is_floated_charger = 0;
				power_supply_changed(&mdwc->usb_psy);
			}
			apsd_retried = false;
#endif
			break;
		case POWER_SUPPLY_TYPE_USB_HVDCP:
			mdwc->chg_type = DWC3_DCP_CHARGER;
			dwc3_msm_gadget_vbus_draw(mdwc, hvdcp_max_current);
			break;
		case POWER_SUPPLY_TYPE_USB_CDP:
			mdwc->chg_type = DWC3_CDP_CHARGER;
			break;
		case POWER_SUPPLY_TYPE_USB_ACA:
			mdwc->chg_type = DWC3_PROPRIETARY_CHARGER;
			break;
		default:
			mdwc->chg_type = DWC3_INVALID_CHARGER;
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
			if (!mdwc->vbus_active) {
				pr_info("%s(): cancel floated_chg_hrtimer\n", __func__);
				hrtimer_cancel(&mdwc->floated_chg_hrtimer);
				cancel_delayed_work(&mdwc->floated_chg_work);
				psy->is_floated_charger = 0;
				power_supply_changed(&mdwc->usb_psy);
			}
#endif
			break;
		}

		if (mdwc->chg_type != DWC3_INVALID_CHARGER)
			mdwc->chg_state = USB_CHG_STATE_DETECTED;

		dev_info(mdwc->dev, "%s: charger type: %s\n", __func__,
				chg_to_string(mdwc->chg_type));
#ifdef CONFIG_LGE_USB_TYPE_C
		if (mdwc->vbus_active_pending &&
		    mdwc->chg_type != DWC3_INVALID_CHARGER)
			power_supply_set_present(psy, true);
#endif
		break;
	case POWER_SUPPLY_PROP_HEALTH:
		mdwc->health_status = val->intval;
		break;
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	case POWER_SUPPLY_PROP_EVP_DETECT_START:
		mdwc->evp_detect = val->intval;
		if (atomic_read(&mdwc->evp_detecting)) {
			dev_info(mdwc->dev, "%s: EVP detection already started.\n", __func__);
			return 0;
		}
		atomic_set(&mdwc->evp_detecting, 1);
		dev_dbg(mdwc->dev, "%s: EVP detection start? %d\n", __func__, val->intval);
		dwc3_evp_event_notify(mdwc);
		break;
#endif
#if defined CONFIG_LGE_USB_MAXIM_EVP && defined CONFIG_LGE_PM_MAXIM_EVP_CONTROL
	case POWER_SUPPLY_PROP_EVP_VOL:
		evp_vol = val->intval;
		dwc3_msm_gadget_func_io(psy, &evp_vol, true);
		break;
#endif
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
	case POWER_SUPPLY_PROP_APSD_RERUN_NEED:
		mdwc->apsd_rerun_need = val->intval;
		break;
#endif
	default:
		return -EINVAL;
	}

	power_supply_changed(&mdwc->usb_psy);
	return 0;
}

static void dwc3_msm_external_power_changed(struct power_supply *psy)
{
	struct dwc3_msm *mdwc = container_of(psy, struct dwc3_msm, usb_psy);
	union power_supply_propval ret = {0,};

	if (!mdwc->ext_vbus_psy)
		mdwc->ext_vbus_psy = power_supply_get_by_name("ext-vbus");

	if (!mdwc->ext_vbus_psy) {
		pr_err("%s: Unable to get ext_vbus power_supply\n", __func__);
		return;
	}

	mdwc->ext_vbus_psy->get_property(mdwc->ext_vbus_psy,
					POWER_SUPPLY_PROP_ONLINE, &ret);
	if (ret.intval) {
		mdwc->ext_vbus_psy->get_property(mdwc->ext_vbus_psy,
					POWER_SUPPLY_PROP_CURRENT_MAX, &ret);
		power_supply_set_current_limit(&mdwc->usb_psy, ret.intval);
	}

	power_supply_set_online(&mdwc->usb_psy, ret.intval);
	power_supply_changed(&mdwc->usb_psy);
}

static int
dwc3_msm_property_is_writeable(struct power_supply *psy,
				enum power_supply_property psp)
{
	switch (psp) {
	case POWER_SUPPLY_PROP_USB_OTG:
	case POWER_SUPPLY_PROP_PRESENT:
	case POWER_SUPPLY_PROP_VOLTAGE_MAX:
		return 1;
	default:
		break;
	}

	return 0;
}


static char *dwc3_msm_pm_power_supplied_to[] = {
	"battery",
	"bms",
};

static enum power_supply_property dwc3_msm_pm_power_props_usb[] = {
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_ONLINE,
	POWER_SUPPLY_PROP_VOLTAGE_MAX,
	POWER_SUPPLY_PROP_CURRENT_MAX,
	POWER_SUPPLY_PROP_TYPE,
	POWER_SUPPLY_PROP_HEALTH,
	POWER_SUPPLY_PROP_USB_OTG,
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	POWER_SUPPLY_PROP_HVDCP_TYPE,
#endif
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
	POWER_SUPPLY_PROP_APSD_RERUN_NEED,
#endif
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
#endif
};

static void dwc3_ext_notify_online(void *ctx, int on)
{
	struct dwc3_msm *mdwc = ctx;

	if (!mdwc) {
		pr_err("%s: DWC3 driver already removed\n", __func__);
		return;
	}

	dev_dbg(mdwc->dev, "notify %s%s\n", on ? "" : "dis", "connected");

	if (!mdwc->ext_vbus_psy)
		mdwc->ext_vbus_psy = power_supply_get_by_name("ext-vbus");

	mdwc->ext_inuse = on;
	if (on)
		/* force OTG to exit B-peripheral state */
		mdwc->vbus_active = false;

	if (mdwc->ext_vbus_psy)
		power_supply_set_present(mdwc->ext_vbus_psy, on);

#ifdef CONFIG_LGE_USB_G_ANDROID
	cancel_delayed_work(&mdwc->resume_work);
#endif
	queue_delayed_work(mdwc->dwc3_wq, &mdwc->resume_work, 0);
}

static void dwc3_id_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, id_work);
	int ret;

	/* Give external client a chance to handle */
	if (!mdwc->ext_inuse && usb_ext) {
		if (mdwc->pmic_id_irq)
			disable_irq(mdwc->pmic_id_irq);

		ret = usb_ext->notify(usb_ext->ctxt, mdwc->id_state,
				      dwc3_ext_notify_online, mdwc);
		dev_dbg(mdwc->dev, "%s: external handler returned %d\n",
			__func__, ret);

		if (mdwc->pmic_id_irq) {
			unsigned long flags;
			local_irq_save(flags);
			/* ID may have changed while IRQ disabled; update it */
			mdwc->id_state = !!irq_read_line(mdwc->pmic_id_irq);
			local_irq_restore(flags);
			enable_irq(mdwc->pmic_id_irq);
		}

		mdwc->ext_inuse = (ret == 0);
	}

	if (!mdwc->ext_inuse)
		dwc3_resume_work(&mdwc->resume_work.work);

	dbg_event(0xFF, "RW (id)", 0);
}

static irqreturn_t dwc3_pmic_id_irq(int irq, void *data)
{
	struct dwc3_msm *mdwc = data;
	enum dwc3_id_state id;

	/* If we can't read ID line state for some reason, treat it as float */
	id = !!irq_read_line(irq);
	if (mdwc->id_state != id) {
		mdwc->id_state = id;
		schedule_work(&mdwc->id_work);
	}

	return IRQ_HANDLED;
}

static int dwc3_cpu_notifier_cb(struct notifier_block *nfb,
		unsigned long action, void *hcpu)
{
	uint32_t cpu = (uintptr_t)hcpu;
	struct dwc3_msm *mdwc =
			container_of(nfb, struct dwc3_msm, dwc3_cpu_notifier);

	if (cpu == cpu_to_affin && action == CPU_ONLINE) {
		pr_debug("%s: cpu online:%u irq:%d\n", __func__,
				cpu_to_affin, mdwc->irq_to_affin);
		irq_set_affinity(mdwc->irq_to_affin, get_cpu_mask(cpu));
	}

	return NOTIFY_OK;
}

static void dwc3_otg_sm_work(struct work_struct *w);

static int dwc3_msm_get_clk_gdsc(struct dwc3_msm *mdwc)
{
	int ret;

	mdwc->dwc3_gdsc = devm_regulator_get(mdwc->dev, "USB3_GDSC");
	if (IS_ERR(mdwc->dwc3_gdsc))
		mdwc->dwc3_gdsc = NULL;

	mdwc->xo_clk = devm_clk_get(mdwc->dev, "xo");
	if (IS_ERR(mdwc->xo_clk)) {
		dev_err(mdwc->dev, "%s unable to get TCXO buffer handle\n",
								__func__);
		ret = PTR_ERR(mdwc->xo_clk);
		return ret;
	}
	clk_set_rate(mdwc->xo_clk, 19200000);

	mdwc->iface_clk = devm_clk_get(mdwc->dev, "iface_clk");
	if (IS_ERR(mdwc->iface_clk)) {
		dev_err(mdwc->dev, "failed to get iface_clk\n");
		ret = PTR_ERR(mdwc->iface_clk);
		return ret;
	}

	/*
	 * DWC3 Core requires its CORE CLK (aka master / bus clk) to
	 * run at 125Mhz in SSUSB mode and >60MHZ for HSUSB mode.
	 * On newer platform it can run at 150MHz as well.
	 */
	mdwc->core_clk = devm_clk_get(mdwc->dev, "core_clk");
	if (IS_ERR(mdwc->core_clk)) {
		dev_err(mdwc->dev, "failed to get core_clk\n");
		ret = PTR_ERR(mdwc->core_clk);
		return ret;
	}

	/*
	 * Get Max supported clk frequency for USB Core CLK and request
	 * to set the same.
	 */
	mdwc->core_clk_rate = clk_round_rate(mdwc->core_clk, LONG_MAX);
	if (IS_ERR_VALUE(mdwc->core_clk_rate)) {
		dev_err(mdwc->dev, "fail to get core clk max freq.\n");
	} else {
		ret = clk_set_rate(mdwc->core_clk, mdwc->core_clk_rate);
		if (ret)
			dev_err(mdwc->dev, "fail to set core_clk freq:%d\n",
									ret);
	}

	mdwc->sleep_clk = devm_clk_get(mdwc->dev, "sleep_clk");
	if (IS_ERR(mdwc->sleep_clk)) {
		dev_err(mdwc->dev, "failed to get sleep_clk\n");
		ret = PTR_ERR(mdwc->sleep_clk);
		return ret;
	}

	clk_set_rate(mdwc->sleep_clk, 32000);
	mdwc->utmi_clk_rate = 19200000;
	mdwc->utmi_clk = devm_clk_get(mdwc->dev, "utmi_clk");
	if (IS_ERR(mdwc->utmi_clk)) {
		dev_err(mdwc->dev, "failed to get utmi_clk\n");
		ret = PTR_ERR(mdwc->utmi_clk);
		return ret;
	}

	clk_set_rate(mdwc->utmi_clk, mdwc->utmi_clk_rate);
	mdwc->bus_aggr_clk = devm_clk_get(mdwc->dev, "bus_aggr_clk");
	if (IS_ERR(mdwc->bus_aggr_clk))
		mdwc->bus_aggr_clk = NULL;

	mdwc->cfg_ahb_clk = devm_clk_get(mdwc->dev, "cfg_ahb_clk");
	if (IS_ERR(mdwc->cfg_ahb_clk)) {
		dev_err(mdwc->dev, "failed to get cfg_ahb_clk\n");
		ret = PTR_ERR(mdwc->cfg_ahb_clk);
		return ret;
	}

	return 0;
}

static int dwc3_msm_probe(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node, *dwc3_node;
	struct device	*dev = &pdev->dev;
	struct dwc3_msm *mdwc;
	struct dwc3	*dwc;
	struct resource *res;
	void __iomem *tcsr;
	unsigned long flags;
	bool host_mode;
	int ret = 0;
	int ext_hub_reset_gpio;
	u32 val;

	mdwc = devm_kzalloc(&pdev->dev, sizeof(*mdwc), GFP_KERNEL);
	if (!mdwc)
		return -ENOMEM;

	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64))) {
		dev_err(&pdev->dev, "setting DMA mask to 64 failed.\n");
		if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32))) {
			dev_err(&pdev->dev, "setting DMA mask to 32 failed.\n");
			return -EOPNOTSUPP;
		}
	}

	platform_set_drvdata(pdev, mdwc);
	mdwc->dev = &pdev->dev;

	INIT_LIST_HEAD(&mdwc->req_complete_list);
	INIT_DELAYED_WORK(&mdwc->resume_work, dwc3_resume_work);
	INIT_WORK(&mdwc->restart_usb_work, dwc3_restart_usb_work);
	INIT_WORK(&mdwc->id_work, dwc3_id_work);
	INIT_WORK(&mdwc->bus_vote_w, dwc3_msm_bus_vote_w);
	init_completion(&mdwc->dwc3_xcvr_vbus_init);
	INIT_DELAYED_WORK(&mdwc->sm_work, dwc3_otg_sm_work);
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	INIT_DELAYED_WORK(&mdwc->cable_adc_work, dwc3_cable_adc_work);
#endif
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	INIT_DELAYED_WORK(&mdwc->evp_connect_work, dwc3_otg_evp_connect_work);
#endif
#ifdef CONFIG_LGE_USB_FLOATED_CHARGER_DETECT
	INIT_DELAYED_WORK(&mdwc->floated_chg_work, dwc3_floated_chg_work);

	hrtimer_init(&mdwc->floated_chg_hrtimer, CLOCK_MONOTONIC, HRTIMER_MODE_ABS);
	mdwc->floated_chg_hrtimer.function = floated_chg_hrtimer_func;
#endif

#ifdef CONFIG_LGE_ALICE_FRIENDS
	if (lge_get_alice_friends() != LGE_ALICE_FRIENDS_NONE)
		mdwc->alice_friends = 1;
	else
		mdwc->alice_friends = 0;
#endif

	mdwc->dwc3_wq = alloc_ordered_workqueue("dwc3_wq", 0);
	if (!mdwc->dwc3_wq) {
		pr_err("%s: Unable to create workqueue dwc3_wq\n", __func__);
		return -ENOMEM;
	}

	/* Get all clks and gdsc reference */
	ret = dwc3_msm_get_clk_gdsc(mdwc);
	if (ret) {
		dev_err(&pdev->dev, "error getting clock or gdsc.\n");
		return ret;
	}

	mdwc->id_state = DWC3_ID_FLOAT;
	mdwc->charging_disabled = of_property_read_bool(node,
				"qcom,charging-disabled");

	mdwc->power_collapse_por = of_property_read_bool(node,
		"qcom,por-after-power-collapse");

	mdwc->power_collapse = of_property_read_bool(node,
		"qcom,power-collapse-on-cable-disconnect");

	dev_dbg(&pdev->dev, "power collapse=%d, POR=%d\n",
		mdwc->power_collapse, mdwc->power_collapse_por);

	ret = of_property_read_u32(node, "qcom,lpm-to-suspend-delay-ms",
				&mdwc->lpm_to_suspend_delay);
	if (ret) {
		dev_dbg(&pdev->dev, "setting lpm_to_suspend_delay to zero.\n");
		mdwc->lpm_to_suspend_delay = 0;
	}

	/*
	 * DWC3 has separate IRQ line for OTG events (ID/BSV) and for
	 * DP and DM linestate transitions during low power mode.
	 */
	mdwc->hs_phy_irq = platform_get_irq_byname(pdev, "hs_phy_irq");
	if (mdwc->hs_phy_irq < 0) {
		dev_err(&pdev->dev, "pget_irq for hs_phy_irq failed\n");
		ret = -EINVAL;
		goto err;
	} else {
		irq_set_status_flags(mdwc->hs_phy_irq, IRQ_NOAUTOEN);
		ret = devm_request_irq(&pdev->dev, mdwc->hs_phy_irq,
				msm_dwc3_hs_phy_irq, IRQF_TRIGGER_RISING,
			       "msm_hs_phy_irq", mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq HSPHYINT failed\n");
			goto err;
		}
	}

	/*
	 * Some platforms have a special interrupt line for indicating resume
	 * while in low power mode, when clocks are disabled.
	 */
	mdwc->pwr_event_irq = platform_get_irq_byname(pdev, "pwr_event_irq");
	if (mdwc->pwr_event_irq < 0) {
		dev_err(&pdev->dev, "pget_irq for pwr_event_irq failed\n");
		ret = -EINVAL;
		goto err;
	} else {
		/*
		 * enable pwr event irq early during PM resume to meet bus
		 * resume timeline from usb device
		 */
		ret = devm_request_threaded_irq(&pdev->dev, mdwc->pwr_event_irq,
					msm_dwc3_pwr_irq,
					msm_dwc3_pwr_irq_thread,
					IRQF_TRIGGER_RISING | IRQF_EARLY_RESUME,
					"msm_dwc3", mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq pwr_event_irq failed: %d\n",
					ret);
			goto err;
		}
	}

	mdwc->pmic_id_irq = platform_get_irq_byname(pdev, "pmic_id_irq");
	if (mdwc->pmic_id_irq > 0) {
		irq_set_status_flags(mdwc->pmic_id_irq, IRQ_NOAUTOEN);
		ret = devm_request_irq(&pdev->dev,
				       mdwc->pmic_id_irq,
				       dwc3_pmic_id_irq,
				       IRQF_TRIGGER_RISING |
				       IRQF_TRIGGER_FALLING,
				       "dwc3_msm_pmic_id",
				       mdwc);
		if (ret) {
			dev_err(&pdev->dev, "irqreq IDINT failed\n");
			goto err;
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tcsr_base");
	if (!res) {
		dev_dbg(&pdev->dev, "missing TCSR memory resource\n");
	} else {
		tcsr = devm_ioremap_nocache(&pdev->dev, res->start,
			resource_size(res));
		if (IS_ERR_OR_NULL(tcsr)) {
			dev_dbg(&pdev->dev, "tcsr ioremap failed\n");
		} else {
			/* Enable USB3 on the primary USB port. */
			writel_relaxed(0x1, tcsr);
			/*
			 * Ensure that TCSR write is completed before
			 * USB registers initialization.
			 */
			mb();
		}
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "core_base");
	if (!res) {
		dev_err(&pdev->dev, "missing memory base resource\n");
		ret = -ENODEV;
		goto err;
	}

	mdwc->base = devm_ioremap_nocache(&pdev->dev, res->start,
			resource_size(res));
	if (!mdwc->base) {
		dev_err(&pdev->dev, "ioremap failed\n");
		ret = -ENODEV;
		goto err;
	}

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
							"ahb2phy_base");
	if (res) {
		mdwc->ahb2phy_base = devm_ioremap_nocache(&pdev->dev,
					res->start, resource_size(res));
		if (IS_ERR_OR_NULL(mdwc->ahb2phy_base)) {
			dev_err(dev, "couldn't find ahb2phy_base addr.\n");
			mdwc->ahb2phy_base = NULL;
		} else {
			clk_prepare_enable(mdwc->cfg_ahb_clk);
			/* Configure AHB2PHY for one wait state read/write*/
			val = readl_relaxed(mdwc->ahb2phy_base +
					PERIPH_SS_AHB2PHY_TOP_CFG);
			if (val != ONE_READ_WRITE_WAIT) {
				writel_relaxed(ONE_READ_WRITE_WAIT,
					mdwc->ahb2phy_base +
					PERIPH_SS_AHB2PHY_TOP_CFG);
				/* complete above write before using USB PHY */
				mb();
			}
			clk_disable_unprepare(mdwc->cfg_ahb_clk);
		}
	}

	if (of_get_property(pdev->dev.of_node, "qcom,usb-dbm", NULL)) {
		mdwc->dbm = usb_get_dbm_by_phandle(&pdev->dev, "qcom,usb-dbm",
						   0);
		if (IS_ERR(mdwc->dbm)) {
			dev_err(&pdev->dev, "unable to get dbm device\n");
			ret = -EPROBE_DEFER;
			goto err;
		}
		/*
		 * Add power event if the dbm indicates coming out of L1
		 * by interrupt
		 */
		if (dbm_l1_lpm_interrupt(mdwc->dbm)) {
			if (!mdwc->pwr_event_irq) {
				dev_err(&pdev->dev,
					"need pwr_event_irq exiting L1\n");
				ret = -EINVAL;
				goto err;
			}
		}
	}

	ext_hub_reset_gpio = of_get_named_gpio(node,
					"qcom,ext-hub-reset-gpio", 0);

	if (gpio_is_valid(ext_hub_reset_gpio)
		&& (!devm_gpio_request(&pdev->dev, ext_hub_reset_gpio,
			"qcom,ext-hub-reset-gpio"))) {
		/* reset external hub */
		gpio_direction_output(ext_hub_reset_gpio, 1);
		/*
		 * Hub reset should be asserted for minimum 5microsec
		 * before deasserting.
		 */
		usleep_range(5, 1000);
		gpio_direction_output(ext_hub_reset_gpio, 0);
	}

	if (of_property_read_u32(node, "qcom,dwc-usb3-msm-tx-fifo-size",
				 &mdwc->tx_fifo_size))
		dev_err(&pdev->dev,
			"unable to read platform data tx fifo size\n");

	mdwc->disable_host_mode_pm = of_property_read_bool(node,
				"qcom,disable-host-mode-pm");

	dwc3_set_notifier(&dwc3_msm_notify_event);

	/* Assumes dwc3 is the only DT child of dwc3-msm */
	dwc3_node = of_get_next_available_child(node, NULL);
	if (!dwc3_node) {
		dev_err(&pdev->dev, "failed to find dwc3 child\n");
		ret = -ENODEV;
		goto err;
	}

	host_mode = of_usb_get_dr_mode(dwc3_node) == USB_DR_MODE_HOST;
	/* usb_psy required only for vbus_notifications */
	if (!host_mode) {
		mdwc->usb_psy.name = "usb";
		mdwc->usb_psy.type = POWER_SUPPLY_TYPE_USB;
		mdwc->usb_psy.supplied_to = dwc3_msm_pm_power_supplied_to;
		mdwc->usb_psy.num_supplicants = ARRAY_SIZE(
						dwc3_msm_pm_power_supplied_to);
		mdwc->usb_psy.properties = dwc3_msm_pm_power_props_usb;
		mdwc->usb_psy.num_properties =
					ARRAY_SIZE(dwc3_msm_pm_power_props_usb);
		mdwc->usb_psy.get_property = dwc3_msm_power_get_property_usb;
		mdwc->usb_psy.set_property = dwc3_msm_power_set_property_usb;
		mdwc->usb_psy.external_power_changed =
					dwc3_msm_external_power_changed;
		mdwc->usb_psy.property_is_writeable =
				dwc3_msm_property_is_writeable;

		ret = power_supply_register(&pdev->dev, &mdwc->usb_psy);
		if (ret < 0) {
			dev_err(&pdev->dev,
					"%s:power_supply_register usb failed\n",
						__func__);
			goto err;
		}
	}

	ret = of_platform_populate(node, NULL, NULL, &pdev->dev);
	if (ret) {
		dev_err(&pdev->dev,
			"failed to add create dwc3 core\n");
		of_node_put(dwc3_node);
		goto put_psupply;
	}

	mdwc->dwc3 = of_find_device_by_node(dwc3_node);
	of_node_put(dwc3_node);
	if (!mdwc->dwc3) {
		dev_err(&pdev->dev, "failed to get dwc3 platform device\n");
		goto put_dwc3;
	}

	mdwc->hs_phy = devm_usb_get_phy_by_phandle(&mdwc->dwc3->dev,
							"usb-phy", 0);
	if (IS_ERR(mdwc->hs_phy)) {
		dev_err(&pdev->dev, "unable to get hsphy device\n");
		ret = PTR_ERR(mdwc->hs_phy);
		goto put_dwc3;
	}
	mdwc->ss_phy = devm_usb_get_phy_by_phandle(&mdwc->dwc3->dev,
							"usb-phy", 1);
	if (IS_ERR(mdwc->ss_phy)) {
		dev_err(&pdev->dev, "unable to get ssphy device\n");
		ret = PTR_ERR(mdwc->ss_phy);
		goto put_dwc3;
	}

	mdwc->bus_scale_table = msm_bus_cl_get_pdata(pdev);
	if (!mdwc->bus_scale_table) {
		dev_err(&pdev->dev, "bus scaling is disabled\n");
	} else {
		mdwc->bus_perf_client =
			msm_bus_scale_register_client(mdwc->bus_scale_table);
		ret = msm_bus_scale_client_update_request(
						mdwc->bus_perf_client, 1);
		if (ret)
			dev_err(&pdev->dev, "Failed to vote for bus scaling\n");
	}

	dwc = platform_get_drvdata(mdwc->dwc3);
	if (!dwc) {
		dev_err(&pdev->dev, "Failed to get dwc3 device\n");
		goto put_dwc3;
	}

	dwc->vbus_active = of_property_read_bool(node, "qcom,vbus-present");
	mdwc->irq_to_affin = platform_get_irq(mdwc->dwc3, 0);
	mdwc->dwc3_cpu_notifier.notifier_call = dwc3_cpu_notifier_cb;
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	mdwc->read_cable_adc = dwc3_read_cable_adc;
#endif

	if (cpu_to_affin)
		register_cpu_notifier(&mdwc->dwc3_cpu_notifier);

	device_init_wakeup(mdwc->dev, 1);
	pm_stay_awake(mdwc->dev);

	if (of_property_read_bool(node, "qcom,disable-dev-mode-pm"))
		pm_runtime_get_noresume(mdwc->dev);

	schedule_delayed_work(&mdwc->sm_work, 0);

	/* Update initial ID state */
	if (mdwc->pmic_id_irq) {
		enable_irq(mdwc->pmic_id_irq);
		local_irq_save(flags);
		mdwc->id_state = !!irq_read_line(mdwc->pmic_id_irq);
		if (mdwc->id_state == DWC3_ID_GROUND)
			schedule_work(&mdwc->id_work);
		local_irq_restore(flags);
		enable_irq_wake(mdwc->pmic_id_irq);
	}

	if (!dwc->is_drd && host_mode) {
		dev_dbg(&pdev->dev, "DWC3 in host only mode\n");
		mdwc->in_host_mode = true;
		mdwc->hs_phy->flags |= PHY_HOST_MODE;
		mdwc->ss_phy->flags |= PHY_HOST_MODE;
		mdwc->id_state = DWC3_ID_GROUND;
		dwc3_ext_event_notify(mdwc);
	}

	return 0;

put_dwc3:
	platform_device_put(mdwc->dwc3);
	if (mdwc->bus_perf_client)
		msm_bus_scale_unregister_client(mdwc->bus_perf_client);
put_psupply:
	if (mdwc->usb_psy.dev)
		power_supply_unregister(&mdwc->usb_psy);
err:
	return ret;
}

static int dwc3_msm_remove_children(struct device *dev, void *data)
{
	device_unregister(dev);
	return 0;
}

static int dwc3_msm_remove(struct platform_device *pdev)
{
	struct dwc3_msm	*mdwc = platform_get_drvdata(pdev);
	int ret_pm;

	if (cpu_to_affin)
		unregister_cpu_notifier(&mdwc->dwc3_cpu_notifier);

	/*
	 * In case of system suspend, pm_runtime_get_sync fails.
	 * Hence turn ON the clocks manually.
	 */
	ret_pm = pm_runtime_get_sync(mdwc->dev);
	dbg_event(0xFF, "Remov gsyn", ret_pm);
	if (ret_pm < 0) {
		dev_err(mdwc->dev,
			"pm_runtime_get_sync failed with %d\n", ret_pm);
		clk_prepare_enable(mdwc->utmi_clk);
		clk_prepare_enable(mdwc->core_clk);
		clk_prepare_enable(mdwc->iface_clk);
		clk_prepare_enable(mdwc->sleep_clk);
		if (mdwc->bus_aggr_clk)
			clk_prepare_enable(mdwc->bus_aggr_clk);
		clk_prepare_enable(mdwc->xo_clk);
	}

	cancel_delayed_work_sync(&mdwc->sm_work);
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	cancel_delayed_work_sync(&mdwc->cable_adc_work);
#endif
#ifdef CONFIG_LGE_USB_MAXIM_EVP
	cancel_delayed_work_sync(&mdwc->evp_connect_work);
#endif
	if (mdwc->usb_psy.dev)
		power_supply_unregister(&mdwc->usb_psy);
	if (mdwc->hs_phy)
		mdwc->hs_phy->flags &= ~PHY_HOST_MODE;
	platform_device_put(mdwc->dwc3);
	device_for_each_child(&pdev->dev, NULL, dwc3_msm_remove_children);

	dbg_event(0xFF, "Remov put", 0);
	pm_runtime_disable(mdwc->dev);
	pm_runtime_barrier(mdwc->dev);
	pm_runtime_put_sync(mdwc->dev);
	pm_runtime_set_suspended(mdwc->dev);
	device_wakeup_disable(mdwc->dev);

	if (mdwc->bus_perf_client)
		msm_bus_scale_unregister_client(mdwc->bus_perf_client);

#ifndef CONFIG_LGE_USB_TYPE_C
	if (!IS_ERR_OR_NULL(mdwc->vbus_reg))
		regulator_disable(mdwc->vbus_reg);
#endif

	if (mdwc->hs_phy_irq)
		disable_irq(mdwc->hs_phy_irq);
	if (mdwc->pwr_event_irq)
		disable_irq(mdwc->pwr_event_irq);

	clk_disable_unprepare(mdwc->utmi_clk);
	clk_set_rate(mdwc->core_clk, 19200000);
	clk_disable_unprepare(mdwc->core_clk);
	clk_disable_unprepare(mdwc->iface_clk);
	clk_disable_unprepare(mdwc->sleep_clk);
	clk_disable_unprepare(mdwc->xo_clk);
	clk_put(mdwc->xo_clk);

#ifdef CONFIG_LGE_PM
	mdwc->xo_vote_for_charger = false;
#endif

	dwc3_msm_config_gdsc(mdwc, 0);

	return 0;
}

#define VBUS_REG_CHECK_DELAY	(msecs_to_jiffies(1000))

/**
 * dwc3_otg_start_host -  helper function for starting/stoping the host controller driver.
 *
 * @mdwc: Pointer to the dwc3_msm structure.
 * @on: start / stop the host controller driver.
 *
 * Returns 0 on success otherwise negative errno.
 */
static int dwc3_otg_start_host(struct dwc3_msm *mdwc, int on)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
	struct usb_hcd *hcd;
	int ret = 0;

	if (!dwc->xhci)
		return -EINVAL;

#ifndef CONFIG_LGE_USB_TYPE_C
	if (!mdwc->vbus_reg) {
		mdwc->vbus_reg = devm_regulator_get(mdwc->dev, "vbus_dwc3");
		if (IS_ERR(mdwc->vbus_reg)) {
			dev_err(mdwc->dev, "Failed to get vbus regulator\n");
			ret = PTR_ERR(mdwc->vbus_reg);
			mdwc->vbus_reg = 0;
			return ret;
		}
	}
#endif

	if (on) {
		dev_dbg(mdwc->dev, "%s: turn on host\n", __func__);
#ifdef CONFIG_LGE_USB_G_ANDROID
		mdwc->hs_phy->flags |= PHY_OTG_MODE;
#endif

		pm_runtime_get_sync(mdwc->dev);
		dbg_event(0xFF, "StrtHost gync",
			atomic_read(&mdwc->dev->power.usage_count));
		usb_phy_notify_connect(mdwc->hs_phy, USB_SPEED_HIGH);
#ifndef CONFIG_LGE_USB_TYPE_C
		ret = regulator_enable(mdwc->vbus_reg);
		if (ret) {
			dev_err(dwc->dev, "unable to enable vbus_reg\n");
#ifdef CONFIG_LGE_USB_G_ANDROID
			mdwc->hs_phy->flags &= ~PHY_OTG_MODE;
#endif
			pm_runtime_put_sync(dwc->dev);
			dbg_event(0xFF, "vregerr psync",
				atomic_read(&dwc->dev->power.usage_count));
			return ret;
		}
#endif

		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_HOST);

		/*
		 * FIXME If micro A cable is disconnected during system suspend,
		 * xhci platform device will be removed before runtime pm is
		 * enabled for xhci device. Due to this, disable_depth becomes
		 * greater than one and runtimepm is not enabled for next microA
		 * connect. Fix this by calling pm_runtime_init for xhci device.
		 */
		pm_runtime_init(&dwc->xhci->dev);
		ret = platform_device_add(dwc->xhci);
		if (ret) {
			dev_err(mdwc->dev,
				"%s: failed to add XHCI pdev ret=%d\n",
				__func__, ret);
#ifdef CONFIG_LGE_USB_G_ANDROID
			mdwc->hs_phy->flags &= ~PHY_OTG_MODE;
#endif
#ifndef CONFIG_LGE_USB_TYPE_C
			regulator_disable(mdwc->vbus_reg);
#endif

			pm_runtime_put_sync(dwc->dev);
			dbg_event(0xFF, "pdeverr psync",
				atomic_read(&dwc->dev->power.usage_count));
			return ret;
		}

		/*
		 * In some cases it is observed that USB PHY is not going into
		 * suspend with host mode suspend functionality. Hence disable
		 * XHCI's runtime PM here if disable_host_mode_pm is set.
		 */
		if (mdwc->disable_host_mode_pm)
			pm_runtime_disable(&dwc->xhci->dev);

		hcd = platform_get_drvdata(dwc->xhci);

		mdwc->in_host_mode = true;
		dwc3_gadget_usb3_phy_suspend(dwc, true);

		/* xHCI should have incremented child count as necessary */
		dbg_event(0xFF, "StrtHost psync",
			atomic_read(&mdwc->dev->power.usage_count));
		pm_runtime_mark_last_busy(mdwc->dev);
		pm_runtime_put_autosuspend(mdwc->dev);
	} else {
		dev_dbg(dwc->dev, "%s: turn off host\n", __func__);
#ifdef CONFIG_LGE_USB_G_ANDROID
		mdwc->hs_phy->flags &= ~PHY_OTG_MODE;
#endif

#ifndef CONFIG_LGE_USB_TYPE_C
		ret = regulator_disable(mdwc->vbus_reg);
		if (ret) {
			dev_err(dwc->dev, "unable to disable vbus_reg\n");
			return ret;
		}
#endif

		pm_runtime_get_sync(mdwc->dev);
		dbg_event(0xFF, "StopHost gsync",
			atomic_read(&mdwc->dev->power.usage_count));
		usb_phy_notify_disconnect(mdwc->hs_phy, USB_SPEED_HIGH);
		platform_device_del(dwc->xhci);

		/*
		 * Perform USB hardware RESET (both core reset and DBM reset)
		 * when moving from host to peripheral. This is required for
		 * peripheral mode to work.
		 */
		dwc3_msm_block_reset(mdwc, true);

		dwc3_gadget_usb3_phy_suspend(dwc, false);
		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);

		mdwc->in_host_mode = false;

		/* re-init core and OTG registers as block reset clears these */
		dwc3_post_host_reset_core_init(dwc);
		pm_runtime_mark_last_busy(mdwc->dev);
		pm_runtime_put_autosuspend(mdwc->dev);
		dbg_event(0xFF, "StopHost psync",
			atomic_read(&mdwc->dev->power.usage_count));
	}

	return 0;
}

/**
 * dwc3_otg_start_peripheral -  bind/unbind the peripheral controller.
 *
 * @mdwc: Pointer to the dwc3_msm structure.
 * @on:   Turn ON/OFF the gadget.
 *
 * Returns 0 on success otherwise negative errno.
 */
static int dwc3_otg_start_peripheral(struct dwc3_msm *mdwc, int on)
{
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	pm_runtime_get_sync(mdwc->dev);
	dbg_event(0xFF, "StrtGdgt gsync",
		atomic_read(&mdwc->dev->power.usage_count));

	if (on) {
		dev_dbg(mdwc->dev, "%s: turn on gadget %s\n",
					__func__, dwc->gadget.name);

		usb_phy_notify_connect(mdwc->hs_phy, USB_SPEED_HIGH);
		usb_phy_notify_connect(mdwc->ss_phy, USB_SPEED_SUPER);

		/* Core reset is not required during start peripheral. Only
		 * DBM reset is required, hence perform only DBM reset here */
		dwc3_msm_block_reset(mdwc, false);

		dwc3_set_mode(dwc, DWC3_GCTL_PRTCAP_DEVICE);
		usb_gadget_vbus_connect(&dwc->gadget);
	} else {
		dev_dbg(mdwc->dev, "%s: turn off gadget %s\n",
					__func__, dwc->gadget.name);
		usb_gadget_vbus_disconnect(&dwc->gadget);
		usb_phy_notify_disconnect(mdwc->hs_phy, USB_SPEED_HIGH);
		usb_phy_notify_disconnect(mdwc->ss_phy, USB_SPEED_SUPER);
		dwc3_gadget_usb3_phy_suspend(dwc, false);
	}
	pm_runtime_put_sync(mdwc->dev);
	dbg_event(0xFF, "StopGdgt psync",
		atomic_read(&mdwc->dev->power.usage_count));

	dev_dbg(mdwc->dev, "%s: END %s\n",
					__func__, dwc->gadget.name);
	return 0;
}

static int dwc3_msm_gadget_vbus_draw(struct dwc3_msm *mdwc, unsigned mA)
{
	enum power_supply_type power_supply_type;
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);
#endif

	if (mdwc->charging_disabled)
		return 0;

	if (mdwc->chg_type != DWC3_INVALID_CHARGER) {
		dev_dbg(mdwc->dev,
			"SKIP setting power supply type again,chg_type = %d\n",
			mdwc->chg_type);
		goto skip_psy_type;
	}

#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	if ((mdwc->chg_type == DWC3_SDP_CHARGER) ||
		(mdwc->chg_type == DWC3_FLOATED_CHARGER))
#else
	if (mdwc->chg_type == DWC3_SDP_CHARGER)
#endif
		power_supply_type = POWER_SUPPLY_TYPE_USB;
	else if (mdwc->chg_type == DWC3_CDP_CHARGER)
		power_supply_type = POWER_SUPPLY_TYPE_USB_CDP;
	else if (mdwc->chg_type == DWC3_DCP_CHARGER ||
			mdwc->chg_type == DWC3_PROPRIETARY_CHARGER)
		power_supply_type = POWER_SUPPLY_TYPE_USB_DCP;
	else
		power_supply_type = POWER_SUPPLY_TYPE_UNKNOWN;

	power_supply_set_supply_type(&mdwc->usb_psy, power_supply_type);

skip_psy_type:

	if (mdwc->chg_type == DWC3_CDP_CHARGER)
		mA = DWC3_IDEV_CHG_MAX;

#ifdef CONFIG_LGE_PM_CABLE_DETECTION
	if (mA > 2 && lge_pm_get_cable_type() != NO_INIT_CABLE) {
		if ((mdwc->chg_type == DWC3_SDP_CHARGER) ||
			(mdwc->chg_type == DWC3_FLOATED_CHARGER)) {
			if (dwc->gadget.speed == USB_SPEED_SUPER)
				mA = DWC3_USB30_CHG_CURRENT;
			else
				mA = lge_pm_get_usb_current();
		} else if ((mdwc->chg_type == DWC3_DCP_CHARGER) ||
			(mdwc->chg_type == DWC3_PROPRIETARY_CHARGER)) {
			mA = lge_pm_get_ta_current();
		}
	}
#endif
#if defined (CONFIG_LGE_TOUCH_CORE)
	touch_notify_connect(mdwc->chg_type);
#endif
	if (mdwc->max_power == mA)
		return 0;

	dev_info(mdwc->dev, "Avail curr from USB = %u\n", mA);

	if (mdwc->max_power <= 2 && mA > 2) {
		/* Enable Charging */
		if (power_supply_set_online(&mdwc->usb_psy, true))
			goto psy_error;
		if (power_supply_set_current_limit(&mdwc->usb_psy, 1000*mA))
			goto psy_error;
	} else if (mdwc->max_power > 0 && (mA == 0 || mA == 2)) {
		/* Disable charging */
		if (power_supply_set_online(&mdwc->usb_psy, false))
			goto psy_error;
	} else {
		/* Enable charging */
		if (power_supply_set_online(&mdwc->usb_psy, true))
			goto psy_error;
	}

	/* Set max current limit in uA */
	if (power_supply_set_current_limit(&mdwc->usb_psy, 1000*mA))
		goto psy_error;

	power_supply_changed(&mdwc->usb_psy);
	mdwc->max_power = mA;

#ifdef CONFIG_LGE_PM
	if (lge_get_boot_mode() != LGE_BOOT_MODE_CHARGERLOGO) {
		if (mdwc->chg_type == DWC3_DCP_CHARGER) {
			if (!mdwc->xo_vote_for_charger) {
				struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

				if (atomic_read(&dwc->in_lpm)) {
					int ret;
					ret = clk_prepare_enable(mdwc->xo_clk);
					if (ret)
						dev_err(mdwc->dev, "%s failed to vote TCXO buffer%d\n",
								__func__, ret);
					else
						dev_err(mdwc->dev, "%s TCXO enabled\n",
								__func__);
				}
				mdwc->xo_vote_for_charger = true;
			}
		} else {
			if (mdwc->xo_vote_for_charger) {
				struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

				if (atomic_read(&dwc->in_lpm)) {
					clk_disable_unprepare(mdwc->xo_clk);
					dev_err(mdwc->dev, "%s TCXO disabled\n",
							__func__);
				}
				mdwc->xo_vote_for_charger = false;
			}
		}
	}
#endif

	return 0;

psy_error:
	dev_dbg(mdwc->dev, "power supply error when setting property\n");
	return -ENXIO;
}


void dwc3_init_sm(struct dwc3_msm *mdwc)
{
	int ret;
	static bool sm_initialized;

	/*
	 * dwc3_init_sm() can be called multiple times in undefined state.
	 * example: QC charger connected during boot up sequeunce, and
	 * performing charger disconnect.
	 */
	if (sm_initialized) {
		pr_debug("%s(): Already sm_initialized.\n", __func__);
		return;
	}

	/*
	 * VBUS initial state is reported after PMIC
	 * driver initialization. Wait for it.
	 */
	ret = wait_for_completion_timeout(&mdwc->dwc3_xcvr_vbus_init,
					msecs_to_jiffies(SM_INIT_TIMEOUT));
	if (!ret) {
		dev_err(mdwc->dev, "%s: completion timeout\n", __func__);
		/* We can safely assume no cable connected */
		set_bit(ID, &mdwc->inputs);
	}

	sm_initialized = true;
}

static void dwc3_initialize(struct dwc3_msm *mdwc)
{
	u32 tmp;
	int ret;
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dbg_event(0xFF, "Initialized Start",
			atomic_read(&mdwc->dev->power.usage_count));

	if (mdwc->bus_perf_client) {
		mdwc->bus_vote = 1;
		schedule_work(&mdwc->bus_vote_w);
	}

	/* enable USB GDSC */
	dwc3_msm_config_gdsc(mdwc, 1);

	/* enable all clocks */
	ret = clk_prepare_enable(mdwc->xo_clk);
	clk_prepare_enable(mdwc->iface_clk);
	clk_prepare_enable(mdwc->core_clk);
	clk_prepare_enable(mdwc->sleep_clk);
	clk_prepare_enable(mdwc->utmi_clk);
	if (mdwc->bus_aggr_clk)
		clk_prepare_enable(mdwc->bus_aggr_clk);

	/* Perform controller GCC reset */
	dwc3_msm_link_clk_reset(mdwc, 1);
	msleep(20);
	dwc3_msm_link_clk_reset(mdwc, 0);

	/*
	 * Get core configuration and initialized
	 * Set Event buffers
	 * Reset both USB PHYs and initialized
	 */
	dwc3_core_pre_init(dwc);

	/* Get initial P3 status and enable IN_P3 event */
	tmp = dwc3_msm_read_reg_field(mdwc->base,
		DWC3_GDBGLTSSM, DWC3_GDBGLTSSM_LINKSTATE_MASK);
	atomic_set(&mdwc->in_p3, tmp == DWC3_LINK_STATE_U3);
	dwc3_msm_write_reg_field(mdwc->base, PWR_EVNT_IRQ_MASK_REG,
			PWR_EVNT_POWERDOWN_IN_P3_MASK, 1);
	enable_irq(mdwc->hs_phy_irq);
}

/**
 * dwc3_otg_sm_work - workqueue function.
 *
 * @w: Pointer to the dwc3 otg workqueue
 *
 * NOTE: After any change in otg_state, we must reschdule the state machine.
 */
static void dwc3_otg_sm_work(struct work_struct *w)
{
	struct dwc3_msm *mdwc = container_of(w, struct dwc3_msm, sm_work.work);
	struct dwc3 *dwc = NULL;
	bool work = 0;
	int ret = 0;
	unsigned long delay = 0;
	const char *state;

	if (mdwc->dwc3)
		dwc = platform_get_drvdata(mdwc->dwc3);

	if (!dwc) {
		dev_err(mdwc->dev, "dwc is NULL.\n");
		return;
	}

	state = usb_otg_state_string(mdwc->otg_state);
#ifdef CONFIG_LGE_USB_G_ANDROID
	dev_info(mdwc->dev, "%s state, vbus:%s\n",
			state, (mdwc->vbus_active)? "online" : "offline" );
#else
	dev_dbg(mdwc->dev, "%s state\n", state);
#endif
	dbg_event(0xFF, state, 0);

	/* Check OTG state */
	switch (mdwc->otg_state) {
	case OTG_STATE_UNDEFINED:
		dwc3_init_sm(mdwc);
		if (!test_bit(ID, &mdwc->inputs)) {
			dbg_event(0xFF, "undef_host", 0);
			atomic_set(&dwc->in_lpm, 0);
			pm_runtime_set_active(mdwc->dev);
			pm_runtime_enable(mdwc->dev);
			pm_runtime_get_noresume(mdwc->dev);
			dwc3_initialize(mdwc);
			mdwc->otg_state = OTG_STATE_A_HOST;
			dwc3_otg_start_host(mdwc, 1);
			pm_runtime_put_noidle(mdwc->dev);
			return;
		}

		if (test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "b_sess_vld\n");
			dbg_event(0xFF, "undef_b_sess_vld", 0);
			switch (mdwc->chg_type) {
			case DWC3_DCP_CHARGER:
#ifdef CONFIG_LGE_USB_MAXIM_EVP
				atomic_set(&dwc->in_lpm, 0);
				pm_runtime_set_active(mdwc->dev);
				pm_runtime_enable(mdwc->dev);
				pm_runtime_get_noresume(mdwc->dev);
				dwc3_initialize(mdwc);
				dwc3_msm_gadget_vbus_draw(mdwc,
						DEFAULT_DCP_CHG_MAX);
				dwc->gadget.evp_sts |= EVP_STS_DCP;
				pm_runtime_put_sync(mdwc->dev);
				dbg_event(0xFF, "Undef DCP",
					atomic_read(
						&mdwc->dev->power.usage_count));
				mdwc->otg_state = OTG_STATE_B_IDLE;
				break;
#endif
			case DWC3_PROPRIETARY_CHARGER:
				dev_dbg(mdwc->dev, "DCP charger\n");
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
				dwc3_msm_gadget_vbus_draw(mdwc,
						DEFAULT_DCP_CHG_MAX);
#else
				dwc3_msm_gadget_vbus_draw(mdwc,
						dcp_max_current);
#endif
				atomic_set(&dwc->in_lpm, 1);
				pm_relax(mdwc->dev);
				break;
			case DWC3_CDP_CHARGER:
			case DWC3_SDP_CHARGER:
				atomic_set(&dwc->in_lpm, 0);
				pm_runtime_set_active(mdwc->dev);
				pm_runtime_enable(mdwc->dev);
				pm_runtime_get_noresume(mdwc->dev);
				dwc3_initialize(mdwc);
				dwc3_otg_start_peripheral(mdwc, 1);
				mdwc->otg_state = OTG_STATE_B_PERIPHERAL;
				dbg_event(0xFF, "Undef SDP",
					atomic_read(
					&mdwc->dev->power.usage_count));
				break;
			default:
				WARN_ON(1);
				break;
			}
		}

		if (!test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dbg_event(0xFF, "undef_!b_sess_vld", 0);
			atomic_set(&dwc->in_lpm, 0);
			pm_runtime_set_active(mdwc->dev);
			pm_runtime_enable(mdwc->dev);
			pm_runtime_get_noresume(mdwc->dev);
			dwc3_initialize(mdwc);
			pm_runtime_put_sync(mdwc->dev);
			dbg_event(0xFF, "Undef NoUSB",
				atomic_read(&mdwc->dev->power.usage_count));
			mdwc->otg_state = OTG_STATE_B_IDLE;
		}
		break;

	case OTG_STATE_B_IDLE:
		if (!test_bit(ID, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "!id\n");
			mdwc->otg_state = OTG_STATE_A_IDLE;
			work = 1;
			mdwc->chg_type = DWC3_INVALID_CHARGER;
		} else if (test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "b_sess_vld\n");
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
			if ((mdwc->chg_type != DWC3_INVALID_CHARGER) &&
				!mdwc->adc_read_complete) {
				mdwc->read_cable_adc(mdwc,
					true);
#ifndef CONFIG_LGE_USB_BC_12_VZW
				mdwc->otg_state = OTG_STATE_B_IDLE;
				work = 1;
				delay = VBUS_REG_CHECK_DELAY;
				break;
#endif
			}
#endif
			switch (mdwc->chg_type) {
			case DWC3_DCP_CHARGER:
#ifdef CONFIG_LGE_USB_MAXIM_EVP
				dev_dbg(mdwc->dev, "DCP charger, evp_sts %u\n", dwc->gadget.evp_sts);
				if (dwc->gadget.evp_sts & EVP_STS_DETGO) {
					pm_runtime_get_sync(mdwc->dev);
					dbg_event(0xFF, "Dcp gsync",
						atomic_read(
							&mdwc->dev->power.usage_count));
					dwc3_otg_start_peripheral(mdwc, 1);
					mdwc->otg_state = OTG_STATE_B_PERIPHERAL;
					work = 1;
				} else {
					dwc3_msm_gadget_vbus_draw(mdwc,
							DEFAULT_DCP_CHG_MAX);
					dwc->gadget.evp_sts |= EVP_STS_DCP;
				}
				break;
#endif

			case DWC3_PROPRIETARY_CHARGER:
				dev_dbg(mdwc->dev, "lpm, DCP charger\n");
				dwc3_msm_gadget_vbus_draw(mdwc,
						dcp_max_current);
				break;
			case DWC3_CDP_CHARGER:
				dwc3_msm_gadget_vbus_draw(mdwc,
						DWC3_IDEV_CHG_MAX);
				/* fall through */
			case DWC3_SDP_CHARGER:
				/*
				 * Increment pm usage count upon cable
				 * connect. Count is decremented in
				 * OTG_STATE_B_PERIPHERAL state on cable
				 * disconnect or in bus suspend.
				 */
				pm_runtime_get_sync(mdwc->dev);
				dbg_event(0xFF, "CHG gsync",
					atomic_read(
						&mdwc->dev->power.usage_count));
				dwc3_otg_start_peripheral(mdwc, 1);
				mdwc->otg_state = OTG_STATE_B_PERIPHERAL;
				work = 1;
				break;
			/* fall through */
			default:
				break;
			}
		} else {
#ifdef CONFIG_LGE_PM_CABLE_DETECTION
			mdwc->read_cable_adc(mdwc, false);
#endif
#ifdef CONFIG_LGE_USB_MAXIM_EVP
			mdwc->evp_sts = 0;
			dwc->gadget.evp_sts = 0;
			dwc->evp_usbctrl_err_cnt = 0;
#endif
			dwc3_msm_gadget_vbus_draw(mdwc, 0);
			dev_dbg(mdwc->dev, "No device, allowing suspend\n");
		}
		break;

	case OTG_STATE_B_PERIPHERAL:
		if (!test_bit(B_SESS_VLD, &mdwc->inputs) ||
				!test_bit(ID, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "!id || !bsv\n");
			mdwc->otg_state = OTG_STATE_B_IDLE;
			dwc3_otg_start_peripheral(mdwc, 0);
			/*
			 * Decrement pm usage count upon cable disconnect
			 * which was incremented upon cable connect in
			 * OTG_STATE_B_IDLE state
			 */
#ifdef CONFIG_LGE_USB_MAXIM_EVP
			if (!(dwc->gadget.evp_sts & EVP_STS_SIMPLE))
#endif
#ifndef CONFIG_LGE_USB_G_ANDROID
			pm_runtime_put_sync(mdwc->dev);
#else
			ret = pm_runtime_put_sync(mdwc->dev);
			if ((ret == -EBUSY) && atomic_read(&mdwc->dev->power.child_count)) {
				pr_info("%s : child count exists, and retry suspend\n",	__func__);
				pm_runtime_idle(dwc->dev);
				usleep_range(1000, 1200);
				pm_runtime_idle(mdwc->dev);
			}
#endif
			dbg_event(0xFF, "BPER psync",
				atomic_read(&mdwc->dev->power.usage_count));
			mdwc->chg_type = DWC3_INVALID_CHARGER;
#ifdef CONFIG_LGE_USB_MAXIM_EVP
			mdwc->evp_sts = 0;
			dwc->gadget.evp_sts = 0;
			dwc->evp_usbctrl_err_cnt = 0;
#endif
			work = 1;
		} else if (test_bit(B_SUSPEND, &mdwc->inputs) &&
			test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "BPER bsv && susp\n");
			mdwc->otg_state = OTG_STATE_B_SUSPEND;
			/*
			 * Decrement pm usage count upon bus suspend.
			 * Count was incremented either upon cable
			 * connect in OTG_STATE_B_IDLE or host
			 * initiated resume after bus suspend in
			 * OTG_STATE_B_SUSPEND state
			 */
			pm_runtime_mark_last_busy(mdwc->dev);
			pm_runtime_put_autosuspend(mdwc->dev);
			dbg_event(0xFF, "SUSP put",
				atomic_read(&mdwc->dev->power.usage_count));
		}
		break;

	case OTG_STATE_B_SUSPEND:
		if (!test_bit(B_SESS_VLD, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "BSUSP: !bsv\n");
			mdwc->otg_state = OTG_STATE_B_IDLE;
			dwc3_otg_start_peripheral(mdwc, 0);
		} else if (!test_bit(B_SUSPEND, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "BSUSP !susp\n");
			mdwc->otg_state = OTG_STATE_B_PERIPHERAL;
			/*
			 * Increment pm usage count upon host
			 * initiated resume. Count was decremented
			 * upon bus suspend in
			 * OTG_STATE_B_PERIPHERAL state.
			 */
			pm_runtime_get_sync(mdwc->dev);
			dbg_event(0xFF, "SUSP gsync",
				atomic_read(&mdwc->dev->power.usage_count));
		}
		break;

	case OTG_STATE_A_IDLE:
		/* Switch to A-Device*/
		if (test_bit(ID, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "id\n");
			mdwc->otg_state = OTG_STATE_B_IDLE;
			mdwc->vbus_retry_count = 0;
			work = 1;
		} else {
			mdwc->otg_state = OTG_STATE_A_HOST;
			ret = dwc3_otg_start_host(mdwc, 1);
			if ((ret == -EPROBE_DEFER) &&
						mdwc->vbus_retry_count < 3) {
				/*
				 * Get regulator failed as regulator driver is
				 * not up yet. Will try to start host after 1sec
				 */
				mdwc->otg_state = OTG_STATE_A_IDLE;
				dev_dbg(mdwc->dev, "Unable to get vbus regulator. Retrying...\n");
				delay = VBUS_REG_CHECK_DELAY;
				work = 1;
				mdwc->vbus_retry_count++;
			} else if (ret) {
				dev_err(mdwc->dev, "unable to start host\n");
				mdwc->otg_state = OTG_STATE_A_IDLE;
				goto ret;
			}
#if defined (CONFIG_LGE_TOUCH_CORE)
			touch_notify_connect(6);
#endif
		}
		break;

	case OTG_STATE_A_HOST:
		if (test_bit(ID, &mdwc->inputs)) {
			dev_dbg(mdwc->dev, "id\n");
			dwc3_otg_start_host(mdwc, 0);
			mdwc->otg_state = OTG_STATE_B_IDLE;
			mdwc->vbus_retry_count = 0;
			work = 1;
		} else {
			dev_dbg(mdwc->dev, "still in a_host state. Resuming root hub.\n");
			dbg_event(0xFF, "XHCIResume", 0);
			if (dwc)
				pm_runtime_resume(&dwc->xhci->dev);
		}
		break;

	default:
		dev_err(mdwc->dev, "%s: invalid otg-state\n", __func__);

	}

	if (work)
		schedule_delayed_work(&mdwc->sm_work, delay);

ret:
	return;
}

#ifdef CONFIG_PM_SLEEP
static int dwc3_msm_pm_suspend(struct device *dev)
{
	int ret = 0;
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);
	struct dwc3 *dwc = platform_get_drvdata(mdwc->dwc3);

	dev_dbg(dev, "dwc3-msm PM suspend\n");
	dbg_event(0xFF, "PM Sus", 0);

	flush_workqueue(mdwc->dwc3_wq);
	if (!atomic_read(&dwc->in_lpm)) {
		dev_err(mdwc->dev, "Abort PM suspend!! (USB is outside LPM)\n");
		return -EBUSY;
	}

	ret = dwc3_msm_suspend(mdwc);
	if (!ret)
		atomic_set(&mdwc->pm_suspended, 1);

	return ret;
}

static int dwc3_msm_pm_resume(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "dwc3-msm PM resume\n");

	dbg_event(0xFF, "PM Res", 0);

	/* flush to avoid race in read/write of pm_suspended */
	flush_workqueue(mdwc->dwc3_wq);
	atomic_set(&mdwc->pm_suspended, 0);

	/* kick in otg state machine */
#ifdef CONFIG_LGE_USB_G_ANDROID
	cancel_delayed_work(&mdwc->resume_work);
#endif
	queue_delayed_work(mdwc->dwc3_wq, &mdwc->resume_work, 0);

	return 0;
}
#endif

#ifdef CONFIG_PM_RUNTIME
static int dwc3_msm_runtime_idle(struct device *dev)
{
	dev_dbg(dev, "DWC3-msm runtime idle\n");
	dbg_event(0xFF, "RT Idle", 0);

	return 0;
}

static int dwc3_msm_runtime_suspend(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "DWC3-msm runtime suspend\n");
	dbg_event(0xFF, "RT Sus", 0);

	return dwc3_msm_suspend(mdwc);
}

static int dwc3_msm_runtime_resume(struct device *dev)
{
	struct dwc3_msm *mdwc = dev_get_drvdata(dev);

	dev_dbg(dev, "DWC3-msm runtime resume\n");
	dbg_event(0xFF, "RT Res", 0);

	return dwc3_msm_resume(mdwc);
}
#endif

static const struct dev_pm_ops dwc3_msm_dev_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(dwc3_msm_pm_suspend, dwc3_msm_pm_resume)
	SET_RUNTIME_PM_OPS(dwc3_msm_runtime_suspend, dwc3_msm_runtime_resume,
				dwc3_msm_runtime_idle)
};

static const struct of_device_id of_dwc3_matach[] = {
	{
		.compatible = "qcom,dwc-usb3-msm",
	},
	{ },
};
MODULE_DEVICE_TABLE(of, of_dwc3_matach);

static struct platform_driver dwc3_msm_driver = {
	.probe		= dwc3_msm_probe,
	.remove		= dwc3_msm_remove,
	.driver		= {
		.name	= "msm-dwc3",
		.pm	= &dwc3_msm_dev_pm_ops,
		.of_match_table	= of_dwc3_matach,
	},
};

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("DesignWare USB3 MSM Glue Layer");

static int dwc3_msm_init(void)
{
	return platform_driver_register(&dwc3_msm_driver);
}
module_init(dwc3_msm_init);

static void __exit dwc3_msm_exit(void)
{
	platform_driver_unregister(&dwc3_msm_driver);
}
module_exit(dwc3_msm_exit);
