/*
 * Copyright(C) 2020 Ruijie Network. All rights reserved.
 */

/*!
* \file xxx.x
* \brief xxx
* 
* 包含..
*
* \copyright 2020 Ruijie Network. All rights reserved.
* \author hongchunhua@ruijie.com.cn
* \version v1.0.0
* \date 2020.08.05
* \note none 
*/

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sched.h>
#include<fcntl.h>
#include <sys/prctl.h>

#include "arpc_session.h"

#define WAIT_THREAD_RUNING_TIMEOUT (1000)
#define ARPC_SESSION_RECONNECT_FAIL_MAX_TIME (2)
#define ARPC_SESSION_RECONNECT_WAIT_TIME_S   (10)
#define ARPC_SESSION_BUSY_WAIT_TIME_MS		(50)
#define ARPC_SESSION_BUSY_RETRY_CNT			(3)

static int session_client_connect(struct arpc_session_handle *session, int64_t timeout_ms);

struct arpc_session_handle *arpc_create_session(enum arpc_session_type type, uint32_t ex_ctx_size)
{
	int ret = 0;
	int i =0;
	struct arpc_session_handle *session = NULL;
	/* handle*/
	session = (struct arpc_session_handle *)arpc_mem_alloc(sizeof(struct arpc_session_handle) + ex_ctx_size, NULL);
	if (!session) {
		ARPC_LOG_ERROR( "malloc error, exit ");
		return NULL;
	}
	memset(session, 0, sizeof(struct arpc_session_handle) + ex_ctx_size);

	ret = arpc_cond_init(&session->cond); /* 初始化信号锁 */
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_buf, "arpc_cond_init fail.");

	ret = arpc_cond_init(&session->deamon_cond); /* 初始化信号锁 */
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_cond, "arpc_cond_init fail.");

	ret = arpc_mutex_init(&session->lock); /* 初始化信号锁 */
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_deamon, "arpc_mutex_init fail.");

	QUEUE_INIT(&session->q);
	QUEUE_INIT(&session->q_con);
	session->threadpool = NULL;
	session->type = type;
	session->is_close = 0;
	session->status = ARPC_SES_STA_INIT;
	session->conn_timeout_ms = XIO_INFINITE;
	session->conn_num = 0;
	return session;
free_deamon:
	arpc_cond_destroy(&session->deamon_cond);
free_cond:
	arpc_cond_destroy(&session->cond);
free_buf:
	SAFE_FREE_MEM(session);
	return NULL;
}

