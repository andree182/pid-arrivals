#pragma once
#include <pebble.h>

// Set to 1 to enable predefined screenshot fixture for emulator, 0 for normal
//#define SCREENSHOT_EMU 1

#define FAV_MAX        10
#define FAV_ID_LEN     40   // fits the longest real station id (SEPTA, 38 B); matches Bundle.id
#define FAV_NAME_LEN   48   // >= Bundle.station (25) with headroom
#define FAV_LABEL_LEN  24   // free-text label ("Home"); cosmetic, watch-side only
#define FAV_AGENCY_LEN 12   // agency id ("trenurbano" = 10); sent back so (agency,id) resolves uniquely

typedef struct { char id[FAV_ID_LEN]; char name[FAV_NAME_LEN]; char label[FAV_LABEL_LEN]; char agency[FAV_AGENCY_LEN]; } Fav;

// Load favorites from persist into RAM. Runs one-time migration from the
// legacy single-pin keys, then deletes them. Safe to call once at init.
void favorites_load(void);

uint8_t favorites_count(void);

// Returns NULL if i is out of range.
const Fav *favorites_get(uint8_t i);

// Index of a favorite by station id, or -1 if not present.
int favorites_index_of(const char *id);

// Adds {id,name}. Returns false if full or a duplicate id already exists.
bool favorites_add(const char *id, const char *name);

// Removes favorite i (compacts the list and persisted keys). No-op if oob.
void favorites_remove(uint8_t i);

// Swaps favorites a and b in RAM and persist. No-op if either is oob.
void favorites_swap(uint8_t a, uint8_t b);

// Updates favorite i's stored name (RAM + persist). No-op if i is oob or the
// name is unchanged. Returns true if a write occurred.
bool favorites_update_name(uint8_t i, const char *name);

// Sets favorite i's label (RAM + persist). No-op if i is oob or unchanged.
// Returns true if a write occurred.
bool favorites_set_label(uint8_t i, const char *label);

// Replaces the entire favorites list with items[0..n) (id+name+label), rewriting
// persist and the count. n is clamped to FAV_MAX; entries with an empty id are
// skipped. Used to apply a config-page edit from the phone.
void favorites_replace_all(const Fav *items, uint8_t n);
