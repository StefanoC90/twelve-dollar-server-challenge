; SQLite layer.  One connection per worker thread, WAL + synchronous=NORMAL,
; with the five statements this API needs prepared once per connection.

%include "common.inc"

extern sqlite3_open_v2
extern sqlite3_close
extern sqlite3_busy_timeout
extern sqlite3_exec
extern sqlite3_prepare_v2
extern sqlite3_bind_int64
extern sqlite3_bind_text
extern sqlite3_step
extern sqlite3_reset
extern sqlite3_column_int64
extern sqlite3_column_text
extern sqlite3_column_bytes
extern sqlite3_changes
extern sqlite3_finalize
extern strlen

extern append_bytes
extern append_u64
extern append_json_string
extern mem_copy

; csrc/bt.c: the likes index walk over SQLite's B-tree cursor
extern bt_open
extern bt_begin
extern bt_end
extern bt_feed
extern bt_count_likes

section .rodata
sql_pragmas:
    db 'PRAGMA journal_mode=WAL;PRAGMA synchronous=NORMAL;PRAGMA foreign_keys=ON;'
    db 'PRAGMA busy_timeout=5000;PRAGMA cache_size=-16384;PRAGMA mmap_size=0;', 0
sql_feed:
    db 'SELECT p.id,p.body,p.created_at,u.username '
    db 'FROM posts p JOIN users u ON u.id=p.user_id '
    db 'ORDER BY p.id DESC LIMIT 20', 0
; The 20 feed posts are the 20 highest ids, so the like rows for exactly those
; posts are one contiguous slice of likes_post_id_idx. One range scan of ~80
; index entries replaces 20 correlated count(*) subqueries.
sql_feed_likes:
    db 'SELECT post_id FROM likes WHERE post_id BETWEEN ? AND ?', 0
sql_get:
    db 'SELECT p.id,p.body,p.created_at,u.username,'
    db '(SELECT count(*) FROM likes l WHERE l.post_id=p.id) '
    db 'FROM posts p JOIN users u ON u.id=p.user_id WHERE p.id=?', 0
sql_insert_post:
    db 'INSERT INTO posts(user_id,body) VALUES(?,?) RETURNING id,created_at', 0
sql_exists:
    db 'SELECT 1 FROM posts WHERE id=?', 0
sql_insert_like:
    db 'INSERT INTO likes(user_id,post_id) VALUES(?,?) '
    db 'ON CONFLICT(user_id,post_id) DO NOTHING', 0
sql_health:
    db 'SELECT 1', 0
sql_begin:
    db 'BEGIN', 0
sql_commit:
    db 'COMMIT', 0
s_bt_fail:
    db 'bt: no B-tree feed path, using SQL: '
s_bt_fail_len equ $ - s_bt_fail
s_nl:
    db 10

s_open_brace:   db '{'
s_close_brace:  db '}'
s_posts_open:   db '{"posts":['
s_posts_close:  db ']}'
s_post_open:    db '{"post":'
s_id:           db '{"id":'
s_body:         db ',"body":'
s_created:      db ',"created_at":'
s_author:       db ',"author":'
s_likes:        db ',"like_count":'

section .bss
global g_sqlite_path
g_sqlite_path: resq 1

; The pass transaction (see db_begin/db_end). One worker per process, so these
; are process-global like the feed scratch below.
txn_begin:  resq 1
txn_commit: resq 1
txn_open:   resd 1

; 1 once csrc/bt.c has validated the schema and self-tested its index walk; 0
; keeps the feed on the SQL likes statement.
bt_ok:      resd 1

; Startup-only message buffer for bt_open, and the feed's ids (descending) and
; their like counts for the B-tree walk.
bt_errbuf:   resb 128
feed_ids:    resq 20
feed_counts: resd 20

; Feed scratch, one worker process owns these (forked before SQLite is opened).
; A row's text is copied out of SQLite before the next step() invalidates it.
; A slot holds the largest legal row: 500 characters of UTF-8 body (<= 2000 B),
; a 24-byte timestamp and a username (the JWT's `username`, which the spec does
; not bound; 512 bytes covers any realistic token).
%define FEED_ROWS   20
%define FEED_SLOT   2560
feed_rows:  resb FEED_ROWS * R_SIZE
feed_arena: resb FEED_ROWS * FEED_SLOT

