/*
 * Nektum Shield - Stats Event Sender (Raw HTTP)
 * Version: 2.1
 *
 * Sends normalized player statistics as JSON over HTTP POST
 * to a central Go service using plain POSIX sockets.
 * No external libraries required.
 *
 * player_connect is sent synchronously to obtain nektum_id,
 * all other events are queued and sent by a background thread.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <time.h>
#include <errno.h>

#include "ns_stats.h"
#include "ns_core.h"

/* ============================ CONSTANTS ============================ */
#define MAX_HTTP_QUEUE_SIZE 2048
#define MAX_URL_LEN         512
#define MAX_JSON_LEN        2048
#define MAX_RESPONSE_LEN    512
#define USER_AGENT          "NektumShield/2.1"
#define HTTP_TIMEOUT_SEC    5

/* ============================ INTERNAL TYPES ============================ */
typedef struct {
    bool active;
    ns_player_id_t player_id;
    int nektum_id;
    uint64_t session_start_ms;
} ns_stats_player_t;

typedef struct {
    char url[MAX_URL_LEN];
    char json[MAX_JSON_LEN];
} http_job_t;

/* ============================ GLOBAL STATE ============================ */
static ns_stats_callbacks_t g_callbacks;
static bool g_initialized = false;
static ns_stats_player_t g_players[NS_MAX_PLAYERS];
static char g_endpoint[MAX_URL_LEN] = "";
static char g_host[256] = "";
static int g_port = 80;
static char g_path[256] = "/";

/* Queue for asynchronous HTTP requests */
static http_job_t* g_queue = NULL;
static int g_queue_capacity = MAX_HTTP_QUEUE_SIZE;
static int g_queue_head = 0;
static int g_queue_tail = 0;
static int g_queue_count = 0;
static pthread_mutex_t g_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_queue_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_worker_thread;
static bool g_worker_running = false;



/* Mutex for protecting global player state */
static pthread_mutex_t g_stats_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ============================ UTILITY FUNCTIONS ============================ */
static void stats_log(const char* fmt, ...) {
    if (!g_callbacks.on_log) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_callbacks.on_log(buffer);
}

static void stats_error(const char* fmt, ...) {
    if (!g_callbacks.on_error) return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    g_callbacks.on_error(buffer);
}

static int get_player_index(ns_player_id_t player_id) {
    for (int i = 0; i < NS_MAX_PLAYERS; i++) {
        if (g_players[i].active && g_players[i].player_id == player_id) {
            return i;
        }
    }
    return -1;
}

/* Parse "http://host:port/path" into host, port, path */
static int parse_endpoint(const char* endpoint, char* host, int* port, char* path) {
    if (!endpoint || strlen(endpoint) < 8) return -1;
    if (strncmp(endpoint, "http://", 7) != 0) return -1;

    const char* rest = endpoint + 7;
    const char* slash = strchr(rest, '/');
    char host_port[256];
    if (slash) {
        snprintf(host_port, sizeof(host_port), "%.*s", (int)(slash - rest), rest);
        snprintf(path, 256, "%s", slash);
    } else {
        snprintf(host_port, sizeof(host_port), "%s", rest);
        snprintf(path, 256, "/");
    }

    const char* colon = strchr(host_port, ':');
    if (colon) {
        snprintf(host, 256, "%.*s", (int)(colon - host_port), host_port);
        *port = atoi(colon + 1);
    } else {
        snprintf(host, 256, "%s", host_port);
        *port = 80;
    }
    return 0;
}

/* Egyszerű HTTP POST raw socketen. Visszatérés: 0 siker, -1 hiba.
   Ha response nem NULL, a válasz testet odamásolja. */
