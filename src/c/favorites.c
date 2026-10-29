#include "favorites.h"
#include <string.h>

#define PERSIST_FAV_COUNT 4
#define PERSIST_FAV_BASE  10
#define LEGACY_USE_NEAREST 2
#define LEGACY_STATION_ID  3

static Fav     s_favs[FAV_MAX];
static uint8_t s_count = 0;

// On-disk layout for one favorite (well under PERSIST_DATA_MAX_LENGTH = 256):
//   [0]      id length L (1..FAV_ID_LEN-1)
//   [1..L]   id bytes
//   [L+1..]  name, NUL-terminated
//   [..]     label, NUL-terminated  (absent in pre-Spec-B blobs => "")
//   [..]     agency, NUL-terminated (absent in pre-agency blobs => "")
static void write_entry(uint8_t i) {
  uint8_t buf[1 + FAV_ID_LEN + FAV_NAME_LEN + FAV_LABEL_LEN + FAV_AGENCY_LEN];
  uint8_t L = (uint8_t)strlen(s_favs[i].id);
  if (L >= FAV_ID_LEN) L = FAV_ID_LEN - 1;
  buf[0] = L;
  memcpy(buf + 1, s_favs[i].id, L);
  size_t nlen = strlen(s_favs[i].name);
  if (nlen >= FAV_NAME_LEN) nlen = FAV_NAME_LEN - 1;
  memcpy(buf + 1 + L, s_favs[i].name, nlen);
  buf[1 + L + nlen] = '\0';
  size_t llen = strlen(s_favs[i].label);
  if (llen >= FAV_LABEL_LEN) llen = FAV_LABEL_LEN - 1;
  size_t off = 1 + L + nlen + 1;
  memcpy(buf + off, s_favs[i].label, llen);
  buf[off + llen] = '\0';
  size_t alen = strlen(s_favs[i].agency);
  if (alen >= FAV_AGENCY_LEN) alen = FAV_AGENCY_LEN - 1;
  size_t aoff = off + llen + 1;
  memcpy(buf + aoff, s_favs[i].agency, alen);
  buf[aoff + alen] = '\0';
  persist_write_data(PERSIST_FAV_BASE + i, buf, aoff + alen + 1);
}

static bool read_entry(uint8_t i, Fav *out) {
  if (!persist_exists(PERSIST_FAV_BASE + i)) return false;
  uint8_t buf[1 + FAV_ID_LEN + FAV_NAME_LEN + FAV_LABEL_LEN + FAV_AGENCY_LEN];
  int n = persist_read_data(PERSIST_FAV_BASE + i, buf, sizeof(buf));
  if (n < 2) return false;
  uint8_t L = buf[0];
  if (L == 0 || L >= FAV_ID_LEN || L + 1 >= n) return false;
  memcpy(out->id, buf + 1, L);
  out->id[L] = '\0';
  // name: from buf+1+L up to its NUL (or end of blob).
  size_t nstart = 1 + L;
  size_t nraw = 0;
  while (nstart + nraw < (size_t)n && buf[nstart + nraw] != '\0') nraw++;
  size_t nlen = nraw >= FAV_NAME_LEN ? FAV_NAME_LEN - 1 : nraw;
  memcpy(out->name, buf + nstart, nlen);
  out->name[nlen] = '\0';
  // label: bytes after the name's NUL, if any (empty for pre-Spec-B blobs).
  out->label[0] = '\0';
  out->agency[0] = '\0';
  size_t lstart = nstart + nraw + 1;   // skip the name's NUL
  if (lstart < (size_t)n) {
    size_t lraw = 0;
    while (lstart + lraw < (size_t)n && buf[lstart + lraw] != '\0') lraw++;
    size_t llen = lraw >= FAV_LABEL_LEN ? FAV_LABEL_LEN - 1 : lraw;
    memcpy(out->label, buf + lstart, llen);
    out->label[llen] = '\0';
    // agency: bytes after the label's NUL ("" for pre-agency blobs).
    size_t astart = lstart + lraw + 1;
    if (astart < (size_t)n) {
      size_t araw = 0;
      while (astart + araw < (size_t)n && buf[astart + araw] != '\0') araw++;
      size_t alen = araw >= FAV_AGENCY_LEN ? FAV_AGENCY_LEN - 1 : araw;
      memcpy(out->agency, buf + astart, alen);
      out->agency[alen] = '\0';
    }
  }
  return true;
}

