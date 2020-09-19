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
#include <arpa/inet.h>
#include <sched.h>

#include "arpc_session.h"

#define WAIT_THREAD_RUNING_TIMEOUT (5*1000)

static const uint32_t conn_magic = 0xff538770;

// client
static struct arpc_connection *arpc_create_connection();
static int arpc_destroy_connection(struct arpc_connection* con);
static int xio_client_run_con(void * ctx);
static void xio_client_stop_con(void * ctx);
static void destroy_connection_handle(QUEUE* con_q);
static void dis_connection(QUEUE* con_q);
static struct arpc_connection *arpc_create_xio_client_con(struct arpc_session_handle *s, uint32_t index);
static int  arpc_destroy_xio_client_con(struct arpc_connection *con);
static struct arpc_connection *arpc_create_xio_server_con(struct arpc_session_handle *s);
static int  arpc_destroy_xio_server_con(struct arpc_connection *con);

//server
static struct arpc_work_handle *arpc_create_work();
static void destroy_work_handle(QUEUE* work_q);
static void destroy_session_handle(QUEUE* session_q);
static int arpc_destroy_work(struct arpc_work_handle* work);
static int xio_server_work_run(void * ctx);
static void xio_server_work_stop(void * ctx);

struct arpc_session_handle *arpc_create_session(enum session_type type, uint32_t ex_ctx_size)
{
	int ret = 0;
	int i =0;
	struct arpc_session_handle *session = NULL;
	/* handle*/
	session = (struct arpc_session_handle *)ARPC_MEM_ALLOC(sizeof(struct arpc_session_handle) + ex_ctx_size, NULL);
	if (!session) {
		ARPC_LOG_ERROR( "malloc error, exit ");
		return NULL;
	}
	memset(session, 0, sizeof(struct arpc_session_handle) + ex_ctx_size);
	ret = arpc_mutex_init(&session->lock); /* 初始化互斥锁 */
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_buf, "arpc_mutex_init fail.");

	ret = arpc_cond_init(&session->cond); /* 初始化互斥锁 */
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_mutex, "arpc_cond_init fail.");

	QUEUE_INIT(&session->q);
	QUEUE_INIT(&session->q_con);
	session->threadpool = _arpc_get_threadpool();
	session->type = type;
	session->is_close = 0;
	return session;
free_mutex:
	arpc_mutex_destroy(&session->lock);
free_buf:
	SAFE_FREE_MEM(session);
	return NULL;
}

int arpc_destroy_session(struct arpc_session_handle* session)
{
	int ret = 0;
	QUEUE* q;
	LOG_THEN_RETURN_VAL_IF_TRUE(!session, -1, "session null.");

	ret = arpc_mutex_lock(&session->lock); /* 锁 */
	LOG_ERROR_IF_VAL_TRUE(ret, "arpc_mutex_lock fail.");
	session->is_close = 1;
	ret = arpc_mutex_unlock(&session->lock); /* 开锁 */
	LOG_ERROR_IF_VAL_TRUE(ret, "arpc_mutex_lock fail.");
	
	if(session->type == SESSION_CLIENT) {
		arpc_cond_lock(&session->cond);
	}
	// 断开
	q = NULL;
	QUEUE_FOREACH_VAL(&session->q_con, q, dis_connection(q));

	if(session->type == SESSION_CLIENT) {
		arpc_cond_wait_timeout(&session->cond, 5*1000);
	}
	// 回收con资源
	while(!QUEUE_EMPTY(&session->q_con)){
		q = QUEUE_HEAD(&session->q_con);
		QUEUE_REMOVE(q);
		QUEUE_INIT(q);
		destroy_connection_handle(q);
	}

	if(session->type == SESSION_CLIENT) {
		arpc_cond_unlock(&session->cond);
	}
	ret = arpc_cond_destroy(&session->cond);
	LOG_ERROR_IF_VAL_TRUE(ret, "arpc_cond_destroy fail.");

	ret = arpc_mutex_destroy(&session->lock);
	LOG_ERROR_IF_VAL_TRUE(ret, "arpc_mutex_unlock fail.");
	SAFE_FREE_MEM(session);
	return 0;
}

