/**
 * hidapi.h — minimal Vita stub of libusb/hidapi's public C API.
 *
 * Vita homebrew has no generic USB HID host passthrough for third-party
 * adapters (no libusb/hidraw equivalent exposed to userland), so raphnet
 * N64-USB-adapter support (libultraship/src/ship/controller/raphnet/) can't
 * do anything real here. Rather than reimplementing that C++ layer for
 * Vita, this stub lets it compile unchanged against a backend that always
 * reports zero devices - RaphnetPhysicalDeviceManager::Init() succeeds
 * having claimed 0 ports, and the standard SDL/keyboard controller path
 * handles everything, same as a desktop machine with no adapter plugged in.
 */

#ifndef HIDAPI_STUB_H
#define HIDAPI_STUB_H

#include <stddef.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hid_device_;
typedef struct hid_device_ hid_device;

struct hid_device_info {
	char *path;
	unsigned short vendor_id;
	unsigned short product_id;
	wchar_t *serial_number;
	unsigned short release_number;
	wchar_t *manufacturer_string;
	wchar_t *product_string;
	unsigned short usage_page;
	unsigned short usage;
	int interface_number;
	struct hid_device_info *next;
};

int hid_init(void);
int hid_exit(void);
struct hid_device_info *hid_enumerate(unsigned short vendor_id, unsigned short product_id);
void hid_free_enumeration(struct hid_device_info *devs);
hid_device *hid_open_path(const char *path);
void hid_close(hid_device *dev);
int hid_get_feature_report(hid_device *dev, unsigned char *data, size_t length);
int hid_send_feature_report(hid_device *dev, const unsigned char *data, size_t length);
const wchar_t *hid_error(hid_device *dev);

#ifdef __cplusplus
}
#endif

#endif /* HIDAPI_STUB_H */
