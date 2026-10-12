/* ======================================================================
 * [해설판] networking.c 발췌 — 클라이언트 한 명의 일생
 * 원본: https://github.com/redis/redis/blob/unstable/src/networking.c (6131줄)
 * 기준 커밋: b540ca4 (2026-09-30)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본 6131줄 중 이벤트루프와 직접 연결된 함수만
 *   골라 "실행 순서대로" 다시 배치했습니다. 원본에서는 순서가 뒤섞여 있습니다.
 *   - 각 함수 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 너무 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다. 뺀 것 외에는 원본과 같은지 확인했습니다.
 *
 * ▶ 목차 (클라이언트의 일생 순서)
 *   [1] 접속    connSocketAcceptHandler(socket.c) → acceptCommonHandler → createClient → clientAcceptHandler
 *   [2] 읽기    readQueryFromClient
 *   [3] 파싱    processInputBuffer → processMultibulkBuffer
 *   [4] 실행    processCommandAndResetClient → (server.c) processCommand → call → 명령 함수
 *   [5] 응답 쌓기 addReply → _prepareClientToWrite → putClientInPendingWriteQueue
 *                         → _addReplyToBufferOrList → 버퍼 / 리스트
 *   [6] 응답 보내기 (beforeSleep) handleClientsWithPendingWrites → writeToClient
 *                         → 못 다 보내면 installClientWriteHandler → sendReplyToClient
 *   [7] 종료    freeClientAsync → beforeNextClient / freeClientsInAsyncFreeQueue → freeClient
 *   [8] 덤      processEventsWhileBlocked (오래 걸리는 작업 중에 루프를 잠깐 돌리기)
 *
 * ▶ 처음 읽을 때는 [2] [3] [5] [6]만 보세요. 나머지는 나중에.
 * ▶ 등장하는 C 타입 중 처음 보는 것:
 *   client     : 클라이언트 연결 하나의 모든 상태 (server.h에 정의, 필드 100개 이상)
 *   connection : 소켓 fd를 감싼 객체. TCP/TLS/Unix 소켓을 같은 방식으로 다루기 위한 추상화
 *   sds        : Redis의 문자열 타입 (Simple Dynamic String). Kotlin StringBuilder 비슷.
 *                sdslen(s) = 길이, sdsavail(s) = 남은 여유 공간
 *   robj       : Redis 값 객체 (redisObject). 명령 인자(argv)도 robj 배열
 *   list       : Redis의 양방향 연결 리스트 (adlist.c)
 *   C_OK / C_ERR : 0 / -1. 성공/실패
 * ====================================================================== */


/* ######################################################################
 * [1] 접속
 * ###################################################################### */

/* ======================================================================
 * ▶ [1-1] connSocketAcceptHandler — socket.c L301~L321 (networking.c가 아니라 socket.c)
 *   ae가 부르는 콜백입니다. server.c가 리슨 소켓(예: 6379 포트)에 대해
 *       aeCreateFileEvent(server.el, 리슨fd, AE_READABLE, accept_handler, ...)
 *   로 등록해두었습니다. 리슨 소켓이 "읽기 가능" = "새 접속이 대기 중"이라는 뜻입니다.
 *
 *   한 번 깨어날 때 최대 max_new_conns_per_cycle(기본 10)명만 받습니다.
 *   접속 폭주가 와도 기존 클라이언트의 명령 처리를 굶기지 않기 위해서입니다.
 *   남은 접속 대기자는? epoll이 level-triggered라서 다음 바퀴에 또 알려줍니다.
 * ====================================================================== */
static void connSocketAcceptHandler(aeEventLoop *el, int fd, void *privdata, int mask) {
    int cport, cfd;
    int max = server.max_new_conns_per_cycle;
    char cip[NET_IP_STR_LEN];
    UNUSED(mask);
    UNUSED(privdata);

    while(max--) {
        /* ▶ accept() 시스템 콜. 성공하면 새 클라이언트 전용 fd(cfd)가 생깁니다.
         *   리슨 소켓은 non-blocking이라, 대기자가 없으면 기다리지 않고 EWOULDBLOCK으로 실패합니다. */
        cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport);
        if (cfd == ANET_ERR) {
            if (anetAcceptFailureNeedsRetry(errno))
                continue;
            if (errno != EWOULDBLOCK)
                serverLog(LL_WARNING,
                    "Accepting client connection: %s", server.neterr);
            return;   /* ▶ 더 받을 접속이 없음 → 끝 */
        }
        serverLog(LL_VERBOSE,"Accepted %s:%d", cip, cport);
        /* ▶ fd를 connection 객체로 감싸서 공통 처리로 넘김 */
        acceptCommonHandler(connCreateAcceptedSocket(el,cfd,NULL), 0, cip);
    }
}

/* ======================================================================
 * ▶ [1-2] acceptCommonHandler — networking.c L1725~L1799
 *   TCP, TLS, Unix 소켓 모두 결국 여기로 옵니다. 하는 일:
 *     1) maxclients 초과면 에러 메시지 보내고 끊기
 *     2) createClient로 client 객체 생성 (+ 읽기 이벤트 등록)
 *     3) connAccept 호출 (TLS면 핸드셰이크 시작, 일반 TCP면 바로 clientAcceptHandler 호출)
 * ====================================================================== */
void acceptCommonHandler(connection *conn, int flags, char *ip) {
    client *c;
    UNUSED(ip);

    if (connGetState(conn) != CONN_STATE_ACCEPTING) {
        char addr[NET_ADDR_STR_LEN] = {0};
        char laddr[NET_ADDR_STR_LEN] = {0};
        connFormatAddr(conn, addr, sizeof(addr), 1);
        connFormatAddr(conn, laddr, sizeof(laddr), 0);
        serverLog(LL_VERBOSE,
                  "Accepted client connection in error state: %s (addr=%s laddr=%s)",
                  connGetLastError(conn), addr, laddr);
        connClose(conn);
        return;
    }

    /* Limit the number of connections we take at the same time.
     *
     * Admission control will happen before a client is created and connAccept()
     * called, because we don't want to even start transport-level negotiation
     * if rejected. */
    if (listLength(server.clients) + getClusterConnectionsCount()
        >= server.maxclients)
    {
        char *err;
        if (server.cluster_enabled)
            err = "-ERR max number of clients + cluster "
                  "connections reached\r\n";
        else
            err = "-ERR max number of clients reached\r\n";

        /* That's a best effort error message, don't check write errors.
         * Note that for TLS connections, no handshake was done yet so nothing
         * is written and the connection will just drop. */
        /* ▶ 여기는 이벤트루프를 거치지 않고 소켓에 바로 write합니다.
         *   보내지든 말든 상관없는 "최선 노력" 메시지라서 그렇습니다. */
        if (connWrite(conn,err,strlen(err)) == -1) {
            /* Nothing to do, Just to avoid the warning... */
        }
        server.stat_rejected_conn++;
        connClose(conn);
        return;
    }

    /* Create connection and client */
    if ((c = createClient(conn)) == NULL) {
        char addr[NET_ADDR_STR_LEN] = {0};
        char laddr[NET_ADDR_STR_LEN] = {0};
        connFormatAddr(conn, addr, sizeof(addr), 1);
        connFormatAddr(conn, laddr, sizeof(laddr), 0);
        serverLog(LL_WARNING,
                  "Error registering fd event for the new client connection: %s (addr=%s laddr=%s)",
                  connGetLastError(conn), addr, laddr);
        connClose(conn); /* May be already closed, just ignore errors */
        return;
    }

    /* Last chance to keep flags */
    c->flags |= flags;

    /* Initiate accept.
     *
     * Note that connAccept() is free to do two things here:
     * 1. Call clientAcceptHandler() immediately;
     * 2. Schedule a future call to clientAcceptHandler().
     *
     * Because of that, we must do nothing else afterwards.
     */
    /* ▶ 일반 TCP(socket.c connSocketAccept): 상태를 CONNECTED로 바꾸고 clientAcceptHandler를 즉시 호출.
     *   TLS(tls.c): 핸드셰이크는 여러 번 주고받아야 하므로 이벤트루프에 맡기고 나중에 호출.
     *   "같은 코드가 동기일 수도, 비동기일 수도 있다" → 그래서 이 뒤에는 아무것도 하면 안 됨. */
    if (connAccept(conn, clientAcceptHandler) == C_ERR) {
        if (connGetState(conn) == CONN_STATE_ERROR)
            serverLog(LL_WARNING,
                      "Error accepting a client connection: %s (addr=%s laddr=%s)",
                      connGetLastError(conn), getClientPeerId(c), getClientSockname(c));
        freeClient(connGetPrivateData(conn));
        return;
    }
}

/* ======================================================================
 * ▶ [1-3] createClient — networking.c L122~L266 (필드 초기화 대부분 생략)
 *   ★ 이 함수의 핵심은 맨 위 if (conn) { ... } 블록입니다. ★
 *       connSetReadHandler(conn, readQueryFromClient);
 *   → socket.c의 connSocketSetReadHandler
 *   → aeCreateFileEvent(el, fd, AE_READABLE, connSocketEventHandler, conn)
 *   이 한 줄로 "이 클라이언트 fd에 데이터가 오면 readQueryFromClient를 불러줘"가 등록됩니다.
 *   ae.c와 networking.c가 연결되는 지점입니다.
 *
 *   호출 경로를 정리하면:
 *     epoll_wait가 fd 깨움 → ae.c aeProcessEvents → connSocketEventHandler(socket.c)
 *     → conn->read_handler = readQueryFromClient (networking.c)
 * ====================================================================== */
