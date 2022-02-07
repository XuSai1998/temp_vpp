/*
 * Copyright (c) 2017-2022 Cisco and/or its affiliates.
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at:
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <http_static/http_static.h>
#include <vppinfra/bihash_template.c>
#include <vppinfra/unix.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/** @file static_server.c
 *  Static http server, sufficient to serve .html / .css / .js content.
 */
/*? %%clicmd:group_label Static HTTP Server %% ?*/

#define HSS_FIFO_THRESH (16 << 10)

hss_main_t hss_main;

static void
hss_cache_lock (void)
{
  clib_spinlock_lock (&hss_main.cache_lock);
}

static void
hss_cache_unlock (void)
{
  clib_spinlock_unlock (&hss_main.cache_lock);
}

static hss_session_t *
hss_session_alloc (u32 thread_index)
{
  hss_main_t *hsm = &hss_main;
  hss_session_t *hs;

  pool_get_zero (hsm->sessions[thread_index], hs);
  hs->session_index = hs - hsm->sessions[thread_index];
  hs->thread_index = thread_index;
  hs->cache_pool_index = ~0;
  return hs;
}

static hss_session_t *
hss_session_get (u32 thread_index, u32 hs_index)
{
  hss_main_t *hsm = &hss_main;
  if (pool_is_free_index (hsm->sessions[thread_index], hs_index))
    return 0;
  return pool_elt_at_index (hsm->sessions[thread_index], hs_index);
}

static void
hss_session_free (hss_session_t *hs)
{
  hss_main_t *hsm = &hss_main;

  pool_put (hsm->sessions[hs->thread_index], hs);

  if (CLIB_DEBUG)
    {
      u32 save_thread_index;
      save_thread_index = hs->thread_index;
      /* Poison the entry, preserve timer state and thread index */
      memset (hs, 0xfa, sizeof (*hs));
      hs->thread_index = save_thread_index;
    }
}

/** \brief Detach cache entry from session
 */
static void
hss_detach_cache_entry (hss_session_t *hs)
{
  hss_main_t *hsm = &hss_main;
  hss_cache_entry_t *ep;

  /*
   * Decrement cache pool entry reference count
   * Note that if e.g. a file lookup fails, the cache pool index
   * won't be set
   */
  if (hs->cache_pool_index != ~0)
    {
      ep = pool_elt_at_index (hsm->cache_pool, hs->cache_pool_index);
      ep->inuse--;
      if (hsm->debug_level > 1)
	clib_warning ("index %d refcnt now %d", hs->cache_pool_index,
		      ep->inuse);
    }
  hs->cache_pool_index = ~0;
  if (hs->free_data)
    vec_free (hs->data);
  hs->data = 0;
  hs->data_offset = 0;
  hs->free_data = 0;
  vec_free (hs->path);
}

/** \brief Disconnect a session
 */
static void
hss_session_disconnect_transport (hss_session_t *hs)
{
  vnet_disconnect_args_t _a = { 0 }, *a = &_a;
  a->handle = hs->vpp_session_handle;
  a->app_index = hss_main.app_index;
  vnet_disconnect_session (a);
}

/** \brief Sanity-check the forward and reverse LRU lists
 */
static inline void
lru_validate (hss_main_t *hsm)
{
#if CLIB_DEBUG > 0
  f64 last_timestamp;
  u32 index;
  int i;
  hss_cache_entry_t *ep;

  last_timestamp = 1e70;
  for (i = 1, index = hsm->first_index; index != ~0;)
    {
      ep = pool_elt_at_index (hsm->cache_pool, index);
      index = ep->next_index;
      /* Timestamps should be smaller (older) as we walk the fwd list */
      if (ep->last_used > last_timestamp)
	{
	  clib_warning ("%d[%d]: last used %.6f, last_timestamp %.6f",
			ep - hsm->cache_pool, i,
			ep->last_used, last_timestamp);
	}
      last_timestamp = ep->last_used;
      i++;
    }

  last_timestamp = 0.0;
  for (i = 1, index = hsm->last_index; index != ~0;)
    {
      ep = pool_elt_at_index (hsm->cache_pool, index);
      index = ep->prev_index;
      /* Timestamps should be larger (newer) as we walk the rev list */
      if (ep->last_used < last_timestamp)
	{
	  clib_warning ("%d[%d]: last used %.6f, last_timestamp %.6f",
			ep - hsm->cache_pool, i,
			ep->last_used, last_timestamp);
	}
      last_timestamp = ep->last_used;
      i++;
    }
#endif
}

/** \brief Remove a data cache entry from the LRU lists
 */
