/*
Copyright (c) 2009-2013 Roger Light <roger@atchoo.org>
Copyright (c) 2013 V.Krishn <vkrishn@insteps.net>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of mosquitto nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#ifndef WIN32
#include <unistd.h>
#else
#include <sysstat.h>
#include <process.h>
#include <winsock2.h>
#define snprintf sprintf_s
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

// try using witout them
#include <stddef.h>
#include <stdlib.h>

#include <mosquitto.h>

/* This struct is used to pass data to callbacks.
 * An instance "ud" is created in main() and populated, then passed to
 * mosquitto_new(). */
struct userdata {
	char **topics;
	int topic_count;
	int topic_qos;
	char *username;
	char *password;
	int verbose;
	bool quiet;
	bool no_retain;
	char *fmask;
	char *fmask_resolve;
	char *idtext;
	char *cwd;
	char *path;
	char *fmask_topic;
};

/*
@(#)Purpose:        Create all directories in path
@(#)Author:         J Leffler
@(#)Copyright:      (C) JLSS 1990-91,1997-98,2001,2005,2008,2012
@(#)Note:           Modified by vkrishn@insteps.net
*/
/* ------------------------------------------------------------- */
typedef struct stat Stat;

static int do_mkdir(const char *path, mode_t mode)
{
	Stat st;
	int status = 0;

	if (stat(path, &st) != 0) {
		/* Directory does not exist. EEXIST for race condition */
		if (mkdir(path, mode) != 0 && errno != EEXIST)
			status = -1;
	} else if (!S_ISDIR(st.st_mode)) {
		errno = ENOTDIR;
		status = -1;
	}
	return(status);
}

int mkpath(const char *path, mode_t mode)
{
	char *pp;
	char *sp;
	int  status;
	char *copypath = strdup(path);

	status = 0;
	pp = copypath;
	while (status == 0 && (sp = strchr(pp, '/')) != 0) {
		if (sp != pp) {
			/* Neither root nor double slash in path */
			*sp = '\0';
			status = do_mkdir(copypath, mode);
			*sp = '/';
		}
		pp = sp + 1;
	}
	if (status == 0)
		status = do_mkdir(path, mode);
	free(copypath);
	return (status);
}
/* ------------------------------------------------------------- */


/* 
 * Expand --fmask string options for output filename.
 * DateTime string expansion for --fmask
/* ------------------------------------------------------------- */
#define FMASK_EPOCH 0
#define FMASK_DATE 1
#define FMASK_YEAR 2
#define FMASK_MONTH 3
#define FMASK_DAY 4
#define FMASK_DATETIME 5
#define FMASK_TIME 6
#define FMASK_HOUR 7
#define FMASK_MINUTE 8
#define FMASK_SECOND 9

const char *datetime(int fmt)
{
	int n;
	int size = 16;     /* limit 16 bytes. */
	char *dt;
	if ((dt = malloc(size)) == NULL)
	       return NULL;

	time_t current;
	struct tm      *now;
	current  = time(NULL);
	now = localtime(&current);

	switch(fmt) {
		case FMASK_EPOCH:
			n = snprintf(dt, size, "%02d", current);
			break;
		case FMASK_DATE:
			n = snprintf(dt, size, "%02d-%02d-%02d", 
				now->tm_year+1900, now->tm_mon+1, now->tm_mday);
			break;
		case FMASK_YEAR:
			n = snprintf(dt, size, "%02d", now->tm_year+1900);
			break;
		case FMASK_MONTH:
			n = snprintf(dt, size, "%02d", now->tm_mon+1);
			break;
		case FMASK_DAY:
			n = snprintf(dt, size, "%02d", now->tm_mday);
			break;
		case FMASK_DATETIME:
			n = snprintf(dt, size, "%02d%02d%02d-%02d%02d%02d", 
				now->tm_year+1900, now->tm_mon+1, now->tm_mday,
				now->tm_hour, now->tm_min, now->tm_sec);
			break;
		case FMASK_TIME:
			n = snprintf(dt, size, "%02d-%02d-%02d", 
				     now->tm_hour, now->tm_min, now->tm_sec);
			break;
		case FMASK_HOUR:
			n = snprintf(dt, size, "%02d", now->tm_hour);
			break;
		case FMASK_MINUTE:
			n = snprintf(dt, size, "%02d", now->tm_min);
			break;
		case FMASK_SECOND:
			n = snprintf(dt, size, "%02d", now->tm_sec);
			break;
		default:
			return NULL;
			break;

	}

        if (n > -1 && n < size) {
             return dt;
        } else {
             free(dt);
             return NULL;
        }
}
/* ------------------------------------------------------------- */

