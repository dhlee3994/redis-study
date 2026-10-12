/* ======================================================================
 * [해설판] server.c 발췌 — 이벤트루프를 "조립"하고 "운영"하는 쪽
 * 원본: https://github.com/redis/redis/blob/unstable/src/server.c (8615줄)
 * 기준 커밋: b540ca4 (2026-09-30)
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본 8615줄 중 이벤트루프와 직접 연결된 부분만
 *   골라 "서버 수명 순서대로" 다시 배치했습니다.
 *   - 각 함수 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다. 뺀 것 외에는 원본과 같은지 확인했습니다.
 *   - [6] blockedBeforeSleep은 blocked.c, [부록]은 server.h / iothread.c에서 가져왔습니다.
 *
 * ▶ 세 파일의 역할 분담 (지금까지 본 것 정리)
 *   ae.c         : 루프 엔진. "언제" 부를지 (epoll, 타이머, beforesleep 훅)
 *   networking.c : 클라이언트 I/O. 읽기/파싱/응답 버퍼/쓰기
 *   server.c     : 조립과 운영. 루프를 만들고, 콜백을 꽂고, 매 바퀴/매 주기 할 일을 정함
 *                  + 명령 실행의 관문(processCommand, call)
 *
 * ▶ 목차
 *   [1] main            : 초기화 순서, 마지막 줄 aeMain
 *   [2] initServer      : 루프 생성 + serverCron 타이머 + beforeSleep/afterSleep 등록
 *   [3] initListeners   : 리슨 소켓에 accept 핸들러 등록
 *   [4] serverCron      : 시간 이벤트. 기본 100ms마다 (hz=10)
 *       └ clientsCron, databasesCron
 *   [5] beforeSleep     : 잠들기 직전 (ae.c ①단계)  ★ 가장 중요 ★
 *   [6] blockedBeforeSleep (blocked.c)
 *   [7] afterSleep      : 깨어난 직후 (ae.c ④단계)
 *   [8] processCommand  : 명령 실행 전 검사 관문
 *   [9] call            : 명령 실행 + 통계 + AOF/복제 전파
 *   [부록] run_with_period 매크로(server.h), I/O 스레드 ↔ 메인 스레드 알림(iothread.c)
 *
 * ▶ 처음 읽을 때는 [2] [5] [4]만 보세요.
 * ====================================================================== */


/* ######################################################################
 * [1] main — 서버 시작부터 aeMain까지
 * ###################################################################### */

/* ======================================================================
 * ▶ [1] main — server.c L8251~L8613 (초기화 순서만 남기고 대부분 생략)
 *   순서가 중요합니다:
 *     initServerConfig → (설정 파일 읽기) → initServer → (모듈 로드) → initListeners
 *     → InitServerLast(I/O 스레드 생성) → loadDataFromDisk(RDB/AOF 로딩) → aeMain
 *
 *   ★ 리슨 소켓(initListeners)은 데이터 로딩(loadDataFromDisk) "전에" 열립니다. ★
 *     그래서 로딩 중에 접속한 클라이언트도 받아줄 수 있습니다.
 *     로딩 중에는 aeMain이 아직 안 돌지만, 로딩 코드가 주기적으로 processEventsWhileBlocked를
 *     불러서 루프를 잠깐씩 돌립니다(networking 발췌 [8]). 그때 "-LOADING" 에러로 응답합니다.
 * ====================================================================== */
int main(int argc, char **argv) {
    struct timeval tv;
    int j;
    char config_from_stdin = 0;

    /* ▶ [생략] 원본 L8255~L8334: 테스트 모드, 로케일, 랜덤 시드, 해시 시드 등 */
    initServerConfig();      /* ▶ 모든 설정을 기본값으로 */
    /* ▶ [생략] 원본 L8336~L8535: ACL 초기화, 명령줄 인자와 설정 파일 파싱, 데몬화 등 */

    initServer();            /* ▶ ★ [2] 이벤트루프 생성, 타이머/훅 등록 ★ */
    if (background || server.pidfile) createPidFile();
    if (server.set_proc_title) redisSetProcTitle(NULL);
    redisAsciiArt();
    checkTcpBacklogSettings();
    if (server.cluster_enabled) {
        /* clusterCommonInit() initializes slot-stats required by clusterInit() */
        clusterCommonInit();
        clusterInit();
    }
    if (!server.sentinel_mode) {
        moduleInitModulesSystemLast();
        moduleLoadInternalModules();
        moduleLoadFromQueue();
    }
    ACLLoadUsersAtStartup();
    initListeners();         /* ▶ ★ [3] 6379 포트 listen + accept 핸들러 등록 ★ */
    if (server.cluster_enabled) {
        clusterInitLast();
    }
    InitServerLast();        /* ▶ 백그라운드 스레드(bio), I/O 스레드 생성 */

    if (!server.sentinel_mode) {
        /* Things not needed when running in Sentinel mode. */
        serverLog(LL_NOTICE,"Server initialized");
        aofLoadManifestFromDisk();
        loadDataFromDisk();  /* ▶ RDB/AOF 로딩. 오래 걸릴 수 있음 */
        /* ▶ [생략] 원본 L8563~L8609: AOF 후처리, "Ready to accept connections" 로그, systemd 알림, CPU 친화도 */

    /* ▶ ★★★ 여기서부터 서버가 꺼질 때까지 이 한 줄 안에 있습니다 ★★★ */
    aeMain(server.el);
    aeDeleteEventLoop(server.el);
    return 0;
}


/* ######################################################################
 * [2] initServer — 이벤트루프 조립
 * ###################################################################### */

/* ======================================================================
 * ▶ [2] initServer — server.c L3024~L3274 (이벤트루프 관련 부분만)
 *   이벤트루프와 관련된 일은 네 가지입니다.
 *     ① aeCreateEventLoop(maxclients + 128)
 *     ② aeCreateTimeEvent(1ms, serverCron)       → 타이머 하나 등록
 *     ③ aeCreateFileEvent(module_pipe, ...)      → 모듈 스레드가 메인 루프를 깨우는 파이프
 *     ④ aeSetBeforeSleepProc / aeSetAfterSleepProc
 *   리슨 소켓 등록은 여기가 아니라 [3] initListeners에서 합니다.
 * ====================================================================== */
void initServer(void) {
    int j;

    /* ▶ SIGPIPE 무시: 끊긴 소켓에 write하면 OS가 SIGPIPE를 보내 프로세스를 죽이는데,
     *   그걸 막고 write가 에러(EPIPE)를 반환하게 함. 네트워크 서버의 기본 위생 처리. */
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    setupSignalHandlers();
    /* ▶ [생략] 원본 L3030~L3104: syslog, 각종 전역 리스트/상태 초기화 (server.clients, clients_pending_write 등) */
    createSharedObjects();   /* ▶ "+OK\r\n" 같은 자주 쓰는 응답 객체를 미리 만들어 둠 (shared.ok) */
    adjustOpenFilesLimit();  /* ▶ ulimit -n을 maxclients에 맞게 올리려 시도 (fd 개수 상한) */
    const char *clk_msg = monotonicInit(monotonicLogCallback);
    serverLog(LL_NOTICE, "monotonic clock: %s", clk_msg);
    /* ▶ ① ★ 이벤트루프 생성 ★ setsize = maxclients + CONFIG_FDSET_INCR(128) */
    server.el = aeCreateEventLoop(server.maxclients+CONFIG_FDSET_INCR);
    if (server.el == NULL) {
        serverLog(LL_WARNING,
            "Failed creating the event loop. Error message: '%s'",
            strerror(errno));
        exit(1);
    }
    /* ▶ [생략] 원본 L3116~L3224: DB 배열 생성, pub/sub, 통계, 복제 상태 등 초기화 */

    /* Create the timer callback, this is our way to process many background
     * operations incrementally, like clients timeout, eviction of unaccessed
     * expired keys and so forth. */
    /* ▶ ② ★ 메인 루프의 정기 타이머 (기본 설정에서는 이것 하나) ★
     *   1ms 뒤 첫 실행. 이후 serverCron이 `return 1000/server.hz;`로 다음 주기를 정함.
     *   (ae.c processTimeEvents의 "반복 타이머 원리" 참고) */
    if (aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL) == AE_ERR) {
        serverPanic("Can't create event loop timers.");
        exit(1);
    }

    /* Register a readable event for the pipe used to awake the event loop
     * from module threads. */
    /* ▶ ③ "자기 자신에게 보내는 알림" 패턴 (self-pipe trick)
     *   다른 스레드가 메인 스레드에게 "일어나서 이거 처리해"라고 하고 싶을 때,
     *   메인 스레드는 epoll_wait에서 자고 있으니 직접 깨울 방법이 없습니다.
     *   그래서 파이프를 하나 만들어 읽기 쪽을 ae에 등록해 두고, 다른 스레드가 쓰기 쪽에
     *   1바이트를 씁니다 → epoll이 "읽기 가능"으로 메인 스레드를 깨움.
     *   I/O 스레드 → 메인 스레드 알림도 같은 방식입니다([부록] iothread.c). */
    if (aeCreateFileEvent(server.el, server.module_pipe[0], AE_READABLE,
        modulePipeReadable,NULL) == AE_ERR) {
            serverPanic(
                "Error registering the readable event for the module pipe.");
    }

    /* Register before and after sleep handlers (note this needs to be done
     * before loading persistence since it is used by processEventsWhileBlocked. */
    /* ▶ ④ ★ 매 바퀴 잠들기 전/후 훅 등록 ★ ([5] [7]) */
    aeSetBeforeSleepProc(server.el,beforeSleep);
    aeSetAfterSleepProc(server.el,afterSleep);
    /* ▶ [생략] 원본 L3245~L3273: 32비트 maxmemory 제한, Lua/함수 엔진 초기화, 슬로우로그 등 */
}


