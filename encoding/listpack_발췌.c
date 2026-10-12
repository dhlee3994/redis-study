/* ======================================================================
 * [해설판] listpack.c 발췌 — 작은 컬렉션을 담는 연속 메모리 형식
 * 원본 (기준 커밋: b540ca4, 2026-09-30):
 *   https://github.com/redis/redis/blob/unstable/src/listpack.c (3588줄, 뒤쪽 1400여 줄(L2163~)은 테스트)
 *   부록: listpack_malloc.h
 *
 * ▶ 이 파일은 컴파일용이 아닙니다. 원본 중 형식, 찾기, 삽입/삭제만 골라 다시 배치했습니다.
 *   - 각 조각 위에 원본 줄 번호를 적었습니다.
 *   - 코드와 영문 주석은 원본 그대로입니다. "▶"로 시작하는 한국어 주석만 추가했습니다.
 *   - 긴 부분은 "▶ [생략]" 표시와 함께 뺐습니다.
 *
 * ▶ 누가 쓰나
 *   작은 List / Hash / Set / ZSet은 모두 listpack 하나에 담깁니다 (object.h 인코딩 표).
 *   단, 정수만 있는 작은 Set은 intset입니다.
 *   큰 List(quicklist)와 Stream도 내부 노드 하나하나가 listpack입니다
 *   (노드 상한보다 큰 원소 하나만 담는 quicklist의 PLAIN 노드는 예외).
 *
 * ▶ 메모리 모양
 *
 *   [total_bytes 4][num_elements 2][원소][원소]...[원소][EOF 0xFF]
 *    └──────── 헤더 6바이트 ───────┘
 *
 *   원소 하나 = [encoding+길이 1~5바이트][데이터][backlen 1~5바이트]
 *     - 정수로 바뀌는 문자열("12345")은 정수로 저장: 0~127은 1바이트, 그 밖은 2~9바이트
 *     - 문자열: 63바이트 이하면 앞에 1바이트, 4095 이하면 2바이트, 그 이상이면 5바이트
 *     - backlen = 이 원소 자신의 (encoding+데이터) 길이. 뒤에서 앞으로 걸어갈 때 씀
 *
 *   예) Hash {name: "kim", age: "30"} → [hdr]["name"]["kim"]["age"][30][EOF]
 *       = 6 + (1+4+1) + (1+3+1) + (1+3+1) + (1+1) + 1 = 25바이트, 할당 1번
 *       같은 내용을 dict로 두면 버킷 배열, 필드/값 sds, (Hash면) Entry 할당이 따로따로 생깁니다.
 *
 * ▶ ziplist와 다른 점 (Redis 7.0에서 ziplist를 listpack으로 교체)
 *   ziplist는 원소마다 "앞 원소의 길이"를 저장했습니다. 앞 원소가 커져서 그 길이 필드가 1바이트에서
 *   5바이트로 늘면 다음 원소도 커지고, 그 다음도... 하고 연쇄로 고쳐 쓰는 "cascade update"가 있었습니다.
 *   listpack은 "자기 길이"만 저장하므로 원소 하나를 바꿔도 다른 원소의 크기가 바뀌지 않습니다.
 *
 * ▶ 비용 요약 (N = 원소 수, B = 전체 바이트)
 *   길이(lpLength)           : O(1). 헤더에 있음 (65535개 이상이면 O(N))
 *   찾기(lpFind)             : O(N). 처음부터 하나씩 건너뛰며 비교              ★
 *   인덱스로 찾기(lpSeek)     : O(N). 가까운 쪽 끝에서 출발
 *   끝에 추가(lpAppend)       : 대개 O(1), 가끔 realloc. realloc이 옮기면 O(B)
 *   중간 삽입/삭제(lpInsert)  : O(B). 뒤쪽 바이트를 memmove로 밀고 당김           ★
 *   해제                      : O(1). free 한 번
 *   → 작을 때는 캐시 친화적이라 빠르고 메모리가 적지만, 커지면 모든 명령이 N에 비례해 느려집니다.
 *     그래서 각 타입이 기준값(512, 128, 64바이트, 8KB)을 넘으면 다른 인코딩으로 바꿉니다.
 *
 * ▶ 목차
 *   [1] 헤더와 크기 상한
 *   [2] 원소 인코딩       : 정수/문자열 길이에 따라 바이트 수가 달라짐
 *   [3] 이동              : lpSkip, lpNext, lpPrev, lpLength
 *   [4] lpFind            : 조회가 O(N)인 이유                                       ★
 *   [5] lpInsert          : 삽입/삭제/교체가 전부 이 함수. realloc + memmove           ★
 *   [6] lpSeek            : 인덱스 접근 (LINDEX가 listpack일 때)
 *   [부록] lp_malloc_size
 * ====================================================================== */


/* ######################################################################
 * [1] 헤더와 크기 상한
 * ###################################################################### */

