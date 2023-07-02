//
// Copyright (c) 2013-2021 Freeman
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_redis.hpp>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
using namespace std;

static RedisConn_T gRedisConn;
static int gRedisQueueSize=0;

enum connection_type{
	CONN_TCP,
	CONN_UNIX,
	CONN_FD
};

typedef struct config{
	enum connection_type type;
	struct {
		const char *host;
		int port;
		struct timeval timeout;
	}tcp;
	struct {
		const char *path;
	}unix_sock;

}RedisCfg_T;

static long long  usec(){
	struct timeval tv;
	gettimeofday(&tv,NULL);
	return ((long long)tv.tv_sec*1000000) + tv.tv_usec;
}

static redisContext *_select_database(redisContext *c)
{
    redisReply *reply;

    reply = (redisReply *)redisCommand(c,"SELECT 9");
    freeReplyObject(reply);

    reply = (redisReply *)redisCommand(c,"DBSIZE");
    if (reply->type == REDIS_REPLY_INTEGER && reply->integer == 0) {
        freeReplyObject(reply);
    } else {
	return NULL;
    }

    return c;
}

static int _disconnect(redisContext *c, int keep_fd)
{
    if (keep_fd)
        return redisFreeKeepFd(c);
    redisFree(c);
    return -1;
}

static redisContext *_connect(struct config config)
{
    redisContext *c = NULL;

    if (config.type == CONN_TCP) {
        c = redisConnect(config.tcp.host, config.tcp.port);
    } else if (config.type == CONN_UNIX) {
        c = redisConnectUnix(config.unix_sock.path);
    } else if (config.type == CONN_FD) {

        redisContext *dummy_ctx = redisConnectUnix(config.unix_sock.path);
        if (dummy_ctx) {
            int fd = _disconnect(dummy_ctx, 1);
            printf("Connecting to inherited fd %d\n", fd);
            c = redisConnectFd(fd);
        }
    }

    if (c == NULL) {
        printf("Connection error: can't allocate redis context\n");
        return NULL;
    } else if (c->err) {
        printf("Connection error: %s\n", c->errstr);
        redisFree(c);
        return NULL;
    }

    return _select_database(c);
}

int redis_close()
{
	if (gRedisConn.hdl){
		redisFree(gRedisConn.hdl); gRedisConn.hdl=NULL;
		pthread_mutex_destroy(&gRedisConn.ThrdLock);
	}
	return 0;
}


int redis_init(const char *hostname,int port,const char *authpass,int db)
{
    int ret=0;
    redisContext *hdl=NULL;
    redisReply *reply=NULL;
    struct timeval timeout={1,500000};

    if (strlen(hostname)<2 || port<1000){
        return -1;
    }

    if (gRedisConn.hdl){
		return 0;
    }

	do {
		hdl=redisConnectWithTimeout(hostname,port,timeout);
		if (!hdl){
			printf("redis connect timeout!\n");
			ret=-2;break;
		}
		if (hdl&&(hdl->err)){
			printf("redis connect error[%d]:%s\n", hdl->err,hdl->errstr);
			ret=-3;break;
		}
		if ((authpass)&&(strlen(authpass)>1)){
			reply=(redisReply*)redisCommand(hdl,"AUTH %s", authpass);
			if (!reply){
				ret=-4;break;
			}
			if (reply->type == REDIS_REPLY_ERROR){
				printf("AUTH error:%s\n", reply->str);
				freeReplyObject(reply);
				ret=-5;break;
			}
			freeReplyObject(reply);
		}

		reply=(redisReply*)redisCommand(hdl,"select %d", db);
		freeReplyObject(reply);

		reply=(redisReply*)redisCommand(hdl,"flushdb");
		freeReplyObject(reply);
	}while(0);

	if (ret<0){
		if (hdl)
			redisFree(hdl);
	}
	else{
		pthread_mutex_init(&gRedisConn.ThrdLock,NULL);
		gRedisConn.hdl=hdl;
		gRedisConn.db=db;
	}

	return ret;
}

int redis_flushdb()
{
	redisReply *reply=NULL;
	redisContext *hdl=gRedisConn.hdl;

	if (hdl){
		pthread_mutex_lock(&gRedisConn.ThrdLock);
		reply=(redisReply*)redisCommand(hdl,"flushdb");
		freeReplyObject(reply);
		pthread_mutex_unlock(&gRedisConn.ThrdLock);
	}
	return 0;
}