/* ######################################################################
 * [3] initListeners — 리슨 소켓에 accept 핸들러 등록
 * ###################################################################### */

/* ======================================================================
 * ▶ [3-1] initListeners — server.c L3276~L3358 (등록 루프 부분만)
 *   TCP, TLS, Unix 소켓 리스너마다:
 *     connListen          : socket() → bind() → listen()  (6379 포트 열기)
 *     createSocketAcceptHandler : 리슨 fd를 ae에 READABLE로 등록
 *   connAcceptHandler(listener->ct)는 연결 종류별 accept 함수를 돌려줍니다.
 *   TCP면 socket.c의 connSocketAcceptHandler (networking 발췌 [1-1])
 * ====================================================================== */
void initListeners(void) {
    /* ▶ [생략] 원본 L3277~L3324: 설정값으로 TCP / TLS / Unix 소켓 리스너 구조체 채우기 */
    int listen_fds = 0;
    int unix_socket_created = 0;
    for (int j = 0; j < CONN_TYPE_MAX; j++) {
        listener = &server.listeners[j];
        if (listener->ct == NULL)
            continue;

        if (connListen(listener) == C_ERR) {
            serverLog(LL_WARNING, "Failed listening on port %u (%s), aborting.", listener->port, listener->ct->get_type(NULL));
            goto listener_error;
        }

        if (!strcasecmp(listener->ct->get_type(NULL), CONN_TYPE_UNIX) && listener->count > 0)
            unix_socket_created = 1;

        if (createSocketAcceptHandler(listener, connAcceptHandler(listener->ct)) != C_OK) {
            serverLog(LL_WARNING, "Failed creating %s listener accept handler, aborting.",
                      listener->ct->get_type(NULL));
            goto listener_error;
        }

        listen_fds += listener->count;
    }
    /* ▶ [생략] 원본 L3347~L3357: 하나도 못 열었으면 종료, 에러 처리 */
}

/* ======================================================================
 * ▶ [3-2] createSocketAcceptHandler — server.c L2847~L2860
 *   bind 주소가 여러 개면(예: bind 127.0.0.1 ::1) 리슨 fd도 여러 개입니다.
 *   전부 등록하거나, 하나라도 실패하면 전부 되돌립니다(rollback).
 * ====================================================================== */
/* Create an event handler for accepting new connections in TCP or TLS domain sockets.
 * This works atomically for all socket fds */
int createSocketAcceptHandler(connListener *sfd, aeFileProc *accept_handler) {
    int j;

    for (j = 0; j < sfd->count; j++) {
        if (aeCreateFileEvent(server.el, sfd->fd[j], AE_READABLE, accept_handler,sfd) == AE_ERR) {
            /* Rollback */
            for (j = j-1; j >= 0; j--) aeDeleteFileEvent(server.el, sfd->fd[j], AE_READABLE);
            return C_ERR;
        }
    }
    return C_OK;
}


/* ######################################################################
 * [4] serverCron — 시간 이벤트
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [4-1] serverCron — server.c L1556~L1873 (일부 생략) ★
 *   ae에 등록된 메인 스레드의 타이머(기본 설정에서는 유일. active defrag, 시간 예산을 넘긴
 *   eviction, 모듈 타이머가 켜지면 더 생김). 초당 server.hz번(기본 10번 = 100ms마다) 불립니다.
 *   "당장 급하지 않지만 주기적으로 해야 하는 일"을 전부 여기서 조금씩 합니다.
 *
 *   ★ 핵심 원칙: 한 번에 다 하지 않고 "조금씩" (incremental) ★
 *     만료 키 삭제, 해시 테이블 rehash, 클라이언트 점검 모두 "이번 호출엔 이만큼만" 합니다.
 *     serverCron이 오래 걸리면 그동안 모든 클라이언트가 기다리기 때문입니다.
 *
 *   ★ 주기 조절: run_with_period(ms) { ... } 매크로 ★
 *     serverCron 자체는 100ms마다 돌지만, 안에서 "이건 1초에 한 번만"을 표현할 때 씁니다.
 *     ([부록] 매크로 정의 참고)
 *
 *   하는 일 목록 (원본 주석의 리스트 + 코드 순서):
 *     - hz 조정 (dynamic-hz: 클라이언트가 많으면 hz를 올림)
 *     - 통계 샘플링 (100ms마다: instantaneous_ops_per_sec 등)
 *     - LRU 시계 갱신, 메모리 통계
 *     - SIGTERM 받았으면 안전하게 종료  ← 시그널 핸들러 안에서는 위험해서 여기서 처리
 *     - clientsCron()    : 클라이언트 timeout, 버퍼 크기 조정
 *     - databasesCron()  : 만료 키 삭제(active expire), rehash
 *     - BGSAVE / AOF rewrite 시작 조건 검사, 자식 프로세스 종료 확인
 *     - replicationCron() (1초마다), clusterCron() (100ms마다)
 *     - 반환: 1000/server.hz → 다음 실행까지 ms
 * ====================================================================== */
/* This is our timer interrupt, called server.hz times per second.
 * Here is where we do a number of things that need to be done asynchronously.
 * For instance:
 *
 * - Active expired keys collection (it is also performed in a lazy way on
 *   lookup).
 * - Software watchdog.
 * - Update some statistic.
 * - Incremental rehashing of the DBs hash tables.
 * - Triggering BGSAVE / AOF rewrite, and handling of terminated children.
 * - Clients timeout of different kinds.
 * - Replication reconnection.
 * - Many more...
 *
 * Everything directly called here will be called server.hz times per second,
 * so in order to throttle execution of things we want to do less frequently
 * a macro is used: run_with_period(milliseconds) { .... }
 */

