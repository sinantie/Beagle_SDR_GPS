/*
--------------------------------------------------------------------------------
This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Library General Public
License as published by the Free Software Foundation; either
version 2 of the License, or (at your option) any later version.
This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Library General Public License for more details.
You should have received a copy of the GNU Library General Public
License along with this library; if not, write to the
Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
Boston, MA  02110-1301, USA.
--------------------------------------------------------------------------------
*/

// Copyright (c) 2014-2016 John Seamons, ZL/KF6VO

#include "kiwi.h"
#include "types.h"
#include "config.h"
#include "misc.h"
#include "timer.h"
#include "web.h"
#include "coroutines.h"
#include "mongoose.h"
#include "nbuf.h"
#include "cfg.h"
#include "net.h"
#include "str.h"

#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <ifaddrs.h>

// we've seen the ident.me site respond very slowly at times, so do this in a separate task
// FIXME: this doesn't work if someone is using WiFi or USB networking because only "eth0" is checked

static void dyn_DNS(void *param)
{
	int i, n, status;
	bool noEthernet = false, noInternet = false;

	if (!do_dyn_dns)
		return;
		
	ddns.serno = serial_number;
	
	char buf[2048];
	
	for (i=0; i<1; i++) {	// hack so we can use 'break' statements below

		// get Ethernet interface MAC address
		//n = non_blocking_cmd("ifconfig eth0", buf, sizeof(buf), NULL);
		n = non_blocking_cmd("cat /sys/class/net/eth0/address", buf, sizeof(buf), &status);
		noEthernet = (status < 0 || WEXITSTATUS(status) != 0);
		if (!noEthernet && n > 0) {
			//n = sscanf(buf, "eth0 Link encap:Ethernet HWaddr %17s", ddns.mac);
			n = sscanf(buf, "%17s", ddns.mac);
			assert (n == 1);
		} else
			break;
		
		if (no_net) {
			noInternet = true;
			break;
		}
		
		// get our public IP with the assistance of ident.me
		// FIXME: should try other sites if ident.me is down or goes away
		// 31-dec-2016 ident.me domain went away!
		//  1-jan-2017 But then it came back a day later. So this just proves the point of
		// needing to be resilient to the variabilities of external websites.
		//n = non_blocking_cmd("curl -s ident.me", buf, sizeof(buf), &status);
		n = non_blocking_cmd("curl -s icanhazip.com", buf, sizeof(buf), &status);
		
		noInternet = (status < 0 || WEXITSTATUS(status) != 0);
		if (!noInternet && n > 0) {
			// FIXME: start using returned routine allocated buffers instead of fixed passed buffers
			//char *p;
			//i = sscanf(buf, "%ms", &p);
			i = sscanf(buf, "%s", ddns.ip_pub);
			check(i == 1);
			//kiwi_copy_terminate_free(p, ddns.ip_pub, sizeof(ddns.ip_pub));
			ddns.pub_valid = true;
		} else
			break;
	}
	
	if (ddns.serno == 0) lprintf("DDNS: no serial number?\n");
	if (noEthernet) lprintf("DDNS: no Ethernet interface active?\n");
	if (noInternet) lprintf("DDNS: no Internet access?\n");

	if (!find_local_IPs()) {
		lprintf("DDNS: no Ethernet interface IP addresses?\n");
		noEthernet = true;
	}

	if (ddns.pub_valid)
		lprintf("DDNS: public ip %s\n", ddns.ip_pub);

	// no Internet access or no serial number available, so no point in registering
	if (noEthernet || noInternet || ddns.serno == 0)
		return;
	
	// Attempt to open NAT port in local network router using UPnP (if router supports IGD).
	// Saves Kiwi owner the hassle of figuring out how to do this manually on their router.
	if (admcfg_bool("auto_add_nat", NULL, CFG_REQUIRED) == true) {
		char *cmd_p;
		asprintf(&cmd_p, "upnpc %s -a %s %d %d TCP 2>&1", (debian_ver != 7)? "-e KiwiSDR" : "",
			ddns.ip_pvt, ddns.port, ddns.port_ext);
		n = non_blocking_cmd(cmd_p, buf, sizeof(buf), &status);
		printf("%s\n", buf);

		if (status >= 0 && n > 0) {
			if (strstr(buf, "code 718")) {
				lprintf("### %s: NAT port mapping in local network firewall/router already exists\n", cmd_p);
				ddns.auto_nat = 3;
			} else
			if (strstr(buf, "is redirected to")) {
				lprintf("### %s: NAT port mapping in local network firewall/router created\n", cmd_p);
				ddns.auto_nat = 1;
			} else {
				lprintf("### %s: No IGD UPnP local network firewall/router found\n", cmd_p);
				lprintf("### %s: See kiwisdr.com for help manually adding a NAT rule on your firewall/router\n", cmd_p);
				ddns.auto_nat = 2;
			}
		} else {
			lprintf("### %s: command failed?\n", cmd_p);
			ddns.auto_nat = 4;
		}
		
		free(cmd_p);
	} else {
		lprintf("### auto NAT is set false\n");
		ddns.auto_nat = 0;
	}
	
	ddns.valid = true;
}