section .text

global db_init
global db_close
global db_health
global db_begin
global db_end
global db_feed
global db_get_post
global db_create_post
global db_like

; -----------------------------------------------------------------------------
; db_init(rdi=wctx) -> eax = 0 ok / -1
; -----------------------------------------------------------------------------
db_init:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov rdi, [rel g_sqlite_path]
    lea rsi, [r12+W_DB]
    mov edx, SQLITE_OPEN_RW
    xor ecx, ecx
    call sqlite3_open_v2
    test eax, eax
    jnz .fail
    mov rdi, [r12+W_DB]
    mov esi, 5000
    call sqlite3_busy_timeout
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_pragmas]
    xor edx, edx
    xor ecx, ecx
    xor r8d, r8d
    call sqlite3_exec
    test eax, eax
    jnz .fail_close
    ; feed
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_feed]
    mov edx, -1
    lea rcx, [r12+W_STMT_FEED]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; feed like counts
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_feed_likes]
    mov edx, -1
    lea rcx, [r12+W_STMT_FEEDL]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; get
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_get]
    mov edx, -1
    lea rcx, [r12+W_STMT_GET]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; insert post
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_insert_post]
    mov edx, -1
    lea rcx, [r12+W_STMT_INS]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; exists
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_exists]
    mov edx, -1
    lea rcx, [r12+W_STMT_EXISTS]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; like
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_insert_like]
    mov edx, -1
    lea rcx, [r12+W_STMT_LIKE]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; health
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_health]
    mov edx, -1
    lea rcx, [r12+W_STMT_HEALTH]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; the pass transaction's two statements
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_begin]
    mov edx, -1
    lea rcx, [rel txn_begin]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    mov rdi, [r12+W_DB]
    lea rsi, [rel sql_commit]
    mov edx, -1
    lea rcx, [rel txn_commit]
    xor r8d, r8d
    call sqlite3_prepare_v2
    test eax, eax
    jnz .fail_close
    ; The B-tree read path validates the schema and self-tests its walk against
    ; SQL. It failing is not fatal: the feed falls back to the SQL statement.
    mov rdi, [r12+W_DB]
    lea rsi, [rel bt_errbuf]
    mov edx, 128
    call bt_open
    test eax, eax
    jnz .bt_fallback
    mov dword [rel bt_ok], 1
    xor eax, eax
    jmp .done
.bt_fallback:
    mov dword [rel bt_ok], 0
    mov eax, SYS_write
    mov edi, 2
    lea rsi, [rel s_bt_fail]
    mov edx, s_bt_fail_len
    syscall
    lea rdi, [rel bt_errbuf]
    call strlen
    mov edx, eax
    mov eax, SYS_write
    mov edi, 2
    lea rsi, [rel bt_errbuf]
    syscall
    mov eax, SYS_write
    mov edi, 2
    lea rsi, [rel s_nl]
    mov edx, 1
    syscall
    xor eax, eax
    jmp .done
.fail_close:
    mov rdi, [r12+W_DB]
    call sqlite3_close
.fail:
    mov eax, -1
.done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_close(rdi=wctx)
; -----------------------------------------------------------------------------
db_close:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, W_STMT_FEED
.fin_loop:
    cmp r13, W_STMT_HEALTH
    ja .close_db
    mov rdi, [r12+r13]
    test rdi, rdi
    jz .fin_next
    call sqlite3_finalize
.fin_next:
    add r13, 8
    jmp .fin_loop
.close_db:
    mov rdi, [rel txn_begin]
    test rdi, rdi
    jz .fin_commit
    call sqlite3_finalize
.fin_commit:
    mov rdi, [rel txn_commit]
    test rdi, rdi
    jz .close_real
    call sqlite3_finalize
.close_real:
    mov rdi, [r12+W_DB]
    test rdi, rdi
    jz .done
    call sqlite3_close