int serverCron(struct aeEventLoop *eventLoop, long long id, void *clientData) {
    int j;
    UNUSED(eventLoop);
    UNUSED(id);
    UNUSED(clientData);

    /* Software watchdog: deliver the SIGALRM that will reach the signal
     * handler if we don't return here fast enough. */
    if (server.watchdog_period) watchdogScheduleSignal(server.watchdog_period);

    server.hz = server.config_hz;
    /* Adapt the server.hz value to the number of configured clients. If we have
     * many clients, we want to call serverCron() with an higher frequency. */
    /* ▶ dynamic-hz (기본 켜짐): 클라이언트 수 / hz > 200 이면 hz를 2배씩 (최대 500).
     *   예: 클라이언트 5000명 → hz 10이면 한 번에 500명을 점검해야 함 → hz를 40으로 올려 한 번에 125명.
     *   한 번의 serverCron이 너무 오래 걸리지 않게 "더 자주, 더 조금씩" 하려는 것. */
    if (server.dynamic_hz) {
        while (listLength(server.clients) / server.hz >
               MAX_CLIENTS_PER_CLOCK_TICK)
        {
            server.hz *= 2;
            if (server.hz > CONFIG_MAX_HZ) {
                server.hz = CONFIG_MAX_HZ;
                break;
            }
        }
    }

    /* for debug purposes: skip actual cron work if pause_cron is on */
    if (server.pause_cron) return 1000/server.hz;

    monotime cron_start = getMonotonicUs();

    run_with_period(100) {
        /* ▶ [생략] 원본 L1606~L1630: 초당 명령 수, 네트워크 입출력량 샘플링 (INFO stats의 instantaneous_* 값) */
    }

    /* We have just LRU_BITS bits per object for LRU information.
     * So we use an (eventually wrapping) LRU clock.
     *
     * Note that even if the counter wraps it's not a big problem,
     * everything will still work but some object will appear younger
     * to Redis. However for this to happen a given object should never be
     * touched for all the time needed to the counter to wrap, which is
     * not likely.
     *
     * Note that you can change the resolution altering the
     * LRU_CLOCK_RESOLUTION define. */
    server.lruclock = getLRUClock();

    cronUpdateMemoryStats();

    /* We received a SIGTERM or SIGINT, shutting down here in a safe way, as it is
     * not ok doing so inside the signal handler. */
    /* ▶ 시그널 핸들러는 "아무 때나" 끼어들어 실행됩니다(명령 실행 도중에도).
     *   그 안에서 RDB 저장 같은 큰 일을 하면 위험하므로, 핸들러는 플래그만 세우고
     *   실제 종료 절차는 여기 serverCron(=루프의 안전한 지점)에서 합니다.
     *   ae의 "삭제 표시만 하고 나중에 처리"와 같은 패턴입니다. */
    if (shouldShutdownAsap() && !isShutdownInitiated()) {
        int shutdownFlags = SHUTDOWN_NOFLAGS;
        int last_sig_received;
        atomicGet(server.last_sig_received, last_sig_received);
        if (last_sig_received == SIGINT && server.shutdown_on_sigint)
            shutdownFlags = server.shutdown_on_sigint;
        else if (last_sig_received == SIGTERM && server.shutdown_on_sigterm)
            shutdownFlags = server.shutdown_on_sigterm;

        if (prepareForShutdown(shutdownFlags) == C_OK) exit(0);
    } else if (isShutdownInitiated()) {
        if (server.mstime >= server.shutdown_mstime || isReadyToShutdown()) {
            if (finishShutdown() == C_OK) exit(0);
            /* Shutdown failed. Continue running. An error has been logged. */
        }
    }

    /* ▶ [생략] 원본 L1667~L1693: 5초마다 DB 키 개수, 접속 클라이언트 수 로그 */

    /* We need to do a few operations on clients asynchronously. */
    clientsCron();       /* ▶ [4-2] */

    /* Handle background operations on Redis databases. */
    databasesCron();     /* ▶ [4-3] */

    backupCron();

    /* Start a scheduled AOF rewrite if this was requested while another state
     * prevented it earlier. */
    if (!hasActiveChildProcess() &&
        server.aof_rewrite_scheduled &&
        !backupIsInProgress() &&
        !aofRewriteLimited())
    {
        rewriteAppendOnlyFileBackground();
    }

    /* Check if a background saving or AOF rewrite in progress terminated. */
    /* ▶ BGSAVE는 fork()로 만든 자식 프로세스가 합니다. 부모(이 루프)는 기다리지 않고
     *   serverCron마다 "끝났나?"만 확인합니다(checkChildrenDone → waitpid(WNOHANG)).
     *   기다리면 루프가 멈추니까요. */
    if (hasActiveChildProcess() || ldbPendingChildren())
    {
        run_with_period(1000) receiveChildInfo();
        checkChildrenDone();
    } else {
        /* If there is not a background saving/rewrite in progress check if
         * we have to save/rewrite now. */
        /* ▶ redis.conf의 "save 3600 1 300 100 60 10000" 조건 검사 → 만족하면 BGSAVE 시작 */
        for (j = 0; j < server.saveparamslen; j++) {
            struct saveparam *sp = server.saveparams+j;

            /* Save if we reached the given amount of changes,
             * the given amount of seconds, and if the latest bgsave was
             * successful or if, in case of an error, at least
             * CONFIG_BGSAVE_RETRY_DELAY seconds already elapsed. */
            if (server.dirty >= sp->changes &&
                server.unixtime-server.lastsave > sp->seconds &&
                (server.unixtime-server.lastbgsave_try >
                 CONFIG_BGSAVE_RETRY_DELAY ||
                 server.lastbgsave_status == C_OK))
            {
                serverLog(LL_NOTICE,"%d changes in %d seconds. Saving...",
                    sp->changes, (int)sp->seconds);
                rdbSaveInfo rsi, *rsiptr;
                rsiptr = rdbPopulateSaveInfo(&rsi);
                rdbSaveBackground(SLAVE_REQ_NONE,server.rdb_filename,rsiptr,RDBFLAGS_NONE);
                break;
            }
        }

        /* ▶ [생략] 원본 L1742~L1756: AOF 파일이 일정 비율 커지면 AOF rewrite 시작 */
    }
    /* ▶ [생략] 원본 L1758~L1781: dict 리사이즈 정책, 미뤄진 AOF flush 재시도 */

    /* Clear the paused actions state if needed. */
    updatePausedActions();

    /* Replication cron function -- used to reconnect to master,
     * detect transfer failures, start background RDB transfers and so forth.
     *
     * If Redis is trying to failover then run the replication cron faster so
     * progress on the handshake happens more quickly. */
    if (server.failover_state != NO_FAILOVER) {
        run_with_period(100) replicationCron();
    } else {
        run_with_period(1000) replicationCron();
    }

    /* Run the Redis Cluster cron. */
    run_with_period(100) {
        if (server.cluster_enabled) {
            clusterCron();
            asmCron();
        }
    }

    /* ▶ [생략] 원본 L1804~L1867: Sentinel 타이머, MIGRATE 소켓 정리, 각종 풀 정리, 예약된 BGSAVE, 모듈 cron */

    server.cronloops++;   /* ▶ 호출 횟수. run_with_period가 이 값으로 주기를 계산 */

    server.el_cron_duration = getMonotonicUs() - cron_start;

    /* ▶ ★ 다음 실행까지 ms. hz=10이면 100 → ae가 100ms 뒤로 재예약 ★ */
    return 1000/server.hz;
}

/* ======================================================================
 * ▶ [4-2] clientsCron — server.c L1244~L1306
 *   ★ "1초에 모든 클라이언트를 한 번씩" 점검하되, 한 번에 다 하지 않는다 ★
 *   한 번 호출에 클라이언트 수 / hz 명만 봅니다. hz=10이면 1/10씩 → 10번(1초)이면 한 바퀴.
 *   리스트 머리를 꺼내 처리하고 꼬리로 보내는(rotate) 방식이라 모든 클라이언트가 공평하게 돌아갑니다.
 *
 *   클라이언트마다 하는 일 (clientsCronRunClient):
 *     - timeout 설정(기본 0 = 끄기)보다 오래 조용하면 끊기 (블록된 클라이언트는 제외.
 *       BLPOP 등의 블록 timeout은 beforeSleep → blockedBeforeSleep → handleBlockedClientsTimeout이 담당.
 *       원본 영문 머리 주석의 설명은 예전 구조 기준입니다)
 *     - querybuf / 출력 버퍼가 쓸데없이 크면 줄이기 (메모리 회수)
 *     - 출력 버퍼 한도 검사
 * ====================================================================== */
/* This function is called by serverCron() and is used in order to perform
 * operations on clients that are important to perform constantly. For instance
 * we use this function in order to disconnect clients after a timeout, including
 * clients blocked in some blocking command with a non-zero timeout.
 *
 * The function makes some effort to process all the clients every second, even
 * if this cannot be strictly guaranteed, since serverCron() may be called with
 * an actual frequency lower than server.hz in case of latency events like slow
 * commands.
 *
 * It is very important for this function, and the functions it calls, to be
 * very fast: sometimes Redis has tens of hundreds of connected clients, and the
 * default server.hz value is 10, so sometimes here we need to process thousands
 * of clients per second, turning this function into a source of latency.
 */
