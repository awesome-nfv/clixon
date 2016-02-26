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

 *
 * Protocol to communicate between clients (eg clicon_cli, clicon_netconf) 
 * and server (clicon_backend)
 */

#ifdef HAVE_CONFIG_H
#include "clicon_config.h" /* generated by config & autoconf */
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <assert.h>
#include <syslog.h>
#include <signal.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>

/* cligen */
#include <cligen/cligen.h>

/* clicon */
#include "clicon_err.h"
#include "clicon_log.h"
#include "clicon_queue.h"
#include "clicon_chunk.h"
#include "clicon_sig.h"
#include "clicon_proto.h"
#include "clicon_proto_encode.h"

static int _atomicio_sig = 0;

struct map_type2str{
    enum clicon_msg_type mt_type;
    char                *mt_str; /* string as in 4.2.4 in RFC 6020 */
};

/* Mapping between yang keyword string <--> clicon constants */
static const struct map_type2str msgmap[] = {
    {CLICON_MSG_COMMIT,       "commit"},
    {CLICON_MSG_VALIDATE,     "validate"},
    {CLICON_MSG_CHANGE,       "change"},
    {CLICON_MSG_SAVE,         "save"},
    {CLICON_MSG_LOAD,         "load"},
    {CLICON_MSG_COPY,         "copy"},
    {CLICON_MSG_RM,           "rm"},
    {CLICON_MSG_INITDB,       "initdb"},
    {CLICON_MSG_LOCK,         "lock"},
    {CLICON_MSG_UNLOCK,       "unlock"},
    {CLICON_MSG_KILL,         "kill"},
    {CLICON_MSG_DEBUG,        "debug"},
    {CLICON_MSG_CALL,         "call"},
    {CLICON_MSG_SUBSCRIPTION, "subscription"},
    {CLICON_MSG_OK,           "ok"},
    {CLICON_MSG_NOTIFY,       "notify"},
    {CLICON_MSG_ERR,          "err"},
    {-1,                      NULL}, 
};

static char *
msg_type2str(enum clicon_msg_type type)
{
    const struct map_type2str *mt;

    for (mt = &msgmap[0]; mt->mt_str; mt++)
	if (mt->mt_type == type)
	    return mt->mt_str;
    return NULL;
}

/*! Open local connection using unix domain sockets
 */