/* ======================================================================
 * ▶ [1-1] 헤더 상수 — listpack.c L27~L32
 * ====================================================================== */
#define LP_HDR_SIZE 6       /* 32 bit total len + 16 bit number of elements. */  /* ▶ ★ [전체 바이트 수 4][원소 수 2] */
#define LP_HDR_NUMELE_UNKNOWN UINT16_MAX  /* ▶ 원소 수 칸은 16비트. 65535개 이상이면 "모름"으로 두고 셀 때 전부 훑음 */
#define LP_MAX_INT_ENCODING_LEN 9
#define LP_MAX_BACKLEN_SIZE 5
#define LP_ENCODING_INT 0
#define LP_ENCODING_STRING 1

/* ======================================================================
 * ▶ [1-2] LP_EOF — listpack.c L76~L76
 * ====================================================================== */
#define LP_EOF 0xFF  /* ▶ 끝 표시 1바이트. 원소의 첫 바이트로는 나올 수 없는 값 */

/* ======================================================================
 * ▶ [1-3] lpSafeToAdd — 1GB 상한 — listpack.c L124~L132
 *   t_hash.c hashTypeTryConversion 등이 "이만큼 더 넣어도 되나"를 물을 때 씁니다.
 *   안 되면 listpack을 포기하고 큰 인코딩으로 바꿉니다.
 * ====================================================================== */
/* Don't let listpacks grow over 1GB in any case, don't wanna risk overflow in
 * Total Bytes header field */
#define LISTPACK_MAX_SAFETY_SIZE (1<<30)  /* ▶ 헤더의 total_bytes가 32비트이므로 넉넉히 1GB에서 막음 */
int lpSafeToAdd(unsigned char* lp, size_t add) {
    size_t len = lp? lpGetTotalBytes(lp): 0;
    if (add > LISTPACK_MAX_SAFETY_SIZE || len > LISTPACK_MAX_SAFETY_SIZE - add)
        return 0;
    return 1;
}

/* ======================================================================
 * ▶ [1-4] lpNew — listpack.c L219~L231
 * ====================================================================== */
/* Create a new, empty listpack.
 * On success the new listpack is returned, otherwise an error is returned.
 * Pre-allocate at least `capacity` bytes of memory,
 * over-allocated memory can be shrunk by `lpShrinkToFit`.
 * */
unsigned char *lpNew(size_t capacity) {
    unsigned char *lp = lp_malloc(capacity > LP_HDR_SIZE+1 ? capacity : LP_HDR_SIZE+1);  /* ▶ 빈 listpack = 헤더 6 + EOF 1 = 7바이트 */
    if (lp == NULL) return NULL;
    lpSetTotalBytes(lp,LP_HDR_SIZE+1);
    lpSetNumElements(lp,0);
    lp[LP_HDR_SIZE] = LP_EOF;  /* ▶ 첫 원소 자리에 EOF */
    return lp;
}


/* ######################################################################
 * [2] 원소 인코딩
 *   같은 값이라도 "정수로 읽히면" 정수로 저장합니다. Hash 값 "30"은 문자열이 아니라 1바이트 정수가 됩니다.
 *   그래서 listpack에서 원소 하나의 크기는 값의 모양에 따라 1바이트에서 수 KB까지 달라집니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [2-1] lpEncodeIntegerGetType — listpack.c L248~L308
 * ====================================================================== */
/* Stores the integer encoded representation of 'v' in the 'intenc' buffer. */
static inline void lpEncodeIntegerGetType(int64_t v, unsigned char *intenc, uint64_t *enclen) {
    if (v >= 0 && v <= 127) {  /* ▶ 0~127: 1바이트 (encoding과 값이 한 바이트에 같이 들어감) */
        /* Single byte 0-127 integer. */
        if (intenc != NULL) intenc[0] = v;
        if (enclen != NULL) *enclen = 1;
    } else if (v >= -4096 && v <= 4095) {  /* ▶ -4096~4095: 2바이트 */
        /* 13 bit integer. */
        if (v < 0) v = ((int64_t)1<<13)+v;
        if (intenc != NULL) {
            intenc[0] = (v>>8)|LP_ENCODING_13BIT_INT;
            intenc[1] = v&0xff;
        }
        if (enclen != NULL) *enclen = 2;
    /* ▶ [생략] 원본 L262~L291: 16, 24, 32비트 정수 (같은 방식으로 3, 4, 5바이트) */
    } else {  /* ▶ 그 밖: 9바이트 (encoding 1 + int64 8) */
        /* 64 bit integer. */
        uint64_t uv = v;
        if (intenc != NULL) {
            intenc[0] = LP_ENCODING_64BIT_INT;
            intenc[1] = uv&0xff;
            intenc[2] = (uv>>8)&0xff;
            intenc[3] = (uv>>16)&0xff;
            intenc[4] = (uv>>24)&0xff;
            intenc[5] = (uv>>32)&0xff;
            intenc[6] = (uv>>40)&0xff;
            intenc[7] = (uv>>48)&0xff;
            intenc[8] = uv>>56;
        }
        if (enclen != NULL) *enclen = 9;
    }
}