int session_insert_con(struct arpc_session_handle *s, struct arpc_connection *con)
{
	int ret;
	ret = arpc_mutex_lock(&s->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_lock session[%p] fail.", s);
	QUEUE_INSERT_TAIL(&s->q_con, &con->q);
	s->conn_num++;
	arpc_mutex_unlock(&s->lock);
	return 0;
}

int session_remove_con(struct arpc_session_handle *s, struct arpc_connection *con)
{
	int ret;
	ret = arpc_mutex_lock(&s->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_lock session[%p] fail.", s);
	QUEUE_REMOVE(&con->q);
	QUEUE_INIT(&con->q);
	s->conn_num--;
	arpc_mutex_unlock(&s->lock);
	return 0;
}

int get_connection(struct arpc_session_handle *s, struct arpc_connection **pcon)
{
	int ret;
	QUEUE* iter;
	struct arpc_connection *con = NULL;

	LOG_THEN_RETURN_VAL_IF_TRUE(!pcon, ARPC_ERROR, "pcon is null.");

	ret = arpc_mutex_lock(&s->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ARPC_ERROR, "arpc_mutex_lock session[%p] fail.", s);
	if(QUEUE_EMPTY(&s->q_con)){
		arpc_mutex_unlock(&s->lock);
		return -1;
	}
	QUEUE_FOREACH_VAL(&s->q_con, iter,
	{
		con = QUEUE_DATA(iter, struct arpc_connection, q);
		arpc_rwlock_wrlock(&con->rwlock);
		if((!con->is_busy) && (con->status == XIO_STA_RUN_ACTION)){
			con->is_busy = 1;
			arpc_rwlock_unlock(&con->rwlock);
			break;
		}else{
			ARPC_LOG_ERROR("connection[%p] is busy, status: %d|%d.", con->xio_con,con->is_busy, con->status);
		}
		arpc_rwlock_unlock(&con->rwlock);
		con = NULL;
	});
	arpc_mutex_unlock(&s->lock);
	*pcon = con;
	return (con)?0:-1;
}

int put_connection(struct arpc_session_handle *s, struct arpc_connection *con)
{
	int ret;

	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ARPC_ERROR, "con is null.");

	ret = arpc_mutex_lock(&s->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ARPC_ERROR, "arpc_mutex_lock session[%p] fail.", s);
	if(QUEUE_EMPTY(&s->q_con)){
		arpc_mutex_unlock(&s->lock);
		return ARPC_ERROR;
	}
	arpc_rwlock_wrlock(&con->rwlock);
	con->is_busy = 0;
	arpc_rwlock_unlock(&con->rwlock);
	arpc_mutex_unlock(&s->lock);
	return 0;
}

struct arpc_connection *arpc_create_con(enum arpc_connection_type type, 
										struct arpc_session_handle *s, 
										uint32_t index)
{
	struct arpc_connection *con = NULL;
	int ret;

	if (type == ARPC_CON_TYPE_CLIENT) {
		con = arpc_create_xio_client_con(s, index);
		LOG_THEN_RETURN_VAL_IF_TRUE(!con, NULL, "arpc_create_xio_client_con fail.");
	}else if (type == ARPC_CON_TYPE_SERVER) {
		con = arpc_create_xio_server_con(s);
	}else{
		ARPC_LOG_ERROR("unkown connetion type[%d]", type);
		return NULL;
	}
	con->type = type;
	con->session = s;
	con->usr_ops_ctx = s->usr_context;
	con->ops = &s->ops;

	ret = session_insert_con(s, con);
	if (ret) {
		ARPC_LOG_ERROR("session[%p] insert con[%p] fail", s, con);
		ret = arpc_destroy_con(con);
		LOG_ERROR_IF_VAL_TRUE(ret, "arpc_destroy_con fail.");
		con = NULL;
	}
	return con;
}

int  arpc_disconnection(struct arpc_connection *con)
{
	int ret;
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ARPC_ERROR, "arpc_connection is null.");

	if (con->type == ARPC_CON_TYPE_CLIENT) {
		arpc_cond_lock(&con->cond);
		if(con->xio_con){
			xio_disconnect(con->xio_con);
			ret = arpc_cond_wait_timeout(&con->cond, 5*1000);
			LOG_ERROR_IF_VAL_TRUE(ret, "arpc_cond_wait_timeout con[%p] disclosed time out.", con);
		}
		arpc_cond_unlock(&con->cond);
		return 0;
	}else if (con->type == ARPC_CON_TYPE_SERVER) {
		if(con->xio_con)
			xio_disconnect(con->xio_con);
		return 0;
	}else{
		ARPC_LOG_ERROR("unkown connetion type[%d]", con->type);
	}
	return -1;
}

