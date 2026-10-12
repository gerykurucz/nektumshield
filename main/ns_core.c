#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <pthread.h>

#include "ns_core.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NS_TIME_DECAY_MS         15000
#define NS_DECAY_AMOUNT          2
#define NS_MAX_HIT_POSITIONS     5
#define NS_SNAP_INTERVAL_MIN_MS  15
#define NS_SNAP_INTERVAL_MAX_MS  100
#define NS_LARGE_SNAP_ANGLE      120.0f

typedef struct {
    bool active;
    ns_player_id_t player_id;
    uint64_t last_activity_ms;
    uint64_t last_decay_time;   // ÚJ: utolsó csökkenés időpontja

    int ac_score;
    char last_reason[NS_MAX_DETECTION_REASON_LEN];

    float last_angles[3];
    uint64_t last_cmd_time_ms;
    uint64_t last_move_time_ms;
    float last_origin[3];
    float large_snap_forward[3];
    uint64_t last_large_snap_time_ms;
    uint64_t last_snap_aim_time_ms;
    uint64_t last_snap_kill_time_ms;

    bool was_firing;
    int firing_frames;
    int recoil_zero_yaw_frames;
    float pitch_sum;
    float pitch_sq_sum;
    int recoil_samples;
    uint64_t last_no_recoil_time_ms;
    uint64_t last_macro_det_time_ms;
    int ads_zero_move_frames;
    bool is_ads;

    bool has_shot_data;
    float last_shot_angles[3];
    float last_shot_origin[3];
    uint64_t last_shot_time_ms;
    bool last_shot_ads;

    int kills;
    int headshots;
    int hs_streak;
    int bl_streak;
    uint64_t first_hs_streak_time_ms;
    uint64_t first_bl_streak_time_ms;
    int last_hs_ratio_check_kills;
    int last_hs_ratio_check_headshots;
    int last_hit_loc;
    int same_hit_streak;
    uint64_t first_same_hit_time_ms;
    uint64_t last_body_lock_time_ms;

    int perfect_shots;
    uint64_t last_perfect_kill_time_ms;
    uint64_t last_perfect_shot_time_ms;

    int hipfire_long_kills;
    uint64_t last_hipfire_kill_time_ms;
    uint64_t last_hipfire_flag_time_ms;

    int moving_headshot_count;
    uint64_t last_moving_hs_time_ms;

    int silent_aim_count;
    uint64_t last_silent_aim_time_ms;

    int no_spread_hit_count;
    uint64_t last_no_spread_hit_time_ms;
    float last_hit_positions[NS_MAX_HIT_POSITIONS][3];
    int hit_pos_index;

    int rapid_switch_kills;
    uint64_t last_kill_time_ms;
    float last_kill_view_angles[3];

    int logic_click_count;
    uint64_t logic_window_start_ms;
    float logic_strikes;
    int scroll_fast_count;
    uint64_t last_scroll_time_ms;
    int rate_fast_count;
    uint64_t last_rate_time_ms;
    int consistency_match_count;
    int last_consistency_gap_ms;
    uint64_t last_consistency_time_ms;

    bool pending_ban_notified;
} ns_player_state_t;

static ns_player_state_t g_players[NS_MAX_PLAYERS];
static ns_config_t g_config;
static ns_core_callbacks_t g_callbacks;
static bool g_initialized = false;
static pthread_mutex_t g_core_mutex = PTHREAD_MUTEX_INITIALIZER;

static int ns_time_diff_ms(uint64_t now, uint64_t prev) {
    if (now < prev) return 0;
    return (int)(now - prev);
}

static float ns_time_diff_sec(uint64_t now, uint64_t prev) {
    if (now < prev) return 0.0f;
    return (float)(now - prev) / 1000.0f;
}

static float ns_get_yaw_distance(float yaw1, float yaw2) {
    float diff = fabsf(yaw1 - yaw2);
    if (diff > 180.0f) diff = 360.0f - diff;
    return diff;
}

