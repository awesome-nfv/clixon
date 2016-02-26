/*
 *
  Copyright (C) 2009-2016 Olof Hagsand and Benny Holmgren

  This file is part of CLICON.

  CLICON is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 3 of the License, or
  (at your option) any later version.

  CLICON is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with CLICON; see the file LICENSE.  If not, see
  <http://www.gnu.org/licenses/>.

 */

#ifdef HAVE_CONFIG_H
#include "clicon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <syslog.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include <clicon/clicon.h>

#include "clicon_netconf.h"
#include "netconf_lib.h"
#include "netconf_hello.h"
#include "netconf_plugin.h"
#include "netconf_rpc.h"

/* Command line options to be passed to getopt(3) */
#define NETCONF_OPTS "hDqf:d:S"

/*! Process incoming packet 
 * @param[in]   h    Clicon handle
 * @param[in]   xf   Packet buffer
 */
static int
process_incoming_packet(clicon_handle h, 
			cbuf         *xf)
{
    char  *str;
    char  *str0;
    cxobj *xml_req = NULL; /* Request (in) */
    int    isrpc = 0;   /* either hello or rpc */
    cbuf  *xf_out;
    cbuf  *xf_err;
    cbuf  *xf1;

    clicon_debug(1, "RECV");
    clicon_debug(2, "%s: RCV: \"%s\"", __FUNCTION__, cbuf_get(xf));
    if ((str0 = strdup(cbuf_get(xf))) == NULL){
	clicon_log(LOG_ERR, "%s: strdup: %s", __FUNCTION__, strerror(errno));
	return -1;
    }
    str = str0;
    /* Parse incoming XML message */
    if (clicon_xml_parse_string(&str, &xml_req) < 0){
	if ((xf = cbuf_new()) == NULL){
	    netconf_create_rpc_error(xf, NULL, 
				     "operation-failed", 
				     "rpc", "error",
				     NULL,
				     NULL);

	    netconf_output(1, xf, "rpc-error");
	    cbuf_free(xf);
	}
	else
	    clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	free(str0);
	goto done;
    }
    free(str0);
    if (xpath_first(xml_req, "//rpc") != NULL){
        isrpc++;
    }
    else
        if (xpath_first(xml_req, "//hello") != NULL)
	    ;
        else{
            clicon_log(LOG_WARNING, "Invalid netconf msg: neither rpc or hello: dropp\
ed");
            goto done;
        }
    /* Initialize response buffers */
    if ((xf_out = cbuf_new()) == NULL){
	clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	goto done;
    }

    /* Create error buf */
    if ((xf_err = cbuf_new()) == NULL){
	clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	goto done;
    }
    netconf_ok_set(0);
    if (isrpc){
	if (netconf_rpc_dispatch(h, 
				 xml_req, 
				 xpath_first(xml_req, "//rpc"), 
				 xf_out, xf_err) < 0){
	    assert(cbuf_len(xf_err));
	    clicon_debug(1, "%s", cbuf_get(xf_err));
	    if (isrpc){
		if (netconf_output(1, xf_err, "rpc-error") < 0)
		    goto done;
	    }
	}
	else{
	    if ((xf1 = cbuf_new()) != NULL){
		if (netconf_create_rpc_reply(xf1, xml_req, cbuf_get(xf_out), netconf_ok_get()) < 0){
		    cbuf_free(xf_out);
		    cbuf_free(xf_err);
		    cbuf_free(xf1);
		    goto done;
		}
		if (netconf_output(1, xf1, "rpc-reply") < 0){
		    cbuf_reset(xf1);
		    netconf_create_rpc_error(xf1, xml_req, "operation-failed", 
					     "protocol", "error", 
					     NULL, cbuf_get(xf_err));
		    netconf_output(1, xf1, "rpc-error");
		    cbuf_free(xf_out);
		    cbuf_free(xf_err);
		    cbuf_free(xf1);
		    goto done;
		}
		cbuf_free(xf1);
	    }
	}
    }
    else{
	netconf_hello_dispatch(xml_req); /* XXX: return-value */
    }
    cbuf_free(xf_out);
    cbuf_free(xf_err);
  done:
    if (xml_req)
	xml_free(xml_req);
    return 0;
}

/*! Get netconf message: detect end-of-msg 
 * @param[in]   s    Socket where input arrived. read from this.
 * @param[in]   arg  Clicon handle.
 */
static int
netconf_input_cb(int   s, 
		 void *arg)
{
    clicon_handle h = arg;
    unsigned char buf[BUFSIZ];
    int           i;
    int           len;
    static cbuf  *xf; /* XXX: should use ce state? */
    int           xml_state = 0;
    int           retval = -1;

    if (xf == NULL)
	if ((xf = cbuf_new()) == NULL){
	    clicon_err(OE_XML, errno, "%s: cbuf_new", __FUNCTION__);
	    return retval;
	}
    memset(buf, 0, sizeof(buf));
    if ((len = read(s, buf, sizeof(buf))) < 0){
	if (errno == ECONNRESET)
	    len = 0; /* emulate EOF */
	else{
	    clicon_log(LOG_ERR, "%s: read: %s", __FUNCTION__, strerror(errno));
	    goto done;
	}
    } /* read */
    if (len == 0){ 	/* EOF */
	cc_closed++;
	close(s);
	retval = 0;
	goto done;
    }

    for (i=0; i<len; i++){
	if (buf[i] == 0)
	    continue; /* Skip NULL chars (eg from terminals) */
	cprintf(xf, "%c", buf[i]);
	if (detect_endtag("]]>]]>",
			  buf[i],
			  &xml_state)) {
	    /* OK, we have an xml string from a client */
	    if (process_incoming_packet(h, xf) < 0){
		goto done;
	    }
	    if (cc_closed)
		break;
	    cbuf_reset(xf);
	}
    }
    retval = 0;
  done:
    //    cbuf_free(xf);
    if (cc_closed) 
	retval = -1;
    return retval;
}