static inline void
lru_remove (hss_main_t *hsm, hss_cache_entry_t *ep)
{
  hss_cache_entry_t *next_ep, *prev_ep;
  u32 ep_index;

  lru_validate (hsm);

  ep_index = ep - hsm->cache_pool;

  /* Deal with list heads */
  if (ep_index == hsm->first_index)
    hsm->first_index = ep->next_index;
  if (ep_index == hsm->last_index)
    hsm->last_index = ep->prev_index;

  /* Fix next->prev */
  if (ep->next_index != ~0)
    {
      next_ep = pool_elt_at_index (hsm->cache_pool, ep->next_index);
      next_ep->prev_index = ep->prev_index;
    }
  /* Fix prev->next */
  if (ep->prev_index != ~0)
    {
      prev_ep = pool_elt_at_index (hsm->cache_pool, ep->prev_index);
      prev_ep->next_index = ep->next_index;
    }
  lru_validate (hsm);
}

/** \brief Add an entry to the LRU lists, tag w/ supplied timestamp
 */
static inline void
lru_add (hss_main_t *hsm, hss_cache_entry_t *ep, f64 now)
{
  hss_cache_entry_t *next_ep;
  u32 ep_index;

  lru_validate (hsm);

  ep_index = ep - hsm->cache_pool;

  /*
   * Re-add at the head of the forward LRU list,
   * tail of the reverse LRU list
   */
  if (hsm->first_index != ~0)
    {
      next_ep = pool_elt_at_index (hsm->cache_pool, hsm->first_index);
      next_ep->prev_index = ep_index;
    }

  ep->prev_index = ~0;

  /* ep now the new head of the LRU forward list */
  ep->next_index = hsm->first_index;
  hsm->first_index = ep_index;

  /* single session case: also the tail of the reverse LRU list */
  if (hsm->last_index == ~0)
    hsm->last_index = ep_index;
  ep->last_used = now;

  lru_validate (hsm);
}

/** \brief Remove and re-add a cache entry from/to the LRU lists
 */
static inline void
lru_update (hss_main_t *hsm, hss_cache_entry_t *ep, f64 now)
{
  lru_remove (hsm, ep);
  lru_add (hsm, ep, now);
}

static void
start_send_data (hss_session_t *hs, http_status_code_t status)
{
  http_msg_t msg;
  session_t *ts;
  int rv;

  ts = session_get (hs->vpp_session_index, hs->thread_index);

  msg.type = HTTP_MSG_REPLY;
  msg.code = status;
  msg.content_type = HTTP_CONTENT_TEXT_HTML;
  msg.data.len = hs->data_len;

  if (hs->data_len > hss_main.use_ptr_thresh)
    {
      msg.data.type = HTTP_MSG_DATA_PTR;
      rv = svm_fifo_enqueue (ts->tx_fifo, sizeof (msg), (u8 *) &msg);
      ASSERT (rv == sizeof (msg));

      uword data = pointer_to_uword (hs->data);
      rv = svm_fifo_enqueue (ts->tx_fifo, sizeof (data), (u8 *) &data);
      ASSERT (rv == sizeof (sizeof (data)));

      goto done;
    }

  msg.data.type = HTTP_MSG_DATA_INLINE;

  rv = svm_fifo_enqueue (ts->tx_fifo, sizeof (msg), (u8 *) &msg);
  ASSERT (rv == sizeof (msg));

  if (!msg.data.len)
    goto done;

  rv = svm_fifo_enqueue (ts->tx_fifo, hs->data_len, hs->data);

  if (rv != hs->data_len)
    {
      hs->data_offset = rv;
      svm_fifo_add_want_deq_ntf (ts->tx_fifo, SVM_FIFO_WANT_DEQ_NOTIF);
    }

done:

  if (svm_fifo_set_event (ts->tx_fifo))
    session_send_io_evt_to_thread (ts->tx_fifo, SESSION_IO_EVT_TX);
}

__clib_export void
hss_session_send_data (hss_url_handler_args_t *args)
{
  hss_session_t *hs;

  hs = hss_session_get (args->sh.thread_index, args->sh.session_index);

  if (hs->data && hs->free_data)
    vec_free (hs->data);

  hs->data = args->data;
  hs->data_len = args->data_len;
  hs->free_data = args->free_vec_data;
  start_send_data (hs, args->sc);
}