static void ns_angle_vectors(const float* angles, float* forward) {
    float pitch = angles[0] * (float)(M_PI / 180.0);
    float yaw = angles[1] * (float)(M_PI / 180.0);
    float sp = sinf(pitch);
    float cp = cosf(pitch);
    float sy = sinf(yaw);
    float cy = cosf(yaw);
    forward[0] = cp * cy;
    forward[1] = cp * sy;
    forward[2] = -sp;
}

static int ns_get_player_index(ns_player_id_t player_id) {
    for (int i = 0; i < NS_MAX_PLAYERS; i++) {
        if (g_players[i].active && g_players[i].player_id == player_id) {
            return i;
        }
    }
    return -1;
}

static void ns_add_score(int idx, int score, const char* reason, ns_detection_type_t type) {
    ns_player_state_t* st = &g_players[idx];
    if (!st->active) return;

    st->ac_score += score;
    snprintf(st->last_reason, sizeof(st->last_reason), "%s", reason);
    st->last_activity_ms = st->last_activity_ms; // maintain

    if (g_callbacks.on_detection) {
        ns_detection_result_t det;
        det.player_id = st->player_id;
        det.type = type;
        det.score_added = score;
        snprintf(det.reason, sizeof(det.reason), "%s", reason);
        det.timestamp_ms = st->last_activity_ms;
        g_callbacks.on_detection(&det);
    }

    if (st->ac_score >= g_config.score_ban && !st->pending_ban_notified) {
        st->pending_ban_notified = true;
        if (g_callbacks.on_ban_threshold) {
            g_callbacks.on_ban_threshold(st->player_id, st->ac_score, reason);
        }
    }
}

void ns_core_init(const ns_config_t* config, const ns_core_callbacks_t* callbacks) {
    pthread_mutex_lock(&g_core_mutex);
    if (!config) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }
    memset(&g_players, 0, sizeof(g_players));
    g_config = *config;
    if (callbacks) {
        g_callbacks = *callbacks;
    } else {
        memset(&g_callbacks, 0, sizeof(g_callbacks));
    }
    g_initialized = true;
    pthread_mutex_unlock(&g_core_mutex);
    if (g_callbacks.on_log) {
        g_callbacks.on_log("Nektum Shield core initialized.");
    }
}

void ns_core_shutdown(void) {
    pthread_mutex_lock(&g_core_mutex);
    g_initialized = false;
    memset(&g_players, 0, sizeof(g_players));
    pthread_mutex_unlock(&g_core_mutex);
}

void ns_core_set_config(const ns_config_t* config) {
    pthread_mutex_lock(&g_core_mutex);
    if (config) {
        g_config = *config;
    }
    pthread_mutex_unlock(&g_core_mutex);
}

void ns_core_register_player(ns_player_id_t player_id) {
    pthread_mutex_lock(&g_core_mutex);
    if (!g_initialized) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }
    // Remove any existing entry with same player_id
    int existing = ns_get_player_index(player_id);
    if (existing != -1) {
        memset(&g_players[existing], 0, sizeof(ns_player_state_t));
    }
    int free_idx = -1;
    for (int i = 0; i < NS_MAX_PLAYERS; i++) {
        if (!g_players[i].active) {
            free_idx = i;
            break;
        }
    }
    if (free_idx == -1) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }

    memset(&g_players[free_idx], 0, sizeof(ns_player_state_t));
    g_players[free_idx].active = true;
    g_players[free_idx].player_id = player_id;
    g_players[free_idx].last_activity_ms = 0;
    g_players[free_idx].last_decay_time = 0;
    pthread_mutex_unlock(&g_core_mutex);
}

void ns_core_unregister_player(ns_player_id_t player_id) {
    pthread_mutex_lock(&g_core_mutex);
    int idx = ns_get_player_index(player_id);
    if (idx != -1) {
        memset(&g_players[idx], 0, sizeof(ns_player_state_t));
    }
    pthread_mutex_unlock(&g_core_mutex);
}