int  arpc_wait_connected(struct arpc_connection *con, uint64_t timeout_ms)
{
	int ret = 0;
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ARPC_ERROR, "arpc_connection is null.");
	if (con->type == ARPC_CON_TYPE_CLIENT) {
		arpc_cond_lock(&con->cond);
		if(con->xio_con && (con->is_busy || con->status != XIO_STA_RUN_ACTION)){
			ret = arpc_cond_wait_timeout(&con->cond, timeout_ms);
			LOG_ERROR_IF_VAL_TRUE(ret, "wait connection finished timeout con[%p].", con);
		}
		arpc_cond_unlock(&con->cond);
	}else{
		ARPC_LOG_ERROR("not need wait connetion finish, type[%d]", con->type);
		return -1;
	}
	return ret;
}

int  arpc_destroy_con(struct arpc_connection *con)
{
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ARPC_ERROR, "arpc_connection is null.");

	if (con->type == ARPC_CON_TYPE_CLIENT) {
		return arpc_destroy_xio_client_con(con);
	}else if (con->type == ARPC_CON_TYPE_SERVER) {
		return arpc_destroy_xio_server_con(con);
	}else{
		ARPC_LOG_ERROR("unkown connetion type[%d]", con->type);
	}
	return -1;
}


int rebuild_session(struct arpc_session_handle *ses)
{
	return 0;
}


// server
struct arpc_server_handle *arpc_create_server(uint32_t ex_ctx_size)
{
	int ret = 0;
	int i =0;
	struct arpc_server_handle *svr = NULL;
	/* handle*/
	svr = (struct arpc_server_handle *)ARPC_MEM_ALLOC(sizeof(struct arpc_server_handle) + ex_ctx_size, NULL);
	if (!svr) {
		ARPC_LOG_ERROR( "malloc error, exit ");
		return NULL;
	}
	memset(svr, 0, sizeof(struct arpc_server_handle) + ex_ctx_size);
	ret = arpc_mutex_init(&svr->lock); /* 初始化互斥锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, NULL, "arpc_mutex_init fail.");
	QUEUE_INIT(&svr->q_session);
	QUEUE_INIT(&svr->q_work);
	svr->iov_max_len = IOV_DEFAULT_MAX_LEN;
	svr->threadpool = _arpc_get_threadpool();
	return svr;
}

int arpc_destroy_server(struct arpc_server_handle* svr)
{
	int ret = 0;
	QUEUE* q;

	LOG_THEN_RETURN_VAL_IF_TRUE(!svr, -1, "svr null.");

	ret = arpc_mutex_lock(&svr->lock); /* 锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_lock fail.");

	q = NULL;
	QUEUE_FOREACH_VAL(&svr->q_session, q, destroy_session_handle(q));
	q = NULL;
	QUEUE_FOREACH_VAL(&svr->q_work, q, destroy_work_handle(q));

	ret = arpc_mutex_unlock(&svr->lock); /* 开锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_unlock fail.");

	ret = arpc_mutex_destroy(&svr->lock); /* 初始化互斥锁 */
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_destroy fail.");

