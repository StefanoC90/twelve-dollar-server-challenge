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

/* One feed row, laid out exactly like the assembly's R_* fields. */
typedef struct {
  i64 id;
  i64 body;
  i64 bodylen;
  i64 created;
  i64 createdlen;
  i64 author;
  i64 authorlen;
  i64 likes;
} FeedRow;

static Btree *bt;
static BtCursor *cur_likes;
static BtCursor *cur_posts;
static BtCursor *cur_users;
static KeyInfo *ki_likes;
static Pgno root_likes;
static Pgno root_posts;
static Pgno root_users;
static int in_pass;

/* Records that spill onto overflow pages are copied here (not expected for a
 * 500-character body, but a record can be larger than the page). */
static u8 *spill;
static u32 spill_cap;

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

/* The record bytes of the cursor's current row: a pointer into the page when
 * the whole record is local, else a copy in the spill buffer.  0 on error.
 * getCellInfo is required: a move that lands on an exact key (TableMoveto) or
 * at an end (Last) only clears info.nSize, it does not re-parse the cell, so
 * info.pPayload would still describe the previous row. */
static const u8 *record(BtCursor *c, u32 *len) {
  u32 amt, total;
  const u8 *p;
  getCellInfo(c);
  total = c->info.nPayload;
  amt = c->info.nLocal;
  p = c->info.pPayload;
  if (amt >= total) {
    *len = total;
    return p;
  }
  if (total > spill_cap) {
    sqlite3_free(spill);
    spill_cap = total + 1024;
    spill = sqlite3_malloc(spill_cap);
    if (!spill) {
      spill_cap = 0;
      return 0;
    }
  }
  if (sqlite3BtreePayload(c, 0, total, spill) != SQLITE_OK) return 0;
  *len = total;
  return spill;
}

/* SQLite's variable-length integer: up to 8 bytes of 7 bits, then a 9th byte
 * of 8 bits.  Returns the bytes consumed, or 0 if it runs out of input. */
static int getvarint(const u8 *p, u32 n, u64 *v) {
  u64 x = 0;
  u32 i;
  for (i = 0; i < 9 && i < n; i++) {
    if (i == 8) {
      *v = (x << 8) | p[8];
      return 9;
    }
    x = (x << 7) | (p[i] & 0x7f);
    if ((p[i] & 0x80) == 0) {
      *v = x;
      return (int)i + 1;
    }
  }
  return 0;
}

static u32 st_size(u64 t) {
  static const u8 s[12] = {0, 1, 2, 3, 4, 6, 8, 8, 0, 0, 0, 0};
  if (t < 12) return s[t];
  return (u32)((t - 12) / 2);
}

/* Walks the record header of a row of posts/users and returns the requested
 * columns.  Fields: 0 the rowid alias (NULL), 1 user_id/username,
 * 2 body/created_at, 3 created_at.  0 ok, -1 unexpected shape. */
typedef struct {
  const u8 *p;
  u32 len;   /* bytes after the header, i.e. the data area */
  u32 hdr;   /* index of the next serial type */
  u32 hdrsz; /* header size */
  u32 off;   /* data offset */
  int left;  /* serial types still unread */
} Rec;

static int rec_start(Rec *r, const u8 *p, u32 len) {
  u64 hsz;
  int k = getvarint(p, len, &hsz);
  if (!k || hsz < 2 || hsz > len) return -1;
  r->p = p;
  r->len = len - (u32)hsz;
  r->hdr = (u32)k;
  r->hdrsz = (u32)hsz;
  r->off = 0;
  r->left = (int)hsz - k;
  return 0;
}

static int rec_type(Rec *r, u64 *t) {
  int k;
  if (r->left <= 0) return -1;
  k = getvarint(r->p + r->hdr, r->hdrsz - r->hdr, t);
  if (!k || k > r->left) return -1;
  r->hdr += (u32)k;
  r->left -= k;
  return 0;
}

/* The next field as text.  0 ok, -1 not text / out of range. */
static int rec_text(Rec *r, const u8 **p, u32 *n) {
  u64 t;
  u32 sz;
  if (rec_type(r, &t)) return -1;
  if (t < 13 || (t & 1) == 0) return -1; /* not a text value */
  sz = st_size(t);
  if (r->off + sz > r->len) return -1;
  *p = r->p + r->hdrsz + r->off;
  *n = sz;
  r->off += sz;
  return 0;
}

