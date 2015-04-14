/*
 ============================================================================
 Name        : usbauth.c
 Author      : Stefan Koch
 Version     : alpha
 Copyright   : 2015 SUSE Linux GmbH
 Description : USB authentication for udev
 ============================================================================
 */

#include "usbauth.h"
#include "../../usbauth_configparser/src/usbauth_configparser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define LOG_FILE "/var/log/usbauth.log"

struct kernels {
	uint8_t busnum;
	uint8_t devpath;
	uint8_t confignum;
	uint8_t intfnum;
};

struct ser {
	bool allowed;
	uint8_t cl;
	uint16_t vId;
	uint16_t pId;
	uint8_t busnum;
	uint8_t devpath;
};

static FILE *logfile = NULL;

static struct udev *udev = NULL;
DBusConnection *bus = NULL;

const char* parameter_strings[] = {"INVALID", "busnum", "devpath", "idVendor", "idProduct", "bDeviceClass", "bDeviceSubClass", "bDeviceProtocol", "bConfigurationValue", "bInterfaceNumber", "bInterfaceClass", "bInterfaceSubClass", "bInterfaceProtocol", "count"};
const char* operator_strings[] = {"==", "!=", "<=", ">=", "<", ">"};

unsigned get_param_val(enum Parameter param, struct udev_device *udevdev) {
	unsigned val = 0;
	struct udev_device *parent = NULL;
	const char* paramStr = param_to_str(param);
	const char* valStr = NULL;

	if(udevdev)
		valStr = udev_device_get_sysattr_value(udevdev, paramStr);

	if(!valStr) {
		parent = udev_device_get_parent(udevdev);
		valStr = udev_device_get_sysattr_value(parent, paramStr);
	}

	if(valStr)
		val = strtoul(valStr, NULL, 16);

	return val;
}

bool match_vals(unsigned lval, enum Operator op, unsigned rval) {
	bool ret = true;

	if (op == eq && !(lval == rval))
		ret = false;
	else if (op == neq && !(lval != rval))
		ret = false;
	else if (op == lt && !(lval <= rval))
		ret = false;
	else if (op == gt && !(lval >= rval))
		ret = false;
	else if (op == l && !(lval < rval))
		ret = false;
	else if (op == g && !(lval > rval))
		ret = false;

	return ret;
}

void serialize_dbus_error_check(DBusError *error) {
	if(dbus_error_is_set(error)) {
		printf("error %s", error->message);
		dbus_error_free(error);
	}
}

/**
 * send dbus message to notifier service for interface
 *
 * @udevdev: udev_device with type "usb_interface"
 * @authorize: true for allow, false for deny
 *
 */
void serialize_dbus(struct udev_device *udevdev, bool authorize) {
	const char *path = udev_device_get_syspath(udevdev);
	//dbus_bus_request_name(bus, "test.signal.source", DBUS_NAME_FLAG_REPLACE_EXISTING, &error);

	DBusMessage *msg = dbus_message_new_signal("/test/signal/Object", "test.signal.Type", "Test");
	if(!msg)
		printf("NULL");

	dbus_message_append_args(msg, DBUS_TYPE_BOOLEAN, &authorize, DBUS_TYPE_STRING, &path, DBUS_TYPE_INVALID);
	fprintf(logfile, "dbus%s\n", path);
	dbus_connection_send(bus, msg, NULL);
	dbus_connection_flush(bus);
	dbus_message_unref(msg);
	msg=NULL;
}

void authorize_interface(struct udev_device *udevdev, bool authorize, bool dbus) {
	const char* path = udev_device_get_devpath(udevdev);
	const char *type = udev_device_get_devtype(udevdev);
	unsigned cl = strtoul(udev_device_get_sysattr_value(udevdev, "bInterfaceClass"), NULL, 16);
	bool value = authorize ? true : false;

	if (!path || !type || strcmp(type, "usb_interface") != 0)
		return;

	fprintf(logfile, "USB Interface with class %02x\n", cl);

	char v[16];
	strcpy(v, "");
	snprintf(v, 16, "%" SCNu8, value);
	fprintf(logfile, "/sys%s/interface_authorized %" SCNu8 "\n", path, value);

	if (dbus)
		serialize_dbus(udevdev, authorize);

	udev_device_set_sysattr_value(udevdev, "interface_authorized", v);
}

