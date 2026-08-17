/* hidapi_stub.c — see hidapi.h. Always reports zero devices. */

#include "hidapi.h"

int hid_init(void) { return 0; }
int hid_exit(void) { return 0; }

struct hid_device_info *hid_enumerate(unsigned short vendor_id, unsigned short product_id) {
	(void)vendor_id;
	(void)product_id;
	return NULL;
}

void hid_free_enumeration(struct hid_device_info *devs) {
	(void)devs;
}

hid_device *hid_open_path(const char *path) {
	(void)path;
	return NULL;
}

void hid_close(hid_device *dev) {
	(void)dev;
}

int hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length) {
	(void)dev;
	(void)data;
	(void)length;
	return -1;
}

int hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length) {
	(void)dev;
	(void)data;
	(void)length;
	return -1;
}

const wchar_t *hid_error(hid_device *dev) {
	(void)dev;
	return L"hidapi not available on Vita";
}