client *createClient(connection *conn) {
    client *c = zmalloc(sizeof(client));

    /* passing NULL as conn it is possible to create a non connected client.
     * This is useful since all the commands needs to be executed
     * in the context of a client. When commands are executed in other
     * contexts (for instance a Lua script) we need a non connected client. */
    /* ▶ conn == NULL인 "가짜 클라이언트"도 있습니다. Lua 스크립트나 AOF 로딩이 명령을
     *   실행할 때 씁니다. Redis에서 모든 명령은 "어떤 client의 요청"으로 실행되기 때문입니다. */
    if (conn) {
        /* ▶ Nagle 알고리즘 끄기: 작은 패킷을 모아 보내느라 지연되는 것을 막음 (저지연 우선) */
        connEnableTcpNoDelay(conn);
        if (server.tcpkeepalive)
            connKeepAlive(conn,server.tcpkeepalive);   /* ▶ 기본 300초. 죽은 연결 감지 */
        connSetReadHandler(conn, readQueryFromClient);  /* ▶ ★ 읽기 이벤트 등록 ★ */
        connSetPrivateData(conn, c);    /* ▶ conn → client 역참조. 콜백에서 client를 꺼내는 데 씀 */
    }
    /* ▶ 응답용 고정 버퍼 16KB (PROTO_REPLY_CHUNK_BYTES). 작은 응답은 여기에만 담깁니다. */
    c->buf = zmalloc_usable(PROTO_REPLY_CHUNK_BYTES, &c->buf_usable_size);
    selectDb(c,0);
    uint64_t client_id;
    atomicGetIncr(server.next_client_id, client_id, 1);
    c->id = client_id;
    /* ▶ tid / running_tid: 이 클라이언트를 맡은 스레드 번호. 처음엔 모두 메인 스레드(0).
     *   io-threads > 1이면 clientAcceptHandler에서 I/O 스레드로 넘어갑니다. */
    c->tid = IOTHREAD_MAIN_THREAD_ID;
    c->running_tid = IOTHREAD_MAIN_THREAD_ID;
    if (conn) server.io_threads_clients_num[c->tid]++;
    /* ▶ [생략] 원본 L144~L249: 필드 초기화 약 100줄.
     *   이벤트루프 이해에 필요한 필드만 추리면:
     *     querybuf = NULL, qb_pos = 0       — 받은 데이터 버퍼와 파싱 위치
     *     reqtype = 0, multibulklen = 0, bulklen = -1 — RESP 파서 상태
     *     argc = 0, argv = NULL             — 파싱된 명령 인자
     *     bufpos = 0, reply = listCreate()  — 보낼 응답 (고정 버퍼 + 넘치면 리스트)
     *     sentlen = 0                       — 응답을 어디까지 보냈나
     *     flags = 0                         — CLIENT_PENDING_WRITE, CLIENT_CLOSE_ASAP 등 상태 비트 */
    if (conn) linkClient(c);   /* ▶ server.clients 전역 리스트에 추가 (CLIENT LIST가 보는 목록) */
    /* ▶ [생략] 원본 L251~L263: 나머지 필드 초기화 */
    atomicSet(c->pending_read, 0);
    return c;
}

/* ======================================================================
 * ▶ [1-4] clientAcceptHandler — networking.c L1652~L1723 (일부 생략)
 *   접속이 완전히 성립된 뒤 불립니다. protected mode 검사, 통계,
 *   그리고 I/O 스레드 배정을 합니다.
 * ====================================================================== */
void clientAcceptHandler(connection *conn) {
    client *c = connGetPrivateData(conn);

    if (connGetState(conn) != CONN_STATE_CONNECTED) {
        serverLog(LL_WARNING,
                  "Error accepting a client connection: %s (addr=%s laddr=%s)",
                  connGetLastError(conn), getClientPeerId(c), getClientSockname(c));
        freeClientAsync(c);
        return;
    }

    /* If the server is running in protected mode (the default) and there
     * is no password set, nor a specific interface is bound, we don't accept
     * requests from non loopback interfaces. Instead we try to explain the
     * user what to do to fix it if needed. */
    /* ▶ [생략] 원본 L1667~L1714:
     *   protected-mode가 켜져 있고 비밀번호가 없으면, 외부(127.0.0.1이 아닌 곳)에서 온 접속에
     *   "-DENIED Redis is running in protected mode ..." 메시지를 보내고 끊습니다.
     *   이어서 TLS 인증서의 사용자 이름으로 자동 로그인하는 처리가 있습니다. */

    server.stat_numconnections++;
    moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE,
                          REDISMODULE_SUBEVENT_CLIENT_CHANGE_CONNECTED,
                          c);

    /* Assign the client to an IO thread */
    /* ▶ io-threads 설정이 2 이상이면, 이 클라이언트의 읽기/쓰기를 I/O 스레드에게 넘깁니다.
     *   I/O 스레드는 자기 aeEventLoop에 이 fd를 다시 등록합니다 (iothread.c).
     *   기본값 io-threads=1이면 모든 일을 메인 스레드가 합니다. */
    if (server.io_threads_num > 1) assignClientToIOThread(c);
}


/* ######################################################################
 * [2] 읽기
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [2] readQueryFromClient — networking.c L3990~L4157 ★
 *   클라이언트 fd가 "읽기 가능"할 때 불리는 읽기 핸들러입니다.
 *
 *   하는 일 (요약):
 *     ① 지금 이 스레드에 읽기 권한이 있는지 확인 (메인과 I/O 스레드 사이를 옮겨 가는 중이면 읽지 않음)
 *        I/O 스레드도 자기 루프에서 이 같은 함수를 실행합니다.
 *     ② 이번에 몇 바이트 읽을지 결정 (기본 16KB)
 *     ③ querybuf 준비 (스레드 공용 버퍼 or 클라이언트 전용 버퍼)
 *     ④ read() 한 번  ← ★ 소켓에서 실제로 데이터를 가져오는 곳
 *     ⑤ 0바이트 = 상대가 끊음 → 종료 예약 / 너무 큼 → 종료 예약
 *     ⑥ processInputBuffer로 파싱 + 실행
 *     ⑦ 정리 (공용 버퍼 반납, 종료 예약된 클라이언트면 즉시 해제)
 *
 *   ★ 핵심: read()는 "딱 한 번"만 부릅니다. 데이터가 더 남아 있어도 반복하지 않습니다.
 *     남은 데이터는 epoll(level-triggered)이 다음 바퀴에 또 알려줍니다.
 *     한 클라이언트가 계속 데이터를 쏟아내도 다른 클라이언트에게 차례가 돌아가게 하기 위해서입니다.
 *
 *   Kotlin 비유:
 *   fun readQueryFromClient(conn: Connection) {
 *       val c = conn.privateData as Client
 *       val n = conn.read(c.querybuf, 16 * 1024)     // 한 번만
 *       if (n == 0) { freeClientAsync(c); return }    // 상대가 끊음
 *       processInputBuffer(c)
 *   }
 * ====================================================================== */
void readQueryFromClient(connection *conn) {
    client *c = connGetPrivateData(conn);   /* ▶ createClient에서 달아둔 client 꺼내기 */
    int nread, big_arg = 0;
    size_t qblen, readlen;

    /* We have to read compressed data but compression for this client is
     * currently disabled. This could happened f.e when client was just fetched
     * to main thread. */
    /* ▶ 압축 통신(client_comp.c)은 unstable에만 있는 새 기능입니다. 8.0에는 없습니다. 무시해도 됩니다. */
    int pending_compression_read = c->compression_state &&
                                   !(c->io_flags & CLIENT_IO_COMPRESSION_ENABLED);

    /* ▶ ① 지금 이 스레드가 읽어도 되나?
     *   클라이언트가 메인 스레드와 I/O 스레드 사이를 옮겨 다니는 중이면 읽기가 잠시 막혀 있습니다.
     *   그때는 "읽을 게 있었다"(pending_read)만 표시하고 돌아갑니다. */
    if (!(c->io_flags & CLIENT_IO_READ_ENABLED) || pending_compression_read) {
        atomicSetWithSync(c->pending_read, 1);
        return;
    } else if (server.io_threads_num > 1) {
        atomicSetWithSync(c->pending_read, 0);
    }

    c->read_error = 0;

    c->stat_total_read_events++;

    /* Update the number of reads of io threads on server */
    atomicIncr(IOThreads[c->running_tid].io_reads_processed, 1);

    /* ▶ ② 기본으로 16KB(PROTO_IOBUF_LEN)씩 읽습니다. */
    readlen = PROTO_IOBUF_LEN;
    /* If this is a multi bulk request, and we are processing a bulk reply
     * that is large enough, try to maximize the probability that the query
     * buffer contains exactly the SDS string representing the object, even
     * at the risk of requiring more read(2) calls. This way the function
     * processMultiBulkBuffer() can avoid copying buffers to create the
     * Redis Object representing the argument. */
    /* ▶ 큰 인자(32KB 이상, 예: SET key <1MB 값>)를 받는 중이면, 그 인자에 "딱 필요한 만큼만" 읽습니다.
     *   그러면 querybuf가 정확히 그 값 하나만 담게 되고, 파서가 복사 없이 querybuf 자체를
     *   값 객체로 넘겨줄 수 있습니다(processMultibulkBuffer의 Optimization 부분). 1MB 복사를 아끼는 최적화. */
    if (c->reqtype == PROTO_REQ_MULTIBULK && c->multibulklen && c->bulklen != -1
        && c->bulklen >= PROTO_MBULK_BIG_ARG)
    {
        /* For big argv, the client always uses its private query buffer.
         * Using the reusable query buffer would eventually expand it beyond 32k,
         * causing the client to take ownership of the reusable query buffer. */
        if (!c->querybuf) c->querybuf = sdsempty();

        ssize_t remaining = (size_t)(c->bulklen+2)-(sdslen(c->querybuf)-c->qb_pos);
        big_arg = 1;

        /* Note that the 'remaining' variable may be zero in some edge case,
         * for example once we resume a blocked client after CLIENT PAUSE. */
        if (remaining > 0) readlen = remaining;

        /* Master client needs expand the readlen when meet BIG_ARG(see #9100),
         * but doesn't need align to the next arg, we can read more data. */
        if (c->flags & CLIENT_MASTER && readlen < PROTO_IOBUF_LEN)
            readlen = PROTO_IOBUF_LEN;
    } else if (c->querybuf == NULL) {
        /* ▶ ③ 스레드 공용 query buffer (thread_reusable_qb)
         *   클라이언트 1만 명이 각자 16KB 버퍼를 들고 있으면 160MB입니다. 그런데 대부분의 순간에
         *   대부분의 클라이언트는 버퍼가 비어 있습니다. 그래서 "남은 데이터가 없는" 클라이언트는
         *   스레드당 하나뿐인 공용 버퍼를 빌려 쓰고, 다 처리하면 반납합니다(⑦의 resetReusableQueryBuf).
         *   한 스레드는 한 번에 한 클라이언트만 처리하니까 가능한 트릭입니다.
         *   __thread = 스레드마다 따로 있는 전역 변수. Kotlin의 ThreadLocal. */
        if (unlikely(thread_reusable_qb_used)) {
            /* The reusable query buffer is already used by another client,
             * switch to using the client's private query buffer. This only
             * occurs when commands are executed nested via processEventsWhileBlocked(). */
            c->querybuf = sdsnewlen(NULL, PROTO_IOBUF_LEN);
            sdsclear(c->querybuf);
        } else {
            /* Create the reusable query buffer if it doesn't exist. */
            if (!thread_reusable_qb) {
                thread_reusable_qb = sdsnewlen(NULL, PROTO_IOBUF_LEN);
                sdsclear(thread_reusable_qb);
            }

            /* Assign the reusable query buffer to the client and mark it as in use. */
            serverAssert(sdslen(thread_reusable_qb) == 0);
            c->querybuf = thread_reusable_qb;
            c->io_flags |= CLIENT_IO_REUSABLE_QUERYBUFFER;
            thread_reusable_qb_used = 1;
        }
    }

    /* ▶ 버퍼 끝에 readlen만큼 빈 공간 확보 (ArrayList.ensureCapacity 같은 것) */
    qblen = sdslen(c->querybuf);
    if (!(c->flags & CLIENT_MASTER) && // master client's querybuf can grow greedy.
        (big_arg || sdsalloc(c->querybuf) < PROTO_IOBUF_LEN)) {
        /* When reading a BIG_ARG we won't be reading more than that one arg
         * into the query buffer, so we don't need to pre-allocate more than we
         * need, so using the non-greedy growing. For an initial allocation of
         * the query buffer, we also don't wanna use the greedy growth, in order
         * to avoid collision with the RESIZE_THRESHOLD mechanism. */
        c->querybuf = sdsMakeRoomForNonGreedy(c->querybuf, readlen);
        /* We later set the peak to the used portion of the buffer, but here we over
         * allocated because we know what we need, make sure it'll not be shrunk before used. */
        if (c->querybuf_peak < qblen + readlen) c->querybuf_peak = qblen + readlen;
    } else {
        c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);

        /* Read as much as possible from the socket to save read(2) system calls. */
        readlen = sdsavail(c->querybuf);
    }
    /* Compression is applied at the client level (see client_comp.c): when
     * decompression is active `nread` is the number of decompressed bytes placed
     * in the query buffer while `network_read` is the number of bytes actually
     * read from the socket. Without compression they are equal. */
    int network_read = 0;
    /* ▶ ④ ★ 여기서 소켓 read() ★  querybuf의 기존 데이터 "뒤에" 이어 붙입니다.
     *   반환값 nread:
     *     > 0 : 읽은 바이트 수
     *     0   : 상대방이 연결을 닫음 (EOF)
     *     -1  : 에러. 단, EAGAIN(지금은 읽을 게 없음)이면 연결은 멀쩡함 */
    nread = clientConnRead(c, c->querybuf+qblen, readlen, &network_read);
    if (nread == -1) {
        if (connGetState(conn) == CONN_STATE_CONNECTED) {
            goto done;    /* ▶ EAGAIN 같은 일시적 상황. 다음 기회에 */
        } else {
            c->read_error = CLIENT_READ_CONN_DISCONNECTED;
            freeClientAsync(c);   /* ▶ 진짜 에러 → 종료 "예약" ([7] 참고) */
            goto done;
        }
    } else if (nread == 0) {
        /* ▶ ⑤ 상대가 끊음 (redis-cli 종료 등) → 종료 예약 */
        c->read_error = CLIENT_READ_CONN_CLOSED;
        freeClientAsync(c);
        goto done;
    }

    /* ▶ read()는 sds 내부 메모리에 직접 썼으므로, sds에게 "길이가 nread만큼 늘었다"고 알려줌 */
    sdsIncrLen(c->querybuf,nread);
    qblen = sdslen(c->querybuf);
    if (c->querybuf_peak < qblen) c->querybuf_peak = qblen;

    /* ▶ 마지막 활동 시각 갱신. serverCron이 timeout 설정에 따라 오래 조용한 클라이언트를 끊을 때 씀 */
    if (!(c->flags & CLIENT_MASTER) || c->running_tid == IOTHREAD_MAIN_THREAD_ID)
        c->lastinteraction = server.unixtime;
    else
        /* Avoid contention with genRedisInfoString as it can access master
         * client's data. If this is a master running in IO thread the value of
         * c->lastinteraction will be updated during processClientsFromIOThread */
        c->io_lastinteraction = server.unixtime;

    /* ▶ 이 아래 CLIENT_MASTER 분기는 "이 서버가 레플리카이고, c가 마스터와의 연결"인 경우입니다.
     *   복제 이야기라서 이벤트루프 공부 단계에서는 건너뛰어도 됩니다. */
    if (c->flags & CLIENT_MASTER) {
        if (c->running_tid == IOTHREAD_MAIN_THREAD_ID) {
            c->read_reploff += nread;
        } else {
            /* Same comment as for c->io_lastinteraction */
            c->io_read_reploff += nread;
        }
        atomicIncr(server.stat_net_repl_input_bytes, network_read);
    } else {
        atomicIncr(server.stat_net_input_bytes, network_read);
    }
    c->net_input_bytes += network_read;

    /* ▶ ⑤ 받은 데이터가 너무 크면 끊기. client-query-buffer-limit(기본 1GB).
     *   인증 안 한 클라이언트는 1MB까지만 허용 (공격 방어). */
    if (!(c->flags & CLIENT_MASTER) &&
        /* The commands cached in the MULTI/EXEC queue have not been executed yet,
         * so they are also considered a part of the query buffer in a broader sense.
         *
         * For unauthenticated clients, the query buffer cannot exceed 1MB at most. */
        (c->mstate.argv_len_sums + sdslen(c->querybuf) > server.client_max_querybuf_len ||
         (c->mstate.argv_len_sums + sdslen(c->querybuf) > 1024*1024 && authRequired(c))))
    {
        c->read_error = CLIENT_READ_REACHED_MAX_QUERYBUF;
        freeClientAsync(c);
        atomicIncr(server.stat_client_qbuf_limit_disconnections, 1);
        goto done;
    }

    /* There is more data in the client input buffer, continue parsing it
     * and check if there is a full command to execute. */
    /* ▶ ⑥ ★ 파싱 + 실행 ([3]) ★
     *   C_ERR = 처리 도중 client가 해제됨 → 이후 c를 만지면 안 되므로 NULL로 바꿔둠 */
    if (processInputBuffer(c) == C_ERR)
         c = NULL;