void authorize_mask(struct udev_device *udevdev, uint32_t mask, bool dbus) {
	const char* path = udev_device_get_devpath(udevdev);
	const char *type = udev_device_get_devtype(udevdev);
	unsigned cl = strtoul(udev_device_get_sysattr_value(udevdev, "bDeviceClass"), NULL, 16);

	if (!path || !type || strcmp(type, "usb_device") != 0)
		return;

	fprintf(logfile, "USB Interface with class %02x\n", cl);
	char v[16];
	strcpy(v, "");
	snprintf(v, 16, "%" SCNx32, mask);
	fprintf(logfile, "/sys%s/interface_authorization_mask %" SCNx32 "\n", path, mask);

	udev_device_set_sysattr_value(udevdev, "interface_authorization_mask", v);

	if (dbus) {
		struct udev_list_entry *devices = NULL, *entry = NULL;
		struct udev_enumerate *enumerate = NULL;
		unsigned dev_class = 0;

		enumerate = udev_enumerate_new(udev);

		if(!enumerate)
			return;

		udev_enumerate_add_match_parent(enumerate, udevdev);
		udev_enumerate_scan_devices(enumerate);
		devices = udev_enumerate_get_list_entry(enumerate);

		if(!devices)
			return;

		// get the current mask from sysfs, because unmatched interfaces should be unchanged
		dev_class = get_param_val(bDeviceClass, udevdev);

		// iterate over the childs (usb_interface's) of the udevdev (usb_device)
		udev_list_entry_foreach(entry, devices)
		{
			const char *path = NULL;
			struct udev_device *interface = NULL;
			const char *type = NULL;

			if (entry)
				path = udev_list_entry_get_name(entry);

			if (path)
				interface = udev_device_new_from_syspath(udev, path);

			if (interface)
				type = udev_device_get_devtype(interface);

			if (type && strcmp(type, "usb_interface") == 0) {
				unsigned intf_class = get_param_val(bInterfaceClass, interface);
				if(dev_class == 9 && intf_class != 9) // dev class is HUB and intf class is not HUB
					continue; // skip device childs from hubs, use only hub's interfaces

				uint8_t nr = get_param_val(bInterfaceNumber, interface);
				serialize_dbus(interface, mask | (1 << nr) ? true : false);
			}

			if (interface)
				udev_device_unref(interface);
		}

		udev_enumerate_unref(enumerate);
	}
}

bool isRule(struct Auth *array, unsigned array_length) {
	bool ret = false;

	int i = 0;
	for (i = 0; i < array_length; i++) {
		if (array[i].type == ALLOW || array[i].type == DENY)
			ret = true;
	}

	return ret;
}

struct match_ret match_auth_interface(struct Auth *rule, struct udev_device *interface) {
	int i;
	struct match_ret ret;
	ret.match_attrs = true;
	ret.match_conds = true;

	if(!rule || !interface || !rule->valid || rule->type == COMMENT) {
		ret.match_attrs = false;
		ret.match_conds = false;
		return ret;
	}

	// iterate over the data structures (case parameters) from the auth structure
	// to check if the auth rule matches the cases
	for (i = 0; i < rule->attr_len; i++) {
		struct Data *d = &rule->attr_array[i];
		unsigned val = 0;

		if(!d) {
			ret.match_attrs = false;
			ret.match_conds = false;
			return ret;
		}

		val = get_param_val(d->param, interface);

		if(count == d->param) // count parameter is not in udev device
			val = rule->count;

		ret.match_attrs &= match_vals(val, d->op, d->val);
	}

	// iterate over the data structures (condition parameters) from the auth structure
	// to check if the auth rule matches the conditions
	for (i = 0; i < rule->cond_len && ret.match_attrs; i++) {
		struct Data *d = &rule->cond_array[i];
		unsigned val = 0;

		if(!d) {
			ret.match_attrs = false;
			ret.match_conds = false;
			return ret;
		}

		if(count == d->param) // count parameter is not in udev device
			val = rule->count;

		ret.match_conds &= match_vals(val, d->op, d->val);
	}

