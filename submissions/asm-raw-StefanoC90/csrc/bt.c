/* The feed's like counts, read straight off likes_post_id_idx.
 *
 * The SQL engine spends most of a /feed request stepping a cursor once per like
 * row - about 80 rows for the 20 newest posts - and handing each row back
 * through sqlite3_step + sqlite3_column_int64.  This file walks the same index
 * with SQLite's own B-tree cursor and decodes the leaf cells in place, so a
 * request pays one seek plus one pass over the entries.
 *
 * It is C because the cursor API, MemPage and the cell layout are internal to
 * SQLite.  The pinned amalgamation is compiled into this translation unit, so
 * the compiler sees the real structs instead of offsets copied by hand.  The
 * rest of the server stays assembly and calls the four functions at the bottom.
 *
 * Every layout assumption is checked against SQL at startup (bt_open runs a
 * self-test that compares this walk with SELECT count(*)).  If anything does
 * not match, bt_open returns -1, the server logs it and keeps using the plain
 * SQL statement, so an unexpected database cannot make /feed wrong.
 */
#include "sqlite3.c"

typedef long long i64;
typedef unsigned int u32;
typedef unsigned char u8;

#define MAX_ROWS 20

static Btree *bt;
static BtCursor *cur_likes;
static KeyInfo *ki_likes;
static Pgno root_likes;
static int in_pass;

/* ---------------------------------------------------------------- startup */

static Pgno rootpage(sqlite3 *db, const char *name) {
  sqlite3_stmt *s = 0;
  Pgno r = 0;
  if (sqlite3_prepare_v2(db, "SELECT rootpage FROM sqlite_schema WHERE name = ?1", -1, &s, 0)) return 0;
  sqlite3_bind_text(s, 1, name, -1, SQLITE_STATIC);
  if (sqlite3_step(s) == SQLITE_ROW) r = (Pgno)sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  return r;
}

/* Compares the column names a statement yields with an expected list. */
static int columns_are(sqlite3 *db, const char *sql, const char *want) {
  sqlite3_stmt *s = 0;
  char got[128];
  size_t n = 0;
  got[0] = 0;
  if (sqlite3_prepare_v2(db, sql, -1, &s, 0)) return 0;
  while (sqlite3_step(s) == SQLITE_ROW) {
    const char *c = (const char *)sqlite3_column_text(s, 0);
    n += (size_t)snprintf(got + n, sizeof got - n, "%s%s", n ? "," : "", c ? c : "");
    if (n >= sizeof got) break;
  }
  sqlite3_finalize(s);
  return strcmp(got, want) == 0;
}

/* A KeyInfo for an index of nkey integer columns plus nextra trailing ones. */
static KeyInfo *keyinfo(sqlite3 *db, int nkey, int nextra, int desc) {
  KeyInfo *k = sqlite3KeyInfoAlloc(db, nkey, nextra);
  int i;
  if (!k) return 0;
  for (i = 0; i < nkey + nextra; i++) k->aColl[i] = db->pDfltColl;
  for (i = 0; i < nkey; i++) k->aSortFlags[i] = desc ? KEYINFO_ORDER_DESC : 0;
  return k;
}

/* ------------------------------------------------------------- the walk */

/* Leaves the index cursor on the first entry whose leading integer is >= k.
 * 1 on an entry, 0 past the end, -1 on error. */
static int seek_ge(BtCursor *c, i64 k) {
  Mem m;
  UnpackedRecord r;
  int res = 0;
  memset(&m, 0, sizeof m);
  m.flags = MEM_Int;
  m.u.i = k;
  memset(&r, 0, sizeof r);
  r.pKeyInfo = c->pKeyInfo;
  r.aMem = &m;
  r.nField = 1;
  r.default_rc = +1; /* entries equal on the key sort after the probe */
  if (sqlite3BtreeIndexMoveto(c, &r, &res) != SQLITE_OK) return -1;
  if (sqlite3BtreeEof(c)) return 0;
  if (res < 0) {
    int rc = sqlite3BtreeNext(c, 0);
    if (rc == SQLITE_DONE) return 0;
    if (rc != SQLITE_OK) return -1;
  }
  return 1;
}

/* Decodes the leading integer column of the index cell at ix.  0 ok, 1 a cell
 * that is not in the expected shape (the caller then uses SQL). */
static int cell_int(BtCursor *c, int ix, i64 *out) {
  MemPage *pg = c->pPage;
  u8 *cell = findCell(pg, ix) + pg->childPtrSize;
  u32 len = cell[0];
  const u8 *p;
  u32 h, t, k, z;
  i64 v;
  if (len >= 0x80) return 1; /* multi-byte payload size: not expected here */
  p = cell + 1;
  if (len < 3 || p + len > pg->aDataEnd) return 1;
  h = p[0]; /* record header size */
  t = p[1]; /* serial type of the first field */
  if (h >= len || h < 3 || t >= 0x80) return 1;
  if (t >= 1 && t <= 6) {
    static const u8 sz[7] = {0, 1, 2, 3, 4, 6, 8};
    const u8 *q;
    k = sz[t];
    if (h + k > len) return 1;
    q = p + h;
    v = (i8)q[0];
    for (z = 1; z < k; z++) v = (v << 8) | q[z];
  } else if (t == 8 || t == 9) {
    v = t - 8;
  } else {
    return 1;
  }
  *out = v;
  return 0;
}

