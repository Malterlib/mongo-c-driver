/*
 * Copyright 2009-present MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <mongoc/mongoc-http-private.h>
#include <mongoc/mongoc-interrupt-private.h>
#include <mongoc/mongoc-socket-private.h>

#include <common-thread-private.h>

#include <mongoc/mongoc.h>

#include <mlib/duration.h>
#include <mlib/test.h>
#include <mlib/time_point.h>
#include <mlib/timer.h>

#include <TestSuite.h>
#include <test-libmongoc.h>

void
test_mongoc_http_get(void *unused)
{
   mongoc_http_request_t req;
   mongoc_http_response_t res;
   bool r;
   bson_error_t error = {0};

   BSON_UNUSED(unused);

   _mongoc_http_request_init(&req);
   _mongoc_http_response_init(&res);

   /* Basic GET request */
   req.method = "GET";
   req.host = "localhost";
   req.path = "get";
   req.port = 18000;
   r = _mongoc_http_send(&req, mlib_expires_after(mlib_duration(10, s)), false, NULL, &res, &error);
   ASSERT_OR_PRINT(r, error);

   ASSERT_WITH_MSG(res.status == 200,
                   "unexpected status code %d\n"
                   "RESPONSE BODY BEGIN\n"
                   "%s"
                   "RESPONSE BODY END\n",
                   res.status,
                   res.body_len > 0 ? res.body : "");
   ASSERT_CMPINT(res.body_len, >, 0);
   _mongoc_http_response_cleanup(&res);
}

void
test_mongoc_http_post(void *unused)
{
   mongoc_http_request_t req;
   mongoc_http_response_t res;
   bool r;
   bson_error_t error = {0};

   BSON_UNUSED(unused);

   _mongoc_http_request_init(&req);
   _mongoc_http_response_init(&res);

   /* Basic POST request with a body. */
   req.method = "POST";
   req.host = "localhost";
   req.path = "post";
   req.port = 18000;
   r = _mongoc_http_send(&req, mlib_expires_after(mlib_duration(10, s)), false, NULL, &res, &error);
   ASSERT_OR_PRINT(r, error);

   ASSERT_WITH_MSG(res.status == 200,
                   "unexpected status code %d\n"
                   "RESPONSE BODY BEGIN\n"
                   "%s"
                   "RESPONSE BODY END\n",
                   res.status,
                   res.body_len > 0 ? res.body : "");
   ASSERT_CMPINT(res.body_len, >, 0);
   _mongoc_http_response_cleanup(&res);
}

static void
_init_valid_req(mongoc_http_request_t *req)
{
   _mongoc_http_request_init(req);
   req->method = "GET";
   req->host = "localhost";
   req->path = "/foo";
   req->port = 80;
}

static void
test_mongoc_http_validate(void)
{
   mongoc_http_request_t req;
   bson_error_t error;

   /* baseline: a well-formed request passes */
   _init_valid_req(&req);
   mlib_check(_mongoc_http_request_validate(&req, &error));

   /* method: empty */
   _init_valid_req(&req);
   req.method = "";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "method");

   /* method: contains CR */
   _init_valid_req(&req);
   req.method = "GET\r";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "method");

   /* method: contains LF */
   _init_valid_req(&req);
   req.method = "GET\n";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "method");

   /* method: contains space (would split the request line) */
   _init_valid_req(&req);
   req.method = "GE T";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "method");

   /* host: empty */
   _init_valid_req(&req);
   req.host = "";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "host");

   /* host: CRLF injection */
   _init_valid_req(&req);
   req.host = "evil.com\r\nInjected: header";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "host");

   /* path: CRLF injection */
   _init_valid_req(&req);
   req.path = "/legit\r\nInjected: header";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "path");

   /* path: bare LF injection */
   _init_valid_req(&req);
   req.path = "/legit\nInjected";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "path");

   /* path: space splits the request line (GET /le git HTTP/1.0) */
   _init_valid_req(&req);
   req.path = "/le git";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "path");

   /* path: NULL is allowed (defaults to /) */
   _init_valid_req(&req);
   req.path = NULL;
   mlib_check(_mongoc_http_request_validate(&req, &error));

   /* extra_headers: well-formed headers pass */
   _init_valid_req(&req);
   req.extra_headers = "X-Foo: bar\r\nX-Baz: qux\r\n";
   mlib_check(_mongoc_http_request_validate(&req, &error));

   /* extra_headers: blank line terminates headers, allowing body injection */
   _init_valid_req(&req);
   req.extra_headers = "X-Foo: bar\r\n\r\nINJECTED BODY";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "extra_headers");

   /* extra_headers: leading CRLF combines with the preceding header's terminator to end headers early */
   _init_valid_req(&req);
   req.extra_headers = "\r\nX-Injected: header";
   mlib_check(!_mongoc_http_request_validate(&req, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_INVALID_STATE, "extra_headers");

   /* extra_headers: NULL is allowed */
   _init_valid_req(&req);
   req.extra_headers = NULL;
   mlib_check(_mongoc_http_request_validate(&req, &error));
}

