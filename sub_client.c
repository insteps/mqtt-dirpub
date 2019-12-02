/*
Copyright (c) 2009-2018 Roger Light <roger@atchoo.org>
Copyright (c) 2013-2019 V.Krishn <vkrishn@insteps.net>

All rights reserved. This program and the accompanying materials
are made available under the terms of the Eclipse Public License v1.0
and Eclipse Distribution License v1.0 which accompany this distribution.
 
The Eclipse Public License is available at
   http://www.eclipse.org/legal/epl-v10.html
and the Eclipse Distribution License is available at
  http://www.eclipse.org/org/documents/edl-v10.php.
 
Contributors:
   Roger Light - initial implementation and documentation.
   V Krishn    - implement dirpub.
*/

#include "config.h"

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libgen.h> /* dirname, basename */
#ifndef WIN32
#include <unistd.h>
#include <signal.h>
#else
#include <sysstat.h>
#include <process.h>
#include <winsock2.h>
#define snprintf sprintf_s
#endif
#include <sys/stat.h>
#include <sys/types.h>

#include <mosquitto.h>
#include "client_shared.h"

bool process_messages = true;
int msg_count = 0;
struct mosquitto *mosq = NULL;

#ifndef WIN32
void my_signal_handler(int signum)
{
	if(signum == SIGALRM){
		process_messages = false;
		mosquitto_disconnect(mosq);
	}
}
#endif

void print_message(struct mosq_config *cfg, const struct mosquitto_message *message);
int mkpath(const char *path, mode_t mode);
void _fmask(char *fmask, void *obj);

/* 
File open with given mode.
returns file descriptor (fd)
*/
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
	struct mosq_config *cfg;
	int i;
	bool res;

	if(process_messages == false) return;

	assert(obj);
	cfg = (struct mosq_config *)obj;

	if(cfg->retained_only && !message->retain && process_messages){
		process_messages = false;
		mosquitto_disconnect(mosq);
		return;
	}
    
	if(message->retain && cfg->no_retain) return;
	if(cfg->filter_outs){
		for(i=0; i<cfg->filter_out_count; i++){
			mosquitto_topic_matches_sub(cfg->filter_outs[i], message->topic, &res);
			if(res) return;
		}
	}
	cfg->fmask_topic = message->topic;

	FILE *fptr = NULL;

	_fmask(cfg->fmask, cfg);

	char *path, *prog;
	path = dirname(strdup(cfg->ffmask));
	prog = basename(strdup(cfg->ffmask));
	
	mkpath(path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH);
	
	/* reasonable method to distinguish between directory 
	 * and a writable node (by default is off) */
	if(cfg->nodesuffix) {
		char *sf = cfg->nsuffix;      /* limit 16 bytes. */
		sf = stpcpy(sf, cfg->nodesuffix);
		if(cfg->nsuffix) {
			char *to = cfg->ffmask;
			to = stpcpy(to, path);
			to = stpcpy(to, "/");
			if(prog) {
				to = stpcpy(to, prog);
			}
			to = stpcpy(to, ".");
			to = stpcpy(to, cfg->nsuffix);
		}
	}

	if(cfg->overwrite) {
		fptr = _mosquitto_fopen(cfg->ffmask, "w");
	} else {
		fptr = _mosquitto_fopen(cfg->ffmask, "a");
	}

	if(!fptr){
		fprintf(stderr, "Error: cannot open outfile, using stdout - %s\n", cfg->ffmask);
		// need to do normal stdout
		//mosquitto_message_callback_set(mosq, "my_message_callback");
	} else{
		if(cfg->verbose){
			if(message->payloadlen){
				fprintf(fptr, "%s ", message->topic);
				fwrite(message->payload, 1, message->payloadlen, fptr);
				if(cfg->eol){
					fprintf(fptr, "\n");
				}
			}else{
				if(cfg->eol){
					fprintf(fptr, "%s (null)\n", message->topic);
				}
			}
		}else{
			if(message->payloadlen){
				fwrite(message->payload, 1, message->payloadlen, fptr);
				if(cfg->eol){
					fprintf(fptr, "\n");
				}
			}
		}
		fclose(fptr);
	}
	if(cfg->msg_count>0){
		msg_count++;
		if(cfg->msg_count == msg_count){
			process_messages = false;
			mosquitto_disconnect(mosq);
		}
	}
}

void my_message_callback(struct mosquitto *mosq, void *obj, const struct mosquitto_message *message)
{
	struct mosq_config *cfg;
	int i;
	bool res;

	if(process_messages == false) return;

	assert(obj);
	cfg = (struct mosq_config *)obj;

	if(cfg->retained_only && !message->retain && process_messages){
		process_messages = false;
		mosquitto_disconnect(mosq);
		return;
	}

	if(message->retain && cfg->no_retain) return;
	if(cfg->filter_outs){
		for(i=0; i<cfg->filter_out_count; i++){
			mosquitto_topic_matches_sub(cfg->filter_outs[i], message->topic, &res);
			if(res) return;
		}
	}

	print_message(cfg, message);

	if(cfg->msg_count>0){
		msg_count++;
		if(cfg->msg_count == msg_count){
			process_messages = false;
			mosquitto_disconnect(mosq);
		}
	}
}

