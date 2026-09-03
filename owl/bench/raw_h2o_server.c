#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <h2o.h>

#define PLAINTEXT_BODY "Hello, World!"
#define JSON_BODY "{\"message\":\"Hello, World!\"}"

static h2o_globalconf_t config;
static h2o_context_t ctx;
static h2o_accept_ctx_t accept_ctx;

static int on_req(h2o_handler_t *self, h2o_req_t *req) {
    (void)self;
    if (!h2o_memis(req->method.base, req->method.len, H2O_STRLIT("GET"))) return -1;

    const char *body;
    const char *ctype;
    size_t body_len;
    size_t ctype_len;
    if (h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT("/plaintext"))) {
        body = PLAINTEXT_BODY;
        body_len = sizeof(PLAINTEXT_BODY) - 1;
        ctype = "text/plain; charset=utf-8";
        ctype_len = strlen(ctype);
    } else if (h2o_memis(req->path_normalized.base, req->path_normalized.len, H2O_STRLIT("/json"))) {
        body = JSON_BODY;
        body_len = sizeof(JSON_BODY) - 1;
        ctype = "application/json";
        ctype_len = sizeof("application/json") - 1;
    } else {
        return -1;
    }

    static h2o_generator_t generator = {NULL, NULL};
    req->res.status = 200;
    req->res.reason = "OK";
    req->res.content_length = body_len;
    h2o_add_header(&req->pool, &req->res.headers, H2O_TOKEN_CONTENT_TYPE, NULL, ctype, ctype_len);
    h2o_start_response(req, &generator);
    h2o_iovec_t buf = h2o_iovec_init(body, body_len);
    h2o_send(req, &buf, 1, H2O_SEND_STATE_FINAL);
    return 0;
}

static void on_accept(h2o_socket_t *listener, const char *err) {
    if (err != NULL) return;
    h2o_socket_t *sock = h2o_evloop_socket_accept(listener);
    if (sock == NULL) return;
    h2o_accept(&accept_ctx, sock);
}

int main(int argc, char **argv) {
    const uint16_t port = argc > 1 ? (uint16_t)atoi(argv[1]) : 8081;

    signal(SIGPIPE, SIG_IGN);

    h2o_config_init(&config);
    h2o_hostconf_t *host = h2o_config_register_host(&config, h2o_iovec_init(H2O_STRLIT("default")), 65535);
    h2o_pathconf_t *pathconf = h2o_config_register_path(host, "/", 0);
    h2o_handler_t *handler = h2o_create_handler(pathconf, sizeof(*handler));
    handler->on_req = on_req;

    h2o_context_init(&ctx, h2o_evloop_create(), &config);
    accept_ctx.ctx = &ctx;
    accept_ctx.hosts = config.hosts;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1) return 1;

    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    const int on = 1;
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0 ||
        bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(fd, SOMAXCONN) != 0) {
        perror("listen");
        return 1;
    }

    h2o_socket_t *listener = h2o_evloop_socket_create(ctx.loop, fd, H2O_SOCKET_FLAG_DONT_READ);
    h2o_socket_read_start(listener, on_accept);

    printf("raw-h2o listening on http://127.0.0.1:%u (/plaintext, /json)\n", port);
    fflush(stdout);
    for (;;) h2o_evloop_run(ctx.loop, INT32_MAX);
}
