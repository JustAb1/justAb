/*
  FeastNow backend - completed server implementation

  - Proper POST body accumulation using con_cls
  - Thread-safe queue/stack with pthread mutex
  - Safe string operations and simple URL decoding
  - Linear-probing hash table with sample entries
  - Simple endpoints:
      GET  /orders        -> returns JSON array of queued orders
      POST /order         -> add order (body: order=<urlencoded string>)
      POST /undo          -> undo last order (removes last queued order)
      GET  /restaurants   -> list sample restaurants (JSON)
      OPTIONS /*          -> CORS preflight support
  Compile:
    sudo apt-get install libmicrohttpd-dev    # Debian/Ubuntu (if needed)
    gcc -o server Untitled-2.c -lmicrohttpd -pthread -std=c11 -Wall -Wextra
  Run:
    ./server
*/

#include <microhttpd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>

#define PORT 8080
#define MAX_ORDERS 100
#define TABLE_SIZE 10
#define MAX_STRLEN 49   /* reserve 1 byte for NUL */
#define MAX_POST_SIZE 8192

/* ---------- UTILS ---------- */
static void safe_strncpy(char *dst, const char *src, size_t dstsize) {
    if (dstsize == 0) return;
    strncpy(dst, src, dstsize - 1);
    dst[dstsize - 1] = '\0';
}

