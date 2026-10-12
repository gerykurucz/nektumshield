package main

import (
    "encoding/json"
    "io"
)

type Event struct {
    Type      string                 `json:"event"`
    Timestamp int64                  `json:"timestamp"` // Unix milliseconds
    ServerID  string                 `json:"server_id,omitempty"`
    PlayerID  uint64                 `json:"player_id,omitempty"`
    NektumID  int                    `json:"nektum_id,omitempty"`
    Data      map[string]interface{} `json:"data,omitempty"`
}

func ParseEvent(r io.Reader) (*Event, error) {
    var evt Event
    decoder := json.NewDecoder(r)
    if err := decoder.Decode(&evt); err != nil {
        return nil, err
    }
    return &evt, nil
}