/* The next field as a non-negative integer.  0 ok, -1 otherwise. */
static int rec_int(Rec *r, i64 *v) {
  u64 t, u = 0;
  u32 sz, i;
  const u8 *q;
  if (rec_type(r, &t)) return -1;
  sz = st_size(t);
  if (t >= 8 && t <= 9) {
    *v = (i64)(t - 8);
    return 0;
  }
  if (t < 1 || t > 6) return -1;
  if (r->off + sz > r->len) return -1;
  q = r->p + r->hdrsz + r->off;
  for (i = 0; i < sz; i++) u = (u << 8) | q[i];
  r->off += sz;
  *v = (i64)u;
  return 0;
}

/* Skips the next field whatever its type. */
static int rec_skip(Rec *r) {
  u64 t;
  u32 sz;
  if (rec_type(r, &t)) return -1;
  sz = st_size(t);
  if (r->off + sz > r->len) return -1;
  r->off += sz;
  return 0;
}

/* Why the walk refused a row (reported by the startup self-test). */
static int bt_why;
static int why(int c) { bt_why = c; return -1; }

/* bt_feed(rows, max, arena, slot): the `max` newest posts, newest first, with
 * each row's body, created_at and author copied into its arena slot.
 * Returns the row count, or -1 (the caller then runs the SQL statement). */
int bt_feed(void *rowsv, int max, u8 *arena, int slot) {
  FeedRow *rows = (FeedRow *)rowsv;
  int n = 0, empty = 1, rc;
  bt_why = 0;
  if (!in_pass) return why(1);
  if (max > MAX_ROWS) max = MAX_ROWS;
  if (sqlite3BtreeLast(cur_posts, &empty) != SQLITE_OK) return why(2);
  if (empty) return 0;
  for (;;) {
    i64 id = sqlite3BtreeIntegerKey(cur_posts);
    i64 uid;
    const u8 *body, *created, *author;
    u32 bodylen, createdlen, authorlen, len;
    const u8 *p;
    Rec r;
    u8 *dst;

    p = record(cur_posts, &len);
    if (!p || rec_start(&r, p, len) || rec_skip(&r) || rec_int(&r, &uid) ||
        rec_text(&r, &body, &bodylen) || rec_text(&r, &created, &createdlen))
      return why(3);
    if (uid <= 0 || bodylen > 2000 || createdlen > 40) return why(4);

    /* users row: (id NULL, username, created_at) */
    {
      int res = 0;
      if (sqlite3BtreeTableMoveto(cur_users, uid, 0, &res) != SQLITE_OK || res != 0) return why(5);
      p = record(cur_users, &len);
      if (!p || rec_start(&r, p, len) || rec_skip(&r) || rec_text(&r, &author, &authorlen))
        return why(6);
      if (authorlen > 512) return why(7);
    }

    if (bodylen + createdlen + authorlen > (u32)slot) return why(8);
    dst = arena + (size_t)n * (u32)slot;
    rows[n].id = id;
    rows[n].likes = 0;
    rows[n].body = (i64)dst;
    rows[n].bodylen = bodylen;
    memcpy(dst, body, bodylen);
    dst += bodylen;
    rows[n].created = (i64)dst;
    rows[n].createdlen = createdlen;
    memcpy(dst, created, createdlen);
    dst += createdlen;
    rows[n].author = (i64)dst;
    rows[n].authorlen = authorlen;
    memcpy(dst, author, authorlen);
    n++;
    if (n >= max) break;
    rc = sqlite3BtreePrevious(cur_posts, 0);
    if (rc == SQLITE_DONE) break;
    if (rc != SQLITE_OK) return why(9);
  }
  return n;
}

/* Defined below; bt_post counts one post's likes with it. */
int bt_count_likes(const i64 *ids, int n, u32 *counts);

/* bt_post(id, row): one post by id, with the same FeedRow layout the feed
 * uses and its text copied into the module's one-row slot.
 * 0 found, 1 no such post, -1 fall back to SQL. */
static u8 one_slot[2560];