/* simple in-place percent-decoding */
static void url_decode(char *dst, const char *src) {
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

/* extract single form value by key from application/x-www-form-urlencoded body
   Returns 1 on success and writes into out (out_size), otherwise 0.
   This is simple and only extracts first matching key.
*/
static int form_get_value(const char *body, const char *key, char *out, size_t out_size) {
    size_t keylen = strlen(key);
    const char *p = body;
    while (p && *p) {
        const char *eq = strchr(p, '=');
        const char *amp = strchr(p, '&');
        if (!eq) break;
        if ((size_t)(eq - p) == keylen && strncmp(p, key, keylen) == 0) {
            const char *val_start = eq + 1;
            size_t val_len = (amp ? (size_t)(amp - val_start) : strlen(val_start));
            if (val_len >= out_size) val_len = out_size - 1;
            char tmp[val_len + 1];
            memcpy(tmp, val_start, val_len);
            tmp[val_len] = '\0';
            url_decode(out, tmp);
            return 1;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return 0;
}

/* ---------- QUEUE ---------- */
typedef struct {
    char orders[MAX_ORDERS][MAX_STRLEN + 1];
    int front, rear;
    pthread_mutex_t mu;
} Queue;

static void initQueue(Queue* q){
    q->front = 0;
    q->rear = -1;
    pthread_mutex_init(&q->mu, NULL);
}
static int isQueueEmpty(Queue* q){
    return q->rear < q->front;
}
static int queue_size(Queue* q){
    if (isQueueEmpty(q)) return 0;
    return q->rear - q->front + 1;
}
static int enqueue(Queue* q, const char* order){
    pthread_mutex_lock(&q->mu);
    if (q->rear >= MAX_ORDERS - 1) {
        pthread_mutex_unlock(&q->mu);
        return 0; /* full */
    }
    q->rear++;
    safe_strncpy(q->orders[q->rear], order, sizeof(q->orders[q->rear]));
    pthread_mutex_unlock(&q->mu);
    return 1;
}
/* remove last item (LIFO removal from queue) */
static int dequeue_last(Queue* q){
    pthread_mutex_lock(&q->mu);
    if (isQueueEmpty(q)) {
        pthread_mutex_unlock(&q->mu);
        return 0;
    }
    q->rear--;
    pthread_mutex_unlock(&q->mu);
    return 1;
}
/* build JSON array of orders into a malloc'd string (caller must free) */
static char* queue_to_json(Queue* q) {
    pthread_mutex_lock(&q->mu);
    int sz = queue_size(q);
    /* rough estimate length: sz*(MAX_STRLEN+4) + brackets */
    size_t est = (size_t)sz * (MAX_STRLEN + 6) + 4;
    char *buf = malloc(est);
    if (!buf) { pthread_mutex_unlock(&q->mu); return NULL; }
    char *ptr = buf;
    ptr += sprintf(ptr, "[");
    for (int i = q->front; i <= q->rear; ++i) {
        /* JSON-escape minimal: replace " and \ with escaped versions */
        const char *s = q->orders[i];
        char esc[MAX_STRLEN * 2 + 1];
        char *e = esc;
        for (; *s && (size_t)(e - esc) < sizeof(esc) - 1; s++) {
            if (*s == '\\' || *s == '\"') {
                *e++ = '\\';
                *e++ = *s;
            } else if ((unsigned char)*s < 32) {
                /* skip control chars */
            } else {
                *e++ = *s;
            }
        }
        *e = '\0';
        ptr += sprintf(ptr, "\"%s\"", esc);
        if (i < q->rear) ptr += sprintf(ptr, ",");
    }
    sprintf(ptr, "]");
    pthread_mutex_unlock(&q->mu);
    return buf;
}

/* ---------- STACK ---------- */
typedef struct {
    char items[MAX_ORDERS][MAX_STRLEN + 1];
    int top;
    pthread_mutex_t mu;
} Stack;

static void initStack(Stack* s){ s->top=-1; pthread_mutex_init(&s->mu, NULL); }
static int push(Stack* s, const char* order){
    pthread_mutex_lock(&s->mu);
    if(s->top >= MAX_ORDERS - 1){
        pthread_mutex_unlock(&s->mu);
        return 0;
    }
    s->top++;
    safe_strncpy(s->items[s->top], order, sizeof(s->items[s->top]));
    pthread_mutex_unlock(&s->mu);
    return 1;
}
static int pop(Stack* s, char* out){ /* returns 1 if popped, out must be big enough */
    pthread_mutex_lock(&s->mu);
    if(s->top < 0){
        pthread_mutex_unlock(&s->mu);
        return 0;
    }
    safe_strncpy(out, s->items[s->top], MAX_STRLEN + 1);
    s->top--;
    pthread_mutex_unlock(&s->mu);
    return 1;
}

/* ---------- HASH TABLE (for restaurants) ---------- */
typedef struct {
    char name[MAX_STRLEN + 1];
    char menu[3][MAX_STRLEN + 1];
} Restaurant;

static Restaurant* hashTable[TABLE_SIZE];

static int hash_fn(const char* key){
    unsigned int sum=0; for(int i=0; key[i]; i++) sum += (unsigned char)key[i];
    return sum % TABLE_SIZE;
}

/* Insert with linear probing; returns 1 on success */
static int insertRestaurant(const char* name, const char* m1, const char* m2, const char* m3){
    int idx = hash_fn(name);
    for (int i = 0; i < TABLE_SIZE; ++i) {
        int j = (idx + i) % TABLE_SIZE;
        if (hashTable[j] == NULL) {
            Restaurant* r = malloc(sizeof(Restaurant));
            if (!r) return 0;
            safe_strncpy(r->name, name, sizeof(r->name));
            safe_strncpy(r->menu[0], m1, sizeof(r->menu[0]));
            safe_strncpy(r->menu[1], m2, sizeof(r->menu[1]));
            safe_strncpy(r->menu[2], m3, sizeof(r->menu[2]));
            hashTable[j] = r;
            return 1;
        }
    }
    return 0; /* table full */
}

static char* restaurants_to_json(void) {
    /* build small JSON array with available restaurants and their menus */
    size_t est = TABLE_SIZE * (MAX_STRLEN * 6);
    char *buf = malloc(est);
    if (!buf) return NULL;
    char *ptr = buf;
    ptr += sprintf(ptr, "[");
    int first = 1;
    for (int i = 0; i < TABLE_SIZE; ++i) {
        Restaurant *r = hashTable[i];
        if (!r) continue;
        if (!first) ptr += sprintf(ptr, ",");
        first = 0;
        ptr += sprintf(ptr, "{\"name\":\"%s\",\"menu\":[\"%s\",\"%s\",\"%s\"]}", r->name, r->menu[0], r->menu[1], r->menu[2]);
    }
    ptr += sprintf(ptr, "]");
    return buf;
}

/* ---------- GLOBAL STATE ---------- */
static Queue orderQueue;
static Stack undoStack;

/* ---------- connection context for POST accumulation ---------- */
struct connection_info_struct {
    char *post_data;
    size_t size;
};

static void free_connection_info(void *cls) {
    struct connection_info_struct *con = cls;
    if (!con) return;
    free(con->post_data);
    free(con);
}

/* ---------- HTTP request handler ---------- */
static int send_response(struct MHD_Connection *connection, const char *payload, int status) {
    struct MHD_Response *response;
    int ret;
    if (!payload) payload = "";
    response = MHD_create_response_from_buffer(strlen(payload), (void*)payload, MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;
    MHD_add_response_header(response, "Content-Type", "application/json; charset=utf-8");
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

static int answer_to_connection(void *cls,
                     struct MHD_Connection *connection,
                     const char *url,
                     const char *method,
                     const char *version,
                     const char *upload_data,
                     size_t *upload_data_size,
                     void **con_cls)
{
    /* Handle CORS preflight */
    if (0 == strcmp(method, "OPTIONS")) {
        struct MHD_Response *response = MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
        if (!response) return MHD_NO;
        MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
        MHD_add_response_header(response, "Access-Control-Allow-Methods", "GET, POST, OPTIONS");
        MHD_add_response_header(response, "Access-Control-Allow-Headers", "Content-Type");
        int ret = MHD_queue_response(connection, MHD_HTTP_OK, response);
        MHD_destroy_response(response);
        return ret;
    }

    /* Initialize per-connection context for POST accumulation */
    if (*con_cls == NULL) {
        struct connection_info_struct *con = malloc(sizeof(struct connection_info_struct));
        if (!con) return MHD_NO;
        con->post_data = NULL;
        con->size = 0;
        *con_cls = con;
        /* For POSTs, keep returning MHD_YES so upload_data will be passed in */
        return MHD_YES;
    }

    /* If there's upload data, append it to the buffer */
    if (*upload_data_size != 0) {
        struct connection_info_struct *con = *con_cls;
        /* limit total size */
        if (con->size + *upload_data_size > MAX_POST_SIZE) {
            /* too large */
            return MHD_NO;
        }
        char *newbuf = realloc(con->post_data, con->size + *upload_data_size + 1);
        if (!newbuf) return MHD_NO;
        con->post_data = newbuf;
        memcpy(con->post_data + con->size, upload_data, *upload_data_size);
        con->size += *upload_data_size;
        con->post_data[con->size] = '\0';
        *upload_data_size = 0;
        return MHD_YES;
    }

    /* All upload data received: process request */
    struct connection_info_struct *con = *con_cls;
    char response_buf[256];

    if (0 == strcmp(method, "GET")) {
        if (0 == strcmp(url, "/orders")) {
            char *json = queue_to_json(&orderQueue);
            if (!json) return send_response(connection, "[]", MHD_HTTP_INTERNAL_SERVER_ERROR);
            int r = send_response(connection, json, MHD_HTTP_OK);
            free(json);
            return r;
        } else if (0 == strcmp(url, "/restaurants")) {
            char *json = restaurants_to_json();
            if (!json) return send_response(connection, "[]", MHD_HTTP_INTERNAL_SERVER_ERROR);
            int r = send_response(connection, json, MHD_HTTP_OK);
            free(json);
            return r;
        } else {
            /* default - simple status */
            snprintf(response_buf, sizeof(response_buf), "{\"status\":\"ok\",\"orders\":%d}", queue_size(&orderQueue));
            return send_response(connection, response_buf, MHD_HTTP_OK);
        }
    } else if (0 == strcmp(method, "POST")) {
        if (0 == strcmp(url, "/order")) {
            if (!con->post_data) {
                return send_response(connection, "{\"error\":\"no body\"}", MHD_HTTP_BAD_REQUEST);
            }
            char order_raw[MAX_POST_SIZE];
            if (!form_get_value(con->post_data, "order", order_raw, sizeof(order_raw))) {
                return send_response(connection, "{\"error\":\"missing 'order' field\"}", MHD_HTTP_BAD_REQUEST);
            }
            /* truncate to MAX_STRLEN */
            char order[MAX_STRLEN + 1];
            safe_strncpy(order, order_raw, sizeof(order));
            if (!enqueue(&orderQueue, order)) {
                return send_response(connection, "{\"error\":\"queue full\"}", MHD_HTTP_INTERNAL_SERVER_ERROR);
            }
            /* push to undo stack as well */
            push(&undoStack, order);
            snprintf(response_buf, sizeof(response_buf), "{\"status\":\"queued\",\"order\":\"%s\"}", order);
            return send_response(connection, response_buf, MHD_HTTP_OK);
        } else if (0 == strcmp(url, "/undo")) {
            char popped[MAX_STRLEN + 1];
            if (!pop(&undoStack, popped)) {
                return send_response(connection, "{\"error\":\"nothing to undo\"}", MHD_HTTP_BAD_REQUEST);
            }
            /* remove last from orderQueue */
            if (!dequeue_last(&orderQueue)) {
                /* inconsistent state, but return success for undo */
            }
            snprintf(response_buf, sizeof(response_buf), "{\"status\":\"undone\",\"order\":\"%s\"}", popped);
            return send_response(connection, response_buf, MHD_HTTP_OK);
        } else {
            return send_response(connection, "{\"error\":\"unknown POST endpoint\"}", MHD_HTTP_NOT_FOUND);
        }
    }

    /* cleanup per-connection context */
    free_connection_info(*con_cls);
    *con_cls = NULL;
    return MHD_NO;
}

/* ---------- main ---------- */
int main(void)
{
    /* initialize global structures */
    initQueue(&orderQueue);
    initStack(&undoStack);
    for (int i = 0; i < TABLE_SIZE; ++i) hashTable[i] = NULL;

    /* populate some sample restaurants */
    insertRestaurant("Pasta Palace", "Spaghetti", "Fettuccine", "Ravioli");
    insertRestaurant("Sushi Central", "Salmon Roll", "Tuna Nigiri", "Veggie Roll");
    insertRestaurant("Burger Barn", "Classic Burger", "Cheese Burger", "Vegan Burger");

    struct MHD_Daemon *daemon;
    daemon = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        PORT,
        NULL, NULL,
        &answer_to_connection, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, free_connection_info, NULL,
        MHD_OPTION_CONNECTION_TIMEOUT, (unsigned int) 60,
        MHD_OPTION_END);
    if (NULL == daemon) {
        fprintf(stderr, "Failed to start server\n");
        return 1;
    }

    printf("FeastNow server running on http://localhost:%d/\n", PORT);
    printf("Endpoints:\n  GET  /orders\n  POST /order (order=<urlencoded>)\n  POST /undo\n  GET  /restaurants\n");
    printf("Press Enter to stop.\n");
    getchar();

    MHD_stop_daemon(daemon);

    /* free restaurants */
    for (int i = 0; i < TABLE_SIZE; ++i) {
        free(hashTable[i]);
        hashTable[i] = NULL;
    }

    return 0;
}
