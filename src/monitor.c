#include "pgrouter.h"
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>

static int writef(int fd, const char *fmt, ...)
{
	int n, wr;
	char *buf;
	va_list ap;

	va_start(ap, fmt);
	n = vasprintf(&buf, fmt, ap);
	va_end(ap);

	if (n > 0 && buf[n - 1] == '\n') {
		buf[n-1] = '.';
		pgr_logf(stderr, LOG_DEBUG, "[monitor] writing %d/%d bytes to fd %d [%s]", n, n, fd, buf);
		buf[n-1] = '\n';
	} else {
		pgr_logf(stderr, LOG_DEBUG, "[monitor] writing %d/%d bytes to fd %d [%s]", n, n, fd, buf);
	}
	wr = 0;
	while ((wr = write(fd, buf + wr, n)) > 0) {
		pgr_logf(stderr, LOG_DEBUG, "[monitor]   wrote %d/%d bytes to fd %d (%d remain)",
			wr, n, fd, n - wr);
		n -= wr;
	}
	if (wr < 0) {
		return wr;
	}
	return n;
}

static int rdlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[monitor] acquiring %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_rdlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to acquire %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static int unlock(pthread_rwlock_t *l, const char *what, int idx)
{
	pgr_logf(stderr, LOG_DEBUG, "[monitor] releasing %s/%d read lock %p", what, idx, l);
	int rc = pthread_rwlock_unlock(l);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to release the %s/%d read lock: %s (errno %d)",
				what, idx, strerror(rc), rc);
		return rc;
	}
	return 0;
}

static void* do_monitor(void *_c)
{
	CONTEXT *c = (CONTEXT*)_c;
	int rc, connfd, i;
	/* FIXME: we should pass a new sockaddr to accept() and log about remote clients */
	/* FIXME: handle both ipv4 and ipv6 */
	while ((connfd = accept(c->monitor4, NULL, NULL)) != -1) {

		rc = rdlock(&c->lock, "context", 0);
		if (rc != 0) {
			close(connfd);
			break;
		}

		writef(connfd, "backends %d/%d\n", c->ok_backends, c->num_backends);
		writef(connfd, "workers %d\n", c->workers);
		writef(connfd, "clients ??\n"); /* FIXME: get real data */
		writef(connfd, "connections ??\n"); /* FIXME: get real data */

		for (i = 0; i < c->num_backends; i++) {
			rc = rdlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				break;
			}

			if (c->backends[i].status == BACKEND_IS_OK) {
				writef(connfd, "%s:%d %s OK %llu/%llu\n",
						c->backends[i].hostname, c->backends[i].port,
						(c->backends[i].master ? "master" : "slave"),
						c->backends[i].health.lag,
						c->backends[i].health.threshold);
			} else {
				writef(connfd, "%s:%d %s %s\n",
						c->backends[i].hostname, c->backends[i].port,
						(c->backends[i].master ? "master" : "slave"),
						(c->backends[i].status == BACKEND_IS_STARTING ? "STARTING" :
						 c->backends[i].status == BACKEND_IS_FAILED   ? "FAILED"   : "UNKNOWN"));
			}

			rc = unlock(&c->backends[i].lock, "backend", i);
			if (rc != 0) {
				break;
			}
		}

		rc = unlock(&c->lock, "context", 0);
		if (rc != 0) {
			close(connfd);
			break;
		}

		close(connfd);
	}

	close(c->monitor4);
	return NULL;
}

int pgr_monitor(CONTEXT *c, pthread_t *tid)
{
	int rc = pthread_create(tid, NULL, do_monitor, c);
	if (rc != 0) {
		pgr_logf(stderr, LOG_ERR, "[monitor] failed to spin up: %s (errno %d)",
				strerror(errno), errno);
		return;
	}

	pgr_logf(stderr, LOG_INFO, "[monitor] spinning up [tid=%i]", *tid);
}
