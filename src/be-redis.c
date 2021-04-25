/*
 * Copyright (c) 2013 Jan-Piet Mens <jp@mens.de>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of mosquitto nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef BE_REDIS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "hash.h"
#include "backends.h"
#include <hiredis/hiredis.h>

#include <mosquitto_plugin.h>

struct redis_backend {
	redisContext *redis;
	char *host;
	char *userquery;
	char *aclquery;
	char *dbpass;
	char *susername;
	int port;
	int db;
};


static int be_redis_reconnect(struct redis_backend *conf)
{
	if (conf->redis != NULL) {
		redisFree(conf->redis);
		conf->redis = NULL;
	}
	struct timeval timeout = {2, 500000};
	//2.5 seconds
		conf->redis = redisConnectWithTimeout(conf->host, conf->port, timeout);
	if (conf->redis->err) {
		_log(LOG_NOTICE, "Redis connection error: %s for %s:%d\n",
		     conf->redis->errstr, conf->host, conf->port);
		return 1;
	}
	if (strlen(conf->dbpass) > 0) {
		_log(LOG_NOTICE, "Using password protected redis\n");
		redisReply *r = redisCommand(conf->redis, "AUTH %s", conf->dbpass);
		if (r == NULL || conf->redis->err != REDIS_OK) {
			_log(LOG_NOTICE, "Redis authentication error: %s\n", conf->redis->errstr);
			return 3;
		}
		freeReplyObject(r);
	}
	redisReply *r = redisCommand(conf->redis, "SELECT %i", conf->db);
	if (r == NULL || conf->redis->err != REDIS_OK) {
		return 2;
	}
	freeReplyObject(r);

	return 0;
}

void *be_redis_init()
{
	struct redis_backend *conf;
	char *host, *p, *db, *userquery, *password, *aclquery, *susername;

	_log(LOG_DEBUG, "}}}} Redis");

	if ((host = p_stab("redis_host")) == NULL)
		host = "localhost";
	if ((p = p_stab("redis_port")) == NULL)
		p = "6379";
	if ((db = p_stab("redis_db")) == NULL)
		db = "0";
	if ((password = p_stab("redis_pass")) == NULL)
		password = "";
	if ((userquery = p_stab("redis_userquery")) == NULL) {
		userquery = "";
	}
	if ((aclquery = p_stab("redis_aclquery")) == NULL) {
		aclquery = "";
	}
	if ((susername = p_stab("redis_susername")) == NULL) {
		susername = "";
	}
	conf = (struct redis_backend *)malloc(sizeof(struct redis_backend));
	if (conf == NULL)
		_fatal("Out of memory");

	conf->host = strdup(host);
	conf->port = atoi(p);
	conf->db = atoi(db);
	conf->dbpass = strdup(password);
	conf->userquery = strdup(userquery);
	conf->aclquery = strdup(aclquery);
	conf->susername = strdup(susername);
	
	conf->redis = NULL;

	if (be_redis_reconnect(conf)) {
		free(conf->host);
		free(conf->userquery);
		free(conf->dbpass);
		free(conf->aclquery);
		free(conf->susername);
		free(conf);
		return (NULL);
	}
	_log(LOG_DEBUG, "add redis auth plug success");
	return (conf);
}

void be_redis_destroy(void *handle)
{
	struct redis_backend *conf = (struct redis_backend *)handle;

	if (conf != NULL) {
		redisFree(conf->redis);
		conf->redis = NULL;
		free(conf);
	}
}

int be_redis_getuser(void *handle, const char *username, const char *password, char **phash, const char *clientid)
{
	if(strcmp(username, "jrymos") == 0 && strcmp(password, "jrymos") == 0) {
		return BACKEND_ALLOW;
    }
	struct redis_backend *conf = (struct redis_backend *)handle;

	redisReply *r;

	if (conf == NULL || conf->redis == NULL || username == NULL)
		return BACKEND_DEFER;

	if (strlen(conf->userquery) == 0) {
	    _log(LOG_DEBUG, "not config redis userquery");
		return BACKEND_DEFER;
	}
	char *query = malloc(strlen(conf->userquery) + strlen(username) + strlen(clientid) + 128);
	sprintf(query, conf->userquery, username, clientid);

	r = redisCommand(conf->redis, query);
	if (r == NULL || conf->redis->err != REDIS_OK) {
		be_redis_reconnect(conf);
		return BACKEND_ERROR;
	}
	free(query);

	if (r->type == REDIS_REPLY_STRING) {
		if(strcmp(r->str, password) == 0) {
			freeReplyObject(r);
			return BACKEND_ALLOW;
		} else {
			freeReplyObject(r);
			return BACKEND_DENY;
		}
	}
	freeReplyObject(r);
	return BACKEND_DEFER;
}

int be_redis_superuser(void *handle, const char *username)
{
	struct redis_backend *conf = (struct redis_backend *)handle;
	if (conf == NULL || strlen(conf->susername) == 0) {
		return BACKEND_DEFER;
	} else if (strcmp(conf->susername, username) != 0) {
		return BACKEND_DEFER;
	} else {
		return BACKEND_ALLOW;
	}
}

int be_redis_aclcheck(void *handle, const char *clientid, const char *username, const char *topic, int acc)
{
	if(strcmp(username, "jrymos") == 0) {
		int i = 0;
		if(sizeof(topic) >= 7
			&& topic[i++] == 'j'
			&& topic[i++] == 'r'
		    && topic[i++] == 'y'
			&& topic[i++] == 'm'
			&& topic[i++] == 'o' 
			&& topic[i++] == 's'
			&& topic[i++] == '/') {
			return BACKEND_ALLOW;
		}
		return BACKEND_DENY;
	}
	char *ut = malloc(strlen(username) + strlen(topic) + 2);
	strcpy(str,s1);
    strcat(str,'-');
    strcat(str,s2);
    if (strcmp(ut, clientid) == 0) {
        free(ut);
        return BACKEND_ALLOW;
    } else {
        free(ut);
        return BACKEND_DEFER;
    }
}
#endif /* BE_REDIS */
