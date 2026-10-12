/* ======================================================================
 * [해설판] ae_epoll.c — ae.c가 OS(리눅스 커널)와 만나는 지점
 * 원본: https://github.com/redis/redis/blob/unstable/src/ae_epoll.c
 * 기준 커밋: b540ca4 (2026-09-30). 코드는 원본 그대로이고, 원본의 영문 주석은
 * 한국어로 옮겼습니다(저작권·라이선스 문구만 원문 유지). "▶"로 시작하는 주석은
 * 추가한 해설입니다.
 *
 * ▶ 이 파일은 단독으로 컴파일되지 않습니다. ae.c가 #include "ae_epoll.c"로
 *   통째로 붙여넣습니다. 그래서 여기 함수들이 전부 static(파일 private)이어도
 *   ae.c에서 부를 수 있습니다.
 *
 * ▶ ae.c가 기대하는 "인터페이스" (Kotlin으로 쓰면):
 *   interface PollApi {
 *       fun create(el): Int                    // aeApiCreate
 *       fun resize(el, setsize): Int           // aeApiResize
 *       fun free(el)                           // aeApiFree
 *       fun addEvent(el, fd, mask): Int        // aeApiAddEvent
 *       fun delEvent(el, fd, delmask)          // aeApiDelEvent
 *       fun poll(el, timeout): Int             // aeApiPoll  ★
 *       fun name(): String                     // aeApiName
 *   }
 *   ae_kqueue.c, ae_select.c도 똑같은 7개 함수를 각자 방식으로 구현합니다.
 *
 * ▶ epoll이란? (리눅스 커널 기능, 시스템 콜 3개)
 *   epoll_create()  : 감시 목록을 하나 만든다. 이 목록 자체도 fd로 돌려받음(epfd)
 *   epoll_ctl()     : 목록에 fd를 추가(ADD)/수정(MOD)/삭제(DEL)
 *   epoll_wait()    : 목록 중 준비된 fd가 생길 때까지 잠듦. 준비된 것만 골라서 돌려줌
 *
 *   select와의 차이: select는 매번 "전체 fd 목록"을 커널에 넘기고, 돌아온 결과를
 *   0..maxfd까지 전부 훑어야 합니다(O(전체 fd 수)). epoll은 목록을 커널이 기억하고
 *   "준비된 것만" 돌려줍니다(O(준비된 fd 수)). 연결 1만 개 중 10개만 활동하면 차이가 큽니다.
 *   JVM의 java.nio.channels.Selector도 리눅스에서는 내부적으로 epoll을 씁니다.
 * ====================================================================== */

/* 리눅스 epoll(2) 기반 ae.c 모듈
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */


#include <sys/epoll.h>

/* ▶ epoll 전용 상태. aeEventLoop->apidata(void*)에 담깁니다.
 *   epfd   : epoll_create가 돌려준 감시 목록의 fd
 *   events : epoll_wait가 결과를 써넣을 버퍼 (setsize 칸)
 *   ae.c는 이 구조체 내용을 모릅니다. 이 파일만 압니다. (캡슐화) */
typedef struct aeApiState {
    int epfd;
    struct epoll_event *events;
} aeApiState;

/* ▶ [create] 결과 버퍼를 setsize만큼 잡고 epoll 감시 목록을 만듦. */
static int aeApiCreate(aeEventLoop *eventLoop) {
    aeApiState *state = zmalloc(sizeof(aeApiState));

    if (!state) return -1;
    state->events = zmalloc(sizeof(struct epoll_event)*eventLoop->setsize);
    if (!state->events) {
        zfree(state);
        return -1;
    }
    state->epfd = epoll_create(1024); /* 1024는 커널에 주는 힌트일 뿐 */
    /* ▶ 1024는 옛 커널용 크기 힌트. 지금 커널은 무시합니다(0보다 크기만 하면 됨). */
    if (state->epfd == -1) {
        zfree(state->events);
        zfree(state);
        return -1;
    }
    /* ▶ close-on-exec 설정: 이 프로세스가 exec로 다른 프로그램을 실행할 때
     *   epfd가 그 프로그램에 딸려가지 않게 함. 이벤트루프 동작과는 무관한 위생 처리. */
    anetCloexec(state->epfd);
    eventLoop->apidata = state;
    return 0;
}