	SAFE_FREE_MEM(svr);
	return 0;
}

struct arpc_work_handle *arpc_create_xio_server_work(const struct arpc_con_info *con_param, 
													void *threadpool, 
													struct xio_session_ops *work_ops,
													uint32_t index)
{
	struct arpc_work_handle *work;
	int ret;
	work_handle_t hd;
	struct tp_thread_work thread;

	work = arpc_create_work();
	LOG_THEN_RETURN_VAL_IF_TRUE(!work, NULL, "arpc_create_work fail.");
	work->affinity = index + 1;
	work->work_ctx = xio_context_create(NULL, 0, work->affinity);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(!work->work_ctx, free_work, "xio_context_create fail.");

	ret = get_uri(con_param, work->uri, URI_MAX_LEN);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_work, "get_uri fail");

	ARPC_LOG_NOTICE("set uri[%s] on work[%u].", work->uri, index);

	work->work = xio_bind(work->work_ctx, work_ops, work->uri, NULL, 0, work);

	ret = arpc_cond_lock(&work->cond);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_xio_work, "arpc_cond_lock fail.");
	thread.loop = &xio_server_work_run;
	thread.stop = &xio_server_work_stop;
	thread.usr_ctx = (void*)work;
	work->thread_handle = tp_post_one_work(threadpool, &thread, WORK_DONE_AUTO_FREE);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(!work->thread_handle, free_cond, "tp_post_one_work fail.");

	ret = arpc_cond_wait_timeout(&work->cond, WAIT_THREAD_RUNING_TIMEOUT);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, cancel_work, "wait thread runing fail.");

	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(work->status != XIO_WORK_STA_RUN, cancel_work, "work run status[%d] fail.", work->status);
	arpc_cond_unlock(&work->cond);

	ARPC_LOG_NOTICE("create work server uri[%s] success.", work->uri);
	return work;

cancel_work:
	tp_cancel_one_work(work->thread_handle);
free_cond:
	arpc_cond_unlock(&work->cond);
free_xio_work:
	xio_context_destroy(work->work_ctx);
free_work:
	arpc_destroy_work(work);
	return NULL;
}

int  arpc_destroy_xio_server_work(struct arpc_work_handle *work)
{
	int ret;
	LOG_THEN_RETURN_VAL_IF_TRUE(!work, -1, "arpc_work_handle is null fail.");

	arpc_cond_lock(&work->cond);

	ret = tp_cancel_one_work(work->thread_handle);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, unlock, "tp_cancel_one_work fail.");
	arpc_cond_wait_timeout(&work->cond, WAIT_THREAD_RUNING_TIMEOUT);
	
	arpc_cond_unlock(&work->cond);

	xio_context_destroy(work->work_ctx);
	arpc_destroy_work(work);
	return 0;
unlock:
	arpc_cond_unlock(&work->cond);
	return -1;
}