done:
    /* ▶ ⑦ 정리 */
    if (c && isClientReadErrorFatal(c)) {
        if (c->running_tid == IOTHREAD_MAIN_THREAD_ID) {
            handleClientReadError(c);   /* ▶ 프로토콜 에러 메시지 응답 + 종료 예약 */
        }
    }

    /* ▶ 공용 버퍼를 빌려 썼으면 반납. (처리 안 된 데이터가 남았으면 공용 버퍼를 통째로 이 클라이언트
     *   소유로 넘기고(복사 없음), 스레드는 다음에 새 공용 버퍼를 만든다) */
    if (c && (c->io_flags & CLIENT_IO_REUSABLE_QUERYBUFFER)) {
        serverAssert(c->qb_pos == 0); /* Ensure the client's query buffer is trimmed in processInputBuffer */
        resetReusableQueryBuf(c);
    }
    /* ▶ 이 클라이언트에 종료 예약(CLOSE_ASAP)이 걸려 있으면 지금 바로 해제 ([7]) */
    beforeNextClient(c);
}


/* ######################################################################
 * [3] 파싱
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [3-1] processInputBuffer — networking.c L3787~L3988 ★
 *   querybuf에 쌓인 바이트를 명령 단위로 잘라서 실행합니다.
 *
 *   ★ 중요한 사실: 한 번의 read()에 명령이 0개, 1개, 여러 개 들어 있을 수 있습니다.
 *     - 0개 반 : "SET k" 까지만 도착 → 파싱 중단, 다음 read를 기다림 (파서 상태는 client에 저장)
 *     - 여러 개: 파이프라인. redis-benchmark -P 16 처럼 명령 16개를 한꺼번에 보냄
 *     그래서 이 함수는 while 루프로 "버퍼가 빌 때까지" 반복합니다.
 *
 *   unstable의 큰 변화: lookahead(기본 16)
 *     8.0: 명령 1개 파싱 → 실행 → 1개 파싱 → 실행 ...
 *     unstable: 최대 16개를 먼저 파싱해 pending_cmds에 쌓고 → 키 메모리를 미리 CPU 캐시로 당겨옴(prefetch)
 *               → 하나씩 실행. 파이프라인 처리 속도를 높이는 최적화입니다.
 *     구조가 복잡해 보이는 이유가 이것입니다. 처음 읽을 때는 "파싱 → 실행 반복"으로만 보세요.
 *
 *   I/O 스레드에서 불렸으면? 파싱까지만 하고 실행은 메인 스레드에 넘깁니다(CLIENT_IO_PENDING_COMMAND).
 *   ★ 명령 "실행"은 항상 메인 스레드 하나에서만 일어납니다. 그래서 Redis 명령은 원자적입니다. ★
 * ====================================================================== */