int redis_queue_rpush(const char *key, char *value)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;

	if ((!key)||(!value)||(!gRedisConn.hdl))
		return -1;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	if (gRedisQueueSize>=2000){
		pthread_mutex_unlock(&gRedisConn.ThrdLock);
		return -2;
	}

	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"rpush %s %s",key,value);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			if (reply->type == REDIS_REPLY_INTEGER){
				ret=reply->integer;
				gRedisQueueSize++;
			}
			else
				ret=-4;
		}
		freeReplyObject(reply);
	}else{
		ret=-5;
	}
	t2 = usec();
	printf("rpush cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);
	return ret;
}

int redis_queue_lpop(const char *key, char *value)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;

	if ((!key)||(!value)||(!gRedisConn.hdl))
		return -1;
	pthread_mutex_lock(&gRedisConn.ThrdLock);
	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"lpop %s",key);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			if (!reply->str)
				ret=-4;
			else{
				strcpy(value,reply->str);
				ret=1;
				gRedisQueueSize--;
			}
		}
		freeReplyObject(reply);
	}else{
		ret=-2;
	}
	t2 = usec();
	printf("lpop cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);
	return ret;
}

int redis_queue_flush(const char *key)
{
	redisReply *reply=NULL;

	if ((!gRedisConn.hdl))
		return -1;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	while(1)
	{
		reply=(redisReply*)redisCommand(gRedisConn.hdl,"lpop %s",key);
		if (reply){
			if (reply->type == REDIS_REPLY_ERROR){
				freeReplyObject(reply);
				break;
			}
			else{
				if (!reply->str){
					freeReplyObject(reply);
					break;
				}
			}
			freeReplyObject(reply);
		}else{
			break;
		}
	}
	gRedisQueueSize=0;
	pthread_mutex_unlock(&gRedisConn.ThrdLock);

	return 0;
}

int redis_hset(const char *key, const char *field, char *value)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;

	if ((!key)||(!field)||(!value)||(!gRedisConn.hdl))
		return -1;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"hset %s %s %s",key,field,value);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			if (reply->type == REDIS_REPLY_INTEGER)
				ret=reply->integer;
			else
				ret=-4;
		}
		freeReplyObject(reply);
	}else{
		ret=-2;
	}
	t2 = usec();
	printf("hset cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);

	return ret;
}

int redis_hget(const char *key, const char *field, char *value)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;

	if ((!key)||(!field)||(!value)||(!gRedisConn.hdl))
		return -1;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"hget %s %s",key,field);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			if (!reply->str)
				ret=-4;
			else{
				strcpy(value,reply->str);
				ret=1;
			}
		}
		freeReplyObject(reply);
	}else{
		ret=-2;
	}
	t2 = usec();
	printf("hget cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);

	return ret;
}

int redis_append(const char *key, const char *value)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;

	if ((!key)||(!value)||(!gRedisConn.hdl))
		return -1;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"append %s %s,",key,value);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			if (reply->type == REDIS_REPLY_INTEGER)
				ret=reply->integer;
			else
				ret=-4;
		}
		freeReplyObject(reply);
	}else{
		ret=-2;
	}
	t2 = usec();
	printf("append cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);

	return ret;
}

int redis_setex(const char *key, const char *value, int exptime)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;

	if ((!key)||(!value)||(!gRedisConn.hdl))
		return -1;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"setex %s %d %s",key,exptime,value);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			ret=1;
		}
		freeReplyObject(reply);
	}else{
		ret=-2;
	}
	t2 = usec();
	printf("setex cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);

	return ret;
}

char *redis_get(const char *key)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;
	char *pVal=NULL;

	if ((!key)||(!gRedisConn.hdl))
		return NULL;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"get %s",key);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			if (!reply->str)
				ret=-4;
			else{
				ret=strlen(reply->str);
				pVal=(char *)malloc(ret+1);
				if (pVal)
					strcpy(pVal,reply->str);
				else
					ret=-5;
			}
		}
		freeReplyObject(reply);
	}else{
		ret=-2;
	}
	t2 = usec();
	printf("get cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);

	return pVal;
}

int redis_del(const char *key)
{
	int ret=-1;
	redisReply *reply=NULL;
	long long t1,t2;

	if ((!key)||(!gRedisConn.hdl))
		return -1;

	pthread_mutex_lock(&gRedisConn.ThrdLock);
	t1=usec();
	reply=(redisReply*)redisCommand(gRedisConn.hdl,"del %s",key);
	if (reply){
		if (reply->type == REDIS_REPLY_ERROR){
			ret=-3;
		}
		else{
			ret=reply->integer;
		}
		freeReplyObject(reply);
	}else{
		ret=-2;
	}
	t2 = usec();
	printf("del cost=%.3f ms\n",(t2-t1)/1000.0);
	pthread_mutex_unlock(&gRedisConn.ThrdLock);

	return ret;
}