.done:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_begin() / db_end() - the event-loop pass's read transaction
;
; BEGIN is deferred, so a pass that reads nothing pays nothing (no btree
; transaction is started until the first read). While the transaction is open
; every read statement in the pass reuses the one read lock, which is what
; removes the per-request fstat + two fcntl calls. db_end() is also called by
; the write handlers before they run their INSERT: a write must not join the
; pass's read transaction, because then its commit would be deferred to the end
; of the pass and the 201 could leave before the commit.
;
; Both helpers keep every caller-saved register: they are called from the
; event loop and from the tail of db_create_post/db_like, where eax carries a
; return value.
; -----------------------------------------------------------------------------
db_begin:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    cmp dword [rel txn_open], 0
    jne .done
    mov rdi, [rel txn_begin]
    test rdi, rdi
    jz .done
    mov dword [rel txn_open], 1
    call sqlite3_reset
    mov rdi, [rel txn_begin]
    call sqlite3_step
    cmp eax, SQLITE_DONE
    jne .fail
    ; The feed reads the likes index through a B-tree cursor (csrc/bt.c), which
    ; needs the pass transaction open on the btree, not just the SQL statement.
    cmp dword [rel bt_ok], 0
    je .done
    call bt_begin
    test eax, eax
    jz .done
    ; A btree transaction could not start: keep serving, but from SQL.
    mov dword [rel bt_ok], 0
.fail:
    ; could not begin: leave the reads in autocommit rather than fail requests
    mov dword [rel txn_open], 0
    mov rdi, [rel txn_commit]
    call sqlite3_reset
    mov rdi, [rel txn_commit]
    call sqlite3_step
    mov rdi, [rel txn_commit]
    call sqlite3_reset
.done:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

db_end:
    push rax
    push rcx
    push rdx
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    cmp dword [rel txn_open], 0
    je .done
    mov dword [rel txn_open], 0
    ; SQLite will not commit with a cursor still open on the btree.
    cmp dword [rel bt_ok], 0
    je .commit
    call bt_end
.commit:
    mov rdi, [rel txn_commit]
    call sqlite3_reset
    mov rdi, [rel txn_commit]
    call sqlite3_step
    mov rdi, [rel txn_commit]
    call sqlite3_reset
.done:
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rax
    ret

; -----------------------------------------------------------------------------
; db_health(rdi=wctx) -> eax 0 ok / -1
; -----------------------------------------------------------------------------
db_health:
    push rbp
    mov rbp, rsp
    sub rsp, 16
    mov [rbp-8], rdi
    mov rsi, [rdi+W_STMT_HEALTH]
    mov [rbp-16], rsi
    mov rdi, rsi
    call sqlite3_reset
    mov rdi, [rbp-16]
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .ok
    cmp eax, SQLITE_DONE
    je .ok
    mov rdi, [rbp-16]
    call sqlite3_reset
    mov eax, -1
    leave
    ret
.ok:
    mov rdi, [rbp-16]
    call sqlite3_reset
    xor eax, eax
    leave
    ret