int server_insert_work(struct arpc_server_handle *server, struct arpc_work_handle *work)
{
	int ret;
	ret = arpc_mutex_lock(&server->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_lock server[%p] fail.", server);
	QUEUE_INSERT_TAIL(&server->q_work, &work->q);
	server->work_num++;
	arpc_mutex_unlock(&server->lock);
	return 0;
}

int server_remove_work(struct arpc_server_handle *server, struct arpc_work_handle *work)
{
	int ret;
	ret = arpc_mutex_lock(&server->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_lock server[%p] fail.", server);
	QUEUE_REMOVE(&work->q);
	QUEUE_INIT(&work->q);
	server->work_num--;
	arpc_mutex_unlock(&server->lock);
	return 0;
}

int server_insert_session(struct arpc_server_handle *server, struct arpc_session_handle *session)
{
	int ret;
	ret = arpc_mutex_lock(&server->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_lock server[%p] fail.", server);
	QUEUE_INSERT_TAIL(&server->q_work, &session->q);
	arpc_mutex_unlock(&server->lock);
	return 0;
}

int server_remove_session(struct arpc_server_handle *server, struct arpc_session_handle *session)
{
	int ret;
	ret = arpc_mutex_lock(&server->lock);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_mutex_lock server[%p] fail.", server);
	QUEUE_REMOVE(&session->q);
	QUEUE_INIT(&session->q);
	arpc_mutex_unlock(&server->lock);
	return 0;
}

static struct arpc_connection *arpc_create_xio_server_con(struct arpc_session_handle *s)
{
	struct arpc_connection *con;
	int ret;
	con = arpc_create_connection();
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, NULL, "arpc_create_connection fail.");
	con->status = XIO_STA_RUN_ACTION;
	return con;
}

static int  arpc_destroy_xio_server_con(struct arpc_connection *con)
{
	int ret;
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, -1, "arpc_connection is null fail.");
	arpc_destroy_connection(con);
	return 0;
}

static struct arpc_connection *arpc_create_xio_client_con(struct arpc_session_handle *s, uint32_t index)
{
	struct arpc_connection *con;
	int ret;
	struct xio_connection_params xio_con_param;
	work_handle_t hd;
	struct tp_thread_work thread;

	con = arpc_create_connection();
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, NULL, "arpc_create_connection fail.");
	con->client.affinity = index + 1;
	con->client.xio_con_ctx = xio_context_create(NULL, 0, con->client.affinity);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(!con->client.xio_con_ctx, free_con, "xio_context_create fail.");
	(void)memset(&xio_con_param, 0, sizeof(struct xio_connection_params));
	xio_con_param.session			= s->xio_s;
	xio_con_param.ctx				= con->client.xio_con_ctx;
	xio_con_param.conn_idx			= 1089+index;
	xio_con_param.conn_user_context	= con;
	con->xio_con = xio_connect(&xio_con_param);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(!con->xio_con, free_xio_ctx, "xio_connect fail.");
	thread.loop = &xio_client_run_con;
	thread.stop = &xio_client_stop_con;
	thread.usr_ctx = (void*)con;

	ret = arpc_cond_lock(&con->cond);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, free_xio_con, "arpc_cond_lock fail.");

	con->client.thread_handle = tp_post_one_work(s->threadpool, &thread, WORK_DONE_AUTO_FREE);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(!con->client.thread_handle, free_cond, "tp_post_one_work fail.");

	ret = arpc_cond_wait_timeout(&con->cond, WAIT_THREAD_RUNING_TIMEOUT);
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, cancel_work, "wait thread runing fail.");
	
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(con->status != XIO_STA_RUN, cancel_work, "con run status[%d] fail.", con->status);
	arpc_cond_unlock(&con->cond);
	return con;

cancel_work:
	arpc_cond_unlock(&con->cond);
	tp_cancel_one_work(&con->client.thread_handle);
free_cond:
	arpc_cond_unlock(&con->cond);
free_xio_con:
	xio_connection_destroy(con->xio_con);
free_xio_ctx:
	xio_context_destroy(con->client.xio_con_ctx);
free_con:
	arpc_destroy_connection(con);
	return NULL;
}

static int  arpc_destroy_xio_client_con(struct arpc_connection *con)
{
	int ret;
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, -1, "arpc_connection is null fail.");

	ret = tp_cancel_one_work(&con->client.thread_handle);// 有锁，不要再用锁了
	LOG_THEN_GOTO_TAG_IF_VAL_TRUE(ret, unlock, "tp_cancel_one_work fail.");

	xio_connection_destroy(con->xio_con);
	xio_context_destroy(con->client.xio_con_ctx);
	arpc_destroy_connection(con);
	return 0;
unlock:
	return -1;
}

static struct arpc_connection *arpc_create_connection()
{
	int ret = 0;
	struct arpc_connection *con = NULL;
	
