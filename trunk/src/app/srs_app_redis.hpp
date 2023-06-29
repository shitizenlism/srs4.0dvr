//
// Copyright (c) 2013-2021 Freeman
//
// SPDX-License-Identifier: MIT
//

#ifndef SRS_APP_REDIS_HPP
#define SRS_APP_REDIS_HPP
#include <hiredis/hiredis.h>

#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <string>

#ifdef __cplusplus
       extern "C" {
#endif
	
	typedef struct{
		redisContext *hdl;
		pthread_mutex_t ThrdLock;
		int db;
	}RedisConn_T;

	int redis_Close();
	int redis_Init(char *hostname,int port,char *authpass,int db);
	int redis_flushdb();
	int redis_queue_rpush(const char *key, char *value);
	int redis_queue_lpop(const char *key, char *value);
	int redis_queue_flush(const char *key);
	int redis_hset(const char *key, const char *field, char *value);
	int redis_hget(const char *key, const char *field, char *value);	
	int redis_append(const char *key, const char *value);
	int redis_setex(const char *key, const char *value, int exptime=3600);
	char *redis_get(const char *key);
	int redis_del(const char *key);

#ifdef __cplusplus
}
#endif 

#endif