static void persist_count(void) { persist_write_int(PERSIST_FAV_COUNT, s_count); }

static void migrate_legacy(void) {
  // Seed favorite 0 from a legacy pinned station id (name unknown pre-migration,
  // so use the id as a placeholder name; it renders fine until re-added).
  if (persist_exists(LEGACY_STATION_ID)) {
    char id[FAV_ID_LEN];
    persist_read_string(LEGACY_STATION_ID, id, sizeof(id));
    if (id[0]) {
      strncpy(s_favs[0].id, id, FAV_ID_LEN - 1);
      s_favs[0].id[FAV_ID_LEN - 1] = '\0';
      strncpy(s_favs[0].name, id, FAV_NAME_LEN - 1);
      s_favs[0].name[FAV_NAME_LEN - 1] = '\0';
      s_favs[0].label[0] = '\0';
      s_favs[0].agency[0] = '\0';
      s_count = 1;
      write_entry(0);
    }
  }
  persist_count();
  if (persist_exists(LEGACY_USE_NEAREST)) persist_delete(LEGACY_USE_NEAREST);
  if (persist_exists(LEGACY_STATION_ID)) persist_delete(LEGACY_STATION_ID);
}

#ifdef SCREENSHOT_EMU
static void load_screenshot_favs(void) {
  s_count = 3;
  strncpy(s_favs[0].id, "Muzeum", FAV_ID_LEN - 1);
  s_favs[0].id[FAV_ID_LEN - 1] = '\0';
  strncpy(s_favs[0].name, "Muzeum", FAV_NAME_LEN - 1);
  s_favs[0].name[FAV_NAME_LEN - 1] = '\0';
  strncpy(s_favs[0].label, "Metro A/C", FAV_LABEL_LEN - 1);
  s_favs[0].label[FAV_LABEL_LEN - 1] = '\0';
  strncpy(s_favs[0].agency, "pid", FAV_AGENCY_LEN - 1);
  s_favs[0].agency[FAV_AGENCY_LEN - 1] = '\0';

  strncpy(s_favs[1].id, "Hlavní nádraží", FAV_ID_LEN - 1);
  s_favs[1].id[FAV_ID_LEN - 1] = '\0';
  strncpy(s_favs[1].name, "Hlavní nádraží", FAV_NAME_LEN - 1);
  s_favs[1].name[FAV_NAME_LEN - 1] = '\0';
  strncpy(s_favs[1].label, "Metro C", FAV_LABEL_LEN - 1);
  s_favs[1].label[FAV_LABEL_LEN - 1] = '\0';
  strncpy(s_favs[1].agency, "pid", FAV_AGENCY_LEN - 1);
  s_favs[1].agency[FAV_AGENCY_LEN - 1] = '\0';

  strncpy(s_favs[2].id, "Anděl", FAV_ID_LEN - 1);
  s_favs[2].id[FAV_ID_LEN - 1] = '\0';
  strncpy(s_favs[2].name, "Anděl", FAV_NAME_LEN - 1);
  s_favs[2].name[FAV_NAME_LEN - 1] = '\0';
  strncpy(s_favs[2].label, "Metro B/Tram", FAV_LABEL_LEN - 1);
  s_favs[2].label[FAV_LABEL_LEN - 1] = '\0';
  strncpy(s_favs[2].agency, "pid", FAV_AGENCY_LEN - 1);
  s_favs[2].agency[FAV_AGENCY_LEN - 1] = '\0';
}
#endif

void favorites_load(void) {
#ifdef SCREENSHOT_EMU
  load_screenshot_favs();
  APP_LOG(APP_LOG_LEVEL_DEBUG, "favorites_load (SCREENSHOT_EMU): %d", (int)s_count);
  return;
#endif
  s_count = 0;
  if (!persist_exists(PERSIST_FAV_COUNT)) { migrate_legacy(); return; }
  int c = persist_read_int(PERSIST_FAV_COUNT);
  if (c < 0) c = 0;
  if (c > FAV_MAX) c = FAV_MAX;
  for (int i = 0; i < c; i++) {
    if (read_entry(i, &s_favs[s_count])) s_count++;
  }
  // If some entries failed to decode, rewrite a compacted, consistent list.
  if (s_count != c) {
    for (uint8_t i = 0; i < s_count; i++) write_entry(i);
    persist_count();
  }
  APP_LOG(APP_LOG_LEVEL_DEBUG, "favorites_load: %d", (int)s_count);
}