void my_connect_callback(struct mosquitto *mosq, void *obj, int result, int flags)
{
	int i;
	struct mosq_config *cfg;

	assert(obj);
	cfg = (struct mosq_config *)obj;

	if(!result){
		for(i=0; i<cfg->topic_count; i++){
			mosquitto_subscribe(mosq, NULL, cfg->topics[i], cfg->qos);
		}
		for(i=0; i<cfg->unsub_topic_count; i++){
			mosquitto_unsubscribe(mosq, NULL, cfg->unsub_topics[i]);
		}
	}else{
		if(result && !cfg->quiet){
			fprintf(stderr, "%s\n", mosquitto_connack_string(result));
		}
		mosquitto_disconnect(mosq);
	}
}

void my_subscribe_callback(struct mosquitto *mosq, void *obj, int mid, int qos_count, const int *granted_qos)
{
	int i;
	struct mosq_config *cfg;

	assert(obj);
	cfg = (struct mosq_config *)obj;

	if(!cfg->quiet) printf("Subscribed (mid: %d): %d", mid, granted_qos[0]);
	for(i=1; i<qos_count; i++){
		if(!cfg->quiet) printf(", %d", granted_qos[i]);
	}
	if(!cfg->quiet) printf("\n");
}

void my_log_callback(struct mosquitto *mosq, void *obj, int level, const char *str)
{
	printf("%s\n", str);
}

void print_usage(void)
{
	int major, minor, revision;

	mosquitto_lib_version(&major, &minor, &revision);
	printf("mosquitto_sub is a simple mqtt client that will subscribe to a set of topics and print all messages it receives.\n");
	printf("mosquitto_sub version %s running on libmosquitto %d.%d.%d.\n\n", VERSION, major, minor, revision);
	printf("Usage: mosquitto_sub {[-h host] [-p port] [-u username [-P password]] -t topic | -L URL [-t topic]}\n");
	printf("                     [-c] [-k keepalive] [-q qos]\n");
	printf("                     [-C msg_count] [-R] [--retained-only] [-T filter_out] [-U topic ...]\n");
	printf("                     [-F format]\n");
#ifndef WIN32
	printf("                     [-W timeout_secs]\n");
#endif
#ifdef WITH_SRV
	printf("                     [-A bind_address] [-S]\n");
#else
	printf("                     [-A bind_address]\n");
#endif
	printf("                     [-i id] [-I id_prefix]\n");
	printf("                     [-d] [-N] [--quiet] [-v]\n");
	printf("                     [--fmask outfile [--overwrite]]\n");
	printf("                     [--will-topic [--will-payload payload] [--will-qos qos] [--will-retain]]\n");
#ifdef WITH_TLS
	printf("                     [{--cafile file | --capath dir} [--cert file] [--key file]\n");
	printf("                      [--ciphers ciphers] [--insecure]]\n");
#ifdef FINAL_WITH_TLS_PSK
	printf("                     [--psk hex-key --psk-identity identity [--ciphers ciphers]]\n");
#endif
#endif
#ifdef WITH_SOCKS
	printf("                     [--proxy socks-url]\n");
#endif
	printf("       mosquitto_sub --help\n\n");
	printf(" -A : bind the outgoing socket to this host/ip address. Use to control which interface\n");
	printf("      the client communicates over.\n");
	printf(" -c : disable 'clean session' (store subscription and pending messages when client disconnects).\n");
	printf(" -C : disconnect and exit after receiving the 'msg_count' messages.\n");
	printf(" -d : enable debug messages.\n");
	printf(" -F : output format.\n");
	printf(" -h : mqtt host to connect to. Defaults to localhost.\n");
	printf(" -i : id to use for this client. Defaults to mosquitto_sub_ appended with the process id.\n");
	printf(" -I : define the client id as id_prefix appended with the process id. Useful for when the\n");
	printf("      broker is using the clientid_prefixes option.\n");
	printf(" -k : keep alive in seconds for this client. Defaults to 60.\n");
	printf(" -L : specify user, password, hostname, port and topic as a URL in the form:\n");
	printf("      mqtt(s)://[username[:password]@]host[:port]/topic\n");
	printf(" -N : do not add an end of line character when printing the payload.\n");
	printf(" -p : network port to connect to. Defaults to 1883 for plain MQTT and 8883 for MQTT over TLS.\n");
	printf(" -P : provide a password\n");
	printf(" -q : quality of service level to use for the subscription. Defaults to 0.\n");
	printf(" -R : do not print stale messages (those with retain set).\n");
#ifdef WITH_SRV
	printf(" -S : use SRV lookups to determine which host to connect to.\n");
#endif
	printf(" -t : mqtt topic to subscribe to. May be repeated multiple times.\n");
	printf(" -T : topic string to filter out of results. May be repeated.\n");
	printf(" -u : provide a username\n");
	printf(" -U : unsubscribe from a topic. May be repeated.\n");
	printf(" -v : print published messages verbosely.\n");
	printf(" -V : specify the version of the MQTT protocol to use when connecting.\n");
	printf("      Can be mqttv31 or mqttv311. Defaults to mqttv311.\n");
#ifndef WIN32
	printf(" -W : Specifies a timeout in seconds how long to process incoming MQTT messages.\n");
#endif
	printf(" --help : display this message.\n");
	printf(" --quiet : don't print error messages.\n");
	printf(" --retained-only : only handle messages with the retained flag set, and exit when the\n");
	printf("                   first non-retained message is received.\n");
	printf(" --fmask : path to message outfile\n");
	printf("            allowed masks are:\n");
	printf("            @[epoch|date|year|month|day|datetime|hour|min|sec|id|topic[1-9]] \n");
	printf("            eg. --fmask='@id@-@date@-@topic' for file id-20101221-topicname\n");
	printf("            NOTE: option -F (new in v1.5.0) does have any effect when used with --fmask\n");
	printf(" --nodesuffix : suffix for leaf/text node, when --fmask is provided\n");
	printf(" --overwrite : overwrite the existing output file, can be used with --fmask only.\n");
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
	printf(" --ciphers : openssl compatible list of TLS ciphers to support.\n");
	printf(" --tls-version : TLS protocol version, can be one of tlsv1.2 tlsv1.1 or tlsv1.\n");
	printf("                 Defaults to tlsv1.2 if available.\n");
	printf(" --insecure : do not check that the server certificate hostname matches the remote\n");
	printf("              hostname. Using this option means that you cannot be sure that the\n");
	printf("              remote host is the server you wish to connect to and so is insecure.\n");
	printf("              Do not use this option in a production environment.\n");
#ifdef FINAL_WITH_TLS_PSK
	printf(" --psk : pre-shared-key in hexadecimal (no leading 0x) to enable TLS-PSK mode.\n");
	printf(" --psk-identity : client identity string for TLS-PSK mode.\n");
#endif
#endif
#ifdef WITH_SOCKS
	printf(" --proxy : SOCKS5 proxy URL of the form:\n");
	printf("           socks5h://[username[:password]@]hostname[:port]\n");
	printf("           Only \"none\" and \"username\" authentication is supported.\n");
#endif
	printf("\nSee http://mosquitto.org/ for more information.\n\n");
}