void clientsCron(void) {
    /* Try to process at least numclients/server.hz of clients
     * per call. Since normally (if there are no big latency events) this
     * function is called server.hz times per second, in the average case we
     * process all the clients in 1 second. */
    int numclients = listLength(server.clients);
    int iterations = numclients/server.hz;

    /* Process at least a few clients while we are at it, even if we need
     * to process less than CLIENTS_CRON_MIN_ITERATIONS to meet our contract
     * of processing each client once per second. */
    if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
        iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                     numclients : CLIENTS_CRON_MIN_ITERATIONS;

    /* ▶ [생략] 원본 L1275~L1290: 최근 메모리 많이 쓴 클라이언트 추적용 슬롯 초기화 */

    while(listLength(server.clients) && iterations--) {
        client *c;
        listNode *head;

        /* Take the current head, process, and then rotate the head to tail.
         * This way we can fairly iterate all clients step by step. */
        head = listFirst(server.clients);
        c = listNodeValue(head);
        listRotateHeadToTail(server.clients);

        /* Clients handled by IO threads will be processed by IOThreadClientsCron. */
        /* ▶ I/O 스레드가 맡은 클라이언트는 그 스레드의 타이머(IOThreadCron)가 점검 */
        if (c->tid != IOTHREAD_MAIN_THREAD_ID) continue;

        clientsCronRunClient(c);
    }
}

/* ======================================================================
 * ▶ [4-3] databasesCron — server.c L1316~L1378
 *   DB 쪽 백그라운드 작업. 역시 전부 "조금씩"입니다.
 *     activeExpireCycle(SLOW) : 만료 시간 지난 키를 무작위 샘플링해서 삭제.
 *                               (키를 읽을 때도 만료 검사를 하지만(lazy), 아무도 안 읽는 키는
 *                                여기서 지워야 메모리가 회수됨)
 *     리사이즈 / rehash        : 해시 테이블 확장·축소를 한 번에 하지 않고 1ms(INCREMENTAL_REHASHING_THRESHOLD_US)씩
 *   fork된 자식(BGSAVE)이 있으면 rehash를 안 합니다. 메모리 페이지를 건드리면
 *   copy-on-write로 메모리 사용량이 불어나기 때문입니다.
 * ====================================================================== */
/* This function handles 'background' operations we are required to do
 * incrementally in Redis databases, such as active key expiring, resizing,
 * rehashing. */
void databasesCron(void) {
    /* Expire keys by random sampling. Not required for slaves
     * as master will synthesize DELs for us. */
    if (server.active_expire_enabled) {
        if (iAmMaster()) {
            activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW);
        } else {
            expireSlaveKeys();
        }
    }

    /* Defrag keys gradually. */
    activeDefragCycle();

    /* Handle active-trim */
    if (server.cluster_enabled)
        asmActiveTrimCycle();

    /* Perform hash tables rehashing if needed, but only if there are no
     * other processes saving the DB on disk. Otherwise rehashing is bad
     * as will cause a lot of copy-on-write of memory pages. */
    if (!hasActiveChildProcess()) {
        /* ▶ [생략] 원본 L1341~L1376: DB를 16개씩 돌아가며 리사이즈, 1ms 예산 안에서 점진적 rehash */
    }
}


/* ######################################################################
 * [5] beforeSleep — 잠들기 직전 (ae.c aeProcessEvents ①단계)
 * ###################################################################### */

/* ======================================================================
 * ▶ ★★★ [5] beforeSleep — server.c L1946~L2170 ★★★
 *   매 바퀴, epoll_wait로 잠들기 직전에 불립니다. 초당 수만 번 불릴 수 있으므로
 *   여기서 하는 일은 전부 "할 게 없으면 즉시 리턴"하도록 짜여 있습니다.
 *
 *   순서가 곧 설계입니다:
 *     (1) TLS 등 버퍼에 남은 데이터 처리 → 남았으면 dont_sleep
 *     (2) 블록된 클라이언트 처리 (BLPOP 깨우기, timeout)  ← 이게 데이터를 쓸 수 있으므로 (4) 전에
 *     (3) 빠른 만료 키 삭제 (activeExpireCycle FAST)
 *     (4) ★ AOF 버퍼를 디스크에 쓰기 (flushAppendOnlyFile) ★
 *     (5) I/O 스레드에서 돌아온 클라이언트 처리
 *     (6) ★ 응답 전송 (handleClientsWithPendingWrites) ★   ← 반드시 (4) 뒤
 *     (7) I/O 스레드에 일 넘기기
 *     (8) 종료 예약된 클라이언트 해제 (freeClientsInAsyncFreeQueue)
 *     (9) 메모리 많이 쓰는 클라이언트 강제 종료 (evictClients)
 *    (10) aeSetDontWait(dont_sleep) — 할 일이 남았으면 다음 바퀴에 잠들지 말 것
 *
 *   ★ (4)가 (6)보다 먼저인 이유 ★
 *     appendfsync always일 때 "클라이언트가 OK를 받았다면, 그 쓰기는 이미 AOF 파일에 fsync까지
 *     돼 있다"를 보장하기 위해서. (기본값 everysec에서는 백그라운드 fsync가 진행 중이면 AOF write
 *     자체를 최대 2초까지 미루므로 이 보장이 없습니다.)
 *     응답을 먼저 보내고 AOF를 나중에 쓰면, 그 사이에 서버가 죽었을 때 클라이언트는 성공으로
 *     알고 있는데 데이터는 없는 상황이 생깁니다.
 * ====================================================================== */
/* This function gets called every time Redis is entering the
 * main loop of the event driven library, that is, before to sleep
 * for ready file descriptors.
 *
 * Note: This function is (currently) called from two functions:
 * 1. aeMain - The main server loop
 * 2. processEventsWhileBlocked - Process clients during RDB/AOF load
 *
 * If it was called from processEventsWhileBlocked we don't want
 * to perform all actions (For example, we don't want to expire
 * keys), but we do need to perform some actions.
 *
 * The most important is freeClientsInAsyncFreeQueue but we also
 * call some other low-risk functions. */
void beforeSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);

    updatePeakMemory();

    /* Just call a subset of vital functions in case we are re-entering
     * the event loop from processEventsWhileBlocked(). Note that in this
     * case we keep track of the number of events we are processing, since
     * processEventsWhileBlocked() wants to stop ASAP if there are no longer
     * events to handle. */
    /* ▶ 로딩 중/긴 스크립트 중에 루프를 잠깐 돌리는 경우(processEventsWhileBlocked):
     *   만료 키 삭제 같은 건 하면 안 되고, 응답 전송과 클라이언트 해제 같은 "필수"만 함. */
    if (ProcessingEventsWhileBlocked) {
        uint64_t processed = 0;
        processed += connTypeProcessPendingData(server.el);
        processed += clientCompressionProcessPendingData(server.el);
        if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE)
            flushAppendOnlyFile(0);
        processed += handleClientsWithPendingWrites();
        processed += freeClientsInAsyncFreeQueue();

        /* Let the clients after the blocking call be processed. */
        processClientsOfAllIOThreads();
        /* New connections may have been established while blocked, clients from
         * IO thread may have replies to write, ensure they are promptly sent to
         * IO threads. */
        processed += sendPendingClientsToIOThreads();

        server.events_processed_while_blocked += processed;
        return;
    }

    /* Handle pending data(typical TLS). (must be done before flushAppendOnlyFile) */
    /* ▶ (1) TLS는 소켓에서 읽은 암호화 데이터를 복호화 라이브러리(OpenSSL) 내부 버퍼에 쌓아둘 수 있습니다.
     *   그 데이터는 소켓에는 이미 없으므로 epoll이 "읽기 가능"으로 알려주지 않습니다.
     *   그래서 여기서 직접 처리하고, 그래도 남았으면 dont_sleep으로 잠들지 않게 합니다. */
    connTypeProcessPendingData(server.el);
    /* Drain any buffered decompressed replication data (see client_comp.c). */
    clientCompressionProcessPendingData(server.el);

    /* If any connection type(typical TLS) or compression still has
     * pending unread data don't sleep at all. */
    int dont_sleep = connTypeHasPendingData(server.el) ||
                     clientCompressionHasPendingData(server.el);

    /* Call the Redis Cluster before sleep function. Note that this function
     * may change the state of Redis Cluster (from ok to fail or vice versa),
     * so it's a good idea to call it before serving the unblocked clients
     * later in this function, must be done before blockedBeforeSleep. */
    if (server.cluster_enabled) {
        clusterBeforeSleep();
        clusterCommonBeforeSleep();
    }

    /* Handle blocked clients.
     * must be done before flushAppendOnlyFile, in case of appendfsync=always,
     * since the unblocked clients may write data. */
    /* ▶ (2) [6] 참고. 예: BLPOP key 5의 timeout 만료, WAIT 응답, 모듈이 타이머에서 키를 준비시킨 경우.
     *   (LPUSH로 준비된 키는 보통 그 명령 직후 processCommand에서 바로 처리됨) */
    blockedBeforeSleep();

    /* Record cron time in beforeSleep, which is the sum of active-expire, active-defrag and all other
     * tasks done by cron and beforeSleep, but excluding read, write and AOF, that are counted by other
     * sets of metrics. */
    monotime cron_start_time_before_aof = getMonotonicUs();

    /* Run a fast expire cycle (the called function will return
     * ASAP if a fast cycle is not needed). */
    /* ▶ (3) 만료 키 "빠른" 삭제. serverCron의 SLOW 사이클(100ms마다)보다 짧게, 자주 돈다.
     *   만료될 키가 많이 쌓였다고 판단될 때만 실제로 일하고, 아니면 바로 리턴. */
    if (server.active_expire_enabled && iAmMaster())
        activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST);

    /* ▶ [생략] 원본 L2024~L2058: 모듈 이벤트, WAIT용 레플리카 ACK 요청, failover 상태,
     *   클라이언트 사이드 캐싱 무효화 메시지 */

    /* Record time consumption of AOF writing. */
    monotime aof_start_time = getMonotonicUs();
    /* Record cron time in beforeSleep. This does not include the time consumed by AOF writing and IO writing below. */
    monotime duration_before_aof = aof_start_time - cron_start_time_before_aof;
    /* Record the fsync'd offset before flushAppendOnly */
    long long prev_fsynced_reploff = server.fsynced_reploff;

    /* Write the AOF buffer on disk,
     * must be done before handleClientsWithPendingWrites and
     * sendPendingClientsToIOThreads, in case of appendfsync=always. */
    /* ▶ (4) ★ 이번 바퀴에 실행된 쓰기 명령들이 server.aof_buf에 쌓여 있음 → 파일에 write.
     *   appendfsync always면 여기서 fsync까지. everysec면 fsync는 백그라운드 스레드(bio)가 1초마다. */
    if (server.aof_state == AOF_ON || server.aof_state == AOF_WAIT_REWRITE)
        flushAppendOnlyFile(0);

    /* Record time consumption of AOF writing. */
    durationAddSample(EL_DURATION_TYPE_AOF, getMonotonicUs() - aof_start_time);

    /* Update the fsynced replica offset.
     * If an initial rewrite is in progress then not all data is guaranteed to have actually been
     * persisted to disk yet, so we cannot update the field. We will wait for the rewrite to complete. */
    if (server.aof_state == AOF_ON && server.fsynced_reploff != -1) {
        long long fsynced_reploff_pending;
        atomicGet(server.fsynced_reploff_pending, fsynced_reploff_pending);
        server.fsynced_reploff = fsynced_reploff_pending;

        /* If we have blocked [WAIT]AOF clients, and fsynced_reploff changed, we want to try to
         * wake them up ASAP. */
        if (listLength(server.clients_waiting_acks) && prev_fsynced_reploff != server.fsynced_reploff)
            dont_sleep = 1;
    }

    /* ▶ (5) I/O 스레드가 읽고 파싱한 클라이언트를 메인 스레드가 넘겨받아 명령 실행.
     *   (보통은 eventfd 알림으로 깨어나서 처리하지만, 알림 없이 넘어온 것도 여기서 마저 처리)
     *   이 블록은 unstable에서 추가됐습니다. 8.0 beforeSleep에는 없습니다. */
    if (server.io_threads_num > 1) {
        /* Corresponding to IOThreadBeforeSleep, process the clients from IO threads
         * without notification. */
        if (processClientsOfAllIOThreads() > 0) {
            /* If there are clients that are processed, it means IO thread is busy to
             * trafer clients to main thread, so the main thread does not sleep. */
            dont_sleep = 1;
        }
        if (!dont_sleep) {
            atomicSetWithSync(server.running, 0); /* Not running if going to sleep. */
            /* Try to process the clients from IO threads again, since before setting running
             * to 0, some clients may be transferred without notification. */
            processClientsOfAllIOThreads();
        }
    }

    /* ▶ [생략] 원본 L2105~L2115: 루프 통계 (입력 처리가 있었던 바퀴 수 세기) */

    /* Handle writes with pending output buffers. */
    /* ▶ (6) ★★ 응답 전송 ★★ networking 발췌 [6-1]
     *   이번 바퀴에 addReply로 쌓인 응답들이 여기서 실제로 소켓에 write됩니다. */
    handleClientsWithPendingWrites();

    /* Check if IO thread replicas have any pending read or writes and send them
     * back to their threads if so. */
    putReplicasInPendingClientsToIOThreads();

    /* Let io thread to handle its pending clients. */
    /* ▶ (7) 명령 실행이 끝난 클라이언트를 I/O 스레드에 돌려보냄 → I/O 스레드가 응답 write */
    sendPendingClientsToIOThreads();

    /* Record cron time in beforeSleep. This does not include the time consumed by AOF writing and IO writing above. */
    monotime cron_start_time_after_write = getMonotonicUs();

    /* Close clients that need to be closed asynchronous */
    /* ▶ (8) freeClientAsync로 예약된 클라이언트 실제 해제. networking 발췌 [7-3] */
    freeClientsInAsyncFreeQueue();

    /* Incrementally trim replication backlog, 10 times the normal speed is
     * to free replication backlog as much as possible. */
    if (server.repl_backlog)
        incrementalTrimReplicationBacklog(10*REPL_BACKLOG_TRIM_BLOCKS_PER_CALL);

    /* Disconnect some clients if they are consuming too much memory. */
    /* ▶ (9) maxmemory-clients 설정을 넘으면 메모리 많이 쓰는 클라이언트부터 끊기 */
    evictClients();

    /* ▶ [생략] 원본 L2140~L2158: 루프 지연 시간, 바퀴당 명령 수 통계 (INFO의 eventloop_* 값) */

    /* Don't sleep at all before the next beforeSleep() if needed (e.g. a
     * connection has pending data) */
    /* ▶ (10) ★ ae.c로 돌아가면 이 값을 보고 epoll_wait timeout을 0으로 할지 결정 ★
     *   (ae.c aeProcessEvents ②단계의 eventLoop->flags & AE_DONT_WAIT 검사) */
    aeSetDontWait(server.el, dont_sleep);

    /* Before we are going to sleep, let the threads access the dataset by
     * releasing the GIL. Redis main thread will not touch anything at this
     * time. */
    /* ▶ 모듈용 GIL(전역 락) 해제. 메인 스레드가 자는 동안만 모듈 스레드가 데이터에 접근 가능.
     *   afterSleep에서 다시 잡습니다. */
    if (moduleCount()) moduleReleaseGIL();
    /********************* WARNING ********************
     * Do NOT add anything below moduleReleaseGIL !!! *
     ***************************** ********************/
}


/* ######################################################################
 * [6] blockedBeforeSleep (blocked.c) — 블록된 클라이언트 처리
 * ###################################################################### */

/* ======================================================================
 * ▶ [6] blockedBeforeSleep — blocked.c L825~L853 (beforeSleep (2)에서 호출)
 *   BLPOP처럼 "데이터가 올 때까지 기다리는" 명령은 스레드를 멈추지 않습니다.
 *   client에 CLIENT_BLOCKED 플래그만 켜고, processInputBuffer가 그 클라이언트의 다음 명령을
 *   처리하지 않게 할 뿐입니다(networking 발췌 [3-1]의 첫 번째 break).
 *   키를 기다리는 대기자는 키를 바꾸는 명령(LPUSH 등)이 실행된 직후 processCommand에서 깨웁니다
 *   (ready_keys 방식, 이벤트 기반). 매 바퀴 여기서 확인하는 것은 timeout 만료, WAIT/WAITAOF,
 *   그 밖의 경로(모듈 등)로 준비된 키입니다.
 *
 *   Kotlin 비유: 코루틴의 suspend. 스레드를 막지 않고 "대기 목록"에 올려뒀다가 조건이 되면 resume.
 * ====================================================================== */