int processInputBuffer(client *c) {
    atomicIncr(server.stat_total_client_process_input_buff_events, 1);

    /* Keep active-client window updates on main-thread paths only (here and
     * in IO-thread handoff processing) to avoid races with serverCron()
     * maintenance of the circular slots. */
    if (c->running_tid == IOTHREAD_MAIN_THREAD_ID)
        statsUpdateActiveClients(c);

    /* We limit the lookahead for unauthenticated connections to 1.
     * This is both to reduce memory overhead, and to prevent errors: AUTH can
     * affect the handling of succeeding commands. Parsing of "large"
     * unauthenticated multibulk commands is rejected, which would cause those
     * commands to incorrectly return an error to the client. */
    const int lookahead = authRequired(c) ? 1 : server.lookahead;

    /* Keep processing while there is something in the input buffer */
    /* ▶ 아직 파싱 안 한 바이트가 있거나(qb_pos < 길이), 파싱은 끝났지만 실행 안 한 명령이 있는 동안 반복 */
    while ((c->querybuf && c->qb_pos < sdslen(c->querybuf)) ||
           c->pending_cmds.ready_len > 0)
    {
        /* ▶ ---- 멈춰야 하는 경우들 ---- */
        /* Immediately abort if the client is in the middle of something. */
        /* ▶ BLPOP 같은 블로킹 명령으로 대기 중이면, 뒤에 온 명령은 처리하지 않고 쌓아둠 */
        if (c->flags & CLIENT_BLOCKED || c->flags & CLIENT_UNBLOCKED) break;

        /* Don't process more buffers from clients that have already pending
         * commands to execute in c->argv. */
        if (c->flags & CLIENT_PENDING_COMMAND) break;

        /* Don't process input from the master while there is a busy script
         * condition on the slave. We want just to accumulate the replication
         * stream (instead of replying -BUSY like we do with other clients) and
         * later resume the processing. */
        if (c->flags & CLIENT_MASTER && isInsideYieldingLongCommand()) break;

        /* CLIENT_CLOSE_AFTER_REPLY closes the connection once the reply is
         * written to the client. Make sure to not let the reply grow after
         * this flag has been set (i.e. don't process more commands).
         *
         * The same applies for clients we want to terminate ASAP. */
        /* ▶ QUIT을 받았거나 종료 예약된 클라이언트면 더 실행하지 않음 */
        if (c->flags & (CLIENT_CLOSE_AFTER_REPLY|CLIENT_CLOSE_ASAP)) break;

        /* Determine if we need to parse more commands from the query buffer.
         * Only parse when there are no ready commands waiting to be processed. */
        const int parse_more = !c->pending_cmds.ready_len;
        int pending_cmd_before_reading = c->pending_cmds.ready_len;

        /* Parse up to lookahead commands only if we don't have enough ready commands */
        /* ▶ ---- 파싱 단계: 최대 lookahead개까지 명령을 잘라냄 ---- */
        while (parse_more && c->pending_cmds.ready_len < lookahead &&
               c->querybuf && c->qb_pos < sdslen(c->querybuf))
        {
            /* Determine request type when unknown. */
            /* ▶ 첫 글자로 프로토콜 종류 판단
             *   '*' → RESP 배열 (redis-cli, 모든 클라이언트 라이브러리가 쓰는 정식 형식)
             *   그 외 → inline (telnet으로 "SET k v\r\n"처럼 사람이 직접 칠 때) */
            if (!c->reqtype) {
                if (c->querybuf[c->qb_pos] == '*') {
                    c->reqtype = PROTO_REQ_MULTIBULK;
                } else {
                    c->reqtype = PROTO_REQ_INLINE;
                }
            }

            pendingCommand *pcmd = NULL;
            if (c->reqtype == PROTO_REQ_INLINE) {
                pcmd = acquirePendingCommand();
                if (processInlineBuffer(c, pcmd) == C_ERR && !pcmd->read_error) {
                    /* If it fails but there are no errors, it means that it might just be
                     * that the desired content cannot be parsed. At this point, we exit and wait for the next time. */
                    freePendingCommand(c, pcmd);
                    break;
                }
            } else if (c->reqtype == PROTO_REQ_MULTIBULK) {
                /* ▶ 지난번 read에서 반쯤 파싱하다 만 명령이 있으면 그걸 이어서 파싱 */
                int incomplete = (c->pending_cmds.len != c->pending_cmds.ready_len);
                if (unlikely(incomplete)) {
                    pcmd = popPendingCommandFromTail(&c->pending_cmds);
                } else {
                    pcmd = acquirePendingCommand();
                }

                /* ▶ C_ERR인데 에러가 아니면 = 데이터가 덜 옴. 다음 read를 기다림 */
                if (processMultibulkBuffer(c, pcmd) == C_ERR && !pcmd->read_error) {
                    /* If it fails but there are no errors, it means that it might just be
                     * that the desired content cannot be parsed. At this point, we exit and wait for the next time. */
                    freePendingCommand(c, pcmd);
                    break;
                }
            } else {
                serverPanic("Unknown request type");
            }

            addPendingCommand(&c->pending_cmds, pcmd);
            if (unlikely(pcmd->read_error || (pcmd->flags & PENDING_CMD_FLAG_INCOMPLETE)))
                break;

            if (c->running_tid == IOTHREAD_MAIN_THREAD_ID)
                pcmd->reploff = c->read_reploff - sdslen(c->querybuf) + c->qb_pos;
            else
                pcmd->reploff = c->io_read_reploff - sdslen(c->querybuf) + c->qb_pos;

            /* ▶ 명령 이름 조회, 키 위치/슬롯 계산 등 실행 전 준비를 미리 해둠 */
            preprocessCommand(c, pcmd);
            resetClientQbufState(c);   /* ▶ 파서 상태 초기화 → 다음 명령 파싱 준비 */
        }

        if (c->pending_cmds.ready_len != pending_cmd_before_reading) {
            int newly_parsed_cmds = c->pending_cmds.ready_len - pending_cmd_before_reading;
            atomicIncr(server.stat_avg_pipeline_length_sum, newly_parsed_cmds);
            atomicIncr(server.stat_avg_pipeline_length_cnt, 1);

            c->stat_avg_pipeline_length_sum += newly_parsed_cmds;
            c->stat_avg_pipeline_length_cnt++;
        }

        /* Try to consume the next ready command from the pending command list. */
        /* ▶ ---- 실행 단계: 준비된 명령 중 맨 앞 하나를 꺼냄 ---- */
        if (!c->pending_cmds.ready_len)
            break;    /* ▶ 완성된 명령이 하나도 없음 = 데이터가 덜 옴 → 루프 종료 */
        pendingCommand *curcmd = c->pending_cmds.head;

        /* We populate the old client fields so we don't have to modify all existing logic to work with pendingCommands */
        /* ▶ 꺼낸 명령을 c->argc / c->argv로 옮김. 기존 코드(processCommand 등)는 c->argv만 보기 때문.
         *   예: SET k v → argc=3, argv=["SET","k","v"] */
        c->argc = curcmd->argc;
        c->argv = curcmd->argv;
        c->argv_len = curcmd->argv_len;
        c->net_input_bytes_curr_cmd += curcmd->input_bytes;
        c->reploff_next = curcmd->reploff;
        c->slot = curcmd->slot;
        c->lookedcmd = curcmd->cmd;
        c->read_error = curcmd->read_error;
        c->current_pending_cmd = curcmd;

        /* Prefetch the command only when more commands have been parsed and we
         * are in the main thread. If running in an IO thread, prefetch will be
         * deferred until the client is processed by the main thread. Skip prefetch
         * if there are too few commands to avoid meaningless prefetching. */
        if (parse_more && c->running_tid == IOTHREAD_MAIN_THREAD_ID &&
            c->pending_cmds.ready_len > 1)
        {
            /* Prefetch the commands. */
            resetCommandsBatch();
            addCommandToBatch(c);
            prefetchCommands();
        }

        /* Check if the client has a fatal read error that requires stopping processing. */
        if (isClientReadErrorFatal(c)) {
            if (c->running_tid != IOTHREAD_MAIN_THREAD_ID) {
                enqueuePendingClientsToMainThread(c, 0);
            }
            break;
        }

        /* Multibulk processing could see a <= 0 length. */
        if (!c->argc) {
            /* A naked newline can be sent from masters as a keep-alive, or from slaves to refresh
             * the last ACK time. In that case there's no command to actually execute. */
            prepareForNextCommand(c, 0);
        } else {
            /* If we are in the context of an I/O thread, we can't really
             * execute the command here. All we can do is to flag the client
             * as one that needs to process the command. */
            /* ▶ ★ I/O 스레드는 여기서 멈춥니다 ★
             *   "실행할 명령 있음" 표시를 하고 클라이언트를 메인 스레드 대기열로 보냅니다.
             *   메인 스레드가 beforeSleep(processClientsOfAllIOThreads) 등에서 받아 실행합니다. */
            if (c->running_tid != IOTHREAD_MAIN_THREAD_ID) {
                c->io_flags |= CLIENT_IO_PENDING_COMMAND;
                enqueuePendingClientsToMainThread(c, 0);
                break;
            }

            /* We are finally ready to execute the command. */
            /* ▶ ★ 명령 실행 ([4]) ★ */
            if (processCommandAndResetClient(c) == C_ERR) {
                /* If the client is no longer valid, we avoid exiting this
                 * loop and trimming the client buffer later. So we return
                 * ASAP in that case. */
                return C_ERR;
            }
        }
    }

    /* ▶ ---- 루프 종료 후: 처리한 앞부분을 querybuf에서 잘라냄 ---- */
    if (c->flags & CLIENT_MASTER) {
        /* If the client is a master, trim the querybuf to repl_applied,
         * since master client is very special, its querybuf not only
         * used to parse command, but also proxy to sub-replicas.
         *
         * Here are some scenarios we cannot trim to qb_pos:
         * 1. we don't receive complete command from master
         * 2. master client blocked cause of client pause
         * 3. io threads operate read, master client flagged with CLIENT_PENDING_COMMAND
         *
         * In these scenarios, qb_pos points to the part of the current command
         * or the beginning of next command, and the current command is not applied yet,
         * so the repl_applied is not equal to qb_pos. */
        if (c->repl_applied) {
            sdsrange(c->querybuf,c->repl_applied,-1);
            serverAssert(c->qb_pos >= (size_t)c->repl_applied);
            c->qb_pos -= c->repl_applied;
            c->repl_applied = 0;
        }
    } else if (c->qb_pos) {
        /* Trim to pos */
        /* ▶ querybuf = querybuf[qb_pos..끝]. 덜 온 명령의 조각만 남김.
         *   Kotlin: sb.delete(0, qbPos); qbPos = 0 */
        sdsrange(c->querybuf,c->qb_pos,-1);
        c->qb_pos = 0;
    }

    /* Update client memory usage after processing the query buffer, this is
     * important in case the query buffer is big and wasn't drained during
     * the above loop (because of partially sent big commands). */
    if (c->running_tid == IOTHREAD_MAIN_THREAD_ID)
        updateClientMemUsageAndBucket(c);

    return C_OK;
}

/* ======================================================================
 * ▶ [3-2] processMultibulkBuffer — networking.c L3366~L3583
 *   RESP 형식 명령 하나를 파싱합니다. "중간에 끊겨도 이어서 파싱할 수 있는" 상태 기계입니다.
 *
 *   RESP 예: SET key value
 *       *3\r\n          ← 인자가 3개 (multibulklen = 3)
 *       $3\r\nSET\r\n   ← 3바이트짜리 인자 "SET" (bulklen = 3)
 *       $3\r\nkey\r\n
 *       $5\r\nvalue\r\n
 *
 *   파서 상태 (client에 저장되므로 read가 여러 번에 나뉘어 와도 이어서 처리 가능):
 *     c->qb_pos       : querybuf에서 어디까지 읽었나
 *     c->multibulklen : 남은 인자 개수 (0 = 아직 *N 줄을 안 읽음)
 *     c->bulklen      : 지금 읽는 인자의 길이 (-1 = 아직 $N 줄을 안 읽음)
 *
 *   반환:
 *     C_OK  + INCOMPLETE 플래그 없음 → 명령 하나 완성 (pcmd->argv에 인자들)
 *     C_OK  + INCOMPLETE 플래그     → 일부만 파싱됨, 다음 read 후 이어서
 *     C_ERR + read_error 없음       → 줄바꿈(\r\n)조차 안 옴, 다음 read 기다림
 *     C_ERR + read_error 있음       → 프로토콜 위반
 *
 *   Kotlin 비유:
 *     if (multibulklen == 0) { multibulklen = readLine().drop(1).toInt() }      // "*3"
 *     while (multibulklen > 0) {
 *         if (bulklen == -1) { bulklen = readLine()?.drop(1)?.toInt() ?: break } // "$3"
 *         if (available() < bulklen + 2) break                                  // 데이터 덜 옴
 *         argv += readBytes(bulklen); skip(2); bulklen = -1; multibulklen--
 *     }
 * ====================================================================== */