/* ======================================================================
 * ▶ [2-2] lpEncodeGetType — listpack.c L310~L332
 * ====================================================================== */
/* Given an element 'ele' of size 'size', determine if the element can be
 * represented inside the listpack encoded as integer, and returns
 * LP_ENCODING_INT if so. Otherwise returns LP_ENCODING_STR if no integer
 * encoding is possible.
 *
 * If the LP_ENCODING_INT is returned, the function stores the integer encoded
 * representation of the element in the 'intenc' buffer.
 *
 * Regardless of the returned encoding, 'enclen' is populated by reference to
 * the number of bytes that the string or integer encoded element will require
 * in order to be represented. */
static inline int lpEncodeGetType(unsigned char *ele, uint32_t size, unsigned char *intenc, uint64_t *enclen) {
    int64_t v;
    if (lpStringToInt64((const char*)ele, size, &v)) {  /* ▶ ★ 문자열이 int64로 정확히 바뀌면 정수로 저장 */
        lpEncodeIntegerGetType(v, intenc, enclen);
        return LP_ENCODING_INT;
    } else {
        if (size < 64) *enclen = 1+size;  /* ▶ 63바이트 이하 문자열: 앞에 1바이트 */
        else if (size < 4096) *enclen = 2+size;  /* ▶ 4095바이트 이하: 앞에 2바이트 */
        else *enclen = 5+(uint64_t)size;  /* ▶ 그 이상: 앞에 5바이트 */
        return LP_ENCODING_STRING;
    }
}

/* ======================================================================
 * ▶ [2-3] lpEncodeBacklen — listpack.c L334~L374
 *   원소 끝에 "자기 길이"를 거꾸로 읽을 수 있게 적습니다. 7비트씩 쓰고 최상위 비트로 "더 있음"을 표시.
 *   덕분에 lpPrev / lpLast가 앞에서부터 훑지 않고 뒤에서 바로 이전 원소로 갈 수 있습니다.
 * ====================================================================== */
/* Store a reverse-encoded variable length field, representing the length
 * of the previous element of size 'l', in the target buffer 'buf'.
 * The function returns the number of bytes used to encode it, from
 * 1 to 5. If 'buf' is NULL the function just returns the number of bytes
 * needed in order to encode the backlen. */
