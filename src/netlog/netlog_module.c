#include <linux/module.h>
#include <linux/init.h>
#include <linux/in.h>
#include <linux/version.h>
#include <linux/file.h>
#include <linux/unistd.h>
#include <linux/syscalls.h>
#include <linux/kallsyms.h>
#include "whitelist.h"
#include "probes.h"
#include "internal.h"
#include "netlog.h"

#ifdef NETLOG_V1_COMPAT
#include "compat_v1.h"
#endif /* NETLOG_V1_COMPAT */

/****************************************************************/
/* Kernel module information (submitted at the end of the file) */
/****************************************************************/

#define MOD_AUTHORS "Panos Sakkos <panos.sakkos@cern.ch>," \
		    "Vincent Brillault <vincent.brillault@cern.ch";
#define MOD_DESC "netlog logs information about every internet connection\n" \
		 "\t\tfrom and to the machine that is installed. This information\n" \
		 "\t\tis source/destination ips and ports, process name and pid,\n" \
		 "\t\tuid and the protocol (TCP/UDP)."
#define MOD_LICENSE "GPL"

/**********************************/
/*      MODULE PARAMETERS         */
/**********************************/

# if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
module_param_call(probes, &all_probes_param_set, &all_probes_param_get, NULL, 0600);
# else /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
module_param_cb(probes, &all_probes_param, NULL, 0600);
# endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(2, 6, 36) */
MODULE_PARM_DESC(probes, " Integer paramter describing which probes should be loaded");

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
# define DEFINE_PROBE_PARAM(name, pos)									\
module_param_call(probe_##name, &one_probe_param_set, &one_probe_param_get, probe_list + pos, 0600);	\
MODULE_PARM_DESC(probe_##name, " Integer paramter describing which probes should be loaded");
#else /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
# define DEFINE_PROBE_PARAM(name, pos)									\
module_param_cb(probe_##name, &one_probe_param, probe_list + pos, 0600);				\
MODULE_PARM_DESC(probe_##name, " Integer paramter describing which probes should be loaded");
#endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(2, 6, 36) */

DEFINE_PROBE_PARAM(tcp_connect, 0)
DEFINE_PROBE_PARAM(tcp_accept,  1)
DEFINE_PROBE_PARAM(tcp_close,   2)
DEFINE_PROBE_PARAM(udp_connect, 3)
DEFINE_PROBE_PARAM(udp_bind,    4)
DEFINE_PROBE_PARAM(udp_close,   5)

#if WHITELISTING
# if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
module_param_call(whitelist, &whitelist_param_set, &whitelist_param_get, NULL, 0600);
# else /* LINUX_VERSION_CODE > KERNEL_VERSION(2, 6, 36) */
module_param_cb(whitelist, &whitelist_param, NULL, 0600);
# endif /* LINUX_VERSION_CODE ? KERNEL_VERSION(2, 6, 36) */
MODULE_PARM_DESC(whitelist, " A coma separated list of strings that contains"
		 " the connections that " MODULE_NAME " will ignore.\n"
		 " The format of the string must be '${executable}|i<${ip}>|<${port}>'."
		 " The ip and port parts are optional.");
# ifdef NETLOG_V1_COMPAT
static int whitelist_length;
static char *connections_to_whitelist[MAX_WHITELIST_SIZE] = {NULL};

module_param_array(connections_to_whitelist, charp, &whitelist_length, 0000);
MODULE_PARM_DESC(connections_to_whitelist, " An array of strings that contains the connections that " MODULE_NAME " will ignore.\n"
					   "\t\tThe format of the string must be '/absolute/executable/path ip_address-port'");
# endif
#endif

#ifdef NETLOG_V1_COMPAT
static int absolute_path_mode;
module_param(absolute_path_mode, int, 0);
MODULE_PARM_DESC(absolute_path_mode, "This parameter is only present for backward compatibility, it is ignored");
#endif /* NETLOG_V1_COMPAT */

/************************************/
/*             INIT MODULE          */
/************************************/

static int __init netlog_init(void)
{
	int ret;

	pr_info("Light monitoring tool for inet connections by CERN Security Team\n");

	ret = probes_init();
	if (ret != 0) {
		unplant_all();
#if WHITELISTING
		destroy_whitelist();
#endif
	}

#if WHITELISTING
# ifdef NETLOG_V1_COMPAT
	ret = create_proc();
	if (ret != 0) {
		unplant_all();
		destroy_whitelist();
	}
	set_whitelist_from_array(connections_to_whitelist, whitelist_length);
# endif /* NETLOG_V1_COMPAT */
#endif /* WHITELISTING */

	return ret;
}

/************************************/
/*             EXIT MODULE          */
/************************************/

static void __exit netlog_exit(void)
{
	unplant_all();

#if WHITELISTING
# ifdef NETLOG_V1_COMPAT
	destroy_proc();
# endif /* NETLOG_V1_COMPAT */
	destroy_whitelist();
#endif /* WHITELISTING */
}


/*********************************************/
/* Register module functions and information */
/*********************************************/

module_init(netlog_init);
module_exit(netlog_exit);

MODULE_LICENSE(MOD_LICENSE);
MODULE_AUTHOR(MOD_AUTHORS);
MODULE_DESCRIPTION(MOD_DESC);