static int
try_url_handler (hss_main_t *hsm, hss_session_t *hs, http_req_method_t rt,
		 u8 *request)
{
  http_status_code_t sc = HTTP_STATUS_OK;
  hss_url_handler_args_t args = {};
  uword *p, *url_table;
  int rv;

  if (!hsm->enable_url_handlers || !request)
    return -1;

  /* Look for built-in GET / POST handlers */
  url_table =
    (rt == HTTP_REQ_GET) ? hsm->get_url_handlers : hsm->post_url_handlers;

  p = hash_get_mem (url_table, request);
  if (!p)
    return -1;

  hs->path = 0;
  hs->data_offset = 0;
  hs->cache_pool_index = ~0;

  if (hsm->debug_level > 0)
    clib_warning ("%s '%s'", (rt == HTTP_REQ_GET) ? "GET" : "POST", request);

  args.reqtype = rt;
  args.request = request;
  args.sh.thread_index = hs->thread_index;
  args.sh.session_index = hs->session_index;

  rv = ((hss_url_handler_fn) p[0]) (&args);

  /* Wait for data from handler */
  if (rv == HSS_URL_HANDLER_ASYNC)
    return 0;

  if (rv == HSS_URL_HANDLER_ERROR)
    {
      clib_warning ("builtin handler %llx hit on %s '%s' but failed!", p[0],
		    (rt == HTTP_REQ_GET) ? "GET" : "POST", request);
      sc = HTTP_STATUS_NOT_FOUND;
    }

  hs->data = args.data;
  hs->data_len = args.data_len;
  hs->free_data = args.free_vec_data;

  start_send_data (hs, sc);

  if (!hs->data)
    hss_session_disconnect_transport (hs);

  return 0;
}