/* 
 * Expand --fmask string options for output filename.
/* ------------------------------------------------------------- */
void *_fmask(char *fmask, void *obj)
{
	struct userdata *ud;
	
	assert(obj);
	ud = (struct userdata *)obj;

	char *str1, *str2, *token, *subtoken;
	char *saveptr1, *saveptr2;
	int j, n;

	//char buf[500];      /* limit 500 bytes. */
	//char *cwd = getcwd(buf, 100);
	char *path, *prog, *fixed;
	path = strdup (fmask);
	prog = strdup (fmask);
	path = dirname (path);
	ud->path = path;
	prog = basename (prog);
	
	char f[1000];      /* limit 1000 bytes. */
	char *to = f;
	const char *dt;
	
	for (j = 1, str1 = prog; ; j++, str1 = NULL) {
		token = strtok_r(str1, "@", &saveptr1);
		if (token == NULL)
			break;
		for (str2 = token; ; str2 = NULL) {
			subtoken = strtok_r(str2, "", &saveptr2);
			if (subtoken == NULL)
			break;

			/* format type */
			if(!strcmp(subtoken, "epoch")) {
				dt = datetime(0);
			} else if(!strcmp(subtoken, "date")) {
				dt = datetime(1);
			} else if(!strcmp(subtoken, "year")) {
				dt = datetime(2);
			} else if(!strcmp(subtoken, "month")) {
				dt = datetime(3);
			} else if(!strcmp(subtoken, "day")) {
				dt = datetime(4);
			} else if(!strcmp(subtoken, "datetime")) {
				dt = datetime(5);
			} else if(!strcmp(subtoken, "time")) {
				dt = datetime(6);
			} else if(!strcmp(subtoken, "hour")) {
				dt = datetime(7);
			} else if(!strcmp(subtoken, "min")) {
				dt = datetime(8);
			} else if(!strcmp(subtoken, "sec")) {
				dt = datetime(9);
			} else if(!strcmp(subtoken, "topic")) {
				dt = ud->fmask_topic;
			} else if(!strcmp(subtoken, "topic1")) {
				dt = ud->topics[0];
			} else if(!strcmp(subtoken, "topic2")) {
				dt = ud->topics[1];
			} else if(!strcmp(subtoken, "topic3")) {
				dt = ud->topics[2];
			} else if(!strcmp(subtoken, "topic4")) {
				dt = ud->topics[3];
			} else if(!strcmp(subtoken, "topic5")) {
				dt = ud->topics[4];
			} else if(!strcmp(subtoken, "topic6")) {
				dt = ud->topics[5];
			} else if(!strcmp(subtoken, "topic7")) {
				dt = ud->topics[6];
			} else if(!strcmp(subtoken, "topic8")) {
				dt = ud->topics[7];
			} else if(!strcmp(subtoken, "topic9")) {
				dt = ud->topics[8];
			} else if(!strcmp(subtoken, "id")) {
				dt = ud->idtext;
			} else {
				dt = strdup (subtoken);
			}
			
			to = stpcpy (to, dt);
			to = stpcpy (to, "-");
		}
	}

	f[strlen(f)-1] = '\0';
	ud->fmask_resolve = f;

}

/* 
 * File open with given mode.
 * returns file descriptor (fd)
/* ------------------------------------------------------------- */
FILE *_mosquitto_fopen(const char *path, const char *mode)
{
#ifdef WIN32
	char buf[MAX_PATH];
	int rc;
	rc = ExpandEnvironmentStrings(path, buf, MAX_PATH);
	if(rc == 0 || rc == MAX_PATH) {
		return NULL;
	}else {
		return fopen(buf, mode);
	}
#else
	return fopen(path, mode);
#endif
}

