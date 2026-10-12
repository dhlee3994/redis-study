/* ======================================================================
 * [해설판] ae.c — Redis 이벤트루프 구현
 * 원본: https://github.com/redis/redis/blob/unstable/src/ae.c
 * 기준 커밋: b540ca4 (2026-09-30). 원본의 영문 주석은 한국어로 옮겼습니다
 * (저작권·라이선스 문구만 원문 유지). "▶"로 시작하는 주석은 추가한 해설입니다.
 *
 * ▶ 함수 순서를 "읽는 순서(호출 순서)"로 바꿨습니다. 함수 본문은 원본 그대로입니다.
 *   (C는 static 함수를 쓰기 전에 선언해야 해서, 이 순서로는 컴파일되지 않습니다.
 *    원본은 아래에서 위로 — 부품을 먼저 정의하고 aeMain이 맨 마지막 — 배치돼 있습니다.)
 *
 * ▶ 이 파일의 순서
 *   1. 진입점과 루프 한 바퀴
 *        aeMain, aeProcessEvents
 *   2. 타이머: 얼마나 잘지, 시각이 된 타이머 실행
 *        usUntilEarliestTimer, processTimeEvents
 *   3. 파일 이벤트 등록/해제/조회
 *        aeCreateFileEvent, aeDeleteFileEvent, aeGetFileClientData, aeGetFileEvents
 *   4. 타이머 등록/삭제
 *        aeCreateTimeEvent, aeDeleteTimeEvent
 *   5. 루프 생성/설정/파괴
 *        aeCreateEventLoop, aeGetSetSize, aeSetDontWait, aeResizeSetSize, aeDeleteEventLoop, aeStop
 *   6. 부록: 루프와 무관한 유틸, 훅 등록
 *        aeWait, aeGetApiName, aeSetBeforeSleepProc / aeSetAfterSleepProc
 *
 * ▶ 이 파일의 역할 분담
 *   ae.c        : OS와 무관한 공통 로직 (등록부 관리, 콜백 호출 순서, 타이머)
 *   ae_epoll.c  : OS와 직접 대화 (epoll_ctl, epoll_wait). aeApiXxx 함수들
 *   ae.c는 aeApiCreate / aeApiAddEvent / aeApiDelEvent / aeApiPoll ... 만 부르고,
 *   그 함수들의 실체는 아래 #include로 붙여넣은 ae_epoll.c 안에 있습니다.
 * ====================================================================== */

/* 간단한 이벤트 기반 프로그래밍 라이브러리. 원래 Jim(Tcl 인터프리터)의
 * 이벤트루프용으로 작성했지만, 나중에 재사용하기 쉽도록 라이브러리 형태로
 * 옮겼다.
 *
 * Copyright (c) 2006-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */

#include "ae.h"
#include "anet.h"          /* ▶ 소켓 유틸. 여기선 epoll 쪽의 anetCloexec 하나 때문에 필요 */
#include "redisassert.h"   /* ▶ panic() 매크로. epoll_wait가 예상 못한 에러를 내면 서버를 죽임 */

/* ▶ 표준 라이브러리 / OS 헤더. Kotlin의 import java.xxx 정도로 보면 됩니다.
 *   poll.h  : aeWait()에서 쓰는 poll()
 *   errno.h : 에러 코드 전역변수 errno
 *   time.h, sys/time.h : struct timeval */
#include <stdio.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <poll.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "zmalloc.h"   /* ▶ zmalloc/zfree/zrealloc: 메모리 사용량을 세는 malloc 래퍼 */
#include "config.h"    /* ▶ 빌드 환경 감지. 리눅스면 HAVE_EPOLL, macOS면 HAVE_KQUEUE를 정의 */

/* 이 시스템이 지원하는 가장 좋은 다중화 계층을 포함한다.
 * 아래 목록은 성능이 좋은 순서(내림차순)로 두어야 한다. */
/* ▶ ===== 컴파일 타임에 OS별 구현을 고르는 부분 =====
 *   .c 파일을 통째로 붙여넣습니다. 결과적으로 아래 중 "딱 하나"만 이 파일에 들어옵니다.
 *     evport : Solaris
 *     epoll  : Linux   ← 실제 운영 Redis는 거의 다 이것. ae_epoll.c 해설판 참고
 *     kqueue : macOS, BSD
 *     select : 최후의 수단 (fd 1024개 제한, 매번 전체를 훑어서 느림)
 *   Kotlin 비유: val api: PollApi = if (linux) EpollApi() else if (mac) KqueueApi() else SelectApi()
 *   단, C는 이 선택을 빌드할 때 끝내므로 런타임 분기 비용이 0입니다. */
#ifdef HAVE_EVPORT
#include "ae_evport.c"
#else
    #ifdef HAVE_EPOLL
    #include "ae_epoll.c"
    #else
        #ifdef HAVE_KQUEUE
        #include "ae_kqueue.c"
        #else
        #include "ae_select.c"
        #endif
    #endif
#endif


/* ######################################################################
 * ▶ 1. 진입점과 루프 한 바퀴
 * ###################################################################### */

/* ======================================================================
 * ▶ ★ [무한 루프] aeMain — 여기서부터 읽으세요 ★
 *   Redis 호출부 (server.c main() 맨 끝):
 *       aeMain(server.el);
 *   이 줄 이후로 Redis 메인 스레드는 서버가 꺼질 때까지 이 while문 안에 있습니다.
 *   모든 일(접속 수락, 명령 처리, 응답, 만료 키 정리, 복제...)은
 *   이 루프가 부르는 콜백 안에서 일어납니다.
 *
 *   Kotlin: fun main(loop: AeEventLoop) { loop.stop = false; while (!loop.stop) loop.processEvents(ALL) }
 *
 *   8.0부터 I/O 스레드도 각자 자기 aeEventLoop로 aeMain을 돌립니다 (iothread.c).
 * ====================================================================== */