static inline unsigned long lpEncodeBacklen(unsigned char *buf, uint64_t l) {
    if (l <= 127) {  /* ▶ 127 이하면 1바이트. 작은 원소는 대부분 여기 */
        if (buf) buf[0] = l;
        return 1;
    } else if (l <= 16383) {
        if (buf) {
            buf[0] = l>>7;
            buf[1] = (l&127)|128;
        }
        return 2;
    /* ▶ [생략] 원본 L349~L373: 3~5바이트 경우 (같은 방식) */
}

/* ======================================================================
 * ▶ [2-4] lpEncodeString — listpack.c L439~L459
 * ====================================================================== */
/* Encode the string element pointed by 's' of size 'len' in the target
 * buffer 's'. The function should be called with 'buf' having always enough
 * space for encoding the string. This is done by calling lpEncodeGetType()
 * before calling this function. */
static inline void lpEncodeString(unsigned char *buf, unsigned char *s, uint32_t len) {
    if (len < 64) {
        buf[0] = len | LP_ENCODING_6BIT_STR;  /* ▶ 1바이트 헤더 = 10xxxxxx (길이 6비트) */
        memcpy(buf+1,s,len);
    } else if (len < 4096) {
        buf[0] = (len >> 8) | LP_ENCODING_12BIT_STR;  /* ▶ 2바이트 헤더 = 1110xxxx xxxxxxxx (길이 12비트) */
        buf[1] = len & 0xff;
        memcpy(buf+2,s,len);
    } else {
        buf[0] = LP_ENCODING_32BIT_STR;
        buf[1] = len & 0xff;
        buf[2] = (len >> 8) & 0xff;
        buf[3] = (len >> 16) & 0xff;
        buf[4] = (len >> 24) & 0xff;
        memcpy(buf+5,s,len);
    }
}


/* ######################################################################
 * [3] 이동 — 다음 원소로 가려면 현재 원소를 해석해야 한다
 *   배열처럼 i번째 주소를 바로 계산할 수 없습니다. 원소마다 길이가 다르기 때문입니다.
 *   그래서 인덱스 접근도, 찾기도 모두 앞(또는 뒤)에서부터 걸어갑니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [3-1] lpCurrentEncodedSizeUnsafe — listpack.c L461~L480
 *   첫 바이트의 비트 모양으로 원소 종류와 길이를 알아냅니다.
 * ====================================================================== */
/* Return the encoded length of the listpack element pointed by 'p'.
 * This includes the encoding byte, length bytes, and the element data itself.
 * If the element encoding is wrong then 0 is returned.
 * Note that this method may access additional bytes (in case of 12 and 32 bit
 * str), so should only be called when we know 'p' was already validated by
 * lpCurrentEncodedSizeBytes or ASSERT_INTEGRITY_LEN (possibly since 'p' is
 * a return value of another function that validated its return. */
static inline uint32_t lpCurrentEncodedSizeUnsafe(unsigned char *p) {
    if (LP_ENCODING_IS_7BIT_UINT(p[0])) return 1;
    if (LP_ENCODING_IS_6BIT_STR(p[0])) return 1+LP_ENCODING_6BIT_STR_LEN(p);
    if (LP_ENCODING_IS_13BIT_INT(p[0])) return 2;
    if (LP_ENCODING_IS_16BIT_INT(p[0])) return 3;
    if (LP_ENCODING_IS_24BIT_INT(p[0])) return 4;
    if (LP_ENCODING_IS_32BIT_INT(p[0])) return 5;
    if (LP_ENCODING_IS_64BIT_INT(p[0])) return 9;
    if (LP_ENCODING_IS_12BIT_STR(p[0])) return 2+LP_ENCODING_12BIT_STR_LEN(p);
    if (LP_ENCODING_IS_32BIT_STR(p[0])) return 5+LP_ENCODING_32BIT_STR_LEN(p);
    if (p[0] == LP_EOF) return 1;
    return 0;
}

/* ======================================================================
 * ▶ [3-2] lpSkip, lpNext — listpack.c L500~L529
 * ====================================================================== */
/* Skip the current entry returning the next. It is invalid to call this
 * function if the current element is the EOF element at the end of the
 * listpack, however, while this function is used to implement lpNext(),
 * it does not return NULL when the EOF element is encountered. */
static inline unsigned char *lpSkip(unsigned char *p) {
    unsigned long entrylen = lpCurrentEncodedSizeUnsafe(p);  /* ▶ 현재 원소의 encoding + 데이터 길이 */
    entrylen += lpEncodeBacklenBytes(entrylen);  /* ▶ + backlen 길이 */
    p += entrylen;  /* ▶ 그만큼 건너뛰면 다음 원소 */
    return p;
}
/* ▶ [생략] 원본 L510~L518: lpNextWithBytes (lpNext와 같고 전체 크기를 인자로 받음) */

/* If 'p' points to an element of the listpack, calling lpNext() will return
 * the pointer to the next element (the one on the right), or NULL if 'p'
 * already pointed to the last element of the listpack. */
unsigned char *lpNext(unsigned char *lp, unsigned char *p) {
    assert(p);
    p = lpSkip(p);
    if (p[0] == LP_EOF) return NULL;
    lpAssertValidEntry(lp, lpBytes(lp), p);  /* ▶ 항상 켜진 무결성 검사 (헤더에서 전체 크기를 읽음. 이 assert는 릴리스 빌드에서도 실행됨) */
    return p;
}

/* ======================================================================
 * ▶ [3-3] lpSkipPrev, lpPrev — listpack.c L555~L574
 * ====================================================================== */
/* Step back to the start of the previous entry, without validating it. Caller
 * must ensure 'p' is not the first entry. */
static inline unsigned char *lpSkipPrev(unsigned char *p) {
    p--; /* Seek the last backlen byte of the previous element. */
    uint64_t prevlen = lpDecodeBacklen(p);  /* ▶ ★ 이전 원소의 backlen을 거꾸로 읽음 */
    prevlen += lpEncodeBacklenBytes(prevlen);
    p -= prevlen-1; /* Seek the first byte of the previous entry. */  /* ▶ 그 길이만큼 앞으로 가면 이전 원소의 시작 */
    return p;
}

/* If 'p' points to an element of the listpack, calling lpPrev() will return
 * the pointer to the previous element (the one on the left), or NULL if 'p'
 * already pointed to the first element of the listpack. */
unsigned char *lpPrev(unsigned char *lp, unsigned char *p) {
    assert(p);
    if (p-lp == LP_HDR_SIZE) return NULL;
    p = lpSkipPrev(p);
    lpAssertValidEntry(lp, lpBytes(lp), p);
    return p;
}

/* ======================================================================
 * ▶ [3-4] lpFirst, lpLast — listpack.c L598~L612
 * ====================================================================== */
/* Return a pointer to the first element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpFirst(unsigned char *lp) {
    unsigned char *p = lp + LP_HDR_SIZE; /* Skip the header. */  /* ▶ 헤더 바로 뒤 */
    if (p[0] == LP_EOF) return NULL;
    lpAssertValidEntry(lp, lpBytes(lp), p);
    return p;
}

/* Return a pointer to the last element of the listpack, or NULL if the
 * listpack has no elements. */
