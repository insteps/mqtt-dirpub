/*
Copyright (c) 2009-2019 Roger Light <roger@atchoo.org>
Copyright (c) 2015-2019 V.Krishn <vkrishn@insteps.net>

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
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif /* HAVE_UNISTD_H */
#include <signal.h>
#else
#include <sysstat.h>    /* Fix up for Windows - inc mode_t */
#include <process.h>
#include <winsock2.h>
#define snprintf sprintf_s
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>

#ifdef __APPLE__
#  include <sys/time.h>
#endif

#include <mosquitto.h>
#include "client_shared.h"

extern struct mosq_config cfg;

static int get_time(struct tm **ti, long *ns)
{
#ifdef WIN32
	SYSTEMTIME st;
#elif defined(__APPLE__)
	struct timeval tv;
#else
	struct timespec ts;
#endif
	time_t s;

#ifdef WIN32
	s = time(NULL);

	GetLocalTime(&st);
	*ns = st.wMilliseconds*1000000L;
#elif defined(__APPLE__)
	gettimeofday(&tv, NULL);
	s = tv.tv_sec;
	*ns = tv.tv_usec*1000;
#else
	if(clock_gettime(CLOCK_REALTIME, &ts) != 0){
		err_printf(&cfg, "Error obtaining system time.\n");
		return 1;
	}
	s = ts.tv_sec;
	*ns = ts.tv_nsec;
#endif

	*ti = localtime(&s);
	if(!(*ti)){
		err_printf(&cfg, "Error obtaining system time.\n");
		return 1;
	}

	return 0;
}


static void write_payload(const unsigned char *payload, int payloadlen, int hex)
{
	int i;

	if(hex == 0){
		(void)fwrite(payload, 1, payloadlen, stdout);
	}else if(hex == 1){
		for(i=0; i<payloadlen; i++){
			fprintf(stdout, "%02x", payload[i]);
		}
	}else if(hex == 2){
		for(i=0; i<payloadlen; i++){
			fprintf(stdout, "%02X", payload[i]);
		}
	}
}


static void write_json_payload(const char *payload, int payloadlen)
{
	int i;

	for(i=0; i<payloadlen; i++){
		if(payload[i] == '"' || payload[i] == '\\' || (payload[i] >=0 && payload[i] < 32)){
			printf("\\u%04x", payload[i]);
		}else{
			fputc(payload[i], stdout);
		}
	}
}


static void json_print(const struct mosquitto_message *message, const struct tm *ti, bool escaped)
{
	char buf[100];

	strftime(buf, 100, "%s", ti);
	printf("{\"tst\":%s,\"topic\":\"%s\",\"qos\":%d,\"retain\":%d,\"payloadlen\":%d,", buf, message->topic, message->qos, message->retain, message->payloadlen);
	if(message->qos > 0){
		printf("\"mid\":%d,", message->mid);
	}
	if(escaped){
		fputs("\"payload\":\"", stdout);
		write_json_payload(message->payload, message->payloadlen);
		fputs("\"}", stdout);
	}else{
		fputs("\"payload\":", stdout);
		write_payload(message->payload, message->payloadlen, 0);
		fputs("}", stdout);
	}
}