void ns_core_reset_player(ns_player_id_t player_id) {
    pthread_mutex_lock(&g_core_mutex);
    int idx = ns_get_player_index(player_id);
    if (idx != -1) {
        ns_player_state_t st;
        memset(&st, 0, sizeof(st));
        st.active = true;
        st.player_id = player_id;
        st.last_decay_time = 0;
        g_players[idx] = st;
    }
    pthread_mutex_unlock(&g_core_mutex);
}

int ns_core_get_score(ns_player_id_t player_id) {
    pthread_mutex_lock(&g_core_mutex);
    int idx = ns_get_player_index(player_id);
    int score = (idx != -1) ? g_players[idx].ac_score : 0;
    pthread_mutex_unlock(&g_core_mutex);
    return score;
}

void ns_core_tick(uint64_t now_ms) {
    pthread_mutex_lock(&g_core_mutex);
    if (!g_initialized) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }
    for (int i = 0; i < NS_MAX_PLAYERS; i++) {
        ns_player_state_t* st = &g_players[i];
        if (!st->active) continue;
        // Csak akkor csökkentünk, ha az utolsó csökkentés óta eltelt legalább 15 mp
        if (st->ac_score > 0 && (st->last_decay_time == 0 || ns_time_diff_ms(now_ms, st->last_decay_time) >= NS_TIME_DECAY_MS)) {
            st->ac_score -= NS_DECAY_AMOUNT;
            if (st->ac_score < 0) st->ac_score = 0;
            st->last_decay_time = now_ms;
        }
    }
    pthread_mutex_unlock(&g_core_mutex);
}