static int processMultibulkBuffer(client *c, pendingCommand *pcmd) {
    char *newline = NULL;
    int ok;
    long long ll;
    size_t querybuf_len = sdslen(c->querybuf); /* Cache sdslen */

    /* ▶ ---- 1단계: "*3\r\n" 읽기 (인자 개수) ---- */
    if (c->multibulklen == 0) {
        /* The pending command should have been reset */
        serverAssertWithInfo(c,NULL,pcmd->argc == 0);

        /* Multi bulk length cannot be read without a \r\n */
        newline = strchr(c->querybuf+c->qb_pos,'\r');
        if (newline == NULL) {
            /* ▶ 줄바꿈 없이 64KB가 넘으면 비정상 → 에러 */
            if (querybuf_len-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                pcmd->read_error = CLIENT_READ_TOO_BIG_MBULK_COUNT_STRING;
                goto readerr;
            }
            return C_ERR;   /* ▶ 아직 덜 옴 */
        }

        /* Buffer should also contain \n */
        if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(querybuf_len-c->qb_pos-2))
            return C_ERR;

        /* We know for sure there is a whole line since newline != NULL,
         * so go ahead and find out the multi bulk length. */
        serverAssertWithInfo(c,NULL,c->querybuf[c->qb_pos] == '*');
        size_t multibulklen_slen = newline - (c->querybuf + 1 + c->qb_pos);
        /* ▶ '*' 다음부터 '\r' 전까지를 숫자로 변환 → ll */
        ok = string2ll(c->querybuf+1+c->qb_pos,newline-(c->querybuf+1+c->qb_pos),&ll);
        if (!ok || ll > INT_MAX) {
            pcmd->read_error = CLIENT_READ_INVALID_MULTIBUCK_LENGTH;
            goto readerr;
        } else if (ll > 10 && authRequired(c)) {
            /* ▶ 인증 전에는 인자 10개 넘는 명령 금지 (메모리 공격 방어) */
            pcmd->read_error = CLIENT_READ_UNAUTH_MBUCK_COUNT;
            goto readerr;
        }

        c->qb_pos = (newline-c->querybuf)+2;   /* ▶ "\r\n" 다음으로 이동 */

        if (ll <= 0) return C_OK;

        c->multibulklen = ll;
        c->bulklen = -1;

        /* Setup argv array on pending command structure.
         * Reallocate argv array when the requested size is greater than current size. */
        if (c->multibulklen > pcmd->argv_len) {
            zfree(pcmd->argv);
            pcmd->argv_len = min(c->multibulklen, 1024);
            pcmd->argv = zmalloc(sizeof(robj*)*(pcmd->argv_len));
            pcmd->argv_len_sum = 0;
        }

        /* ▶ [생략] 원본 L3419~L3449: 슬롯별 네트워크 입력 바이트 통계 계산 방법 설명 주석 */
        pcmd->input_bytes += (multibulklen_slen + 3);
    }

    serverAssertWithInfo(c,NULL,c->multibulklen > 0);
    /* ▶ ---- 2단계: 인자를 하나씩 읽기 ---- */
    while(c->multibulklen) {
        /* Read bulk length if unknown */
        /* ▶ 2-a: "$3\r\n" 읽기 (이번 인자의 길이) */
        if (c->bulklen == -1) {
            newline = memchr(c->querybuf+c->qb_pos,'\r',sdslen(c->querybuf) - c->qb_pos);
            if (newline == NULL) {
                if (querybuf_len-c->qb_pos > PROTO_INLINE_MAX_SIZE) {
                    pcmd->read_error = CLIENT_READ_TOO_BIG_BUCK_COUNT_STRING;
                    goto readerr;
                }
                break;
            }

            /* Buffer should also contain \n */
            if (newline-(c->querybuf+c->qb_pos) > (ssize_t)(querybuf_len-c->qb_pos-2))
                break;

            if (c->querybuf[c->qb_pos] != '$') {
                pcmd->read_error = CLIENT_READ_EXPECTED_DOLLAR;
                goto readerr;
            }

            size_t bulklen_slen = newline - (c->querybuf + c->qb_pos + 1);
            ok = string2ll(c->querybuf+c->qb_pos+1,newline-(c->querybuf+c->qb_pos+1),&ll);
            if (!ok || ll < 0 ||
                (!(c->flags & CLIENT_MASTER) && ll > server.proto_max_bulk_len)) {
                pcmd->read_error = CLIENT_READ_INVALID_BUCK_LENGTH;
                goto readerr;
            } else if (ll > 16384 && authRequired(c)) {
                /* ▶ 인증 전에는 16KB 넘는 인자 금지 */
                pcmd->read_error = CLIENT_READ_UNAUTH_BUCK_LENGTH;
                goto readerr;
            }

            c->qb_pos = newline-c->querybuf+2;
            /* ▶ 32KB 이상 큰 인자면: querybuf 앞부분을 잘라내서 "이 인자가 querybuf의 0번 위치에서
             *   시작하게" 맞춰둠. 그러면 아래 Optimization에서 복사 없이 querybuf를 통째로 넘길 수 있음.
             *   readQueryFromClient의 big_arg 처리와 짝을 이룹니다. */
            if (!(c->flags & CLIENT_MASTER) && ll >= PROTO_MBULK_BIG_ARG) {
                /* When the client is not a master client (because master
                 * client's querybuf can only be trimmed after data applied
                 * and sent to replicas).
                 *
                 * If we are going to read a large object from network
                 * try to make it likely that it will start at c->querybuf
                 * boundary so that we can optimize object creation
                 * avoiding a large copy of data.
                 *
                 * But only when the data we have not parsed is less than
                 * or equal to ll+2. If the data length is greater than
                 * ll+2, trimming querybuf is just a waste of time, because
                 * at this time the querybuf contains not only our bulk. */
                if (querybuf_len-c->qb_pos <= (size_t)ll+2) {
                    sdsrange(c->querybuf,c->qb_pos,-1);
                    querybuf_len = sdslen(c->querybuf);
                    c->qb_pos = 0;
                    /* Hint the sds library about the amount of bytes this string is
                     * going to contain. */
                    c->querybuf = sdsMakeRoomForNonGreedy(c->querybuf,ll+2-querybuf_len);
                    /* We later set the peak to the used portion of the buffer, but here we over
                     * allocated because we know what we need, make sure it'll not be shrunk before used. */
                    if (c->querybuf_peak < (size_t)ll + 2) c->querybuf_peak = ll + 2;
                    querybuf_len = sdslen(c->querybuf); /* Update cached length */
                }
            }
            c->bulklen = ll;
            /* Per-slot network bytes-in calculation, 2nd component. */
            pcmd->input_bytes += (bulklen_slen + 3);
        } else {
            serverAssert(pcmd->flags & PENDING_CMD_FLAG_INCOMPLETE);
        }

        /* Read bulk argument */
        /* ▶ 2-b: 인자 본문 읽기. 본문 + "\r\n"이 다 도착했는지 확인 */
        if (querybuf_len-c->qb_pos < (size_t)(c->bulklen+2)) {
            break;    /* ▶ 덜 옴 → INCOMPLETE로 끝내고 다음 read를 기다림 */
        } else {
            /* Check if we have space in argv, grow if needed */
            if (pcmd->argc >= pcmd->argv_len) {
                pcmd->argv_len = min(pcmd->argv_len < INT_MAX/2 ? (pcmd->argv_len)*2 : INT_MAX, pcmd->argc+c->multibulklen);
                pcmd->argv = zrealloc(pcmd->argv, sizeof(robj*)*(pcmd->argv_len));
            }

            /* Optimization: if a non-master client's buffer contains JUST our bulk element
             * instead of creating a new object by *copying* the sds we
             * just use the current sds string. */
            if (!(c->flags & CLIENT_MASTER) &&
                c->qb_pos == 0 &&
                c->bulklen >= PROTO_MBULK_BIG_ARG &&
                querybuf_len == (size_t)(c->bulklen+2))
            {
                /* ▶ 복사 없이 querybuf 자체를 인자 객체로 만들고, 클라이언트에게는 새 querybuf를 줌 */
                (pcmd->argv)[(pcmd->argc)++] = createObject(OBJ_STRING,c->querybuf);
                pcmd->argv_len_sum += c->bulklen;
                c->all_argv_len_sum += c->bulklen;
                sdsIncrLen(c->querybuf,-2); /* remove CRLF */
                /* Assume that if we saw a fat argument we'll see another one likely...
                 * But only if that fat argument is not too big compared to the memory limit. */
                if (!server.maxmemory || (size_t)c->bulklen < server.maxmemory / 32) {
                    c->querybuf = sdsnewlen(SDS_NOINIT,c->bulklen+2);
                } else {
                    c->querybuf = sdsnewlen(SDS_NOINIT, PROTO_IOBUF_LEN);
                }
                sdsclear(c->querybuf);
                querybuf_len = sdslen(c->querybuf); /* Update cached length */
            } else {
                /* ▶ 보통의 경우: querybuf의 해당 구간을 복사해서 문자열 객체 생성 */
                (pcmd->argv)[(pcmd->argc)++] =
                    createStringObject(c->querybuf+c->qb_pos,c->bulklen);
                pcmd->argv_len_sum += c->bulklen;
                c->all_argv_len_sum += c->bulklen;
                c->qb_pos += c->bulklen+2;
            }
            c->bulklen = -1;       /* ▶ 다음 인자는 다시 "$N"부터 */
            c->multibulklen--;     /* ▶ 남은 인자 하나 줄이기 */
        }
    }

    /* We're done when c->multibulk == 0 */
    if (c->multibulklen == 0) {
        /* Per-slot network bytes-in calculation, 3rd and 4th components. */
        pcmd->input_bytes += (pcmd->argv_len_sum + (pcmd->argc * 2));
        pcmd->flags &= ~PENDING_CMD_FLAG_INCOMPLETE;
        return C_OK;    /* ▶ 명령 하나 완성 */
    }

    /* Still not ready to process the command */
    pcmd->flags |= PENDING_CMD_FLAG_INCOMPLETE;
    return C_OK;

readerr:
    /* A command that hit a read error is finished, however many reads it was
     * parsed across, so it must not stay flagged as incomplete: otherwise
     * addPendingCommand() will not count it as ready and the error is never
     * reported to the client. */
    pcmd->flags &= ~PENDING_CMD_FLAG_INCOMPLETE;
    return C_ERR;
}


/* ######################################################################
 * [4] 실행
 * ###################################################################### */

/* ======================================================================
 * ▶ [4] processCommandAndResetClient — networking.c L3652~L3676
 *   server.c의 processCommand()를 부르는 얇은 래퍼입니다.
 *   processCommand가 하는 일 (다음 단계 server.c에서 자세히):
 *     명령 조회 → 인자 개수/권한/메모리/클러스터 검사 → call(c) → setCommand(c) 같은 명령 함수
 *     → 명령 함수 안에서 addReply(c, shared.ok) 로 응답 등록 ([5])
 *
 *   server.current_client: "지금 명령을 실행 중인 클라이언트". 실행 도중 이 클라이언트가
 *   해제되면 NULL이 됩니다. 그걸로 "client가 죽었는지" 판단합니다(deadclient).
 * ====================================================================== */
int processCommandAndResetClient(client *c) {
    int deadclient = 0;
    client *old_client = server.current_client;
    server.current_client = c;
    if (processCommand(c) == C_OK) {
        /* ▶ argv 해제, 파서 상태 초기화 등 다음 명령 준비. (BLPOP으로 블록됐으면 리셋 안 함) */
        commandProcessed(c);
        /* Update the client's memory to include output buffer growth following the
         * processed command. */
        if (c->conn) updateClientMemUsageAndBucket(c);
    }

    if (server.current_client == NULL) deadclient = 1;
    /*
     * Restore the old client, this is needed because when a script
     * times out, we will get into this code from processEventsWhileBlocked.
     * Which will cause to set the server.current_client. If not restored
     * we will return 1 to our caller which will falsely indicate the client
     * is dead and will stop reading from its buffer.
     */
    server.current_client = old_client;
    /* performEvictions may flush slave output buffers. This may
     * result in a slave, that may be the active client, to be
     * freed. */
    return deadclient ? C_ERR : C_OK;
}