unsigned char *lpLast(unsigned char *lp) {
    unsigned char *p = lp+lpGetTotalBytes(lp)-1; /* Seek EOF element. */  /* ▶ total_bytes로 EOF 위치를 바로 계산 → 거꾸로 한 칸. 끝 쪽 접근이 O(1)인 이유 */
    return lpPrev(lp,p); /* Will return NULL if EOF is the only element. */
}

/* ======================================================================
 * ▶ [3-5] lpLength — listpack.c L614~L636
 * ====================================================================== */
/* Return the number of elements inside the listpack. This function attempts
 * to use the cached value when within range, otherwise a full scan is
 * needed. As a side effect of calling this function, the listpack header
 * could be modified, because if the count is found to be already within
 * the 'numele' header field range, the new value is set. */
unsigned long lpLength(unsigned char *lp) {
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) return numele;  /* ▶ ★ 보통은 헤더 값을 바로 반환 O(1) */

    /* Too many elements inside the listpack. We need to scan in order
     * to get the total number. */
    uint32_t count = 0;
    unsigned char *p = lpFirst(lp);
    while(p) {  /* ▶ 65535개 이상일 때만 전부 셈 O(N) */
        count++;
        p = lpNext(lp,p);
    }

    /* If the count is again within range of the header numele field,
     * set it. */
    if (count < LP_HDR_NUMELE_UNKNOWN) lpSetNumElements(lp,count);
    return count;
}


/* ######################################################################
 * [4] lpFind — listpack 조회가 O(N)인 이유
 *   HGET(listpack), SISMEMBER(listpack), ZSCORE(listpack)가 모두 이 함수로 처음부터 훑습니다.
 *   Hash에서는 skip=1로 불러서 [필드][값][필드][값]의 값 칸을 건너뛰며 필드만 비교합니다
 *   (hashset/t_hash_발췌.c [6-3] hashTypeGetFromListpack).
 *   원소 수가 기준값(Hash 512개) 이하라 길어야 수백 번 비교이고, 데이터가 연속이라 캐시에 잘 맞습니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [4-1] lpFindCbInternal — listpack.c L889~L940
 * ====================================================================== */
/* Find pointer to the entry with a comparator callback.
 *
 * 'cmp' is a comparator callback. If it returns zero, current entry pointer
 * will be returned. 'user' is passed to this callback.
 * Skip 'skip' entries between every comparison.
 * Returns NULL when the field could not be found. */
static inline unsigned char *lpFindCbInternal(unsigned char *lp, unsigned char *p,
                                              void *user, lpCmp cmp, unsigned int skip)
{
    int skipcnt = 0;
    unsigned char *value;
    int64_t ll;
    uint64_t entry_size = 123456789; /* initialized to avoid warning. */
    uint32_t lp_bytes = lpBytes(lp);

    if (!p)
        p = lpFirst(lp);

    while (p) {
        if (skipcnt == 0) {
            value = lpGetWithSize(p, &ll, NULL, &entry_size);  /* ▶ 원소를 해석해 문자열이면 포인터, 정수면 값을 얻음 */
            if (value) {
                /* check the value doesn't reach outside the listpack before accessing it */
                assert(p >= lp + LP_HDR_SIZE && p + entry_size < lp + lp_bytes);
            }

            if (unlikely(cmp(lp, p, user, value, ll) == 0))  /* ▶ 비교 (lpFindCmp) */
                return p;

            /* Reset skip count */
            skipcnt = skip;  /* ▶ skip개를 건너뛸 준비 (Hash면 다음 값 칸) */
            p += entry_size;  /* ▶ 다음 원소 */
        } else {
            /* Skip entry */
            skipcnt--;

            /* Move to next entry, avoid use `lpNext` due to `lpAssertValidEntry` in
            * `lpNext` will call `lpBytes`, will cause performance degradation */
            p = lpSkip(p);  /* ▶ 건너뛸 원소는 해석 없이 길이만 보고 넘어감 */
        }

        /* The next call to lpGetWithSize could read at most 8 bytes past `p`
         * We use the slower validation call only when necessary. */
        if (p + 8 >= lp + lp_bytes)
            lpAssertValidEntry(lp, lp_bytes, p);
        else
            assert(p >= lp + LP_HDR_SIZE && p < lp + lp_bytes);
        if (p[0] == LP_EOF) break;  /* ▶ EOF면 못 찾음 → 끝까지 다 봤다는 뜻. 없는 필드 조회가 가장 비쌈 */
    }

    return NULL;
}

/* ======================================================================
 * ▶ [4-2] lpFindArg, lpFindCmp — listpack.c L948~L990
 * ====================================================================== */
struct lpFindArg {
    unsigned char *s; /* Item to search */
    uint32_t slen;    /* Item len */
    int vencoding;
    int64_t vll;
};

