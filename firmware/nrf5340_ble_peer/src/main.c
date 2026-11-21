#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

#define CONFORMANCE_SERVICE_UUID 0xff00
#define CONFORMANCE_VALUE_UUID 0xff01
#define SECONDARY_VALUE_UUID 0xff02
#define VALUE_CAPACITY 20U

static uint8_t value[VALUE_CAPACITY] = {0x00, 0xff, 0x80};
static uint16_t value_len = 3;
static const uint8_t secondary_value[] = {0x7f, 0x00, 0xff};
static const uint8_t notification_value[] = {0x80, 0x00, 0xff, 0xc3, 0x28};

static atomic_t notifications_enabled;
static struct k_work notification_work;
static struct k_work advertising_work;

static ssize_t read_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, value_len);
}

static ssize_t read_secondary_value(struct bt_conn *conn,
				    const struct bt_gatt_attr *attr, void *buf,
				    uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset, secondary_value,
				 sizeof(secondary_value));
}

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t ccc_value)
{
	(void)attr;
	atomic_set(&notifications_enabled, ccc_value == BT_GATT_CCC_NOTIFY);
}

static ssize_t write_value(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			   const void *buf, uint16_t len, uint16_t offset,
			   uint8_t flags)
{
	(void)conn;
	(void)attr;
	(void)flags;

	if (offset != 0U) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len == 0U || len > sizeof(value)) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	memcpy(value, buf, len);
	value_len = len;

	/* This exact post-subscription write is the notification test trigger. */
	if (atomic_get(&notifications_enabled) != 0 &&
	    len == sizeof(notification_value) &&
	    memcmp(buf, notification_value, len) == 0) {
		(void)k_work_submit(&notification_work);
	}

	return len;
}

BT_GATT_SERVICE_DEFINE(conformance_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(CONFORMANCE_SERVICE_UUID)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(CONFORMANCE_VALUE_UUID),
		BT_GATT_CHRC_READ | BT_GATT_CHRC_WRITE |
			BT_GATT_CHRC_WRITE_WITHOUT_RESP | BT_GATT_CHRC_NOTIFY,
		BT_GATT_PERM_READ | BT_GATT_PERM_WRITE,
		read_value, write_value, NULL),
	BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(SECONDARY_VALUE_UUID),
		BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
		read_secondary_value, NULL, NULL),
);

static void notify_test_value(struct k_work *work)
{
	int err;

	(void)work;
	if (atomic_get(&notifications_enabled) == 0) {
		return;
	}

	err = bt_gatt_notify(NULL, &conformance_svc.attrs[2], notification_value,
			     sizeof(notification_value));
	if (err != 0) {
		printk("GATT notification failed (%d)\n", err);
	}
}

static const struct bt_data advertisement[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(CONFORMANCE_SERVICE_UUID)),
};

static const struct bt_data scan_response[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};

static int advertise(void)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, advertisement,
				  ARRAY_SIZE(advertisement), scan_response,
				  ARRAY_SIZE(scan_response));

	if (err != 0) {
		printk("Advertising failed (%d)\n", err);
		return err;
	}

	printk("Advertising as " CONFIG_BT_DEVICE_NAME " with service ff00\n");
	return 0;
}

static void restart_advertising(struct k_work *work)
{
	(void)work;
	(void)advertise();
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	(void)conn;
	if (err != 0U) {
		printk("Connection failed (0x%02x)\n", err);
		return;
	}
	printk("Peer connected\n");
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
	atomic_set(&notifications_enabled, 0);
{
	(void)conn;
	printk("Peer disconnected (0x%02x)\n", reason);
}

static void connection_recycled(void)
{
	(void)k_work_submit(&advertising_work);
}

BT_CONN_CB_DEFINE(connection_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
	.recycled = connection_recycled,
};

int main(void)
{
	int err;

	k_work_init(&notification_work, notify_test_value);
	k_work_init(&advertising_work, restart_advertising);

	err = bt_enable(NULL);
	if (err != 0) {
		printk("Bluetooth initialization failed (%d)\n", err);
		return err;
	}

	printk("Microbrowser BLE conformance peer ready\n");
	return advertise();
}
