package main

import (
    "os"
    "strconv"
)

type Config struct {
    Port        string
    DBHost      string
    DBUser      string
    DBPassword  string
    DBName      string
    WorkerCount int
}

func LoadConfig() Config {
    return Config{
        Port:        getEnv("NSSTATS_PORT", "8080"),
        DBHost:      getEnv("NSSTATS_DB_HOST", "localhost:3306"),
        DBUser:      getEnv("NSSTATS_DB_USER", "nektum"),
        DBPassword:  getEnv("NSSTATS_DB_PASSWORD", "valami_eros_jelszo"),
        DBName:      getEnv("NSSTATS_DB_NAME", "nektum"),
        WorkerCount: getEnvInt("NSSTATS_WORKERS", 10),
    }
}

func getEnv(key, fallback string) string {
    if v, ok := os.LookupEnv(key); ok && v != "" {
        return v
    }
    return fallback
}

func getEnvInt(key string, fallback int) int {
    if v, ok := os.LookupEnv(key); ok {
        if n, err := strconv.Atoi(v); err == nil {
            return n
        }
    }
    return fallback
}