/* ######################################################################
 * [5] 응답 쌓기 — addReply는 소켓에 쓰지 않는다!
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [5-1] addReply — networking.c L639~L654 ★
 *   명령 구현이 응답을 보낼 때 부르는 함수입니다. 예: addReply(c, shared.ok) → "+OK\r\n"
 *   addReplyBulk, addReplyLongLong, addReplyError 등 수십 개의 변형이 있지만 구조는 같습니다:
 *     1) _prepareClientToWrite(c) : 응답을 보내도 되는 클라이언트인가? + 쓰기 대기열 등록
 *     2) _addReplyToBufferOrList  : 응답 바이트를 클라이언트의 출력 버퍼에 복사
 *
 *   ★ 여기서 write() 시스템 콜은 일어나지 않습니다. 메모리 버퍼에 쌓기만 합니다. ★
 *   실제 전송은 이번 바퀴가 끝나고 잠들기 직전, beforeSleep에서 한꺼번에 합니다 ([6]).
 *   Kotlin 비유: response.append("+OK\r\n"); pendingWrites.add(client)
 * ====================================================================== */
void addReply(client *c, robj *obj) {
    if (_prepareClientToWrite(c) != C_OK) return;

    if (sdsEncodedObject(obj)) {
        _addReplyToBufferOrList(c,obj->ptr,sdslen(obj->ptr));
    } else if (obj->encoding == OBJ_ENCODING_INT) {
        /* For integer encoded strings we just convert it into a string
         * using our optimized function, and attach the resulting string
         * to the output buffer. */
        char buf[32];
        size_t len = ll2string(buf,sizeof(buf),(long)obj->ptr);
        _addReplyToBufferOrList(c,buf,len);
    } else {
        serverPanic("Wrong obj->encoding in addReply()");
    }
}

/* ======================================================================
 * ▶ [5-2] _prepareClientToWrite — networking.c L312~L344
 *   "이 클라이언트에게 응답을 보내도 되나?" + "보낼 게 생겼으니 대기열에 올려라"
 *   C_ERR이면 응답을 버립니다. 응답을 버리는 경우:
 *     - CLIENT REPLY OFF 상태
 *     - 이 서버가 레플리카이고 c가 마스터 (마스터에게는 응답하지 않음)
 *     - 소켓 없는 가짜 클라이언트 (AOF 로딩용)
 *     - 종료 예약된 클라이언트
 * ====================================================================== */
static inline int _prepareClientToWrite(client *c) {
    const uint64_t _flags = c->flags;
    /* If it's the Lua client we always return ok without installing any
     * handler since there is no socket at all. */
    if (unlikely(_flags & (CLIENT_SCRIPT|CLIENT_MODULE))) return C_OK;

    /* If CLIENT_CLOSE_ASAP flag is set, we need not write anything. */
    if (unlikely(_flags & CLIENT_CLOSE_ASAP)) return C_ERR;

    /* CLIENT REPLY OFF / SKIP handling: don't send replies.
     * CLIENT_PUSHING handling: disables the reply silencing flags. */
    if (unlikely((_flags & (CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP)) &&
        !(_flags & CLIENT_PUSHING))) return C_ERR;

    /* Masters don't receive replies, unless CLIENT_MASTER_FORCE_REPLY flag
     * is set. */
    if (unlikely((_flags & CLIENT_MASTER) &&
        !(_flags & CLIENT_MASTER_FORCE_REPLY))) return C_ERR;

    if (unlikely(!c->conn)) return C_ERR; /* Fake client for AOF loading. */

    /* Schedule the client to write the output buffers to the socket, unless
     * it should already be setup to do so (it has already pending data).
     *
     * If the client runs in an IO thread, we should not put the client in the
     * pending write queue. Instead, we will install the write handler to the
     * corresponding IO thread’s event loop and let it handle the reply. */
    /* ▶ 출력 버퍼가 비어 있었다 = 이번이 "첫" 응답 → 쓰기 대기열에 등록.
     *   이미 보낼 게 있었다면 이미 대기열에 있거나 쓰기 핸들러가 설치돼 있으니 할 일 없음. */
    if (likely(c->running_tid == IOTHREAD_MAIN_THREAD_ID) && !clientHasPendingReplies(c))
        putClientInPendingWriteQueue(c);

    /* Authorize the caller to queue in the output buffer of this client. */
    return C_OK;
}

/* ======================================================================
 * ▶ ★ [5-3] putClientInPendingWriteQueue — networking.c L292~L310 ★
 *   쓰기 이벤트(AE_WRITABLE)를 바로 등록하지 않고, 전역 리스트 server.clients_pending_write에
 *   이름만 올립니다. 영문 주석이 이유를 정확히 설명합니다:
 *
 *   쓰기 이벤트를 등록하면 → epoll_ctl 시스템 콜 1번 + 다음 바퀴 epoll_wait가 깨워줌 + 콜백 + write
 *   대기열에 올리면       → 잠들기 직전 beforeSleep에서 바로 write 1번
 *   대부분의 응답은 작아서 write 한 번에 다 나갑니다. 그러면 쓰기 이벤트는 등록할 필요조차 없습니다.
 * ====================================================================== */
void putClientInPendingWriteQueue(client *c) {
    /* Schedule the client to write the output buffers to the socket only
     * if not already done and, for slaves, if the slave can actually receive
     * writes at this stage. */
    if (!(c->flags & CLIENT_PENDING_WRITE) &&
        (c->replstate == REPL_STATE_NONE ||
         c->replstate == SLAVE_STATE_SEND_BULK_AND_STREAM ||
         (c->replstate == SLAVE_STATE_ONLINE && !c->repl_start_cmd_stream_on_ack)))
    {
        /* Here instead of installing the write handler, we just flag the
         * client and put it into a list of clients that have something
         * to write to the socket. This way before re-entering the event
         * loop, we can try to directly write to the client sockets avoiding
         * a system call. We'll only really install the write handler if
         * we'll not be able to write the whole reply at once. */
        c->flags |= CLIENT_PENDING_WRITE;   /* ▶ 중복 등록 방지 플래그 */
        listLinkNodeHead(server.clients_pending_write, &c->clients_pending_write_node);
    }
}

/* ======================================================================
 * ▶ [5-4] _addReplySegmentsToBufferOrList — networking.c L532~L576 (+ 래퍼 L579~L582)
 *   응답 바이트를 실제로 복사하는 곳. 출력 버퍼는 2단 구조입니다:
 *     c->buf   : 고정 16KB 배열 (bufpos까지 사용). 작은 응답은 여기서 끝
 *     c->reply : 16KB 블록들의 연결 리스트. buf가 꽉 차면 넘치는 부분이 여기로
 *
 *       c->buf  [+OK\r\n$5\r\nhello\r\n........ 16KB]   ← 먼저 채움
 *       c->reply → [블록 16KB] → [블록 16KB] → ...      ← 넘치면 여기에 이어 붙임
 *
 *   reply 리스트에 블록이 남아 있는 동안에는, 순서를 지키기 위해 그 뒤 응답도 리스트로 갑니다.
 *   리스트를 다 보내서 비우면 다시 buf를 씁니다.
 *   (_addReplyPayloadToBuffer의 "listLength(c->reply) > 0이면 0 반환" 부분)
 * ====================================================================== */
static inline __attribute__((always_inline))
void _addReplySegmentsToBufferOrList(client *c, const replySegment *seg, int nseg) {
    if (c->flags & CLIENT_CLOSE_AFTER_REPLY) return;

    /* Replicas should normally not cause any writes to the reply buffer. In case a rogue replica sent a command on the
     * replication link that caused a reply to be generated we'll simply disconnect it.
     * Note this is the simplest way to check a command added a response. Replication links are used to write data but
     * not for responses, so we should normally never get here on a replica client. */
    if (unlikely(clientTypeIsSlave(c))) {
        sds cmdname = c->lastcmd ? c->lastcmd->fullname : NULL;
        logInvalidUseAndFreeClientAsync(c, "Replica generated a reply to command '%s'",
                                        cmdname ? cmdname : "<unknown>");
        return;
    }

    for (int i = 0; i < nseg; i++) c->net_output_bytes_curr_cmd += seg[i].len;
    /* We call it here because this may affect the reply buffer offset (see the
     * reqres function comment); it is idempotent per request. */
    reqresSaveClientReplyOffset(c);

    /* If we're processing a push message into the current client (i.e. executing PUBLISH
     * to a channel which we are subscribed to, then we wanna postpone that message to be added
     * after the command's reply (specifically important during multi-exec). the exception is
     * the SUBSCRIBE command family, which (currently) have a push message instead of a proper reply.
     * The check for executing_client also avoids affecting push messages that are part of eviction.
     * Check CLIENT_PUSHING first to avoid race conditions, as it's absent in module's fake client. */
    if ((c->flags & CLIENT_PUSHING) && c == server.current_client &&
        server.executing_client && !cmdHasPushAsReply(server.executing_client->cmd))
    {
        for (int i = 0; i < nseg; i++)
            if (seg[i].len)
                _addReplyPayloadToList(c, server.pending_push_messages,
                                       seg[i].ptr, seg[i].len, PLAIN_REPLY);
        return;
    }

    /* ▶ 핵심 두 줄: 고정 버퍼에 들어가는 만큼 넣고(reply_len), 남은 건 리스트로 */
    for (int i = 0; i < nseg; i++) {
        const char *s = seg[i].ptr;
        size_t len = seg[i].len;
        if (!len) continue;
        size_t reply_len = _addReplyPayloadToBuffer(c, s, len, PLAIN_REPLY);
        if (len > reply_len)
            _addReplyPayloadToList(c, c->reply, s + reply_len, len - reply_len, PLAIN_REPLY);
    }
}

/* Append a single contiguous reply segment (thin wrapper over the batched form). */
void _addReplyToBufferOrList(client *c, const char *s, size_t len) {
    replySegment seg = { s, len };
    _addReplySegmentsToBufferOrList(c, &seg, 1);
}

/* ======================================================================
 * ▶ [5-5] _addReplyPayloadToBuffer — networking.c L467~L486
 *   고정 버퍼 c->buf에 들어가는 만큼 memcpy. 반환값 = 실제로 넣은 바이트 수.
 *   buf_encoded 분기는 unstable의 "큰 값 복사 회피" 최적화(BULK_STR_REF)용입니다.
 *   평범한 경우는 else 쪽 memcpy 두 줄입니다.
 * ====================================================================== */
