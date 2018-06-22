/*
 *   Copyright (C) 2018 Samsung Electronics Co., Ltd.
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 */

#ifndef __CIFSD_TRANSPORT_TCP_H__
#define __CIFSD_TRANSPORT_TCP_H__

#include <linux/list.h>
#include <linux/ip.h>
#include <net/sock.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>
#include <net/request_sock.h>
#include <linux/kthread.h>
#include <linux/nls.h>

#define CIFSD_SERVER_PORT		445
#define CIFSD_SOCKET_BACKLOG		16

#define IS_SMB2(x) ((x)->vals->protocol_id != SMB10_PROT_ID)

/*
 * WARNING
 *
 * This is nothing but a HACK. Session status should move to channel
 * or to session. As of now we have 1 tcp_conn : 1 cifsd_session, but
 * we need to change it to 1 tcp_conn : N cifsd_sessions.
 */
enum {
	CIFSD_SESS_NEW = 0,
	CIFSD_SESS_GOOD,
	CIFSD_SESS_EXITING,
	CIFSD_SESS_NEED_RECONNECT,
	CIFSD_SESS_NEED_NEGOTIATE
};

/* crypto hashing related structure/fields, not specific to a sec mech */
struct cifsd_secmech {
	struct crypto_shash *hmacmd5; /* hmac-md5 hash function */
	struct crypto_shash *md5; /* md5 hash function */
	struct crypto_shash *hmacsha256; /* hmac-sha256 hash function */
	struct crypto_shash *cmacaes; /* block-cipher based MAC function */
	struct crypto_shash *sha512; /* sha512 hash function */
	struct sdesc *sdeschmacmd5;  /* ctxt to generate ntlmv2 hash, CR1 */
	struct sdesc *sdescmd5; /* ctxt to generate cifs/smb signature */
	struct sdesc *sdeschmacsha256;  /* ctxt to generate smb2 signature */
	struct sdesc *sdesccmacaes;  /* ctxt to generate smb3 signature */
	struct sdesc *sdescsha512;  /* ctxt to generate preauth integrity */
};

struct cifsd_tcp_conn_ops {
	int	(*init_fn)(struct cifsd_tcp_conn *conn);
	int	(*process_fn)(struct cifsd_tcp_conn *conn);
	int	(*terminate_fn)(struct cifsd_tcp_conn *conn);

	size_t	(*header_size_fn)(void);
};

struct cifsd_tcp_conn {
	struct socket			*sock;
	unsigned short			family;
	/* The number of sessions attached with this connection */
	int				sess_count;
	struct smb_version_values	*vals;
	struct smb_version_ops		*ops;
	struct smb_version_cmds		*cmds;
	unsigned int			max_cmds;
	char				*hostname;
	struct mutex			srv_mutex;
	int				tcp_status;
	unsigned int			cli_cap;
	unsigned int			srv_cap;
	struct kvec			*iov;
	unsigned int			nr_iov;
	void 				*request_buf;
	struct nls_table		*local_nls;
	unsigned int			total_read;
	struct list_head		tcp_conns;
	/* smb session 1 per user */
	struct list_head		cifsd_sess;
	struct task_struct		*handler;
	unsigned long			last_active;
	/* How many request are running currently */
	atomic_t			req_running;
	/* References which are made for this Server object*/
	atomic_t			r_count;
	wait_queue_head_t		req_running_q;
	/* Lock to protect requests list*/
	spinlock_t			request_lock;
	struct list_head		requests;
	struct list_head		async_requests;
	int				max_credits;
	int				credits_granted;
	char				peeraddr[MAX_ADDRBUFLEN];
	int				connection_type;
	struct cifsd_stats		stats;
#ifdef CONFIG_CIFS_SMB2_SERVER
	char				ClientGUID[SMB2_CLIENT_GUID_SIZE];
#endif
	struct cifsd_secmech		secmech;
	union {
		/* pending trans request table */
		struct trans_state	*recent_trans;
		/* Used by ntlmssp */
		char			*ntlmssp_cryptkey;
	};

	struct preauth_integrity_info	*preauth_info;

	/* Supports NTLMSSP */
	bool				sec_ntlmssp;
	/* Supports U2U Kerberos */
	bool				sec_kerberosu2u;
	/* Supports plain Kerberos */
	bool				sec_kerberos;
	/* Supports legacy MS Kerberos */
	bool				sec_mskerberos;
	bool				sign;
	bool				need_neg;
	bool				use_spnego:1;
	__u16				cli_sec_mode;
	__u16				srv_sec_mode;
	/* dialect index that server chose */
	__u16				dialect;

	char				*mechToken;

	struct cifsd_tcp_conn_ops	*conn_ops;
};

void cifsd_tcp_conn_lock(struct cifsd_tcp_conn *conn);
void cifsd_tcp_conn_unlock(struct cifsd_tcp_conn *conn);
void cifsd_tcp_conn_wait_idle(struct cifsd_tcp_conn *conn);

int cifsd_tcp_for_each_conn(int (*match)(struct cifsd_tcp_conn *, void *),
	void *arg);
int cifsd_tcp_read(struct cifsd_tcp_conn *conn,
		   char *buf,
		   unsigned int to_read);
struct cifsd_work;
int cifsd_tcp_write(struct cifsd_work *work);

void cifsd_tcp_enqueue_request(struct cifsd_work *work);
void cifsd_tcp_init_server_callbacks(struct cifsd_tcp_conn_ops *ops);

void cifsd_tcp_destroy(void);
int cifsd_tcp_init(void);

/*
 * WARNING
 *
 * This is a hack. We will move status to a proper place once we land
 * a multi-sessions support.
 */
static inline bool cifsd_tcp_good(struct cifsd_work *work)
{
	return work->conn->tcp_status == CIFSD_SESS_GOOD;
}

static inline bool cifsd_tcp_need_negotiate(struct cifsd_work *work)
{
	return work->conn->tcp_status == CIFSD_SESS_NEED_NEGOTIATE;
}

static inline bool cifsd_tcp_need_reconnect(struct cifsd_work *work)
{
	return work->conn->tcp_status == CIFSD_SESS_NEED_RECONNECT;
}

static inline bool cifsd_tcp_exiting(struct cifsd_work *work)
{
	return work->conn->tcp_status == CIFSD_SESS_EXITING;
}

static inline void cifsd_tcp_set_good(struct cifsd_work *work)
{
	work->conn->tcp_status = CIFSD_SESS_GOOD;
}

static inline void cifsd_tcp_set_need_negotiate(struct cifsd_work *work)
{
	work->conn->tcp_status = CIFSD_SESS_NEED_NEGOTIATE;
}

static inline void cifsd_tcp_set_need_reconnect(struct cifsd_work *work)
{
	work->conn->tcp_status = CIFSD_SESS_NEED_RECONNECT;
}

static inline void cifsd_tcp_set_exiting(struct cifsd_work *work)
{
	work->conn->tcp_status = CIFSD_SESS_EXITING;
}
#endif /* __CIFSD_TRANSPORT_TCP_H__ */
