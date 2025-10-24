/*
  FeastNow backend - ready to commit
  - Proper POST body accumulation using con_cls
  - Thread-safe queue/stack with pthread mutex
  - Safe string operations and simple URL decoding
  - Linear-probing hash table
  - CORS headers added to allow browser-based frontend (development)
  Compile:
    sudo apt-get install libmicrohttpd-dev    # Debian/Ubuntu
    gcc -o server backend/server.c -lmicrohttpd -pthread
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

/* ---------- GLOBAL STATE ---------- */
static Queue orderQueue;
static Stack undoStack;

/* ---------- POST handling context