/* ▶ [resize] 결과 버퍼 크기만 바꿈. epoll 감시 목록 자체는 크기 제한이 없음. */
static int aeApiResize(aeEventLoop *eventLoop, int setsize) {
    aeApiState *state = eventLoop->apidata;

    state->events = zrealloc(state->events, sizeof(struct epoll_event)*setsize);
    return 0;
}

static void aeApiFree(aeEventLoop *eventLoop) {
    aeApiState *state = eventLoop->apidata;

    close(state->epfd);
    zfree(state->events);
    zfree(state);
}

/* ======================================================================
 * ▶ [addEvent] fd에 대한 관심사를 커널에 등록
 *   ae.c의 aeCreateFileEvent가 "등록부(events[fd].mask)를 바꾸기 전에" 부릅니다.
 *   그래서 여기서 읽는 events[fd].mask는 "기존" 관심사입니다.
 *
 *   epoll_ctl은 관심사를 "더하는" 게 아니라 "통째로 교체"합니다.
 *   그래서 기존 mask와 새 mask를 합쳐서(|=) 전체를 다시 넘깁니다.
 *     기존 NONE      + READABLE → ADD (EPOLLIN)
 *     기존 READABLE  + WRITABLE → MOD (EPOLLIN | EPOLLOUT)
 * ====================================================================== */
static int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* valgrind 경고 방지 */
    /* fd가 이미 어떤 이벤트로 감시되고 있으면 MOD 연산이 필요하다.
     * 그렇지 않으면 ADD 연산이 필요하다. */
    int op = eventLoop->events[fd].mask == AE_NONE ?
            EPOLL_CTL_ADD : EPOLL_CTL_MOD;

    ee.events = 0;
    mask |= eventLoop->events[fd].mask; /* 기존 이벤트와 합침 */
    /* ▶ ae의 마스크 → epoll의 마스크로 번역
     *   AE_READABLE → EPOLLIN, AE_WRITABLE → EPOLLOUT
     *   (AE_BARRIER는 epoll과 무관한 ae 내부 개념이라 번역하지 않음)
     *
     *   ★ EPOLLET(edge-triggered)를 넣지 않습니다 → level-triggered 모드.
     *     level-triggered: "읽을 데이터가 남아 있는 한" 매번 알려줌.
     *     edge-triggered : "새 데이터가 도착한 순간"에만 한 번 알려줌.
     *     Redis는 level-triggered라서 한 번에 다 못 읽어도(일부만 읽고 다른 클라이언트에게
     *     양보해도) 다음 바퀴에 다시 알림이 옵니다. 데이터를 놓칠 걱정이 없습니다. */
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    /* ▶ 이벤트가 발생하면 커널이 이 값을 그대로 돌려줍니다. "어느 fd였는지" 알기 위해 fd를 넣어둠.
     *   (data는 union이라 포인터를 넣을 수도 있지만, Redis는 fd만 넣고 events[fd]로 찾아감) */
    ee.data.fd = fd;
    if (epoll_ctl(state->epfd,op,fd,&ee) == -1) return -1;
    return 0;
}

/* ▶ [delEvent] 관심사 일부 또는 전부를 해제
 *   남는 관심사가 있으면 MOD(남은 것만으로 교체), 하나도 없으면 DEL(목록에서 제거). */
static void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask) {
    aeApiState *state = eventLoop->apidata;
    struct epoll_event ee = {0}; /* valgrind 경고 방지 */
    int mask = eventLoop->events[fd].mask & (~delmask);   /* ▶ 기존 - 삭제할 것 = 남는 것 */

    ee.events = 0;
    if (mask & AE_READABLE) ee.events |= EPOLLIN;
    if (mask & AE_WRITABLE) ee.events |= EPOLLOUT;
    ee.data.fd = fd;
    if (mask != AE_NONE) {
        epoll_ctl(state->epfd,EPOLL_CTL_MOD,fd,&ee);
    } else {
        /* 참고: 커널 2.6.9 미만은 EPOLL_CTL_DEL에도 NULL이 아닌
         * event 포인터가 필요하다. */
        epoll_ctl(state->epfd,EPOLL_CTL_DEL,fd,&ee);
    }
}