typedef struct {
   mongoc_socket_t *listen_sock;
   mongoc_interrupt_t *interrupt;
} cancel_partial_ctx_t;

// Sends the headers of a 200 response and the start of its body, then cancels the client while it waits for the rest
static BSON_THREAD_FUN(_cancel_partial_server, arg)
{
   cancel_partial_ctx_t *ctx = arg;
   const char partial[] = "HTTP/1.1 200 OK\r\nContent-Length: 1000\r\n\r\ntruncated";
   char buf[1024];
   size_t request_len = 0;

   mongoc_socket_t *conn_sock = mongoc_socket_accept(ctx->listen_sock, bson_get_monotonic_time() + 10 * 1000 * 1000);
   BSON_ASSERT(conn_sock);

   // The whole request, so the client is done writing and the cancellation can only end its read
   while (request_len < 4 || memcmp(buf + request_len - 4, "\r\n\r\n", 4) != 0) {
      BSON_ASSERT(request_len < sizeof buf);
      const ssize_t got = mongoc_socket_recv(
         conn_sock, buf + request_len, sizeof buf - request_len, 0, bson_get_monotonic_time() + 10 * 1000 * 1000);
      BSON_ASSERT(got > 0);
      request_len += (size_t)got;
   }

   BSON_ASSERT(mongoc_socket_send(conn_sock, partial, sizeof partial - 1, -1) == (ssize_t)(sizeof partial - 1));

   // Lets the client read the partial response before it is cancelled. That can't be observed from here, so a client
   // that is cancelled first fails without having read it, which still fails as cancelled.
   mlib_sleep_for(200, ms);
   _mongoc_interrupt_cancel(ctx->interrupt);

   // Until the client closes, so the close isn't a reset that ends its read first
   while (mongoc_socket_recv(conn_sock, buf, sizeof buf, 0, bson_get_monotonic_time() + 10 * 1000 * 1000) > 0) {
   }

   mongoc_socket_destroy(conn_sock);
   BSON_THREAD_RETURN;
}

static void
test_mongoc_http_cancel_partial_response(void)
{
   cancel_partial_ctx_t ctx = {0};
   struct sockaddr_in server_addr = {0};
   mongoc_socklen_t sock_len = sizeof server_addr;
   bson_thread_t thread;
   mongoc_http_request_t req;
   mongoc_http_response_t res;
   bson_error_t error = {0};

   ctx.listen_sock = mongoc_socket_new(AF_INET, SOCK_STREAM, 0, NULL);
   BSON_ASSERT(ctx.listen_sock);
   server_addr.sin_family = AF_INET;
   server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
   ASSERT_CMPINT(mongoc_socket_bind(ctx.listen_sock, (struct sockaddr *)&server_addr, sizeof server_addr), ==, 0);
   ASSERT_CMPINT(mongoc_socket_getsockname(ctx.listen_sock, (struct sockaddr *)&server_addr, &sock_len), ==, 0);
   ASSERT_CMPINT(mongoc_socket_listen(ctx.listen_sock, 10), ==, 0);
   ctx.interrupt = _mongoc_interrupt_new(10000);
   BSON_ASSERT(ctx.interrupt);
   ASSERT_CMPINT(mcommon_thread_create(&thread, _cancel_partial_server, &ctx), ==, 0);

   _mongoc_http_request_init(&req);
   _mongoc_http_response_init(&res);
   req.method = "GET";
   req.host = "127.0.0.1";
   req.path = "token";
   req.port = ntohs(server_addr.sin_port);
   mlib_check(!_mongoc_http_send_interruptible(
      &req, mlib_expires_after(mlib_duration(10, s)), false, NULL, ctx.interrupt, &res, &error));
   ASSERT_ERROR_CONTAINS(error, MONGOC_ERROR_STREAM, MONGOC_ERROR_STREAM_SOCKET, "cancelled");

   mcommon_thread_join(thread);
   _mongoc_http_response_cleanup(&res);
   _mongoc_interrupt_destroy(ctx.interrupt);
   mongoc_socket_destroy(ctx.listen_sock);
}

void
test_http_install(TestSuite *suite)
{
   TestSuite_Add(suite, "/http/validate", test_mongoc_http_validate);
   TestSuite_Add(suite, "/http/cancel_partial_response", test_mongoc_http_cancel_partial_response);
   TestSuite_AddFull(suite,
                     "/http/get [uses:simple-http-server-18000]",
                     test_mongoc_http_get,
                     NULL /* dtor */,
                     NULL /* ctx */,
                     test_framework_skip_if_offline);

   TestSuite_AddFull(suite,
                     "/http/post [uses:simple-http-server-18000]",
                     test_mongoc_http_post,
                     NULL /* dtor */,
                     NULL /* ctx */,
                     test_framework_skip_if_offline);
}