void blockedBeforeSleep(void) {
    /* Handle precise timeouts of blocked clients. */
    /* ▶ BLPOP key 5 → 5초 지나면 nil 응답하고 블록 해제 */
    handleBlockedClientsTimeout();

    /* Handle for expired pending entries. */
    handleClaimableStreamEntries();

    /* Unblock all the clients blocked for synchronous replication
     * in WAIT or WAITAOF. */
    if (listLength(server.clients_waiting_acks))
        processClientsWaitingReplicas();

    /* Try to process blocked clients every once in while.
     *
     * Example: A module calls RM_SignalKeyAsReady from within a timer callback
     * (So we don't visit processCommand() at all).
     *
     * This may unblock clients, so must be done before processUnblockedClients */
    /* ▶ 이번 바퀴에 LPUSH 등으로 "준비된" 키가 있으면 그 키를 기다리던 클라이언트를 깨움 */
    handleClientsBlockedOnKeys();

    /* Check if there are clients unblocked by modules that implement
     * blocking commands. */
    if (moduleCount())
        moduleHandleBlockedClients();

    /* Try to process pending commands for clients that were just unblocked. */
    /* ▶ 블록이 풀린 클라이언트의 querybuf에 쌓여 있던 다음 명령들을 이제 처리 */
    if (listLength(server.unblocked_clients))
        processUnblockedClients();
}


/* ######################################################################
 * [7] afterSleep — 깨어난 직후 (ae.c aeProcessEvents ④단계)
 * ###################################################################### */

/* ======================================================================
 * ▶ [7] afterSleep — server.c L2175~L2213
 *   epoll_wait에서 깨어나자마자, 이벤트 콜백들을 부르기 "전에" 불립니다.
 *     - 모듈 GIL 다시 잡기 (이제 메인 스레드가 데이터를 만질 거니까)
 *     - ★ 시간 캐시 갱신 (updateCachedTime) ★
 *
 *   시간 캐시: server.unixtime, server.mstime 같은 전역 변수에 현재 시각을 저장해 둡니다.
 *   명령 실행 중 객체에 접근할 때마다 time()을 부르는 대신 이 값을 읽으려는 것입니다.
 *   (networking.c의 c->lastinteraction = server.unixtime 등)
 *   단, 한 바퀴 동안 고정은 아닙니다. call()이 최상위 명령마다 enterExecutionUnit()으로 다시
 *   갱신합니다(하드웨어 시계면 10µs 경과 또는 명령 25개마다). [9] call의 생략 주석 참고.
 * ====================================================================== */
void afterSleep(struct aeEventLoop *eventLoop) {
    UNUSED(eventLoop);
    /********************* WARNING ********************
     * Do NOT add anything above moduleAcquireGIL !!! *
     ***************************** ********************/
    if (!ProcessingEventsWhileBlocked) {
        /* Acquire the modules GIL so that their threads won't touch anything. */
        if (moduleCount()) {
            mstime_t latency;
            latencyStartMonitor(latency);

            atomicSet(server.module_gil_acquiring, 1);
            moduleAcquireGIL();
            atomicSet(server.module_gil_acquiring, 0);
            moduleFireServerEvent(REDISMODULE_EVENT_EVENTLOOP,
                                  REDISMODULE_SUBEVENT_EVENTLOOP_AFTER_SLEEP,
                                  NULL);
            latencyEndMonitor(latency);
            latencyAddSampleIfNeeded("module-acquire-GIL",latency);
        }
        /* Set the eventloop start time. */
        server.el_start = getMonotonicUs();
        /* Set the eventloop command count at start. */
        server.el_cmd_cnt_start = server.stat_numcommands;
    }

    /* Set running after waking up */
    if (server.io_threads_num > 1) atomicSetWithSync(server.running, 1);

    /* Update the time cache. */
    updateCachedTime(1);

    /* Update command time snapshot in case it'll be required without a command
     * e.g. somehow used by module timers. Don't update it while yielding to a
     * blocked command, call() will handle that and restore the original time. */
    if (!ProcessingEventsWhileBlocked) {
        server.cmd_time_snapshot = server.mstime;
    }
}


/* ######################################################################
 * [8] processCommand — 명령 실행 관문
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [8] processCommand — server.c L4660~L5042 (검사 항목 상당수 생략) ★
 *   networking.c의 processCommandAndResetClient가 부릅니다.
 *   이름과 달리 대부분은 "실행해도 되는지 검사"이고, 실제 실행은 맨 끝 call() 한 줄입니다.
 *
 *   검사 순서 (거부되면 rejectCommand → addReply로 에러 응답 쌓고 C_OK 반환):
 *     명령 존재? → 인자 개수? → 인증? → ACL 권한? → 클러스터 슬롯 맞나(MOVED)?
 *     → 메모리 부족(maxmemory, 필요하면 eviction 실행)? → 디스크 쓰기 에러? → 레플리카 읽기전용?
 *     → 로딩 중? → 긴 스크립트 실행 중(BUSY)? → CLIENT PAUSE 중?
 *     → MULTI 안이면 큐에 넣기 / 아니면 ★ call() ★
 *
 *   반환값 의미:
 *     C_OK  : 명령을 처리함(실행됐든, 거부됐든) → 호출자가 commandProcessed()로 마무리
 *     C_ERR : 마무리 단계를 건너뜀 (예: HTTP 요청이 감지돼 비동기 종료를 예약한 경우)
 *   클라이언트가 해제됐는지는 반환값이 아니라 server.current_client == NULL로 판단합니다
 *   (networking.c processCommandAndResetClient).
 *
 *   ★ 이벤트루프 관점에서 중요한 점 ★
 *     여기서 블로킹 I/O는 일어나지 않습니다. 거부도, 실행도, 응답도 전부 메모리 작업입니다.
 *     예외적으로 기다려야 하는 경우(CLIENT PAUSE 등)에는 blockPostponeClient로
 *     클라이언트를 "블록 상태"로 바꾸고 바로 리턴합니다. 스레드는 절대 멈추지 않습니다.
 * ====================================================================== */