; -----------------------------------------------------------------------------
; fill_row(rdi=stmt, rsi=row)
; -----------------------------------------------------------------------------
fill_row:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, rsi
    mov rdi, r12
    xor esi, esi
    call sqlite3_column_int64
    mov [r13+R_ID], rax
    mov rdi, r12
    mov esi, 1
    call sqlite3_column_text
    mov [r13+R_BODY], rax
    mov rdi, r12
    mov esi, 1
    call sqlite3_column_bytes
    mov eax, eax
    mov [r13+R_BODYLEN], rax
    mov rdi, r12
    mov esi, 2
    call sqlite3_column_text
    mov [r13+R_CREATED], rax
    mov rdi, r12
    mov esi, 2
    call sqlite3_column_bytes
    mov eax, eax
    mov [r13+R_CREATEDLEN], rax
    mov rdi, r12
    mov esi, 3
    call sqlite3_column_text
    mov [r13+R_AUTHOR], rax
    mov rdi, r12
    mov esi, 3
    call sqlite3_column_bytes
    mov eax, eax
    mov [r13+R_AUTHORLEN], rax
    mov rdi, r12
    mov esi, 4
    call sqlite3_column_int64
    mov [r13+R_LIKES], rax
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; fill_row_copy(rdi=stmt, rsi=row, rdx=slot) -> eax = 0 ok / -1 row too large
; Same as fill_row, but copies the three text columns into slot: SQLite's
; column pointers die on the next step(), and the feed needs all 20 rows before
; it can count likes and render. Slot is FEED_SLOT bytes.
; -----------------------------------------------------------------------------
fill_row_copy:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi                ; stmt
    mov r13, rsi                ; row
    mov r14, rdx                ; slot cursor
    mov rdi, r12
    xor esi, esi
    call sqlite3_column_int64
    mov [r13+R_ID], rax
    mov qword [r13+R_LIKES], 0
    ; body
    mov rdi, r12
    mov esi, 1
    call sqlite3_column_text
    mov r15, rax
    mov rdi, r12
    mov esi, 1
    call sqlite3_column_bytes
    mov eax, eax
    cmp rax, 2000
    ja .fail
    mov rbx, rax
    mov [r13+R_BODY], r14
    mov [r13+R_BODYLEN], rbx
    mov rdi, r14
    mov rsi, r15
    mov rdx, rbx
    call mem_copy
    add r14, rbx
    ; created_at
    mov rdi, r12
    mov esi, 2
    call sqlite3_column_text
    mov r15, rax
    mov rdi, r12
    mov esi, 2
    call sqlite3_column_bytes
    mov eax, eax
    cmp rax, 40
    ja .fail
    mov rbx, rax
    mov [r13+R_CREATED], r14
    mov [r13+R_CREATEDLEN], rbx
    mov rdi, r14
    mov rsi, r15
    mov rdx, rbx
    call mem_copy
    add r14, rbx
    ; author
    mov rdi, r12
    mov esi, 3
    call sqlite3_column_text
    mov r15, rax
    mov rdi, r12
    mov esi, 3
    call sqlite3_column_bytes
    mov eax, eax
    cmp rax, 512
    ja .fail
    mov rbx, rax
    mov [r13+R_AUTHOR], r14
    mov [r13+R_AUTHORLEN], rbx
    mov rdi, r14
    mov rsi, r15
    mov rdx, rbx
    call mem_copy
    xor eax, eax
    jmp .out
.fail:
    mov eax, -1
.out:
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; render_post(rdi=dst, rsi=row) -> rax = new end
; -----------------------------------------------------------------------------
render_post:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 8
    mov r12, rdi
    mov r13, rsi
    ; Fixed JSON keys are written 16 bytes at a time straight from .rodata and
    ; the write pointer is advanced by the key length; the trailing bytes are
    ; overwritten by the next field (or lie past the body length).
    movdqu xmm0, [rel s_id]
    movdqu [r12], xmm0
    add r12, 6
    mov rdi, r12
    mov rsi, [r13+R_ID]
    call append_u64
    mov r12, rax
    movdqu xmm0, [rel s_body]
    movdqu [r12], xmm0
    add r12, 8
    mov rdi, r12
    mov rsi, [r13+R_BODY]
    mov rdx, [r13+R_BODYLEN]
    call append_json_string
    mov r12, rax
    movdqu xmm0, [rel s_created]
    movdqu [r12], xmm0
    add r12, 14
    mov rdi, r12
    mov rsi, [r13+R_CREATED]
    mov rdx, [r13+R_CREATEDLEN]
    call append_json_string
    mov r12, rax
    movdqu xmm0, [rel s_author]
    movdqu [r12], xmm0
    add r12, 10
    mov rdi, r12
    mov rsi, [r13+R_AUTHOR]
    mov rdx, [r13+R_AUTHORLEN]
    call append_json_string
    mov r12, rax
    movdqu xmm0, [rel s_likes]
    movdqu [r12], xmm0
    add r12, 14
    mov rdi, r12
    mov rsi, [r13+R_LIKES]
    call append_u64
    mov r12, rax
    mov byte [r12], '}'
    inc r12
    mov rax, r12
    add rsp, 8
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_feed(rdi=wctx, rsi=dst, rdx=cap) -> rax = body length / -1
; -----------------------------------------------------------------------------
db_feed:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 184
    mov r12, rdi
    mov [rbp-152], rsi          ; dst
    mov [rbp-160], rdx          ; cap
    mov r14, [r12+W_STMT_FEED]
    cmp dword [rel bt_ok], 0
    je .sql_rows
    ; The 20 newest posts straight off the posts/users B-trees (csrc/bt.c):
    ; sqlite3BtreeLast + BtreePrevious instead of a VDBE row per post.
    lea rdi, [rel feed_rows]
    mov esi, FEED_ROWS
    lea rdx, [rel feed_arena]
    mov ecx, FEED_SLOT
    call bt_feed
    test eax, eax
    js .sql_rows                ; a row the walk does not expect: use SQL
    mov r15d, eax               ; rows copied
    jmp .rows_done
