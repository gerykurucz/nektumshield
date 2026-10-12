package main

import (
    "fmt"
    "log"
    "net/http"
)

func main() {
    cfg := LoadConfig()

    db, err := ConnectDB(cfg)
    if err != nil {
        log.Fatalf("Database connection failed: %v", err)
    }
    defer db.Close()

    pool := NewWorkerPool(db, cfg.WorkerCount)
    pool.Start()
    defer pool.Stop()

    knownEvents := map[string]bool{
        "player_connect": true,
        "player_disconnect": true,
        "shot_fired": true,
        "kill": true,
        "damage": true,
        "detection": true,
        "moderation": true,
        "session_info": true,
    }

    http.HandleFunc("/events", func(w http.ResponseWriter, r *http.Request) {
        if r.Method != http.MethodPost {
            http.Error(w, "Method not allowed", http.StatusMethodNotAllowed)
            return
        }
        evt, err := ParseEvent(r.Body)
        if err != nil {
            http.Error(w, "Invalid JSON", http.StatusBadRequest)
            return
        }

        if _, ok := knownEvents[evt.Type]; !ok {
            http.Error(w, "Unknown event type", http.StatusBadRequest)
            return
        }

        if evt.Type == "player_connect" {
            nektumID, err := ResolveNektumID(db, evt)
            if err != nil {
                http.Error(w, "Failed to resolve nektum ID", http.StatusInternalServerError)
                return
            }
            w.Header().Set("Content-Type", "application/json")
            w.WriteHeader(http.StatusOK)
            fmt.Fprintf(w, `{"nektum_id":%d}`, nektumID)
            return
        }

        pool.Submit(evt)
        w.WriteHeader(http.StatusAccepted)
    })

    http.HandleFunc("/healthz", func(w http.ResponseWriter, r *http.Request) {
        w.WriteHeader(http.StatusOK)
        w.Write([]byte("OK"))
    })

    addr := "0.0.0.0:" + cfg.Port
    log.Printf("Nektum Shield is listening on %s", addr)
    log.Fatal(http.ListenAndServe(addr, nil))
}