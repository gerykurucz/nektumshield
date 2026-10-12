/*
 * Nektum Shield - Universal Server-Side Anti-Cheat Core API
 * Version: 2.0 (detection-only)
 *
 * This header defines the game-agnostic detection core.
 * The core only processes normalized movement/fire/kill events
 * and emits detection results via callbacks.
 *
 * All administrative tasks (banning, IP handling, player names, etc.)
 * are the responsibility of the game-specific adapter.
 */

#ifndef NS_CORE_H
#define NS_CORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ CONSTANTS ============================ */
#define NS_MAX_PLAYERS               64      /* maximum concurrent players */
#define NS_MAX_DETECTION_REASON_LEN  128

/* Button flags for ns_frame_event_t.buttons */
#define NS_BUTTON_FIRE       (1 << 0)
#define NS_BUTTON_ADS        (1 << 1)
#define NS_BUTTON_MOVE_FWD   (1 << 2)
#define NS_BUTTON_MOVE_BACK  (1 << 3)
#define NS_BUTTON_MOVE_LEFT  (1 << 4)
#define NS_BUTTON_MOVE_RIGHT (1 << 5)
#define NS_BUTTON_JUMP       (1 << 6)
#define NS_BUTTON_CROUCH     (1 << 7)

/* Generic hit locations */
#define NS_HITLOC_NONE        0
#define NS_HITLOC_HEAD        1
#define NS_HITLOC_NECK        2
#define NS_HITLOC_TORSO_UPPER 3
#define NS_HITLOC_TORSO_LOWER 4
#define NS_HITLOC_LIMB        5

/* ============================ TYPES ============================ */
typedef uint64_t ns_player_id_t;

typedef struct {
    float x, y, z;
} ns_vec3_t;

/* Frame event: called for every player move command (or tick) */
typedef struct {
    ns_player_id_t player_id;
    uint64_t timestamp_ms;        /* absolute server time in ms */
    float view_angles[3];         /* pitch, yaw, roll (degrees) */
    ns_vec3_t origin;             /* player origin */
    uint32_t buttons;             /* bitmask of NS_BUTTON_* */
    float forwardmove;            /* movement input -1..1 */
    float rightmove;              /* movement input -1..1 */
} ns_frame_event_t;

/* Kill event: called when a player kills another */
typedef struct {
    ns_player_id_t attacker_id;
    ns_player_id_t victim_id;
    uint64_t timestamp_ms;
    ns_vec3_t victim_origin;
    ns_vec3_t attacker_origin;
    int hit_location;             /* NS_HITLOC_* */
    int means_of_death;           /* e.g. bullet, headshot */
    float distance;               /* distance between attacker and victim */
    bool attacker_is_ads;         /* whether attacker was ADSing */
    bool attacker_moving;         /* whether attacker was moving */
} ns_kill_event_t;

/* Detection types (for callback / log) */
typedef enum {
    NS_DET_AIMBOT_SNAP = 0,
    NS_DET_AIMBOT_HS_RATIO,
    NS_DET_AIMBOT_SNAP_KILL,
    NS_DET_AIMBOT_MOVING_HS,
    NS_DET_AIM_ASSIST_BODYLOCK,
    NS_DET_AIM_ASSIST_PERFECT_SHOT,
    NS_DET_AIM_ASSIST_TARGET_SWITCH,
    NS_DET_NO_RECOIL,
    NS_DET_SILENT_AIM,
    NS_DET_NO_SPREAD,
    NS_DET_MACRO_LOGIC,
    NS_DET_MACRO_SCROLL,
    NS_DET_MACRO_RATE,
    NS_DET_MACRO_CONSISTENCY,
    NS_DET_COUNT
} ns_detection_type_t;

/* Detection result: passed to callback when a detection triggers */
typedef struct {
    ns_player_id_t player_id;
    ns_detection_type_t type;
    int score_added;
    char reason[NS_MAX_DETECTION_REASON_LEN];
    uint64_t timestamp_ms;
} ns_detection_result_t;