void aeMain(aeEventLoop *eventLoop) {
    eventLoop->stop = 0;
    while (!eventLoop->stop) {
        aeProcessEvents(eventLoop, AE_ALL_EVENTS|
                                   AE_CALL_BEFORE_SLEEP|
                                   AE_CALL_AFTER_SLEEP);
    }
}

/* 대기 중인 파일 이벤트를 모두 처리한 다음, 대기 중인 시간 이벤트를 모두
 * 처리한다(방금 처리한 파일 이벤트 콜백이 등록한 것일 수도 있다).
 * 특별한 flag가 없으면 이 함수는 어떤 파일 이벤트가 발생하거나,
 * 다음 시간 이벤트 시각이 될 때까지(있다면) 잠든다.
 *
 * flags가 0이면 아무것도 하지 않고 반환한다.
 * flags에 AE_ALL_EVENTS가 있으면 모든 종류의 이벤트를 처리한다.
 * flags에 AE_FILE_EVENTS가 있으면 파일 이벤트를 처리한다.
 * flags에 AE_TIME_EVENTS가 있으면 시간 이벤트를 처리한다.
 * flags에 AE_DONT_WAIT가 있으면, 기다리지 않고 처리할 수 있는 이벤트를
 * 모두 처리한 뒤 최대한 빨리 반환한다.
 * flags에 AE_CALL_AFTER_SLEEP가 있으면 aftersleep 콜백을 호출한다.
 * flags에 AE_CALL_BEFORE_SLEEP가 있으면 beforesleep 콜백을 호출한다.
 *
 * 처리한 이벤트 수를 반환한다. */
/* ======================================================================
 * ▶ ★★★ [루프 한 바퀴] aeProcessEvents — 이 파일의 핵심 ★★★
 *
 *   ① beforesleep()              잠들기 전 정리 (AOF write → 응답 전송 등)
 *   ② timeout 계산               얼마나 잘까? (0 / 다음 타이머까지 / 무한)
 *   ③ aeApiPoll() = epoll_wait   ★ 여기서 스레드가 잠듦. 이벤트 or timeout에 깨어남
 *   ④ aftersleep()               깨어난 직후 처리
 *   ⑤ fired[] 순회               fd마다 읽기/쓰기 콜백 호출
 *   ⑥ processTimeEvents()        시각 된 타이머 실행
 *
 *   flags는 "이번 한 바퀴의 옵션"입니다. 평소(aeMain)에는
 *   AE_ALL_EVENTS | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP 로 불립니다.
 *   RDB/AOF 로딩 중에는 processEventsWhileBlocked(networking.c)가
 *   AE_FILE_EVENTS | AE_DONT_WAIT | ... 로 불러서 "안 자고, 타이머 없이" 한 바퀴만 돕니다.
 *   (로딩 중에 접속한 클라이언트에게 -LOADING 같은 응답이라도 보내기 위해)
 * ====================================================================== */
