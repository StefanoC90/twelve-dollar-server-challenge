; Shared constants and structure layouts for the feed server.
; NASM include file, no code.

; ---- Linux syscalls (x86-64) ----
%define SYS_read            0
%define SYS_write           1
%define SYS_close           3
%define SYS_writev          20
%define SYS_socket          41
%define SYS_accept          43
%define SYS_accept4         288
%define SYS_bind            49
%define SYS_listen          50
%define SYS_setsockopt      54
%define SYS_fcntl           72
%define SYS_epoll_create1   291
%define SYS_epoll_ctl       233
%define SYS_epoll_wait      232
%define SYS_clock_gettime   228
%define SYS_getrlimit       97
%define SYS_setrlimit       160
%define SYS_fork            57

%define RLIMIT_NOFILE       7

%define CLOCK_REALTIME      0
%define CLOCK_MONOTONIC     1

%define AF_INET             2
%define SOCK_STREAM         1
%define SOCK_NONBLOCK       0x800
%define SOL_SOCKET          1
%define SO_REUSEADDR        2
%define SO_REUSEPORT        15
%define SO_RCVTIMEO         20
%define SO_SNDTIMEO         21
%define IPPROTO_TCP         6
%define TCP_NODELAY         1

%define F_SETFL             4
%define O_NONBLOCK          0x800
%define MSG_DONTWAIT        0x40

%define EPOLLIN             0x001
%define EPOLLOUT            0x004
%define EPOLLERR            0x008
%define EPOLLHUP            0x010
%define EPOLLRDHUP          0x2000
%define EPOLL_CTL_ADD       1
%define EPOLL_CTL_DEL       2
%define EPOLL_CTL_MOD       3

%define EINTR               -4
%define EAGAIN              -11

; ---- event loop ----
%define MAX_CONN            65536
%define MAX_EVENTS          256
; How many connections the workers should be able to hold together. The fd
; limit is per process, so the server forks ceil(CONN_TARGET / per-worker cap)
; SO_REUSEPORT workers (1..8). A worker holds up to MAX_CONN connections on the
; scoring box, so this target is three workers and ~196k slots: enough that the
; fd limit is not what ends a run (the CPU/p95 cliff comes first). A held
; connection costs one 64-byte slot, so the whole target fits in ~13 MB.
; The extra event loop is free: with saturated /feed load on one pinned core,
; two, three and four workers cost 29, 29 and 28 us of CPU per request, since
; they only split the same work over the same core. Two workers were the
; earlier choice because they were the cheapest way to cover 130k fds; with
; that ceiling out of the way the worker count is purely an fd budget.
%define CONN_TARGET         195000
; Per-request read buffer. A request line plus headers plus a 500-character
; JSON body is under 1 KiB for every client in the benchmark model, and
; anything larger grows on demand, so this is only a floor.
; The buffer is not owned by the connection while it is idle: it is allocated
; when readable bytes arrive and released once the last complete request in it
; has been answered, so a keep-alive connection waiting out its think time
; holds no buffer at all. Measured on 20k held connections: 1153 -> 120 bytes
; of server RSS per connection.
%define CONN_INIT_CAP       1024
; How many released read buffers a worker keeps for reuse. Only buffers that
; are still CONN_INIT_CAP are pooled; a grown one goes back to the allocator.
%define BUFPOOL_MAX         1024
%define IDLE_TIMEOUT_MS     75000
%define SWEEP_INTERVAL_MS   2000

; one live connection, indexed by file descriptor (CONN_ prefix: C_* is taken
; by the db_create_post argument block below).
; The read buffer is only present while bytes are pending, so CONN_BUF cannot
; tell a live connection from an empty slot: CONN_LAST does. It holds the
; monotonic millisecond stamp of the last traffic (always non-zero in a running
; process) and is zeroed when the slot is released.
%define CONN_BUF            0
%define CONN_LEN            8
%define CONN_CAP            16
%define CONN_LAST           24
%define CONN_OUTBUF         32
%define CONN_OUTOFF         40
%define CONN_OUTLEN         48
%define CONN_OUTCAP         56
%define CONN_SIZE           64

; ---- HTTP ----
%define REQ_SIZE            65536
%define RESP_SIZE           262144
%define TMP_SIZE            32768
%define USER_SIZE           16384
%define HEADER_RESERVE      512

%define M_OTHER             0
%define M_GET               1
%define M_POST              2

%define ST_200              0
%define ST_201              1
%define ST_400              2
%define ST_401              3
%define ST_404              4
%define ST_500              5
%define ST_503              6

; parse_request output
%define P_METHOD            0
%define P_PATH              8
%define P_PATHLEN           16
%define P_VERSION11         24
%define P_CLOSE             28
%define P_AUTH              32
%define P_AUTHLEN           40
%define P_BODYLEN           48
%define P_BODYOFF           56
%define P_HEADEREND         64
%define P_SIZE              72

; worker context (one per thread)
%define W_DB                0
%define W_REQ               8
%define W_RESP              16
%define W_TMP               24
%define W_USER              32
%define W_STMT_FEED         40
%define W_STMT_FEEDL        48
%define W_STMT_GET          56
%define W_STMT_INS          64
%define W_STMT_EXISTS       72
%define W_STMT_LIKE         80
%define W_STMT_HEALTH       88
%define W_SIZE              96

; A read statement in autocommit mode is its own read transaction, and in WAL
; mode that costs three system calls every time: pagerPagecount()'s fstat, the
; WAL read-lock fcntl and its unlock fcntl. Under saturated /feed load strace
; counts 5 system calls per request, 3 of them that. One explicit transaction
; per event-loop pass (see db_begin/db_end) makes all the reads of the pass
; share a single one, so the three calls are paid once per pass instead of once
; per request. The pass is short (it is the batch epoll_wait just returned), and
; a write commits the transaction first so an acknowledged write is still
; committed before its response leaves.

; one rendered post
%define R_ID                0
%define R_BODY              8
%define R_BODYLEN           16
%define R_CREATED           24
%define R_CREATEDLEN        32
%define R_AUTHOR            40
%define R_AUTHORLEN         48
%define R_LIKES             56
%define R_SIZE              64

; db_create_post argument block
%define C_UID               0
%define C_BODY              8
%define C_BODYLEN           16
%define C_UNAME             24
%define C_UNAMELEN          32
%define C_DST               40
%define C_OUTLEN            48
%define C_SIZE              56

; JWT payload parse result
%define J_UID               0
%define J_UNAMELEN          8
%define J_EXP               16
%define J_FLAGS             24
%define J_SUBLEN            32
%define J_SIZE              40

%define JF_EXP              1
%define JF_SUB              2
%define JF_UNAME            4

; SQLite constants
%define SQLITE_OK           0
%define SQLITE_ROW          100
%define SQLITE_DONE         101
%define SQLITE_OPEN_RW      6          ; READWRITE | CREATE
%define SQLITE_TRANSIENT    -1