void my_message_file_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
	struct userdata *ud;

	assert(obj);
	ud = (struct userdata *)obj;

	if(message->retain && ud->no_retain) return;
	ud->fmask_topic = message->topic;

	//char buf[100];
	//char *cwd = getcwd(buf, 100);
	//ud->cwd = cwd;
	FILE *fptr = NULL;

	_fmask(ud->fmask, ud);
	
	char file[200];      /* limit 200 bytes. */
	char *to = file;
	to = stpcpy (to, ud->path);
	to = stpcpy (to, "/");
	to = stpcpy (to, ud->fmask_resolve);

	char *path, *prog, *fixed;
	path = strdup (file);
	prog = strdup (file);
	path = dirname (path);
	prog = basename (prog);
	
	int status = mkpath(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	
	fptr = _mosquitto_fopen(file, "a");
	if(!fptr){
		fprintf(stderr, "Error: cannot open outfile, using stdout.\n");
		// need to do normal stdout
		//mosquitto_message_callback_set(mosq, "my_message_callback");
	} else{
		if(ud->verbose){
			if(message->payloadlen){
				fprintf(fptr, "%s %s\n", message->topic, (const char *)message->payload);
			}else{
				fprintf(fptr, "%s (null)\n", message->topic);
			}
		}else{
			if(message->payloadlen){
				fprintf(fptr, "%s\n", (const char *)message->payload);
			}
		}
		fclose(fptr);
	}
	
}

void my_message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
	struct userdata *ud;

	assert(obj);
	ud = (struct userdata *)obj;

	if(message->retain && ud->no_retain) return;

	if(ud->verbose){
		if(message->payloadlen){
			printf("%s %s\n", message->topic, (const char *)message->payload);
		}else{
			printf("%s (null)\n", message->topic);
		}
		fflush(stdout);
	}else{
		if(message->payloadlen){
			printf("%s\n", (const char *)message->payload);
			fflush(stdout);
		}
	}
}

void my_connect_callback(struct mosquitto *mosq, void *obj, int result)
{
	int i;
	struct userdata *ud;

	assert(obj);
	ud = (struct userdata *)obj;

	if(!result){
		for(i=0; i<ud->topic_count; i++){
			mosquitto_subscribe(mosq, NULL, ud->topics[i], ud->topic_qos);
		}
	}else{
		if(result && !ud->quiet){
			fprintf(stderr, "%s\n", mosquitto_connack_string(result));
		}
	}
}

void my_subscribe_callback(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
	int i;
	struct userdata *ud;

	assert(obj);
	ud = (struct userdata *)obj;

	if(!ud->quiet) printf("Subscribed (mid: %d): %d", mid, granted_qos[0]);
	for(i=1; i<qos_count; i++){
		if(!ud->quiet) printf(", %d", granted_qos[i]);
	}
	if(!ud->quiet) printf("\n");
}

void my_log_callback(struct mosquitto *mosq, void *obj, int level, const char *str)
{
	printf("%s\n", str);
}