int aeProcessEvents(aeEventLoop *eventLoop, int flags)
{
    int processed = 0, numevents;

    /* 할 일이 없나? 그러면 바로 반환 */
    if (!(flags & AE_TIME_EVENTS) && !(flags & AE_FILE_EVENTS)) return 0;

    /* 처리할 파일 이벤트가 없더라도 시간 이벤트를 처리하려 한다면
     * aeApiPoll()을 호출한다는 점에 주의. 다음 시간 이벤트가
     * 실행될 때까지 잠들기 위해서다. */
    /* ▶ poll(잠들기)을 할지 결정:
     *     감시 중인 fd가 하나라도 있다           → poll 해야 함
     *     fd는 없어도 타이머 처리 + 기다려도 됨  → poll을 "타이머까지 자는 용도"로 씀
     *   fd가 하나도 없고 DONT_WAIT이면 → poll 생략하고 바로 ⑥으로.
     *   (beforesleep/aftersleep도 이 블록 안에 있으므로 그때는 안 불립니다) */
    if (eventLoop->maxfd != -1 ||
        ((flags & AE_TIME_EVENTS) && !(flags & AE_DONT_WAIT))) {
        int j;
        struct timeval tv, *tvp = NULL; /* NULL은 무한 대기를 뜻한다. */
        int64_t usUntilTimer;

        /* ▶ ① 잠들기 전 훅. Redis에선 server.c의 beforeSleep().
         *   여기서 AOF 버퍼를 파일에 write하고(always면 fsync까지), 그다음 쌓인 응답을
         *   소켓에 직접 write하고(handleClientsWithPendingWrites), 필요하면 aeSetDontWait(1)을 켭니다. */
        if (eventLoop->beforesleep != NULL && (flags & AE_CALL_BEFORE_SLEEP))
            eventLoop->beforesleep(eventLoop);

        /* eventLoop->flags는 beforesleep 안에서 바뀔 수 있다.
         * 그래서 beforesleep을 호출한 뒤에 검사해야 한다. 동시에 인자로 받은
         * flags가 항상 가장 높은 우선순위를 가져야 한다.
         * 즉 인자 flags에 AE_DONT_WAIT가 설정돼 있으면,
         * eventLoop->flags가 어떤 값이든 무시해야 한다. */
        /* ▶ ② timeout 계산. 결과는 tvp에 담김.
         *     tvp == NULL      → 무한 대기 (이벤트 올 때까지)
         *     tv = {0, 0}      → 전혀 안 기다림 (상태만 확인하고 바로 리턴)
         *     tv = {초, 마이크로초} → 그만큼만 기다림
         *   beforesleep 안에서 aeSetDontWait을 켰을 수 있으므로 반드시 그 "다음에" 검사. */
        if ((flags & AE_DONT_WAIT) || (eventLoop->flags & AE_DONT_WAIT)) {
            tv.tv_sec = tv.tv_usec = 0;
            tvp = &tv;
        } else if (flags & AE_TIME_EVENTS) {
            usUntilTimer = usUntilEarliestTimer(eventLoop);
            if (usUntilTimer >= 0) {
                tv.tv_sec = usUntilTimer / 1000000;     /* ▶ us → 초 */
                tv.tv_usec = usUntilTimer % 1000000;    /* ▶ 나머지 us */
                tvp = &tv;
            }
            /* ▶ usUntilTimer == -1(타이머 없음)이면 tvp는 NULL 그대로 → 무한 대기 */
        }
        /* 다중화 API를 호출한다. timeout이 되거나 어떤 이벤트가
         * 발생해야만 반환된다. */
        /* ▶ ③ ★ 여기서 잠듭니다 ★
         *   epoll_wait(epfd, ..., timeout)을 부르고, OS가 깨워줄 때까지 이 스레드는 멈춰 있습니다.
         *   CPU를 전혀 쓰지 않습니다. 깨어나면 발생한 이벤트를 fired[0..numevents-1]에 채워두고
         *   개수를 돌려줍니다. 구현은 ae_epoll.c의 aeApiPoll. */
        numevents = aeApiPoll(eventLoop, tvp);

        /* 요청받지 않았으면 파일 이벤트는 처리하지 않는다. */
        /* ▶ 타이머만 처리하라고 했으면 poll은 "자는 용도"로만 쓰고 결과는 버림. */
        if (!(flags & AE_FILE_EVENTS)) {
            numevents = 0;
        }

        /* 잠든 뒤(깨어난 직후) 콜백. */
        /* ▶ ④ 깨어난 직후 훅. Redis에선 server.c의 afterSleep() (모듈 GIL 재획득, 시간 캐시 갱신 등). */
        if (eventLoop->aftersleep != NULL && flags & AE_CALL_AFTER_SLEEP)
            eventLoop->aftersleep(eventLoop);

        /* ▶ ⑤ 발생한 이벤트마다 등록된 콜백 호출.
         *   fired[j]  = "무엇이 일어났나" (fd, 실제 발생 mask)
         *   events[fd] = "그 fd에 무엇을 등록했나" (관심 mask, 콜백, clientData) */
        for (j = 0; j < numevents; j++) {
            int fd = eventLoop->fired[j].fd;
            aeFileEvent *fe = &eventLoop->events[fd];
            int mask = eventLoop->fired[j].mask;
            int fired = 0; /* 현재 fd에 대해 실행한 이벤트 수. */

            /* 보통은 읽기 이벤트를 먼저, 쓰기 이벤트를 나중에 실행한다.
             * 이렇게 하면 요청을 처리한 직후 바로 그 응답을 보낼 수 있는
             * 경우가 있어서 유용하다.
             *
             * 하지만 mask에 AE_BARRIER가 설정돼 있으면, 애플리케이션이
             * 반대로 하라고 요청하는 것이다. 즉 읽기 다음에 쓰기 이벤트를
             * 절대 실행하지 않는다. 이 경우 호출 순서를 뒤집는다.
             * 예를 들어 클라이언트에 응답하기 전에 beforeSleep() 훅에서
             * 파일을 디스크에 fsync하는 등의 일을 하고 싶을 때 유용하다. */
            /* ▶ 기본 순서: 읽기 → 쓰기
             *     요청을 읽고 처리한 뒤, 같은 바퀴에서 바로 응답을 쓸 기회를 얻음.
             *   BARRIER면: 쓰기 → 읽기 (뒤집기)
             *     이번에 읽은 요청의 응답은 이번 바퀴의 쓰기 콜백으로 안 나감.
             *     → 반드시 다음 beforeSleep(AOF write/fsync 뒤)을 거쳐서 나가게 됨.
             *   참고: Redis 본체는 AE_BARRIER를 쓰지 않습니다(이 파일과 ae.h에만 있음).
             *     같은 일을 connection 계층의 CONN_FLAG_WRITE_BARRIER가 하고, 그것도
             *     appendfsync always에서 쓰기 핸들러가 설치된 클라이언트에만 켜는 보조 장치입니다.
             *     "OK면 AOF에 fsync됨"(always 한정)의 주된 보장은 beforeSleep 안의 순서
             *     (AOF write/fsync → 응답 write)입니다. */
            int invert = fe->mask & AE_BARRIER;

            /* "fe->mask & mask & ..." 코드에 주목: 앞서 처리한 이벤트가,
             * 발생했지만 아직 처리하지 않은 항목을 제거했을 수 있다. 그래서
             * 이벤트가 아직 유효한지 확인한다.
             *
             * 호출 순서가 뒤집히지 않았다면 읽기 이벤트를 실행한다. */
            /* ▶ "fe->mask & mask & AE_READABLE" 3중 AND의 의미:
             *     fe->mask : 지금도 읽기에 관심 있나? (앞선 콜백이 해제했을 수 있음)
             *     mask     : 실제로 읽기 이벤트가 왔나?
             *   예: fired에 fd 7, fd 9가 있는데 fd 7 처리 중에 fd 9 클라이언트를 끊었다면,
             *       fd 9의 fe->mask는 이미 NONE → 콜백을 부르지 않음. */
            if (!invert && fe->mask & mask & AE_READABLE) {
                fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                fired++;
                fe = &eventLoop->events[fd]; /* 배열 크기가 바뀌었을 경우를 대비해 다시 가져온다. */
                /* ▶ 콜백 안에서 aeCreateFileEvent가 불려 events 배열이 zrealloc으로
                 *   이사했을 수 있음 → fe를 새 주소로 다시 계산. (Kotlin에선 필요 없는 줄) */
            }

            /* 쓰기 이벤트를 실행한다. */
            if (fe->mask & mask & AE_WRITABLE) {
                /* ▶ "!fired || wfileProc != rfileProc":
                 *   읽기 콜백을 이미 불렀고, 쓰기 콜백이 "같은 함수"라면 또 부르지 않음.
                 *   Redis 소켓 계층은 읽기/쓰기 모두 connSocketEventHandler 하나를 등록하고,
                 *   그 함수가 mask를 보고 내부에서 읽기/쓰기를 나눠 처리합니다.
                 *   그래서 ae 입장에서는 fd당 한 번만 불러주면 됩니다. */
                if (!fired || fe->wfileProc != fe->rfileProc) {
                    fe->wfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            /* 호출 순서를 뒤집어야 한다면, 쓰기 이벤트를 실행한 지금
             * 읽기 이벤트를 실행한다. */
            /* ▶ BARRIER일 때: 쓰기를 먼저 했으니 이제 읽기. 같은 함수 중복 호출 방지도 동일. */
            if (invert) {
                fe = &eventLoop->events[fd]; /* 배열 크기가 바뀌었을 경우를 대비해 다시 가져온다. */
                if ((fe->mask & mask & AE_READABLE) &&
                    (!fired || fe->wfileProc != fe->rfileProc))
                {
                    fe->rfileProc(eventLoop,fd,fe->clientData,mask);
                    fired++;
                }
            }

            processed++;   /* ▶ fd 단위로 1 증가 (콜백 호출 횟수가 아님) */
        }
    }
    /* 시간 이벤트 확인 */
    /* ▶ ⑥ 타이머 처리. 파일 이벤트 "뒤에" 하므로, 방금 콜백들이 만든 타이머도 시각이 됐으면 실행됨. */
    if (flags & AE_TIME_EVENTS)
        processed += processTimeEvents(eventLoop);

    return processed; /* 처리한 파일/시간 이벤트 수를 반환 */
}


/* ######################################################################
 * ▶ 2. 타이머: 얼마나 잘지, 시각이 된 타이머 실행
 * ###################################################################### */

/* 가장 먼저 실행될 타이머까지 남은 마이크로초.
 * 타이머가 없으면 -1을 반환한다.
 *
 * 시간 이벤트가 정렬돼 있지 않으므로 O(N)이다.
 * 가능한 최적화 (지금까지 Redis에는 필요 없었지만...):
 * 1) 이벤트를 순서대로 삽입해서 가장 가까운 것이 항상 head가 되게 한다.
 *    훨씬 낫지만 타이머 삽입이나 삭제는 여전히 O(N)이다.
 * 2) skiplist를 써서 이 연산을 O(1), 삽입을 O(log(N))으로 만든다.
 */
/* ▶ "가장 가까운 타이머까지 몇 us 남았나?" → epoll_wait에서 얼마나 잘지 결정.
 *   반환: -1 = 타이머 없음(무한정 자도 됨), 0 = 이미 지남(바로 깨어나야 함), 그 외 = 남은 us
 *
 *   리스트 전체를 훑는 O(N). Redis 메인 루프의 타이머는 기본 설정에서 serverCron 하나라 충분
 *   (active defrag, 시간 예산을 넘긴 eviction, 모듈 타이머가 켜지면 몇 개 더 생김).
 *   Kotlin: timers.filter { it.id != DELETED }.minByOrNull { it.when } */
static int64_t usUntilEarliestTimer(aeEventLoop *eventLoop) {
    aeTimeEvent *te = eventLoop->timeEventHead;
    if (te == NULL) return -1;

    aeTimeEvent *earliest = NULL;
    while (te) {
        if ((!earliest || te->when < earliest->when) && te->id != AE_DELETED_EVENT_ID)
            earliest = te;
        te = te->next;
    }

    /* 리스트에 AE_DELETED_EVENT_ID로 표시된 이벤트만 남아 있으면 가장 이른
     * 타이머가 없다. 위의 빈 리스트 경우와 똑같이 "타이머 없음"을 반환하고,
     * NULL인 earliest를 역참조하지 않는다. */
    /* ▶ 8.0에는 없고 unstable에서 추가된 방어 코드. 삭제 예정 타이머만 남은 경우. */
    if (earliest == NULL) return -1;

    monotime now = getMonotonicUs();
    return (now >= earliest->when) ? 0 : earliest->when - now;
}

/* 시간 이벤트 처리 */
/* ======================================================================
 * ▶ [타이머 실행] processTimeEvents — 루프 한 바퀴의 마지막 단계
 *   리스트를 한 번 훑으면서 노드마다 세 가지 중 하나를 합니다.
 *     (A) 삭제 예정(id == -1)  → 리스트에서 떼어내고 해제 (실행 중이면 보류)
 *     (B) 이번 바퀴에 새로 생긴 것 → 건너뜀
 *     (C) 시각이 됨            → 콜백 실행 후, 반환값에 따라 재예약 or 삭제 예정 표시
 *
 *   ★ 반복 타이머의 원리
 *     retval = 콜백 반환값
 *     retval == AE_NOMORE(-1) → 일회성. 삭제 예정으로 표시
 *     그 외                    → when = 지금 + retval ms  (같은 노드를 재사용해 재예약)
 *   즉 "매 N ms마다"라는 설정이 따로 없고, 콜백이 매번 "다음엔 N ms 뒤에"라고 답합니다.
 *   serverCron이 hz를 바꾸면(dynamic-hz) 다음 주기도 바로 바뀌는 이유입니다.
 * ====================================================================== */
static int processTimeEvents(aeEventLoop *eventLoop) {
    int processed = 0;
    aeTimeEvent *te;
    long long maxId;

    te = eventLoop->timeEventHead;
    maxId = eventLoop->timeEventNextId-1;   /* ▶ 지금 시점까지 발급된 가장 큰 id. (B)에서 사용 */
    monotime now = getMonotonicUs();
    while(te) {
        long long id;

        /* 삭제 예정인 이벤트를 제거한다. */
        /* ▶ (A) 삭제 예정 노드 처리 */
        if (te->id == AE_DELETED_EVENT_ID) {
            aeTimeEvent *next = te->next;
            /* 이 시간 이벤트에 대한 참조가 남아 있으면
             * 해제하지 않는다. 현재 이 값은 timerProc이
             * 재귀적으로 호출될 때 증가한다. */
            /* ▶ refcount > 0 = 이 타이머의 콜백이 "아직 실행 중".
             *   언제 이런 일이? 타이머 콜백 안에서 다시 aeProcessEvents를 부르면
             *   (재귀) 이 함수가 안쪽에서 또 실행됩니다. 그때 바깥에서 실행 중인
             *   노드를 해제하면, 바깥으로 돌아갔을 때 해제된 메모리를 만지게 됩니다.
             *   그래서 실행 중이면 해제를 다음 기회로 미룹니다. */
            if (te->refcount) {
                te = next;
                continue;
            }
            /* ▶ 양방향 리스트에서 노드 떼어내기 (앞뒤를 서로 이어줌)
             *     prev <-> te <-> next   →   prev <-> next */
            if (te->prev)
                te->prev->next = te->next;
            else
                eventLoop->timeEventHead = te->next;   /* ▶ te가 head였으면 head 교체 */
            if (te->next)
                te->next->prev = te->prev;
            if (te->finalizerProc) {
                te->finalizerProc(eventLoop, te->clientData);
                now = getMonotonicUs();   /* ▶ finalizer가 시간을 썼을 수 있으니 현재 시각 갱신 */
            }
            zfree(te);
            te = next;
            continue;
        }

        /* 이번 반복에서 시간 이벤트(의 콜백)가 만든 시간 이벤트는 처리하지
         * 않도록 한다. 참고로 지금은 이 검사가 쓸모없다. 새 타이머는 항상
         * head에 추가되기 때문이다. 하지만 구현 세부가 바뀌면 다시 쓸모 있어질
         * 수 있으므로, 미래를 위한 방어로 남겨 둔다. */
        /* ▶ (B) 이번 순회 중에 콜백이 만든 새 타이머는 건너뜀.
         *   원본 주석대로 지금은 새 타이머가 head에 붙어서(이미 지나온 위치) 사실상 안 걸립니다.
         *   구현이 바뀔 때를 대비한 방어 코드. */
        if (te->id > maxId) {
            te = te->next;
            continue;
        }

        /* ▶ (C) 시각이 된 타이머 실행 */
        if (te->when <= now) {
            int retval;

            id = te->id;
            te->refcount++;                                  /* ▶ "실행 중" 표시 */
            retval = te->timeProc(eventLoop, id, te->clientData);   /* ▶ 예: serverCron() */
            te->refcount--;
            processed++;
            now = getMonotonicUs();                          /* ▶ 콜백이 시간을 썼으니 갱신 */
            if (retval != AE_NOMORE) {
                te->when = now + (monotime)retval * 1000;    /* ▶ 재예약: retval ms 뒤 */
            } else {
                te->id = AE_DELETED_EVENT_ID;                /* ▶ 일회성: 다음 순회 때 해제 */
            }
        }
        /* ▶ 콜백이 다른 타이머를 지웠어도 "표시"만 했으므로 te->next는 여전히 유효합니다.
         *   aeDeleteTimeEvent가 바로 free하지 않는 이유가 바로 이 줄을 지키기 위해서입니다. */
        te = te->next;
    }
    return processed;
}


/* ######################################################################
 * ▶ 3. 파일 이벤트 등록/해제/조회
 * ###################################################################### */

/* ======================================================================
 * ▶ [파일 이벤트 등록] aeCreateFileEvent
 *   "fd가 mask(읽기/쓰기) 가능해지면 proc(clientData)를 불러줘"
 *
 *   Redis 호출부 예 (socket.c connSocketSetReadHandler):
 *       aeCreateFileEvent(conn->el, conn->fd, AE_READABLE,
 *                         conn->type->ae_handler, conn);
 *   → ae_handler = connSocketEventHandler, clientData = connection 객체.
 *   읽기/쓰기 모두 같은 connSocketEventHandler를 등록한다는 점을 기억하세요.
 *   (aeProcessEvents의 "wfileProc != rfileProc" 검사와 연결됩니다)
 *
 *   여러 번 불러도 됩니다. 이미 READABLE이 있는 fd에 WRITABLE을 추가하면
 *   mask가 합쳐집니다(|=). epoll 쪽에서도 ADD가 아니라 MOD로 처리합니다.
 * ====================================================================== */
int aeCreateFileEvent(aeEventLoop *eventLoop, int fd, int mask,
        aeFileProc *proc, void *clientData)
{
    /* ▶ 상한 초과. errno에 이유를 남기고 실패 반환. */
    if (fd >= eventLoop->setsize) {
        errno = ERANGE;
        return AE_ERR;
    }

    /* fd가 현재 이벤트 배열 크기를 넘으면
     * events와 fired 배열의 크기를 늘린다. */
    /* ▶ fd가 현재 배열 크기를 넘으면 배열을 키움.
     *   새 크기 = max(현재 * 2, fd + 1), 단 setsize를 넘지 않게.
     *   ArrayList가 꽉 차면 1.5배로 늘리는 것과 같은 아이디어.
     *
     *   ⚠ zrealloc은 배열을 "다른 주소로 옮길" 수 있습니다.
     *   그래서 이 함수가 콜백 안에서 불리면, aeProcessEvents가 들고 있던
     *   fe 포인터가 낡은 주소가 됩니다. → aeProcessEvents의 "다시 가져온다" 주석 참고 */
    if (unlikely(fd >= eventLoop->nevents)) {
        int newnevents = eventLoop->nevents;
        newnevents = (newnevents * 2 > fd + 1) ? newnevents * 2 : fd + 1;
        newnevents = (newnevents > eventLoop->setsize) ? eventLoop->setsize : newnevents;
        eventLoop->events = zrealloc(eventLoop->events, sizeof(aeFileEvent) * newnevents);
        eventLoop->fired = zrealloc(eventLoop->fired, sizeof(aeFiredEvent) * newnevents);

        /* 새로 생긴 칸을 AE_NONE mask로 초기화 */
        for (int i = eventLoop->nevents; i < newnevents; i++)
            eventLoop->events[i].mask = AE_NONE;
        eventLoop->nevents = newnevents;
    }

    /* ▶ events[fd] 칸을 가리키는 포인터. Kotlin: val fe = eventLoop.events[fd] */
    aeFileEvent *fe = &eventLoop->events[fd];

    /* ▶ 순서 주의: OS(epoll)에 먼저 등록하고, 성공하면 등록부를 갱신.
     *   aeApiAddEvent는 "기존 mask"(fe->mask)를 보고 epoll ADD냐 MOD냐를 정하므로
     *   반드시 fe->mask를 바꾸기 "전에" 불러야 합니다. */
    if (aeApiAddEvent(eventLoop, fd, mask) == -1)
        return AE_ERR;
    fe->mask |= mask;                               /* ▶ 관심사 추가 (기존 것 유지) */
    if (mask & AE_READABLE) fe->rfileProc = proc;   /* ▶ 읽기 콜백 설정 */
    if (mask & AE_WRITABLE) fe->wfileProc = proc;   /* ▶ 쓰기 콜백 설정 */
    fe->clientData = clientData;                    /* ▶ 하나뿐. 덮어씀 */
    if (fd > eventLoop->maxfd)
        eventLoop->maxfd = fd;
    return AE_OK;
}

/* ▶ [파일 이벤트 해제] mask에 해당하는 관심사만 뺍니다.
 *   READABLE|WRITABLE 중 WRITABLE만 빼면 READABLE은 남습니다.
 *   Redis 예: 응답을 다 보내면 쓰기 감시만 해제 (socket.c: aeDeleteFileEvent(..., AE_WRITABLE))
 *   쓰기 감시를 계속 켜두면, 소켓은 거의 항상 쓰기 가능하므로 epoll이 매 바퀴 깨워서 CPU를 태웁니다.
 *
 *   참고: 함수 첫 줄은 fd < setsize만 검사하고 fd < nevents는 검사하지 않습니다.
 *   이 파일만 보면 nevents 밖의 fd가 들어오면 배열 범위 밖을 읽게 됩니다. (원본 그대로 둠) */
void aeDeleteFileEvent(aeEventLoop *eventLoop, int fd, int mask)
{
    if (fd >= eventLoop->setsize) return;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return;     /* ▶ 등록된 게 없으면 할 일 없음 */

    /* AE_WRITABLE을 제거할 때는 AE_BARRIER가 설정돼 있으면
     * 항상 같이 제거한다. */
    /* ▶ BARRIER는 WRITABLE에 딸린 옵션이라, WRITABLE을 빼면 같이 뺌. */
    if (mask & AE_WRITABLE) mask |= AE_BARRIER;

    aeApiDelEvent(eventLoop, fd, mask);  /* ▶ OS(epoll)에서 먼저 해제 (역시 기존 mask 참조) */
    fe->mask = fe->mask & (~mask);       /* ▶ 등록부에서 해당 비트 끄기 */
    /* ▶ 지운 fd가 maxfd였고 이제 완전히 비었으면, maxfd를 아래로 내려가며 다시 찾음. */
    if (fd == eventLoop->maxfd && fe->mask == AE_NONE) {
        /* max fd 갱신 */
        int j;

        for (j = eventLoop->maxfd-1; j >= 0; j--)
            if (eventLoop->events[j].mask != AE_NONE) break;
        eventLoop->maxfd = j;            /* ▶ 하나도 없으면 -1이 됨 */
    }
}

/* ▶ fd에 달아둔 clientData 조회. 등록 안 됐으면 NULL. */
void *aeGetFileClientData(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return NULL;
    aeFileEvent *fe = &eventLoop->events[fd];
    if (fe->mask == AE_NONE) return NULL;

    return fe->clientData;
}

/* ▶ fd의 현재 관심사(mask) 조회. */
int aeGetFileEvents(aeEventLoop *eventLoop, int fd) {
    if (fd >= eventLoop->setsize) return 0;
    aeFileEvent *fe = &eventLoop->events[fd];

    return fe->mask;
}


/* ######################################################################
 * ▶ 4. 타이머 등록/삭제
 * ###################################################################### */

/* ======================================================================
 * ▶ [타이머 등록] aeCreateTimeEvent
 *   "milliseconds 뒤에 proc(clientData)를 불러줘". 새 타이머의 id를 반환.
 *
 *   Redis 호출부 (server.c initServer):
 *       aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL);
 *   → 1ms 뒤 serverCron 첫 실행. serverCron은 `return 1000/server.hz;`로 끝나므로
 *     기본 hz=10이면 이후 100ms마다 반복됩니다. (반복 원리는 processTimeEvents 참고)
 *
 *   새 노드는 리스트 "맨 앞"에 붙습니다. 정렬하지 않습니다. O(1) 삽입.
 * ====================================================================== */
long long aeCreateTimeEvent(aeEventLoop *eventLoop, long long milliseconds,
        aeTimeProc *proc, void *clientData,
        aeEventFinalizerProc *finalizerProc)
{
    long long id = eventLoop->timeEventNextId++;   /* ▶ 0, 1, 2, ... 순서로 발급 */
    aeTimeEvent *te;

    te = zmalloc(sizeof(*te));
    if (te == NULL) return AE_ERR;
    te->id = id;
    te->when = getMonotonicUs() + milliseconds * 1000;   /* ▶ ms → us 변환 */
    te->timeProc = proc;
    te->finalizerProc = finalizerProc;
    te->clientData = clientData;
    /* ▶ 양방향 리스트 맨 앞에 끼워 넣기:
     *     [새 te] <-> [기존 head] <-> ...
     *   Kotlin: timers.addFirst(te) */
    te->prev = NULL;
    te->next = eventLoop->timeEventHead;
    te->refcount = 0;
    if (te->next)
        te->next->prev = te;
    eventLoop->timeEventHead = te;
    return id;
}

/* ▶ [타이머 삭제] 노드를 바로 지우지 않고 id만 -1로 바꿔 "삭제 예정" 표시.
 *   실제 해제는 다음 processTimeEvents에서 합니다.
 *
 *   왜 바로 안 지우나?
 *   processTimeEvents가 리스트를 순회하며 콜백을 부르는 "도중에" 콜백이
 *   다른 타이머(또는 자기 자신)를 지울 수 있습니다. 그때 노드를 바로 free하면
 *   순회 중인 te->next가 해제된 메모리를 가리키게 됩니다.
 *   (Kotlin에서 for 루프 도중 list.remove()하면 ConcurrentModificationException이
 *    나는 것과 같은 문제. Java는 예외로 막아주지만 C는 조용히 메모리를 망가뜨립니다.) */
int aeDeleteTimeEvent(aeEventLoop *eventLoop, long long id)
{
    aeTimeEvent *te = eventLoop->timeEventHead;
    while(te) {
        if (te->id == id) {
            te->id = AE_DELETED_EVENT_ID;
            return AE_OK;
        }
        te = te->next;
    }
    return AE_ERR; /* 지정한 ID의 이벤트를 찾지 못함 */
}


/* ######################################################################
 * ▶ 5. 루프 생성/설정/파괴
 * ###################################################################### */

/* ======================================================================
 * ▶ [생성] aeCreateEventLoop
 *   Redis 호출부 (server.c initServer):
 *       server.el = aeCreateEventLoop(server.maxclients + CONFIG_FDSET_INCR);
 *   CONFIG_FDSET_INCR = 128 (32 + 96). 클라이언트 외에 리슨 소켓, 파이프,
 *   로그 파일 등 서버 자신이 쓰는 fd 몫을 여유로 더한 것입니다.
 *   기본 maxclients=10000이면 setsize=10128.
 *
 *   Kotlin 대응:
 *   fun aeCreateEventLoop(setsize: Int): AeEventLoop? {
 *       val n = minOf(setsize, 1024)
 *       return AeEventLoop(setsize = setsize, nevents = n,
 *                          events = Array(n) { AeFileEvent(mask = AE_NONE) },
 *                          fired  = Array(n) { AeFiredEvent() },
 *                          maxfd = -1, ...).also { api.create(it) }
 *   }
 * ====================================================================== */
#define INITIAL_EVENT 1024
aeEventLoop *aeCreateEventLoop(int setsize) {
    aeEventLoop *eventLoop;
    int i;

    monotonicInit(NULL);    /* 호출한 앱이 초기화하지 않았을 경우를 대비 */

    /* ▶ 구조체 하나만큼 메모리를 빌림. 실패하면 err로 점프해서 정리. */
    if ((eventLoop = zmalloc(sizeof(*eventLoop))) == NULL) goto err;
    /* ▶ 배열을 처음부터 setsize(예: 10128)만큼 잡지 않고 최대 1024칸으로 시작.
     *   fd가 커지면 aeCreateFileEvent에서 늘립니다. 메모리 절약. */
    eventLoop->nevents = setsize < INITIAL_EVENT ? setsize : INITIAL_EVENT;
    eventLoop->events = zmalloc(sizeof(aeFileEvent)*eventLoop->nevents);
    eventLoop->fired = zmalloc(sizeof(aeFiredEvent)*eventLoop->nevents);
    if (eventLoop->events == NULL || eventLoop->fired == NULL) goto err;
    eventLoop->setsize = setsize;
    eventLoop->timeEventHead = NULL;
    eventLoop->timeEventNextId = 0;
    eventLoop->stop = 0;
    eventLoop->maxfd = -1;          /* ▶ -1 = 등록된 fd 없음 */
    eventLoop->beforesleep = NULL;
    eventLoop->aftersleep = NULL;
    eventLoop->flags = 0;
    memset(eventLoop->privdata, 0, sizeof(eventLoop->privdata));
    /* ▶ OS별 준비. epoll이면 epoll_create()로 "감시용 fd"를 하나 만듭니다. */
    if (aeApiCreate(eventLoop) == -1) goto err;
    /* mask == AE_NONE인 이벤트는 등록되지 않은 것이다. 그러니 배열을
     * 이 값으로 초기화한다. */
    /* ▶ zmalloc은 메모리를 0으로 채워주지 않습니다(쓰레기 값). 그래서 직접 초기화.
     *   Kotlin에서는 Array(n) { AeFileEvent() }가 알아서 해주는 일. */
    for (i = 0; i < eventLoop->nevents; i++)
        eventLoop->events[i].mask = AE_NONE;
    return eventLoop;

/* ▶ 실패 시 정리 코드. 빌린 것을 반납하고 NULL(=null) 반환.
 *   (zfree(NULL)은 아무것도 안 하므로 안전) */
err:
    if (eventLoop) {
        zfree(eventLoop->events);
        zfree(eventLoop->fired);
        zfree(eventLoop);
    }
    return NULL;
}

/* 현재 set 크기를 반환한다. */
int aeGetSetSize(aeEventLoop *eventLoop) {
    return eventLoop->setsize;
}

/*
 * 이벤트 처리 쪽에 대기 timeout을 가능한 한 빨리 바꾸라고 알린다.
 *
 * 참고: 실제로 하는 일은 전역 AE_DONT_WAIT를 켜고 끄는 것뿐이다.
 */
/* ▶ "다음 바퀴에 epoll에서 잠들지 마라" 스위치.
 *   Redis 호출부 (server.c beforeSleep 맨 끝):
 *       aeSetDontWait(server.el, dont_sleep);
 *   예: TLS 라이브러리 버퍼에 이미 읽어 둔 데이터가 남아 있으면(소켓에는 없으니 epoll이
 *   알려주지 않음) 잠들지 않고 timeout 0으로 바로 다음 바퀴를 돌게 합니다.
 *   그 밖에 압축 계층에 남은 데이터, WAITAOF 대기자, I/O 스레드에서 넘어온 클라이언트를
 *   처리한 경우에도 켭니다.
 *
 *   비트 연산: flags |= X 는 X 켜기, flags &= ~X 는 X 끄기 */
void aeSetDontWait(aeEventLoop *eventLoop, int noWait) {
    if (noWait)
        eventLoop->flags |= AE_DONT_WAIT;
    else
        eventLoop->flags &= ~AE_DONT_WAIT;
}

/* 이벤트루프의 최대 set 크기를 바꾼다.
 * 요청한 크기가 현재 크기보다 작은데, 이미 (요청한 크기 - 1) 이상인
 * fd가 사용 중이면 AE_ERR를 반환하고 아무것도 하지 않는다.
 *
 * 그렇지 않으면 AE_OK를 반환하고 변경에 성공한다. */
/* ▶ setsize(상한) 변경. CONFIG SET maxclients로 런타임에 바꿀 때 쓰입니다.
 *   줄이려는데 이미 그보다 큰 fd를 쓰고 있으면 거부. */
int aeResizeSetSize(aeEventLoop *eventLoop, int setsize) {
    if (setsize == eventLoop->setsize) return AE_OK;
    if (eventLoop->maxfd >= setsize) return AE_ERR;
    if (aeApiResize(eventLoop,setsize) == -1) return AE_ERR;

    eventLoop->setsize = setsize;

    /* 현재 할당된 공간이 요청한 크기보다 크면,
     * 요청한 크기로 줄여야 한다. */
    /* ▶ 늘릴 때는 아무것도 안 함(필요할 때 aeCreateFileEvent가 늘림).
     *   줄일 때만 배열을 잘라냄. */
    if (setsize < eventLoop->nevents) {
        eventLoop->events = zrealloc(eventLoop->events,sizeof(aeFileEvent)*setsize);
        eventLoop->fired = zrealloc(eventLoop->fired,sizeof(aeFiredEvent)*setsize);
        eventLoop->nevents = setsize;
    }
    return AE_OK;
}

/* ▶ [파괴] 루프 해제. 타이머마다 finalizer를 불러 clientData를 정리하게 합니다.
 *   Kotlin이면 close()에서 timers.forEach { it.finalizer?.invoke(...) } */
void aeDeleteEventLoop(aeEventLoop *eventLoop) {
    aeApiFree(eventLoop);
    zfree(eventLoop->events);
    zfree(eventLoop->fired);

    /* 시간 이벤트 리스트를 해제한다. */
    /* ▶ 연결 리스트 순회하며 해제하는 C 관용구.
     *   te를 해제하기 "전에" next를 따로 저장해야 합니다. 해제 후엔 te->next를 읽을 수 없으니까. */
    aeTimeEvent *next_te, *te = eventLoop->timeEventHead;
    while (te) {
        next_te = te->next;
        if (te->finalizerProc)
            te->finalizerProc(eventLoop, te->clientData);
        zfree(te);
        te = next_te;
    }
    zfree(eventLoop);
}

/* ▶ 루프 종료 요청. 즉시 멈추는 게 아니라, 현재 바퀴가 끝나고 while 조건에서 빠져나갑니다. */
void aeStop(aeEventLoop *eventLoop) {
    eventLoop->stop = 1;
}


/* ######################################################################
 * ▶ 6. 부록: 루프와 무관한 유틸, 훅 등록
 * ###################################################################### */

/* 주어진 fd가 쓰기 가능/읽기 가능/예외 상태가 될 때까지
 * 최대 milliseconds만큼 기다린다 */
/* ▶ 이벤트루프와 무관한 "동기 대기" 유틸. fd 하나만 poll()로 기다립니다.
 *   Redis 호출부 예 (socket.c connSocketBlockingConnect): 연결이 성립(쓰기 가능)될 때까지 timeout ms 대기.
 *   반환: 발생한 mask / 0(timeout) / -1(에러)
 *   에러·끊김(POLLERR/POLLHUP)은 WRITABLE로 알려줌 → 호출자가 쓰기를 시도하다 에러를 발견하게. */
int aeWait(int fd, int mask, long long milliseconds) {
    struct pollfd pfd;
    int retmask = 0, retval;

    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    if (mask & AE_READABLE) pfd.events |= POLLIN;
    if (mask & AE_WRITABLE) pfd.events |= POLLOUT;

    if ((retval = poll(&pfd, 1, milliseconds))== 1) {
        if (pfd.revents & POLLIN) retmask |= AE_READABLE;
        if (pfd.revents & POLLOUT) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLERR) retmask |= AE_WRITABLE;
        if (pfd.revents & POLLHUP) retmask |= AE_WRITABLE;
        return retmask;
    } else {
        return retval;
    }
}

/* ▶ "epoll" / "kqueue" / "select" 같은 이름 반환. INFO server의 multiplexing_api 항목. */
char *aeGetApiName(void) {
    return aeApiName();
}

/* ▶ 훅 등록. Redis 호출부 (server.c initServer):
 *       aeSetBeforeSleepProc(server.el, beforeSleep);
 *       aeSetAfterSleepProc(server.el, afterSleep); */
void aeSetBeforeSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *beforesleep) {
    eventLoop->beforesleep = beforesleep;
}

void aeSetAfterSleepProc(aeEventLoop *eventLoop, aeBeforeSleepProc *aftersleep) {
    eventLoop->aftersleep = aftersleep;
}
