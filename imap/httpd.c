/* httpd.c -- HTTP/WebDAV/CalDAV server protocol parsing
 *
 * Copyright (c) 1994-2011 Carnegie Mellon University.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * 3. The name "Carnegie Mellon University" must not be used to
 *    endorse or promote products derived from this software without
 *    prior written permission. For permission or any legal
 *    details, please contact
 *      Carnegie Mellon University
 *      Center for Technology Transfer and Enterprise Creation
 *      4615 Forbes Avenue
 *      Suite 302
 *      Pittsburgh, PA  15213
 *      (412) 268-7393, fax: (412) 268-7395
 *      innovation@andrew.cmu.edu
 *
 * 4. Redistributions of any form whatsoever must retain the following
 *    acknowledgment:
 *    "This product includes software developed by Computing Services
 *     at Carnegie Mellon University (http://www.cmu.edu/computing/)."
 *
 * CARNEGIE MELLON UNIVERSITY DISCLAIMS ALL WARRANTIES WITH REGARD TO
 * THIS SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS, IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY BE LIABLE
 * FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN
 * AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <config.h>


#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/param.h>
#include <syslog.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ctype.h>
#include "prot.h"

#include <sasl/sasl.h>
#include <sasl/saslutil.h>

#include "httpd.h"
#include "http_proxy.h"

#include "assert.h"
#include "util.h"
#include "iptostring.h"
#include "global.h"
#include "tls.h"
#include "map.h"

#include "exitcodes.h"
#include "imapd.h"
#include "imap_err.h"
#include "http_err.h"
#include "version.h"
#include "xstrlcpy.h"
#include "xstrlcat.h"
#include "sync_log.h"
#include "telemetry.h"
#include "backend.h"
#include "proxy.h"
#include "userdeny.h"
#include "message.h"
#include "idle.h"
#include "rfc822date.h"
#include "tok.h"

#ifdef WITH_DAV
#include "http_dav.h"
#endif

#include <libxml/tree.h>
#include <libxml/HTMLtree.h>
#include <libxml/uri.h>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif /* HAVE_ZLIB */


#define DEBUG 1


static const char tls_message[] =
    HTML_DOCTYPE
    "<html>\n<head>\n<title>TLS Required</title>\n</head>\n" \
    "<body>\n<h2>TLS is required to use Basic authentication</h2>\n" \
    "Use <a href=\"%s\">%s</a> instead.\n" \
    "</body>\n</html>\n";

extern int optind;
extern char *optarg;
extern int opterr;


#ifdef HAVE_SSL
static SSL *tls_conn;
#endif /* HAVE_SSL */

sasl_conn_t *httpd_saslconn; /* the sasl connection context */

static struct mailbox *httpd_mailbox = NULL;
int httpd_timeout, httpd_keepalive;
char *httpd_userid = 0;
struct auth_state *httpd_authstate = 0;
int httpd_userisadmin = 0;
int httpd_userisproxyadmin = 0;
struct sockaddr_storage httpd_localaddr, httpd_remoteaddr;
int httpd_haveaddr = 0;
char httpd_clienthost[NI_MAXHOST*2+1] = "[local]";
struct protstream *httpd_out = NULL;
struct protstream *httpd_in = NULL;
struct protgroup *protin = NULL;
static int httpd_logfd = -1;

static sasl_ssf_t extprops_ssf = 0;
int https = 0;
int httpd_tls_done = 0;
int httpd_tls_required = 0;
unsigned avail_auth_schemes = 0; /* bitmask of available auth schemes */
unsigned long config_httpmodules;

struct buf serverinfo = BUF_INITIALIZER;

static void digest_send_success(const char *name __attribute__((unused)),
				const char *data)
{
    prot_printf(httpd_out, "Authentication-Info: %s\r\n", data);
}

/* List of HTTP auth schemes that we support */
struct auth_scheme_t auth_schemes[] = {
    { AUTH_BASIC, "Basic", NULL, AUTH_SERVER_FIRST | AUTH_BASE64, NULL, NULL },
    { AUTH_DIGEST, "Digest", HTTP_DIGEST_MECH, AUTH_NEED_BODY|AUTH_SERVER_FIRST,
      &digest_send_success, digest_recv_success },
    { AUTH_SPNEGO, "Negotiate", "GSS-SPNEGO", AUTH_BASE64, NULL, NULL },
    { AUTH_NTLM, "NTLM", "NTLM", AUTH_NEED_PERSIST | AUTH_BASE64, NULL, NULL },
    { -1, NULL, NULL, 0, NULL, NULL }
};


/* the sasl proxy policy context */
static struct proxy_context httpd_proxyctx = {
    0, 1, &httpd_authstate, &httpd_userisadmin, &httpd_userisproxyadmin
};

/* signal to config.c */
const int config_need_data = CONFIG_NEED_PARTITION_DATA;

/* current namespace */
struct namespace httpd_namespace;

/* PROXY STUFF */
/* we want a list of our outgoing connections here and which one we're
   currently piping */

/* the current server most commands go to */
struct backend *backend_current = NULL;

/* our cached connections */
struct backend **backend_cached = NULL;

/* end PROXY stuff */

static void starttls(int https);
void usage(void);
void shut_down(int code) __attribute__ ((noreturn));

extern void setproctitle_init(int argc, char **argv, char **envp);
extern int proc_register(const char *progname, const char *clienthost, 
			 const char *userid, const char *mailbox);
extern void proc_cleanup(void);

/* Enable the resetting of a sasl_conn_t */
static int reset_saslconn(sasl_conn_t **conn);

static void cmdloop(void);
static int parse_expect(struct transaction_t *txn);
static void parse_connection(struct transaction_t *txn, int *tls_upgrade);
static struct accept *parse_accept(const char **hdr);
static int http_auth(const char *creds, struct transaction_t *txn);
static void keep_alive(int sig);

static int meth_propfind_root(struct transaction_t *txn, void *params);


static struct {
    char *ipremoteport;
    char *iplocalport;
    sasl_ssf_t ssf;
    char *authid;
} saslprops = {NULL,NULL,0,NULL};

static struct sasl_callback mysasl_cb[] = {
    { SASL_CB_GETOPT, (mysasl_cb_ft *) &mysasl_config, NULL },
    { SASL_CB_PROXY_POLICY, (mysasl_cb_ft *) &mysasl_proxy_policy, (void*) &httpd_proxyctx },
    { SASL_CB_CANON_USER, (mysasl_cb_ft *) &mysasl_canon_user, NULL },
    { SASL_CB_LIST_END, NULL, NULL }
};

struct accept {
    char *token;
    float qual;
    struct accept *next;
};

/* Flags for known methods*/
enum {
    METH_NOBODY =	(1<<0),	/* Method does not expect a body */
};

/* Array of HTTP methods known by our server. */
const struct known_meth_t http_methods[] = {
    { "ACL",		0 },
    { "COPY",	   	METH_NOBODY },
    { "DELETE",	   	METH_NOBODY },
    { "GET",	   	METH_NOBODY },
    { "HEAD",	   	METH_NOBODY },
    { "LOCK",		0 },
    { "MKCALENDAR",	0 },
    { "MKCOL",		0 },
    { "MOVE",		METH_NOBODY },
    { "OPTIONS",	METH_NOBODY },
    { "POST",		0 },
    { "PROPFIND",	0 },
    { "PROPPATCH",	0 },
    { "PUT",		0 },
    { "REPORT",		0 },
    { "TRACE",	   	METH_NOBODY },
    { "UNLOCK",	   	METH_NOBODY },
    { NULL,		0 }
};

/* Namespace to fetch static content from filesystem */
struct namespace_t namespace_default = {
    URL_NS_DEFAULT, 1, "", NULL, 0 /* no auth */, ALLOW_READ,
    NULL, NULL, NULL, NULL,
    {
	{ NULL,			NULL },			/* ACL		*/
	{ NULL,			NULL },			/* COPY		*/
	{ NULL,			NULL },			/* DELETE	*/
	{ &meth_get_doc,	NULL },			/* GET		*/
	{ &meth_get_doc,	NULL },			/* HEAD		*/
	{ NULL,			NULL },			/* LOCK		*/
	{ NULL,			NULL },			/* MKCALENDAR	*/
	{ NULL,			NULL },			/* MKCOL	*/
	{ NULL,			NULL },			/* MOVE		*/
	{ &meth_options,	NULL },			/* OPTIONS	*/
	{ NULL,			NULL },			/* POST		*/
	{ &meth_propfind_root,	NULL },			/* PROPFIND	*/
	{ NULL,			NULL },			/* PROPPATCH	*/
	{ NULL,			NULL },			/* PUT		*/
	{ NULL,			NULL },			/* REPORT	*/
	{ &meth_trace,		NULL },			/* TRACE	*/
	{ NULL,			NULL },			/* UNLOCK	*/
    }
};

/* Array of different namespaces and features supported by the server */
struct namespace_t *namespaces[] = {
#ifdef WITH_DAV
    &namespace_principal,
    &namespace_calendar,
    &namespace_ischedule,
    &namespace_domainkey,
    &namespace_addressbook,
#endif
#ifdef WITH_RSS
    &namespace_rss,
#endif
    &namespace_default,		/* MUST be present and be last!! */
    NULL,
};


static void httpd_reset(void)
{
    int i;
    int bytes_in = 0;
    int bytes_out = 0;

    /* Do any namespace specific cleanup */
    for (i = 0; namespaces[i]; i++) {
	if (namespaces[i]->enabled && namespaces[i]->reset)
	    namespaces[i]->reset();
    }

    proc_cleanup();

    /* close backend connections */
    i = 0;
    while (backend_cached && backend_cached[i]) {
	proxy_downserver(backend_cached[i]);
	free(backend_cached[i]->context);
	free(backend_cached[i]);
	i++;
    }
    if (backend_cached) free(backend_cached);
    backend_cached = NULL;
    backend_current = NULL;

    if (httpd_mailbox) mailbox_close(&httpd_mailbox);
    httpd_mailbox = NULL;

    if (httpd_in) {
	prot_NONBLOCK(httpd_in);
	prot_fill(httpd_in);
	bytes_in = prot_bytes_in(httpd_in);
	prot_free(httpd_in);
    }

    if (httpd_out) {
	prot_flush(httpd_out);
	bytes_out = prot_bytes_out(httpd_out);
	prot_free(httpd_out);
    }

    if (config_auditlog) {
	syslog(LOG_NOTICE,
	       "auditlog: traffic sessionid=<%s> bytes_in=<%d> bytes_out=<%d>", 
	       session_id(), bytes_in, bytes_out);
    }
    
    httpd_in = httpd_out = NULL;

    if (protin) protgroup_reset(protin);

#ifdef HAVE_SSL
    if (tls_conn) {
	tls_reset_servertls(&tls_conn);
	tls_conn = NULL;
    }
#endif

    cyrus_reset_stdio();

    strcpy(httpd_clienthost, "[local]");
    if (httpd_logfd != -1) {
	close(httpd_logfd);
	httpd_logfd = -1;
    }
    if (httpd_userid != NULL) {
	free(httpd_userid);
	httpd_userid = NULL;
    }
    if (httpd_authstate) {
	auth_freestate(httpd_authstate);
	httpd_authstate = NULL;
    }
    if (httpd_saslconn) {
	sasl_dispose(&httpd_saslconn);
	httpd_saslconn = NULL;
    }
    httpd_tls_done = 0;

    if(saslprops.iplocalport) {
       free(saslprops.iplocalport);
       saslprops.iplocalport = NULL;
    }
    if(saslprops.ipremoteport) {
       free(saslprops.ipremoteport);
       saslprops.ipremoteport = NULL;
    }
    if(saslprops.authid) {
       free(saslprops.authid);
       saslprops.authid = NULL;
    }
    saslprops.ssf = 0;
}

/*
 * run once when process is forked;
 * MUST NOT exit directly; must return with non-zero error code
 */
int service_init(int argc __attribute__((unused)),
		 char **argv __attribute__((unused)),
		 char **envp __attribute__((unused)))
{
    int r, opt, i, allow_trace = config_getswitch(IMAPOPT_HTTPALLOWTRACE);

    LIBXML_TEST_VERSION

    initialize_http_error_table();

    if (geteuid() == 0) fatal("must run as the Cyrus user", EC_USAGE);
    setproctitle_init(argc, argv, envp);

    /* set signal handlers */
    signals_set_shutdown(&shut_down);
    signal(SIGPIPE, SIG_IGN);

    /* load the SASL plugins */
    global_sasl_init(1, 1, mysasl_cb);

    /* open the mboxlist, we'll need it for real work */
    mboxlist_init(0);
    mboxlist_open(NULL);

    /* open the quota db, we'll need it for expunge */
    quotadb_init(0);
    quotadb_open(NULL);

    /* open the user deny db */
    denydb_init(0);
    denydb_open(NULL);

    /* open annotations.db, we'll need it for collection properties */
    annotatemore_init(0, NULL, NULL);
    annotatemore_open(NULL);

    /* setup for sending IMAP IDLE notifications */
    idle_enabled();

    /* Set namespace */
    if ((r = mboxname_init_namespace(&httpd_namespace, 1)) != 0) {
	syslog(LOG_ERR, "%s", error_message(r));
	fatal(error_message(r), EC_CONFIG);
    }
    /* External names are in URIs (UNIX sep) */
    httpd_namespace.hier_sep = '/';

    while ((opt = getopt(argc, argv, "sp:")) != EOF) {
	switch(opt) {
	case 's': /* https (do TLS right away) */
	    https = 1;
	    if (!tls_enabled()) {
		syslog(LOG_ERR, "https: required OpenSSL options not present");
		fatal("https: required OpenSSL options not present",
		      EC_CONFIG);
	    }
	    break;

	case 'p': /* external protection */
	    extprops_ssf = atoi(optarg);
	    break;

	default:
	    usage();
	}
    }

    /* Create a protgroup for input from the client and selected backend */
    protin = protgroup_new(2);

    /* Construct serverinfo string */
    if (config_serverinfo == IMAP_ENUM_SERVERINFO_ON) {
	buf_printf(&serverinfo, "Cyrus/%s%s Cyrus-SASL/%u.%u.%u",
		   cyrus_version(), config_mupdate_server ? " (Murder)" : "",
		   SASL_VERSION_MAJOR, SASL_VERSION_MINOR, SASL_VERSION_STEP);
#ifdef HAVE_SSL
	buf_printf(&serverinfo, " OpenSSL/%s", SHLIB_VERSION_NUMBER);
#endif
#ifdef HAVE_ZLIB
	buf_printf(&serverinfo, " zlib/%s", ZLIB_VERSION);
#endif
	buf_printf(&serverinfo, " libxml/%s", LIBXML_DOTTED_VERSION);
    }

    /* Do any namespace specific initialization */
    config_httpmodules = config_getbitfield(IMAPOPT_HTTPMODULES);
    for (i = 0; namespaces[i]; i++) {
	if (allow_trace) namespaces[i]->allow |= ALLOW_TRACE;
	if (namespaces[i]->init) namespaces[i]->init(&serverinfo);
    }

    return 0;
}