void ns_core_on_frame(const ns_frame_event_t* event) {
    pthread_mutex_lock(&g_core_mutex);
    if (!g_initialized || !event) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }
    int idx = ns_get_player_index(event->player_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }

    ns_player_state_t* st = &g_players[idx];
    uint64_t now_ms = event->timestamp_ms;
    st->last_activity_ms = now_ms;

    if (st->last_scroll_time_ms == 0) st->last_scroll_time_ms = now_ms;
    if (st->logic_window_start_ms == 0) st->logic_window_start_ms = now_ms;
    if (st->last_rate_time_ms == 0) st->last_rate_time_ms = now_ms;
    if (st->last_consistency_time_ms == 0) st->last_consistency_time_ms = now_ms;

    float current_angles[3] = { event->view_angles[0], event->view_angles[1], event->view_angles[2] };
    bool firing = (event->buttons & NS_BUTTON_FIRE) != 0;
    bool current_ads = (event->buttons & NS_BUTTON_ADS) != 0;

    // Snap detection
    if (st->last_cmd_time_ms != 0) {
        float yaw_change = ns_get_yaw_distance(current_angles[1], st->last_angles[1]);
        int dt_ms = ns_time_diff_ms(now_ms, st->last_cmd_time_ms);

        bool is_moving = ns_time_diff_sec(now_ms, st->last_move_time_ms) < 1.0f;
        if (is_moving && dt_ms >= NS_SNAP_INTERVAL_MIN_MS && dt_ms <= NS_SNAP_INTERVAL_MAX_MS &&
            yaw_change > g_config.snap_threshold &&
            ns_time_diff_sec(now_ms, st->last_snap_aim_time_ms) > 5.0f) {
            ns_add_score(idx, 10, "NS | Auto Detection: Aimbot", NS_DET_AIMBOT_SNAP);
            st->last_snap_aim_time_ms = now_ms;
        }

        if (is_moving && dt_ms >= NS_SNAP_INTERVAL_MIN_MS && dt_ms <= NS_SNAP_INTERVAL_MAX_MS &&
            yaw_change > g_config.snap_kill_angle) {
            st->last_snap_kill_time_ms = now_ms;
        }

        if (yaw_change > NS_LARGE_SNAP_ANGLE) {
            st->last_large_snap_time_ms = now_ms;
            ns_angle_vectors(current_angles, st->large_snap_forward);
        }
    }

    // Fire transition (shot start)
    if (firing && !st->was_firing) {
        st->has_shot_data = true;
        st->last_shot_angles[0] = current_angles[0];
        st->last_shot_angles[1] = current_angles[1];
        st->last_shot_angles[2] = current_angles[2];
        st->last_shot_origin[0] = event->origin.x;
        st->last_shot_origin[1] = event->origin.y;
        st->last_shot_origin[2] = event->origin.z;
        st->last_shot_time_ms = now_ms;
        st->last_shot_ads = current_ads;

        // Macro logic detection
        if (ns_time_diff_ms(now_ms, st->logic_window_start_ms) >= g_config.macro_logic_window) {
            st->logic_window_start_ms = now_ms;
            st->logic_click_count = 1;
        } else {
            st->logic_click_count++;
        }

        if (st->logic_click_count >= g_config.macro_logic_clicks) {
            st->logic_strikes += 1.0f;
            if (st->logic_strikes >= g_config.macro_logic_strikes) {
                st->logic_strikes = 0.0f;
                ns_add_score(idx, 10, "NS | Auto Detection: Macro Logic", NS_DET_MACRO_LOGIC);
            }
            st->logic_click_count = 0;
            st->logic_window_start_ms = now_ms;
        } else if (st->logic_strikes > 0.0f && st->logic_click_count == 1) {
            st->logic_strikes -= 0.2f;
            if (st->logic_strikes < 0.0f) st->logic_strikes = 0.0f;
        }

        int scroll_diff = ns_time_diff_ms(now_ms, st->last_scroll_time_ms);
        if (scroll_diff > 0 && scroll_diff < g_config.macro_scroll_fast) {
            st->scroll_fast_count++;
        } else if (scroll_diff >= g_config.macro_scroll_reset) {
            st->scroll_fast_count = 0;
        }
        if (st->scroll_fast_count >= g_config.macro_scroll_count) {
            st->scroll_fast_count = 0;
            ns_add_score(idx, 10, "NS | Auto Detection: Macro Scroll", NS_DET_MACRO_SCROLL);
        }
        st->last_scroll_time_ms = now_ms;

        int rate_diff = ns_time_diff_ms(now_ms, st->last_rate_time_ms);
        if (rate_diff > 0 && rate_diff < g_config.macro_rate_fast) {
            st->rate_fast_count++;
        } else if (st->rate_fast_count > 0) {
            st->rate_fast_count--;
        }
        if (st->rate_fast_count >= g_config.macro_rate_count) {
            st->rate_fast_count = 0;
            ns_add_score(idx, 10, "NS | Auto Detection: Macro Rate", NS_DET_MACRO_RATE);
        }
        st->last_rate_time_ms = now_ms;

        int consistency_gap = ns_time_diff_ms(now_ms, st->last_consistency_time_ms);
        if (consistency_gap > 0 && consistency_gap < g_config.macro_consistency_gap) {
            if (st->last_consistency_gap_ms > 0 && abs(consistency_gap - st->last_consistency_gap_ms) <= 2) {
                st->consistency_match_count++;
            } else {
                st->consistency_match_count = 0;
            }
            if (st->consistency_match_count >= g_config.macro_consistency_count) {
                st->consistency_match_count = 0;
                ns_add_score(idx, 10, "NS | Auto Detection: Macro Consistency", NS_DET_MACRO_CONSISTENCY);
            }
        } else {
            st->consistency_match_count = 0;
        }
        st->last_consistency_gap_ms = consistency_gap;
        st->last_consistency_time_ms = now_ms;
    }

    // Recoil data collection
    if (firing) {
        if (st->was_firing) {
            st->firing_frames++;
            int dp = (int)(current_angles[0] - st->last_angles[0]);
            int dy = (int)ns_get_yaw_distance(current_angles[1], st->last_angles[1]);

            if (current_ads) {
                if (abs(dp) <= 8 && abs(dy) <= 8) {
                    st->ads_zero_move_frames++;
                } else {
                    st->ads_zero_move_frames = 0;
                }
            } else {
                st->ads_zero_move_frames = 0;
            }

            if (dy > -3 && dy < 3) {
                st->recoil_zero_yaw_frames++;
            }

            st->pitch_sum += (float)dp;
            st->pitch_sq_sum += (float)(dp * dp);
            st->recoil_samples++;
        } else {
            st->firing_frames = 1;
            st->recoil_zero_yaw_frames = 0;
            st->ads_zero_move_frames = 0;
            st->pitch_sum = 0.0f;
            st->pitch_sq_sum = 0.0f;
            st->recoil_samples = 0;
        }
    } else {
        if (st->was_firing && st->recoil_samples > g_config.recoil_samples) {
            float mean_pitch = st->pitch_sum / (float)st->recoil_samples;
            float var_pitch = (st->pitch_sq_sum / (float)st->recoil_samples) - (mean_pitch * mean_pitch);

            if (var_pitch < g_config.recoil_macro_var &&
                mean_pitch < g_config.recoil_macro_mean &&
                ns_time_diff_sec(now_ms, st->last_macro_det_time_ms) > 10.0f) {
                ns_add_score(idx, 10, "NS | Auto Detection: No-Recoil", NS_DET_NO_RECOIL);
                st->last_macro_det_time_ms = now_ms;
            }

            float yaw_stability = (float)st->recoil_zero_yaw_frames / (float)st->firing_frames;
            if (yaw_stability > g_config.norecoil_threshold &&
                ns_time_diff_sec(now_ms, st->last_no_recoil_time_ms) > 10.0f) {
                ns_add_score(idx, 10, "NS | Auto Detection: No-Recoil", NS_DET_NO_RECOIL);
                st->last_no_recoil_time_ms = now_ms;
            }

            float zero_move_ratio = (float)st->ads_zero_move_frames / (float)st->firing_frames;
            if (st->ads_zero_move_frames >= g_config.ads_norecoil_frames &&
                st->firing_frames >= 120 &&
                zero_move_ratio >= 0.80f &&
                ns_time_diff_sec(now_ms, st->last_no_recoil_time_ms) > 30.0f) {
                ns_add_score(idx, 20, "NS | Auto Detection: No-Recoil ADS", NS_DET_NO_RECOIL);
                st->last_no_recoil_time_ms = now_ms;
            }
        }

        st->firing_frames = 0;
        st->recoil_zero_yaw_frames = 0;
        st->ads_zero_move_frames = 0;
        st->pitch_sum = 0.0f;
        st->pitch_sq_sum = 0.0f;
        st->recoil_samples = 0;
    }

    // Movement tracking
    float dx = event->origin.x - st->last_origin[0];
    float dy = event->origin.y - st->last_origin[1];
    float dz = event->origin.z - st->last_origin[2];
    if (dx*dx + dy*dy + dz*dz > 1.0f) {
        st->last_move_time_ms = now_ms;
    }
    st->last_origin[0] = event->origin.x;
    st->last_origin[1] = event->origin.y;
    st->last_origin[2] = event->origin.z;

    st->was_firing = firing;
    st->is_ads = current_ads;
    st->last_angles[0] = current_angles[0];
    st->last_angles[1] = current_angles[1];
    st->last_angles[2] = current_angles[2];
    st->last_cmd_time_ms = now_ms;

    pthread_mutex_unlock(&g_core_mutex);
}