/* Comparator function to find item */
static inline int lpFindCmp(const unsigned char *lp, unsigned char *p,
                            void *user, unsigned char *s, long long slen) {
    (void) lp;
    (void) p;
    struct lpFindArg *arg = user;

    if (s) {
        if (slen == arg->slen && memcmp(arg->s, s, slen) == 0) {  /* ▶ ★ 길이가 같을 때만 memcmp. 길이가 다르면 바로 다음 */
            return 0;
        }
    } else {
        /* Find out if the searched field can be encoded. Note that
         * we do it only the first time, once done vencoding is set
         * to non-zero and vll is set to the integer value. */
        if (arg->vencoding == 0) {
            /* If the entry can be encoded as integer we set it to
             * 1, else set it to UCHAR_MAX, so that we don't retry
             * again the next time. */
            if (arg->slen >= 32 || arg->slen == 0 || !lpStringToInt64((const char*)arg->s, arg->slen, &arg->vll)) {  /* ▶ 찾는 값이 정수로 바뀌는지는 처음 한 번만 확인 */
                arg->vencoding = UCHAR_MAX;
            } else {
                arg->vencoding = 1;
            }
        }

        /* Compare current entry with specified entry, do it only
         * if vencoding != UCHAR_MAX because if there is no encoding
         * possible for the field it can't be a valid integer. */
        if (arg->vencoding != UCHAR_MAX && slen == arg->vll) {  /* ▶ 정수 원소는 정수끼리 비교 */
            return 0;
        }
    }

    return 1;
}

/* ======================================================================
 * ▶ [4-3] lpFind — listpack.c L992~L1002
 * ====================================================================== */
/* Find pointer to the entry equal to the specified entry. Skip 'skip' entries
 * between every comparison. Returns NULL when the field could not be found. */
unsigned char *lpFind(unsigned char *lp, unsigned char *p, unsigned char *s,
                      uint32_t slen, unsigned int skip)
{
    struct lpFindArg arg = {
        .s = s,
        .slen = slen
    };
    return lpFindCbInternal(lp, p, &arg, lpFindCmp, skip);
}


/* ######################################################################
 * [5] lpInsert — 삽입, 삭제, 교체가 모두 이 함수
 *   연속 메모리라 중간에 넣으려면 뒤쪽 바이트를 전부 밀어야 하고(memmove), 지우면 당겨야 합니다.
 *   listpack이 커질수록 쓰기 한 번의 비용이 전체 바이트 수 B에 비례합니다.
 *   lpAppend(끝에 추가)도 이 함수를 부르지만 옮길 바이트가 EOF 1바이트뿐이라 싸고,
 *   비싼 것은 realloc이 블록을 다른 곳으로 옮겨야 할 때입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [5-1] lpInsert — listpack.c L1004~L1179
 * ====================================================================== */
/* Insert, delete or replace the specified string element 'elestr' of length
 * 'size' or integer element 'eleint' at the specified position 'p', with 'p'
 * being a listpack element pointer obtained with lpFirst(), lpLast(), lpNext(),
 * lpPrev() or lpSeek().
 *
 * The element is inserted before, after, or replaces the element pointed
 * by 'p' depending on the 'where' argument, that can be LP_BEFORE, LP_AFTER
 * or LP_REPLACE.
 * 
 * If both 'elestr' and `eleint` are NULL, the function removes the element
 * pointed by 'p' instead of inserting one.
 * If `eleint` is non-NULL, 'size' is the length of 'eleint', the function insert
 * or replace with a 64 bit integer, which is stored in the 'eleint' buffer.
 * If 'elestr` is non-NULL, 'size' is the length of 'elestr', the function insert
 * or replace with a string, which is stored in the 'elestr' buffer.
 * 
 * Returns NULL on out of memory or when the listpack total length would exceed
 * the max allowed size of 2^32-1, otherwise the new pointer to the listpack
 * holding the new element is returned (and the old pointer passed is no longer
 * considered valid)
 *
 * If 'newp' is not NULL, at the end of a successful call '*newp' will be set
 * to the address of the element just added, so that it will be possible to
 * continue an interaction with lpNext() and lpPrev().
 *
 * For deletion operations (both 'elestr' and 'eleint' set to NULL) 'newp' is
 * set to the next element, on the right of the deleted one, or to NULL if the
 * deleted element was the last one. */