static int http_post_raw(const char* host, int port, const char* path, const char* json,
                         char* response, size_t response_size) {
    int sockfd;
    struct sockaddr_in serv_addr;
    struct hostent* server;

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        stats_error("socket() failed: %s", strerror(errno));
        return -1;
    }

    server = gethostbyname(host);
    if (!server) {
        stats_error("gethostbyname failed for %s", host);
        close(sockfd);
        return -1;
    }

    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(port);

    struct timeval timeout;
    timeout.tv_sec = HTTP_TIMEOUT_SEC;
    timeout.tv_usec = 0;
    setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        stats_error("connect() failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    char request[MAX_JSON_LEN + 512];
    snprintf(request, sizeof(request),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        path, host, USER_AGENT, (int)strlen(json), json);

    if (send(sockfd, request, strlen(request), 0) < 0) {
        stats_error("send() failed: %s", strerror(errno));
        close(sockfd);
        return -1;
    }

    // Válasz olvasása
    if (response && response_size > 0) {
        char buffer[1024];
        int total_read = 0;
        while (total_read < (int)response_size - 1) {
            int n = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
            if (n <= 0) break;
            buffer[n] = '\0';
            // Keressük a HTTP test elejét (üres sor után)
            char* body = strstr(buffer, "\r\n\r\n");
            if (body) {
                body += 4;
                int copy_len = n - (int)(body - buffer);
                if (copy_len > (int)response_size - 1 - total_read)
                    copy_len = (int)response_size - 1 - total_read;
                memcpy(response + total_read, body, copy_len);
                total_read += copy_len;
                response[total_read] = '\0';
                break; // elég a válasz test
            }
        }
        if (total_read == 0) response[0] = '\0';
    }

    close(sockfd);
    return 0;
}

/* ============================ QUEUE MANAGEMENT ============================ */
static void queue_init(void) {
    g_queue = (http_job_t*)calloc(g_queue_capacity, sizeof(http_job_t));
    if (!g_queue) {
        stats_error("Failed to allocate HTTP queue");
    }
}

static void queue_destroy(void) {
    if (g_queue) {
        free(g_queue);
        g_queue = NULL;
    }
}

static bool queue_push(const char* url, const char* json) {
    if (!g_queue) return false;

    pthread_mutex_lock(&g_queue_mutex);
    if (g_queue_count >= g_queue_capacity) {
        stats_error("HTTP queue full, dropping event");
        pthread_mutex_unlock(&g_queue_mutex);
        return false;
    }

    snprintf(g_queue[g_queue_tail].url, MAX_URL_LEN, "%s", url);
    snprintf(g_queue[g_queue_tail].json, MAX_JSON_LEN, "%s", json);
    g_queue_tail = (g_queue_tail + 1) % g_queue_capacity;
    g_queue_count++;
    pthread_cond_signal(&g_queue_cond);
    pthread_mutex_unlock(&g_queue_mutex);
    return true;
}

static bool queue_pop(char* json) {
    if (!g_queue) return false;

    pthread_mutex_lock(&g_queue_mutex);
    while (g_queue_count == 0 && g_worker_running) {
        pthread_cond_wait(&g_queue_cond, &g_queue_mutex);
    }
    if (g_queue_count == 0) {
        pthread_mutex_unlock(&g_queue_mutex);
        return false;
    }

    snprintf(json, MAX_JSON_LEN, "%s", g_queue[g_queue_head].json);
    g_queue_head = (g_queue_head + 1) % g_queue_capacity;
    g_queue_count--;
    pthread_mutex_unlock(&g_queue_mutex);
    return true;
}

/* Worker thread: folyamatosan küldi a HTTP kéréseket. */
static void* worker_thread_func(void* arg) {
    (void)arg;
    char json[MAX_JSON_LEN];

    while (g_worker_running) {
        if (queue_pop(json)) {
            http_post_raw(g_host, g_port, g_path, json, NULL, 0);
        }
    }
    return NULL;
}

/* ============================ INIT / SHUTDOWN ============================ */
void ns_stats_init(const char* endpoint, const ns_stats_callbacks_t* callbacks) {
    if (g_initialized) {
        stats_error("ns_stats already initialized");
        return;
    }

    if (callbacks) {
        g_callbacks = *callbacks;
    } else {
        memset(&g_callbacks, 0, sizeof(g_callbacks));
    }

    if (!endpoint || endpoint[0] == '\0') {
        stats_error("Invalid endpoint URL");
        return;
    }

    if (parse_endpoint(endpoint, g_host, &g_port, g_path) != 0) {
        stats_error("Failed to parse endpoint URL: %s", endpoint);
        return;
    }

    snprintf(g_endpoint, sizeof(g_endpoint), "%s", endpoint);
    memset(g_players, 0, sizeof(g_players));

    queue_init();

    g_worker_running = true;
    if (pthread_create(&g_worker_thread, NULL, worker_thread_func, NULL) != 0) {
        stats_error("Failed to create HTTP worker thread");
        g_worker_running = false;
        queue_destroy();
        return;
    }

    g_initialized = true;
    stats_log("ns_stats initialized, sending events to %s", g_endpoint);
}

void ns_stats_shutdown(void) {
    if (!g_initialized) return;

    g_worker_running = false;
    pthread_cond_broadcast(&g_queue_cond);
    pthread_join(g_worker_thread, NULL);

    queue_destroy();
    g_initialized = false;
    stats_log("ns_stats shut down");
}

