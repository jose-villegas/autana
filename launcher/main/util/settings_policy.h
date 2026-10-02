/*
 * settings_policy: when the settings store is started afresh. Pure, over
 * the store's own start and erase calls passed in, so the rule is tested on
 * a host; settings_device.c passes the flash store's.
 */
#pragma once

typedef struct {
    int (*init)(void);
    int (*erase)(void);
    /* The two `init` results that mean the store is full or written by a
     * newer format, and is worth erasing rather than giving up on. Zero is
     * success for every call. */
    int stale_full;
    int stale_format;
} settings_store_ops_t;

/* Starts the store: an init that reports it stale is followed by one erase and
 * one more init. Returns 0 when the store is usable, else the failing result. */
int settings_store_start(const settings_store_ops_t* ops);