static int
handle_request (hss_session_t *hs, http_req_method_t rt, u8 *request)
{
  http_status_code_t sc = HTTP_STATUS_OK;
  hss_main_t *hsm = &hss_main;
  struct stat _sb, *sb = &_sb;
  clib_error_t *error;
  u8 *path;

  if (!try_url_handler (hsm, hs, rt, request))
    return 0;

  if (!hsm->www_root)
    {
      sc = HTTP_STATUS_NOT_FOUND;
      goto done;
    }

  /*
   * Construct the file to open
   * Browsers are capable of sporadically including a leading '/'
   */
  if (!request)
    path = format (0, "%s%c", hsm->www_root, 0);
  else if (request[0] == '/')
    path = format (0, "%s%s%c", hsm->www_root, request, 0);
  else
    path = format (0, "%s/%s%c", hsm->www_root, request, 0);

  if (hsm->debug_level > 0)
    clib_warning ("%s '%s'", (rt == HTTP_REQ_GET) ? "GET" : "POST", path);

  /* Try to find the file. 2x special cases to find index.html */
  if (stat ((char *) path, sb) < 0	/* cant even stat the file */
      || sb->st_size < 20	/* file too small */
      || (sb->st_mode & S_IFMT) != S_IFREG /* not a regular file */ )
    {
      u32 save_length = vec_len (path) - 1;
      /* Try appending "index.html"... */
      _vec_len (path) -= 1;
      path = format (path, "index.html%c", 0);
      if (stat ((char *) path, sb) < 0	/* cant even stat the file */
	  || sb->st_size < 20	/* file too small */
	  || (sb->st_mode & S_IFMT) != S_IFREG /* not a regular file */ )
	{
	  _vec_len (path) = save_length;
	  path = format (path, "/index.html%c", 0);

	  /* Send a redirect, otherwise the browser will confuse itself */
	  if (stat ((char *) path, sb) < 0	/* cant even stat the file */
	      || sb->st_size < 20	/* file too small */
	      || (sb->st_mode & S_IFMT) != S_IFREG /* not a regular file */ )
	    {
	      sc = HTTP_STATUS_NOT_FOUND;
	      goto done;
	    }
	  else
	    {
	      transport_endpoint_t endpoint;
	      transport_proto_t proto;
	      u16 local_port;
	      int print_port = 0;
	      u8 *port_str = 0;
	      session_t *ts;

	      /*
	       * To make this bit work correctly, we need to know our local
	       * IP address, etc. and send it in the redirect...
	       */
	      u8 *redirect;

	      vec_delete (path, vec_len (hsm->www_root) - 1, 0);

	      ts = session_get (hs->vpp_session_index, hs->thread_index);
	      session_get_endpoint (ts, &endpoint, 1 /* is_local */);

	      local_port = clib_net_to_host_u16 (endpoint.port);

	      proto = session_type_transport_proto (ts->session_type);

	      if ((proto == TRANSPORT_PROTO_TCP && local_port != 80)
		  || (proto == TRANSPORT_PROTO_TLS && local_port != 443))
		{
		  print_port = 1;
		  port_str = format (0, ":%u", (u32) local_port);
		}

	      redirect = format (0, "HTTP/1.1 301 Moved Permanently\r\n"
				 "Location: http%s://%U%s%s\r\n\r\n",
				 proto == TRANSPORT_PROTO_TLS ? "s" : "",
				 format_ip46_address, &endpoint.ip,
				 endpoint.is_ip4,
				 print_port ? port_str : (u8 *) "", path);
	      if (hsm->debug_level > 0)
		clib_warning ("redirect: %s", redirect);

	      vec_free (port_str);

	      hs->data = redirect;
	      goto done;
	    }
	}
    }

  /* find or read the file if we haven't done so yet. */
  if (hs->data == 0)
    {
      BVT (clib_bihash_kv) kv;
      hss_cache_entry_t *ce;

      hs->path = path;

      /* First, try the cache */
      kv.key = (u64) hs->path;
      if (BV (clib_bihash_search) (&hsm->name_to_data, &kv, &kv) == 0)
	{
	  if (hsm->debug_level > 1)
	    clib_warning ("lookup '%s' returned %lld", kv.key, kv.value);

	  hss_cache_lock ();

	  /* found the data.. */
	  ce = pool_elt_at_index (hsm->cache_pool, kv.value);
	  hs->data = ce->data;
	  /* Update the cache entry, mark it in-use */
	  lru_update (hsm, ce, vlib_time_now (vlib_get_main ()));
	  hs->cache_pool_index = ce - hsm->cache_pool;
	  ce->inuse++;
	  if (hsm->debug_level > 1)
	    clib_warning ("index %d refcnt now %d", hs->cache_pool_index,
			  ce->inuse);

	  hss_cache_unlock ();
	}
      else
	{
	  hss_cache_lock ();

	  if (hsm->debug_level > 1)
	    clib_warning ("lookup '%s' failed", kv.key, kv.value);
	  /* Need to recycle one (or more cache) entries? */
	  if (hsm->cache_size > hsm->cache_limit)
	    {
	      int free_index = hsm->last_index;

	      while (free_index != ~0)
		{
		  /* pick the LRU */
		  ce = pool_elt_at_index (hsm->cache_pool, free_index);
		  free_index = ce->prev_index;
		  /* Which could be in use... */
		  if (ce->inuse)
		    {
		      if (hsm->debug_level > 1)
			clib_warning ("index %d in use refcnt %d",
				      ce - hsm->cache_pool, ce->inuse);
		    }
		  kv.key = (u64) (ce->filename);
		  kv.value = ~0ULL;
		  if (BV (clib_bihash_add_del) (&hsm->name_to_data, &kv,
						0 /* is_add */ ) < 0)
		    {
		      clib_warning ("LRU delete '%s' FAILED!", ce->filename);
		    }
		  else if (hsm->debug_level > 1)
		    clib_warning ("LRU delete '%s' ok", ce->filename);

		  lru_remove (hsm, ce);
		  hsm->cache_size -= vec_len (ce->data);
		  hsm->cache_evictions++;
		  vec_free (ce->filename);
		  vec_free (ce->data);
		  if (hsm->debug_level > 1)
		    clib_warning ("pool put index %d", ce - hsm->cache_pool);
		  pool_put (hsm->cache_pool, ce);
		  if (hsm->cache_size < hsm->cache_limit)
		    break;
		}
	    }

	  /* Read the file */
	  error = clib_file_contents ((char *) (hs->path), &hs->data);
	  if (error)
	    {
	      clib_warning ("Error reading '%s'", hs->path);
	      clib_error_report (error);
	      sc = HTTP_STATUS_INTERNAL_ERROR;
	      hss_cache_unlock ();
	      goto done;
	    }
	  /* Create a cache entry for it */
	  pool_get_zero (hsm->cache_pool, ce);
	  ce->filename = vec_dup (hs->path);
	  ce->data = hs->data;
	  hs->cache_pool_index = ce - hsm->cache_pool;
	  ce->inuse++;
	  if (hsm->debug_level > 1)
	    clib_warning ("index %d refcnt now %d", hs->cache_pool_index,
			  ce->inuse);
	  lru_add (hsm, ce, vlib_time_now (vlib_get_main ()));
	  kv.key = (u64) vec_dup (hs->path);
	  kv.value = ce - hsm->cache_pool;
	  /* Add to the lookup table */
	  if (hsm->debug_level > 1)
	    clib_warning ("add '%s' value %lld", kv.key, kv.value);

	  if (BV (clib_bihash_add_del) (&hsm->name_to_data, &kv,
					1 /* is_add */ ) < 0)
	    {
	      clib_warning ("BUG: add failed!");
	    }
	  hsm->cache_size += vec_len (ce->data);

	  hss_cache_unlock ();
	}
      hs->data_offset = 0;
    }

done:

  start_send_data (hs, sc);
  if (!hs->data)
    hss_session_disconnect_transport (hs);

  return sc;
}

static int
hss_ts_rx_callback (session_t *ts)
{
  hss_session_t *hs;
  u8 *request = 0;
  http_msg_t msg;
  int rv;

  hs = hss_session_get (ts->thread_index, ts->opaque);

  /* Read the http message header */
  rv = svm_fifo_dequeue (ts->rx_fifo, sizeof (msg), (u8 *) &msg);
  ASSERT (rv == sizeof (msg));

  if (msg.type != HTTP_MSG_REQUEST ||
      (msg.method_type != HTTP_REQ_GET && msg.method_type != HTTP_REQ_POST))
    {
      hs->data = 0;
      start_send_data (hs, HTTP_STATUS_METHOD_NOT_ALLOWED);
      return 0;
    }

  /* Read request */
  if (msg.data.len)
    {
      vec_validate (request, msg.data.len - 1);
      rv = svm_fifo_dequeue (ts->rx_fifo, msg.data.len, request);
      ASSERT (rv == msg.data.len);
    }

  /* Find and send data */
  handle_request (hs, msg.method_type, request);

  vec_free (request);

  return 0;
}