int bt_post(i64 id, void *rowv) {
  FeedRow *row = (FeedRow *)rowv;
  int res = 0, rc;
  u32 len, bodylen, createdlen, authorlen;
  const u8 *p, *body, *created, *author;
  i64 uid;
  u32 cnt = 0;
  Rec r;
  u8 *dst = one_slot;

  if (!in_pass) return -1;
  if (sqlite3BtreeTableMoveto(cur_posts, id, 0, &res) != SQLITE_OK) return -1;
  if (res != 0) return 1;
  p = record(cur_posts, &len);
  if (!p || rec_start(&r, p, len) || rec_skip(&r) || rec_int(&r, &uid) ||
      rec_text(&r, &body, &bodylen) || rec_text(&r, &created, &createdlen))
    return -1;
  if (uid <= 0 || bodylen > 2000 || createdlen > 40) return -1;

  {
    int res2 = 0;
    if (sqlite3BtreeTableMoveto(cur_users, uid, 0, &res2) != SQLITE_OK || res2 != 0) return -1;
    p = record(cur_users, &len);
    if (!p || rec_start(&r, p, len) || rec_skip(&r) || rec_text(&r, &author, &authorlen))
      return -1;
    if (authorlen > 512) return -1;
  }

  rc = bt_count_likes(&id, 1, &cnt);
  if (rc) return -1;

  if (bodylen + createdlen + authorlen > (u32)sizeof one_slot) return -1;
  row->id = id;
  row->likes = cnt;
  row->body = (i64)dst;
  row->bodylen = bodylen;
  memcpy(dst, body, bodylen);
  dst += bodylen;
  row->created = (i64)dst;
  row->createdlen = createdlen;
  memcpy(dst, created, createdlen);
  dst += createdlen;
  row->author = (i64)dst;
  row->authorlen = authorlen;
  memcpy(dst, author, authorlen);
  return 0;
}

/* Opens the pass's read transaction and its cursors.  0 ok, -1 error. */
int bt_begin(void) {
  if (in_pass) return 0;
  if (sqlite3BtreeBeginTrans(bt, 0, 0) != SQLITE_OK) return -1;
  sqlite3BtreeCursorZero(cur_posts);
  sqlite3BtreeCursorZero(cur_users);
  sqlite3BtreeCursorZero(cur_likes);
  if (sqlite3BtreeCursor(bt, root_posts, 0, 0, cur_posts) != SQLITE_OK ||
      sqlite3BtreeCursor(bt, root_users, 0, 0, cur_users) != SQLITE_OK ||
      sqlite3BtreeCursor(bt, root_likes, 0, ki_likes, cur_likes) != SQLITE_OK)
    return -1;
  in_pass = 1;
  return 0;
}

/* Closes the cursors so the pass transaction can commit. */
void bt_end(void) {
  if (!in_pass) return;
  in_pass = 0;
  sqlite3BtreeCloseCursor(cur_likes);
  sqlite3BtreeCloseCursor(cur_users);
  sqlite3BtreeCloseCursor(cur_posts);
}
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

/* Validates the schema, opens the B-tree and self-tests the walk against SQL.
 * 0 ok, -1 unsupported database (the caller keeps using SQL). */
