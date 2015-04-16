// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2014 Red Hat
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#ifndef CEPH_OS_FS_H
#define CEPH_OS_FS_H

#include <errno.h>
#include <time.h>

#include "acconfig.h"
#ifdef HAVE_LIBAIO
# include <libaio.h>
#endif

#include <string>

#include "include/types.h"
#include "common/Mutex.h"
#include "common/Cond.h"

class FS {
public:
  virtual ~FS() { }

  static FS *create(uint64_t f_type);
  static FS *create_by_fd(int fd);

  virtual const char *get_name() {
    return "generic";
  }

  virtual int set_alloc_hint(int fd, uint64_t hint);

  virtual int get_handle(int fd, std::string *h);
  virtual int open_handle(int mount_fd, const std::string& h, int flags);

  virtual int copy_file_range(int to_fd, uint64_t to_offset,
			      int from_fd,
			      uint64_t from_offset, uint64_t from_len);
  virtual int zero(int fd, uint64_t offset, uint64_t length);

  // -- aio --

  struct aio_t {
    struct iocb iocb;  // must be first element; see shenanigans in aio_queue_t
    void *priv;
    int fd;
    vector<iovec> iov;

    aio_t(void *p, int f) : priv(p), fd(f) {
      memset(&iocb, 0, sizeof(iocb));
    }

    void pwritev(uint64_t offset) {
      io_prep_pwritev(&iocb, fd, &iov[0], iov.size(), offset);
    }

    bool empty() const {
      return iov.empty();
    }
  };

  struct aio_queue_t {
    io_context_t ctx;
    Mutex lock;
    Cond cond;
    atomic_t num;

    aio_queue_t() : ctx(0), lock("FS::aio_queue_t::lock") {
      int r = io_setup(128, &ctx);
      assert(r == 0);
    }
    ~aio_queue_t() {
      int r = io_destroy(ctx);
      assert(r == 0);
    }

    int submit(aio_t &aio) {
      int attempts = 10;
      iocb *piocb = &aio.iocb;
      do {
	int r = io_submit(ctx, 1, &piocb);
	if (r < 0) {
	  if (r == -EAGAIN && attempts-- > 0) {
	    usleep(500);
	    continue;
	  }
	  return r;
	}
	int was = num.inc();
	if (was == 0) {
	  Mutex::Locker l(lock);
	  cond.Signal();
	}
      } while (false);
      return 0;
    }

    int get_next_completed(int timeout_ms, aio_t **paio) {
      if (num.read() == 0) {
	Mutex::Locker l(lock);
	utime_t t;
	t.set_from_double((double)timeout_ms / 1000.0);
	cond.WaitInterval(NULL, lock, t);
	if (num.read() == 0) {
	  return 0;
	}
      }
      io_event event[1];
      struct timespec t = {
	timeout_ms / 1000,
	(timeout_ms % 1000) * 1000 * 1000
      };
      int r = io_getevents(ctx, 1, 1, event, &t);
      if (r <= 0) {
	return r;
      }
      *paio = (aio_t *)event[0].obj;
      return 1;
    }
  };

};

#endif
