#ifndef SERVICE_SETTINGS_H
#define SERVICE_SETTINGS_H

#include "net.h"

/* <settings checks> */
enum service_user_default {
	SERVICE_USER_DEFAULT_NONE = 0,
	SERVICE_USER_DEFAULT_INTERNAL,
	SERVICE_USER_DEFAULT_LOGIN
};

enum service_type {
	SERVICE_TYPE_UNKNOWN,
	SERVICE_TYPE_LOG,
	SERVICE_TYPE_ANVIL,
	SERVICE_TYPE_CONFIG,
	SERVICE_TYPE_LOGIN,
	SERVICE_TYPE_STARTUP,
	/* Worker processes are intentionally limited to their process_limit,
	   and they can regularly reach it. There shouldn't be unnecessary
	   warnings about temporarily reaching the limit. */
	SERVICE_TYPE_WORKER,
};
/* </settings checks> */

struct file_listener_settings {
	const char *path;
	const char *type;
	unsigned int mode;
	const char *user;
	const char *group;
};
ARRAY_DEFINE_TYPE(file_listener_settings, struct file_listener_settings *);

struct inet_listener_settings {
	const char *name;
	const char *type;
	const char *address;
	in_port_t port;
	bool ssl;
	bool reuse_port;
	bool haproxy;
};
ARRAY_DEFINE_TYPE(inet_listener_settings, struct inet_listener_settings *);

struct service_settings {
	const char *name;
	const char *protocol;
	const char *type;
	const char *executable;
	const char *user;
	const char *group;
	const char *privileged_group;
	const char *extra_groups;
	const char *chroot;

	bool drop_priv_before_exec;

	unsigned int process_min_avail;
	unsigned int process_limit;
	unsigned int client_limit;
	unsigned int service_count;
	unsigned int idle_kill;
	uoff_t vsz_limit;

	ARRAY_TYPE(file_listener_settings) unix_listeners;
	ARRAY_TYPE(file_listener_settings) fifo_listeners;
	ARRAY_TYPE(inet_listener_settings) inet_listeners;

	/* internal to master: */
	struct master_settings *master_set;
	enum service_type parsed_type;
	enum service_user_default user_default;
	bool login_dump_core:1;

	/* -- flags that can be set internally -- */

	/* process_limit must not be higher than 1 */
	bool process_limit_1:1;
};
ARRAY_DEFINE_TYPE(service_settings, struct service_settings *);

#endif