// routine that processes the output of the registration wget command

#define RETRYTIME_WORKED	20
#define RETRYTIME_FAIL		2

static int _reg_SDR_hu(void *param)
{
	nbcmd_args_t *args = (nbcmd_args_t *) param;
	int n = args->bc;
	char *sp = args->bp, *sp2;
	int retrytime_mins = args->func_param;

	if (n > 0 && (sp = strstr(args->bp, "UPDATE:")) != 0) {
		sp += 7;
		if (strncmp(sp, "SUCCESS", 7) == 0) {
			if (retrytime_mins != RETRYTIME_WORKED) lprintf("sdr.hu registration: WORKED\n");
			retrytime_mins = RETRYTIME_WORKED;
		} else {
			if ((sp2 = strchr(sp, '\n')) != NULL)
				*sp2 = '\0';
			lprintf("sdr.hu registration: \"%s\"\n", sp);
			retrytime_mins = RETRYTIME_FAIL;
		}
	} else {
		lprintf("sdr.hu registration: FAILED n=%d sp=%p <%.32s>\n", n, sp, sp);
		retrytime_mins = RETRYTIME_FAIL;
	}
	
	return retrytime_mins;
}

static void reg_SDR_hu(void *param)
{
	int n;
	char *cmd_p;
	int retrytime_mins = RETRYTIME_FAIL;
	
	// reply is a bunch of HTML, buffer has to be big enough not to miss/split status
	#define NBUF 16384
	
	const char *server_url = cfg_string("server_url", NULL, CFG_OPTIONAL);
	const char *api_key = admcfg_string("api_key", NULL, CFG_OPTIONAL);
	asprintf(&cmd_p, "wget --timeout=15 -qO- http://sdr.hu/update --post-data \"url=http://%s:%d&apikey=%s\" 2>&1",
		server_url, ddns.port, api_key);
	cfg_string_free(server_url);
	admcfg_string_free(api_key);

	while (1) {
		retrytime_mins = non_blocking_cmd_child(cmd_p, _reg_SDR_hu, retrytime_mins, NBUF);
		TaskSleepUsec(SEC_TO_USEC(MINUTES_TO_SEC(retrytime_mins)));
	}
	
	free(cmd_p);
	#undef NBUF
}

static int _reg_kiwisdr_com(void *param)
{
	return 0;
}

static void reg_kiwisdr_com(void *param)
{
	int n;
	char *cmd_p;
	int retrytime_mins = 30;
	
	// reply is a bunch of HTML, buffer has to be big enough not to miss/split status
	#define NBUF 256
	
	const char *server_url = cfg_string("server_url", NULL, CFG_OPTIONAL);
	const char *api_key = admcfg_string("api_key", NULL, CFG_OPTIONAL);
	const char *admin_email = cfg_string("admin_email", NULL, CFG_OPTIONAL);
	char *email = str_encode((char *) admin_email);
	cfg_string_free(admin_email);
	int add_nat = (admcfg_bool("auto_add_nat", NULL, CFG_OPTIONAL) == true)? 1:0;

	TaskSleepUsec(SEC_TO_USEC(10));		// long enough for ddns.mac to become valid

	while (1) {
		asprintf(&cmd_p, "wget --timeout=15 -qO- \"http://kiwisdr.com/php/update.php?url=http://%s:%d&apikey=%s&mac=%s&email=%s&add_nat=%d&ver=%d.%d&up=%d\" 2>&1",
			server_url, ddns.port, api_key, ddns.mac,
			email, add_nat, VERSION_MAJ, VERSION_MIN, timer_sec());
		non_blocking_cmd_child(cmd_p, _reg_kiwisdr_com, retrytime_mins, NBUF);
		free(cmd_p);
		TaskSleepUsec(SEC_TO_USEC(MINUTES_TO_SEC(retrytime_mins)));
	}
	
	cfg_string_free(server_url);
	admcfg_string_free(api_key);
	free(email);
	#undef NBUF
}

void services_start(bool restart)
{
	CreateTask(dyn_DNS, 0, WEBSERVER_PRIORITY);

	if (!no_net && !restart && !down && !alt_port && admcfg_bool("sdr_hu_register", NULL, CFG_PRINT) == true) {
		CreateTask(reg_SDR_hu, 0, WEBSERVER_PRIORITY);
		CreateTask(reg_kiwisdr_com, 0, WEBSERVER_PRIORITY);
	}
}