static void formatted_print(const struct mosq_config *lcfg, const struct mosquitto_message *message)
{
	int len;
	int i;
	struct tm *ti = NULL;
	long ns;
	char strf[3];
	char buf[100];

	len = strlen(lcfg->format);

	for(i=0; i<len; i++){
		if(lcfg->format[i] == '%'){
			if(i < len-1){
				i++;
				switch(lcfg->format[i]){
					case '%':
						fputc('%', stdout);
						break;

					case 'I':
						if(!ti){
							if(get_time(&ti, &ns)){
								err_printf(lcfg, "Error obtaining system time.\n");
								return;
							}
						}
						if(strftime(buf, 100, "%FT%T%z", ti) != 0){
							fputs(buf, stdout);
						}
						break;

					case 'j':
						if(!ti){
							if(get_time(&ti, &ns)){
								err_printf(lcfg, "Error obtaining system time.\n");
								return;
							}
						}
						json_print(message, ti, true);
						break;

					case 'J':
						if(!ti){
							if(get_time(&ti, &ns)){
								err_printf(lcfg, "Error obtaining system time.\n");
								return;
							}
						}
						json_print(message, ti, false);
						break;

					case 'l':
						printf("%d", message->payloadlen);
						break;

					case 'm':
						printf("%d", message->mid);
						break;

					case 'p':
						write_payload(message->payload, message->payloadlen, 0);
						break;

					case 'q':
						fputc(message->qos + 48, stdout);
						break;

					case 'r':
						if(message->retain){
							fputc('1', stdout);
						}else{
							fputc('0', stdout);
						}
						break;

					case 't':
						fputs(message->topic, stdout);
						break;

					case 'U':
						if(!ti){
							if(get_time(&ti, &ns)){
								err_printf(lcfg, "Error obtaining system time.\n");
								return;
							}
						}
						if(strftime(buf, 100, "%s", ti) != 0){
							printf("%s.%09ld", buf, ns);
						}
						break;

					case 'x':
						write_payload(message->payload, message->payloadlen, 1);
						break;

					case 'X':
						write_payload(message->payload, message->payloadlen, 2);
						break;
				}
			}
		}else if(lcfg->format[i] == '@'){
			if(i < len-1){
				i++;
				if(lcfg->format[i] == '@'){
					fputc('@', stdout);
				}else{
					if(!ti){
						if(get_time(&ti, &ns)){
							err_printf(lcfg, "Error obtaining system time.\n");
							return;
						}
					}

					strf[0] = '%';
					strf[1] = lcfg->format[i];
					strf[2] = 0;

					if(lcfg->format[i] == 'N'){
						printf("%09ld", ns);
					}else{
						if(strftime(buf, 100, strf, ti) != 0){
							fputs(buf, stdout);
						}
					}
				}
			}
		}else if(lcfg->format[i] == '\\'){
			if(i < len-1){
				i++;
				switch(lcfg->format[i]){
					case '\\':
						fputc('\\', stdout);
						break;

					case '0':
						fputc('\0', stdout);
						break;

					case 'a':
						fputc('\a', stdout);
						break;

					case 'e':
						fputc('\033', stdout);
						break;

					case 'n':
						fputc('\n', stdout);
						break;

					case 'r':
						fputc('\r', stdout);
						break;

					case 't':
						fputc('\t', stdout);
						break;

					case 'v':
						fputc('\v', stdout);
						break;
				}
			}
		}else{
			fputc(lcfg->format[i], stdout);
		}
	}
	if(lcfg->eol){
		fputc('\n', stdout);
	}
	fflush(stdout);
}


void print_message(struct mosq_config *cfg, const struct mosquitto_message *message)
{
	if(cfg->format){
		formatted_print(cfg, message);
	}else if(cfg->verbose){
		if(message->payloadlen){
			printf("%s ", message->topic);
			write_payload(message->payload, message->payloadlen, false);
			if(cfg->eol){
				printf("\n");
			}
		}else{
			if(cfg->eol){
				printf("%s (null)\n", message->topic);
			}
		}
		fflush(stdout);
	}else{
		if(message->payloadlen){
			write_payload(message->payload, message->payloadlen, false);
			if(cfg->eol){
				printf("\n");
			}
			fflush(stdout);
		}
	}
}

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

/**
** mkpath - ensure all directories in path exist
** Algorithm takes the pessimistic view and works top-down to ensure
** each directory in path exists, rather than optimistically creating
** the last element and working backwards.
*/
static int mkpath(const char *path, mode_t mode)
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

/* Expand --fmask string options for output filename.
   DateTime string expansion for --fmask
*/
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

