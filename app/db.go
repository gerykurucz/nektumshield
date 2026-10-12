package main

import (
    "database/sql"
    "fmt"
    "log"
    "strings"
    "time"

    _ "github.com/go-sql-driver/mysql"
)

func ConnectDB(cfg Config) (*sql.DB, error) {
    dsn := fmt.Sprintf("%s:%s@tcp(%s)/%s?parseTime=true&charset=utf8mb4",
        cfg.DBUser, cfg.DBPassword, cfg.DBHost, cfg.DBName)

    db, err := sql.Open("mysql", dsn)
    if err != nil {
        return nil, err
    }

    db.SetMaxOpenConns(20)
    db.SetMaxIdleConns(5)
    db.SetConnMaxLifetime(5 * time.Minute)

    if err := db.Ping(); err != nil {
        db.Close()
        return nil, err
    }
    return db, nil
}

// ResolveNektumID meghatározza a nektum_id-t a megadott azonosítók alapján.
// Ha egyik azonosító sem ismert, új játékost hoz létre, és visszaadja az új ID-t.
func ResolveNektumID(db *sql.DB, evt *Event) (int, error) {
    // Kinyerjük az azonosítókat a Data-ból
    playerID := getUint64FromData(evt.Data, "player_id")
    steamID := getUint64FromData(evt.Data, "steam_id")
    playerName := getStringFromData(evt.Data, "player_name")
    ipHash := getStringFromData(evt.Data, "ip_hash")
    subnet24 := getStringFromData(evt.Data, "subnet24")

    // Megpróbáljuk megtalálni a meglévő nektum_id-t
    nektumID, err := findNektumID(db, playerID, steamID, playerName, ipHash, subnet24)
    if err != nil {
        return 0, err
    }
    if nektumID != 0 {
        // Frissítjük a last_seen mezőt a players táblában
        _, err := db.Exec("UPDATE players SET last_seen = ? WHERE nektum_id = ?", time.UnixMilli(evt.Timestamp), nektumID)
        if err != nil {
            return 0, err
        }
        // Frissítjük az azonosítók last_seen-jét
        if err = updateIdentifiersLastSeen(db, nektumID, playerID, steamID, playerName, ipHash, subnet24, time.UnixMilli(evt.Timestamp)); err != nil {
            return 0, err
        }
        return nektumID, nil
    }

    // Új játékos létrehozása tranzakcióban
    tx, err := db.Begin()
    if err != nil {
        return 0, err
    }
    defer tx.Rollback()

    // Új players sor
    res, err := tx.Exec("INSERT INTO players (first_seen, last_seen, total_playtime_seconds, total_kills, total_deaths, total_assists, total_shots_fired, total_shots_hit, total_detections, total_warnings, total_restrictions, total_bans, is_active) VALUES (?, ?, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1)",
        time.UnixMilli(evt.Timestamp), time.UnixMilli(evt.Timestamp))
    if err != nil {
        return 0, err
    }
    newID, err := res.LastInsertId()
    if err != nil {
        return 0, err
    }

    // Azonosítók beszúrása
    if err = insertIdentifier(tx, int(newID), "playerid", fmt.Sprintf("%d", playerID), time.UnixMilli(evt.Timestamp)); err != nil {
        return 0, err
    }
    if steamID != 0 {
        if err = insertIdentifier(tx, int(newID), "steamid", fmt.Sprintf("%d", steamID), time.UnixMilli(evt.Timestamp)); err != nil {
            return 0, err
        }
    }
    if playerName != "" {
        if err = insertIdentifier(tx, int(newID), "name", playerName, time.UnixMilli(evt.Timestamp)); err != nil {
            return 0, err
        }
    }
    if ipHash != "" {
        if err = insertIdentifier(tx, int(newID), "ip_hash", ipHash, time.UnixMilli(evt.Timestamp)); err != nil {
            return 0, err
        }
    }
    if subnet24 != "" {
        if err = insertIdentifier(tx, int(newID), "subnet24", subnet24, time.UnixMilli(evt.Timestamp)); err != nil {
            return 0, err
        }
    }

    if err = tx.Commit(); err != nil {
        return 0, err
    }

    return int(newID), nil
}