int
clicon_connect_unix(char *sockpath)
{
    struct sockaddr_un addr;
    int retval = -1;
    int s;

    if ((s = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	clicon_err(OE_CFG, errno, "socket");
	return -1;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sockpath, sizeof(addr.sun_path)-1);

    clicon_debug(2, "%s: connecting to %s", __FUNCTION__, addr.sun_path);
    if (connect(s, (struct sockaddr *)&addr, SUN_LEN(&addr)) < 0){
	if (errno == EACCES)
	    clicon_err(OE_CFG, errno, "connecting unix socket: %s.\n"
		       "Client should be member of group $CLICON_SOCK_GROUP: ", 
		       sockpath);
	else
	    clicon_err(OE_CFG, errno, "connecting unix socket: %s", sockpath);
	close(s);
	goto done;
    }
    retval = s;
  done:
    return retval;
}

static void
atomicio_sig_handler(int arg)
{
    _atomicio_sig++;
}


/*! Ensure all of data on socket comes through. fn is either read or write
 */
static ssize_t
atomicio(ssize_t (*fn) (int, void *, size_t), int fd, void *_s, size_t n)
{
    char *s = _s;
    ssize_t res, pos = 0;

    while (n > pos) {
	_atomicio_sig = 0;
	res = (fn)(fd, s + pos, n - pos);
	switch (res) {
	case -1:
	    if (errno == EINTR){
		if (!_atomicio_sig)
		    continue;
	    }
	    else
		if (errno == EAGAIN)
		    continue;
	case 0: /* fall thru */
	    return (res);
	default:
	    pos += res;
	}
    }
    return (pos);
}

static int
msg_dump(struct clicon_msg *msg)
{
    int  i;
    char buf[9*8];
    char buf2[9*8];
    
    memset(buf2, 0, sizeof(buf2));
    snprintf(buf2, sizeof(buf2), "%s:", __FUNCTION__);
    for (i=0; i<ntohs(msg->op_len); i++){
	snprintf(buf, sizeof(buf), "%s%02x", buf2, ((char*)msg)[i]&0xff);
	if ((i+1)%32==0){
	    clicon_debug(2, buf);
	    snprintf(buf, sizeof(buf), "%s:", __FUNCTION__);
	}
	else
	    if ((i+1)%4==0)
		snprintf(buf, sizeof(buf), "%s ", buf2);
	strncpy(buf2, buf, sizeof(buf2));
    }
    if (i%32)
	clicon_debug(2, buf);
    return 0;
}

int
clicon_msg_send(int s, struct clicon_msg *msg)
{ 
    int retval = -1;

    clicon_debug(2, "%s: send msg seq=%d len=%d", 
		 __FUNCTION__, ntohs(msg->op_type), ntohs(msg->op_len));
    if (debug > 2)
	msg_dump(msg);
    if (atomicio((ssize_t (*)(int, void *, size_t))write, 
		 s, msg, ntohs(msg->op_len)) < 0){
	clicon_err(OE_CFG, errno, "%s", __FUNCTION__);
	goto done;
    }
    retval = 0;
  done:
    return retval;
}


/*! Receive a CLICON message on a UNIX domain socket
 *
 * XXX: timeout? and signals?
 * There is rudimentary code for turning on signals and handling them 
 * so that they can be interrupted by ^C. But the problem is that this
 * is a library routine and such things should be set up in the cli 
 * application for example: a daemon calling this function will want another 
 * behaviour.
 * Now, ^C will interrupt the whole process, and this may not be what you want.
 *
 * @param[in]   s      UNIX domain socket to communicate with backend
 * @param[out]  msg    CLICON msg data reply structure. allocated using CLICON chunks, 
 *                     freed by caller with unchunk*(...,label)
 * @param[out]  eof    Set if eof encountered
 * @param[in]   label  Label used in chunk allocation and deallocation.
 * Note: caller must ensure that s is closed if eof is set after call.
 */
int
clicon_msg_rcv(int                s,
	      struct clicon_msg **msg,
	      int                *eof,
	      const char         *label)
{ 
    int       retval = -1;
    struct clicon_msg hdr;
    int       hlen;
    int       len2;
    sigfn_t   oldhandler;
    uint16_t  mlen;

    *eof = 0;
    if (0)
	set_signal(SIGINT, atomicio_sig_handler, &oldhandler);

    if ((hlen = atomicio(read, s, &hdr, sizeof(hdr))) < 0){ 
	clicon_err(OE_CFG, errno, "%s", __FUNCTION__);
	goto done;
    }
    if (hlen == 0){
	retval = 0;
	*eof = 1;
	goto done;
    }
    if (hlen != sizeof(hdr)){
	clicon_err(OE_CFG, errno, "%s: header too short (%d)", __FUNCTION__, hlen);
	goto done;
    }
    mlen = ntohs(hdr.op_len);
    clicon_debug(2, "%s: rcv msg seq=%d, len=%d",  
		 __FUNCTION__, ntohs(hdr.op_type), mlen);
    if ((*msg = (struct clicon_msg *)chunk(mlen, label)) == NULL){
	clicon_err(OE_CFG, errno, "%s: chunk", __FUNCTION__);
	goto done;
    }
    memcpy(*msg, &hdr, hlen);
    if ((len2 = read(s, (*msg)->op_body, mlen - sizeof(hdr))) < 0){
	clicon_err(OE_CFG, errno, "%s: read", __FUNCTION__);
	goto done;
    }
    if (len2 != mlen - sizeof(hdr)){
	clicon_err(OE_CFG, errno, "%s: body too short", __FUNCTION__);
	goto done;
    }
    if (debug > 1)
	msg_dump(*msg);
    retval = 0;
  done:
    if (0)
	set_signal(SIGINT, oldhandler, NULL);
    return retval;
}


/*! Connect to server, send an clicon_msg message and wait for result.
 * Compared to clicon_rpc, this is a one-shot rpc: open, send, get reply and close.
 * NOTE: this is dependent on unix domain
 */
int
clicon_rpc_connect_unix(struct clicon_msg *msg, 
			char              *sockpath,
			char             **data, 
			uint16_t          *datalen,
			int               *sock0, 
			const char        *label)
{
    int retval = -1;
    int s = -1;
    struct stat sb;

    clicon_debug(1, "Send %s msg on %s", 
		 msg_type2str(ntohs(msg->op_type)), sockpath);
    /* special error handling to get understandable messages (otherwise ENOENT) */
    if (stat(sockpath, &sb) < 0){
	clicon_err(OE_PROTO, errno, "%s: config daemon not running?", sockpath);
	goto done;
    }
    if (!S_ISSOCK(sb.st_mode)){
	clicon_err(OE_PROTO, EIO, "%s: Not unix socket", sockpath);
	goto done;
    }
    if ((s = clicon_connect_unix(sockpath)) < 0)
	goto done;
    if (clicon_rpc(s, msg, data, datalen, label) < 0)
	goto done;
    if (sock0 != NULL)
	*sock0 = s;
    retval = 0;
  done:
    if (sock0 == NULL && s >= 0)
	close(s);
    return retval;
}

/*! Connect to server, send an clicon_msg message and wait for result using an inet socket
 * Compared to clicon_rpc, this is a one-shot rpc: open, send, get reply and close.
 */
int
clicon_rpc_connect_inet(struct clicon_msg *msg, 
			char              *dst,
			uint16_t           port,
			char             **data, 
			uint16_t          *datalen,
			int               *sock0, 
			const char        *label)
{
    int retval = -1;
    int s = -1;
    struct sockaddr_in addr;

    clicon_debug(1, "Send %s msg to %s:%hu", 
		 msg_type2str(ntohs(msg->op_type)), dst, port);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(addr.sin_family, dst, &addr.sin_addr) != 1)
	goto done; /* Could check getaddrinfo */
    
    /* special error handling to get understandable messages (otherwise ENOENT) */
    if ((s = socket(addr.sin_family, SOCK_STREAM, 0)) < 0) {
	clicon_err(OE_CFG, errno, "socket");
	return -1;
    }
    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0){
	clicon_err(OE_CFG, errno, "connecting socket inet4");
	close(s);
	goto done;
    }
    if (clicon_rpc(s, msg, data, datalen, label) < 0)
	goto done;
    if (sock0 != NULL)
	*sock0 = s;
    retval = 0;
  done:
    if (sock0 == NULL && s >= 0)
	close(s);
    return retval;
}

