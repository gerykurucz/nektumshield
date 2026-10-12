/*
 * Nektum Shield - COD4X Adapter
 * Version: 1.0
 *
 * This adapter connects the universal NS core to Call of Duty 4 X.
 * It captures game events, converts them to normalized structures,
 * and forwards them to the core. Detections are logged, and when
 * the ban threshold is reached, the player is dropped.
 *
 * All admin commands and persistent banlist are omitted.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdarg.h>

#include "api/pinc.h"        // COD4X plugin API
#include "../../main/ns_core.h"     // Universal anti-cheat core

#define LOG_FILE "nektumshield_adapter.log"
#define DROP_DELAY_MS 5000
#define MAXP 64

/* Fontos: a gclient_t típus előre deklarálása, ahogy az eredeti plugin is tette */
typedef struct gclient_s gclient_t;

static ns_config_t g_core_config;
static ns_core_callbacks_t g_core_callbacks;
static FILE *g_log_file = NULL;

static uint64_t g_client_to_playerid[MAXP];
static bool g_client_active[MAXP];

typedef struct {
    uint64_t player_id;
    int scheduled_drop_time;
    char reason[256];
} PendingBan;

static PendingBan g_pending_bans[MAXP];
static int g_num_pending = 0;

static CONVAR_T *cv_score_ban;
static CONVAR_T *cv_drop_delay_ms;

/* Segédfüggvények */
static bool IsBot(client_t *cl) {
    if (!cl) return true;
    if (cl->netchan.remoteAddress.type == NA_BOT) return true;
    if (cl->steamid == 0 && cl->playerid == 0) return true;
    return false;
}

static float ShortToAngle(int s) {
    return (float)(s & 65535) * (360.0f / 65536.0f);
}

static void WriteLog(const char* fmt, ...) {
    if (!g_log_file) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    fprintf(g_log_file, "[%02d:%02d:%02d] ", t->tm_hour, t->tm_min, t->tm_sec);
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    va_end(args);
    fprintf(g_log_file, "\n");
    fflush(g_log_file);
}

static void OnDetection(const ns_detection_result_t* det) {
    Plugin_Printf("^3[Nektum Shield] ^7Detection: ^2%s ^7| Score +%d | PlayerID: %llu\n",
                  det->reason, det->score_added, (unsigned long long)det->player_id);
    WriteLog("DETECT: PID=%llu | Score+%d | %s",
             (unsigned long long)det->player_id, det->score_added, det->reason);
}

static void OnBanThreshold(ns_player_id_t player_id, int score, const char* reason) {
    int client_num = -1;
    for (int i = 0; i < MAXP; i++) {
        if (g_client_active[i] && g_client_to_playerid[i] == player_id) {
            client_num = i;
            break;
        }
    }
    if (client_num == -1) return;

    Plugin_Printf("^1[Nektum Shield] ^7Ban threshold reached for PID=%llu (Score %d). Reason: %s\n",
                  (unsigned long long)player_id, score, reason);
    WriteLog("BAN THRESHOLD: PID=%llu | Score=%d | %s",
             (unsigned long long)player_id, score, reason);

    if (g_num_pending < MAXP) {
        g_pending_bans[g_num_pending].player_id = player_id;
        g_pending_bans[g_num_pending].scheduled_drop_time = Plugin_Milliseconds() + DROP_DELAY_MS;
        snprintf(g_pending_bans[g_num_pending].reason,
                 sizeof(g_pending_bans[g_num_pending].reason), "%s", reason);
        g_num_pending++;
    }
}

static void OnCoreLog(const char* message) {
    WriteLog("%s", message);
}

static void InitCore(void) {
    memset(&g_core_config, 0, sizeof(g_core_config));
    g_core_config.score_ban = 30;
    g_core_config.snap_threshold = 150.0f;
    g_core_config.snap_kill_angle = 70.0f;
    g_core_config.snap_kill_window = 0.5f;
    g_core_config.hs_ratio = 0.8f;
    g_core_config.min_kills = 40;
    g_core_config.time_hs_streak = 6;
    g_core_config.time_hs_window = 10.0f;
    g_core_config.time_bl_streak = 10;
    g_core_config.time_bl_window = 10.0f;
    g_core_config.body_lock_streak = 15;
    g_core_config.moving_hs_dist = 750.0f;
    g_core_config.moving_hs_streak = 5;
    g_core_config.moving_hs_window = 15.0f;
    g_core_config.silent_aim_angle = 3.5f;
    g_core_config.silent_aim_dist = 500.0f;
    g_core_config.perfect_yaw_pitch = 2.5f;
    g_core_config.perfect_shots = 7;
    g_core_config.silent_aim_threshold_hipfire = 0.30f;
    g_core_config.silent_aim_threshold_ads = 0.80f;
    g_core_config.silent_aim_streak = 4;
    g_core_config.hipfire_kills = 10;
    g_core_config.hipfire_dist = 4000.0f;
    g_core_config.norecoil_threshold = 0.998f;
    g_core_config.recoil_macro_var = 0.3f;
    g_core_config.recoil_macro_mean = -1.0f;
    g_core_config.recoil_samples = 7;
    g_core_config.ads_norecoil_frames = 265;
    g_core_config.macro_logic_clicks = 5;
    g_core_config.macro_logic_window = 350;
    g_core_config.macro_logic_strikes = 2.0f;
    g_core_config.macro_scroll_count = 4;
    g_core_config.macro_scroll_fast = 70;
    g_core_config.macro_scroll_reset = 110;
    g_core_config.macro_rate_count = 7;
    g_core_config.macro_rate_fast = 90;
    g_core_config.macro_consistency_count = 5;
    g_core_config.macro_consistency_gap = 150;
    g_core_config.nospread_max_deviation = 20.0f;
    g_core_config.nospread_streak = 4;
    g_core_config.target_switch_time = 0.05f;
    g_core_config.target_switch_angle = 50.0f;
    g_core_config.target_switch_streak = 4;

    memset(&g_core_callbacks, 0, sizeof(g_core_callbacks));
    g_core_callbacks.on_detection = OnDetection;
    g_core_callbacks.on_ban_threshold = OnBanThreshold;
    g_core_callbacks.on_log = OnCoreLog;

    ns_core_init(&g_core_config, &g_core_callbacks);
}