int bt_open(sqlite3 *db, char *err, int n) {
  sqlite3_stmt *s = 0;
  i64 ids[MAX_ROWS];
  u32 got[MAX_ROWS], want[MAX_ROWS];
  static u8 test_arena[MAX_ROWS * 2560];
  FeedRow rows[MAX_ROWS];
  int nid = 0, i, rc = 0, nrows;

  if (!columns_are(db, "SELECT name FROM pragma_table_info('likes')", "user_id,post_id,created_at") ||
      !columns_are(db, "SELECT name FROM pragma_table_info('posts')", "id,user_id,body,created_at") ||
      !columns_are(db, "SELECT name FROM pragma_table_info('users')", "id,username,created_at") ||
      !columns_are(db, "SELECT name FROM pragma_index_info('likes_post_id_idx')", "post_id")) {
    snprintf(err, n, "unexpected schema");
    return -1;
  }
  bt = db->aDb[0].pBt;
  root_likes = rootpage(db, "likes_post_id_idx");
  root_posts = rootpage(db, "posts");
  root_users = rootpage(db, "users");
  ki_likes = keyinfo(db, 1, 1, 0);
  cur_likes = sqlite3_malloc(sqlite3BtreeCursorSize());
  cur_posts = sqlite3_malloc(sqlite3BtreeCursorSize());
  cur_users = sqlite3_malloc(sqlite3BtreeCursorSize());
  if (!bt || !root_likes || !root_posts || !root_users || !ki_likes || !cur_likes ||
      !cur_posts || !cur_users) {
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
  sqlite3_finalize(s);
  s = 0;

  /* Self-test the row walk the same way, field by field. */
  nrows = bt_feed(rows, MAX_ROWS, test_arena, 2560);
  if (nrows != nid) {
    snprintf(err, n, "self-test: walk returned %d (step %d), SQL %d", nrows, bt_why, nid);
    rc = -1;
    goto out;
  }
  if (sqlite3_prepare_v2(db, "SELECT p.id,p.body,p.created_at,u.username "
                             "FROM posts p JOIN users u ON u.id=p.user_id "
                             "ORDER BY p.id DESC LIMIT 20", -1, &s, 0)) {
    rc = -1;
    goto out;
  }
  for (i = 0; i < nrows; i++) {
    int col;
    if (sqlite3_step(s) != SQLITE_ROW) {
      rc = -1;
      goto out;
    }
    for (col = 0; col < 4; col++) {
      const u8 *wp;
      i64 wantlen;
      const u8 *gp;
      if (col == 0) {
        if (sqlite3_column_int64(s, 0) != rows[i].id) {
          snprintf(err, n, "self-test: row %d id differs", i);
          rc = -1;
          goto out;
        }
        continue;
      }
      wp = sqlite3_column_text(s, col);
      wantlen = sqlite3_column_bytes(s, col);
      gp = (const u8 *)(col == 1 ? rows[i].body : col == 2 ? rows[i].created : rows[i].author);
      if (wantlen != (col == 1 ? rows[i].bodylen : col == 2 ? rows[i].createdlen : rows[i].authorlen) ||
          memcmp(wp, gp, (size_t)wantlen) != 0) {
        snprintf(err, n, "self-test: row %d column %d differs (%d bytes, walk %d)",
                 i, col, (int)wantlen,
                 (int)(col == 1 ? rows[i].bodylen : col == 2 ? rows[i].createdlen : rows[i].authorlen));
        rc = -1;
        goto out;
      }
    }
  }

  /* And the single-post path, against the get statement, same 20 ids. */
  sqlite3_finalize(s);
  s = 0;
  if (sqlite3_prepare_v2(db, "SELECT p.id,p.body,p.created_at,u.username,"
                             "(SELECT count(*) FROM likes l WHERE l.post_id=p.id) "
                             "FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?1",
                         -1, &s, 0)) {
    rc = -1;
    goto out;
  }
  for (i = 0; i < nid; i++) {
    FeedRow got1;
    int col;
    if (sqlite3_bind_int64(s, 1, ids[i]) || sqlite3_step(s) != SQLITE_ROW) {
      rc = -1;
      goto out;
    }
    if (bt_post(ids[i], &got1) != 0) {
      snprintf(err, n, "self-test: post %lld not found by the walk", ids[i]);
      rc = -1;
      goto out;
    }
    for (col = 0; col < 5; col++) {
      const u8 *gp;
      const u8 *wp;
      i64 wantlen, gotlen;
      if (col == 0) {
        if (sqlite3_column_int64(s, 0) != got1.id) {
          snprintf(err, n, "self-test: post %lld id differs", ids[i]);
          rc = -1;
          goto out;
        }
        continue;
      }
      if (col == 4) {
        if ((i64)sqlite3_column_int64(s, 4) != got1.likes) {
          snprintf(err, n, "self-test: post %lld likes differ", ids[i]);
          rc = -1;
          goto out;
        }
        continue;
      }
      wp = sqlite3_column_text(s, col);
      wantlen = sqlite3_column_bytes(s, col);
      gp = (const u8 *)(col == 1 ? got1.body : col == 2 ? got1.created : got1.author);
      gotlen = col == 1 ? got1.bodylen : col == 2 ? got1.createdlen : got1.authorlen;
      if (wantlen != gotlen || memcmp(wp, gp, (size_t)wantlen) != 0) {
        snprintf(err, n, "self-test: post %lld column %d differs", ids[i], col);
        rc = -1;
        goto out;
      }
    }
    sqlite3_reset(s);
  }
out:
  if (s) sqlite3_finalize(s);
  bt_end();
  if (!sqlite3_get_autocommit(db)) sqlite3_exec(db, "ROLLBACK", 0, 0, 0);
  return rc;
}