/*
 * run for each accepted connection
 */
int service_main(int argc __attribute__((unused)),
		 char **argv __attribute__((unused)),
		 char **envp __attribute__((unused)))
{
    socklen_t salen;
    char hbuf[NI_MAXHOST];
    char localip[60], remoteip[60];
    int niflags;
    sasl_security_properties_t *secprops=NULL;
    const char *mechlist, *mech;
    int mechcount = 0;
    size_t mechlen;
    struct auth_scheme_t *scheme;

    session_new_id();

    signals_poll();

    sync_log_init();

    httpd_in = prot_new(0, 0);
    httpd_out = prot_new(1, 1);
    protgroup_insert(protin, httpd_in);

    /* Find out name of client host */
    salen = sizeof(httpd_remoteaddr);
    if (getpeername(0, (struct sockaddr *)&httpd_remoteaddr, &salen) == 0 &&
	(httpd_remoteaddr.ss_family == AF_INET ||
	 httpd_remoteaddr.ss_family == AF_INET6)) {
	if (getnameinfo((struct sockaddr *)&httpd_remoteaddr, salen,
			hbuf, sizeof(hbuf), NULL, 0, NI_NAMEREQD) == 0) {
    	    strncpy(httpd_clienthost, hbuf, sizeof(hbuf));
	    strlcat(httpd_clienthost, " ", sizeof(httpd_clienthost));
	} else {
	    httpd_clienthost[0] = '\0';
	}
	niflags = NI_NUMERICHOST;
#ifdef NI_WITHSCOPEID
	if (((struct sockaddr *)&httpd_remoteaddr)->sa_family == AF_INET6)
	    niflags |= NI_WITHSCOPEID;
#endif
	if (getnameinfo((struct sockaddr *)&httpd_remoteaddr, salen, hbuf,
			sizeof(hbuf), NULL, 0, niflags) != 0)
	    strlcpy(hbuf, "unknown", sizeof(hbuf));
	strlcat(httpd_clienthost, "[", sizeof(httpd_clienthost));
	strlcat(httpd_clienthost, hbuf, sizeof(httpd_clienthost));
	strlcat(httpd_clienthost, "]", sizeof(httpd_clienthost));
	salen = sizeof(httpd_localaddr);
	if (getsockname(0, (struct sockaddr *)&httpd_localaddr, &salen) == 0) {
	    httpd_haveaddr = 1;
	}

	/* Create pre-authentication telemetry log based on client IP */
	httpd_logfd = telemetry_log(hbuf, httpd_in, httpd_out, 0);
    }

    /* other params should be filled in */
    if (sasl_server_new("HTTP", config_servername, NULL, NULL, NULL, NULL,
			SASL_USAGE_FLAGS, &httpd_saslconn) != SASL_OK)
	fatal("SASL failed initializing: sasl_server_new()",EC_TEMPFAIL); 

    /* will always return something valid */
    secprops = mysasl_secprops(0);

    /* no HTTP clients seem to use "auth-int" */
    secprops->max_ssf = 0;				/* "auth" only */
    secprops->maxbufsize = 0;  			   	/* don't need maxbuf */
    if (sasl_setprop(httpd_saslconn, SASL_SEC_PROPS, secprops) != SASL_OK)
	fatal("Failed to set SASL property", EC_TEMPFAIL);
    if (sasl_setprop(httpd_saslconn, SASL_SSF_EXTERNAL, &extprops_ssf) != SASL_OK)
	fatal("Failed to set SASL property", EC_TEMPFAIL);
    
    if(iptostring((struct sockaddr *)&httpd_localaddr,
		  salen, localip, 60) == 0) {
	sasl_setprop(httpd_saslconn, SASL_IPLOCALPORT, localip);
	saslprops.iplocalport = xstrdup(localip);
    }
    
    if(iptostring((struct sockaddr *)&httpd_remoteaddr,
		  salen, remoteip, 60) == 0) {
	sasl_setprop(httpd_saslconn, SASL_IPREMOTEPORT, remoteip);  
	saslprops.ipremoteport = xstrdup(remoteip);
    }

    /* See which auth schemes are available to us */
    if ((extprops_ssf >= 2) || config_getswitch(IMAPOPT_ALLOWPLAINTEXT)) {
	avail_auth_schemes |= (1 << AUTH_BASIC);
    }
    sasl_listmech(httpd_saslconn, NULL, NULL, " ", NULL,
		  &mechlist, NULL, &mechcount);
    for (mech = mechlist; mechcount--; mech += ++mechlen) {
	mechlen = strcspn(mech, " \0");
	for (scheme = auth_schemes; scheme->name; scheme++) {
	    if (scheme->saslmech && !strncmp(mech, scheme->saslmech, mechlen)) {
		avail_auth_schemes |= (1 << scheme->idx);
		break;
	    }
	}
    }
    httpd_tls_required = !avail_auth_schemes;

    proc_register("httpd", httpd_clienthost, NULL, NULL);

    /* Set inactivity timer */
    httpd_timeout = config_getint(IMAPOPT_HTTPTIMEOUT);
    if (httpd_timeout < 0) httpd_timeout = 0;
    httpd_timeout *= 60;
    prot_settimeout(httpd_in, httpd_timeout);
    prot_setflushonread(httpd_in, httpd_out);

    /* we were connected on https port so we should do 
       TLS negotiation immediatly */
    if (https == 1) starttls(1);

    /* Setup the signal handler for keepalive heartbeat */
    httpd_keepalive = config_getint(IMAPOPT_HTTPKEEPALIVE);
    if (httpd_keepalive < 0) httpd_keepalive = 0;
    if (httpd_keepalive) {
	struct sigaction action;

	sigemptyset(&action.sa_mask);
	action.sa_flags = 0;
#ifdef SA_RESTART
	action.sa_flags |= SA_RESTART;
#endif
	action.sa_handler = keep_alive;
	if (sigaction(SIGALRM, &action, NULL) < 0) {
	    syslog(LOG_ERR, "unable to install signal handler for %d: %m", SIGALRM);
	    httpd_keepalive = 0;
	}
    }

    cmdloop();

    /* Closing connection */

    /* cleanup */
    signal(SIGALRM, SIG_IGN);
    httpd_reset();

    return 0;
}


/* Called by service API to shut down the service */
void service_abort(int error)
{
    shut_down(error);
}


void usage(void)
{
    prot_printf(httpd_out, "%s: usage: httpd [-C <alt_config>] [-s]\r\n",
		error_message(HTTP_SERVER_ERROR));
    prot_flush(httpd_out);
    exit(EC_USAGE);
}


/*
 * Cleanly shut down and exit
 */
void shut_down(int code)
{
    int i;
    int bytes_in = 0;
    int bytes_out = 0;

    in_shutdown = 1;

    /* Do any namespace specific cleanup */
    for (i = 0; namespaces[i]; i++) {
	if (namespaces[i]->enabled && namespaces[i]->shutdown)
	    namespaces[i]->shutdown();
    }

    xmlCleanupParser();

    proc_cleanup();

    /* close backend connections */
    i = 0;
    while (backend_cached && backend_cached[i]) {
	proxy_downserver(backend_cached[i]);
	free(backend_cached[i]->context);
	free(backend_cached[i]);
	i++;
    }
    if (backend_cached) free(backend_cached);

    if (httpd_mailbox) mailbox_close(&httpd_mailbox);

    sync_log_done();

    mboxlist_close();
    mboxlist_done();

    quotadb_close();
    quotadb_done();

    denydb_close();
    denydb_done();

    annotatemore_close();
    annotatemore_done();

    if (httpd_in) {
	prot_NONBLOCK(httpd_in);
	prot_fill(httpd_in);
	bytes_in = prot_bytes_in(httpd_in);
	prot_free(httpd_in);
    }

    if (httpd_out) {
	prot_flush(httpd_out);
	bytes_out = prot_bytes_out(httpd_out);
	prot_free(httpd_out);
    }

    if (protin) protgroup_free(protin);

    if (config_auditlog)
	syslog(LOG_NOTICE,
	       "auditlog: traffic sessionid=<%s> bytes_in=<%d> bytes_out=<%d>", 
	       session_id(), bytes_in, bytes_out);

#ifdef HAVE_SSL
    tls_shutdown_serverengine();
#endif

    cyrus_done();

    exit(code);
}


void fatal(const char* s, int code)
{
    static int recurse_code = 0;

    if (recurse_code) {
	/* We were called recursively. Just give up */
	proc_cleanup();
	exit(recurse_code);
    }
    recurse_code = code;
    if (httpd_out) {
	prot_printf(httpd_out,
		    "HTTP/1.1 %s\r\n"
		    "Content-Type: text/plain\r\n"
		    "Connection: close\r\n\r\n"
		    "Fatal error: %s\r\n",
		    error_message(HTTP_SERVER_ERROR), s);
	prot_flush(httpd_out);
    }
    syslog(LOG_ERR, "Fatal error: %s", s);
    shut_down(code);
}




#ifdef HAVE_SSL
static void starttls(int https)
{
    int result;
    int *layerp;
    sasl_ssf_t ssf;
    char *auth_id;

    /* SASL and openssl have different ideas about whether ssf is signed */
    layerp = (int *) &ssf;

    result=tls_init_serverengine("http",
				 5,        /* depth to verify */
				 !https,   /* can client auth? */
				 !https);  /* TLS only? */

    if (result == -1) {
	syslog(LOG_ERR, "[httpd] error initializing TLS");
	fatal("tls_init() failed",EC_TEMPFAIL);
    }

    if (!https) {
	/* tell client to start TLS upgrade (RFC 2817) */
	response_header(HTTP_SWITCH_PROT, NULL);
    }
  
    result=tls_start_servertls(0, /* read */
			       1, /* write */
			       https ? 180 : httpd_timeout,
			       layerp,
			       &auth_id,
			       &tls_conn);

    /* if error */
    if (result == -1) {
	syslog(LOG_NOTICE, "https failed: %s", httpd_clienthost);
	fatal("tls_start_servertls() failed", EC_TEMPFAIL);
    }

    /* tell SASL about the negotiated layer */
    result = sasl_setprop(httpd_saslconn, SASL_SSF_EXTERNAL, &ssf);
    if (result != SASL_OK) {
	fatal("sasl_setprop() failed: starttls()", EC_TEMPFAIL);
    }
    saslprops.ssf = ssf;

    result = sasl_setprop(httpd_saslconn, SASL_AUTH_EXTERNAL, auth_id);
    if (result != SASL_OK) {
        fatal("sasl_setprop() failed: starttls()", EC_TEMPFAIL);
    }
    if (saslprops.authid) {
	free(saslprops.authid);
	saslprops.authid = NULL;
    }
    if (auth_id) saslprops.authid = xstrdup(auth_id);

    /* tell the prot layer about our new layers */
    prot_settls(httpd_in, tls_conn);
    prot_settls(httpd_out, tls_conn);

    httpd_tls_done = 1;
    httpd_tls_required = 0;

    avail_auth_schemes |= (1 << AUTH_BASIC);
}
#else
static void starttls(int https __attribute__((unused)))
{
    fatal("starttls() called, but no OpenSSL", EC_SOFTWARE);
}
#endif /* HAVE_SSL */


/* Reset the given sasl_conn_t to a sane state */
static int reset_saslconn(sasl_conn_t **conn) 
{
    int ret;
    sasl_security_properties_t *secprops = NULL;

    sasl_dispose(conn);
    /* do initialization typical of service_main */
    ret = sasl_server_new("HTTP", config_servername, NULL, NULL, NULL, NULL,
			  SASL_USAGE_FLAGS, conn);
    if(ret != SASL_OK) return ret;

    if(saslprops.ipremoteport)
       ret = sasl_setprop(*conn, SASL_IPREMOTEPORT,
                          saslprops.ipremoteport);
    if(ret != SASL_OK) return ret;
    
    if(saslprops.iplocalport)
       ret = sasl_setprop(*conn, SASL_IPLOCALPORT,
                          saslprops.iplocalport);
    if(ret != SASL_OK) return ret;
    secprops = mysasl_secprops(0);

    /* no HTTP clients seem to use "auth-int" */
    secprops->max_ssf = 0;				/* "auth" only */
    secprops->maxbufsize = 0;  			   	/* don't need maxbuf */
    ret = sasl_setprop(*conn, SASL_SEC_PROPS, secprops);
    if(ret != SASL_OK) return ret;
    /* end of service_main initialization excepting SSF */

    /* If we have TLS/SSL info, set it */
    if(saslprops.ssf) {
	ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &saslprops.ssf);
    } else {
	ret = sasl_setprop(*conn, SASL_SSF_EXTERNAL, &extprops_ssf);
    }

    if(ret != SASL_OK) return ret;

    if(saslprops.authid) {
       ret = sasl_setprop(*conn, SASL_AUTH_EXTERNAL, saslprops.authid);
       if(ret != SASL_OK) return ret;
    }
    /* End TLS/SSL Info */

    return SASL_OK;
}


/*
 * Top-level command loop parsing
 */