int processCommand(client *c) {
    /* ▶ [생략] 원본 L4661~L4688: 스크립트 상태 단언, 모듈 명령 필터, 모듈 yield 중이면 연기 */

    /* Now lookup the command and check ASAP about trivial error conditions
     * such as wrong arity, bad command name and so forth.
     * In case we are reprocessing a command after it was blocked,
     * we do not have to repeat the same checks */
    if (!client_reprocessing_command) {
        /* check if we can reuse the last command instead of looking up if we already have that info */
        /* ▶ lookedcmd: unstable의 lookahead 단계(networking preprocessCommand)에서 미리 찾아둔 명령 */
        struct redisCommand *cmd = c->lookedcmd;

        /* The command may have been modified by modules (e.g., in CommandFilters callbacks),
         * so we need to look it up again. */
        if (!cmd) {
            if (isCommandReusable(c->lastcmd, c->argv, c->argc))
                cmd = c->lastcmd;
            else
                cmd = lookupCommand(c->argv, c->argc);   /* ▶ 명령 테이블(dict)에서 "SET" 찾기 */
        }

        /* ▶ [생략] 원본 L4705~L4719: "POST", "Host:" 같은 HTTP 요청 감지(보안 공격 방어), 내부 명령 숨기기 */

        c->cmd = c->lastcmd = c->realcmd = cmd;
        sds err;
        if (!commandCheckExistence(c, &err)) {
            rejectCommandSds(c, err);     /* ▶ -ERR unknown command */
            return C_OK;
        }
        if (!commandCheckArity(c->cmd, c->argc, &err)) {
            rejectCommandSds(c, err);     /* ▶ -ERR wrong number of arguments */
            return C_OK;
        }

        /* ▶ [생략] 원본 L4730~L4745: DEBUG / MODULE 같은 보호 명령 허용 여부 */
    }

    /* ▶ [생략] 원본 L4748~L4765: 명령 플래그(읽기/쓰기/denyoom 등) 계산 */

    if (authRequired(c)) {
        /* AUTH and HELLO and no auth commands are valid even in
         * non-authenticated state. */
        if (!(c->cmd->flags & CMD_NO_AUTH)) {
            rejectCommand(c,shared.noautherr);   /* ▶ -NOAUTH */
            return C_OK;
        }
    }

    /* ▶ [생략] 원본 L4773~L4832: MULTI 안 금지 명령, ACL 권한 검사, 클러스터 리다이렉트(-MOVED), 슬롯 호환성 */

    /* Disconnect some clients if total clients memory is too high. We do this
     * before key eviction, after the last command was executed and consumed
     * some client output buffer memory. */
    evictClients();
    if (server.current_client == NULL) {
        /* If we evicted ourself then abort processing the command */
        return C_ERR;
    }

    /* Handle the maxmemory directive.
     *
     * Note that we do not want to reclaim memory if we are here re-entering
     * the event loop since there is a busy Lua script running in timeout
     * condition, to avoid mixing the propagation of scripts with the
     * propagation of DELs due to eviction. */
    /* ▶ maxmemory 초과면 eviction 정책(allkeys-lru 등)에 따라 키를 지워서 공간 확보.
     *   그래도 부족하고 쓰기 명령이면 -OOM 에러. eviction은 주로 "명령 실행 직전"에 동기적으로 일어나고,
     *   시간 예산을 넘기면 나머지는 evictionTimeProc 타이머로 다음 바퀴들에서 이어 갑니다. */
    if (server.maxmemory && !isInsideYieldingLongCommand()) {
        int out_of_memory = (performEvictions() == EVICT_FAIL);

        /* performEvictions may evict keys, so we need flush pending tracking
         * invalidation keys. If we don't do this, we may get an invalidation
         * message after we perform operation on the key, where in fact this
         * message belongs to the old value of the key before it gets evicted.*/
        trackingHandlePendingKeyInvalidations();

        /* performEvictions may flush slave output buffers. This may result
         * in a slave, that may be the active client, to be freed. */
        if (server.current_client == NULL) return C_ERR;

        if (out_of_memory && is_denyoom_command) {
            rejectCommand(c, shared.oomerr);
            return C_OK;
        }

        /* Save out_of_memory result at command start, otherwise if we check OOM
         * in the first write within script, memory used by lua stack and
         * arguments might interfere. We need to save it for EXEC and module
         * calls too, since these can call EVAL, but avoid saving it during an
         * interrupted / yielding busy script / module. */
        server.pre_command_oom_state = out_of_memory;
    }

    /* ▶ [생략] 원본 L4873~L4972: 디스크 쓰기 에러 시 쓰기 거부(-MISCONF), min-replicas 검사,
     *   읽기전용 레플리카(-READONLY), RESP2 구독 모드 제한, 마스터 연결 끊긴 레플리카 */

    /* Loading DB? Return an error if the command has not the
     * CMD_LOADING flag. */
    /* ▶ 로딩 중이면 -LOADING. processEventsWhileBlocked 덕분에 이 응답이라도 나갈 수 있습니다. */
    if (server.loading && !server.async_loading && is_denyloading_command) {
        rejectCommand(c, shared.loadingerr);
        return C_OK;
    }

    /* During async-loading, block certain commands. */
    if (server.async_loading && is_deny_async_loading_command) {
        rejectCommand(c,shared.loadingerr);
        return C_OK;
    }

    /* when a busy job is being done (script / module)
     * Only allow a limited number of commands.
     * Note that we need to allow the transactions commands, otherwise clients
     * sending a transaction with pipelining without error checking, may have
     * the MULTI plus a few initial commands refused, then the timeout
     * condition resolves, and the bottom-half of the transaction gets
     * executed, see Github PR #7022. */
    /* ▶ Lua 스크립트가 busy-reply-threshold(기본 5초)를 넘겨 실행 중이면, 그 사이에 들어온 명령은 -BUSY.
     *   (스크립트 실행 중에도 processEventsWhileBlocked가 루프를 돌려서 여기까지 올 수 있음)
     *   SCRIPT KILL, SHUTDOWN NOSAVE 같은 CMD_ALLOW_BUSY 명령만 통과 */
    if (isInsideYieldingLongCommand() && !(c->cmd->flags & CMD_ALLOW_BUSY)) {
        if (server.busy_module_yield_flags && server.busy_module_yield_reply) {
            rejectCommandFormat(c, "-BUSY %s", server.busy_module_yield_reply);
        } else if (server.busy_module_yield_flags) {
            rejectCommand(c, shared.slowmoduleerr);
        } else if (scriptIsEval()) {
            rejectCommand(c, shared.slowevalerr);
        } else {
            rejectCommand(c, shared.slowscripterr);
        }
        return C_OK;
    }

    /* Prevent a replica from sending commands that access the keyspace.
     * The main objective here is to prevent abuse of client pause check
     * from which replicas are exempt. */
    if ((c->flags & CLIENT_SLAVE) && (is_may_replicate_command || is_write_command || is_read_command)) {
        rejectCommandFormat(c, "Replica can't interact with the keyspace");
        return C_OK;
    }

    /* If the server is paused, block the client until
     * the pause has ended. Replicas are never paused. */
    /* ▶ CLIENT PAUSE 중이면 스레드를 멈추는 대신 클라이언트를 블록 상태로 → 바로 리턴.
     *   pause가 풀리면 beforeSleep → blockedBeforeSleep에서 이어서 처리됩니다. */
    if (!(c->flags & CLIENT_SLAVE) &&
        ((isPausedActions(PAUSE_ACTION_CLIENT_ALL)) ||
        ((isPausedActions(PAUSE_ACTION_CLIENT_WRITE)) && is_may_replicate_command)))
    {
        blockPostponeClient(c);
        return C_OK;
    }

    /* Exec the command */
    /* ▶ MULTI ~ EXEC 사이면 실행하지 않고 큐에 쌓고 +QUEUED 응답 */
    if (c->flags & CLIENT_MULTI &&
        c->cmd->proc != execCommand &&
        c->cmd->proc != discardCommand &&
        c->cmd->proc != multiCommand &&
        c->cmd->proc != watchCommand &&
        c->cmd->proc != quitCommand &&
        c->cmd->proc != resetCommand)
    {
        queueMultiCommand(c, cmd_flags);
        addReply(c,shared.queued);
    } else {
        int flags = CMD_CALL_FULL;
        call(c,flags);      /* ▶ ★ 실행 ([9]) ★ */
        /* ▶ 방금 명령이 LPUSH 등으로 어떤 키를 "준비" 상태로 만들었으면, 그 키를 기다리던
         *   BLPOP 클라이언트를 바로 깨움 (beforeSleep까지 안 기다림) */
        if (listLength(server.ready_keys) && !isInsideYieldingLongCommand())
            handleClientsBlockedOnKeys();
    }
    return C_OK;
}


/* ######################################################################
 * [9] call — 명령 실행
 * ###################################################################### */

/* ======================================================================
 * ▶ [9] call — server.c L4151~L4448 (상당 부분 생략)
 *   핵심은 단 한 줄입니다:
 *       c->cmd->proc(c);
 *   cmd->proc는 함수 포인터. SET이면 setCommand(t_string.c), GET이면 getCommand.
 *   Kotlin: commandTable["set"].proc.invoke(c)
 *
 *   그 앞뒤로:
 *     앞: 시간 측정 시작, server.dirty(변경 횟수) 기록
 *     뒤: 실행 시간 측정 → 슬로우로그, latency 모니터, INFO commandstats
 *         dirty가 늘었으면(=데이터가 바뀌었으면) AOF / 레플리카로 전파할 목록에 추가(alsoPropagate)
 *         → 실제 AOF 버퍼 기록은 afterCommand → propagatePendingCommands에서,
 *           디스크 write는 beforeSleep의 flushAppendOnlyFile에서
 *
 *   ★ 슬로우로그에 찍히는 시간 = 이 proc(c) 실행 시간 ★
 *     네트워크 왕복, 대기열 대기 시간은 포함되지 않습니다. 여기가 오래 걸리면
 *     루프 전체가 그만큼 멈추므로, 모든 클라이언트의 지연이 늘어납니다.
 * ====================================================================== */