/* ---------------------------------------------------------------- export */

/* bt_count_likes(ids, n, counts): counts[i] = number of likes on ids[i].
 * ids must be strictly descending and n <= MAX_ROWS; counts is zeroed here.
 * 0 ok, -1 error, 1 unexpected cell shape. */
int bt_count_likes(const i64 *ids, int n, u32 *counts) {
  int r, j, i, last;
  MemPage *pg;
  i64 v;
  if (!in_pass || n <= 0 || n > MAX_ROWS) return -1;
  for (i = 0; i < n; i++) counts[i] = 0;
  r = seek_ge(cur_likes, ids[n - 1]);
  if (r <= 0) return r;
  j = n - 1; /* ids is descending, entries arrive ascending */
  for (;;) {
    pg = cur_likes->pPage;
    i = cur_likes->ix;
    last = pg->leaf ? pg->nCell : i + 1;
    for (; i < last; i++) {
      r = cell_int(cur_likes, i, &v);
      if (r) return r;
      if (v > ids[0]) return 0;
      while (j >= 0 && ids[j] < v) j--;
      if (j >= 0 && ids[j] == v) counts[j]++;
    }
    /* Skip the rest of this leaf in one step instead of walking to its end. */
    cur_likes->ix = (u16)(last - 1);
    r = sqlite3BtreeNext(cur_likes, 0);
    if (r == SQLITE_DONE) return 0;
    if (r != SQLITE_OK) return -1;
  }
}

/* Opens the pass's read transaction and its cursors.  0 ok, -1 error. */
int bt_begin(void) {
  if (in_pass) return 0;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return -1;
  sqlite3BtreeCursorZero(cur_likes);
  if (sqlite3BtreeCursor(bt, root_likes, 0, ki_likes, cur_likes) != SQLITE_OK) return -1;
  in_pass = 1;
  return 0;
}

/* Closes the cursors so the pass transaction can commit. */
void bt_end(void) {
  if (!in_pass) return;
  in_pass = 0;
  sqlite3BtreeCloseCursor(cur_likes);
}

/* Validates the schema, opens the B-tree and self-tests the walk against SQL.
 * 0 ok, -1 unsupported database (the caller keeps using SQL). */
int bt_open(sqlite3 *db, char *err, int n) {
  sqlite3_stmt *s = 0;
  i64 ids[MAX_ROWS];
  u32 got[MAX_ROWS], want[MAX_ROWS];
  int nid = 0, i, rc = 0;

  if (!columns_are(db, "SELECT name FROM pragma_table_info('likes')", "user_id,post_id,created_at") ||
      !columns_are(db, "SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id")) {
    snprintf(err, n, "unexpected likes schema");
    return -1;
  }
  bt = db->aDb[0].pBt;
  root_likes = rootpage(db, "likes_post_id_idx");
  ki_likes = keyinfo(db, 1, 1, 0);
  cur_likes = sqlite3_malloc(sqlite3BtreeCursorSize());
  if (!bt || !root_likes || !ki_likes || !cur_likes) {
    snprintf(err, n, "btree setup failed");
    return -1;
  }

  /* Self-test: the newest posts' like counts, this walk against SQL. */
  if (sqlite3_exec(db, "BEGIN", 0, 0, 0) || bt_begin()) {
    snprintf(err, n, "self-test transaction failed");
    rc = -1;
    goto out;
  }
  if (sqlite3_prepare_v2(db, "SELECT id FROM posts ORDER BY id DESC LIMIT 20", -1, &s, 0)) {
    rc = -1;
    goto out;
  }
  while (sqlite3_step(s) == SQLITE_ROW && nid < MAX_ROWS) ids[nid++] = sqlite3_column_int64(s, 0);
  sqlite3_finalize(s);
  s = 0;
  if (nid == 0 || bt_count_likes(ids, nid, got)) {
    rc = -1;
    goto out;
  }
  if (sqlite3_prepare_v2(db, "SELECT count(*) FROM likes WHERE post_id = ?1", -1, &s, 0)) {
    rc = -1;
    goto out;
  }
  for (i = 0; i < nid; i++) {
    sqlite3_bind_int64(s, 1, ids[i]);
    if (sqlite3_step(s) != SQLITE_ROW) {
      rc = -1;
      goto out;
    }
    want[i] = (u32)sqlite3_column_int64(s, 0);
    sqlite3_reset(s);
  }
  for (i = 0; i < nid; i++) {
    if (got[i] != want[i]) {
      snprintf(err, n, "self-test: post %lld has %u likes, walk says %u", ids[i], want[i], got[i]);
      rc = -1;
      goto out;
    }
  }
out:
  if (s) sqlite3_finalize(s);
  bt_end();
  if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
  return rc;
}