int arpc_destroy_session(struct arpc_session_handle* session, int64_t timeout_ms)
{
	int ret = 0;
	QUEUE* iter;
	struct arpc_connection *con;

	LOG_THEN_RETURN_VAL_IF_TRUE(!session, -1, "session null.");
	ret = arpc_cond_lock(&session->cond); /* 锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_lock session fail.");
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(session->status == ARPC_SES_STA_SLEEP, unlock, "session on sleeping...");
	session->is_close = 1;
	arpc_cond_notify_all(&session->cond);//通知释放资源
	arpc_cond_unlock(&session->cond);
	arpc_usleep(10*1000);
	ret = arpc_cond_lock(&session->cond); /* 锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_lock session fail.");

	// 断开链路
	if (session->type == ARPC_SESSION_CLIENT) {
		if (session->status == ARPC_SES_STA_ACTIVE) {
			QUEUE_FOREACH_VAL(&session->q_con, iter, 
			{
				con = QUEUE_DATA(iter, struct arpc_connection, q);
				arpc_cond_unlock(&session->cond);
				ret = arpc_client_disconnect(con, timeout_ms);
				arpc_cond_lock(&session->cond);
				LOG_ERROR_IF_VAL_TRUE(ret, "arpc_client_disconnect fail.");
			});
		}
		
		ARPC_LOG_DEBUG(" to wait session close, status[%d]......", session->status);
		if(timeout_ms && session->status != ARPC_SES_STA_CLOSE){
			ret = arpc_cond_wait_timeout(&session->cond, timeout_ms);
			LOG_ERROR_IF_VAL_TRUE(ret, "arpc_cond_wait_timeout session status [%d] timeout.", session->status);
		}
	}
	
	// 释放con资源
	while(!QUEUE_EMPTY(&session->q_con)){
		iter = QUEUE_HEAD(&session->q_con);
		con = QUEUE_DATA(iter, struct arpc_connection, q);

		arpc_lock_connection(con);
		QUEUE_REMOVE(iter);
		QUEUE_INIT(iter);
		arpc_unlock_connection(con);
		arpc_cond_unlock(&session->cond);
		ret = arpc_destroy_connection(con);
		arpc_cond_lock(&session->cond);
		LOG_ERROR_IF_VAL_TRUE(ret, "arpc_destroy_connection fail.");
	}
	// 
	arpc_cond_unlock(&session->cond);

	ret = arpc_cond_destroy(&session->cond);
	LOG_ERROR_IF_VAL_TRUE(ret, "arpc_cond_destroy fail.");

	ret = arpc_mutex_destroy(&session->lock);
	LOG_ERROR_IF_VAL_TRUE(ret, "arpc_mutex_destroy fail.");
	arpc_cond_lock(&session->deamon_cond);
	arpc_cond_notify_all(&session->deamon_cond);
	arpc_cond_unlock(&session->deamon_cond);
	arpc_usleep(1000);

	ret = arpc_cond_destroy(&session->deamon_cond);
	LOG_ERROR_IF_VAL_TRUE(ret, "arpc_cond_destroy fail.");

	SAFE_FREE_MEM(session);
	return 0;
unlock:
	arpc_cond_unlock(&session->cond);
	return -1;
}

int arpc_session_connect_for_client(struct arpc_session_handle *session, int64_t timeout_ms)
{
	int ret = 0;
	QUEUE* iter;
	struct arpc_connection *con;

	LOG_THEN_RETURN_VAL_IF_TRUE(!session, -1, "session null.");
	ret = arpc_cond_lock(&session->cond); 
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_lock session fail.");
	if (session->type != ARPC_SESSION_CLIENT || session->status == ARPC_SES_STA_ACTIVE) {
		goto success;
	}
	ret = session_client_connect(session, timeout_ms);
success:
	arpc_cond_unlock(&session->cond);
	return ret;
}

int arpc_session_disconnect_for_client(struct arpc_session_handle *session, int64_t timeout_ms)
{
	int ret = 0;
	QUEUE* iter;
	struct arpc_connection *con;

	LOG_THEN_RETURN_VAL_IF_TRUE(!session, -1, "session null.");
	ret = arpc_cond_lock(&session->cond); 
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_lock session fail.");
	QUEUE_FOREACH_VAL(&session->q_con, iter, 
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		ret = arpc_client_disconnect(con, timeout_ms);
		LOG_ERROR_IF_VAL_TRUE(ret, "client conn[%u] disconnect fail.", con->id);
	});
	arpc_cond_unlock(&session->cond);
	return 0;
}
int session_established_for_client(struct arpc_session_handle *session)
{
	LOG_THEN_RETURN_VAL_IF_TRUE(!session, ARPC_ERROR, "session is null.");
	arpc_cond_lock(&session->cond);
	session->status = ARPC_SES_STA_ACTIVE;
	session->reconnect_times = 0;
	arpc_cond_notify_all(&session->cond);
	arpc_cond_unlock(&session->cond);
	ARPC_LOG_NOTICE("client established session[%p] success!!", session);
	return 0;
}

int session_notify_wakeup(struct arpc_session_handle *session)
{
	LOG_THEN_RETURN_VAL_IF_TRUE(!session, ARPC_ERROR, "session is null.");
	arpc_cond_lock(&session->cond);
	arpc_cond_notify(&session->cond);
	arpc_cond_unlock(&session->cond);
	return 0;
}

static int task_rebuild_for_client(void *thread_ctx)
{
	struct arpc_session_handle *session = (struct arpc_session_handle *)thread_ctx;
	struct arpc_client_ctx *client_ctx;
	int ret;
	QUEUE* iter;
	struct arpc_connection *con = NULL;

	ret = arpc_cond_lock(&session->cond);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ARPC_ERROR, "arpc_mutex_lock session[%p] fail.", session);

	if (session->reconnect_times > ARPC_SESSION_RECONNECT_FAIL_MAX_TIME) {
		session->status = ARPC_SES_STA_SLEEP;
		ARPC_LOG_NOTICE("session[%p] sleep wait next try time", session);
		arpc_cond_wait_timeout(&session->cond, ARPC_SESSION_RECONNECT_WAIT_TIME_S*1000);
		LOG_THEN_GOTO_TAG_IF_VAL_TRUE(session->is_close, unlock, "session have been closed.");
	}else{
		session->reconnect_times++;
	}
	client_ctx = (struct arpc_client_ctx *)session->ex_ctx;
	session->status = ARPC_SES_STA_REBUILD;
	ARPC_LOG_NOTICE("try rebuild session[%p]..., retry times[%u]", session, session->reconnect_times);
	session->xio_s = xio_session_create(&client_ctx->xio_param);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(!session->xio_s, unlock, "rebuild xio_session_create fail.");

	QUEUE_FOREACH_VAL(&session->q_con, iter,
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		ret = arpc_client_reconnect(con);
		LOG_ERROR_IF_VAL_TRUE(ret, "arpc_client_reconnect fail.");
	});

	arpc_cond_unlock(&session->cond);
	return 0;
unlock:
	arpc_cond_unlock(&session->cond);
	return ARPC_ERROR;
}

// 外部session已加锁,异步重建
int session_rebuild_for_client(struct arpc_session_handle *session)
{
	work_handle_t 	thread_handle;
	struct tp_thread_work thread;

	thread.loop = &task_rebuild_for_client;
	thread.stop = NULL;
	thread.usr_ctx = (void*)session;

	thread_handle = tp_post_one_work(session->threadpool, &thread, WORK_DONE_AUTO_FREE);
	LOG_THEN_RETURN_VAL_IF_TRUE(!thread_handle, ARPC_ERROR, "tp_post_one_work fail.");
	return 0;
}


// 外部session已加锁
static int session_cleanup_for_client(struct arpc_session_handle *session)
{
	struct arpc_client_ctx *client_ctx;
	int ret;
	QUEUE* iter;
	struct arpc_connection *con = NULL;

	session->status = ARPC_SES_STA_CLOSE;
	QUEUE_FOREACH_VAL(&session->q_con, iter,
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		ret = arpc_client_conn_stop_loop(con);
		LOG_ERROR_IF_VAL_TRUE(ret, "arpc_client_conn_stop_loop fail.");
	});
	session->xio_s = NULL;
	ARPC_LOG_NOTICE("stop connection loop of session[%p] end!!", session);

	return 0;
}

int session_client_teardown_event(struct arpc_session_handle *session)
{
	struct arpc_client_ctx *client_ctx;
	int ret;
	QUEUE* iter;
	struct xio_connection_params xio_con_param;
	struct arpc_connection *con = NULL;
	LOG_THEN_RETURN_VAL_IF_TRUE(!session, ARPC_ERROR, "session is null.");

	ret = arpc_cond_lock(&session->cond);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ARPC_ERROR, "arpc_cond_lock session[%p] fail.", session);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(session->type != ARPC_SESSION_CLIENT, unlock, "session not client.");
	ARPC_LOG_DEBUG("session[%p], close:%u, flag:%x, status:%d",session, session->is_close, session->flags, session->status);
	if (session->is_close || IS_SET(session->flags, ARPC_SESSION_ATTR_AUTO_DISCONNECT) 
		|| session->status == ARPC_SES_STA_INIT) {
		ret = session_cleanup_for_client(session);
	}else{
		ret = session_rebuild_for_client(session);
	}
	arpc_cond_notify_all(&session->cond);
	arpc_cond_unlock(&session->cond);

	return ret;
unlock:
	arpc_cond_unlock(&session->cond);
	return -1;
}

int session_insert_con(struct arpc_session_handle *s, struct arpc_connection *con)
{
	int ret;
	ret = arpc_cond_lock(&s->cond); /* 锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_lock session[%p] fail.", s);

	arpc_lock_connection(con);
	QUEUE_INSERT_TAIL(&s->q_con, &con->q);
	arpc_unlock_connection(con);

	s->conn_num++;
	arpc_cond_notify(&s->cond);
	arpc_cond_unlock(&s->cond);
	return 0;
}

int session_remove_con(struct arpc_session_handle *s, struct arpc_connection *con)
{
	int ret;
	ret = arpc_cond_lock(&s->cond); /* 锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_lock session[%p] fail.", s);

	arpc_lock_connection(con);
	QUEUE_REMOVE(&con->q);
	QUEUE_INIT(&con->q);
	arpc_unlock_connection(con);

	s->conn_num--;
	arpc_cond_notify(&s->cond);
	arpc_cond_unlock(&s->cond);
	return 0;
}

int session_move_con_tail(struct arpc_session_handle *s, struct arpc_connection *con)
{
	int ret;
	ret = arpc_cond_lock(&s->cond); /* 锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_lock session[%p] fail.", s);

	arpc_lock_connection(con);
	QUEUE_REMOVE(&con->q);
	QUEUE_INSERT_TAIL(&s->q_con, &con->q);
	arpc_unlock_connection(con);

	arpc_cond_unlock(&s->cond);
	return 0;
}
//外部已加锁
static int session_client_connect(struct arpc_session_handle *session, int64_t timeout_ms)
{
	int ret;
	QUEUE* iter;
	struct arpc_client_ctx *client_ctx;
	struct arpc_connection *con = NULL;

	client_ctx = (struct arpc_client_ctx *)session->ex_ctx;
	session->xio_s = xio_session_create(&client_ctx->xio_param);
	LOG_THEN_RETURN_VAL_IF_TRUE(!session->xio_s, ARPC_ERROR, "xio_session_create fail.");

	QUEUE_FOREACH_VAL(&session->q_con, iter, 
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		ret = arpc_client_connect(con, timeout_ms);
		LOG_ERROR_IF_VAL_TRUE(ret, " client conn[%u] connect fail.", con->id);
	});

	ret = arpc_cond_wait_timeout(&session->cond, timeout_ms);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_conn, 
								"wait session[%p] connect timeout, status[%d].", session, session->status);

	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(session->status != ARPC_SES_STA_ACTIVE, free_conn, 
								"session[%p] connect fail, status[%d].", session, session->status);

	QUEUE_FOREACH_VAL(&session->q_con, iter, 
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		ret = arpc_client_wait_connected(con, timeout_ms);
		LOG_ERROR_IF_VAL_TRUE(ret, " client wait conn[%u] connected fail.", con->id);
	});

	return 0;

free_conn:
	QUEUE_FOREACH_VAL(&session->q_con, iter, 
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		ret = arpc_client_disconnect(con, timeout_ms);
		LOG_ERROR_IF_VAL_TRUE(ret, "arpc_client_disconnect fail.");
	});
	return ARPC_ERROR;
}

int session_get_idle_conn(struct arpc_session_handle *session, struct arpc_connection **conn, 
							enum  arpc_msg_type msg_type, int64_t timeout_ms)
{
	int ret;
	QUEUE* iter;
	int try_time = 0;
	struct arpc_connection *con = NULL;

	LOG_THEN_RETURN_VAL_IF_TRUE(!session, ARPC_ERROR, "session is null.");

	ret = arpc_mutex_lock(&session->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ARPC_ERROR, "arpc_mutex_lock session[%p] fail.", session);

	ret = arpc_cond_lock(&session->cond);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ARPC_ERROR, "arpc_mutex_lock session[%p] fail.", session);
	if (session->type == ARPC_SESSION_CLIENT && session->status == ARPC_SES_STA_CLOSE
		&& IS_SET(session->flags, ARPC_SESSION_ATTR_AUTO_DISCONNECT)) {
		ARPC_LOG_NOTICE("session[%p] close down[%d], try to reconnection", session, session->status);
		ret = session_client_connect(session, timeout_ms);
		LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, unlock,"session_client_connect fail");
	}

	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(QUEUE_EMPTY(&session->q_con), unlock,"connection empty fail");
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(session->status != ARPC_SES_STA_ACTIVE, unlock, 
								"session[%p] invalid, session status[%d]", session, session->status);
	while(!con){
		QUEUE_FOREACH_VAL(&session->q_con, iter,
		{
			con = QUEUE_DATA(iter, struct arpc_connection, q);
			ret = arpc_check_connection_valid(con, msg_type);
			if(!ret){
				ret = arpc_lock_connection(con);
				QUEUE_REMOVE(&con->q);
				QUEUE_INSERT_TAIL(&session->q_con, &con->q);
				arpc_unlock_connection(con);
				break;
			}
			con = NULL;
		});
		
		if(con){break;}
		try_time++;
		ARPC_LOG_NOTICE("warning: session[%p][con_num:%u], status[%u],flags[0x%x] wait[%u ms]...", 
						session, session->conn_num, session->status, session->flags, ARPC_SESSION_BUSY_WAIT_TIME_MS);
		ret = arpc_cond_wait_timeout(&session->cond, ARPC_SESSION_BUSY_WAIT_TIME_MS);
		if (ret && try_time > ARPC_SESSION_BUSY_RETRY_CNT) {
			ARPC_LOG_ERROR("session[%p] wait idle connection timeout[%lu ms].", session, (uint64_t)(try_time *ARPC_SESSION_BUSY_WAIT_TIME_MS));
			goto unlock;
		}
	}
	arpc_cond_unlock(&session->cond);
	arpc_mutex_unlock(&session->lock);
	*conn = con;
	return (*conn)?0:ARPC_ERROR;
unlock:
	arpc_cond_unlock(&session->cond);
	arpc_mutex_unlock(&session->lock);
	return ARPC_ERROR;
}

static inline const char *get_type_str(enum arpc_session_type type)
{
	static const  char *server = "server";
	static const  char *client = "client";
	static const  char *none = "unkown type";
	if(type == ARPC_SESSION_SERVER){
		return server;
	}
	if(type == ARPC_SESSION_CLIENT){
		return client;
	}
	return none;
}

#define SESSION_STATUS_SHOW_HEAD\
 "\n  ### session:%p   session type:%s  conn-num:%u  timeout-ms:%d  status:%d  data-max:%lu head-max:%u iov-max:%u ###\n"\
 "*conn-id     "\
 "*tx-req:cnt*2/cmit/send      *tx-rsp:cnt*2/cmit/send      *tx-ow:cnt*2/cmit/send       "\
 "*rx-req:cnt/rx  /cb          *rx-rsp:cnt/rx  /cb          *rx-ow:cnt/rx  /cb   \n"

#define SESSION_STATUS_SHOW\
 "%-4u  %8lu|%02lu.%06lu|%02lu.%06lu %8lu|%02lu.%06lu|%02lu.%06lu %8lu|%02lu.%06lu|%02lu.%06lu %8lu|%02lu.%06lu|%02lu.%06lu %8lu|%02lu.%06lu|%02lu.%06lu %8lu|%02lu.%06lu|%02lu.%06lu\n" 

void print_session_status(struct arpc_session_handle *session, struct timeval *now)
{
	int ret;
	QUEUE* iter;
	int try_time = 0;
	struct arpc_connection *con = NULL;
	if (!session) {
		return;
	}

	if (now && (session->interval.tv_sec + 3 >= now->tv_sec)) {
		return;
	}

	ret = arpc_cond_lock(&session->cond);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ;, "arpc_mutex_lock session[%p] fail.", session);
	if(now){
		session->interval = *now;
	}else{
		session->interval.tv_sec = 0;
	}
	ARPC_LOG_STATUS(SESSION_STATUS_SHOW_HEAD, session, get_type_str(session->type), session->conn_num, 
					session->conn_timeout_ms, session->status,
					session->msg_data_max_len, session->msg_head_max_len, session->msg_iov_max_len);
	QUEUE_FOREACH_VAL(&session->q_con, iter,
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		ARPC_LOG_STATUS(SESSION_STATUS_SHOW, 
						con->id,
						con->tx_req_count, con->tx_req_cmit.ave.tv_sec, con->tx_req_cmit.ave.tv_usec, con->tx_req.ave.tv_sec, con->tx_req.ave.tv_usec,
						con->tx_rsp_count, con->tx_rsp_cmit.ave.tv_sec, con->tx_rsp_cmit.ave.tv_usec, con->tx_rsp.ave.tv_sec, con->tx_rsp.ave.tv_usec,
						con->tx_ow_count, con->tx_ow_cmit.ave.tv_sec, con->tx_ow_cmit.ave.tv_usec, con->tx_ow.ave.tv_sec, con->tx_ow.ave.tv_usec,
						con->rx_req_count, con->rx_req_send.ave.tv_sec, con->rx_req_send.ave.tv_usec, con->rx_req.ave.tv_sec, con->rx_req.ave.tv_usec,
						con->rx_rsp_count, con->rx_rsp_send.ave.tv_sec, con->rx_rsp_send.ave.tv_usec, con->rx_rsp.ave.tv_sec, con->rx_rsp.ave.tv_usec,
						con->rx_ow_count, con->rx_ow_send.ave.tv_sec, con->rx_ow_send.ave.tv_usec, con->rx_ow.ave.tv_sec, con->rx_ow.ave.tv_usec);
	});
	ARPC_LOG_STATUS("-----------------------------------------------\n\n");
	arpc_cond_unlock(&session->cond);
	return;
} 

enum arpc_session_status arpc_get_session_status(const arpc_session_handle_t fd)
{
	if(!fd)
		return ARPC_SES_STA_INIT;
	return ((const struct arpc_session_handle *)fd)->status;
}

void arpc_session_info(const arpc_session_handle_t fd)
{
	if(!fd)
		return ;
	print_session_status(((struct arpc_session_handle *)fd), NULL);
}