/* ============================ EVENT HANDLERS ============================ */
void ns_stats_on_player_connect(const ns_stats_player_connect_t* event) {
    if (!g_initialized || !event) return;

    pthread_mutex_lock(&g_stats_mutex);

    int free_idx = -1;
    for (int i = 0; i < NS_MAX_PLAYERS; i++) {
        if (!g_players[i].active) {
            free_idx = i;
            break;
        }
    }
    if (free_idx == -1) {
        stats_error("No free player slot for stats");
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    // JSON összeállítása
    char json[MAX_JSON_LEN];
    snprintf(json, sizeof(json),
        "{\"event\":\"player_connect\",\"timestamp\":\"%lld\",\"server_id\":\"%s\","
        "\"player_id\":%llu,\"steam_id\":%llu,\"player_name\":\"%s\",\"ip_hash\":\"%s\","
        "\"subnet24\":\"%s\"}",
        (long long)event->timestamp_ms,
        "",
        (unsigned long long)event->player_id,
        (unsigned long long)event->steamid,
        event->player_name,
        event->ip_address,  // raw IP, a Go oldal majd hasheli
        event->subnet24);

    // Szinkron válasz kérése a nektum_id megszerzéséhez
    // Temporarily unlock for HTTP request to avoid blocking other threads
    pthread_mutex_unlock(&g_stats_mutex);

    char response[MAX_RESPONSE_LEN] = "";
    if (http_post_raw(g_host, g_port, g_path, json, response, sizeof(response)) == 0) {
        int nektum_id = 0;
        if (sscanf(response, "{\"nektum_id\":%d}", &nektum_id) == 1) {
            pthread_mutex_lock(&g_stats_mutex);
            g_players[free_idx].nektum_id = nektum_id;
            pthread_mutex_unlock(&g_stats_mutex);
        } else {
            stats_error("Failed to parse nektum_id from response: %s", response);
        }
    }

    pthread_mutex_lock(&g_stats_mutex);
    g_players[free_idx].active = true;
    g_players[free_idx].player_id = event->player_id;
    g_players[free_idx].session_start_ms = event->timestamp_ms;

    if (g_callbacks.on_nektum_id) {
        g_callbacks.on_nektum_id(event->player_id, g_players[free_idx].nektum_id);
    }
    pthread_mutex_unlock(&g_stats_mutex);
}

void ns_stats_on_player_disconnect(const ns_stats_player_disconnect_t* event) {
    if (!g_initialized || !event) return;

    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(event->player_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    uint64_t duration_ms = event->timestamp_ms - g_players[idx].session_start_ms;
    int duration_seconds = (int)(duration_ms / 1000);
    if (duration_seconds < 0) duration_seconds = 0;

    char json[MAX_JSON_LEN];
    snprintf(json, sizeof(json),
        "{\"event\":\"player_disconnect\",\"timestamp\":\"%lld\",\"server_id\":\"%s\","
        "\"nektum_id\":%d,\"session_duration_seconds\":%d}",
        (long long)event->timestamp_ms,
        "",
        g_players[idx].nektum_id,
        duration_seconds);

    queue_push(g_endpoint, json);
    g_players[idx].active = false;

    pthread_mutex_unlock(&g_stats_mutex);
}

void ns_stats_set_session_info(const ns_stats_session_info_t* info) {
    if (!g_initialized || !info) return;

    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(info->player_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    char json[MAX_JSON_LEN];
    snprintf(json, sizeof(json),
        "{\"event\":\"session_info\",\"timestamp\":\"%lld\",\"nektum_id\":%d,"
        "\"server_id\":\"%s\",\"map_name\":\"%s\"}",
        (long long)info->timestamp_ms,
        g_players[idx].nektum_id,
        info->server_id ? info->server_id : "",
        info->map_name ? info->map_name : "");

    queue_push(g_endpoint, json);

    pthread_mutex_unlock(&g_stats_mutex);
}

void ns_stats_on_frame(const ns_frame_event_t* event) {
    if (!g_initialized || !event) return;

    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(event->player_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    static bool prev_firing[NS_MAX_PLAYERS] = {false};
    bool firing = (event->buttons & NS_BUTTON_FIRE) != 0;
    if (firing && !prev_firing[idx]) {
        char json[MAX_JSON_LEN];
        snprintf(json, sizeof(json),
            "{\"event\":\"shot_fired\",\"timestamp\":\"%lld\",\"nektum_id\":%d,"
            "\"weapon_mod\":0,\"is_ads\":%s}",
            (long long)event->timestamp_ms,
            g_players[idx].nektum_id,
            (event->buttons & NS_BUTTON_ADS) ? "true" : "false");
        queue_push(g_endpoint, json);
    }
    prev_firing[idx] = firing;

    pthread_mutex_unlock(&g_stats_mutex);
}

void ns_stats_on_kill(const ns_kill_event_t* event) {
    if (!g_initialized || !event) return;

    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(event->attacker_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    int victim_idx = get_player_index(event->victim_id);
    int victim_nektum_id = (victim_idx != -1) ? g_players[victim_idx].nektum_id : 0;

    char json[MAX_JSON_LEN];
    snprintf(json, sizeof(json),
        "{\"event\":\"kill\",\"timestamp\":\"%lld\",\"nektum_id\":%d,"
        "\"victim_nektum_id\":%d,\"hit_location\":%d,\"weapon_mod\":%d,"
        "\"distance\":%.1f,\"attacker_is_ads\":%s,\"attacker_moving\":%s}",
        (long long)event->timestamp_ms,
        g_players[idx].nektum_id,
        victim_nektum_id,
        event->hit_location,
        event->means_of_death,
        event->distance,
        event->attacker_is_ads ? "true" : "false",
        event->attacker_moving ? "true" : "false");

    queue_push(g_endpoint, json);

    pthread_mutex_unlock(&g_stats_mutex);
}

void ns_stats_on_damage(const ns_stats_damage_event_t* event) {
    if (!g_initialized || !event) return;

    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(event->attacker_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    char json[MAX_JSON_LEN];
    snprintf(json, sizeof(json),
        "{\"event\":\"damage\",\"timestamp\":\"%lld\",\"nektum_id\":%d,"
        "\"hit_location\":%d,\"killed\":%s}",
        (long long)event->timestamp_ms,
        g_players[idx].nektum_id,
        event->hit_location,
        event->killed ? "true" : "false");

    queue_push(g_endpoint, json);

    pthread_mutex_unlock(&g_stats_mutex);
}

void ns_stats_on_detection(const ns_detection_result_t* detection) {
    if (!g_initialized || !detection) return;

    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(detection->player_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    char json[MAX_JSON_LEN];
    snprintf(json, sizeof(json),
        "{\"event\":\"detection\",\"timestamp\":\"%lld\",\"nektum_id\":%d,"
        "\"detection_type\":%d,\"reason\":\"%s\",\"score_added\":%d}",
        (long long)detection->timestamp_ms,
        g_players[idx].nektum_id,
        detection->type,
        detection->reason,
        detection->score_added);

    queue_push(g_endpoint, json);

    pthread_mutex_unlock(&g_stats_mutex);
}

void ns_stats_on_moderation(ns_player_id_t player_id,
                            ns_moderation_action_t action_type,
                            const char* reason,
                            const char* admin_name) {
    if (!g_initialized) return;

    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(player_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_stats_mutex);
        return;
    }

    const char* action_str = NULL;
    switch (action_type) {
        case NS_MOD_WARN:     action_str = "warn"; break;
        case NS_MOD_RESTRICT: action_str = "restrict"; break;
        case NS_MOD_BAN:      action_str = "ban"; break;
        case NS_MOD_UNBAN:    action_str = "unban"; break;
        default:
            pthread_mutex_unlock(&g_stats_mutex);
            return;
    }

    char json[MAX_JSON_LEN];
    snprintf(json, sizeof(json),
        "{\"event\":\"moderation\",\"timestamp\":\"%lld\",\"nektum_id\":%d,"
        "\"action\":\"%s\",\"reason\":\"%s\",\"admin_name\":\"%s\"}",
        (long long)time(NULL),
        g_players[idx].nektum_id,
        action_str,
        reason ? reason : "",
        admin_name ? admin_name : "");

    queue_push(g_endpoint, json);

    pthread_mutex_unlock(&g_stats_mutex);
}

int ns_stats_get_nektum_id(ns_player_id_t player_id) {
    pthread_mutex_lock(&g_stats_mutex);

    int idx = get_player_index(player_id);
    int result = (idx == -1) ? -1 : g_players[idx].nektum_id;

    pthread_mutex_unlock(&g_stats_mutex);
    return result;
}

void ns_stats_flush(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 100000000; // 100ms
    nanosleep(&ts, NULL);
}