static int
hss_ts_tx_callback (session_t *ts)
{
  hss_session_t *hs;
  u32 to_send;
  int rv;

  hs = hss_session_get (ts->thread_index, ts->opaque);
  if (!hs || !hs->data)
    return 0;

  to_send = hs->data_len - hs->data_offset;
  rv = svm_fifo_enqueue (ts->tx_fifo, to_send, hs->data + hs->data_offset);

  if (rv <= 0)
    {
      svm_fifo_add_want_deq_ntf (ts->tx_fifo, SVM_FIFO_WANT_DEQ_NOTIF);
      return 0;
    }

  if (rv < to_send)
    {
      hs->data_offset += rv;
      svm_fifo_add_want_deq_ntf (ts->tx_fifo, SVM_FIFO_WANT_DEQ_NOTIF);
    }

  if (svm_fifo_set_event (ts->tx_fifo))
    session_send_io_evt_to_thread (ts->tx_fifo, SESSION_IO_EVT_TX);

  return 0;
}

/** \brief Session accept callback
 */
static int
hss_ts_accept_callback (session_t *ts)
{
  hss_session_t *hs;
  u32 thresh;

  hs = hss_session_alloc (ts->thread_index);

  hs->vpp_session_index = ts->session_index;
  hs->vpp_session_handle = session_handle (ts);

  /* The application sets a threshold for it's fifo to get notified when
   * additional data can be enqueued. We want to keep the TX fifo reasonably
   * full, however avoid entering a state where the
   * fifo is full all the time and small chunks of data are being enqueued
   * each time. If the fifo is small (under 16K) we set
   * the threshold to it's size, meaning a notification will be given when the
   * fifo empties.
   */
  thresh = clib_min (svm_fifo_size (ts->tx_fifo), HSS_FIFO_THRESH);
  svm_fifo_set_deq_thresh (ts->tx_fifo, thresh);

  ts->opaque = hs->session_index;
  ts->session_state = SESSION_STATE_READY;
  return 0;
}

static void
hss_ts_disconnect_callback (session_t *ts)
{
  hss_main_t *hsm = &hss_main;
  vnet_disconnect_args_t _a = { 0 }, *a = &_a;

  a->handle = session_handle (ts);
  a->app_index = hsm->app_index;
  vnet_disconnect_session (a);
}

static void
hss_ts_reset_callback (session_t *ts)
{
  hss_main_t *hsm = &hss_main;
  vnet_disconnect_args_t _a = { 0 }, *a = &_a;

  a->handle = session_handle (ts);
  a->app_index = hsm->app_index;
  vnet_disconnect_session (a);
}

static int
hss_ts_connected_callback (u32 app_index, u32 api_context, session_t *ts,
			   session_error_t err)
{
  clib_warning ("called...");
  return -1;
}

static int
hss_add_segment_callback (u32 client_index, u64 segment_handle)
{
  return 0;
}

static void
hss_ts_cleanup (session_t *s, session_cleanup_ntf_t ntf)
{
  hss_session_t *hs;

  if (ntf == SESSION_CLEANUP_TRANSPORT)
    return;

  hs = hss_session_get (s->thread_index, s->opaque);
  if (!hs)
    return;

  hss_detach_cache_entry (hs);
  hss_session_free (hs);
}

static session_cb_vft_t hss_cb_vft = {
  .session_accept_callback = hss_ts_accept_callback,
  .session_disconnect_callback = hss_ts_disconnect_callback,
  .session_connected_callback = hss_ts_connected_callback,
  .add_segment_callback = hss_add_segment_callback,
  .builtin_app_rx_callback = hss_ts_rx_callback,
  .builtin_app_tx_callback = hss_ts_tx_callback,
  .session_reset_callback = hss_ts_reset_callback,
  .session_cleanup_callback = hss_ts_cleanup,
};