// findNektumID megkeresi a nektum_id-t a lehetséges azonosítók alapján.
func findNektumID(db *sql.DB, playerID uint64, steamID uint64, playerName, ipHash, subnet24 string) (int, error) {
    // Sorban ellenőrizzük az azonosítókat, mert az egyedi kulcs miatt gyors.
    // Először a legerősebb azonosítókat nézzük.
    var id int

    if playerID != 0 {
        err := db.QueryRow("SELECT nektum_id FROM player_identifiers WHERE identifier_type='playerid' AND identifier_value=?", fmt.Sprintf("%d", playerID)).Scan(&id)
        if err == nil {
            return id, nil
        }
        if err != sql.ErrNoRows {
            return 0, err
        }
    }

    if steamID != 0 {
        err := db.QueryRow("SELECT nektum_id FROM player_identifiers WHERE identifier_type='steamid' AND identifier_value=?", fmt.Sprintf("%d", steamID)).Scan(&id)
        if err == nil {
            return id, nil
        }
        if err != sql.ErrNoRows {
            return 0, err
        }
    }

    if playerName != "" {
        err := db.QueryRow("SELECT nektum_id FROM player_identifiers WHERE identifier_type='name' AND identifier_value=?", playerName).Scan(&id)
        if err == nil {
            return id, nil
        }
        if err != sql.ErrNoRows {
            return 0, err
        }
    }

    if ipHash != "" {
        err := db.QueryRow("SELECT nektum_id FROM player_identifiers WHERE identifier_type='ip_hash' AND identifier_value=?", ipHash).Scan(&id)
        if err == nil {
            return id, nil
        }
        if err != sql.ErrNoRows {
            return 0, err
        }
    }

    if subnet24 != "" {
        err := db.QueryRow("SELECT nektum_id FROM player_identifiers WHERE identifier_type='subnet24' AND identifier_value=?", subnet24).Scan(&id)
        if err == nil {
            return id, nil
        }
        if err != sql.ErrNoRows {
            return 0, err
        }
    }

    return 0, nil // nem találtuk
}

// updateIdentifiersLastSeen frissíti a last_seen értékét a játékos összes azonosítóján.
func updateIdentifiersLastSeen(db *sql.DB, nektumID int, playerID uint64, steamID uint64, playerName, ipHash, subnet24 string, ts time.Time) error {
    // Minden azonosítóra frissítünk, ha szerepel
    if playerID != 0 {
        if _, err := db.Exec("UPDATE player_identifiers SET last_seen=? WHERE identifier_type='playerid' AND identifier_value=?", ts, fmt.Sprintf("%d", playerID)); err != nil {
            return err
        }
    }
    if steamID != 0 {
        if _, err := db.Exec("UPDATE player_identifiers SET last_seen=? WHERE identifier_type='steamid' AND identifier_value=?", ts, fmt.Sprintf("%d", steamID)); err != nil {
            return err
        }
    }
    if playerName != "" {
        if _, err := db.Exec("UPDATE player_identifiers SET last_seen=? WHERE identifier_type='name' AND identifier_value=?", ts, playerName); err != nil {
            return err
        }
    }
    if ipHash != "" {
        if _, err := db.Exec("UPDATE player_identifiers SET last_seen=? WHERE identifier_type='ip_hash' AND identifier_value=?", ts, ipHash); err != nil {
            return err
        }
    }
    if subnet24 != "" {
        if _, err := db.Exec("UPDATE player_identifiers SET last_seen=? WHERE identifier_type='subnet24' AND identifier_value=?", ts, subnet24); err != nil {
            return err
        }
    }
    return nil
}