unsigned char *lpInsert(unsigned char *lp, unsigned char *elestr, unsigned char *eleint,
                        uint32_t size, unsigned char *p, int where, unsigned char **newp)
{
    unsigned char intenc[LP_MAX_INT_ENCODING_LEN];
    unsigned char backlen[LP_MAX_BACKLEN_SIZE];

    uint64_t enclen; /* The length of the encoded element. */
    int delete = (elestr == NULL && eleint == NULL);  /* ▶ 삭제 = 길이 0인 원소로 교체 */

    /* when deletion, it is conceptually replacing the element with a
     * zero-length element. So whatever we get passed as 'where', set
     * it to LP_REPLACE. */
    if (delete) where = LP_REPLACE;

    /* If we need to insert after the current element, we just jump to the
     * next element (that could be the EOF one) and handle the case of
     * inserting before. So the function will actually deal with just two
     * cases: LP_BEFORE and LP_REPLACE. */
    if (where == LP_AFTER) {
        p = lpSkip(p);
        where = LP_BEFORE;
        ASSERT_INTEGRITY(lp, p);
    }

    /* Store the offset of the element 'p', so that we can obtain its
     * address again after a reallocation. */
    unsigned long poff = p-lp;  /* ▶ realloc 뒤에 주소가 바뀔 수 있으므로 오프셋으로 기억 */

    int enctype;
    if (elestr) {
        /* Calling lpEncodeGetType() results into the encoded version of the
        * element to be stored into 'intenc' in case it is representable as
        * an integer: in that case, the function returns LP_ENCODING_INT.
        * Otherwise if LP_ENCODING_STR is returned, we'll have to call
        * lpEncodeString() to actually write the encoded string on place later.
        *
        * Whatever the returned encoding is, 'enclen' is populated with the
        * length of the encoded element. */
        enctype = lpEncodeGetType(elestr,size,intenc,&enclen);  /* ▶ 정수로 저장할지, 문자열로 저장할지, 몇 바이트인지 계산 */
        if (enctype == LP_ENCODING_INT) eleint = intenc;
    } else if (eleint) {
        enctype = LP_ENCODING_INT;
        enclen = size; /* 'size' is the length of the encoded integer element. */
    } else {
        enctype = -1;
        enclen = 0;
    }

    /* We need to also encode the backward-parsable length of the element
     * and append it to the end: this allows to traverse the listpack from
     * the end to the start. */
    unsigned long backlen_size = (!delete) ? lpEncodeBacklen(backlen,enclen) : 0;  /* ▶ 원소 뒤에 붙일 backlen */
    uint64_t old_listpack_bytes = lpGetTotalBytes(lp);
    uint32_t replaced_len  = 0;
    if (where == LP_REPLACE) {
        replaced_len = lpCurrentEncodedSizeUnsafe(p);
        replaced_len += lpEncodeBacklenBytes(replaced_len);
        ASSERT_INTEGRITY_LEN(lp, p, replaced_len);
    }

    uint64_t new_listpack_bytes = old_listpack_bytes + enclen + backlen_size  /* ▶ 새 전체 크기 */
                                  - replaced_len;
    if (new_listpack_bytes > UINT32_MAX) return NULL;  /* ▶ 32비트 헤더에 못 담으면 실패 */

    /* We now need to reallocate in order to make space or shrink the
     * allocation (in case 'when' value is LP_REPLACE and the new element is
     * smaller). However we do that before memmoving the memory to
     * make room for the new element if the final allocation will get
     * larger, or we do it after if the final allocation will get smaller. */

    unsigned char *dst = lp + poff; /* May be updated after reallocation. */

    /* Realloc before: we need more room. */
    if (new_listpack_bytes > old_listpack_bytes &&
        new_listpack_bytes > lp_malloc_size(lp)) {  /* ▶ ★ jemalloc이 실제로 준 크기(usable size) 안이면 realloc을 건너뜀 */
        if ((lp = lp_realloc(lp,new_listpack_bytes)) == NULL) return NULL;  /* ▶ ★ realloc. 제자리에서 못 늘리면 새 블록에 통째로 복사 O(B) */
        dst = lp + poff;
    }

    /* Setup the listpack relocating the elements to make the exact room
     * we need to store the new one. */
    if (where == LP_BEFORE) {
        memmove(dst+enclen+backlen_size,dst,old_listpack_bytes-poff);  /* ▶ ★ 삽입 지점 뒤쪽 전체를 뒤로 밀기 O(B - poff) */
    } else { /* LP_REPLACE. */
        memmove(dst+enclen+backlen_size,  /* ▶ 교체/삭제: 뒤쪽 전체를 당기기 */
                dst+replaced_len,
                old_listpack_bytes-poff-replaced_len);
    }

    /* Realloc after: we need to free space. */
    if (new_listpack_bytes < old_listpack_bytes) {
        if ((lp = lp_realloc(lp,new_listpack_bytes)) == NULL) return NULL;  /* ▶ 줄어들면 memmove 뒤에 realloc으로 줄임 */
        dst = lp + poff;
    }

    /* Store the entry. */
    if (newp) {
        *newp = dst;
        /* In case of deletion, set 'newp' to NULL if the next element is
         * the EOF element. */
        if (delete && dst[0] == LP_EOF) *newp = NULL;
    }
    if (!delete) {
        if (enctype == LP_ENCODING_INT) {
            memcpy(dst,eleint,enclen);
        } else if (elestr) {
            lpEncodeString(dst,elestr,size);
        } else {
            redis_unreachable();
        }
        dst += enclen;
        memcpy(dst,backlen,backlen_size);
        dst += backlen_size;
    }

    /* Update header. */
    if (where != LP_REPLACE || delete) {
        uint32_t num_elements = lpGetNumElements(lp);
        if (num_elements != LP_HDR_NUMELE_UNKNOWN) {
            if (!delete)
                lpSetNumElements(lp,num_elements+1);  /* ▶ 원소 수 +1 (65535 미만일 때만 관리) */
            else
                lpSetNumElements(lp,num_elements-1);
        }
    }
    lpSetTotalBytes(lp,new_listpack_bytes);  /* ▶ 헤더의 전체 바이트 수 갱신 */

/* ▶ [생략] 원본 L1159~L1176: 디버그용 코드 (#if 0으로 꺼져 있음) */

    return lp;
}