/* Configuration structure: all thresholds and windows */
typedef struct {
    /* General */
    int score_ban;                  /* score at which core notifies adapter to ban */
    
    /* Aimbot / snap */
    float snap_threshold;           /* degrees per interval */
    float snap_kill_angle;          /* degrees for snap+kill tracking */
    float snap_kill_window;         /* seconds after snap for kill */
    float hs_ratio;                 /* max allowed HS ratio */
    int min_kills;                  /* min kills before ratio checks */
    int time_hs_streak;             /* HS streak count */
    float time_hs_window;           /* seconds */
    int time_bl_streak;             /* bodylock streak count */
    float time_bl_window;           /* seconds */
    int body_lock_streak;           /* same hitloc streak */
    float moving_hs_dist;           /* min distance for moving HS */
    int moving_hs_streak;           /* consecutive moving HS */
    float moving_hs_window;         /* seconds */

    /* Silent aim / perfect shot */
    float silent_aim_angle;         /* max angle deviation for perfect */
    float silent_aim_dist;          /* min distance for silent aim */
    float perfect_yaw_pitch;        /* perfect shot angle tolerance */
    int perfect_shots;              /* consecutive perfect shots to flag */
    float silent_aim_threshold_hipfire; /* dot product threshold hipfire */
    float silent_aim_threshold_ads;     /* dot product threshold ADS */
    int silent_aim_streak;          /* consecutive impossible hits */

    /* Hipfire */
    int hipfire_kills;              /* long range hipfire kills streak */
    float hipfire_dist;             /* min distance for hipfire detection */

    /* No recoil */
    float norecoil_threshold;       /* yaw stability ratio */
    float recoil_macro_var;         /* pitch variance threshold */
    float recoil_macro_mean;        /* mean pitch threshold */
    int recoil_samples;             /* min samples for variance */
    int ads_norecoil_frames;        /* min zero-move frames in ADS */

    /* Macro detection */
    int macro_logic_clicks;
    int macro_logic_window;         /* ms */
    float macro_logic_strikes;      /* strikes before ban score */
    int macro_scroll_count;
    int macro_scroll_fast;          /* ms */
    int macro_scroll_reset;         /* ms */
    int macro_rate_count;
    int macro_rate_fast;            /* ms */
    int macro_consistency_count;
    int macro_consistency_gap;      /* ms */

    /* No spread */
    float nospread_max_deviation;   /* max distance between hits */
    int nospread_streak;            /* consecutive hits */

    /* Target switch */
    float target_switch_time;       /* seconds between kills */
    float target_switch_angle;      /* degrees camera turn */
    int target_switch_streak;       /* consecutive switches */
} ns_config_t;

/* Callback interface: the core will call these to notify the adapter */
typedef struct {
    /* Called when a detection occurs and adds score */
    void (*on_detection)(const ns_detection_result_t* det);
    /* Called when a player's score reaches the ban threshold */
    void (*on_ban_threshold)(ns_player_id_t player_id, int score, const char* reason);
    /* Optional: simple logging */
    void (*on_log)(const char* message);
} ns_core_callbacks_t;

/* ============================ FUNCTIONS ============================ */
/* Initialize core with configuration and callbacks. */
void ns_core_init(const ns_config_t* config, const ns_core_callbacks_t* callbacks);

/* Shutdown core, free resources. */
void ns_core_shutdown(void);

/* Update configuration at runtime (e.g., CVAR changes). */
void ns_core_set_config(const ns_config_t* config);

/* Register a player: initializes internal state for this player_id.
 * Must be called before sending frame/kill events for that player. */
void ns_core_register_player(ns_player_id_t player_id);

/* Unregister a player: clears internal state. */
void ns_core_unregister_player(ns_player_id_t player_id);

/* Main input events from adapter */
void ns_core_on_frame(const ns_frame_event_t* event);
void ns_core_on_kill(const ns_kill_event_t* event);

/* Periodic tick (called every second or more) for score decay, etc. */
void ns_core_tick(uint64_t now_ms);

/* Query current score for a player */
int ns_core_get_score(ns_player_id_t player_id);

/* Reset a player's internal detection state (e.g., after death or spawn) */
void ns_core_reset_player(ns_player_id_t player_id);

#ifdef __cplusplus
}
#endif

#endif /* NS_CORE_H */