	/* handle*/
	con = (struct arpc_connection *)ARPC_MEM_ALLOC(sizeof(struct arpc_connection), NULL);
	if (!con) {
		ARPC_LOG_ERROR( "malloc error, exit ");
		return NULL;
	}
	memset(con, 0, sizeof(struct arpc_connection));

	ret = arpc_cond_init(&con->cond); 
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, NULL, "arpc_mutex_init fail.");
	ret = arpc_rwlock_init(&con->rwlock); 
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, NULL, "arpc_rwlock_init fail.");
	QUEUE_INIT(&con->q);
	con->is_busy = 1;
	con->magic = conn_magic;
	return con;
}

static int arpc_destroy_connection(struct arpc_connection* con)
{
	int ret = 0;
	ret = arpc_cond_destroy(&con->cond); 
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, -1, "arpc_cond_destroy fail.");
	SAFE_FREE_MEM(con);
	return 0;
}

static int xio_client_run_con(void * ctx)
{
	struct arpc_connection *con = (struct arpc_connection *)ctx;
	int32_t sleep_time = 0;
	cpu_set_t		cpuset;

	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ARPC_ERROR, "con null fail.");
	ARPC_LOG_DEBUG("session run on the thread[%lu].", pthread_self());
	arpc_cond_lock(&con->cond);

	CPU_ZERO(&cpuset);
	CPU_SET(con->client.affinity, &cpuset);
	pthread_setaffinity_np(tp_get_work_thread_id(con->client.thread_handle), sizeof(cpu_set_t), &cpuset);
	con->status = XIO_STA_RUN;

	arpc_cond_notify(&con->cond);

	arpc_cond_unlock(&con->cond);
	for(;;){	
		if (xio_context_run_loop(con->client.xio_con_ctx, XIO_INFINITE) < 0)
			ARPC_LOG_ERROR("xio error msg: %s.", xio_strerror(xio_errno()));
		ARPC_LOG_DEBUG("xio context run loop pause...");
		arpc_cond_lock(&con->cond);
		sleep_time = con->client.recon_interval_s;
		if(con->status == XIO_STA_CLEANUP){
			arpc_cond_notify(&con->cond);
			break;
		}
		arpc_cond_unlock(&con->cond);
		if (sleep_time > 0) 
			arpc_sleep(sleep_time);	// 恢复周期
	}
	ARPC_LOG_NOTICE("xio connection[%p] on thread[%lu] exit now.", con,  pthread_self());
	arpc_cond_unlock(&con->cond);
	return ARPC_SUCCESS;
}

static void xio_client_stop_con(void * ctx)
{
	struct arpc_connection *con = (struct arpc_connection *)ctx;
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ;, "con null fail.");
	arpc_cond_lock(&con->cond);
	con->status = XIO_STA_CLEANUP;
	if(con->client.xio_con_ctx)
		xio_context_stop_loop(con->client.xio_con_ctx);
	arpc_cond_unlock(&con->cond);
	return;
}

static void dis_connection(QUEUE* con_q)
{
	struct arpc_connection *con;
	int ret;

	LOG_THEN_RETURN_VAL_IF_TRUE(!con_q, ;, "con_q null.");
	con = QUEUE_DATA(con_q, struct arpc_connection, q);
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ;, "arpc_connection null.");

	ret = arpc_disconnection(con);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ;, "arpc_disconnection fail.");
}

static void destroy_connection_handle(QUEUE* con_q)
{
	struct arpc_connection *con;
	int ret;

	LOG_THEN_RETURN_VAL_IF_TRUE(!con_q, ;, "con_q null.");
	con = QUEUE_DATA(con_q, struct arpc_connection, q);
	LOG_THEN_RETURN_VAL_IF_TRUE(!con, ;, "arpc_connection null.");

	ret = arpc_destroy_con(con);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ;, "arpc_destroy_con fail.");
}
//server inter
static struct arpc_work_handle *arpc_create_work()
{
	int ret = 0;
	struct arpc_work_handle *work = NULL;
	