int main(int argc, char *argv[])
{
	struct mosq_config cfg;
	int rc;
#ifndef WIN32
		struct sigaction sigact;
#endif
	
	memset(&cfg, 0, sizeof(struct mosq_config));

	rc = client_config_load(&cfg, CLIENT_SUB, argc, argv);
	if(rc){
		client_config_cleanup(&cfg);
		if(rc == 2){
			/* --help */
			print_usage();
		}else{
			fprintf(stderr, "\nUse 'mosquitto_sub --help' to see usage.\n");
		}
		return 1;
	}

	if(cfg.no_retain && cfg.retained_only){
		fprintf(stderr, "\nError: Combining '-R' and '--retained-only' makes no sense.\n");
		return 1;
	}

	mosquitto_lib_init();

	if(client_id_generate(&cfg, "mosqsub")){
		return 1;
	}

	mosq = mosquitto_new(cfg.id, cfg.clean_session, &cfg);
	cfg.idtext = cfg.id;
	if(!mosq){
		switch(errno){
			case ENOMEM:
				if(!cfg.quiet) fprintf(stderr, "Error: Out of memory.\n");
				break;
			case EINVAL:
				if(!cfg.quiet) fprintf(stderr, "Error: Invalid id and/or clean_session.\n");
				break;
		}
		mosquitto_lib_cleanup();
		return 1;
	}
	if(client_opts_set(mosq, &cfg)){
		return 1;
	}
	if(cfg.debug){
		mosquitto_log_callback_set(mosq, my_log_callback);
		mosquitto_subscribe_callback_set(mosq, my_subscribe_callback);
	}
	mosquitto_connect_with_flags_callback_set(mosq, my_connect_callback);

	if(cfg.isfmask) {
		mosquitto_message_callback_set(mosq, my_message_file_callback);
	} else {
		mosquitto_message_callback_set(mosq, my_message_callback);
	}

	rc = client_connect(mosq, &cfg);
	if(rc) return rc;

#ifndef WIN32
	sigact.sa_handler = my_signal_handler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;

	if(sigaction(SIGALRM, &sigact, NULL) == -1){
		perror("sigaction");
		return 1;
	}

	if(cfg.timeout){
		alarm(cfg.timeout);
	}
#endif

	rc = mosquitto_loop_forever(mosq, -1, 1);

	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();

	if(cfg.msg_count>0 && rc == MOSQ_ERR_NO_CONN){
		rc = 0;
	}
	if(rc){
		fprintf(stderr, "Error: %s\n", mosquitto_strerror(rc));
	}
	return rc;
}