/* ======================================================================
 * ▶ [5-2] lpAppend — listpack.c L1398~L1405
 * ====================================================================== */
/* Append the specified element 'ele' of length 'size' at the end of the
 * listpack. It is implemented in terms of lpInsert(), so the return value is
 * the same as lpInsert(). */
unsigned char *lpAppend(unsigned char *lp, unsigned char *ele, uint32_t size) {
    uint64_t listpack_bytes = lpGetTotalBytes(lp);
    unsigned char *eofptr = lp + listpack_bytes - 1;
    return lpInsert(lp,ele,NULL,size,eofptr,LP_BEFORE,NULL);  /* ▶ EOF 앞에 삽입 = 끝에 추가. 밀어야 할 바이트는 EOF 1바이트뿐 */
}

/* ======================================================================
 * ▶ [5-3] lpDelete — listpack.c L1443~L1449
 * ====================================================================== */
/* Remove the element pointed by 'p', and return the resulting listpack.
 * If 'newp' is not NULL, the next element pointer (to the right of the
 * deleted one) is returned by reference. If the deleted element was the
 * last one, '*newp' is set to NULL. */
unsigned char *lpDelete(unsigned char *lp, unsigned char *p, unsigned char **newp) {
    return lpInsert(lp,NULL,NULL,0,p,LP_REPLACE,newp);  /* ▶ 원소 하나 삭제. 뒤쪽을 당기므로 앞쪽 원소를 지울수록 비쌈 */
}


/* ######################################################################
 * [6] lpSeek — 인덱스로 찾기
 *   LINDEX, LSET, LRANGE의 시작점 찾기가 listpack일 때 이 함수를 씁니다. 원소 수를 알면 가까운 끝에서
 *   출발하므로 최대 N/2번 이동입니다.
 * ###################################################################### */

/* ======================================================================
 * ▶ [6-1] lpSeek — listpack.c L1689~L1725
 * ====================================================================== */
/* Seek the specified element and returns the pointer to the seeked element.
 * Positive indexes specify the zero-based element to seek from the head to
 * the tail, negative indexes specify elements starting from the tail, where
 * -1 means the last element, -2 the penultimate and so forth. If the index
 * is out of range, NULL is returned. */
unsigned char *lpSeek(unsigned char *lp, long index) {
    int forward = 1; /* Seek forward by default. */

    /* We want to seek from left to right or the other way around
     * depending on the listpack length and the element position.
     * However if the listpack length cannot be obtained in constant time,
     * we always seek from left to right. */
    uint32_t numele = lpGetNumElements(lp);
    if (numele != LP_HDR_NUMELE_UNKNOWN) {
        if (index < 0) index = (long)numele+index;
        if (index < 0) return NULL; /* Index still < 0 means out of range. */
        if (index >= (long)numele) return NULL; /* Out of range the other side. */
        /* We want to scan right-to-left if the element we are looking for
         * is past the half of the listpack. */
        if (index > (long)numele/2) {  /* ▶ ★ 뒤쪽 절반이면 끝에서부터 거꾸로 */
            forward = 0;
            index = (long)numele - index - 1; /* Backward steps from the last entry. */
        }
    } else {
        /* If the listpack length is unspecified, for negative indexes we
         * want to always scan right-to-left. */
        if (index < 0) {
            forward = 0;
            index = -index - 1; /* Backward steps; -1 is the last entry. */
        }
    }

    if (forward)
        return lpNextN(lp, lpFirst(lp), index);  /* ▶ 앞에서 index번 이동 O(index) */
    else
        return lpPrevN(lp, lpLast(lp), index);
}


/* ######################################################################
 * [부록] listpack_malloc.h L25~L28
 *   listpack의 할당 함수는 Redis 할당기(zmalloc, 기본 jemalloc)를 그대로 씁니다.
 *   lp_malloc_size는 "요청한 크기"가 아니라 jemalloc이 실제로 준 크기입니다.
 *   jemalloc은 크기를 등급(size class)으로 올려서 주므로, 끝에 조금씩 붙이는 동안은
 *   남는 공간을 쓰다가 등급을 넘을 때만 realloc합니다 (lpInsert L1106).
 * ###################################################################### */
#define lp_malloc(sz) zmalloc_usable(sz,NULL)
#define lp_realloc(ptr,sz) zrealloc_usable(ptr,sz,NULL,NULL)
#define lp_free zfree
#define lp_malloc_size zmalloc_usable_size