// insertIdentifier beszúr egy azonosítót a player_identifiers táblába.
func insertIdentifier(tx *sql.Tx, nektumID int, identType, identValue string, ts time.Time) error {
    _, err := tx.Exec("INSERT INTO player_identifiers (nektum_id, identifier_type, identifier_value, first_seen, last_seen) VALUES (?, ?, ?, ?, ?)",
        nektumID, identType, identValue, ts, ts)
    return err
}

// InsertEvent egy eseményt dolgoz fel és ír be az adatbázisba.
func InsertEvent(db *sql.DB, evt *Event) error {
    switch evt.Type {
    case "player_disconnect":
        return handlePlayerDisconnect(db, evt)
    case "shot_fired":
        return handleShotFired(db, evt)
    case "kill":
        return handleKill(db, evt)
    case "damage":
        return handleDamage(db, evt)
    case "detection":
        return handleDetection(db, evt)
    case "moderation":
        return handleModeration(db, evt)
    default:
        log.Printf("Unknown event type: %s", evt.Type)
        return fmt.Errorf("unknown event type: %s", evt.Type)
    }
}

func handlePlayerDisconnect(db *sql.DB, evt *Event) error {
    // A C oldal küldi a nektum_id-t és a session időtartamát másodpercben.
    nektumID := evt.NektumID
    durationSeconds := getIntFromData(evt.Data, "session_duration_seconds")
    if durationSeconds < 0 {
        durationSeconds = 0
    }

    // Lezárjuk a legutolsó nyitott sessiont
    _, err := db.Exec("UPDATE sessions SET end_time=?, duration_seconds=? WHERE nektum_id=? AND end_time IS NULL ORDER BY start_time DESC LIMIT 1",
        time.UnixMilli(evt.Timestamp), durationSeconds, nektumID)
    if err != nil {
        return err
    }

    // Total playtime növelése
    _, err = db.Exec("UPDATE players SET total_playtime_seconds = total_playtime_seconds + ? WHERE nektum_id=?", durationSeconds, nektumID)
    if err != nil {
        return err
    }

    // Napi playtime növelése
    dateStr := time.UnixMilli(evt.Timestamp).Format("2006-01-02")
    _, err = db.Exec(`INSERT INTO daily_stats (nektum_id, stat_date, kills, deaths, assists, playtime_seconds, shots_fired, shots_hit, detections)
                      VALUES (?, ?, 0, 0, 0, ?, 0, 0, 0)
                      ON DUPLICATE KEY UPDATE playtime_seconds = playtime_seconds + ?`,
        nektumID, dateStr, durationSeconds, durationSeconds)
    return err
}

func handleShotFired(db *sql.DB, evt *Event) error {
    nektumID := evt.NektumID

    // Total shots_fired növelése
    _, err := db.Exec("UPDATE players SET total_shots_fired = total_shots_fired + 1 WHERE nektum_id=?", nektumID)
    if err != nil {
        return err
    }

    // Napi shots_fired növelése
    dateStr := time.UnixMilli(evt.Timestamp).Format("2006-01-02")
    _, err = db.Exec(`INSERT INTO daily_stats (nektum_id, stat_date, kills, deaths, assists, playtime_seconds, shots_fired, shots_hit, detections)
                      VALUES (?, ?, 0, 0, 0, 0, 1, 0, 0)
                      ON DUPLICATE KEY UPDATE shots_fired = shots_fired + 1`,
        nektumID, dateStr)
    return err
}

