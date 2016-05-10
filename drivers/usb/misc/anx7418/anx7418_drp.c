#include <linux/usb/class-dual-role.h>
#include <linux/i2c.h>

static enum dual_role_property drp_properties[] = {
	DUAL_ROLE_PROP_MODE,
	DUAL_ROLE_PROP_PR,
	DUAL_ROLE_PROP_DR,
};

/* Callback for "cat /sys/class/dual_role_usb/otg_default/<property>" */
static int drp_get_property(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop,
		unsigned int *val)
{
	struct anx7418 *anx = dev_get_drvdata(dual_role->dev.parent);
	int rc = 0;

	if (!anx) {
		pr_err("%s: drvdata is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&anx->mutex);

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
#ifdef CONFIG_LGE_ALICE_FRIENDS
		if (anx->friends != LGE_ALICE_FRIENDS_NONE)
			*val = DUAL_ROLE_PROP_MODE_NONE;
		else
#endif
		*val = anx->mode;
		break;

	case DUAL_ROLE_PROP_PR:
#ifdef CONFIG_LGE_ALICE_FRIENDS
		if (anx->friends != LGE_ALICE_FRIENDS_NONE)
			*val = DUAL_ROLE_PROP_PR_NONE;
		else
#endif
		*val = anx->pr;
		break;

	case DUAL_ROLE_PROP_DR:
#ifdef CONFIG_LGE_ALICE_FRIENDS
		if (anx->friends != LGE_ALICE_FRIENDS_NONE)
			*val = DUAL_ROLE_PROP_DR_NONE;
		else
#endif
		*val = anx->dr;
		break;

	default:
		pr_err("%s: unknown property. %d\n", __func__, prop);
		rc = -EINVAL;
		break;
	}

	mutex_unlock(&anx->mutex);

	return rc;
}

/* Callback for "echo <value> >
 *                      /sys/class/dual_role_usb/<name>/<property>"
 * Block until the entire final state is reached.
 * Blocking is one of the better ways to signal when the operation
 * is done.
 * This function tries to switched to Attached.SRC or Attached.SNK
 * by forcing the mode into SRC or SNK.
 * On failure, we fall back to Try.SNK state machine.
 */
static int drp_set_property(struct dual_role_phy_instance *dual_role,
		enum dual_role_property prop,
		const unsigned int *val)
{
	struct anx7418 *anx = dev_get_drvdata(dual_role->dev.parent);
	struct i2c_client *client;
	struct device *cdev;
	int rc = 0;

	if (!anx) {
		pr_err("%s: drvdata is NULL\n", __func__);
		return -EIO;
	}

	client = anx->client;
	cdev = &client->dev;

	mutex_lock(&anx->mutex);

	if (!atomic_read(&anx->pwr_on)) {
		dev_err(cdev, "%s: power down\n", __func__);
		goto out;
	}

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
		if (*val == anx->mode)
			goto out;

		switch (*val) {
		case DUAL_ROLE_PROP_MODE_UFP:
			anx7418_write_reg(client, ANALOG_STATUS, 0);
			anx7418_write_reg(client, ANALOG_CTRL_9, 1);
			break;
		case DUAL_ROLE_PROP_MODE_DFP:
			anx7418_write_reg(client, ANALOG_STATUS, 2);
			anx7418_write_reg(client, ANALOG_CTRL_9, 1);
			break;
		default:
			dev_err(cdev, "%s: unknown mode value. %d\n",
					__func__, *val);
			rc = -EINVAL;
			break;
		}
		break;

	default:
		dev_err(cdev, "%s: unknown property. %d\n", __func__, prop);
		rc = -EINVAL;
		break;
	}

out:
	mutex_unlock(&anx->mutex);
	return rc;
}

/* Decides whether userspace can change a specific property */
static int drp_is_writeable(struct dual_role_phy_instance *drp,
		enum dual_role_property prop)
{
	int rc;

	switch (prop) {
	case DUAL_ROLE_PROP_MODE:
		rc = 1;
		break;
	default:
		rc = 0;
		break;
	}

	return rc;
}

int anx7418_drp_init(struct anx7418 *anx)
{
	struct device *cdev = &anx->client->dev;
	struct dual_role_phy_desc *desc;
	struct dual_role_phy_instance *dual_role;

	desc = devm_kzalloc(cdev, sizeof(struct dual_role_phy_desc),
			GFP_KERNEL);
	if (!desc) {
		dev_err(cdev, "unable to allocate dual role descriptor\n");
		return -ENOMEM;
	}

	desc->name = "otg_default";
	desc->supported_modes = DUAL_ROLE_SUPPORTED_MODES_DFP_AND_UFP;
	desc->get_property = drp_get_property;
	desc->set_property = drp_set_property;
	desc->properties = drp_properties;
	desc->num_properties = ARRAY_SIZE(drp_properties);
	desc->property_is_writeable = drp_is_writeable;
	dual_role = devm_dual_role_instance_register(cdev, desc);
	anx->dual_role = dual_role;
	anx->desc = desc;

	return 0;
}