static void InitCvars(void) {
    cv_score_ban = Plugin_Cvar_RegisterInt("ns_score_ban", 30, 10, 100, 0, "Ban threshold for NS core");
    cv_drop_delay_ms = Plugin_Cvar_RegisterInt("ns_drop_delay_ms", 5000, 1000, 30000, 0, "Delay before drop");
}

static void UpdateConfigFromCvars(void) {
    ns_config_t config = g_core_config;
    config.score_ban = Plugin_Cvar_GetInteger(cv_score_ban);
    ns_core_set_config(&config);
}

PCL void OnClientEnterWorld(client_t* client) {
    if (!client || !client->gentity || IsBot(client)) return;
    int clientNum = client->gentity->s.clientNum;
    if (clientNum < 0 || clientNum >= MAXP) return;

    uint64_t pid = client->playerid;
    if (pid == 0) {
        pid = (uint64_t)(clientNum + 1) * 1000000;
    }

    ns_core_register_player(pid);
    ns_core_reset_player(pid);
    g_client_to_playerid[clientNum] = pid;
    g_client_active[clientNum] = true;
}

PCL void OnClientDC(client_t* client, const char* reason) {
    (void)reason;
    if (!client || !client->gentity || IsBot(client)) return;
    int clientNum = client->gentity->s.clientNum;
    if (clientNum < 0 || clientNum >= MAXP) return;

    if (g_client_active[clientNum]) {
        ns_core_unregister_player(g_client_to_playerid[clientNum]);
        g_client_active[clientNum] = false;
    }
}

PCL void OnClientMoveCommand(client_t* client, usercmd_t* ucmd) {
    if (!client || !client->gentity || IsBot(client)) return;
    int clientNum = client->gentity->s.clientNum;
    if (clientNum < 0 || clientNum >= MAXP || !g_client_active[clientNum]) return;

    gclient_t* gcl = client->gentity->client;
    if (!gcl || gcl->sess.sessionState != STATE_PLAYING) return;

    ns_frame_event_t fev;
    memset(&fev, 0, sizeof(fev));
    fev.player_id = g_client_to_playerid[clientNum];
    fev.timestamp_ms = Plugin_Milliseconds();

    fev.view_angles[0] = ShortToAngle(ucmd->angles[0]);
    fev.view_angles[1] = ShortToAngle(ucmd->angles[1]);
    fev.view_angles[2] = 0.0f;

    fev.origin.x = client->gentity->r.currentOrigin[0];
    fev.origin.y = client->gentity->r.currentOrigin[1];
    fev.origin.z = client->gentity->r.currentOrigin[2];

    fev.buttons = 0;
    if (ucmd->buttons & 1) fev.buttons |= NS_BUTTON_FIRE;
    if (ucmd->buttons & 256) fev.buttons |= NS_BUTTON_ADS;
    if (ucmd->forwardmove > 0) fev.buttons |= NS_BUTTON_MOVE_FWD;
    if (ucmd->forwardmove < 0) fev.buttons |= NS_BUTTON_MOVE_BACK;
    if (ucmd->rightmove > 0) fev.buttons |= NS_BUTTON_MOVE_RIGHT;
    if (ucmd->rightmove < 0) fev.buttons |= NS_BUTTON_MOVE_LEFT;

    fev.forwardmove = (float)ucmd->forwardmove / 127.0f;
    fev.rightmove = (float)ucmd->rightmove / 127.0f;

    ns_core_on_frame(&fev);
}