void print_usage(void)
{
	int major, minor, revision;

	mosquitto_lib_version(&major, &minor, &revision);
	printf("mosquitto_sub is a simple mqtt client that will subscribe to a single topic and print all messages it receives.\n");
	printf("mosquitto_sub version %s running on libmosquitto %d.%d.%d.\n\n", VERSION, major, minor, revision);
	printf("Usage: mosquitto_sub [-c] [-h host] [-k keepalive] [-p port] [-q qos] [-R] [-v] -t topic ...\n");
	printf("                     [-A bind_address]\n");
	printf("                     [-i id] [-I id_prefix]\n");
	printf("                     [-d] [--quiet]\n");
	printf("                     [-u username [-P password]]\n");
	printf("                     [--fmask outfile]\n");
	printf("                     [--will-topic [--will-payload payload] [--will-qos qos] [--will-retain]]\n");
#ifdef WITH_TLS
	printf("                     [{--cafile file | --capath dir} [--cert file] [--key file] [--insecure]]\n");
#ifdef WITH_TLS_PSK
	printf("                     [--psk hex-key --psk-identity identity]\n");
#endif
#endif
	printf("       mosquitto_sub --help\n\n");
	printf(" -A : bind the outgoing socket to this host/ip address. Use to control which interface\n");
	printf("      the client communicates over.\n");
	printf(" -c : disable 'clean session' (store subscription and pending messages when client disconnects).\n");
	printf(" -d : enable debug messages.\n");
	printf(" -h : mqtt host to connect to. Defaults to localhost.\n");
	printf(" -i : id to use for this client. Defaults to mosquitto_sub_ appended with the process id.\n");
	printf(" -I : define the client id as id_prefix appended with the process id. Useful for when the\n");
	printf("      broker is using the clientid_prefixes option.\n");
	printf(" -k : keep alive in seconds for this client. Defaults to 60.\n");
	printf(" -p : network port to connect to. Defaults to 1883.\n");
	printf(" -q : quality of service level to use for the subscription. Defaults to 0.\n");
	printf(" -R : do not print stale messages (those with retain set).\n");
	printf(" -t : mqtt topic to subscribe to. May be repeated multiple times.\n");
	printf(" -u : provide a username (requires MQTT 3.1 broker)\n");
	printf(" -v : print published messages verbosely.\n");
	printf(" -P : provide a password (requires MQTT 3.1 broker)\n");
	printf(" --help : display this message.\n");
	printf(" --quiet : don't print error messages.\n");
	printf(" --fmask : path to message outfile\n");
	printf("            allowed masks are:\n");
	printf("            @[epoch|date|year|month|day|datetime|hour|min|sec|id|topic[1-9]] \n");
	printf("            eg. --fmask='@id@date@topic' for file id-2010-12-21-topicname\n");
	printf(" --will-payload : payload for the client Will, which is sent by the broker in case of\n");
	printf("                  unexpected disconnection. If not given and will-topic is set, a zero\n");
	printf("                  length message will be sent.\n");
	printf(" --will-qos : QoS level for the client Will.\n");
	printf(" --will-retain : if given, make the client Will retained.\n");
	printf(" --will-topic : the topic on which to publish the client Will.\n");
#ifdef WITH_TLS
	printf(" --cafile : path to a file containing trusted CA certificates to enable encrypted\n");
	printf("            certificate based communication.\n");
	printf(" --capath : path to a directory containing trusted CA certificates to enable encrypted\n");
	printf("            communication.\n");
	printf(" --cert : client certificate for authentication, if required by server.\n");
	printf(" --key : client private key for authentication, if required by server.\n");
	printf(" --tls-version : TLS protocol version, can be one of tlsv1.2 tlsv1.1 or tlsv1.\n");
	printf("                 Defaults to tlsv1.2 if available.\n");
	printf(" --insecure : do not check that the server certificate hostname matches the remote\n");
	printf("              hostname. Using this option means that you cannot be sure that the\n");
	printf("              remote host is the server you wish to connect to and so is insecure.\n");
	printf("              Do not use this option in a production environment.\n");
#ifdef WITH_TLS_PSK
	printf(" --psk : pre-shared-key in hexadecimal (no leading 0x) to enable TLS-PSK mode.\n");
	printf(" --psk-identity : client identity string for TLS-PSK mode.\n");
#endif
#endif
	printf("\nSee http://mosquitto.org/ for more information.\n\n");
}