	/* handle*/
	work = (struct arpc_work_handle *)ARPC_MEM_ALLOC(sizeof(struct arpc_work_handle), NULL);
	if (!work) {
		ARPC_LOG_ERROR( "malloc error, exit ");
		return NULL;
	}
	memset(work, 0, sizeof(struct arpc_work_handle));

	ret = arpc_cond_init(&work->cond); 
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, NULL, "arpc_cond_init fail.");
	QUEUE_INIT(&work->q);
	return work;
}

static void destroy_work_handle(QUEUE* work_q)
{
	struct arpc_work_handle *work_handle;
	int ret;

	LOG_THEN_RETURN_VAL_IF_TRUE(!work_q, ;, "work_q null.");
	work_handle = QUEUE_DATA(work_q, struct arpc_work_handle, q);
	LOG_THEN_RETURN_VAL_IF_TRUE(!work_handle, ;, "work_handle null.");

	ret = arpc_destroy_xio_server_work(work_handle);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ;, "arpc_destroy_session fail.");
}

static void destroy_session_handle(QUEUE* session_q)
{
	struct arpc_session_handle *session;
	int ret;

	LOG_THEN_RETURN_VAL_IF_TRUE(!session_q, ;, "session_q null.");
	session = QUEUE_DATA(session_q, struct arpc_session_handle, q);
	LOG_THEN_RETURN_VAL_IF_TRUE(!session, ;, "session null.");

	ret = arpc_destroy_session(session);
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ;, "arpc_destroy_session fail.");
}

static int arpc_destroy_work(struct arpc_work_handle* work)
{
	int ret = 0;
	ret = arpc_cond_destroy(&work->cond); 
	LOG_THEN_RETURN_VAL_IF_TRUE(ret, ARPC_ERROR, "arpc_cond_destroy fail.");
	SAFE_FREE_MEM(work);
	return 0;
}

static int xio_server_work_run(void * ctx)
{
	struct arpc_work_handle *work = (struct arpc_work_handle *)ctx;
	cpu_set_t		cpuset;

	LOG_THEN_RETURN_VAL_IF_TRUE(!work, ARPC_ERROR, "work null fail.");
	ARPC_LOG_DEBUG("work run on the thread[%lu].", pthread_self());
	arpc_cond_lock(&work->cond);

	CPU_ZERO(&cpuset);
	CPU_SET(work->affinity, &cpuset);
	pthread_setaffinity_np(tp_get_work_thread_id(work->thread_handle), sizeof(cpu_set_t), &cpuset);
	work->status = XIO_WORK_STA_RUN;

	arpc_cond_notify(&work->cond);

	arpc_cond_unlock(&work->cond);
	for(;;){	
		if (xio_context_run_loop(work->work_ctx, XIO_INFINITE) < 0)
			ARPC_LOG_ERROR("xio error msg: %s.", xio_strerror(xio_errno()));
		ARPC_LOG_DEBUG("xio context run loop pause...");
		arpc_cond_lock(&work->cond);
		if(work->status == XIO_WORK_STA_EXIT){
			arpc_cond_notify(&work->cond);
			break;
		}
		arpc_cond_unlock(&work->cond);
		arpc_sleep(1);
	}
	ARPC_LOG_NOTICE("xio server work[%p] on thread[%lu] exit now.", work, pthread_self());
	arpc_cond_unlock(&work->cond);
	return ARPC_SUCCESS;
}

static void xio_server_work_stop(void * ctx)
{
	struct arpc_work_handle *work = (struct arpc_work_handle *)ctx;
	LOG_THEN_RETURN_VAL_IF_TRUE(!work, ;, "work null fail.");
	work->status = XIO_WORK_STA_EXIT;
	if(work->work_ctx)
		xio_context_stop_loop(work->work_ctx);
	return;
}