PCL void OnPlayerKilled(gentity_t* self, gentity_t* inflictor, gentity_t* attacker,
                        int damage, int meansOfDeath, int iWeapon, hitLocation_t hitLocation) {
    (void)inflictor; (void)damage; (void)iWeapon;
    if (!self || !attacker || !attacker->client || attacker == self) return;

    gclient_t* attacker_gcl = attacker->client;
    team_t at = attacker_gcl->sess.cs.team;
    gclient_t* victim_gcl = self->client;
    team_t vt = (victim_gcl) ? victim_gcl->sess.cs.team : TEAM_FREE;
    if (at != TEAM_FREE && at == vt) return;

    int attackerNum = attacker->s.clientNum;
    if (attackerNum < 0 || attackerNum >= MAXP || !g_client_active[attackerNum]) return;

    client_t* cl = Plugin_GetClientForClientNum(attackerNum);
    if (!cl || IsBot(cl)) return;

    if (attacker_gcl->sess.sessionState != STATE_PLAYING) return;

    ns_kill_event_t kev;
    memset(&kev, 0, sizeof(kev));
    kev.attacker_id = g_client_to_playerid[attackerNum];

    if (self->client) {
        int victimNum = self->s.clientNum;
        client_t* victim_cl = Plugin_GetClientForClientNum(victimNum);
        kev.victim_id = victim_cl ? victim_cl->playerid : 0;
    } else {
        kev.victim_id = 0;
    }
    kev.timestamp_ms = Plugin_Milliseconds();

    kev.victim_origin.x = self->r.currentOrigin[0];
    kev.victim_origin.y = self->r.currentOrigin[1];
    kev.victim_origin.z = self->r.currentOrigin[2];

    kev.attacker_origin.x = attacker->r.currentOrigin[0];
    kev.attacker_origin.y = attacker->r.currentOrigin[1];
    kev.attacker_origin.z = attacker->r.currentOrigin[2];

    switch (hitLocation) {
        case HITLOC_HEAD:
        case HITLOC_HELMET:
        case HITLOC_NECK:
            kev.hit_location = NS_HITLOC_HEAD;
            break;
        case HITLOC_TORSO_UPR:
        case HITLOC_TORSO_LWR:
            kev.hit_location = NS_HITLOC_TORSO_UPPER;
            break;
        default:
            kev.hit_location = NS_HITLOC_LIMB;
            break;
    }

    kev.means_of_death = meansOfDeath;

    float dx = kev.victim_origin.x - kev.attacker_origin.x;
    float dy = kev.victim_origin.y - kev.attacker_origin.y;
    kev.distance = sqrtf(dx*dx + dy*dy);

    kev.attacker_is_ads = (cl->lastUsercmd.buttons & 256) != 0;
    kev.attacker_moving = (cl->lastUsercmd.forwardmove != 0 || cl->lastUsercmd.rightmove != 0);

    ns_core_on_kill(&kev);
}

PCL void OnFrame(void) {
    int now = Plugin_Milliseconds();

    for (int i = 0; i < g_num_pending; i++) {
        PendingBan *pb = &g_pending_bans[i];
        if (now >= pb->scheduled_drop_time) {
            int clientNum = -1;
            for (int j = 0; j < MAXP; j++) {
                if (g_client_active[j] && g_client_to_playerid[j] == pb->player_id) {
                    clientNum = j;
                    break;
                }
            }
            if (clientNum != -1) {
                client_t* cl = Plugin_GetClientForClientNum(clientNum);
                if (cl && !IsBot(cl)) {
                    Plugin_DropClient(clientNum, pb->reason);
                }
            }
            for (int k = i; k < g_num_pending - 1; k++) {
                g_pending_bans[k] = g_pending_bans[k+1];
            }
            g_num_pending--;
            i--;
        }
    }

    static int last_tick = 0;
    if (now - last_tick >= 1000) {
        ns_core_tick(now);
        last_tick = now;
    }
}

PCL int OnInit(void) {
    char home[512], path[600];
    Plugin_Cvar_VariableStringBuffer("fs_homepath", home, sizeof(home));
    snprintf(path, sizeof(path), "%s/%s", home, LOG_FILE);
    g_log_file = fopen(path, "a+");
    if (!g_log_file) {
        Plugin_Printf("^1[Nektum Shield Adapter] ^7Failed to open log file.\n");
        return 1;
    }

    InitCore();
    InitCvars();
    UpdateConfigFromCvars();

    Plugin_Printf("^2Nektum Shield Core Adapter loaded.\n");
    return 0;
}

PCL void OnTerminate(void) {
    ns_core_shutdown();
    if (g_log_file) {
        fclose(g_log_file);
        g_log_file = NULL;
    }
}

PCL void OnInfoRequest(pluginInfo_t* info) {
    info->handlerVersion.major = PLUGIN_HANDLER_VERSION_MAJOR;
    info->handlerVersion.minor = PLUGIN_HANDLER_VERSION_MINOR;
    info->pluginVersion.major = 1;
    info->pluginVersion.minor = 0;
    snprintf(info->fullName, sizeof(info->fullName), "Nektum Shield Core Adapter");
    snprintf(info->shortDescription, sizeof(info->shortDescription), "COD4X adapter for universal NS core");
}