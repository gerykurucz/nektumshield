#ifndef NS_STATS_H
#define NS_STATS_H

#include <stdint.h>
#include <stdbool.h>
#include "ns_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define NS_STATS_MAX_IDENTIFIERS    8
#define NS_STATS_MAX_REASON_LEN     255
#define NS_STATS_MAX_ADMIN_LEN      64
#define NS_STATS_MAX_MAP_LEN        64
#define NS_STATS_MAX_SERVER_ID_LEN  64

typedef enum {
    NS_MOD_WARN = 0,
    NS_MOD_RESTRICT,
    NS_MOD_BAN,
    NS_MOD_UNBAN
} ns_moderation_action_t;

typedef enum {
    NS_IDENT_PLAYERID = 0,
    NS_IDENT_STEAMID,
    NS_IDENT_NAME,
    NS_IDENT_IP_HASH,
    NS_IDENT_SUBNET24
} ns_identifier_type_t;

typedef struct {
    ns_identifier_type_t type;
    char value[256];
} ns_identifier_t;

typedef struct {
    ns_player_id_t player_id;
    uint64_t timestamp_ms;
    char player_name[64];
    char ip_address[64];
    char subnet24[20];
    uint64_t steamid;
} ns_stats_player_connect_t;

typedef struct {
    ns_player_id_t player_id;
    uint64_t timestamp_ms;
} ns_stats_player_disconnect_t;

typedef struct {
    ns_player_id_t attacker_id;
    ns_player_id_t victim_id;
    uint64_t timestamp_ms;
    int hit_location;
    bool killed;
} ns_stats_damage_event_t;

typedef struct {
    ns_player_id_t player_id;
    uint64_t timestamp_ms;
    const char* server_id;
    const char* map_name;
} ns_stats_session_info_t;

typedef struct {
    void (*on_nektum_id)(ns_player_id_t player_id, int nektum_id);
    void (*on_error)(const char* error_message);
    void (*on_log)(const char* message);
} ns_stats_callbacks_t;

/* Inicializálás: endpoint a Go szolgáltatás URL-je, pl. "http://127.0.0.1:8080/events" */
void ns_stats_init(const char* endpoint, const ns_stats_callbacks_t* callbacks);
void ns_stats_shutdown(void);
void ns_stats_on_player_connect(const ns_stats_player_connect_t* event);
void ns_stats_on_player_disconnect(const ns_stats_player_disconnect_t* event);
void ns_stats_set_session_info(const ns_stats_session_info_t* info);
void ns_stats_on_frame(const ns_frame_event_t* event);
void ns_stats_on_kill(const ns_kill_event_t* event);
void ns_stats_on_damage(const ns_stats_damage_event_t* event);
void ns_stats_on_detection(const ns_detection_result_t* detection);
void ns_stats_on_moderation(ns_player_id_t player_id,
                            ns_moderation_action_t action_type,
                            const char* reason,
                            const char* admin_name);
int ns_stats_get_nektum_id(ns_player_id_t player_id);
void ns_stats_flush(void);
void ns_stats_tick(uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* NS_STATS_H */