static void cmdloop(void)
{
    int gzip_enabled = 0;
    struct transaction_t txn;

    /* Start with an empty (clean) transaction */
    memset(&txn, 0, sizeof(struct transaction_t));

    /* Pre-allocate our working buffer */
    buf_ensure(&txn.buf, 1024);

#ifdef HAVE_ZLIB
    /* Always use gzip format because IE incorrectly uses raw deflate */
    if (config_getswitch(IMAPOPT_HTTPALLOWCOMPRESS) &&
	deflateInit2(&txn.zstrm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
		     16+MAX_WBITS, MAX_MEM_LEVEL, Z_DEFAULT_STRATEGY) == Z_OK) {
	gzip_enabled = 1;
    }
#endif

    for (;;) {
	int ret, tls_upgrade, empty, r, i, c;
	char *p;
	tok_t tok;
	const char **hdr;
	const struct namespace_t *namespace;
	const struct method_t *meth_t;
	struct request_line_t *req_line = &txn.req_line;

	/* Reset txn state */
	txn.meth = METH_UNKNOWN;
	memset(&txn.flags, 0, sizeof(struct txn_flags_t));
	txn.flags.close = !httpd_timeout;
	txn.flags.vary = gzip_enabled ? VARY_AE : 0;
	memset(req_line, 0, sizeof(struct request_line_t));
	txn.req_uri = NULL;
	txn.auth_chal.param = NULL;
	txn.req_hdrs = NULL;
	txn.location = NULL;
	memset(&txn.error, 0, sizeof(struct error_t));
	memset(&txn.resp_body, 0,  /* Don't zero the response payload buffer */
	       sizeof(struct resp_body_t) - sizeof(struct buf));
	buf_reset(&txn.buf);
	ret = empty = 0;

	/* Create header cache */
	if (!(txn.req_hdrs = spool_new_hdrcache())) {
	    txn.error.desc = "Unable to create header cache";
	    ret = HTTP_SERVER_ERROR;
	}

      req_line:
	do {
	    /* Flush any buffered output */
	    prot_flush(httpd_out);
	    if (backend_current) prot_flush(backend_current->out);

	    /* Check for shutdown file */
	    if (shutdown_file(txn.buf.s, txn.buf.alloc) ||
		(httpd_userid &&
		 userdeny(httpd_userid, config_ident, txn.buf.s, txn.buf.alloc))) {
		txn.error.desc = txn.buf.s;
		ret = HTTP_UNAVAILABLE;
		break;
	    }

	    signals_poll();

	} while (!proxy_check_input(protin, httpd_in, httpd_out,
				    backend_current ? backend_current->in : NULL,
				    NULL, 0));
	if (ret) {
	    txn.flags.close = 1;
	    error_response(ret, &txn);
	    protgroup_free(protin);
	    shut_down(0);
	}


	/* Read request-line */
	syslog(LOG_DEBUG, "read & parse request-line");
	if (!prot_fgets(req_line->buf, MAX_REQ_LINE+1, httpd_in)) {
	    txn.error.desc = prot_error(httpd_in);
	    if (txn.error.desc && strcmp(txn.error.desc, PROT_EOF_STRING)) {
		/* client timed out */
		syslog(LOG_WARNING, "%s, closing connection", txn.error.desc);
		ret = HTTP_TIMEOUT;
	    }
	    else {
		/* client closed connection */
	    }

	    txn.flags.close = 1;
	    goto done;
	}

	/* Trim CRLF from request-line */
	p = req_line->buf + strlen(req_line->buf);
	if (p[-1] == '\n') *--p = '\0';
	if (p[-1] == '\r') *--p = '\0';

	/* Ignore 1 empty line before request-line per HTTPbis Part 1 Sec 3.5 */
	if (!empty++ && !*req_line->buf) goto req_line;

	/* Parse request-line = method SP request-target SP HTTP-version CRLF */
	tok_initm(&tok, req_line->buf, " ", 0);
	if (!(req_line->meth = tok_next(&tok))) {
	    ret = HTTP_BAD_REQUEST;
	    txn.error.desc = "Missing method in request-line";
	}
	else if (!(req_line->uri = tok_next(&tok))) {
	    ret = HTTP_BAD_REQUEST;
	    txn.error.desc = "Missing request-target in request-line";
	}
	else if ((size_t) (p - req_line->buf) > MAX_REQ_LINE - 2) {
	    /* request-line overran the size of our buffer */
	    ret = HTTP_TOO_LONG;
	    buf_printf(&txn.buf,
		       "Length of request-line MUST be less than %u octets",
		       MAX_REQ_LINE);
	    txn.error.desc = buf_cstring(&txn.buf);
	}
	else if (!(req_line->ver = tok_next(&tok))) {
	    ret = HTTP_BAD_REQUEST;
	    txn.error.desc = "Missing HTTP-version in request-line";
	}
	else if (tok_next(&tok)) {
	    ret = HTTP_BAD_REQUEST;
	    txn.error.desc = "Unexpected extra argument(s) in request-line";
	}

	/* Check HTTP-Version - MUST be HTTP/1.x */
	else if (strlen(req_line->ver) != HTTP_VERSION_LEN
		 || strncmp(req_line->ver, HTTP_VERSION, HTTP_VERSION_LEN-1)
		 || !isdigit(req_line->ver[HTTP_VERSION_LEN-1])) {
	    ret = HTTP_BAD_VERSION;
	    buf_printf(&txn.buf,
		     "This server only speaks %.*sx",
		       HTTP_VERSION_LEN-1, HTTP_VERSION);
	    txn.error.desc = buf_cstring(&txn.buf);
	}
	else if (req_line->ver[HTTP_VERSION_LEN-1] == '0') {
	    /* HTTP/1.0 - non-persistent connection by default */
	    txn.flags.ver1_0 = txn.flags.close = 1;
	}
	tok_fini(&tok);

	if (ret) {
	    txn.flags.close = 1;
	    goto done;
	}

	/* Read and parse headers */
	syslog(LOG_DEBUG, "read & parse headers");
	if ((r = spool_fill_hdrcache(httpd_in, NULL, txn.req_hdrs, NULL))) {
	    ret = HTTP_BAD_REQUEST;
	    txn.error.desc = error_message(r);
	}
	else if ((txn.error.desc = prot_error(httpd_in)) &&
		 strcmp(txn.error.desc, PROT_EOF_STRING)) {
	    /* client timed out */
	    syslog(LOG_WARNING, "%s, closing connection", txn.error.desc);
	    ret = HTTP_TIMEOUT;
	}

	/* Read CRLF separating headers and body */
	else if ((c = prot_getc(httpd_in)) != '\r' ||
		 (c = prot_getc(httpd_in)) != '\n') {
	    ret = HTTP_BAD_REQUEST;
	    txn.error.desc = error_message(IMAP_MESSAGE_NOBLANKLINE);
	}

	if (ret) {
	    txn.flags.close = 1;
	    goto done;
	}

	/* Check for Connection options */
	parse_connection(&txn, &tls_upgrade);
	if (tls_upgrade) starttls(0);

	/* Check Method against our list of known methods */
	for (txn.meth = 0; (txn.meth < METH_UNKNOWN) &&
		 strcmp(http_methods[txn.meth].name, req_line->meth);
	     txn.meth++);

	if (txn.meth == METH_UNKNOWN) ret = HTTP_NOT_IMPLEMENTED;

	/* Check for Expectations (HTTP/1.1+ only) */
	else if (!txn.flags.ver1_0 && (r = parse_expect(&txn))) ret = r;

	/* Parse request-target URI */
	else if (!(txn.req_uri = parse_uri(txn.meth, req_line->uri,
					   &txn.error.desc))) {
	    ret = HTTP_BAD_REQUEST;
	}

	/* Check for mandatory Host header (HTTP/1.1+ only) */
	else if ((hdr = spool_getheader(txn.req_hdrs, "Host")) && hdr[1]) {
	    ret = HTTP_BAD_REQUEST;
	    txn.error.desc = "Too many Host headers";
	}
	else if (!hdr) {
	    if (txn.flags.ver1_0) {
		/* HTTP/1.0 - create a Host header from URI */
		if (txn.req_uri->server) {
		    buf_setcstr(&txn.buf, txn.req_uri->server);
		    if (txn.req_uri->port)
			buf_printf(&txn.buf, ":%d", txn.req_uri->port);
		}
		else buf_setcstr(&txn.buf, config_servername);

		spool_cache_header(xstrdup("Host"),
				   xstrdup(buf_cstring(&txn.buf)),
				   txn.req_hdrs);
		buf_reset(&txn.buf);
	    }
	    else {
		ret = HTTP_BAD_REQUEST;
		txn.error.desc = "Missing Host header";
	    }
	}

	if (ret) goto done;

	/* Find the namespace of the requested resource */
	for (i = 0; namespaces[i]; i++) {
	    const char *path = txn.req_uri->path;
	    const char *query = txn.req_uri->query_raw;
	    size_t len;

	    /* Skip disabled namespaces */
	    if (!namespaces[i]->enabled) continue;

	    /* Handle any /.well-known/ bootstrapping */
	    if (namespaces[i]->well_known) {
		len = strlen(namespaces[i]->well_known);
		if (!strncmp(path, namespaces[i]->well_known, len) &&
		    (!path[len] || path[len] == '/')) {
			
		    buf_setcstr(&txn.buf, namespaces[i]->prefix);
		    buf_appendcstr(&txn.buf, path + len);
		    if (query) buf_printf(&txn.buf, "?%s", query);
		    txn.location = buf_cstring(&txn.buf);

		    ret = HTTP_MOVED;
		    goto done;
		}
	    }

	    /* See if the prefix matches - terminated with NUL or '/' */
	    len = strlen(namespaces[i]->prefix);
	    if (!strncmp(path, namespaces[i]->prefix, len) &&
		(!path[len] || (path[len] == '/') || !strcmp(path, "*"))) {
		break;
	    }
	}
	if ((namespace = namespaces[i])) {
	    txn.req_tgt.namespace = namespace->id;
	    txn.req_tgt.allow = namespace->allow;

	    /* Check if method is supported in this namespace */
	    meth_t = &namespace->methods[txn.meth];
	    if (!meth_t->proc) ret = HTTP_NOT_ALLOWED;

	    /* Check if method expects a body */
	    else if ((http_methods[txn.meth].flags & METH_NOBODY) &&
		     spool_getheader(txn.req_hdrs, "Content-Type"))
		ret = HTTP_BAD_MEDIATYPE;
	} else {
	    /* XXX  Should never get here */
	    ret = HTTP_SERVER_ERROR;
	}

	if (ret) goto done;

	/* Perform authentication, if necessary */
	if (!httpd_userid) {
	    if ((hdr = spool_getheader(txn.req_hdrs, "Authorization"))) {
		/* Check the auth credentials */
		r = http_auth(hdr[0], &txn);
		if ((r < 0) || !txn.auth_chal.scheme) {
		    /* Auth failed - reinitialize */
		    syslog(LOG_DEBUG, "auth failed - reinit");
		    reset_saslconn(&httpd_saslconn);
		    txn.auth_chal.scheme = NULL;
		    r = SASL_FAIL;
		}
	    }
	    else if (txn.auth_chal.scheme) {
		/* Started auth exchange, but client didn't engage - reinit */
		syslog(LOG_DEBUG, "client didn't complete auth - reinit");
		reset_saslconn(&httpd_saslconn);
		txn.auth_chal.scheme = NULL;
	    }
	}

	/* Request authentication, if necessary */
	if (!httpd_userid && (r || namespace->need_auth)) {
	  need_auth:
	    /* User must authenticate */

	    if (httpd_tls_required) {
		/* We only support TLS+Basic, so tell client to use TLS */

		/* Check which response is required */
		if ((hdr = spool_getheader(txn.req_hdrs, "Upgrade")) &&
		    !strncmp(hdr[0], TLS_VERSION, strcspn(hdr[0], " ,"))) {
		    /* Client (Murder proxy) supports RFC 2817 (TLS upgrade) */

		    response_header(HTTP_UPGRADE, &txn);
		}
		else {
		    /* All other clients use RFC 2818 (HTTPS) */
		    const char *path = txn.req_uri->path;
		    const char *query = txn.req_uri->query_raw;
		    struct buf *html = &txn.resp_body.payload;

		    /* Create https URL */
		    hdr = spool_getheader(txn.req_hdrs, "Host");
		    buf_printf(&txn.buf, "https://%s", hdr[0]);
		    if (strcmp(path, "*")) {
			buf_appendcstr(&txn.buf, path);
			if (query) buf_printf(&txn.buf, "?%s", query);
		    }

		    txn.location = buf_cstring(&txn.buf);

		    /* Create HTML body */
		    buf_reset(html);
		    buf_printf(html, tls_message,
			       buf_cstring(&txn.buf), buf_cstring(&txn.buf));

		    /* Output our HTML response */
		    txn.resp_body.type = "text/html; charset=utf-8";
		    write_body(HTTP_MOVED, &txn,
			       buf_cstring(html), buf_len(html));
		}
	    }
	    else {
		/* Tell client to authenticate */
		ret = HTTP_UNAUTHORIZED;
		if (r == SASL_CONTINUE)
		    txn.error.desc = "Continue authentication exchange";
		else if (r) txn.error.desc = "Authentication failed";
		else txn.error.desc =
			 "Must authenticate to access the specified target";
	    }

	    goto done;
	}

	/* Check if we should compress response body */
	if (gzip_enabled) {
	    /* XXX  Do we want to support deflate even though M$
	       doesn't implement it correctly (raw deflate vs. zlib)? */

	    if (!txn.flags.ver1_0 &&
		(hdr = spool_getheader(txn.req_hdrs, "TE"))) {
		struct accept *e, *enc = parse_accept(hdr);

		for (e = enc; e && e->token; e++) {
		    if (!strcasecmp(e->token, "gzip") ||
			!strcasecmp(e->token, "x-gzip")) {
			txn.flags.te = TE_GZIP;
		    }
		    free(e->token);
		}
		if (enc) free(enc);
	    }
	    else if ((hdr = spool_getheader(txn.req_hdrs, "Accept-Encoding"))) {
		struct accept *e, *enc = parse_accept(hdr);

		for (e = enc; e && e->token; e++) {
		    if (!strcasecmp(e->token, "gzip") ||
			!strcasecmp(e->token, "x-gzip")) {
			txn.flags.ce = CE_GZIP;
		    }
		    free(e->token);
		}
		if (enc) free(enc);
	    }
	}

	/* Start method processing alarm (HTTP/1.1+ only) */
	if (!txn.flags.ver1_0) alarm(httpd_keepalive);

	/* Process the requested method */
	ret = (*meth_t->proc)(&txn, meth_t->params);
	if (ret == HTTP_UNAUTHORIZED) goto need_auth;

      done:
	/* Handle errors (success responses handled by method functions) */
	if (ret) error_response(ret, &txn);

	/* Memory cleanup */
	if (txn.req_uri) xmlFreeURI(txn.req_uri);
	if (txn.req_hdrs) spool_free_hdrcache(txn.req_hdrs);

	if (txn.flags.close) {
	    buf_free(&txn.req_body);
	    buf_free(&txn.resp_body.payload);
#ifdef HAVE_ZLIB
	    deflateEnd(&txn.zstrm);
#endif
	    return;
	}

	continue;
    }
}