static int
hss_attach ()
{
  vnet_app_add_cert_key_pair_args_t _ck_pair, *ck_pair = &_ck_pair;
  hss_main_t *hsm = &hss_main;
  u64 options[APP_OPTIONS_N_OPTIONS];
  vnet_app_attach_args_t _a, *a = &_a;
  u32 segment_size = 128 << 20;

  clib_memset (a, 0, sizeof (*a));
  clib_memset (options, 0, sizeof (options));

  if (hsm->private_segment_size)
    segment_size = hsm->private_segment_size;

  a->api_client_index = ~0;
  a->name = format (0, "http_static_server");
  a->session_cb_vft = &hss_cb_vft;
  a->options = options;
  a->options[APP_OPTIONS_SEGMENT_SIZE] = segment_size;
  a->options[APP_OPTIONS_ADD_SEGMENT_SIZE] = segment_size;
  a->options[APP_OPTIONS_RX_FIFO_SIZE] =
    hsm->fifo_size ? hsm->fifo_size : 8 << 10;
  a->options[APP_OPTIONS_TX_FIFO_SIZE] =
    hsm->fifo_size ? hsm->fifo_size : 32 << 10;
  a->options[APP_OPTIONS_FLAGS] = APP_OPTIONS_FLAGS_IS_BUILTIN;
  a->options[APP_OPTIONS_PREALLOC_FIFO_PAIRS] = hsm->prealloc_fifos;
  a->options[APP_OPTIONS_TLS_ENGINE] = CRYPTO_ENGINE_OPENSSL;

  if (vnet_application_attach (a))
    {
      vec_free (a->name);
      clib_warning ("failed to attach server");
      return -1;
    }
  vec_free (a->name);
  hsm->app_index = a->app_index;

  clib_memset (ck_pair, 0, sizeof (*ck_pair));
  ck_pair->cert = (u8 *) test_srv_crt_rsa;
  ck_pair->key = (u8 *) test_srv_key_rsa;
  ck_pair->cert_len = test_srv_crt_rsa_len;
  ck_pair->key_len = test_srv_key_rsa_len;
  vnet_app_add_cert_key_pair (ck_pair);
  hsm->ckpair_index = ck_pair->index;

  return 0;
}

static int
hss_transport_needs_crypto (transport_proto_t proto)
{
  return proto == TRANSPORT_PROTO_TLS || proto == TRANSPORT_PROTO_DTLS ||
	 proto == TRANSPORT_PROTO_QUIC;
}

static int
hss_listen (void)
{
  hss_main_t *hsm = &hss_main;
  session_endpoint_cfg_t sep = SESSION_ENDPOINT_CFG_NULL;
  vnet_listen_args_t _a, *a = &_a;
  char *uri = "tcp://0.0.0.0/80";
  u8 need_crypto;
  int rv;

  clib_memset (a, 0, sizeof (*a));
  a->app_index = hsm->app_index;

  if (hsm->uri)
    uri = (char *) hsm->uri;

  if (parse_uri (uri, &sep))
    return -1;

  need_crypto = hss_transport_needs_crypto (a->sep_ext.transport_proto);

  sep.transport_proto = TRANSPORT_PROTO_HTTP;
  clib_memcpy (&a->sep_ext, &sep, sizeof (sep));

  if (need_crypto)
    {
      session_endpoint_alloc_ext_cfg (&a->sep_ext,
				      TRANSPORT_ENDPT_EXT_CFG_CRYPTO);
      a->sep_ext.ext_cfg->crypto.ckpair_index = hsm->ckpair_index;
    }

  rv = vnet_listen (a);

  if (need_crypto)
    clib_mem_free (a->sep_ext.ext_cfg);

  return rv;
}

static void
hss_url_handlers_init (hss_main_t *hsm)
{
  if (!hsm->get_url_handlers)
    {
      hsm->get_url_handlers = hash_create_string (0, sizeof (uword));
      hsm->post_url_handlers = hash_create_string (0, sizeof (uword));
    }

  hss_builtinurl_json_handlers_init ();
}

int
hss_create (vlib_main_t *vm)
{
  vlib_thread_main_t *vtm = vlib_get_thread_main ();
  hss_main_t *hsm = &hss_main;
  u32 num_threads;

  num_threads = 1 /* main thread */  + vtm->n_threads;
  vec_validate (hsm->sessions, num_threads - 1);

  clib_spinlock_init (&hsm->cache_lock);

  if (hss_attach ())
    {
      clib_warning ("failed to attach server");
      return -1;
    }
  if (hss_listen ())
    {
      clib_warning ("failed to start listening");
      return -1;
    }

  /* Init path-to-cache hash table */
  BV (clib_bihash_init) (&hsm->name_to_data, "http cache", 128, 32 << 20);

  if (hsm->enable_url_handlers)
    hss_url_handlers_init (hsm);

  return 0;
}