/*
 * send_hello
 * args: s file descriptor to write on (eg 1 - stdout)
 */
static int
send_hello(int s)
{
    cbuf *xf;
    int retval = -1;
    
    if ((xf = cbuf_new()) == NULL){
	clicon_log(LOG_ERR, "%s: cbuf_new", __FUNCTION__);
	goto done;
    }
    if (netconf_create_hello(xf, getpid()) < 0)
	goto done;
    if (netconf_output(s, xf, "hello") < 0)
	goto done;
    retval = 0;
  done:
    if (xf)
	cbuf_free(xf);
    return retval;
}

/* from init_candidate_db() and clicon_rpc_copy() */
static int
init_candidate_db(clicon_handle h, char *running_db, char *candidate_db)
{
    struct stat      sb;
    int                retval = -1;

    /* init shared candidate */
    if (lstat(candidate_db, &sb) < 0){
	if (clicon_rpc_copy(h, running_db, candidate_db) < 0)
	    goto done;
    }
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

static int
terminate(clicon_handle h)
{
    yang_spec      *yspec;

    if ((yspec = clicon_dbspec_yang(h)) != NULL)
	yspec_free(yspec);
    clicon_handle_exit(h);
    return 0;
}


/*
 * usage
 */
static void
usage(char *argv0, clicon_handle h)
{
    char *netconfdir = clicon_netconf_dir(h);

    fprintf(stderr, "usage:%s\n"
	    "where options are\n"
            "\t-h\t\tHelp\n"
            "\t-D\t\tDebug\n"
            "\t-q\t\tQuiet: dont send hello prompt\n"
    	    "\t-f <file>\tConfiguration file (mandatory)\n"
	    "\t-d <dir>\tSpecify netconf plugin directory dir (default: %s)\n"
	    "\t-S\t\tLog on syslog\n",
	    argv0,
	    netconfdir
	    );
    exit(0);
}

int
main(int argc, char **argv)
{
    char             c;
    char            *tmp;
    char            *argv0 = argv[0];
    int              quiet = 0;
    clicon_handle    h;
    int              use_syslog;
    char              *running_db;
    char              *candidate_db;

    /* Defaults */
    use_syslog = 0;

    /* In the startup, logs to stderr & debug flag set later */
    clicon_log_init(__PROGRAM__, LOG_INFO, CLICON_LOG_STDERR); 
    /* Create handle */
    if ((h = clicon_handle_init()) == NULL)
	return -1;

    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	    usage(argv[0], h);
	    break;
	case 'D' : /* debug */
	    debug = 1;
	    break;
	 case 'f': /* override config file */
	    if (!strlen(optarg))
		usage(argv[0], h);
	    clicon_option_str_set(h, "CLICON_CONFIGFILE", optarg);
	    break;
	 case 'S': /* Log on syslog */
	     use_syslog = 1;
	     break;
	}

    /* 
     * Logs, error and debug to stderr or syslog, set debug level
     */
    clicon_log_init(__PROGRAM__, debug?LOG_DEBUG:LOG_INFO, 
		    use_syslog?CLICON_LOG_SYSLOG:CLICON_LOG_STDERR); 
    clicon_debug_init(debug, NULL); 

    /* Find and read configfile */
    if (clicon_options_main(h) < 0)
	return -1;

    /* Now rest of options */
    optind = 1;
    opterr = 0;
    while ((c = getopt(argc, argv, NETCONF_OPTS)) != -1)
	switch (c) {
	case 'h' : /* help */
	case 'D' : /* debug */
	case 'f': /* config file */
	case 'S': /* Log on syslog */
	    break; /* see above */
	case 'q':  /* quiet: dont write hello */
	    quiet++;
	    break;
	case 'd':  /* Plugin directory */
	    if (!strlen(optarg))
		usage(argv[0], h);
	    clicon_option_str_set(h, "CLICON_NETCONF_DIR", optarg);
	    break;
	default:
	    usage(argv[0], h);
	    break;
	}
    argc -= optind;
    argv += optind;

    /* Parse db spec file */
    if (yang_spec_main(h, stdout, 0) < 0)
	goto done;

    /* Initialize plugins group */
    if (netconf_plugin_load(h) < 0)
	return -1;

    if ((running_db = clicon_running_db(h)) == NULL){
	clicon_err(OE_FATAL, 0, "running db not set");
	goto done;
    }
    if ((candidate_db = clicon_candidate_db(h)) == NULL){
	clicon_err(OE_FATAL, 0, "candidate db not set");
	goto done;
    }
    if (init_candidate_db(h, running_db, candidate_db) < 0)
	return -1;
    /* Call start function is all plugins before we go interactive */
    tmp = *(argv-1);
    *(argv-1) = argv0;
    netconf_plugin_start(h, argc+1, argv-1);
    *(argv-1) = tmp;

    if (!quiet)
	send_hello(1);
    if (event_reg_fd(0, netconf_input_cb, h, "netconf socket") < 0)
	goto done;
    if (debug)
	clicon_option_dump(h, debug);

    if (event_loop() < 0)
	goto done;
  done:

    netconf_plugin_unload(h);
    terminate(h);
    clicon_log_init(__PROGRAM__, LOG_INFO, 0); /* Log on syslog no stderr */
    clicon_log(LOG_NOTICE, "%s: %u Terminated\n", __PROGRAM__, getpid());
    return 0;
}