static size_t _addReplyPayloadToBuffer(client *c, const void *payload, size_t len, uint8_t payload_type) {
    /* If there already are entries in the reply list, we cannot
     * add anything more to the static buffer. */
    if (listLength(c->reply) > 0) return 0;

    size_t available = c->buf_usable_size - c->bufpos;
    size_t reply_len = min(available, len);
    if (c->buf_encoded) {
        if (!tryAddPayload(c, c->buf, &c->bufpos, c->buf_usable_size, payload_type, payload, len))
            return 0;
        reply_len = len;
    } else {
        memcpy(c->buf + c->bufpos, payload, reply_len);   /* ▶ 버퍼 끝에 이어 붙이기 */
        c->bufpos+=reply_len;
    }

    /* We update the buffer peak after appending the reply to the buffer */
    if (c->buf_peak < (size_t)c->bufpos) c->buf_peak = (size_t)c->bufpos;
    return reply_len;
}

/* ▶ [5-6] _addReplyPayloadToList(L397~L453)는 생략했습니다.
 *   하는 일: reply 리스트의 마지막 블록에 여유가 있으면 거기에 채우고, 모자라면 16KB 이상짜리
 *   새 블록을 만들어 붙입니다. 그리고 closeClientOnOutputBufferLimitReached로
 *   출력 버퍼 한도(client-output-buffer-limit)를 넘었는지 검사해서, 넘었으면 종료 예약합니다.
 *   (응답을 안 읽어가는 느린 클라이언트가 서버 메모리를 다 먹는 것을 막는 장치) */


/* ######################################################################
 * [6] 응답 보내기
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [6-1] handleClientsWithPendingWrites — networking.c L3046~L3092 ★
 *   호출 위치: server.c beforeSleep() — 즉 ae.c aeProcessEvents의 ①단계, 잠들기 직전.
 *   (beforeSleep 안에서 AOF flush "다음"에 불립니다. 그래서 appendfsync always에서도
 *    응답이 디스크 기록보다 먼저 나가지 않습니다.)
 *
 *   대기열의 클라이언트마다:
 *     1) writeToClient(c, 0)로 바로 write 시도
 *     2) 그래도 남았으면 (소켓 송신 버퍼가 꽉 참) → 그제서야 쓰기 이벤트 등록
 *        → 다음 바퀴부터 소켓이 비면 epoll이 깨워주고 sendReplyToClient가 나머지를 보냄
 * ====================================================================== */
int handleClientsWithPendingWrites(void) {
    listIter li;
    listNode *ln;
    int processed = listLength(server.clients_pending_write);

    listRewind(server.clients_pending_write,&li);
    while((ln = listNext(&li))) {
        client *c = listNodeValue(ln);

        /* We handle IO thread replicas in putReplicasInPendingClientsToIOThreads */
        if (c->flags & CLIENT_SLAVE && c->tid != IOTHREAD_MAIN_THREAD_ID)
            continue;

        c->flags &= ~CLIENT_PENDING_WRITE;          /* ▶ 대기열에서 빠짐 */
        listUnlinkNode(server.clients_pending_write,ln);

        /* If a client is protected, don't do anything,
         * that may trigger write error or recreate handler. */
        if (c->flags & CLIENT_PROTECTED) continue;

        /* Don't write to clients that are going to be closed anyway. */
        if (c->flags & CLIENT_CLOSE_ASAP) continue;

        /* Let IO thread handle the client if possible. */
        /* ▶ I/O 스레드가 있으면 쓰기도 I/O 스레드에게 넘김 */
        if (server.io_threads_num > 1 &&
            !(c->flags & CLIENT_CLOSE_AFTER_REPLY) &&
            c->tid == IOTHREAD_MAIN_THREAD_ID &&
            !isClientMustHandledByMainThread(c))
        {
            assignClientToIOThread(c);
            continue;
        }

        /* Try to write buffers to the client socket. */
        /* ▶ 1) 바로 쓰기. 두 번째 인자 0 = "쓰기 핸들러 통해 온 게 아님" */
        if (writeToClient(c,0) == C_ERR) continue;

        /* If after the synchronous writes above we still have data to
         * output to the client, we need to install the writable handler. */
        /* ▶ 2) 다 못 보냈으면 그제서야 쓰기 이벤트 등록 */
        if (clientHasPendingReplies(c)) {
            installClientWriteHandler(c);
        }
    }
    /* Record the flushed offset so flushSlavesOutputBuffersIfNeeded() only
     * fires when a single event loop accumulates more than the threshold. */
    server.repl_last_flush_offset = server.master_repl_offset;
    return processed;
}

/* ======================================================================
 * ▶ [6-2] writeToClient — networking.c L2935~L3034
 *   출력 버퍼(buf + reply 리스트)를 소켓에 씁니다.
 *
 *   ★ 공정성: 한 번에 64KB(NET_MAX_WRITES_PER_EVENT)까지만 씁니다. ★
 *     KEYS * 처럼 응답이 수백 MB인 클라이언트 하나 때문에 다른 클라이언트들이 멈추면 안 되니까요.
 *     남은 건 쓰기 이벤트를 통해 다음 바퀴에 이어서 보냅니다.
 *     예외: maxmemory를 넘은 상태면 메모리를 빨리 비우려고 한도 없이 보냅니다.
 *
 *   다 보냈으면:
 *     - 쓰기 핸들러를 해제 (계속 켜두면 소켓은 거의 항상 쓰기 가능이라 epoll이 매 바퀴 깨움 → CPU 낭비)
 *     - CLIENT_CLOSE_AFTER_REPLY(QUIT 명령 등)면 이제 종료
 * ====================================================================== */
int writeToClient(client *c, int handler_installed) {
    if (!(c->io_flags & CLIENT_IO_WRITE_ENABLED)) return C_OK;
    /* Update the number of writes of io threads on server */
    atomicIncr(IOThreads[c->running_tid].io_writes_processed, 1);

    ssize_t nwritten = 0, totwritten = 0;
    const int is_slave = clientTypeIsSlave(c);

    if (unlikely(is_slave)) {
        /* ▶ 레플리카에게는 복제 스트림을 보내는 경로. 이벤트루프 공부 단계에서는 건너뛰세요. */
        /* We send as much as possible if the client is
         * a slave (otherwise, on high-speed traffic, the
         * replication buffer will grow indefinitely) */
        while(_clientHasPendingRepliesSlave(c)) {
            int ret = _writeToClientSlave(c, &nwritten);
            if (ret == C_ERR) break;
            totwritten += nwritten;
        }
        atomicIncr(server.stat_net_repl_output_bytes, totwritten);
    } else {
        /* If we reach this block and client is marked with CLIENT_SLAVE flag
         * it's because it's a MONITOR/slot-migration client, which are marked
         * as replicas, but exposed as normal clients */
        const int is_normal_client = !(c->flags & CLIENT_SLAVE);
        /* ▶ 보낼 게 남아 있는 동안 반복 */
        while (_clientHasPendingRepliesNonSlave(c)) {
            int ret = _writeToClientNonSlave(c, &nwritten);
            if (ret == C_ERR) break;    /* ▶ 소켓이 꽉 참(EAGAIN) 또는 에러 */
            totwritten += nwritten;
            /* Note that we avoid to send more than NET_MAX_WRITES_PER_EVENT
             * bytes, in a single threaded server it's a good idea to serve
             * other clients as well, even if a very large request comes from
             * super fast link that is always able to accept data (in real world
             * scenario think about 'KEYS *' against the loopback interface).
             *
             * However if we are over the maxmemory limit we ignore that and
             * just deliver as much data as it is possible to deliver.
             *
             * Moreover, we also send as much as possible if the client is
             * a slave (covered above) or a monitor (covered here).
             * (otherwise, on high-speed traffic, the
             * output buffer will grow indefinitely) */
            if (totwritten > NET_MAX_WRITES_PER_EVENT &&
                (server.maxmemory == 0 ||
                zmalloc_used_memory() < server.maxmemory) &&
                is_normal_client) break;
        }
        atomicIncr(server.stat_net_output_bytes, totwritten);
    }
    c->net_output_bytes += totwritten;

    /* ▶ write 에러 + 연결이 끊긴 상태면 종료 예약. (EAGAIN이면 연결은 멀쩡하므로 그냥 넘어감) */
    if (nwritten == -1) {
        if (connGetState(c->conn) != CONN_STATE_CONNECTED) {
            serverLog(LL_VERBOSE,
                "Error writing to client: %s", connGetLastError(c->conn));
            freeClientAsync(c);
            return C_ERR;
        }
    }
    if (totwritten > 0) {
        /* For clients representing masters we don't count sending data
         * as an interaction, since we always send REPLCONF ACK commands
         * that take some time to just fill the socket output buffer.
         * We just rely on data / pings received for timeout detection. */
        if (!(c->flags & CLIENT_MASTER)) c->lastinteraction = server.unixtime;
    }
    /* ▶ 다 보냈다! */
    if (!clientHasPendingReplies(c)) {
        c->sentlen = 0;
        /* Note that writeToClient() is called in a threaded way, but
         * aeDeleteFileEvent() is not thread safe: however writeToClient()
         * is always called with handler_installed set to 0 from threads
         * so we are fine. */
        /* ▶ 쓰기 이벤트를 통해 왔다면(=sendReplyToClient) 쓰기 이벤트 해제
         *   → connSetWriteHandler(NULL) → aeDeleteFileEvent(el, fd, AE_WRITABLE) */
        if (handler_installed) {
            /* IO Thread also can do that now. */
            connSetWriteHandler(c->conn, NULL);
        }

        /* Close connection after entire reply has been sent. */
        if (c->flags & CLIENT_CLOSE_AFTER_REPLY) {
            freeClientAsync(c);
            return C_ERR;
        }

        /* Remove client from pending referenced reply clients list. */
        if (c->running_tid == IOTHREAD_MAIN_THREAD_ID)
            tryUnlinkClientFromPendingRefReply(c, 1);

        /* If replica client has sent all the replication data it knows about
         * we send it to main thread so it can pick up new repl data ASAP.
         * Note, that we keep it in IO thread in case we have a pending ACK read. */
        if (c->flags & CLIENT_SLAVE && c->running_tid != IOTHREAD_MAIN_THREAD_ID) {
            if (!replicaFromIOThreadHasPendingRead(c))
                enqueuePendingClientsToMainThread(c, 0);
        }
    }
    /* Update client's memory usage after writing.
     * Since this isn't thread safe we do this conditionally. */
    if (c->running_tid == IOTHREAD_MAIN_THREAD_ID) {
        updateClientMemUsageAndBucket(c);
    }
    return C_OK;
}

/* ======================================================================
 * ▶ [6-3] _writeToClientNonSlave — networking.c L2814~L2845
 *   실제 write 시스템 콜. 두 갈래:
 *     reply 리스트가 있으면 → writev: buf + 리스트 블록 여러 개를 시스템 콜 "한 번"에 보냄
 *     buf만 있으면         → write: buf[sentlen..bufpos]를 보냄
 *   sentlen = "현재 블록에서 이미 보낸 바이트 수". 소켓이 일부만 받아주면(partial write)
 *   다음에 그 뒤부터 이어서 보내기 위해 기억해 둡니다.
 *   (_writevToClient L2687~L2807는 블록 관리가 복잡해서 생략)
 * ====================================================================== */