func handleKill(db *sql.DB, evt *Event) error {
    attackerNektumID := evt.NektumID
    victimNektumID := getIntFromData(evt.Data, "victim_nektum_id")
    hitLocation := getIntFromData(evt.Data, "hit_location")
    // weaponMod := getIntFromData(evt.Data, "weapon_mod")  // opcionális
    // distance := getFloatFromData(evt.Data, "distance")    // opcionális

    // Attacker kills növelése
    _, err := db.Exec("UPDATE players SET total_kills = total_kills + 1 WHERE nektum_id=?", attackerNektumID)
    if err != nil {
        return err
    }

    // Victim deaths növelése, ha ismert
    if victimNektumID != 0 {
        _, err = db.Exec("UPDATE players SET total_deaths = total_deaths + 1 WHERE nektum_id=?", victimNektumID)
        if err != nil {
            return err
        }
    }

    // Napi aggregátumok
    dateStr := time.UnixMilli(evt.Timestamp).Format("2006-01-02")
    _, err = db.Exec(`INSERT INTO daily_stats (nektum_id, stat_date, kills, deaths, assists, playtime_seconds, shots_fired, shots_hit, detections)
                      VALUES (?, ?, 1, 0, 0, 0, 0, 0, 0)
                      ON DUPLICATE KEY UPDATE kills = kills + 1`,
        attackerNektumID, dateStr)
    if err != nil {
        return err
    }
    if victimNektumID != 0 {
        _, err = db.Exec(`INSERT INTO daily_stats (nektum_id, stat_date, kills, deaths, assists, playtime_seconds, shots_fired, shots_hit, detections)
                          VALUES (?, ?, 0, 1, 0, 0, 0, 0, 0)
                          ON DUPLICATE KEY UPDATE deaths = deaths + 1`,
            victimNektumID, dateStr)
        if err != nil {
            return err
        }
    }

    // Hit location statisztika növelése az attackerre
    if hitLocation > 0 {
        _, err = db.Exec(`INSERT INTO hit_location_stats (nektum_id, hit_location, hit_count, kill_count)
                          VALUES (?, ?, 1, 1)
                          ON DUPLICATE KEY UPDATE hit_count = hit_count + 1, kill_count = kill_count + 1`,
            attackerNektumID, hitLocation)
        if err != nil {
            return err
        }
    }

    return nil
}

func handleDamage(db *sql.DB, evt *Event) error {
    attackerNektumID := evt.NektumID
    hitLocation := getIntFromData(evt.Data, "hit_location")
    killed := getBoolFromData(evt.Data, "killed")

    // Shots hit növelése
    _, err := db.Exec("UPDATE players SET total_shots_hit = total_shots_hit + 1 WHERE nektum_id=?", attackerNektumID)
    if err != nil {
        return err
    }

    // Napi shots_hit növelése
    dateStr := time.UnixMilli(evt.Timestamp).Format("2006-01-02")
    _, err = db.Exec(`INSERT INTO daily_stats (nektum_id, stat_date, kills, deaths, assists, playtime_seconds, shots_fired, shots_hit, detections)
                      VALUES (?, ?, 0, 0, 0, 0, 0, 1, 0)
                      ON DUPLICATE KEY UPDATE shots_hit = shots_hit + 1`,
        attackerNektumID, dateStr)
    if err != nil {
        return err
    }

    // Hit location stat növelése
    if hitLocation > 0 {
        killIncrement := 0
        if killed {
            killIncrement = 1
        }
        _, err = db.Exec(`INSERT INTO hit_location_stats (nektum_id, hit_location, hit_count, kill_count)
                          VALUES (?, ?, 1, ?)
                          ON DUPLICATE KEY UPDATE hit_count = hit_count + 1, kill_count = kill_count + ?`,
            attackerNektumID, hitLocation, killIncrement, killIncrement)
        if err != nil {
            return err
        }
    }

    return nil
}