static clib_error_t *
hss_create_command_fn (vlib_main_t *vm, unformat_input_t *input,
		       vlib_cli_command_t *cmd)
{
  unformat_input_t _line_input, *line_input = &_line_input;
  hss_main_t *hsm = &hss_main;
  clib_error_t *error = 0;
  u64 seg_size;
  int rv;

  if (hsm->app_index != (u32) ~0)
    return clib_error_return (0, "http server already running...");

  hsm->prealloc_fifos = 0;
  hsm->private_segment_size = 0;
  hsm->fifo_size = 0;
  /* 10mb cache limit, before LRU occurs */
  hsm->cache_limit = 10 << 20;

  /* Get a line of input. */
  if (!unformat_user (input, unformat_line_input, line_input))
    goto no_input;

  while (unformat_check_input (line_input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (line_input, "www-root %s", &hsm->www_root))
	;
      else
	if (unformat (line_input, "prealloc-fifos %d", &hsm->prealloc_fifos))
	;
      else if (unformat (line_input, "private-segment-size %U",
			 unformat_memory_size, &seg_size))
	hsm->private_segment_size = seg_size;
      else if (unformat (line_input, "fifo-size %d", &hsm->fifo_size))
	hsm->fifo_size <<= 10;
      else if (unformat (line_input, "cache-size %U", unformat_memory_size,
			 &hsm->cache_limit))
	;
      else if (unformat (line_input, "uri %s", &hsm->uri))
	;
      else if (unformat (line_input, "debug %d", &hsm->debug_level))
	;
      else if (unformat (line_input, "debug"))
	hsm->debug_level = 1;
      else if (unformat (line_input, "ptr-thresh %U", unformat_memory_size,
			 &hsm->use_ptr_thresh))
	;
      else if (unformat (line_input, "url-handlers"))
	hsm->enable_url_handlers = 1;
      else
	{
	  error = clib_error_return (0, "unknown input `%U'",
				     format_unformat_error, line_input);
	}
    }

  unformat_free (line_input);

no_input:

  if (error)
    goto done;

  if (hsm->www_root == 0 && !hsm->enable_url_handlers)
    {
      error = clib_error_return (0, "Must set www-root or url-handlers");
      goto done;
    }

  if (hsm->cache_limit < (128 << 10))
    {
      error = clib_error_return (0, "cache-size must be at least 128kb");
      vec_free (hsm->www_root);
      goto done;
    }

  vnet_session_enable_disable (vm, 1 /* turn on TCP, etc. */ );

  if ((rv = hss_create (vm)))
    {
      error = clib_error_return (0, "server_create returned %d", rv);
      vec_free (hsm->www_root);
    }

done:

  return error;
}

/*?
 * Enable the static http server
 *
 * @cliexpar
 * This command enables the static http server. Only the www-root
 * parameter is required
 * @clistart
 * http static server www-root /tmp/www uri tcp://0.0.0.0/80 cache-size 2m
 * @cliend
 * @cliexcmd{http static server www-root <path> [prealloc-fios <nn>]
 *   [private-segment-size <nnMG>] [fifo-size <nbytes>] [uri <uri>]}
?*/
VLIB_CLI_COMMAND (hss_create_command, static) = {
  .path = "http static server",
  .short_help =
    "http static server www-root <path> [prealloc-fifos <nn>]\n"
    "[private-segment-size <nnMG>] [fifo-size <nbytes>] [uri <uri>]\n"
    "[ptr-thresh <nn>] [url-handlers] [debug [nn]]\n",
  .function = hss_create_command_fn,
};

/** \brief format a file cache entry
 */
static u8 *
format_hss_cache_entry (u8 *s, va_list *args)
{
  hss_cache_entry_t *ep = va_arg (*args, hss_cache_entry_t *);
  f64 now = va_arg (*args, f64);

  /* Header */
  if (ep == 0)
    {
      s = format (s, "%40s%12s%20s", "File", "Size", "Age");
      return s;
    }
  s = format (s, "%40s%12lld%20.2f", ep->filename, vec_len (ep->data),
	      now - ep->last_used);
  return s;
}

static u8 *
format_hss_session (u8 *s, va_list *args)
{
  hss_session_t *hs = va_arg (*args, hss_session_t *);
  int __clib_unused verbose = va_arg (*args, int);

  s = format (s, "\n path %s, data length %u, data_offset %u",
	      hs->path ? hs->path : (u8 *) "[none]", hs->data_len,
	      hs->data_offset);
  return s;
}