int main(int argc, char *argv[])
{
	char *id = NULL;
	char *id_prefix = NULL;
	int i;
	char *host = "localhost";
	int port = 1883;
	char *bind_address = NULL;
	int keepalive = 60;
	bool clean_session = true;
	bool debug = false;
	struct mosquitto *mosq = NULL;
	int rc;
	char hostname[256];
	char err[1024];
	struct userdata ud;
	int len;
	
	char *will_payload = NULL;
	long will_payloadlen = 0;
	int will_qos = 0;
	bool will_retain = false;
	char *will_topic = NULL;

	bool insecure = false;
	//char *fmask = NULL;
	char *cafile = NULL;
	char *capath = NULL;
	char *certfile = NULL;
	char *keyfile = NULL;
	char *tls_version = NULL;

	char *psk = NULL;
	char *psk_identity = NULL;

	memset(&ud, 0, sizeof(struct userdata));

	for(i=1; i<argc; i++){
		if(!strcmp(argv[i], "-p") || !strcmp(argv[i], "--port")){
			if(i==argc-1){
				fprintf(stderr, "Error: -p argument given but no port specified.\n\n");
				print_usage();
				return 1;
			}else{
				port = atoi(argv[i+1]);
				if(port<1 || port>65535){
					fprintf(stderr, "Error: Invalid port given: %d\n", port);
					print_usage();
					return 1;
				}
			}
			i++;
		}else if(!strcmp(argv[i], "-A")){
			if(i==argc-1){
				fprintf(stderr, "Error: -A argument given but no address specified.\n\n");
				print_usage();
				return 1;
			}else{
				bind_address = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "-c") || !strcmp(argv[i], "--disable-clean-session")){
			clean_session = false;
		}else if(!strcmp(argv[i], "--fmask")){
			if(i==argc-1){
				fprintf(stderr, "Error: --fmask argument given but no file specified.\n\n");
				print_usage();
				return 1;
			}else{
				//fmask = argv[i+1];
				ud.fmask = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--cafile")){
			if(i==argc-1){
				fprintf(stderr, "Error: --cafile argument given but no file specified.\n\n");
				print_usage();
				return 1;
			}else{
				cafile = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--capath")){
			if(i==argc-1){
				fprintf(stderr, "Error: --capath argument given but no directory specified.\n\n");
				print_usage();
				return 1;
			}else{
				capath = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--cert")){
			if(i==argc-1){
				fprintf(stderr, "Error: --cert argument given but no file specified.\n\n");
				print_usage();
				return 1;
			}else{
				certfile = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "-d") || !strcmp(argv[i], "--debug")){
			debug = true;
		}else if(!strcmp(argv[i], "--help")){
			print_usage();
			return 0;
		}else if(!strcmp(argv[i], "-h") || !strcmp(argv[i], "--host")){
			if(i==argc-1){
				fprintf(stderr, "Error: -h argument given but no host specified.\n\n");
				print_usage();
				return 1;
			}else{
				host = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--insecure")){
			insecure = true;
		}else if(!strcmp(argv[i], "-i") || !strcmp(argv[i], "--id")){
			if(id_prefix){
				fprintf(stderr, "Error: -i and -I argument cannot be used together.\n\n");
				print_usage();
				return 1;
			}
			if(i==argc-1){
				fprintf(stderr, "Error: -i argument given but no id specified.\n\n");
				print_usage();
				return 1;
			}else{
				id = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "-I") || !strcmp(argv[i], "--id-prefix")){
			if(id){
				fprintf(stderr, "Error: -i and -I argument cannot be used together.\n\n");
				print_usage();
				return 1;
			}
			if(i==argc-1){
				fprintf(stderr, "Error: -I argument given but no id prefix specified.\n\n");
				print_usage();
				return 1;
			}else{
				id_prefix = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "-k") || !strcmp(argv[i], "--keepalive")){
			if(i==argc-1){
				fprintf(stderr, "Error: -k argument given but no keepalive specified.\n\n");
				print_usage();
				return 1;
			}else{
				keepalive = atoi(argv[i+1]);
				if(keepalive>65535){
					fprintf(stderr, "Error: Invalid keepalive given: %d\n", keepalive);
					print_usage();
					return 1;
				}
			}
			i++;
		}else if(!strcmp(argv[i], "--key")){
			if(i==argc-1){
				fprintf(stderr, "Error: --key argument given but no file specified.\n\n");
				print_usage();
				return 1;
			}else{
				keyfile = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--psk")){
			if(i==argc-1){
				fprintf(stderr, "Error: --psk argument given but no key specified.\n\n");
				print_usage();
				return 1;
			}else{
				psk = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--psk-identity")){
			if(i==argc-1){
				fprintf(stderr, "Error: --psk-identity argument given but no identity specified.\n\n");
				print_usage();
				return 1;
			}else{
				psk_identity = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "-q") || !strcmp(argv[i], "--qos")){
			if(i==argc-1){
				fprintf(stderr, "Error: -q argument given but no QoS specified.\n\n");
				print_usage();
				return 1;
			}else{
				ud.topic_qos = atoi(argv[i+1]);
				if(ud.topic_qos<0 || ud.topic_qos>2){
					fprintf(stderr, "Error: Invalid QoS given: %d\n", ud.topic_qos);
					print_usage();
					return 1;
				}
			}
			i++;
		}else if(!strcmp(argv[i], "--quiet")){
			ud.quiet = true;
		}else if(!strcmp(argv[i], "-R")){
			ud.no_retain = true;
		}else if(!strcmp(argv[i], "-t") || !strcmp(argv[i], "--topic")){
			if(i==argc-1){
				fprintf(stderr, "Error: -t argument given but no topic specified.\n\n");
				print_usage();
				return 1;
			}else{
				ud.topic_count++;
				ud.topics = realloc(ud.topics, ud.topic_count*sizeof(char *));
				ud.topics[ud.topic_count-1] = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--tls-version")){
			if(i==argc-1){
				fprintf(stderr, "Error: --tls-version argument given but no version specified.\n\n");
				print_usage();
				return 1;
			}else{
				tls_version = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "-u") || !strcmp(argv[i], "--username")){
			if(i==argc-1){
				fprintf(stderr, "Error: -u argument given but no username specified.\n\n");
				print_usage();
				return 1;
			}else{
				ud.username = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "-v") || !strcmp(argv[i], "--verbose")){
			ud.verbose = 1;
		}else if(!strcmp(argv[i], "-P") || !strcmp(argv[i], "--pw")){
			if(i==argc-1){
				fprintf(stderr, "Error: -P argument given but no password specified.\n\n");
				print_usage();
				return 1;
			}else{
				ud.password = argv[i+1];
			}
			i++;
		}else if(!strcmp(argv[i], "--will-payload")){
			if(i==argc-1){
				fprintf(stderr, "Error: --will-payload argument given but no will payload specified.\n\n");
				print_usage();
				return 1;
			}else{
				will_payload = argv[i+1];
				will_payloadlen = strlen(will_payload);
			}
			i++;
		}else if(!strcmp(argv[i], "--will-qos")){
			if(i==argc-1){
				fprintf(stderr, "Error: --will-qos argument given but no will QoS specified.\n\n");
				print_usage();
				return 1;
			}else{
				will_qos = atoi(argv[i+1]);
				if(will_qos < 0 || will_qos > 2){
					fprintf(stderr, "Error: Invalid will QoS %d.\n\n", will_qos);
					return 1;
				}
			}
			i++;
		}else if(!strcmp(argv[i], "--will-retain")){
			will_retain = true;
		}else if(!strcmp(argv[i], "--will-topic")){
			if(i==argc-1){
				fprintf(stderr, "Error: --will-topic argument given but no will topic specified.\n\n");
				print_usage();
				return 1;
			}else{
				will_topic = argv[i+1];
			}
			i++;
		}else{
			fprintf(stderr, "Error: Unknown option '%s'.\n",argv[i]);
			print_usage();
			return 1;
		}
	}

	if(clean_session == false && (id_prefix || !id)){
		if(!ud.quiet) fprintf(stderr, "Error: You must provide a client id if you are using the -c option.\n");
		return 1;
	}

	if(ud.topic_count == 0){
		fprintf(stderr, "Error: You must specify a topic to subscribe to.\n");
		print_usage();
		return 1;
	}
	if(will_payload && !will_topic){
		fprintf(stderr, "Error: Will payload given, but no will topic given.\n");
		print_usage();
		return 1;
	}
	if(will_retain && !will_topic){
		fprintf(stderr, "Error: Will retain given, but no will topic given.\n");
		print_usage();
		return 1;
	}
	if(ud.password && !ud.username){
		if(!ud.quiet) fprintf(stderr, "Warning: Not using password since username not set.\n");
	}
	if((certfile && !keyfile) || (keyfile && !certfile)){
		fprintf(stderr, "Error: Both certfile and keyfile must be provided if one of them is.\n");
		print_usage();
		return 1;
	}
	if((cafile || capath) && psk){
		if(!ud.quiet) fprintf(stderr, "Error: Only one of --psk or --cafile/--capath may be used at once.\n");
		return 1;
	}
	if(psk && !psk_identity){
		if(!ud.quiet) fprintf(stderr, "Error: --psk-identity required if --psk used.\n");
		return 1;
	}

	mosquitto_lib_init();

	if(id_prefix){
		id = malloc(strlen(id_prefix)+10);
		if(!id){
			if(!ud.quiet) fprintf(stderr, "Error: Out of memory.\n");
			mosquitto_lib_cleanup();
			return 1;
		}
		snprintf(id, strlen(id_prefix)+10, "%s%d", id_prefix, getpid());
	}else if(!id){
		hostname[0] = '\0';
		gethostname(hostname, 256);
		hostname[255] = '\0';
		len = strlen("mosqsub/-") + 6 + strlen(hostname);
		id = malloc(len);
		if(!id){
			if(!ud.quiet) fprintf(stderr, "Error: Out of memory.\n");
			mosquitto_lib_cleanup();
			return 1;
		}
		snprintf(id, len, "mosqsub/%d-%s", getpid(), hostname);
		if(strlen(id) > MOSQ_MQTT_ID_MAX_LENGTH){
			/* Enforce maximum client id length of 23 characters */
			id[MOSQ_MQTT_ID_MAX_LENGTH] = '\0';
		}
	}

	mosq = mosquitto_new(id, clean_session, &ud);
	ud.idtext =  id;
	if(!mosq){
		switch(errno){
			case ENOMEM:
				if(!ud.quiet) fprintf(stderr, "Error: Out of memory.\n");
				break;
			case EINVAL:
				if(!ud.quiet) fprintf(stderr, "Error: Invalid id and/or clean_session.\n");
				break;
		}
		mosquitto_lib_cleanup();
		return 1;
	}
	if(debug){
		mosquitto_log_callback_set(mosq, my_log_callback);
	}
	if(will_topic && mosquitto_will_set(mosq, will_topic, will_payloadlen, will_payload, will_qos, will_retain)){
		if(!ud.quiet) fprintf(stderr, "Error: Problem setting will.\n");
		mosquitto_lib_cleanup();
		return 1;
	}
	if(ud.username && mosquitto_username_pw_set(mosq, ud.username, ud.password)){
		if(!ud.quiet) fprintf(stderr, "Error: Problem setting username and password.\n");
		mosquitto_lib_cleanup();
		return 1;
	}
	if((cafile || capath) && mosquitto_tls_set(mosq, cafile, capath, certfile, keyfile, NULL)){
		if(!ud.quiet) fprintf(stderr, "Error: Problem setting TLS options.\n");
		mosquitto_lib_cleanup();
		return 1;
	}
	if(insecure && mosquitto_tls_insecure_set(mosq, true)){
		if(!ud.quiet) fprintf(stderr, "Error: Problem setting TLS insecure option.\n");
		mosquitto_lib_cleanup();
		return 1;
	}
	if(psk && mosquitto_tls_psk_set(mosq, psk, psk_identity, NULL)){
		if(!ud.quiet) fprintf(stderr, "Error: Problem setting TLS-PSK options.\n");
		mosquitto_lib_cleanup();
		return 1;
	}
	if(tls_version && mosquitto_tls_opts_set(mosq, 1, tls_version, NULL)){
		if(!ud.quiet) fprintf(stderr, "Error: Problem setting TLS options.\n");
		mosquitto_lib_cleanup();
		return 1;
	}
	mosquitto_connect_callback_set(mosq, my_connect_callback);
//	mosquitto_message_callback_set(mosq, my_message_callback);
	mosquitto_message_callback_set(mosq, my_message_file_callback);
	if(debug){
		mosquitto_subscribe_callback_set(mosq, my_subscribe_callback);
	}

	rc = mosquitto_connect_bind(mosq, host, port, keepalive, bind_address);
	if(rc){
		if(!ud.quiet){
			if(rc == MOSQ_ERR_ERRNO){
#ifndef WIN32
				strerror_r(errno, err, 1024);
#else
				FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM, NULL, errno, 0, (LPTSTR)&err, 1024, NULL);
#endif
				fprintf(stderr, "Error: %s\n", err);
			}else{
				fprintf(stderr, "Unable to connect (%d).\n", rc);
			}
		}
		return rc;
		mosquitto_lib_cleanup();
	}

	rc = mosquitto_loop_forever(mosq, -1, 1);

	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();

	if(rc){
		if(rc == MOSQ_ERR_ERRNO){
			fprintf(stderr, "Error: %s\n", strerror(errno));
		}else{
			fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
		}
	}
	return rc;
}