.sql_rows:
    mov rdi, r14
    call sqlite3_reset
    xor r15d, r15d              ; rows copied
    lea rbx, [rel feed_arena]
.row_loop:
    cmp r15, FEED_ROWS
    jae .rows_done
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .row
    cmp eax, SQLITE_DONE
    je .rows_done
    jmp .err_feed
.row:
    mov rdi, r14
    lea rsi, [rel feed_rows]
    mov rax, r15
    shl rax, 6                   ; * R_SIZE (64)
    add rsi, rax
    mov rdx, rbx
    call fill_row_copy
    test eax, eax
    jnz .err_feed
    add rbx, FEED_SLOT
    inc r15
    jmp .row_loop
.rows_done:
    ; The feed rows are the 20 highest ids, so exactly their like rows are one
    ; contiguous range of likes_post_id_idx. Count them in place.
    test r15, r15
    jz .render
    cmp dword [rel bt_ok], 0
    je .sql_likes
    ; Walk the index with SQLite's B-tree cursor (csrc/bt.c): one seek plus one
    ; pass over the entries, instead of a VDBE step and a column read per row.
    lea rsi, [rel feed_rows]
    lea rdi, [rel feed_ids]
    xor ecx, ecx
.gather_ids:
    mov rax, [rsi+R_ID]
    mov [rdi+rcx*8], rax
    add rsi, R_SIZE
    inc rcx
    cmp rcx, r15
    jb .gather_ids
    lea rdi, [rel feed_ids]
    mov esi, r15d
    lea rdx, [rel feed_counts]
    call bt_count_likes
    test eax, eax
    jnz .sql_likes              ; unexpected index shape: recount with SQL
    lea rsi, [rel feed_rows]
    lea rdi, [rel feed_counts]
    xor ecx, ecx
.store_counts:
    mov eax, [rdi+rcx*4]
    mov [rsi+R_LIKES], rax
    add rsi, R_SIZE
    inc rcx
    cmp rcx, r15
    jb .store_counts
    jmp .render
.sql_likes:
    mov rbx, [r12+W_STMT_FEEDL]
    mov rdi, rbx
    call sqlite3_reset
    lea rax, [rel feed_rows]
    mov rcx, r15
    dec rcx
    shl rcx, 6                   ; * R_SIZE (64)
    mov rdi, rbx
    mov esi, 1
    mov rdx, [rax+rcx+R_ID]
    call sqlite3_bind_int64
    lea rax, [rel feed_rows]
    mov rdi, rbx
    mov esi, 2
    mov rdx, [rax+R_ID]
    call sqlite3_bind_int64
    ; likes come back in ascending post_id order and the feed rows are in
    ; descending id order, so walk the feed rows downwards once instead of
    ; rescanning all of them for every like row.
    mov rax, r15
    dec rax
    mov [rbp-168], rax
.like_loop:
    mov rdi, rbx
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .like_row
    cmp eax, SQLITE_DONE
    je .likes_done
    jmp .err_likes