	return ret;
}

struct auth_ret match_auths_interface(struct Auth *rule_array, size_t array_len, struct udev_device *usb_interface) {

	int i;
	struct auth_ret ret;
	ret.match = false;
	ret.allowed = false;

	// iterate over the rules without conditions from the auth array
	// for each rule that attributes matches with the given interface
	for (i = 0; i < array_len; i++) {
		bool ruleApplicable = match_auth_interface(&rule_array[i], usb_interface).match_attrs; // true if interface is affected by rule
		if (rule_array[i].type != COND && ruleApplicable) {
			int j = 0;

			// iterate only over the conditions from the auth array
			// to check whether the auth rule matches the conditions
			for (j = 0; j < array_len; j++) {
				// conditions affecting only ALLOW rules
				if (rule_array[j].type == COND && rule_array[i].type == ALLOW) {
					struct match_ret r = match_auth_interface(&rule_array[j], usb_interface);
					// if the condition belongs to the interface (match_attrs, case parts in config file)
					// AND the condition is fulfilled (match_conds, condition parts in config file)
					if (r.match_attrs && r.match_conds) {
						rule_array[j].count++; // count affects r.match_conds
						unsigned u = rule_array[j].count;
						fprintf(logfile, "cc %i  %u\n", j, u);
					} else if (r.match_attrs && !r.match_conds) // only if the condition belongs to the interface and the condition is not fulfilled
						ruleApplicable = false; // condition conflicts with affected rule then ignore the rule
				}
			}

			if (ruleApplicable) { // if current/iterated interface matched rule and was not disabled by conflicting condition
				rule_array[i].count++; // describes how much interfaces are affected by the rule

				unsigned u = rule_array[i].count;
				fprintf(logfile, "dd %i  %u\n", i, u);

				ret.match |= true;
				ret.allowed = rule_array[i].type == ALLOW ? true : false; // allow or deny usb_interface
				fprintf(logfile, "ff %i  %i\n", ret.match, ret.allowed);
			}
		}
	}
	return ret;
}

void match_auths_device_interfaces(struct Auth *rule_array, size_t array_len, struct udev_device *usb_device, bool prepare) {
	const char *type = udev_device_get_devtype(usb_device);
	const char *path = udev_device_get_syspath(usb_device);
	struct udev_list_entry *devices = NULL, *entry = NULL;
	struct udev_enumerate *enumerate = NULL;
	unsigned dev_class = 0;
	uint32_t mask = 0;

	if(!path || !type)
		return;

	if (strcmp(type, "usb_device") != 0)
		return;

	fprintf(logfile, "DEV %s %s\n", path, type);

	enumerate = udev_enumerate_new(udev);

	if(!enumerate)
		return;

	udev_enumerate_add_match_parent(enumerate, usb_device);
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);

	if(!devices)
		return;

	// get the current mask from sysfs, because unmatched interfaces should be unchanged
	dev_class = get_param_val(bDeviceClass, usb_device);
	const char* maskStr = udev_device_get_sysattr_value(usb_device, "interface_authorization_mask");
	if (maskStr)
		mask = strtoul(maskStr, NULL, 16);

	// iterate over the childs (usb_interface's) of the udevdev (usb_device)
	udev_list_entry_foreach(entry, devices)
	{
		const char *path = NULL;
		struct udev_device *interface = NULL;
		const char *type = NULL;

		if (entry)
			path = udev_list_entry_get_name(entry);

		if (path)
			interface = udev_device_new_from_syspath(udev, path);

		if (interface)
			type = udev_device_get_devtype(interface);

		if (type && strcmp(type, "usb_interface") == 0) {
			unsigned intf_class = get_param_val(bInterfaceClass, interface);
			if(dev_class == 9 && intf_class != 9) // dev class is HUB and intf class is not HUB
				continue; // skip device childs from hubs, use only hub's interfaces

			fprintf(logfile, "path %s %s\n", path, type);

			struct auth_ret r = match_auths_interface(rule_array, array_len, interface);

			uint8_t nr = get_param_val(bInterfaceNumber, interface);

			// do only if one rule has matched, so if there would no generic rule and no specific rule do nothing
			if (r.match && r.allowed)
				mask |= (1 << nr);
			else if (r.match && !r.allowed)
				mask &= ~(1 << nr);
		}

		if (interface)
			udev_device_unref(interface);
	}

	udev_enumerate_unref(enumerate);

	// do nothing at preparing (e. g. count increments)
	if(!prepare)
		authorize_mask(usb_device, mask, true);
}