/* ======================================================================
 * ▶ ★ [poll] 잠들었다가, 준비된 fd 목록을 fired[]에 채워서 개수를 반환 ★
 *   ae.c aeProcessEvents의 ③단계에서 불립니다.
 *
 *   Kotlin 비유:
 *   fun poll(el: AeEventLoop, timeout: Duration?): Int {
 *       val ready = selector.select(timeout)        // ← 잠듦
 *       ready.forEachIndexed { j, key -> el.fired[j] = AeFiredEvent(key.fd, toAeMask(key)) }
 *       return ready.size
 *   }
 * ====================================================================== */
static int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp) {
    aeApiState *state = eventLoop->apidata;
    int retval, numevents = 0;

    /* ▶ epoll_wait(감시목록, 결과버퍼, 최대개수, timeout_ms)
     *   timeout 변환: tvp가 NULL이면 -1(무한 대기),
     *   아니면 초*1000 + 마이크로초를 ms로 "올림"((us + 999) / 1000).
     *   올림하는 이유: 내림하면 타이머 시각보다 살짝 일찍 깨어나서, 아직 실행할 타이머가
     *   없는데 또 0~1ms 자는 헛바퀴가 생길 수 있기 때문입니다.
     *
     *   ★ 이 호출 동안 스레드는 커널 안에서 잠들어 있습니다(CPU 0%).
     *     다음 중 하나가 일어나면 깨어납니다.
     *       - 감시 중인 fd 중 하나라도 준비됨
     *       - timeout 경과
     *       - 시그널 수신 (→ EINTR) */
    retval = epoll_wait(state->epfd,state->events,eventLoop->setsize,
            tvp ? (tvp->tv_sec*1000 + (tvp->tv_usec + 999)/1000) : -1);
    if (retval > 0) {
        int j;

        numevents = retval;
        /* ▶ 커널이 준 결과(epoll_event)를 ae 형식(aeFiredEvent)으로 번역해서 fired[]에 복사. */
        for (j = 0; j < numevents; j++) {
            int mask = 0;
            struct epoll_event *e = state->events+j;   /* ▶ = &state->events[j] */

            if (e->events & EPOLLIN) mask |= AE_READABLE;
            if (e->events & EPOLLOUT) mask |= AE_WRITABLE;
            /* ▶ 에러(EPOLLERR)나 상대방 끊김(EPOLLHUP)은 "읽기+쓰기 둘 다 가능"으로 바꿔서 전달.
             *   ae에는 "에러 콜백"이 따로 없습니다. 등록된 read/write 핸들러가 불리고,
             *   핸들러가 read()/write()를 시도하다 0바이트나 에러를 받아서 연결을 정리합니다. */
            if (e->events & EPOLLERR) mask |= AE_WRITABLE|AE_READABLE;
            if (e->events & EPOLLHUP) mask |= AE_WRITABLE|AE_READABLE;
            eventLoop->fired[j].fd = e->data.fd;      /* ▶ addEvent 때 넣어둔 fd를 되돌려받음 */
            eventLoop->fired[j].mask = mask;
        }
    } else if (retval == -1 && errno != EINTR) {
        /* ▶ EINTR(시그널에 의해 중단)은 정상 상황 → 이벤트 0개로 간주하고 다음 바퀴로.
         *   그 외 에러는 복구 불가로 보고 서버를 중단(panic). */
        panic("aeApiPoll: epoll_wait, %s", strerror(errno));
    }
    /* ▶ retval == 0 이면 timeout. numevents = 0 그대로 반환 → ae.c는 타이머만 처리. */

    return numevents;
}

static char *aeApiName(void) {
    return "epoll";
}