func handleDetection(db *sql.DB, evt *Event) error {
    nektumID := evt.NektumID
    detectionType := getIntFromData(evt.Data, "detection_type")
    reason := getStringFromData(evt.Data, "reason")
    scoreAdded := getIntFromData(evt.Data, "score_added")

    // Naplózás a detection_log táblába
    _, err := db.Exec("INSERT INTO detection_log (nektum_id, detection_type, reason, score_added, timestamp) VALUES (?, ?, ?, ?, ?)",
        nektumID, detectionType, reason, scoreAdded, time.UnixMilli(evt.Timestamp))
    if err != nil {
        return err
    }

    // Total detections növelése
    _, err = db.Exec("UPDATE players SET total_detections = total_detections + 1 WHERE nektum_id=?", nektumID)
    if err != nil {
        return err
    }

    // Napi detections növelése
    dateStr := time.UnixMilli(evt.Timestamp).Format("2006-01-02")
    _, err = db.Exec(`INSERT INTO daily_stats (nektum_id, stat_date, kills, deaths, assists, playtime_seconds, shots_fired, shots_hit, detections)
                      VALUES (?, ?, 0, 0, 0, 0, 0, 0, 1)
                      ON DUPLICATE KEY UPDATE detections = detections + 1`,
        nektumID, dateStr)
    return err
}

func handleModeration(db *sql.DB, evt *Event) error {
    nektumID := evt.NektumID
    action := strings.ToLower(getStringFromData(evt.Data, "action"))
    reason := getStringFromData(evt.Data, "reason")
    adminName := getStringFromData(evt.Data, "admin_name")

    // Ellenőrizzük az action értékét
    if action != "warn" && action != "restrict" && action != "ban" && action != "unban" {
        return fmt.Errorf("invalid moderation action: %s", action)
    }

    // Naplózás a moderation_actions táblába
    _, err := db.Exec("INSERT INTO moderation_actions (nektum_id, action_type, reason, admin_name, timestamp) VALUES (?, ?, ?, ?, ?)",
        nektumID, action, reason, adminName, time.UnixMilli(evt.Timestamp))
    if err != nil {
        return err
    }

    // Players tábla frissítése az akció típusától függően
    switch action {
    case "warn":
        _, err = db.Exec("UPDATE players SET total_warnings = total_warnings + 1 WHERE nektum_id=?", nektumID)
    case "restrict":
        _, err = db.Exec("UPDATE players SET total_restrictions = total_restrictions + 1 WHERE nektum_id=?", nektumID)
    case "ban":
        _, err = db.Exec("UPDATE players SET total_bans = total_bans + 1, is_active = 0 WHERE nektum_id=?", nektumID)
    case "unban":
        _, err = db.Exec("UPDATE players SET total_bans = GREATEST(total_bans - 1, 0), is_active = 1 WHERE nektum_id=?", nektumID)
    }

    return err
}

// Segédfüggvények a Data map-ből való típusos kinyeréshez
func getStringFromData(data map[string]interface{}, key string) string {
    if v, ok := data[key]; ok {
        if s, ok := v.(string); ok {
            return s
        }
        return fmt.Sprintf("%v", v)
    }
    return ""
}

func getIntFromData(data map[string]interface{}, key string) int {
    if v, ok := data[key]; ok {
        switch n := v.(type) {
        case int:
            return n
        case int64:
            return int(n)
        case float64:
            return int(n)
        case uint64:
            return int(n)
        case string:
            var i int
            fmt.Sscanf(n, "%d", &i)
            return i
        }
    }
    return 0
}

func getUint64FromData(data map[string]interface{}, key string) uint64 {
    if v, ok := data[key]; ok {
        switch n := v.(type) {
        case uint64:
            return n
        case int64:
            return uint64(n)
        case int:
            return uint64(n)
        case float64:
            return uint64(n)
        case string:
            var i uint64
            fmt.Sscanf(n, "%d", &i)
            return i
        }
    }
    return 0
}

func getBoolFromData(data map[string]interface{}, key string) bool {
    if v, ok := data[key]; ok {
        if b, ok := v.(bool); ok {
            return b
        }
        if n, ok := v.(int); ok {
            return n != 0
        }
        if s, ok := v.(string); ok {
            return strings.ToLower(s) == "true" || s == "1"
        }
    }
    return false
}