.like_row:
    mov rdi, rbx
    xor esi, esi
    call sqlite3_column_int64
.merge:
    mov rdx, [rbp-168]
    test rdx, rdx
    js .like_loop                ; below the lowest feed id: no match possible
    lea rcx, [rel feed_rows]
    shl rdx, 6                   ; * R_SIZE (64)
    add rcx, rdx
    cmp [rcx+R_ID], rax
    jae .merge_done
    dec qword [rbp-168]
    jmp .merge
.merge_done:
    jne .like_loop               ; feed id > this post id: a gap, no match
    inc qword [rcx+R_LIKES]
    jmp .like_loop
.likes_done:
    mov rdi, rbx
    call sqlite3_reset
.render:
    mov r13, [rbp-152]
    mov rdi, r13
    lea rsi, [rel s_posts_open]
    mov edx, 10
    call append_bytes
    mov r13, rax
    xor ebx, ebx
.render_loop:
    cmp rbx, r15
    jae .render_done
    test rbx, rbx
    jz .no_comma
    mov byte [r13], ','
    inc r13
.no_comma:
    mov rdi, r13
    lea rsi, [rel feed_rows]
    mov rax, rbx
    shl rax, 6                   ; * R_SIZE (64)
    add rsi, rax
    call render_post
    mov r13, rax
    inc rbx
    jmp .render_loop
.render_done:
    mov byte [r13], ']'
    mov byte [r13+1], '}'
    add r13, 2
    mov rdi, r14
    call sqlite3_reset
    mov rax, r13
    sub rax, [rbp-152]
    jmp .out
.err_likes:
    mov rdi, rbx
    call sqlite3_reset
    mov rax, -1
    jmp .out
.err_feed:
    mov rdi, r14
    call sqlite3_reset
    mov rax, -1
.out:
    add rsp, 184
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_get_post(rdi=wctx, rsi=id, rdx=dst, rcx=cap)
;   -> eax = 0 found / 1 not found / -1 error
; -----------------------------------------------------------------------------
db_get_post:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 120
    mov r12, rdi
    mov r13, rsi                ; id
    mov [rbp-152], rdx          ; dst
    mov [rbp-160], rcx          ; cap
    mov r14, [r12+W_STMT_GET]
    mov rdi, r14
    call sqlite3_reset
    mov rdi, r14
    mov esi, 1
    mov rdx, r13
    call sqlite3_bind_int64
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .row
    cmp eax, SQLITE_DONE
    je .not_found
    mov rdi, r14
    call sqlite3_reset
    mov eax, -1
    jmp .out
.row:
    mov rdi, r14
    lea rsi, [rbp-128]
    call fill_row
    mov rdi, [rbp-152]
    lea rsi, [rel s_post_open]
    mov edx, 8
    call append_bytes
    mov rdi, rax
    lea rsi, [rbp-128]
    call render_post
    mov byte [rax], '}'
    inc rax
    mov r15, rax
    sub r15, [rbp-152]
    mov rdi, r14
    call sqlite3_reset
    mov rdx, r15
    xor eax, eax
    jmp .out
.not_found:
    mov rdi, r14
    call sqlite3_reset
    mov eax, 1