uint8_t favorites_count(void) { return s_count; }

const Fav *favorites_get(uint8_t i) { return i < s_count ? &s_favs[i] : NULL; }

int favorites_index_of(const char *id) {
  for (uint8_t i = 0; i < s_count; i++)
    if (strncmp(s_favs[i].id, id, FAV_ID_LEN) == 0) return i;
  return -1;
}

bool favorites_add(const char *id, const char *name) {
  if (s_count >= FAV_MAX) return false;
  if (!id || !id[0]) return false;
  if (favorites_index_of(id) >= 0) return false;
  Fav *f = &s_favs[s_count];
  strncpy(f->id, id, FAV_ID_LEN - 1);   f->id[FAV_ID_LEN - 1] = '\0';
  strncpy(f->name, name ? name : id, FAV_NAME_LEN - 1); f->name[FAV_NAME_LEN - 1] = '\0';
  f->label[0] = '\0';
  f->agency[0] = '\0';   // ring-added from the live board; the watch doesn't carry agency, so id-only resolves it
  write_entry(s_count);
  s_count++;
  persist_count();
  return true;
}

void favorites_remove(uint8_t i) {
  if (i >= s_count) return;
  for (uint8_t j = i; j + 1 < s_count; j++) s_favs[j] = s_favs[j + 1];
  s_count--;
  for (uint8_t j = i; j < s_count; j++) write_entry(j);
  if (persist_exists(PERSIST_FAV_BASE + s_count)) persist_delete(PERSIST_FAV_BASE + s_count);
  persist_count();
}

void favorites_swap(uint8_t a, uint8_t b) {
  if (a >= s_count || b >= s_count || a == b) return;
  Fav t = s_favs[a]; s_favs[a] = s_favs[b]; s_favs[b] = t;
  write_entry(a); write_entry(b);
}

bool favorites_update_name(uint8_t i, const char *name) {
  if (i >= s_count || !name) return false;
  if (strncmp(s_favs[i].name, name, FAV_NAME_LEN) == 0) return false;
  strncpy(s_favs[i].name, name, FAV_NAME_LEN - 1);
  s_favs[i].name[FAV_NAME_LEN - 1] = '\0';
  write_entry(i);
  return true;
}

bool favorites_set_label(uint8_t i, const char *label) {
  if (i >= s_count) return false;
  const char *l = label ? label : "";
  if (strncmp(s_favs[i].label, l, FAV_LABEL_LEN) == 0) return false;
  strncpy(s_favs[i].label, l, FAV_LABEL_LEN - 1);
  s_favs[i].label[FAV_LABEL_LEN - 1] = '\0';
  write_entry(i);
  return true;
}

void favorites_replace_all(const Fav *items, uint8_t n) {
  if (n > FAV_MAX) n = FAV_MAX;
  uint8_t w = 0;
  for (uint8_t i = 0; i < n; i++) {
    if (!items[i].id[0]) continue;                 // skip empty ids
    strncpy(s_favs[w].id, items[i].id, FAV_ID_LEN - 1);       s_favs[w].id[FAV_ID_LEN - 1] = '\0';
    strncpy(s_favs[w].name, items[i].name, FAV_NAME_LEN - 1); s_favs[w].name[FAV_NAME_LEN - 1] = '\0';
    strncpy(s_favs[w].label, items[i].label, FAV_LABEL_LEN - 1); s_favs[w].label[FAV_LABEL_LEN - 1] = '\0';
    strncpy(s_favs[w].agency, items[i].agency, FAV_AGENCY_LEN - 1); s_favs[w].agency[FAV_AGENCY_LEN - 1] = '\0';
    write_entry(w);
    w++;
  }
  for (uint8_t i = w; i < s_count; i++)
    if (persist_exists(PERSIST_FAV_BASE + i)) persist_delete(PERSIST_FAV_BASE + i);
  s_count = w;
  persist_count();
}