/****************************  Parsing Routines  ******************************/

/* Parse URI, returning the path */
xmlURIPtr parse_uri(unsigned meth, const char *uri, const char **errstr)
{
    xmlURIPtr p_uri;  /* parsed URI */

    /* Parse entire URI */
    if ((p_uri = xmlParseURI(uri)) == NULL) {
	*errstr = "Illegal request target URI";
	goto bad_request;
    }

    if (p_uri->scheme) {
	/* Check sanity of scheme */

	if (strcasecmp(p_uri->scheme, "http") &&
	    strcasecmp(p_uri->scheme, "https")) {
	    *errstr = "Unsupported URI scheme";
	    goto bad_request;
	}
    }

    /* Check sanity of path */
    if (!p_uri->path || !*p_uri->path) {
	*errstr = "Empty path in target URI";
	goto bad_request;
    }

    if (strlen(p_uri->path) > MAX_MAILBOX_PATH) goto bad_request;

    if ((p_uri->path[0] != '/') &&
	(strcmp(p_uri->path, "*") || (meth != METH_OPTIONS))) {
	*errstr = "Illegal request target URI";
	goto bad_request;
    }

    return p_uri;

  bad_request:
    if (p_uri) xmlFreeURI(p_uri);
    return NULL;
}


/* Compare Content-Types */
int is_mediatype(const char *hdr, const char *type)
{
    size_t tlen = strcspn(type, "; \r\n\0");
    size_t hlen = strcspn(hdr, "; \r\n\0");

    return ((tlen == hlen) && !strncasecmp(hdr, type, tlen));
}


/*
 * Read the body of a request or response.
 * Handles chunked, gzip, deflate TE only.
 * Handles close-delimited response bodies (no Content-Length specified) 
 * Handles gzip and deflate CE only.
 */
int read_body(struct protstream *pin, hdrcache_t hdrs, struct buf *body,
	      unsigned flags, const char **errstr)
{
    const char **hdr;
    unsigned long len = 0;
    enum { LEN_DELIM_LENGTH, LEN_DELIM_CHUNKED, LEN_DELIM_CLOSE };
    unsigned len_delim = LEN_DELIM_LENGTH, te = TE_NONE, max_msgsize, n;
    char buf[PROT_BUFSIZE];

    syslog(LOG_DEBUG, "read_body(%x: %s)", flags, !body ? "discard" : "");

    if (body) buf_reset(body);
    else if (flags & BODY_CONTINUE) {
	/* Don't care about the body and client hasn't sent it, we're done */
	return 0;
    }

    max_msgsize = config_getint(IMAPOPT_MAXMESSAGESIZE);

    /* If max_msgsize is 0, allow any size */
    if (!max_msgsize) max_msgsize = INT_MAX;

    /* Check for Transfer-Encoding */
    if ((hdr = spool_getheader(hdrs, "Transfer-Encoding"))) {
	for (; *hdr; hdr++) {
	    tok_t tok = TOK_INITIALIZER(*hdr, ",", TOK_TRIMLEFT|TOK_TRIMRIGHT);
	    char *token;

	    while ((token = tok_next(&tok))) {
		if (te & TE_CHUNKED) {
		    /* "chunked" MUST only appear once and MUST be last */
		    break;
		}
		else if (!strcasecmp(token, "chunked")) {
		    te |= TE_CHUNKED;
		    len_delim = LEN_DELIM_CHUNKED;
		}
		else if (te & ~TE_CHUNKED) {
		    /* can't combine compression codings */
		    break;
		}
#ifdef HAVE_ZLIB
		else if (!strcasecmp(token, "deflate"))
		    te = TE_DEFLATE;
		else if (!strcasecmp(token, "gzip") ||
			 !strcasecmp(token, "x-gzip"))
		    te = TE_GZIP;
#endif
		else if (body) {
		    /* unknown/unsupported TE */
		    break;
		}
	    }
	    tok_fini(&tok);
	    if (token) break;  /* error */
	}

	if (*hdr) {
	    *errstr = "Specified Transfer-Encoding not implemented";
	    return HTTP_NOT_IMPLEMENTED;
	}

	/* Check if this is a non-chunked response */
	else if (!(te & TE_CHUNKED)) {
	    if ((flags & BODY_RESPONSE) && (flags & BODY_CLOSE)) {
		len_delim = LEN_DELIM_CLOSE;
	    }
	    else {
		*errstr = "Final Transfer-Encoding MUST be \"chunked\"";
		return HTTP_NOT_IMPLEMENTED;
	    }
	}
    }

    /* Check for Content-Length */
    else if ((hdr = spool_getheader(hdrs, "Content-Length"))) {
	if (hdr[1]) {
	    *errstr = "Multiple Content-Length header fields";
	    return HTTP_BAD_REQUEST;
	}

	len = strtoul(hdr[0], NULL, 10);
	if (len > max_msgsize) return HTTP_TOO_LARGE;

	len_delim = LEN_DELIM_LENGTH;
    }
	
    /* Check if this is a close-delimited response */
    else if (flags & BODY_RESPONSE) {
	if (flags & BODY_CLOSE) len_delim = LEN_DELIM_CLOSE;
	else return HTTP_LENGTH_REQUIRED;
    }


    if (flags & BODY_CONTINUE) {
	/* Tell client to send the body */
	response_header(HTTP_CONTINUE, NULL);
    }

    /* Read and buffer the body */
    if (len_delim == LEN_DELIM_CHUNKED) {
	unsigned last = 0;

	/* Read chunks until last-chunk (zero chunk-size) */
	do {
	    unsigned chunk;

	    /* Read chunk-size */
	    if (!prot_fgets(buf, PROT_BUFSIZE, pin) ||
		sscanf(buf, "%x", &chunk) != 1) {
		*errstr = "Unable to read chunk size";
		goto read_failure;
	    }
	    if ((len += chunk) > max_msgsize) return HTTP_TOO_LARGE;

	    if (!chunk) {
		/* last-chunk */
		last = 1;

		/* Read/parse any trailing headers */
		spool_fill_hdrcache(pin, NULL, hdrs, NULL);
	    }
	    
	    /* Read 'chunk' octets */ 
	    for (; chunk; chunk -= n) {
		if (body) n = prot_readbuf(pin, body, chunk);
		else n = prot_read(pin, buf, MIN(chunk, PROT_BUFSIZE));
		
		if (!n) {
		    syslog(LOG_ERR, "prot_read() error");
		    *errstr = "Unable to read chunk data";
		    goto read_failure;
		}
	    }

	    /* Read CRLF terminating the chunk/trailer */
	    if (!prot_fgets(buf, sizeof(buf), pin)) {
		*errstr = "Missing CRLF following chunk/trailer";
		goto read_failure;
	    }

	} while (!last);

	te &= ~TE_CHUNKED;
    }
    else if (len_delim == LEN_DELIM_CLOSE) {
	/* Read until EOF */
	do {
	    if (body) n = prot_readbuf(pin, body, PROT_BUFSIZE);
	    else n = prot_read(pin, buf, PROT_BUFSIZE);

	    if ((len += n) > max_msgsize) return HTTP_TOO_LARGE;

	} while (n);

	if (!pin->eof) goto read_failure;
    }
    else if (len) {
	/* Read 'len' octets */
	for (; len; len -= n) {
	    if (body) n = prot_readbuf(pin, body, len);
	    else n = prot_read(pin, buf, MIN(len, PROT_BUFSIZE));

	    if (!n) {
		syslog(LOG_ERR, "prot_read() error");
		*errstr = "Unable to read body data";
		goto read_failure;
	    }
	}
    }


    if (body && buf_len(body)) {
	int r = 0;

#ifdef HAVE_ZLIB
	/* Decode the payload, if necessary */
	if (te == TE_DEFLATE)
	    r = buf_inflate(body, DEFLATE_ZLIB);
	else if (te == TE_GZIP)
	    r = buf_inflate(body, DEFLATE_GZIP);

	if (r) {
	    *errstr = "Error decoding payload";
	    return HTTP_BAD_REQUEST;
	}
#endif

	/* Decode the representation, if necessary */
	if (flags & BODY_DECODE) {
	    if (!(hdr = spool_getheader(hdrs, "Content-Encoding"))) {
		/* nothing to see here */
	    }

#ifdef HAVE_ZLIB
	    else if (!strcasecmp(hdr[0], "deflate")) {
		const char **ua = spool_getheader(hdrs, "User-Agent");

		/* Try to detect Microsoft's broken deflate */
		if (ua && strstr(ua[0], "; MSIE "))
		    r = buf_inflate(body, DEFLATE_RAW);
		else
		    r = buf_inflate(body, DEFLATE_ZLIB);
	    }
	    else if (!strcasecmp(hdr[0], "gzip") ||
		     !strcasecmp(hdr[0], "x-gzip"))
		r = buf_inflate(body, DEFLATE_GZIP);
#endif
	    else {
		*errstr = "Specified Content-Encoding not accepted";
		return HTTP_BAD_MEDIATYPE;
	    }

	    if (r) {
		*errstr = "Error decoding content";
		return HTTP_BAD_REQUEST;
	    }
	}
    }

    return 0;

  read_failure:
    if (strcmpsafe(prot_error(httpd_in), PROT_EOF_STRING)) {
	/* client timed out */
	*errstr = prot_error(httpd_in);
	syslog(LOG_WARNING, "%s, closing connection", *errstr);
	return HTTP_TIMEOUT;
    }
    else return HTTP_BAD_REQUEST;
}


/* Parse Expect header(s) for interesting expectations */
static int parse_expect(struct transaction_t *txn)
{
    const char **exp = spool_getheader(txn->req_hdrs, "Expect");
    int i, ret = 0;

    /* Look for interesting expectations.  Unknown == error */
    for (i = 0; !ret && exp && exp[i]; i++) {
	tok_t tok = TOK_INITIALIZER(exp[i], ",", TOK_TRIMLEFT|TOK_TRIMRIGHT);
	char *token;

	while (!ret && (token = tok_next(&tok))) {
	    /* Check if this is a non-persistent connection */
	    if (!strcasecmp(token, "100-continue")) {
		syslog(LOG_DEBUG, "Expect: 100-continue");
		txn->flags.cont = 1;
	    }
	    else {
		txn->error.desc = "Unsupported Expectation";
		ret = HTTP_EXPECT_FAILED;
	    }
	}

	tok_fini(&tok);
    }

    return ret;
}


/* Parse Connection header(s) for interesting options */
static void parse_connection(struct transaction_t *txn, int *tls_upgrade)
{
    const char **conn = spool_getheader(txn->req_hdrs, "Connection");
    int i;

    *tls_upgrade = 0;

    /* Look for interesting connection tokens */
    for (i = 0; conn && conn[i]; i++) {
	tok_t tok = TOK_INITIALIZER(conn[i], ",", TOK_TRIMLEFT|TOK_TRIMRIGHT);
	char *token;

	while ((token = tok_next(&tok))) {
	    /* Check if this is a persistent 1.0 connection */
	    if (txn->flags.ver1_0) {
		if (httpd_timeout && !strcasecmp(token, "keep-alive")) {
		    syslog(LOG_DEBUG, "persistent 1.0 connection");
		    txn->flags.close = 0;
		}
	    }

	    /* Check if this is a non-persistent 1.1 connection */
	    else if (!strcasecmp(token, "close")) {
		syslog(LOG_DEBUG, "non-persistent 1.1 connection");
		txn->flags.close = 1;
	    }

	    /* Check if we need to upgrade to TLS */
	    else if (!httpd_tls_done && tls_enabled() &&
		     !strcasecmp(token, "Upgrade")) {
		const char **upgrd;

		if ((upgrd = spool_getheader(txn->req_hdrs, "Upgrade")) &&
		    !strncmp(upgrd[0], TLS_VERSION, strcspn(upgrd[0], " ,"))) {
		    syslog(LOG_DEBUG, "client requested TLS");
		    *tls_upgrade = 1;
		}
	    }
	}

	tok_fini(&tok);
    }
}


/* Compare accept quality values so that they sort in descending order */
static int compare_accept(const struct accept *a1, const struct accept *a2)
{
    if (a2->qual < a1->qual) return -1;
    if (a2->qual > a1->qual) return 1;
    return 0;
}

static struct accept *parse_accept(const char **hdr)
{
    int i, n = 0, alloc = 0;
    struct accept *ret = NULL;
#define GROW_ACCEPT 10;

    for (i = 0; hdr && hdr[i]; i++) {
	tok_t tok = TOK_INITIALIZER(hdr[i], ";,", TOK_TRIMLEFT|TOK_TRIMRIGHT);
	char *token;

	while ((token = tok_next(&tok))) {
	    if (!strncmp(token, "q=", 2)) {
		if (!ret) break;
		ret[n-1].qual = strtof(token+2, NULL);
	    }
	    else {
		if (n + 1 >= alloc)  {
		    alloc += GROW_ACCEPT;
		    ret = xrealloc(ret, alloc * sizeof(struct accept));
		}
		ret[n].token = xstrdup(token);
		ret[n].qual = 1.0;
		ret[++n].token = NULL;
	    }
	}
	tok_fini(&tok);
    }

    qsort(ret, n, sizeof(struct accept),
	  (int (*)(const void *, const void *)) &compare_accept);

    return ret;
}


/****************************  Response Routines  *****************************/