static clib_error_t *
hss_show_command_fn (vlib_main_t *vm, unformat_input_t *input,
		     vlib_cli_command_t *cmd)
{
  hss_main_t *hsm = &hss_main;
  hss_cache_entry_t *ep, **entries = 0;
  int verbose = 0;
  int show_cache = 0;
  int show_sessions = 0;
  u32 index;
  f64 now;

  if (hsm->www_root == 0)
    return clib_error_return (0, "Static server disabled");

  while (unformat_check_input (input) != UNFORMAT_END_OF_INPUT)
    {
      if (unformat (input, "verbose %d", &verbose))
	;
      else if (unformat (input, "verbose"))
	verbose = 1;
      else if (unformat (input, "cache"))
	show_cache = 1;
      else if (unformat (input, "sessions"))
	show_sessions = 1;
      else
	break;
    }

  if ((show_cache + show_sessions) == 0)
    return clib_error_return (0, "specify one or more of cache, sessions");

  if (show_cache)
    {
      if (verbose == 0)
	{
	  vlib_cli_output
	    (vm, "www_root %s, cache size %lld bytes, limit %lld bytes, "
	     "evictions %lld",
	     hsm->www_root, hsm->cache_size, hsm->cache_limit,
	     hsm->cache_evictions);
	  return 0;
	}

      now = vlib_time_now (vm);

      vlib_cli_output (vm, "%U", format_hss_cache_entry, 0 /* header */, now);

      for (index = hsm->first_index; index != ~0;)
	{
	  ep = pool_elt_at_index (hsm->cache_pool, index);
	  index = ep->next_index;
	  vlib_cli_output (vm, "%U", format_hss_cache_entry, ep, now);
	}

      vlib_cli_output (vm, "%40s%12lld", "Total Size", hsm->cache_size);

      vec_free (entries);
    }

  if (show_sessions)
    {
      u32 *session_indices = 0;
      hss_session_t *hs;
      int i, j;

      hss_cache_lock ();

      for (i = 0; i < vec_len (hsm->sessions); i++)
	{
	  pool_foreach (hs, hsm->sessions[i])
           {
            vec_add1 (session_indices, hs - hsm->sessions[i]);
          }

	  for (j = 0; j < vec_len (session_indices); j++)
	    {
	      vlib_cli_output (
		vm, "%U", format_hss_session,
		pool_elt_at_index (hsm->sessions[i], session_indices[j]),
		verbose);
	    }
	  vec_reset_length (session_indices);
	}
      hss_cache_unlock ();
      vec_free (session_indices);
    }
  return 0;
}

/*?
 * Display static http server cache statistics
 *
 * @cliexpar
 * This command shows the contents of the static http server cache
 * @clistart
 * show http static server
 * @cliend
 * @cliexcmd{show http static server sessions cache [verbose [nn]]}
?*/
VLIB_CLI_COMMAND (hss_show_command, static) = {
  .path = "show http static server",
  .short_help = "show http static server sessions cache [verbose [<nn>]]",
  .function = hss_show_command_fn,
};

static clib_error_t *
hss_clear_cache_command_fn (vlib_main_t *vm, unformat_input_t *input,
			    vlib_cli_command_t *cmd)
{
  hss_main_t *hsm = &hss_main;
  hss_cache_entry_t *ce;
  u32 free_index;
  u32 busy_items = 0;
  BVT (clib_bihash_kv) kv;

  if (hsm->www_root == 0)
    return clib_error_return (0, "Static server disabled");

  hss_cache_lock ();

  /* Walk the LRU list to find active entries */
  free_index = hsm->last_index;
  while (free_index != ~0)
    {
      ce = pool_elt_at_index (hsm->cache_pool, free_index);
      free_index = ce->prev_index;
      /* Which could be in use... */
      if (ce->inuse)
	{
	  busy_items++;
	  free_index = ce->next_index;
	  continue;
	}
      kv.key = (u64) (ce->filename);
      kv.value = ~0ULL;
      if (BV (clib_bihash_add_del) (&hsm->name_to_data, &kv,
				    0 /* is_add */ ) < 0)
	{
	  clib_warning ("BUG: cache clear delete '%s' FAILED!", ce->filename);
	}

      lru_remove (hsm, ce);
      hsm->cache_size -= vec_len (ce->data);
      hsm->cache_evictions++;
      vec_free (ce->filename);
      vec_free (ce->data);
      if (hsm->debug_level > 1)
	clib_warning ("pool put index %d", ce - hsm->cache_pool);
      pool_put (hsm->cache_pool, ce);
      free_index = hsm->last_index;
    }
  hss_cache_unlock ();
  if (busy_items > 0)
    vlib_cli_output (vm, "Note: %d busy items still in cache...", busy_items);
  else
    vlib_cli_output (vm, "Cache cleared...");
  return 0;
}

/*?
 * Clear the static http server cache, to force the server to
 * reload content from backing files
 *
 * @cliexpar
 * This command clear the static http server cache
 * @clistart
 * clear http static cache
 * @cliend
 * @cliexcmd{clear http static cache}
?*/
VLIB_CLI_COMMAND (clear_hss_cache_command, static) = {
  .path = "clear http static cache",
  .short_help = "clear http static cache",
  .function = hss_clear_cache_command_fn,
};

static clib_error_t *
hss_main_init (vlib_main_t *vm)
{
  hss_main_t *hsm = &hss_main;

  hsm->app_index = ~0;
  hsm->vlib_main = vm;
  hsm->first_index = hsm->last_index = ~0;

  return 0;
}

VLIB_INIT_FUNCTION (hss_main_init);

/*
 * fd.io coding-style-patch-verification: ON
 *
 * Local Variables:
 * eval: (c-set-style "gnu")
 * End:
 */