static const char *datetime(int fmt)
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
			n = snprintf(dt, size, "%02d", (int)current);
			break;
		case FMASK_DATE:
			n = snprintf(dt, size, "%02d%02d%02d",
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
			n = snprintf(dt, size, "%02d%02d%02d.%02d%02d%02d",
				now->tm_year+1900, now->tm_mon+1, now->tm_mday,
				now->tm_hour, now->tm_min, now->tm_sec);
			break;
		case FMASK_TIME:
			n = snprintf(dt, size, "%02d%02d%02d",
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

/* Expand/resolve fmask token string. */
/* ------------------------------------------------------------- */
static void _setfmask(char *token, void *obj)
{
	struct mosq_config *cfg;

	assert(obj);
	cfg = (struct mosq_config *)obj;

	char *str2, *subtoken;
	char *saveptr2;

	char *to = cfg->ftoken;      /* limit 1000 bytes. */
	const char *dt;

	for (str2 = token; ; str2 = NULL) {
		subtoken = strtok_r(str2, "@", &saveptr2);
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
			dt = cfg->fmask_topic;
		} else if(!strcmp(subtoken, "topic1")) {
			dt = cfg->topics[0];
		} else if(!strcmp(subtoken, "topic2")) {
			dt = cfg->topics[1];
		} else if(!strcmp(subtoken, "topic3")) {
			dt = cfg->topics[2];
		} else if(!strcmp(subtoken, "topic4")) {
			dt = cfg->topics[3];
		} else if(!strcmp(subtoken, "topic5")) {
			dt = cfg->topics[4];
		} else if(!strcmp(subtoken, "topic6")) {
			dt = cfg->topics[5];
		} else if(!strcmp(subtoken, "topic7")) {
			dt = cfg->topics[6];
		} else if(!strcmp(subtoken, "topic8")) {
			dt = cfg->topics[7];
		} else if(!strcmp(subtoken, "topic9")) {
			dt = cfg->topics[8];
		} else if(!strcmp(subtoken, "id")) {
			dt = cfg->idtext;
		} else {
			dt = strdup(subtoken);
		}

		to = stpcpy(to, dt);
	}
}

/* Expand --fmask string options for output filename. */
/* ------------------------------------------------------------- */
static void _fmask(char *fmask, void *obj, const struct mosquitto_message *message)
{
	struct mosq_config *cfg;

	assert(obj);
	cfg = (struct mosq_config *)obj;

	char *str1, *token;
	char *saveptr1;

	char *path;
	path = strdup(fmask);
	char *to = cfg->ffmask;      /* limit 1000 bytes. */
	if(cfg->verbose == 1) {
		printf("%s\t", path); /* if verbose (-v) is enabled */
	}

	to = stpcpy(to, "/");  /* make sure path starts with a slash */
	if(cfg->format == NULL && strlen(cfg->fmask) >= 1) {
		for (str1 = path; ; str1 = NULL) {
			token = strtok_r(str1, "/", &saveptr1);
			if (token == NULL)
				break;

			/* format type */
			_setfmask(token, cfg);
			to = stpcpy(to, cfg->ftoken);
			to = stpcpy(to, "/");
		}
	} else { /* experimental */
		char buf[1000] = { 0 };
		fclose(stdout);
		stdout = fmemopen(buf, sizeof(buf), "w");
		setbuf(stdout, NULL);
		formatted_print(cfg, message);
		to = stpcpy(to, buf);
		int fd;
		fd = open("/dev/tty",  O_WRONLY);
		stdout = fdopen(fd, "w");    
	}

	to[strlen(to)-1] = '\0';
	if(cfg->verbose == 1) {
		printf("%s\n", cfg->ffmask); /* if verbose (-v) is enabled */
	}

}

/*
File open with given mode.
returns file descriptor (fd)
*/
/* ------------------------------------------------------------- */
static FILE *_mosquitto_fopen(const char *path, const char *mode)
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

void print_message_file(struct mosq_config *cfg, const struct mosquitto_message *message)
{

	cfg->fmask_topic = message->topic;

	FILE *fptr = NULL;

	if(cfg->format && strlen(cfg->fmask) == 0) {
		_fmask(cfg->format, cfg, message); /* experimental */
	} else {
		_fmask(cfg->fmask, cfg, message);
	}

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
    
}