/* Create HTTP-date ('buf' must be at least 30 characters) */
void httpdate_gen(char *buf, size_t len, time_t t)
{
    struct tm *tm;
    static char *month[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
			     "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    static char *wday[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };

    tm = gmtime(&t);

    snprintf(buf, len, "%3s, %02d %3s %4d %02d:%02d:%02d GMT",
	     wday[tm->tm_wday], 
	     tm->tm_mday, month[tm->tm_mon], tm->tm_year + 1900,
	     tm->tm_hour, tm->tm_min, tm->tm_sec);
}


/* Create an HTTP Status-Line given response code */
const char *http_statusline(long code)
{
    static struct buf statline = BUF_INITIALIZER;
    static unsigned tail = 0;

    if (!tail) {
	buf_setcstr(&statline, HTTP_VERSION);
	buf_putc(&statline, ' ');
	tail = buf_len(&statline);
    }

    buf_truncate(&statline, tail);
    buf_appendcstr(&statline, error_message(code));
    return buf_cstring(&statline);
}


/* Output an HTTP response header.
 * 'code' specifies the HTTP Status-Code and Reason-Phrase.
 * 'txn' contains the transaction context
 */

#define WWW_Authenticate(name, param)				\
    prot_printf(httpd_out, "WWW-Authenticate: %s", name);	\
    if (param) prot_printf(httpd_out, " %s", param);		\
    prot_puts(httpd_out, "\r\n")

static void comma_list_hdr(const char *hdr, const char *vals[], unsigned flags)
{
    const char *sep = "";
    int i;

    prot_printf(httpd_out, "%s:", hdr);
    for (i = 0; vals[i]; i++) {
	if (flags & (1 << i)) {
	    prot_printf(httpd_out, "%s %s", sep, vals[i]);
	    sep = ",";
	}
    }
    prot_puts(httpd_out, "\r\n");
}

void response_header(long code, struct transaction_t *txn)
{
    time_t now;
    char datestr[30];
    unsigned keepalive;
    const char **hdr, *conn_token;
    struct auth_challenge_t *auth_chal;
    struct resp_body_t *resp_body;
    static struct buf log = BUF_INITIALIZER;

    if (txn && txn->req_hdrs) {
	/* If we haven't the read body, read and possibly discard it */
	if (!txn->flags.havebody) {
	    const char *errstr = NULL;

	    txn->flags.havebody = 1;
	    if (read_body(httpd_in, txn->req_hdrs,
			  code == HTTP_UNAUTHORIZED ? &txn->req_body : NULL,
			  txn->flags.cont | BODY_DECODE, &errstr)) {
		txn->flags.close = 1;
	    }
	}

	/* Log the client request and our response */
	buf_reset(&log);
	buf_printf(&log, "%s", httpd_clienthost);
	if (httpd_userid) buf_printf(&log, " as \"%s\"", httpd_userid);
	if ((hdr = spool_getheader(txn->req_hdrs, "User-Agent"))) {
	    buf_printf(&log, " with \"%s\"", hdr[0]);
	}
	buf_printf(&log, "; \"%s",
		   txn->req_line.meth ? txn->req_line.meth : "");
	if (txn->req_line.uri) buf_printf(&log, " %s", txn->req_line.uri);
	if (txn->req_line.ver) {
	    buf_printf(&log, " %s", txn->req_line.ver);
	    if (code != HTTP_TOO_LONG) {
		char *p = txn->req_line.ver + strlen(txn->req_line.ver) + 1;
		if (*p) buf_printf(&log, " %s", p);
	    }
	}
	buf_appendcstr(&log, "\"");
	if ((hdr = spool_getheader(txn->req_hdrs, "Destination"))) {
	    buf_printf(&log, " (destination=%s)", hdr[0]);
	}
	else if ((hdr = spool_getheader(txn->req_hdrs, ":type"))) {
	    buf_printf(&log, " (type=%s", hdr[0]);
	    if ((hdr = spool_getheader(txn->req_hdrs, "Depth"))) {
		buf_printf(&log, "; depth=%s", hdr[0]);
	    }
	    buf_appendcstr(&log, ")");
	}
	buf_printf(&log, " => \"%s\"", error_message(code));
	if (txn->location) {
	    buf_printf(&log, " (location=%s)", txn->location);
	}
	else if (txn->error.desc) {
	    buf_printf(&log, " (error=%s)", txn->error.desc);
	}
	syslog(LOG_INFO, "%s", buf_cstring(&log));
    }


    /* Stop method processing alarm */
    alarm(0);


    /* Status-Line */
    prot_printf(httpd_out, "%s\r\n", http_statusline(code));


    /* Connection Management */
    conn_token = "";
    keepalive = httpd_keepalive;

    switch (code) {
    case HTTP_SWITCH_PROT:
	keepalive = 0;  /* No alarm during TLS negotiation */
	prot_printf(httpd_out, "Upgrade: %s, %s\r\n",
		    TLS_VERSION, HTTP_VERSION);
	prot_puts(httpd_out, "Connection: upgrade\r\n");
	/* Fall through as provisional response */

    case HTTP_CONTINUE:
    case HTTP_PROCESSING:
	/* Provisional response - nothing else needed */

	/* CRLF terminating the header block */
	prot_puts(httpd_out, "\r\n");

	/* Force the response to the client immediately */
	prot_flush(httpd_out);

	/* Reset method processing alarm */
	alarm(keepalive);

	return;

    case HTTP_UPGRADE:
	conn_token = " upgrade,";
	prot_printf(httpd_out, "Upgrade: %s, %s\r\n",
		    TLS_VERSION, HTTP_VERSION);
	/* Fall through as final response */

    default:
	/* Final response */
	if (txn->flags.close) {
	    prot_printf(httpd_out, "Connection:%s close\r\n", conn_token);
	}
	else {
	    prot_printf(httpd_out, "Keep-Alive: timeout=%d\r\n", httpd_timeout);
	    prot_printf(httpd_out, "Connection:%s keep-alive\r\n", conn_token);
	}

	auth_chal = &txn->auth_chal;
	resp_body = &txn->resp_body;
    }


    /* Control Data */
    now = time(0);
    httpdate_gen(datestr, sizeof(datestr), now);
    prot_printf(httpd_out, "Date: %s\r\n", datestr);

    if (httpd_tls_done) {
	prot_puts(httpd_out, "Strict-Transport-Security: max-age=600\r\n");
    }
    if (txn->flags.cc) {
	/* Construct Cache-Control header */
	const char *cc_dirs[] = {
	    "no-cache", "no-transform", "private", NULL
	};

	comma_list_hdr("Cache-Control", cc_dirs, txn->flags.cc);
    }
    if (txn->location) {
	prot_printf(httpd_out, "Location: %s\r\n", txn->location);
    }
    if (resp_body->prefs) {
	/* Construct Preference-Applied header */
	const char *prefs[] = {
	    "return=minimal", "return=representation", "depth-noroot", NULL
	};

	comma_list_hdr("Preference-Applied", prefs, resp_body->prefs);
    }
    if (txn->flags.vary) {
	/* Construct Vary header */
	const char *vary_hdrs[] = {
	    "accept-encoding", "brief", "prefer", NULL
	};

	comma_list_hdr("Vary", vary_hdrs, txn->flags.vary);
    }


    /* Response Context */
    if (config_serverinfo == IMAP_ENUM_SERVERINFO_ON) {
	prot_printf(httpd_out, "Server: %s\r\n", buf_cstring(&serverinfo));
    }

    if (txn->req_tgt.allow & ALLOW_ISCHEDULE) {
	prot_puts(httpd_out, "iSchedule-Version: 1.0\r\n");
	if (resp_body->iserial) {
	    prot_printf(httpd_out, "iSchedule-Capabilities: %ld\r\n",
			resp_body->iserial);
	}
    }

    if (code == HTTP_UNAUTHORIZED) {
	/* Authentication Challenges */
	if (!auth_chal->scheme) {
	    /* Require authentication by advertising all possible schemes */
	    struct auth_scheme_t *scheme;

	    for (scheme = auth_schemes; scheme->name; scheme++) {
		/* Only advertise what is available and
		   can work with the type of connection */
		if ((avail_auth_schemes & (1 << scheme->idx)) &&
		    !(txn->flags.close && (scheme->flags & AUTH_NEED_PERSIST))) {
		    auth_chal->param = NULL;

		    if (scheme->flags & AUTH_SERVER_FIRST) {
			/* Generate the initial challenge */
			http_auth(scheme->name, txn);

			if (!auth_chal->param) continue;  /* If fail, skip it */
		    }
		    WWW_Authenticate(scheme->name, auth_chal->param);
		}
	    }
	}
	else {
	    /* Continue with current authentication exchange */ 
	    WWW_Authenticate(auth_chal->scheme->name, auth_chal->param);
	}
    }
    else {
	/* Authentication completed/unnecessary */
	if (auth_chal->param) {
	    /* Authentication completed with success data */
	    if (auth_chal->scheme->send_success) {
		/* Special handling of success data for this scheme */
		auth_chal->scheme->send_success(auth_chal->scheme->name,
						auth_chal->param);
	    }
	    else {
		/* Default handling of success data */
		WWW_Authenticate(auth_chal->scheme->name, auth_chal->param);
	    }
	}

	switch (txn->meth) {
	case METH_GET:
	    if (code == HTTP_OK) {
		/* Construct Accept-Ranges header for GET and HEAD responses */
		prot_printf(httpd_out, "Accept-Ranges: %s\r\n",
			    txn->flags.ranges ? "bytes" : "none");
	    }
	    break;

	case METH_OPTIONS:
	    if (code != HTTP_OK) break;

	    if (txn->req_tgt.allow & ALLOW_DAV) {
		/* Construct DAV header(s) based on namespace of request URL */
		prot_printf(httpd_out, "DAV: 1,%s 3, access-control%s\r\n",
			    (txn->req_tgt.allow & ALLOW_WRITE) ? " 2," : "",
			    (txn->req_tgt.allow & ALLOW_WRITECOL) ?
			    ", extended-mkcol" : "");
		if (txn->req_tgt.allow & ALLOW_CAL) {
		    prot_printf(httpd_out, "DAV: calendar-access%s\r\n",
				(txn->req_tgt.allow & ALLOW_CAL_SCHED) ?
				", calendar-auto-schedule" : "");
		}
		if (txn->req_tgt.allow & ALLOW_CARD) {
		    prot_puts(httpd_out, "DAV: addressbook\r\n");
		}
	    }

	    /* Fall through and add Allow header(s) */
	    code = HTTP_NOT_ALLOWED;

	default:
	    if (code == HTTP_NOT_ALLOWED) {
		/* Construct Allow header(s) for OPTIONS and 405 response */
		const char *http_meth[] = {
		    "OPTIONS, GET, HEAD", "POST", "PUT", "DELETE", "TRACE", NULL
		};

		comma_list_hdr("Allow", http_meth, txn->req_tgt.allow);

		if (txn->req_tgt.allow & ALLOW_DAV) {
		    prot_puts(httpd_out, "Allow: PROPFIND, REPORT");
		    if (txn->req_tgt.allow & ALLOW_WRITE) {
			prot_puts(httpd_out, ", COPY, MOVE, LOCK, UNLOCK");
		    }
		    if (txn->req_tgt.allow & ALLOW_WRITECOL) {
			prot_puts(httpd_out, ", PROPPATCH, MKCOL, ACL");
			if (txn->req_tgt.allow & ALLOW_CAL) {
			    prot_puts(httpd_out, "\r\nAllow: MKCALENDAR");
			}
		    }
		    prot_puts(httpd_out, "\r\n");
		}
	    }
	}
    }


    /* Validators */
    if (resp_body->lock) {
	prot_printf(httpd_out, "Lock-Token: <%s>\r\n", resp_body->lock);
    }
    if (resp_body->stag) {
	prot_printf(httpd_out, "Schedule-Tag: \"%s\"\r\n", resp_body->stag);
    }
    if (resp_body->etag) {
	prot_printf(httpd_out, "ETag: \"%s\"\r\n", resp_body->etag);
    }
    if (resp_body->lastmod) {
	/* Last-Modified MUST NOT be in the future */
	resp_body->lastmod = MIN(resp_body->lastmod, now);
	httpdate_gen(datestr, sizeof(datestr), resp_body->lastmod);
	prot_printf(httpd_out, "Last-Modified: %s\r\n", datestr);
    }


    /* Representation Metadata */
    if (resp_body->type) {
	prot_printf(httpd_out, "Content-Type: %s\r\n", resp_body->type);

	if (resp_body->enc) {
	    prot_printf(httpd_out, "Content-Encoding: %s\r\n", resp_body->enc);
	}
	if (resp_body->lang) {
	    prot_printf(httpd_out, "Content-Language: %s\r\n", resp_body->lang);
	}
	if (resp_body->loc) {
	    prot_printf(httpd_out, "Content-Location: %s\r\n", resp_body->loc);
	}
    }


    /* Payload */
    switch (code) {
    case HTTP_NO_CONTENT:
    case HTTP_NOT_MODIFIED:
	/* MUST NOT include a body */
	break;

    case HTTP_PARTIAL:
    case HTTP_UNSAT_RANGE:
	if (resp_body->range) {
	    prot_puts(httpd_out, "Content-Range: bytes ");
	    if (code == HTTP_PARTIAL) {
		prot_printf(httpd_out, "%lu-%lu",
			    resp_body->range->first, resp_body->range->last);
	    }
	    else prot_printf(httpd_out, "*");
	    prot_printf(httpd_out, "/%lu\r\n", resp_body->range->len);

	    free(resp_body->range);
	}

	/* Fall through and specify framing */

    default:
	if (txn->flags.te) {
	    /* HTTP/1.1+ only - we use close-delimiting for HTTP/1.0 */
	    if (!txn->flags.ver1_0) {
		prot_puts(httpd_out, "Transfer-Encoding:");
		if (txn->flags.te & TE_GZIP)
		    prot_puts(httpd_out, " gzip,");
		else if (txn->flags.te & TE_DEFLATE)
		    prot_puts(httpd_out, " deflate,");

		/* Any TE implies "chunked", which is always last */
		prot_puts(httpd_out, " chunked\r\n");
	    }
	}
	else prot_printf(httpd_out, "Content-Length: %lu\r\n", resp_body->len);
    }


    /* CRLF terminating the header block */
    prot_puts(httpd_out, "\r\n");
}


static void keep_alive(int sig)
{
    if (sig == SIGALRM) response_header(HTTP_PROCESSING, NULL);
}


/* List of incompressible MIME types */
static const char *comp_mime[] = {
    "image/gif",
    "image/jpeg",
    "image/png",
    NULL
};


/* Determine if a MIME type is incompressible */
static int is_incompressible(const char *type)
{
    const char **m;

    for (m = comp_mime; *m && strcasecmp(*m, type); m++);
    return (*m != NULL);
}


/*
 * Output an HTTP response with body data, compressed as necessary.
 *
 * For chunked body data, an initial call with 'code' != 0 will output
 * a response header and the first body chunk.
 * All subsequent calls should have 'code' = 0 to output just the body chunk.
 * A final call with 'len' = 0 ends the chunked body.
 *
 * NOTE: HTTP/1.0 clients can't handle chunked encoding,
 *       so we use bare chunks and close the connection when done.
 */
void write_body(long code, struct transaction_t *txn,
		const char *buf, unsigned len)
{
#define GZIP_MIN_LEN 300

    static unsigned is_dynamic;

    if (code) {
	/* Initial call - prepare response header based on CE, TE and version */
	is_dynamic = (txn->flags.te & TE_CHUNKED);

	if ((!is_dynamic && len < GZIP_MIN_LEN) ||
	    is_incompressible(txn->resp_body.type)) {
	    /* Don't compress small or imcompressible bodies */
	    txn->flags.ce = CE_IDENTITY;
	    txn->flags.te &= TE_CHUNKED;
	}

	if (txn->flags.te & ~TE_CHUNKED) {
	    /* compressed output will be always chunked (streamed) */
	    txn->flags.te |= TE_CHUNKED;
	}
	else if (txn->flags.ce) {
	    /* set content-encoding */
	    if (txn->flags.ce == CE_GZIP)
		txn->resp_body.enc = "gzip";
	    else if (txn->flags.ce == CE_DEFLATE)
		txn->resp_body.enc = "deflate";

	    /* compressed output will be always chunked (streamed) */
	    txn->flags.te |= TE_CHUNKED;
	}
	else if (!is_dynamic) {
	    /* full body (no encoding) */
	    txn->resp_body.len = len;
	}

	if (txn->flags.ver1_0 && (txn->flags.te & TE_CHUNKED)) {
	    /* HTTP/1.0 close-delimited data */
	    txn->flags.close = 1;
	}

	response_header(code, txn);

	/* MUST NOT send a body for 1xx/204/304 response or any HEAD response */
	switch (code) {
	case HTTP_CONTINUE:
	case HTTP_SWITCH_PROT:
	case HTTP_PROCESSING:
	case HTTP_NO_CONTENT:
	case HTTP_NOT_MODIFIED:
	    return;

	default:
	    if (txn->meth == METH_HEAD) return;
	}
    }

    /* Send [partial] body based on CE and TE */
    if (txn->flags.ce || txn->flags.te & ~TE_CHUNKED) {
#ifdef HAVE_ZLIB
	char zbuf[PROT_BUFSIZE];
	unsigned flush, out;

	if (code) deflateReset(&txn->zstrm);

	/* don't flush until last (zero-length) or only chunk */
	flush = (is_dynamic && len) ? Z_NO_FLUSH : Z_FINISH;

	txn->zstrm.next_in = (Bytef *) buf;
	txn->zstrm.avail_in = len;

	do {
	    txn->zstrm.next_out = (Bytef *) zbuf;
	    txn->zstrm.avail_out = PROT_BUFSIZE;

	    deflate(&txn->zstrm, flush);
	    out = PROT_BUFSIZE - txn->zstrm.avail_out;

	    if (out && !txn->flags.ver1_0) {
		/* HTTP/1.1 chunk of compressed output */
		prot_printf(httpd_out, "%x\r\n", out);
		prot_write(httpd_out, zbuf, out);
		prot_puts(httpd_out, "\r\n");
	    }
	    else {
		/* HTTP/1.0 close-delimited data */
		prot_write(httpd_out, zbuf, out);
	    }

	} while (!txn->zstrm.avail_out);

	if (flush == Z_FINISH && !txn->flags.ver1_0) {
	    /* terminate the HTTP/1.1 body with a zero-length chunk */
	    prot_puts(httpd_out, "0\r\n");
	    /* empty trailer */
	    prot_puts(httpd_out, "\r\n");
	}
#else
	/* XXX should never get here */
	fatal("Content-Encoding requested, but no zlib", EC_SOFTWARE);
#endif /* HAVE_ZLIB */
    }
    else if (is_dynamic && !txn->flags.ver1_0) {
	/* HTTP/1.1 chunk */
	prot_printf(httpd_out, "%x\r\n", len);
	if (len) prot_write(httpd_out, buf, len);
	else {
	    /* empty trailer */
	}
	prot_puts(httpd_out, "\r\n");
    }
    else {
	/* full body or HTTP/1.0 close-delimited data */
	prot_write(httpd_out, buf, len);
    }
}


/* Output an HTTP response with text/html body */
void html_response(long code, struct transaction_t *txn, xmlDocPtr html)
{
    xmlChar *buf;
    int bufsiz;

    /* Dump HTML response tree into a text buffer */
    htmlDocDumpMemoryFormat(html, &buf, &bufsiz, DEBUG ? 1 : 0);

    if (buf) {
	/* Output the XML response */
	txn->resp_body.type = "text/html; charset=utf-8";

	write_body(code, txn, (char *) buf, bufsiz);

	/* Cleanup */
	xmlFree(buf);
    }
    else {
	txn->error.precond = 0;
	txn->error.desc = "Error dumping HTML tree\r\n";
	error_response(HTTP_SERVER_ERROR, txn);
    }
}


/* Output an HTTP response with application/xml body */
void xml_response(long code, struct transaction_t *txn, xmlDocPtr xml)
{
    xmlChar *buf;
    int bufsiz;

    switch (code) {
    case HTTP_OK:
    case HTTP_CREATED:
    case HTTP_NO_CONTENT:
    case HTTP_MULTI_STATUS:
	break;

    default:
	/* Neither Brief nor Prefer affect error response bodies */
	txn->flags.vary &= ~(VARY_BRIEF | VARY_PREFER);
	txn->resp_body.prefs = 0;
    }

    /* Dump XML response tree into a text buffer */
    xmlDocDumpFormatMemoryEnc(xml, &buf, &bufsiz, "utf-8", DEBUG ? 1 : 0);

    if (buf) {
	/* Output the XML response */
	txn->resp_body.type = "application/xml; charset=utf-8";

	write_body(code, txn, (char *) buf, bufsiz);

	/* Cleanup */
	xmlFree(buf);
    }
    else {
	txn->error.precond = 0;
	txn->error.desc = "Error dumping XML tree\r\n";
	error_response(HTTP_SERVER_ERROR, txn);
    }
}


/* Output an HTTP error response with optional XML or HTML body */
void error_response(long code, struct transaction_t *txn)
{
    const char error_body[] = HTML_DOCTYPE
	"<html>\n<head>\n<title>%s</title>\n</head>\n"	\
	"<body>\n<h1>%s</h1>\n<p>%s</p>\n"		\
	"<hr>\n<address>%s Server at %s Port %s</address>\n" \
	"</body>\n</html>\n";
    struct buf *html = &txn->resp_body.payload;

    /* Neither Brief nor Prefer affect error response bodies */
    txn->flags.vary &= ~(VARY_BRIEF | VARY_PREFER);
    txn->resp_body.prefs = 0;

#ifdef WITH_DAV
    if (txn->error.precond) {
	xmlNodePtr root = xml_add_error(NULL, &txn->error, NULL);

	if (root) {
	    xml_response(code, txn, root->doc);
	    xmlFreeDoc(root->doc);
	    return;
	}
    }
#endif

    if (!txn->error.desc) {
	switch (code) {
	    /* 4xx codes */
	case HTTP_BAD_REQUEST:
	    txn->error.desc =
		"The request was not understood by this server.";
	    break;

	case HTTP_NOT_FOUND:
	    txn->error.desc =
		"The requested URL was not found on this server.";
	    break;

	case HTTP_NOT_ALLOWED:
	    txn->error.desc =
		"The requested method is not allowed for the URL.";
	    break;

	case HTTP_GONE:
	    txn->error.desc =
		"The requested URL has been removed from this server.";
	    break;

	    /* 5xx codes */
	case HTTP_SERVER_ERROR:
	    txn->error.desc =
		"The server encountered an internal error.";
	    break;

	case HTTP_NOT_IMPLEMENTED:
	    txn->error.desc =
		"The requested method is not implemented by this server.";
	    break;

	case HTTP_UNAVAILABLE:
	    txn->error.desc =
		"The server is unable to process the request at this time.";
	    break;
	}
    }

    buf_reset(html);
    if (txn->error.desc) {
	const char **hdr, *host = "";
	char *port = NULL;

	if (txn->req_hdrs &&
	    (hdr = spool_getheader(txn->req_hdrs, "Host")) &&
	    hdr[0] && *hdr[0]) {
	    host = (char *) hdr[0];
	    if ((port = strchr(host, ':'))) *port++ = '\0';
	}
	else if (config_serverinfo != IMAP_ENUM_SERVERINFO_OFF) {
	    host = config_servername;
	}
	if (!port) port = strchr(saslprops.iplocalport, ';')+1;

	buf_printf(html, error_body, error_message(code), error_message(code)+4,
		   txn->error.desc, buf_cstring(&serverinfo), host, port);
	txn->resp_body.type = "text/html; charset=utf-8";
    }

    write_body(code, txn, buf_cstring(html), buf_len(html));
}


/* Write cached header (redacting authorization credentials) to buffer. */
static void log_cachehdr(const char *name, const char *contents, void *rock)
{
    struct buf *buf = (struct buf *) rock;

    /* Ignore private headers in our cache */
    if (name[0] == ':') return;

    buf_printf(buf, "%c%s: ", toupper(name[0]), name+1);
    if (!strcmp(name, "authorization")) {
	/* Replace authorization credentials with an ellipsis */
	const char *creds = strchr(contents, ' ') + 1;
	buf_printf(buf, "%.*s%-*s\r\n",
		   creds - contents, contents, strlen(creds), "...");
    }
    else buf_printf(buf, "%s\r\n", contents);
}


/* Perform HTTP Authentication based on the given credentials ('creds').
 * Returns the selected auth scheme and any server challenge in 'chal'.
 * May be called multiple times if auth scheme requires multiple steps.
 * SASL status between steps is maintained in 'status'.
 */
#define BASE64_BUF_SIZE 21848	/* per RFC 4422: ((16K / 3) + 1) * 4  */

static int http_auth(const char *creds, struct transaction_t *txn)
{
    struct auth_challenge_t *chal = &txn->auth_chal;
    static int status = SASL_OK;
    size_t slen;
    const char *clientin = NULL, *user;
    unsigned int clientinlen = 0;
    struct auth_scheme_t *scheme;
    static char base64[BASE64_BUF_SIZE+1];
    const void *canon_user;
    const char **authzid = spool_getheader(txn->req_hdrs, "Authorize-As");
    int i;

    chal->param = NULL;

    /* Split credentials into auth scheme and response */
    slen = strcspn(creds, " \0");
    if ((clientin = strchr(creds, ' '))) clientinlen = strlen(++clientin);

    syslog(LOG_DEBUG,
	   "http_auth: status=%d   scheme='%s'   creds='%.*s%s'   authzid='%s'",
	   status, chal->scheme ? chal->scheme->name : "",
	   slen, creds, clientin ? " <response>" : "",
	   authzid ? authzid[0] : "");

    if (chal->scheme) {
	/* Use current scheme, if possible */
	scheme = chal->scheme;

	if (strncasecmp(scheme->name, creds, slen)) {
	    /* Changing auth scheme -> reset state */
	    syslog(LOG_DEBUG, "http_auth: changing scheme");
	    reset_saslconn(&httpd_saslconn);
	    chal->scheme = NULL;
	    status = SASL_OK;
	}
    }

    if (!chal->scheme) {
	/* Find the client-specified auth scheme */
	syslog(LOG_DEBUG, "http_auth: find client scheme");
	for (scheme = auth_schemes; scheme->name; scheme++) {
	    if (slen && !strncasecmp(scheme->name, creds, slen)) {
		/* Found a supported scheme, see if its available */
		if (!(avail_auth_schemes & (1 << scheme->idx))) scheme = NULL;
		break;
	    }
	}
	if (!scheme || !scheme->name) {
	    /* Didn't find a matching scheme that is available */
	    syslog(LOG_DEBUG, "Unknown auth scheme '%.*s'", slen, creds);
	    return SASL_NOMECH;
	}
	/* We found it! */
	syslog(LOG_DEBUG, "http_auth: found matching scheme: %s", scheme->name);
	chal->scheme = scheme;
	status = SASL_OK;
    }

    /* Base64 decode any client response, if necesary */
    if (clientin && (scheme->flags & AUTH_BASE64)) {
	int r = sasl_decode64(clientin, clientinlen,
			      base64, BASE64_BUF_SIZE, &clientinlen);
	if (r != SASL_OK) {
	    syslog(LOG_ERR, "Base64 decode failed: %s",
		   sasl_errstring(r, NULL, NULL));
	    return r;
	}
	clientin = base64;
    }

#ifdef SASL_HTTP_REQUEST
    /* Setup SASL HTTP request, if necessary */
    if (scheme->flags & AUTH_NEED_BODY) {
	sasl_http_request_t sasl_http_req;

	/* Read body */
	if (!txn->flags.havebody) {
	    txn->flags.havebody = 1;
	    if (read_body(httpd_in, txn->req_hdrs, &txn->req_body,
			  txn->flags.cont | BODY_DECODE, &txn->error.desc)) {
		txn->flags.close = 1;
		return SASL_FAIL;
	    }
	}
	sasl_http_req.method = txn->req_line.meth;
	sasl_http_req.uri = txn->req_line.uri;
	sasl_http_req.entity = (u_char *) buf_cstring(&txn->req_body);
	sasl_http_req.elen = buf_len(&txn->req_body);
	sasl_http_req.non_persist = txn->flags.close;
	sasl_setprop(httpd_saslconn, SASL_HTTP_REQUEST, &sasl_http_req);
    }
#endif /* SASL_HTTP_REQUEST */

    if (scheme->idx == AUTH_BASIC) {
	/* Basic (plaintext) authentication */
	char *pass;

	if (!clientin) {
	    /* Create initial challenge (base64 buffer is static) */
	    snprintf(base64, BASE64_BUF_SIZE,
		     "realm=\"%s\"", config_servername);
	    chal->param = base64;
	    chal->scheme = NULL;  /* make sure we don't reset the SASL ctx */
	    return status;
	}

	/* Split credentials into <user> ':' <pass>.
	 * We are working with base64 buffer, so we can modify it.
	 */
	user = base64;
	pass = strchr(base64, ':');
	if (!pass) {
	    syslog(LOG_ERR, "Basic auth: Missing password");
	    return SASL_BADPARAM;
	}
	*pass++ = '\0';
	
	/* Verify the password */
	status = sasl_checkpass(httpd_saslconn, user, strlen(user),
				pass, strlen(pass));
	memset(pass, 0, strlen(pass));		/* erase plaintext password */

	if (status) {
	    syslog(LOG_NOTICE, "badlogin: %s Basic %s %s",
		   httpd_clienthost, user, sasl_errdetail(httpd_saslconn));

	    /* Don't allow user probing */
	    if (status == SASL_NOUSER) status = SASL_BADAUTH;
	    return status;
	}

	/* Successful authentication - fall through */
    }
    else {
	/* SASL-based authentication (Digest, Negotiate, NTLM) */
	const char *serverout = NULL;
	unsigned int serveroutlen = 0;

	if (status == SASL_CONTINUE) {
	    /* Continue current authentication exchange */
	    syslog(LOG_DEBUG, "http_auth: continue %s", scheme->saslmech);
	    status = sasl_server_step(httpd_saslconn, clientin, clientinlen,
				      &serverout, &serveroutlen);
	}
	else {
	    /* Start new authentication exchange */
	    syslog(LOG_DEBUG, "http_auth: start %s", scheme->saslmech);
	    status = sasl_server_start(httpd_saslconn, scheme->saslmech,
				       clientin, clientinlen,
				       &serverout, &serveroutlen);
	}

	/* Failure - probably bad client response */
	if ((status != SASL_OK) && (status != SASL_CONTINUE)) {
	    syslog(LOG_ERR, "SASL failed: %s",
		   sasl_errstring(status, NULL, NULL));
	    return status;
	}

	/* Base64 encode any server challenge, if necesary */
	if (serverout && (scheme->flags & AUTH_BASE64)) {
	    int r = sasl_encode64(serverout, serveroutlen,
				   base64, BASE64_BUF_SIZE, NULL);
	    if (r != SASL_OK) {
		syslog(LOG_ERR, "Base64 encode failed: %s",
		       sasl_errstring(r, NULL, NULL));
		return r;
	    }
	    serverout = base64;
	}

	chal->param = serverout;

	if (status == SASL_CONTINUE) {
	    /* Need another step to complete authentication */
	    return status;
	}

	/* Successful authentication
	 *
	 * HTTP doesn't support security layers,
	 * so don't attach SASL context to prot layer.
	 */
    }

    /* Get the userid from SASL - already canonicalized */
    status = sasl_getprop(httpd_saslconn, SASL_USERNAME, &canon_user);
    if (status != SASL_OK) {
	syslog(LOG_ERR, "weird SASL error %d getting SASL_USERNAME", status);
	return status;
    }

    if (authzid && *authzid[0]) {
	/* Trying to proxy as another user */
	char authzbuf[MAX_MAILBOX_BUFFER];
	unsigned authzlen;

	/* Canonify the authzid */
	status = mysasl_canon_user(httpd_saslconn, NULL,
				   authzid[0], strlen(authzid[0]),
				   SASL_CU_AUTHZID, NULL,
				   authzbuf, sizeof(authzbuf), &authzlen);
	if (status) {
	    syslog(LOG_NOTICE, "badlogin: %s %s %s invalid user",
		   httpd_clienthost, scheme->name, beautify_string(authzid[0]));
	    return status;
	}
	user = (const char *) canon_user;

	/* See if user is allowed to proxy */
	status = mysasl_proxy_policy(httpd_saslconn, &httpd_proxyctx,
				     authzbuf, authzlen, user, strlen(user),
				     NULL, 0, NULL);

	if (status) {
	    syslog(LOG_NOTICE, "badlogin: %s %s %s %s",
		   httpd_clienthost, scheme->name, user,
		   sasl_errdetail(httpd_saslconn));
	    return status;
	}

	canon_user = authzbuf;
    }

    httpd_userid = xstrdup((const char *) canon_user);

    proc_register("httpd", httpd_clienthost, httpd_userid, (char *)0);

    syslog(LOG_NOTICE, "login: %s %s %s%s %s",
	   httpd_clienthost, httpd_userid, scheme->name,
	   httpd_tls_done ? "+TLS" : "", "User logged in");


    /* Recreate telemetry log entry for request (w/ credentials redacted) */
    assert(!buf_len(&txn->buf));
    buf_printf(&txn->buf, "<%ld<", time(NULL));		/* timestamp */
    buf_printf(&txn->buf, "%s %s %s\r\n",		/* request-line*/
	       txn->req_line.meth, txn->req_line.uri, txn->req_line.ver);
    spool_enum_hdrcache(txn->req_hdrs,			/* header fields */
			&log_cachehdr, &txn->buf);
    buf_appendcstr(&txn->buf, "\r\n");			/* CRLF */
    buf_append(&txn->buf, &txn->req_body);		/* message body */
    buf_appendmap(&txn->buf,				/* buffered input */
		  (const char *) httpd_in->ptr, httpd_in->cnt);

    /* Close IP-based telemetry log */
    if (httpd_logfd != -1) {
	/* Rewind log to current request and overwrite with redacted version */
	ftruncate(httpd_logfd,
		  lseek(httpd_logfd, -buf_len(&txn->buf), SEEK_END));
	write(httpd_logfd, buf_cstring(&txn->buf), buf_len(&txn->buf));
	close(httpd_logfd);
    }

    /* Create new telemetry log based on userid */
    httpd_logfd = telemetry_log(httpd_userid, httpd_in, httpd_out, 0);
    if (httpd_logfd != -1) {
	/* Log credential-redacted request */
	write(httpd_logfd, buf_cstring(&txn->buf), buf_len(&txn->buf));
    }

    buf_reset(&txn->buf);

    /* Do any namespace specific post-auth processing */
    for (i = 0; namespaces[i]; i++) {
	if (namespaces[i]->enabled && namespaces[i]->auth)
	    namespaces[i]->auth(httpd_userid);
    }

    return status;
}


/*************************  Method Execution Routines  ************************/


/* "Open" the requested mailbox.  Either return the existing open
 * mailbox if it matches, or close the existing and open the requested.
 */
int http_mailbox_open(const char *name, struct mailbox **mailbox, int locktype)
{
    int r;

    if (httpd_mailbox && !strcmp(httpd_mailbox->name, name)) {
	r = mailbox_lock_index(httpd_mailbox, locktype);
    }
    else {
	if (httpd_mailbox) {
	    mailbox_close(&httpd_mailbox);
	    httpd_mailbox = NULL;
	}
	if (locktype == LOCK_EXCLUSIVE)
	    r = mailbox_open_iwl(name, &httpd_mailbox);
	else
	    r = mailbox_open_irl(name, &httpd_mailbox);
    }

    *mailbox = httpd_mailbox;
    return r;
}


/* Compare an etag in a header to a resource etag.
 * Returns 0 if a match, non-zero otherwise.
 */
int etagcmp(const char *hdr, const char *etag)
{
    size_t len;

    if (!etag) return -1;		/* no representation	   */
    if (!strcmp(hdr, "*")) return 0;	/* any representation	   */

    len = strlen(etag);
    if (!strncmp(hdr, "W/", 2)) hdr+=2;	/* skip weak prefix	   */
    if (*hdr++ != '\"') return 1;    	/* match/skip open DQUOTE  */
    if (strlen(hdr) != len+1) return 1;	/* make sure lengths match */
    if (hdr[len] != '\"') return 1;    	/* match close DQUOTE	   */

    return strncmp(hdr, etag, len);
}


/* Compare a resource etag to a comma-separated list and/or multiple headers
 * looking for a match.  Returns 1 if a match is found, 0 otherwise.
 */
static unsigned etag_match(const char *hdr[], const char *etag)
{
    unsigned i, match = 0;
    tok_t tok;
    char *token;

    for (i = 0; !match && hdr[i]; i++) {
	tok_init(&tok, hdr[i], ",", TOK_TRIMLEFT|TOK_TRIMRIGHT);
	while (!match && (token = tok_next(&tok))) {
	    if (!etagcmp(token, etag)) match = 1;
	}
	tok_fini(&tok);
    }

    return match;
}


/* Evaluate If header.  Note that we can't short-circuit any of the tests
   because we need to check for a lock-token anywhere in the header */
static int eval_if(const char *hdr, const char *etag, const char *lock_token,
		   unsigned *locked)
{
    unsigned ret = 0;
    tok_t tok_l;
    char *list;

    /* Process each list, ORing the results */
    tok_init(&tok_l, hdr, ")", TOK_TRIMLEFT|TOK_TRIMRIGHT);
    while ((list = tok_next(&tok_l))) {
	unsigned ret_l = 1;
	tok_t tok_c;
	char *cond;

	/* XXX  Need to handle Resource-Tag for Tagged-list (COPY/MOVE dest) */

	/* Process each condition, ANDing the results */
	tok_initm(&tok_c, list+1, "]>", TOK_TRIMLEFT|TOK_TRIMRIGHT);
	while ((cond = tok_next(&tok_c))) {
	    unsigned r, not = 0;

	    if (!strncmp(cond, "Not", 3)) {
		not = 1;
		cond += 3;
		while (*cond == ' ') cond++;
	    }
	    if (*cond == '[') {
		/* ETag */
		r = !etagcmp(cond+1, etag);
	    }
	    else {
		/* State Token */
		if (!lock_token) r = 0;
		else {
		    r = !strcmp(cond+1, lock_token);
		    if (r) {
			/* Correct lock-token has been provided */
			*locked = 0;
		    }
		}
	    }

	    ret_l &= (not ? !r : r);
	}

	tok_fini(&tok_c);

	ret |= ret_l;
    }

    tok_fini(&tok_l);

    return (ret || locked);
}


static int parse_ranges(const char *hdr, unsigned long len,
			struct range **ranges)
{
    int ret = HTTP_UNSAT_RANGE;
    struct range *new, *tail = *ranges = NULL;
    tok_t tok;
    char *token;

    if (!len) return HTTP_OK;  /* need to know length of representation */

    /* we only handle byte-unit */
    if (!hdr || strncmp(hdr, "bytes=", 6)) return HTTP_OK;

    tok_init(&tok, hdr+6, ",", TOK_TRIMLEFT|TOK_TRIMRIGHT);
    while ((token = tok_next(&tok))) {
	/* default to entire representation */
	unsigned long first = 0;
	unsigned long last = len - 1;
	char *p, *endp;

	if (!(p = strchr(token, '-'))) continue;  /* bad byte-range-set */

	if (p == token) {
	    /* suffix-byte-range-spec */
	    unsigned long suffix = strtoul(++p, &endp, 10);

	    if (endp == p || *endp) continue;  /* bad suffix-length */
	    if (!suffix) continue;	/* unsatisfiable suffix-length */
		
	    /* don't start before byte zero */
	    if (suffix < len) first = len - suffix;
	}
	else {
	    /* byte-range-spec */
	    first = strtoul(token, &endp, 10);
	    if (endp != p) continue;      /* bad first-byte-pos */
	    if (first >= len) continue;   /* unsatisfiable first-byte-pos */

	    if (*++p) {
		/* last-byte-pos */
		last = strtoul(p, &endp, 10);
		if (*endp || last < first) continue; /* bad last-byte-pos */

		/* don't go past end of representation */
		if (last >= len) last = len - 1;
	    }
	}

	ret = HTTP_PARTIAL;

	/* Coalesce overlapping ranges, or those with a gap < 80 bytes */
	if (tail &&
	    first >= tail->first && (long) (first - tail->last) < 80) {
	    tail->last = MAX(last, tail->last);
	    continue;
	}

	/* Create a new range and append it to linked list */
	new = xzmalloc(sizeof(struct range));
	new->first = first;
	new->last = last;
	new->len = len;

	if (tail) tail->next = new;
	else *ranges = new;
	tail = new;
    }

    tok_fini(&tok);

    if (ret == HTTP_UNSAT_RANGE) {
	*ranges = new = xzmalloc(sizeof(struct range));
	new->len = len;
    }

    return ret;
}


/* Check headers for any preconditions.
 *
 * Interaction is complex and is documented in RFC 4918 and
 * Section 5 of HTTPbis, Part 4.
 */
int check_precond(struct transaction_t *txn, const void *data,
		  const char *etag, time_t lastmod, unsigned long len)
{
    const char *lock_token = NULL;
    unsigned locked = 0;
    hdrcache_t hdrcache = txn->req_hdrs;
    const char **hdr;
    time_t since;

#ifdef WITH_DAV
    struct dav_data *ddata = (struct dav_data *) data;

    /* Check for a write-lock on the source */
    if (ddata && ddata->lock_expire > time(NULL)) {
	lock_token = ddata->lock_token;

	switch (txn->meth) {
	case METH_DELETE:
	case METH_LOCK:
	case METH_MOVE:
	case METH_POST:
	case METH_PUT:
	    /* State-changing method: Only the lock owner can execute
	       and MUST provide the correct lock-token in an If header */
	    if (strcmp(ddata->lock_ownerid, httpd_userid)) return HTTP_LOCKED;

	    locked = 1;
	    break;

	case METH_UNLOCK:
	    /* State-changing method: Authorized in meth_unlock() */
	    break;

	case METH_ACL:
	case METH_MKCALENDAR:
	case METH_MKCOL:
	case METH_PROPPATCH:
	    /* State-changing method: Locks on collections unsupported */
	    break;

	default:
	    /* Non-state-changing method: Always allowed */
	    break;
	}
    }
#else
    assert(!data);
#endif /* WITH_DAV */

    /* Per RFC 4918, If is similar to If-Match, but with lock-token submission.
       Per Section 5 of HTTPbis, Part 4, LOCK errors supercede preconditions */
    if ((hdr = spool_getheader(hdrcache, "If"))) {
	/* State tokens (sync-token, lock-token) and Etags */
	if (!eval_if(hdr[0], etag, lock_token, &locked))
	    return HTTP_PRECOND_FAILED;
    }

    if (locked) {
	/* Correct lock-token was not provided in If header */
	return HTTP_LOCKED;
    }

    /* Evaluate other precondition headers per Section 5 of HTTPbis, Part 4 */

    /* Step 1 */
    if ((hdr = spool_getheader(hdrcache, "If-Match"))) {
	if (!etag_match(hdr, etag)) return HTTP_PRECOND_FAILED;

	/* Continue to step 3 */
    }

    /* Step 2 */
    else if ((hdr = spool_getheader(hdrcache, "If-Unmodified-Since"))) {
	since = message_parse_date((char *) hdr[0],
				   PARSE_DATE|PARSE_TIME|PARSE_ZONE|
				   PARSE_GMT|PARSE_NOCREATE);

	if (since && (lastmod > since)) return HTTP_PRECOND_FAILED;

	/* Continue to step 3 */
    }

    /* Step 3 */
    if ((hdr = spool_getheader(hdrcache, "If-None-Match"))) {
	if (etag_match(hdr, etag)) {
	    if (txn->meth == METH_GET || txn->meth == METH_HEAD)
		return HTTP_NOT_MODIFIED;
	    else
		return HTTP_PRECOND_FAILED;
	}

	/* Continue to step 5 */
    }

    /* Step 4 */
    else if ((txn->meth == METH_GET || txn->meth == METH_HEAD) &&
	     (hdr = spool_getheader(hdrcache, "If-Modified-Since"))) {
	since = message_parse_date((char *) hdr[0],
				   PARSE_DATE|PARSE_TIME|PARSE_ZONE|
				   PARSE_GMT|PARSE_NOCREATE);

	if (lastmod <= since) return HTTP_NOT_MODIFIED;

	/* Continue to step 5 */
    }

    /* Step 5 */
    if (txn->flags.ranges &&  /* Only if we support Range requests */
	txn->meth == METH_GET && (hdr = spool_getheader(hdrcache, "Range"))) {
	const char *ranges = hdr[0];

	if ((hdr = spool_getheader(hdrcache, "If-Range"))) {
	    since = message_parse_date((char *) hdr[0],
				       PARSE_DATE|PARSE_TIME|PARSE_ZONE|
				       PARSE_GMT|PARSE_NOCREATE);
	}

	/* Only process Range if If-Range isn't present or validator matches */
	if (!hdr || (since && (lastmod <= since)) || !etagcmp(hdr[0], etag))
	    return parse_ranges(ranges, len, &txn->resp_body.range);
    }

    /* Step 6 */
    return HTTP_OK;
}


/* Output multipart/byteranges */
void multipart_byteranges(struct transaction_t *txn, const char *msg_base)
{
    struct range *range = txn->resp_body.range;
    struct buf *body = &txn->resp_body.payload;
    const char *type = txn->resp_body.type;
    const char *preamble =
	"This is a message with multiple parts in MIME format.\r\n";
    char boundary[100];

    /* Create multipart boundary */
    snprintf(boundary, sizeof(boundary), "%s-%ld-%ld-%ld",
	     *spool_getheader(txn->req_hdrs, "Host"),
	     (long) getpid(), (long) time(NULL), (long) rand());

    /* Create Content-Type w/ boundary */
    assert(!buf_len(&txn->buf));
    buf_printf(&txn->buf, "multipart/byteranges; boundary=\"%s\"", boundary);
    txn->resp_body.type = buf_cstring(&txn->buf);

    /* Setup for chunked response and begin output */
    txn->flags.te |= TE_CHUNKED;
    txn->resp_body.range = NULL;
    write_body(HTTP_PARTIAL, txn, preamble, strlen(preamble));

    while (range) {
	unsigned long offset = range->first;
	unsigned long datalen = range->last - range->first + 1;
	struct range *next = range->next;

	/* Output header for next range */
	buf_reset(body);
	buf_printf(body, "\r\n--%s\r\n"
		   "Content-Type: %s\r\n"
		   "Content-Range: bytes %lu-%lu/%lu\r\n\r\n",
		   boundary, type, range->first, range->last, range->len);
	write_body(0, txn, buf_cstring(body), buf_len(body));

	/* Output range data */
	write_body(0, txn, msg_base + offset, datalen);

	/* Cleanup */
	free(range);
	range = next;
    }

    /* Output final boundary */
    buf_reset(body);
    buf_printf(body, "\r\n--%s--\r\n", boundary);
    write_body(0, txn, buf_cstring(body), buf_len(body));

    /* End of output */
    write_body(0, txn, NULL, 0);
}


/* Perform a GET/HEAD request */
int meth_get_doc(struct transaction_t *txn,
		 void *params __attribute__((unused)))
{
    int ret = 0, fd, precond;
    const char *prefix, *path, *ext;
    static struct buf pathbuf = BUF_INITIALIZER;
    struct stat sbuf;
    const char *msg_base = NULL;
    unsigned long msg_size = 0, offset = 0, datalen;
    struct resp_body_t *resp_body = &txn->resp_body;

    /* Serve up static pages */
    prefix = config_getstring(IMAPOPT_HTTPDOCROOT);
    if (!prefix) return HTTP_NOT_FOUND;

    buf_setcstr(&pathbuf, prefix);
    if (!txn->req_tgt.path || !*txn->req_tgt.path ||
	(txn->req_tgt.path[0] == '/' && txn->req_tgt.path[1] == '\0'))
	buf_appendcstr(&pathbuf, "/index.html");
    else
	buf_appendcstr(&pathbuf, txn->req_tgt.path);
    path = buf_cstring(&pathbuf);

    /* See if file exists and get Content-Length & Last-Modified time */
    if (stat(path, &sbuf) || !S_ISREG(sbuf.st_mode)) return HTTP_NOT_FOUND;

    datalen = sbuf.st_size;

    /* Generate Etag */
    assert(!buf_len(&txn->buf));
    buf_printf(&txn->buf, "%ld-%ld", (long) sbuf.st_mtime, (long) sbuf.st_size);

    /* Check any preconditions, including range request */
    txn->flags.ranges = !txn->flags.ce;
    precond = check_precond(txn, NULL, buf_cstring(&txn->buf), sbuf.st_mtime,
			    datalen);

    switch (precond) {
    case HTTP_OK:
	break;

    case HTTP_PARTIAL:
	/* Set data parameters for range */
	offset += resp_body->range->first;
	datalen = resp_body->range->last - resp_body->range->first + 1;
	break;

    case HTTP_NOT_MODIFIED:
	/* Fill in ETag for 304 response */
	resp_body->etag = buf_cstring(&txn->buf);

    default:
	/* We failed a precondition - don't perform the request */
	return precond;
    }

    /* Open and mmap the file */
    if ((fd = open(path, O_RDONLY)) == -1) return HTTP_SERVER_ERROR;
    map_refresh(fd, 1, &msg_base, &msg_size, sbuf.st_size, path, NULL);

    /* Fill in ETag and Last-Modified */
    resp_body->etag = buf_cstring(&txn->buf);
    resp_body->lastmod = sbuf.st_mtime;

    if (resp_body->type) {
	/* Caller has specified the Content-Type */
    }
    else if ((ext = strrchr(txn->req_tgt.path, '.'))) {
	/* Try to use filename extension to identity Content-Type */
	if (!strcasecmp(ext, ".text") || !strcmp(ext, ".txt"))
	    resp_body->type = "text/plain";
	else if (!strcasecmp(ext, ".html") || !strcmp(ext, ".htm"))
	    resp_body->type = "text/html";
	else if (!strcasecmp(ext, ".css"))
	    resp_body->type = "text/css";
	else if (!strcasecmp(ext, ".js"))
	    resp_body->type = "text/javascript";
	else if (!strcasecmp(ext, ".jpeg") || !strcmp(ext, ".jpg"))
	    resp_body->type = "image/jpeg";
	else if (!strcasecmp(ext, ".gif"))
	    resp_body->type = "image/gif";
	else if (!strcasecmp(ext, ".png"))
	    resp_body->type = "image/png";
	else
	    resp_body->type = "application/octet-stream";
    }
    else {
	/* Try to use filetype signatures to identity Content-Type */
	if (msg_size >= 8 &&
	    !memcmp(msg_base, "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A", 8)) {
	    resp_body->type = "image/png";
	} else if (msg_size >= 4 &&
		   !memcmp(msg_base, "\xFF\xD8\xFF\xE0", 4)) {
	    resp_body->type = "image/jpeg";
	} else if (msg_size >= 6 &&
		   (!memcmp(msg_base, "GIF87a", 6) ||
		    !memcmp(msg_base, "GIF89a", 6))) {
	    resp_body->type = "image/gif";
	} else {
	    resp_body->type = "application/octet-stream";
	}
    }

    if (resp_body->range && resp_body->range->next) {
	/* multiple ranges */
	multipart_byteranges(txn, msg_base);
    }
    else write_body(precond, txn, msg_base + offset, datalen);

    map_free(&msg_base, &msg_size);
    close(fd);

    return ret;
}


/* Perform an OPTIONS request */
int meth_options(struct transaction_t *txn, void *params)
{
    parse_path_t parse_path = (parse_path_t) params;
    int r;

    /* Response should not be cached */
    txn->flags.cc |= CC_NOCACHE;

    /* Response doesn't have a body, so no Vary */
    txn->flags.vary = 0;

    /* Special case "*" - show all features/methods available on server */
    if (!strcmp(txn->req_tgt.path, "*")) {
	int i;

	for (i = 0; namespaces[i]; i++) {
	    if (namespaces[i]->enabled)
		txn->req_tgt.allow |= namespaces[i]->allow;
	}
    }
    else if (parse_path) {
	/* Parse the path */
	r = parse_path(txn->req_uri->path, &txn->req_tgt, &txn->error.desc);
	if (r) return r;
    }

    response_header(HTTP_OK, txn);
    return 0;
}


/* Perform an PROPFIND request on "/" iff we support CalDAV */
int meth_propfind_root(struct transaction_t *txn,
		       void *params __attribute__((unused)))
{
    assert(txn);

#ifdef WITH_DAV
    /* Apple iCal and Evolution both check "/" */
    if (!strcmp(txn->req_tgt.path, "/")) {
	if (!httpd_userid) return HTTP_UNAUTHORIZED;

	txn->req_tgt.allow |= ALLOW_DAV;
	return meth_propfind(txn, NULL);
    }
#endif

    return HTTP_NOT_ALLOWED;
}


/* Write cached header to buf, excluding any that might have sensitive data. */
static void trace_cachehdr(const char *name, const char *contents, void *rock)
{
    struct buf *buf = (struct buf *) rock;
    const char **hdr, *sensitive[] =
	{ "authorization", "cookie", "proxy-authorization", NULL };

    /* Ignore private headers in our cache */
    if (name[0] == ':') return;

    for (hdr = sensitive; *hdr && strcmp(name, *hdr); hdr++);

    if (!*hdr) buf_printf(buf, "%c%s: %s\r\n",
			  toupper(name[0]), name+1, contents);
}

/* Perform an TRACE request */
int meth_trace(struct transaction_t *txn, void *params)
{
    parse_path_t parse_path = (parse_path_t) params;
    const char **hdr;
    unsigned long max_fwd = -1;
    struct buf *msg = &txn->resp_body.payload;

    /* Response should not be cached */
    txn->flags.cc |= CC_NOCACHE;

    /* Make sure method is allowed */
    if (!(txn->req_tgt.allow & ALLOW_TRACE)) return HTTP_NOT_ALLOWED;

    if ((hdr = spool_getheader(txn->req_hdrs, "Max-Forwards"))) {
	max_fwd = strtoul(hdr[0], NULL, 10);
    }

    if (max_fwd && parse_path) {
	/* Parse the path */
	int r;

	if ((r = parse_path(txn->req_uri->path,
			    &txn->req_tgt, &txn->error.desc))) return r;

	if (*txn->req_tgt.mboxname) {
	    /* Locate the mailbox */
	    char *server;

	    r = http_mlookup(txn->req_tgt.mboxname, &server, NULL, NULL);
	    if (r) {
		syslog(LOG_ERR, "mlookup(%s) failed: %s",
		       txn->req_tgt.mboxname, error_message(r));
		txn->error.desc = error_message(r);

		switch (r) {
		case IMAP_PERMISSION_DENIED: return HTTP_FORBIDDEN;
		case IMAP_MAILBOX_NONEXISTENT: return HTTP_NOT_FOUND;
		default: return HTTP_SERVER_ERROR;
		}
	    }

	    if (server) {
		/* Remote mailbox */
		struct backend *be;

		be = proxy_findserver(server, &http_protocol, httpd_userid,
				      &backend_cached, NULL, NULL, httpd_in);
		if (!be) return HTTP_UNAVAILABLE;

		return http_pipe_req_resp(be, txn);
	    }

	    /* Local mailbox */
	}
    }

    /* Echo the request back to the client as a message/http:
     *
     * - Piece the Request-line back together
     * - Use all non-sensitive cached headers from client
     */
    buf_printf(msg, "TRACE %s %s\r\n", txn->req_line.uri, txn->req_line.ver);
    spool_enum_hdrcache(txn->req_hdrs, &trace_cachehdr, msg);
    buf_appendcstr(msg, "\r\n");

    txn->resp_body.type = "message/http";
    txn->resp_body.len = buf_len(msg);

    write_body(HTTP_OK, txn, buf_cstring(msg), buf_len(msg));

    return 0;
}