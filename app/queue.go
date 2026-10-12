package main

import (
    "database/sql"
    "log"
    "sync"
)

type WorkerPool struct {
    db      *sql.DB
    jobs    chan *Event
    workers int
    wg      sync.WaitGroup
    stopped bool
    mu      sync.Mutex
}

func NewWorkerPool(db *sql.DB, workers int) *WorkerPool {
    return &WorkerPool{
        db:      db,
        jobs:    make(chan *Event, 1000),
        workers: workers,
    }
}

func (p *WorkerPool) Start() {
    p.mu.Lock()
    defer p.mu.Unlock()
    if p.stopped {
        return
    }
    for i := 0; i < p.workers; i++ {
        p.wg.Add(1)
        go p.worker()
    }
}

func (p *WorkerPool) Submit(evt *Event) {
    p.mu.Lock()
    defer p.mu.Unlock()
    if p.stopped {
        return
    }
    select {
    case p.jobs <- evt:
    default:
        log.Println("Job queue full, dropping event")
    }
}

func (p *WorkerPool) Stop() {
    p.mu.Lock()
    if p.stopped {
        p.mu.Unlock()
        return
    }
    p.stopped = true
    p.mu.Unlock()

    close(p.jobs) // bezárjuk, a worker-ek kiolvassák a maradékot
    p.wg.Wait()   // megvárjuk, hogy minden worker befejezze
}

func (p *WorkerPool) worker() {
    defer p.wg.Done()
    for evt := range p.jobs {
        if err := InsertEvent(p.db, evt); err != nil {
            log.Printf("InsertEvent error: %v", err)
        }
    }
}