void perform_rules_devices(struct Auth *rule_array, size_t array_len, bool prepare) {
	struct udev_enumerate *enumerate;
	struct udev_list_entry *devices, *entry;

	enumerate = udev_enumerate_new(udev);

	if(!enumerate)
		return;

	udev_enumerate_add_match_subsystem(enumerate, "usb");
	udev_enumerate_scan_devices(enumerate);
	devices = udev_enumerate_get_list_entry(enumerate);

	if(!devices)
		return;

	// iterate over all USB devices
	udev_list_entry_foreach(entry, devices)
	{
		const char *path = NULL;
		struct udev_device *udevdev = NULL;
		const char *type = NULL;

		if (entry)
			path = udev_list_entry_get_name(entry);

		if (path)
			udevdev = udev_device_new_from_syspath(udev, path);

		if (udevdev)
			type = udev_device_get_devtype(udevdev);

		if (type && strcmp(type, "usb_device") == 0) // filter out interfaces, to avoid multiple iterations
			match_auths_device_interfaces(rule_array, array_len, udevdev, prepare);

		if (udevdev)
			udev_device_unref(udevdev);
	}

	udev_enumerate_unref(enumerate);
}

bool perform_rules_environment(struct Auth *rule_array, size_t array_len) {
	struct udev_device *udevdev = udev_device_new_from_environment(udev);

	if(!udevdev)
		return false;

	const char *path = udev_device_get_devpath(udevdev);
	const char *type = udev_device_get_devtype(udevdev);

	if(!path || !type)
		return false;

	if (strcmp(type, "usb_device") == 0) { // filter out interfaces because they will enumerated by called function
		fprintf(logfile, "path %s %s\n", path, type);
		unsigned cl = get_param_val(bDeviceClass, udevdev);
		fprintf(logfile, "class %x\n", cl);

		// check all interfaces of the device
		match_auths_device_interfaces(rule_array, array_len, udevdev, false);
	}

	udev_device_unref(udevdev);

	return true;
}

int main(int argc, char **argv) {
	udev = udev_new();

	DBusError error;
	dbus_error_init(&error);
	bus = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
	serialize_dbus_error_check(&error);

	logfile = fopen(LOG_FILE, "w");

	if(!logfile)
		logfile = stderr;

	usbauth_config_read();
	unsigned length;
	struct Auth *auths;
	usbauth_config_get_auths(&auths, &length);

	if(!isRule(auths, length)) {
		fprintf(logfile, "Config file not found or empty.\n");
	} else if (argc <= 1) { // called by udev
		perform_rules_devices(auths, length, true);
		perform_rules_environment(auths, length);
	} else if(strcmp(argv[1], "init") == 0) { // called manually with init parameter
		perform_rules_devices(auths, length, false);
	} else if (argc > 2 && (strcmp(argv[1], "allow") == 0 || strcmp(argv[1], "deny") == 0)) { // called by notifier
		struct udev_device *udevdev = udev_device_new_from_syspath(udev, argv[2]);
		if(udevdev) {
			bool allw = strcmp(argv[1], "allow") == 0 ? true : false;
			authorize_interface(udevdev, allw, false);
			udev_device_unref(udevdev);
		}
	}

	dbus_connection_unref(bus);
	bus=NULL;

	udev_unref(udev);
	udev = NULL;
	usbauth_config_free_auths(auths, length);

	return 0;
}