void ns_core_on_kill(const ns_kill_event_t* event) {
    pthread_mutex_lock(&g_core_mutex);
    if (!g_initialized || !event) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }
    int idx = ns_get_player_index(event->attacker_id);
    if (idx == -1) {
        pthread_mutex_unlock(&g_core_mutex);
        return;
    }

    ns_player_state_t* st = &g_players[idx];
    uint64_t now_ms = event->timestamp_ms;
    st->last_activity_ms = now_ms;

    st->kills++;
    bool is_headshot = (event->hit_location == NS_HITLOC_HEAD ||
                        event->hit_location == NS_HITLOC_NECK);
    bool is_body = (event->hit_location == NS_HITLOC_TORSO_UPPER ||
                    event->hit_location == NS_HITLOC_TORSO_LOWER);

    if (is_headshot) {
        st->headshots++;
        st->hs_streak++;
        if (st->hs_streak == 1) st->first_hs_streak_time_ms = now_ms;
    } else {
        st->hs_streak = 0;
    }

    if (is_body) {
        st->bl_streak++;
        if (st->bl_streak == 1) st->first_bl_streak_time_ms = now_ms;
    } else {
        st->bl_streak = 0;
    }

    // HS ratio sliding window
    if (st->kills >= g_config.min_kills) {
        int window_kills = st->kills - st->last_hs_ratio_check_kills;
        int window_hs = st->headshots - st->last_hs_ratio_check_headshots;
        if (window_kills >= g_config.min_kills) {
            float hr = (float)window_hs / (float)window_kills;
            if (hr > g_config.hs_ratio) {
                ns_add_score(idx, 20, "NS | Auto Detection: Aimbot (HS Ratio)", NS_DET_AIMBOT_HS_RATIO);
            }
            st->last_hs_ratio_check_kills = st->kills;
            st->last_hs_ratio_check_headshots = st->headshots;
        }
    }

    // Time-based HS streak
    if (st->hs_streak >= g_config.time_hs_streak &&
        ns_time_diff_sec(now_ms, st->first_hs_streak_time_ms) <= g_config.time_hs_window) {
        ns_add_score(idx, 20, "NS | Auto Detection: Aimbot (Time HS)", NS_DET_AIMBOT_HS_RATIO);
        st->hs_streak = 0;
    }

    // Time-based body lock
    if (st->bl_streak >= g_config.time_bl_streak &&
        ns_time_diff_sec(now_ms, st->first_bl_streak_time_ms) <= g_config.time_bl_window) {
        ns_add_score(idx, 30, "NS | Auto Detection: Aim Assist (Body Lock)", NS_DET_AIM_ASSIST_BODYLOCK);
        st->bl_streak = 0;
    }

    // Same hit location streak
    if (event->hit_location != NS_HITLOC_NONE) {
        if (event->hit_location == st->last_hit_loc) {
            st->same_hit_streak++;
        } else {
            st->same_hit_streak = 1;
            st->last_hit_loc = event->hit_location;
            st->first_same_hit_time_ms = now_ms;
        }
        if (st->same_hit_streak >= g_config.body_lock_streak &&
            ns_time_diff_sec(now_ms, st->first_same_hit_time_ms) <= g_config.time_bl_window &&
            ns_time_diff_sec(now_ms, st->last_body_lock_time_ms) > 15.0f) {
            ns_add_score(idx, 30, "NS | Auto Detection: Aim Assist (Body Lock)", NS_DET_AIM_ASSIST_BODYLOCK);
            st->last_body_lock_time_ms = now_ms;
            st->same_hit_streak = 0;
        }
    }

    // Snap + Kill
    if (st->last_snap_kill_time_ms > 0 &&
        ns_time_diff_sec(now_ms, st->last_snap_kill_time_ms) <= g_config.snap_kill_window) {
        ns_add_score(idx, 8, "NS | Auto Detection: Aimbot (Snap+Kill)", NS_DET_AIMBOT_SNAP_KILL);
        st->last_snap_kill_time_ms = 0;
    }

    // Moving headshot
    if (event->attacker_moving && event->distance > g_config.moving_hs_dist) {
        if (is_headshot) {
            st->moving_headshot_count++;
            if (st->moving_headshot_count >= g_config.moving_hs_streak &&
                ns_time_diff_sec(now_ms, st->last_moving_hs_time_ms) <= g_config.moving_hs_window) {
                ns_add_score(idx, 25, "NS | Auto Detection: Aimbot (Moving HS)", NS_DET_AIMBOT_MOVING_HS);
                st->moving_headshot_count = 0;
            }
            st->last_moving_hs_time_ms = now_ms;
        }
    } else {
        st->moving_headshot_count = 0;
    }

    // Perfect shot / silent aim angle
    if (event->distance > g_config.silent_aim_dist) {
        float dx = event->victim_origin.x - event->attacker_origin.x;
        float dy = event->victim_origin.y - event->attacker_origin.y;
        float dz = event->victim_origin.z - event->attacker_origin.z;
        float dist = event->distance;
        float iy = atan2f(dy, dx) * (180.0f / (float)M_PI);
        if (iy < 0) iy += 360.0f;
        float ip = -atan2f(dz, dist) * (180.0f / (float)M_PI);

        float ay = st->last_angles[1];
        float ap = st->last_angles[0];
        if (ap > 180.0f) ap -= 360.0f;

        float yaw_diff = ns_get_yaw_distance(ay, iy);
        float pitch_diff = fabsf(ap - ip);

        if (yaw_diff < g_config.perfect_yaw_pitch && pitch_diff < g_config.perfect_yaw_pitch) {
            if (st->perfect_shots > 0 && ns_time_diff_sec(now_ms, st->last_perfect_kill_time_ms) > 15.0f) {
                st->perfect_shots = 0;
            }
            st->perfect_shots++;
            st->last_perfect_kill_time_ms = now_ms;
        } else if (yaw_diff > g_config.silent_aim_angle || pitch_diff > g_config.silent_aim_angle) {
            st->perfect_shots = 0;
        }

        if (st->perfect_shots >= g_config.perfect_shots &&
            ns_time_diff_sec(now_ms, st->last_perfect_shot_time_ms) > 60.0f) {
            ns_add_score(idx, 20, "NS | Auto Detection: Aim Assist (Perfect Shot)", NS_DET_AIM_ASSIST_PERFECT_SHOT);
            st->last_perfect_shot_time_ms = now_ms;
            st->perfect_shots = 0;
        }
    } else {
        st->perfect_shots = 0;
    }

    // Hipfire long range
    if (!event->attacker_is_ads && event->distance > g_config.hipfire_dist) {
        if (st->hipfire_long_kills > 0 && ns_time_diff_sec(now_ms, st->last_hipfire_kill_time_ms) > 10.0f) {
            st->hipfire_long_kills = 0;
        }
        st->hipfire_long_kills++;
        st->last_hipfire_kill_time_ms = now_ms;
        if (st->hipfire_long_kills >= g_config.hipfire_kills &&
            ns_time_diff_sec(now_ms, st->last_hipfire_flag_time_ms) > 60.0f) {
            ns_add_score(idx, 25, "NS | Auto Detection: Silent Aim (Hipfire)", NS_DET_SILENT_AIM);
            st->last_hipfire_flag_time_ms = now_ms;
            st->hipfire_long_kills = 0;
        }
    }

    // Silent aim dot product
    if (st->has_shot_data && ns_time_diff_sec(now_ms, st->last_shot_time_ms) <= 2.0f) {
        float threshold = st->last_shot_ads ? g_config.silent_aim_threshold_ads : g_config.silent_aim_threshold_hipfire;

        float shot_origin[3];
        shot_origin[0] = st->last_shot_origin[0];
        shot_origin[1] = st->last_shot_origin[1];
        shot_origin[2] = st->last_shot_origin[2] + 55.0f;

        float victim_origin[3];
        victim_origin[0] = event->victim_origin.x;
        victim_origin[1] = event->victim_origin.y;
        victim_origin[2] = event->victim_origin.z + 55.0f;

        float dir[3] = {
            victim_origin[0] - shot_origin[0],
            victim_origin[1] - shot_origin[1],
            victim_origin[2] - shot_origin[2]
        };
        float len = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
        if (len > 0.0001f) {
            dir[0] /= len; dir[1] /= len; dir[2] /= len;
        }

        float forward[3];
        ns_angle_vectors(st->last_shot_angles, forward);
        float dot = forward[0]*dir[0] + forward[1]*dir[1] + forward[2]*dir[2];

        if (dot < threshold) {
            st->silent_aim_count++;
            if (ns_time_diff_sec(now_ms, st->last_silent_aim_time_ms) > 30.0f) {
                st->silent_aim_count = 1;
            }
            st->last_silent_aim_time_ms = now_ms;
            if (st->silent_aim_count >= g_config.silent_aim_streak) {
                ns_add_score(idx, 20, "NS | Auto Detection: Silent Aim", NS_DET_SILENT_AIM);
                st->silent_aim_count = 0;
            }
        }
        st->has_shot_data = false;
    }

    // No-spread detection
    bool suspicious_condition = (event->distance > 600.0f && (!event->attacker_is_ads || event->attacker_moving));
    if (suspicious_condition) {
        st->last_hit_positions[st->hit_pos_index][0] = event->victim_origin.x;
        st->last_hit_positions[st->hit_pos_index][1] = event->victim_origin.y;
        st->last_hit_positions[st->hit_pos_index][2] = event->victim_origin.z;
        st->hit_pos_index = (st->hit_pos_index + 1) % NS_MAX_HIT_POSITIONS;

        if (ns_time_diff_sec(now_ms, st->last_no_spread_hit_time_ms) < 1.5f) {
            float max_deviation = 0.0f;
            for (int i = 0; i < NS_MAX_HIT_POSITIONS - 1; i++) {
                for (int j = i + 1; j < NS_MAX_HIT_POSITIONS - 1; j++) {
                    int idx1 = (st->hit_pos_index - 1 - i + NS_MAX_HIT_POSITIONS) % NS_MAX_HIT_POSITIONS;
                    int idx2 = (st->hit_pos_index - 1 - j + NS_MAX_HIT_POSITIONS) % NS_MAX_HIT_POSITIONS;
                    float diff[3] = {
                        st->last_hit_positions[idx1][0] - st->last_hit_positions[idx2][0],
                        st->last_hit_positions[idx1][1] - st->last_hit_positions[idx2][1],
                        st->last_hit_positions[idx1][2] - st->last_hit_positions[idx2][2]
                    };
                    float d = sqrtf(diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2]);
                    if (d > max_deviation) max_deviation = d;
                }
            }
            if (max_deviation < g_config.nospread_max_deviation) {
                st->no_spread_hit_count++;
                if (st->no_spread_hit_count >= g_config.nospread_streak) {
                    ns_add_score(idx, 25, "NS | Auto Detection: No-Spread", NS_DET_NO_SPREAD);
                    st->no_spread_hit_count = 0;
                }
            } else {
                st->no_spread_hit_count = 0;
            }
        }
        st->last_no_spread_hit_time_ms = now_ms;
    } else {
        st->no_spread_hit_count = 0;
    }

    // Target switch
    if (st->last_kill_time_ms > 0 && ns_time_diff_ms(now_ms, st->last_kill_time_ms) < 5000) {
        float time_between = (float)ns_time_diff_ms(now_ms, st->last_kill_time_ms);
        float camera_turn_angle = ns_get_yaw_distance(st->last_kill_view_angles[1], st->last_angles[1]);
        if (time_between > 50.0f &&
            time_between < (g_config.target_switch_time * 1000.0f) &&
            camera_turn_angle > g_config.target_switch_angle &&
            event->distance > 400.0f) {
            st->rapid_switch_kills++;
            if (st->rapid_switch_kills >= g_config.target_switch_streak) {
                ns_add_score(idx, 10, "NS | Auto Detection: Aim Assist (Target Switch)", NS_DET_AIM_ASSIST_TARGET_SWITCH);
                st->rapid_switch_kills = 0;
            }
        } else {
            if (time_between > (g_config.target_switch_time * 1000.0f) ||
                camera_turn_angle < g_config.target_switch_angle) {
                st->rapid_switch_kills = 0;
            }
        }
    }

    st->last_kill_time_ms = now_ms;
    st->last_kill_view_angles[0] = st->last_angles[0];
    st->last_kill_view_angles[1] = st->last_angles[1];
    st->last_kill_view_angles[2] = st->last_angles[2];

    pthread_mutex_unlock(&g_core_mutex);
}