.out:
    add rsp, 120
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_create_post(rdi=wctx, rsi=args) -> eax 0 ok / -1
;   args: C_UID, C_BODY, C_BODYLEN, C_UNAME, C_UNAMELEN, C_DST, C_OUTLEN
; -----------------------------------------------------------------------------
db_create_post:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 120
    mov r12, rdi
    mov r13, rsi
    call db_end                  ; a write must not join the pass's read txn
    mov r14, [r12+W_STMT_INS]
    mov rdi, r14
    call sqlite3_reset
    mov rdi, r14
    mov esi, 1
    mov rdx, [r13+C_UID]
    call sqlite3_bind_int64
    mov rdi, r14
    mov esi, 2
    mov rdx, [r13+C_BODY]
    mov ecx, [r13+C_BODYLEN]
    mov r8, SQLITE_TRANSIENT
    call sqlite3_bind_text
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_ROW
    jne .fail
    ; row: id, created_at
    mov rdi, r14
    xor esi, esi
    call sqlite3_column_int64
    mov [rbp-128+R_ID], rax
    mov rdi, r14
    mov esi, 1
    call sqlite3_column_text
    mov [rbp-128+R_CREATED], rax
    mov rdi, r14
    mov esi, 1
    call sqlite3_column_bytes
    mov eax, eax
    mov [rbp-128+R_CREATEDLEN], rax
    mov rax, [r13+C_BODY]
    mov [rbp-128+R_BODY], rax
    mov rax, [r13+C_BODYLEN]
    mov [rbp-128+R_BODYLEN], rax
    mov rax, [r13+C_UNAME]
    mov [rbp-128+R_AUTHOR], rax
    mov rax, [r13+C_UNAMELEN]
    mov [rbp-128+R_AUTHORLEN], rax
    mov qword [rbp-128+R_LIKES], 0
    ; render while created_at is still valid
    mov rdi, [r13+C_DST]
    lea rsi, [rel s_post_open]
    mov edx, 8
    call append_bytes
    mov rdi, rax
    lea rsi, [rbp-128]
    call render_post
    mov byte [rax], '}'
    inc rax
    sub rax, [r13+C_DST]
    mov [r13+C_OUTLEN], rax
    ; step to DONE so the implicit transaction is committed before we answer
.commit:
    mov rdi, r14
    call sqlite3_step
    cmp eax, SQLITE_DONE
    je .committed
    cmp eax, SQLITE_ROW
    je .commit
    mov rdi, r14
    call sqlite3_reset
    mov eax, -1
    jmp .out
.committed:
    mov rdi, r14
    call sqlite3_reset
    xor eax, eax
    jmp .out
.fail:
    mov rdi, r14
    call sqlite3_reset
    mov eax, -1
.out:
    call db_begin                ; reopen the pass's read transaction
    add rsp, 120
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; -----------------------------------------------------------------------------
; db_like(rdi=wctx, rsi=user_id, rdx=post_id)
;   -> eax = 0 inserted / 1 already liked / 2 post not found / -1 error
; -----------------------------------------------------------------------------
db_like:
    push rbp
    mov rbp, rsp
    push rbx
    push r12
    push r13
    push r14
    push r15
    sub rsp, 24
    mov r12, rdi
    mov r13, rsi
    mov r14, rdx
    call db_end                  ; a write must not join the pass's read txn
    mov r15, [r12+W_STMT_EXISTS]
    mov rdi, r15
    call sqlite3_reset
    mov rdi, r15
    mov esi, 1
    mov rdx, r14
    call sqlite3_bind_int64
    mov rdi, r15
    call sqlite3_step
    cmp eax, SQLITE_ROW
    je .exists
    cmp eax, SQLITE_DONE
    je .no_post
    mov rdi, r15
    call sqlite3_reset
    mov eax, -1
    jmp .out
.exists:
    mov rdi, r15
    call sqlite3_reset
    mov r15, [r12+W_STMT_LIKE]
    mov rdi, r15
    call sqlite3_reset
    mov rdi, r15
    mov esi, 1
    mov rdx, r13
    call sqlite3_bind_int64
    mov rdi, r15
    mov esi, 2
    mov rdx, r14
    call sqlite3_bind_int64
.insert:
    mov rdi, r15
    call sqlite3_step
    cmp eax, SQLITE_DONE
    je .inserted
    cmp eax, SQLITE_ROW
    je .insert
    mov rdi, r15
    call sqlite3_reset
    mov eax, -1
    jmp .out
.inserted:
    mov rdi, [r12+W_DB]
    call sqlite3_changes
    mov ebx, eax
    mov rdi, r15
    call sqlite3_reset
    test ebx, ebx
    jz .already
    xor eax, eax
    jmp .out
.already:
    mov eax, 1
    jmp .out
.no_post:
    mov rdi, r15
    call sqlite3_reset
    mov eax, 2
.out:
    call db_begin                ; reopen the pass's read transaction
    add rsp, 24
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

extern sqlite3_finalize