void call(client *c, int flags) {
    long long dirty;
    uint64_t client_old_flags = c->flags;
    struct redisCommand *real_cmd = c->realcmd;
    client *prev_client = server.executing_client;
    server.executing_client = c;

    /* ▶ [생략] 원본 L4157~L4178: 통계 갱신 여부, 재실행 여부 판단, 전파 플래그 초기화 */

    /* Call the command. */
    dirty = server.dirty;     /* ▶ 실행 전 변경 횟수 기록 → 실행 후 차이로 "데이터가 바뀌었나" 판단 */
    long long old_master_repl_offset = server.master_repl_offset;
    incrCommandStatsOnError(NULL, 0);

    /* ▶ [생략] 원본 L4183~L4214: 단조 시계로 시작 시각 측정, 시간 캐시 갱신 */

    /* setting the CLIENT_EXECUTING_COMMAND flag so we will avoid
     * sending client side caching message in the middle of a command reply.
     * In case of blocking commands, the flag will be un-set only after successfully
     * re-processing and unblock the client.*/
    c->flags |= CLIENT_EXECUTING_COMMAND;

    /* ▶ [생략] 원본 L4216~L4234: 중첩 호출의 전파 대상 제한 */

    /* ▶ ★★★ 실제 명령 실행 ★★★  예: setCommand(c) → setKey(...) → addReply(c, shared.ok) */
    c->cmd->proc(c);

    /* ▶ [생략] 원본 L4236~L4291: 실행 시간 계산(duration), dirty 차이 계산, 실패 통계 */

    /* Log the command into the Slow log if needed.
     * If the client is blocked we will handle slowlog when it is unblocked. */
    if (update_command_stats && !(c->flags & CLIENT_BLOCKED))
        slowlogPushCurrentCommand(c, real_cmd, c->duration);

    /* ▶ [생략] 원본 L4296~L4342: MONITOR 클라이언트에게 명령 복사, commandstats, hotkey 통계 */

    /* Propagate the command into the AOF and replication link.
     * We never propagate EXEC explicitly, it will be implicitly
     * propagated if needed (see propagatePendingCommands).
     * Also, module commands take care of themselves */
    if (flags & CMD_CALL_PROPAGATE &&
        (c->flags & CLIENT_PREVENT_PROP) != CLIENT_PREVENT_PROP &&
        c->cmd->proc != execCommand &&
        !(c->cmd->flags & CMD_MODULE))
    {
        int propagate_flags = PROPAGATE_NONE;

        /* Check if the command operated changes in the data set. If so
         * set for replication / AOF propagation. */
        /* ▶ GET처럼 아무것도 안 바꾼 명령은 dirty == 0 → 전파 안 함 */
        if (dirty) propagate_flags |= (PROPAGATE_AOF|PROPAGATE_REPL);

        /* ▶ [생략] 원본 L4357~L4369: 강제 전파 / 전파 금지 플래그 반영 */

        /* Call alsoPropagate() only if at least one of AOF / replication
         * propagation is needed. */
        if (propagate_flags != PROPAGATE_NONE)
            alsoPropagateEx(c->db->id,c->argv,c->argc,propagate_flags,duration);
    }

    /* ▶ [생략] 원본 L4375~L4398: 전파 플래그 복원, 클라이언트 사이드 캐싱 키 추적 */

    if (!(c->flags & CLIENT_BLOCKED)) {
        /* Modules may call commands in cron, in which case server.current_client
         * is not set. */
        if (server.current_client) {
            server.current_client->commands_processed++;
        }
        server.stat_numcommands++;   /* ▶ INFO의 total_commands_processed */
    }

    /* Do some maintenance job and cleanup.
     * Use total_duration (not stack duration) so blocked-client reprocessing
     * still credits accumulated time when pending UNKNOWN ops are drained. */
    /* ▶ 쌓아둔 전파 목록을 AOF 버퍼(server.aof_buf)와 레플리카 버퍼에 실제로 기록 */
    afterCommandEx(c, total_duration, ops_before);

    /* ▶ [생략] 원본 L4412~L4447: 네트워크 바이트 통계, 실행 중 클라이언트 복원 */
}


/* ######################################################################
 * [부록]
 * ###################################################################### */

/* ======================================================================
 * ▶ [부록 1] run_with_period 매크로 — server.h L856, L861
 *   serverCron 안에서 "ms 밀리초마다 한 번만" 실행하게 하는 if문입니다.
 *
 *     run_with_period(1000) { ... }
 *   → if ((1000 <= 1000/hz) || !(cronloops % (1000/(1000/hz)))) { ... }
 *   → hz=10이면 1000/hz = 100ms. 1000 <= 100은 거짓이므로
 *     cronloops % (1000/100) = cronloops % 10 == 0 일 때만 실행 → serverCron 10번에 1번 = 1초마다
 *
 *   주기가 serverCron 주기보다 짧거나 같으면(예: run_with_period(100), hz=10) 매번 실행됩니다.
 *   Kotlin: if (period <= tickMs || cronloops % (period / tickMs) == 0L) { ... }
 * ====================================================================== */
#define _run_with_period(_cronloops_, _ms_, _hz_) if (((_ms_) <= 1000/(_hz_)) || !((_cronloops_)%((_ms_)/(1000/(_hz_)))))

/* Using the following macro you can run code inside serverCron() with the
 * specified period, specified in milliseconds.
 * The actual resolution depends on server.hz. */
#define run_with_period(_ms_) _run_with_period(server.cronloops, (_ms_), server.hz)

/* ======================================================================
 * ▶ [부록 2] I/O 스레드 ↔ 메인 스레드 알림 — iothread.c initThreadedIO 안 (L1025~L1056 일부)
 *   스레드가 여러 개면 "다른 스레드를 어떻게 깨우나?"가 문제입니다. 각 스레드는 자기
 *   epoll_wait에서 자고 있으니까요. 해답은 [2] module_pipe와 같은 패턴입니다:
 *     알림용 fd(eventfd 또는 pipe)를 만들어 받는 쪽 스레드의 aeEventLoop에 READABLE로 등록.
 *     보내는 쪽은 그 fd에 쓰기만 하면, 받는 쪽 epoll_wait가 깨어납니다.
 *
 *   I/O 스레드마다 두 개의 알림 채널:
 *     메인 → I/O : t->pending_clients_notifier            → I/O 스레드의 t->el에 등록
 *     I/O → 메인 : mainThreadPendingClientsNotifiers[i]  → 메인의 server.el에 등록
 *   그리고 I/O 스레드도 자기 타이머(IOThreadCron)를 가집니다. 메인의 serverCron 축소판입니다.
 *
 *   즉 io-threads=4면 aeEventLoop가 4개(메인 1 + I/O 3) 돌아갑니다. io-threads는 메인을 포함한
 *   숫자라서 위 for문이 i = 1부터 돕니다. 모두 같은 ae.c 코드입니다.
 * ====================================================================== */
        t->pending_clients_notifier = createEventNotifier();
        if (aeCreateFileEvent(t->el, getReadEventFd(t->pending_clients_notifier),
                              AE_READABLE, handleClientsFromMainThread, t) != AE_OK)
        {
            serverLog(LL_WARNING, "Fatal: Can't register file event for IO thread notifications.");
            exit(1);
        }

        /* This is the timer callback of the IO thread, used to gradually handle
         * some background operations, such as clients cron. */
        if (aeCreateTimeEvent(t->el, 1, IOThreadCron, t, NULL) == AE_ERR) {
            serverLog(LL_WARNING, "Fatal: Can't create event loop timers in IO thread.");
            exit(1);
        }

        /* ▶ [생략] 원본 L1039~L1050: IO 스레드 생성(pthread_create), 메인 스레드 쪽 리스트/뮤텍스 준비 */
        mainThreadPendingClientsNotifiers[i] = createEventNotifier();
        if (aeCreateFileEvent(server.el, getReadEventFd(mainThreadPendingClientsNotifiers[i]),
                              AE_READABLE, handleClientsFromIOThread, t) != AE_OK)
        {
            serverLog(LL_WARNING, "Fatal: Can't register file event for main thread notifications.");
            exit(1);
        }