static inline int _writeToClientNonSlave(client *c, ssize_t *nwritten) {
    *nwritten = 0;
    /* When the reply list is not empty, it's better to use writev to save us some
     * system calls and TCP packets. */
    if (listLength(c->reply) > 0) {
        int ret = _writevToClient(c, nwritten);
        if (ret != C_OK) return ret;

        /* If there are no longer objects in the list, we expect
         * the count of reply bytes to be exactly zero. */
        if (listLength(c->reply) == 0)
            serverAssert(c->reply_bytes == 0 && c->reply_bytes_shared == 0);
    } else if (c->bufpos > 0) {
        /* For encoded buffers, we need to use writev to handle bulk string references */
        if (c->buf_encoded) {
            int ret = _writevToClient(c, nwritten);
            return ret;
        }

        /* ▶ ★ 소켓 write() ★ */
        *nwritten = connWrite(c->conn, c->buf + c->sentlen, c->bufpos - c->sentlen);
        if (*nwritten <= 0) return C_ERR;
        c->sentlen += *nwritten;

        /* If the buffer was sent, set bufpos to zero to continue with
         * the remainder of the reply. */
        if (c->sentlen == c->bufpos) {
            c->bufpos = 0;      /* ▶ 다 보냄 → 버퍼 비우기 (메모리는 재사용) */
            c->sentlen = 0;
        }
    }
    return C_OK;
}

/* ======================================================================
 * ▶ [6-4] installClientWriteHandler — networking.c L268~L283
 *   "한 번에 다 못 보냈을 때만" 불립니다. 쓰기 이벤트를 등록합니다.
 *     connSetWriteHandlerWithBarrier → (socket.c) aeCreateFileEvent(el, fd, AE_WRITABLE, ...)
 *
 *   barrier: appendfsync always면 켭니다. (02 흐름지도 6-2절, ae.c AE_BARRIER 해설 참고)
 *   켜지면 같은 바퀴에서 "읽기 → 쓰기" 순서가 "쓰기 → 읽기"로 뒤집혀서, 방금 읽은 명령의
 *   응답이 AOF fsync(다음 beforeSleep) 전에 나가는 일이 없어집니다.
 * ====================================================================== */
void installClientWriteHandler(client *c) {
    int ae_barrier = 0;
    /* For the fsync=always policy, we want that a given FD is never
     * served for reading and writing in the same event loop iteration,
     * so that in the middle of receiving the query, and serving it
     * to the client, we'll call beforeSleep() that will do the
     * actual fsync of AOF to disk. the write barrier ensures that. */
    if (server.aof_state == AOF_ON &&
        server.aof_fsync == AOF_FSYNC_ALWAYS)
    {
        ae_barrier = 1;
    }
    if (connSetWriteHandlerWithBarrier(c->conn, sendReplyToClient, ae_barrier) == C_ERR) {
        freeClientAsync(c);
    }
}

/* ======================================================================
 * ▶ [6-5] sendReplyToClient — networking.c L3037~L3040
 *   쓰기 이벤트 콜백. 소켓이 다시 쓰기 가능해지면 epoll이 깨우고, 여기가 불립니다.
 *   두 번째 인자 1 = "핸들러를 통해 왔음" → 다 보내면 writeToClient가 핸들러를 해제합니다.
 * ====================================================================== */
/* Write event handler. Just send data to the client. */
void sendReplyToClient(connection *conn) {
    client *c = connGetPrivateData(conn);
    writeToClient(c,1);
}


/* ######################################################################
 * [7] 종료 — 바로 지우지 않고 "예약"한다
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [7-1] freeClientAsync — networking.c L2473~L2492 ★
 *   ae.c의 타이머 삭제(id = -1 표시)와 같은 아이디어입니다.
 *   readQueryFromClient 깊은 곳에서 client를 바로 free하면, 호출 스택 위쪽 함수들이
 *   해제된 메모리를 계속 만지게 됩니다. 그래서 CLIENT_CLOSE_ASAP 플래그를 켜고
 *   server.clients_to_close 리스트에 넣어두기만 합니다.
 *
 *   실제 해제 시점:
 *     - 읽기 핸들러가 끝날 때: readQueryFromClient 끝의 beforeNextClient(c)
 *     - 잠들기 직전: beforeSleep → freeClientsInAsyncFreeQueue
 * ====================================================================== */
void freeClientAsync(client *c) {
    /* ▶ I/O 스레드가 맡은 클라이언트면 메인 스레드로 돌려보내면서 종료 표시 */
    if (c->running_tid != IOTHREAD_MAIN_THREAD_ID) {
        int main_thread = pthread_equal(pthread_self(), server.main_thread_id);
        /* Make sure the main thread can access IO thread data safely. */
        if (main_thread) pauseIOThread(c->tid);
        if (!(c->io_flags & CLIENT_IO_CLOSE_ASAP)) {
            c->io_flags |= CLIENT_IO_CLOSE_ASAP;
            enqueuePendingClientsToMainThread(c, 1);
        }
        if (main_thread) resumeIOThread(c->tid);
        return;
    }

    if (c->flags & CLIENT_CLOSE_ASAP || c->flags & CLIENT_SCRIPT) return;   /* ▶ 이미 예약됨 */
    c->flags |= CLIENT_CLOSE_ASAP;
    /* Replicas that was marked as CLIENT_CLOSE_ASAP should not keep the
     * replication backlog from been trimmed. */
    if (c->flags & CLIENT_SLAVE) freeReplicaReferencedReplBuffer(c);
    listAddNodeTail(server.clients_to_close,c);
}

/* ======================================================================
 * ▶ [7-2] beforeNextClient — networking.c L2516~L2536
 *   readQueryFromClient 맨 끝에서 불립니다. 이 클라이언트 처리가 다 끝났으니
 *   종료 예약돼 있으면 지금 해제해도 안전합니다. 다음 클라이언트로 넘어가기 전에 정리.
 * ====================================================================== */
int beforeNextClient(client *c) {
    /* Notice, this code is also called from 'processUnblockedClients'.
     * But in case of a module blocked client (see RM_Call 'K' flag) we do not reach this code path.
     * So whenever we change the code here we need to consider if we need this change on module
     * blocked client as well */

    /* Skip the client processing if we're in an IO thread, in that case we'll perform
       this operation later (this function is called again) in the fan-in stage of the threading mechanism */
    if (c && c->running_tid != IOTHREAD_MAIN_THREAD_ID)
        return C_OK;
    /* Handle async frees */
    /* Note: this doesn't make the server.clients_to_close list redundant because of
     * cases where we want an async free of a client other than myself. For example
     * in ACL modifications we disconnect clients authenticated to non-existent
     * users (see ACL LOAD). */
    if (c && (c->flags & CLIENT_CLOSE_ASAP)) {
        freeClient(c);
        return C_ERR;
    }
    return C_OK;
}

/* ======================================================================
 * ▶ [7-3] freeClientsInAsyncFreeQueue — networking.c L2540~L2557
 *   beforeSleep에서 불림. 예약된 클라이언트를 한꺼번에 해제.
 *   (예: ACL 변경으로 끊기는 클라이언트, 출력 버퍼 한도 초과, I/O 스레드에서 돌아온 종료 요청.
 *    CLIENT KILL은 다른 클라이언트를 freeClient()로 즉시 해제하고, 자기 자신이면 응답 후 종료로 표시)
 * ====================================================================== */
int freeClientsInAsyncFreeQueue(void) {
    int freed = 0;
    listIter li;
    listNode *ln;

    listRewind(server.clients_to_close,&li);
    while ((ln = listNext(&li)) != NULL) {
        client *c = listNodeValue(ln);

        if (c->flags & CLIENT_PROTECTED) continue;

        c->flags &= ~CLIENT_CLOSE_ASAP;
        freeClient(c);
        listDelNode(server.clients_to_close,ln);
        freed++;
    }
    return freed;
}

/* ▶ [7-4] freeClient(L2276~L2467)는 생략했습니다. 약 200줄이며 하는 일은:
 *   I/O 스레드에서 회수 → unlinkClient(전역 리스트에서 제거, connClose → aeDeleteFileEvent로
 *   읽기/쓰기 이벤트 해제 → close(fd)) → 버퍼/구독/트랜잭션 상태 해제 → zfree(c). */


/* ######################################################################
 * [8] 덤: 루프를 "안에서" 돌리기
 * ###################################################################### */

/* ======================================================================
 * ▶ [8] processEventsWhileBlocked — networking.c L5768~L5815 (앞부분 일부 생략)
 *   Redis는 스레드 하나라서 오래 걸리는 작업(서버 시작 시 RDB/AOF 로딩, 오래 도는 Lua 스크립트)
 *   중에는 aeMain으로 돌아갈 수 없습니다. 그동안 클라이언트가 완전히 무응답이 되지 않도록,
 *   작업 도중에 이 함수를 주기적으로 불러 이벤트루프를 "잠깐" 돌립니다.
 *     AE_FILE_EVENTS | AE_DONT_WAIT : 타이머는 안 돌리고, 잠들지도 않음
 *   그 덕에 로딩 중엔 "-LOADING", 스크립트 실행 중엔 "-BUSY" 같은 응답이라도 보낼 수 있습니다.
 *   최대 4바퀴(accept → read → write → close 한 사이클이 돌 정도) 돌립니다.
 * ====================================================================== */
void processEventsWhileBlocked(void) {
    int iterations = 4; /* See the function top-comment. */

    /* Update our cached time since it is used to create and update the last
     * interaction time with clients and for other important things. */
    updateCachedTime(0);

    /* ▶ [생략] 원본 L5775~L5786: 명령 시간 스냅샷, 전파 대상 저장 */

    /* Note: when we are processing events while blocked (for instance during
     * busy Lua scripts), we set a global flag. When such flag is set, we
     * avoid handling the read part of clients using threaded I/O.
     * See https://github.com/redis/redis/issues/6988 for more info.
     * Note that there could be cases of nested calls to this function,
     * specifically on a busy script during async_loading rdb, and scripts
     * that came from AOF. */
    ProcessingEventsWhileBlocked++;
    while (iterations--) {
        long long startval = server.events_processed_while_blocked;
        long long ae_events = aeProcessEvents(server.el,
            AE_FILE_EVENTS|AE_DONT_WAIT|
            AE_CALL_BEFORE_SLEEP|AE_CALL_AFTER_SLEEP);
        /* Note that server.events_processed_while_blocked will also get
         * incremented by callbacks called by the event loop handlers. */
        server.events_processed_while_blocked += ae_events;
        long long events = server.events_processed_while_blocked - startval;
        if (!events) break;    /* ▶ 처리할 게 없으면 바로 작업으로 복귀 */
    }

    whileBlockedCron();

    ProcessingEventsWhileBlocked--;
    serverAssert(ProcessingEventsWhileBlocked >= 0);

    /* ▶ [생략] 원본 L5813~L5814: 저장해둔 값 복원 */
}