/*! Send a clicon_msg message and wait for result.
 *
 * TBD: timeout, interrupt?
 * retval may be -1 and
 * errno set to ENOTCONN which means that socket is now closed probably
 * due to remote peer disconnecting. The caller may have to do something,...
 *
 * @param[in]  s       Socket to communicate with backend
 * @param[in]  msg     CLICON msg data structure. It has fixed header and variable body.
 * @param[out] data    Returned data as byte-strin exclusing header. 
 *                      Deallocate w unchunk...(..., label)
 * @param[out] datalen Length of returned data
 * @param[in]  label   Label used in chunk allocation.
 */
int
clicon_rpc(int                s, 
	   struct clicon_msg *msg, 
	   char             **data, 
	   uint16_t          *datalen,
	   const char        *label)
{
    int                retval = -1;
    struct clicon_msg *reply;
    int                eof;
    uint32_t           err;
    uint32_t           suberr;
    char              *reason;
    enum clicon_msg_type type;

    if (clicon_msg_send(s, msg) < 0)
	goto done;
    if (clicon_msg_rcv(s, &reply, &eof, label) < 0)
	goto done;
    if (eof){
	clicon_err(OE_PROTO, ESHUTDOWN, "%s: Socket unexpected close", __FUNCTION__);
	close(s);
	errno = ESHUTDOWN;
	goto done;
    }
    type = ntohs(reply->op_type);
    switch (type){
    case CLICON_MSG_OK:
        if (data != NULL) {
	    *data = reply->op_body;
	    *datalen = ntohs(reply->op_len) - sizeof(*reply);
	}
	break;
    case CLICON_MSG_ERR:
	if (clicon_msg_err_decode(reply, &err, &suberr, &reason, label) < 0) 
	    goto done;
	clicon_err(err, suberr, "%s", reason);
	goto done;
	break;
    default:
	clicon_err(OE_PROTO, 0, "%s: unexpected reply: %d", 
		__FUNCTION__, type);
	goto done;
	break;
    }
    retval = 0;
  done:
    return retval;
}

int 
send_msg_reply(int      s, 
	       uint16_t type, 
	       char    *data, 
	       uint16_t datalen)
{
    int                retval = -1;
    struct clicon_msg *reply;
    uint16_t           len;

    len = sizeof(*reply) + datalen;
    if ((reply = (struct clicon_msg *)chunk(len, __FUNCTION__)) == NULL)
	goto done;
    memset(reply, 0, len);
    reply->op_type = htons(type);
    reply->op_len = htons(len);
    if (datalen > 0)
      memcpy(reply->op_body, data, datalen);
    if (clicon_msg_send(s, reply) < 0)
	goto done;
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

int
send_msg_ok(int s)
{
    return send_msg_reply(s, CLICON_MSG_OK, NULL, 0);
}

int
send_msg_notify(int s, int level, char *event)
{
    int retval = -1;
    struct clicon_msg *msg;

    if ((msg=clicon_msg_notify_encode(level, event, __FUNCTION__)) == NULL)
	goto done;
    if (clicon_msg_send(s, msg) < 0)
	goto done;
    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

int
send_msg_err(int s, int err, int suberr, char *format, ...)
{
    va_list args;
    char *reason;
    int len;
    int retval = -1;
    struct clicon_msg *msg;

    va_start(args, format);
    len = vsnprintf(NULL, 0, format, args) + 1;
    va_end(args);
    if ((reason = (char *)chunk(len, __FUNCTION__)) == NULL)
	return -1;
    memset(reason, 0, len);
    va_start(args, format);
    vsnprintf(reason, len, format, args);
    va_end(args);
    if ((msg=clicon_msg_err_encode(clicon_errno, clicon_suberrno, 
				   reason, __FUNCTION__)) == NULL)
	goto done;
    if (clicon_msg_send(s, msg) < 0)
	goto done;

    retval = 0;
  done:
    unchunk_group(__FUNCTION__);
    return retval;
}

