/* fdt.c - 플랫 디바이스 트리 읽기와 /chosen/bootargs 갈아끼우기.
 *
 * ── 형식 ─────────────────────────────────────────────────────────
 * DTB 는 세 덩어리로 되어 있다.
 *
 *   헤더        40바이트. 나머지 덩어리들이 어디 있는지가 적혀 있다
 *   메모리 예약  커널이 쓰면 안 되는 영역 목록
 *   구조 블록    토큰의 나열. 트리 모양이 여기 들어 있다
 *   문자열 블록  속성 이름들. 구조 블록은 여기의 오프셋으로 이름을 가리킨다
 *
 * 구조 블록의 토큰은 다섯 가지뿐이다.
 *
 *   BEGIN_NODE(1)  노드 시작. 뒤에 이름이 널 종료 + 4바이트 정렬로 붙는다
 *   END_NODE(2)    노드 끝
 *   PROP(3)        속성. 뒤에 길이(4), 이름 오프셋(4), 값(4바이트 정렬)
 *   NOP(4)         무시. 제자리 편집의 흔적으로 남는다
 *   END(9)         구조 블록 끝
 *
 * 전부 빅엔디언이다. ARM 은 리틀엔디언으로 도니까 읽을 때마다 뒤집는다.
 * 이게 이 형식에서 가장 흔한 실수라, 바이트 단위로 조립하는 함수를
 * 하나 두고 그것만 쓴다.
 */
#include "fdt.h"
#include "printf.h"
#include "string.h"
#include "types.h"

#define FDT_BEGIN_NODE  1u
#define FDT_END_NODE    2u
#define FDT_PROP        3u
#define FDT_NOP         4u
#define FDT_END         9u

/* 빅엔디언 32비트를 정렬 없이 읽고 쓴다. */
static u32 be32(const void *p)
{
    const u8 *b = (const u8 *)p;
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
}

static void put_be32(void *p, u32 v)
{
    u8 *b = (u8 *)p;
    b[0] = (u8)(v >> 24); b[1] = (u8)(v >> 16);
    b[2] = (u8)(v >> 8);  b[3] = (u8)v;
}

/* 헤더 필드 위치 (바이트 오프셋) */
#define H_MAGIC         0
#define H_TOTALSIZE     4
#define H_OFF_STRUCT    8
#define H_OFF_STRINGS   12
#define H_OFF_RSVMAP    16
#define H_VERSION       20
#define H_LAST_COMP     24
#define H_BOOT_CPUID    28
#define H_SIZE_STRINGS  32
#define H_SIZE_STRUCT   36
#define FDT_HEADER_SIZE 40

static u32 align4(u32 n) { return (n + 3u) & ~3u; }

bool fdt_valid(const void *fdt)
{
    if (!fdt)
        return false;
    const u8 *p = (const u8 *)fdt;

    if (be32(p + H_MAGIC) != FDT_MAGIC)
        return false;

    /* 버전 17 이 지금 쓰이는 형식이다. 그보다 낮은 것을 읽으려 하면
     * 구조 블록의 배치가 달라 조용히 엉뚱한 것을 읽게 된다. */
    if (be32(p + H_LAST_COMP) > 17)
        return false;

    u32 total = be32(p + H_TOTALSIZE);
    if (total < FDT_HEADER_SIZE || total > (16u << 20))
        return false;

    /* 덩어리들이 전체 크기 안에 들어 있는지. 여기서 걸러내지 않으면
     * 뒤의 순회가 남의 메모리를 읽는다. */
    u32 so = be32(p + H_OFF_STRUCT), ss = be32(p + H_SIZE_STRUCT);
    u32 to = be32(p + H_OFF_STRINGS), ts = be32(p + H_SIZE_STRINGS);
    if (so + ss > total || to + ts > total)
        return false;

    return true;
}

u32 fdt_size(const void *fdt)
{
    return fdt_valid(fdt) ? be32((const u8 *)fdt + H_TOTALSIZE) : 0;
}

/* ── 문자열 블록에서 이름 찾기 ───────────────────────────────────*/

static const char *string_at(const u8 *fdt, u32 off)
{
    u32 to = be32(fdt + H_OFF_STRINGS);
    return (const char *)(fdt + to + off);
}

/* 이름이 이미 문자열 블록에 있으면 그 오프셋을, 없으면 0xFFFFFFFF. */
static u32 find_string(const u8 *fdt, const char *name)
{
    u32 to = be32(fdt + H_OFF_STRINGS);
    u32 ts = be32(fdt + H_SIZE_STRINGS);
    u32 i = 0;

    while (i < ts) {
        const char *s = (const char *)(fdt + to + i);
        if (strcmp(s, name) == 0)
            return i;
        i += (u32)strlen(s) + 1;
    }
    return 0xFFFFFFFFu;
}

/* ── 읽기 ────────────────────────────────────────────────────────*/

/* /chosen/bootargs 를 찾는다. 없으면 NULL. */
static const char *find_bootargs(const u8 *fdt)
{
    u32 off = be32(fdt + H_OFF_STRUCT);
    u32 end = off + be32(fdt + H_SIZE_STRUCT);
    int depth = 0;
    bool in_chosen = false;

    while (off < end) {
        u32 tok = be32(fdt + off);
        off += 4;

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(fdt + off);
            depth++;
            /* 루트 노드의 이름은 빈 문자열이고 그것이 깊이 1 이다.
             * 그래서 /chosen 은 깊이 2 에 있다. 여기를 1 로 썼더니
             * chosen 을 영영 못 찾고 - 없는 줄 알고 하나 더 만들어
             * 넣었다. 커널은 진짜 chosen 을 읽으므로 커맨드라인이
             * 조용히 빈 채로 넘어갔다. */
            if (depth == 2 && strcmp(name, "chosen") == 0)
                in_chosen = true;
            off += align4((u32)strlen(name) + 1);
        } else if (tok == FDT_END_NODE) {
            if (in_chosen && depth == 2)
                in_chosen = false;
            depth--;
        } else if (tok == FDT_PROP) {
            u32 len     = be32(fdt + off);
            u32 nameoff = be32(fdt + off + 4);
            const char *pname = string_at(fdt, nameoff);
            const char *value = (const char *)(fdt + off + 8);
            off += 8 + align4(len);
            if (in_chosen && strcmp(pname, "bootargs") == 0 && len > 0)
                return value;
        } else if (tok == FDT_NOP) {
            /* 아무것도 아님 */
        } else {
            break;                      /* FDT_END 또는 깨진 토큰 */
        }
    }
    return NULL;
}

void fdt_describe(const void *fdt)
{
    if (!fdt_valid(fdt)) {
        kprintf("  쓸 만한 디바이스 트리가 아니다\n");
        return;
    }
    const u8 *p = (const u8 *)fdt;
    kprintf("  DTB %u 바이트, 버전 %u\n",
            be32(p + H_TOTALSIZE), be32(p + H_VERSION));

    const char *args = find_bootargs(p);
    if (args)
        kprintf("  커맨드라인: %s\n", args);
    else
        kprintf("  커맨드라인이 비어 있다\n");
}

/* ── 다시 쓰기 ───────────────────────────────────────────────────*/

/* 쓰는 쪽의 상태. 용량을 넘기면 그 자리에서 멈추고 실패로 남긴다 -
 * 넘겨 쓴 DTB 를 커널에 주는 것보다 부팅을 포기하는 편이 낫다. */
typedef struct {
    u8 *base;
    u32 cap;
    u32 pos;
    bool overflow;
} wbuf;

static void w_bytes(wbuf *w, const void *src, u32 n)
{
    if (w->pos + n > w->cap) { w->overflow = true; return; }
    memcpy(w->base + w->pos, src, n);
    w->pos += n;
}

static void w_u32(wbuf *w, u32 v)
{
    if (w->pos + 4 > w->cap) { w->overflow = true; return; }
    put_be32(w->base + w->pos, v);
    w->pos += 4;
}

static void w_pad4(wbuf *w)
{
    while (w->pos & 3) {
        if (w->pos >= w->cap) { w->overflow = true; return; }
        w->base[w->pos++] = 0;
    }
}

/* 속성 하나를 쓴다. */
static void w_prop(wbuf *w, u32 nameoff, const void *val, u32 len)
{
    w_u32(w, FDT_PROP);
    w_u32(w, len);
    w_u32(w, nameoff);
    w_bytes(w, val, len);
    w_pad4(w);
}

/* 루트의 #address-cells / #size-cells 를 읽는다.
 *
 * /memory 의 reg 를 몇 바이트로 써야 하는지가 여기서 정해진다. 라즈베리
 * 파이는 1/1(각 32비트)이지만 표준은 2/2(각 64비트)도 허용하고, 숫자를
 * 박아두면 다른 보드에서 메모리 크기를 엉뚱하게 쓰게 된다. */
static void root_cells(const u8 *fdt, u32 *addr_cells, u32 *size_cells)
{
    *addr_cells = 2;                    /* 규격의 기본값 */
    *size_cells = 1;

    u32 off = be32(fdt + H_OFF_STRUCT);
    u32 end = off + be32(fdt + H_SIZE_STRUCT);
    int depth = 0;

    while (off < end) {
        u32 tok = be32(fdt + off);
        off += 4;

        if (tok == FDT_BEGIN_NODE) {
            depth++;
            if (depth > 1)
                return;                 /* 루트를 지나쳤다 */
            off += align4((u32)strlen((const char *)(fdt + off)) + 1);
        } else if (tok == FDT_PROP) {
            u32 len     = be32(fdt + off);
            u32 nameoff = be32(fdt + off + 4);
            const char *pname = string_at(fdt, nameoff);
            const u8 *val = fdt + off + 8;
            if (depth == 1 && len == 4) {
                if (strcmp(pname, "#address-cells") == 0) *addr_cells = be32(val);
                if (strcmp(pname, "#size-cells") == 0)    *size_cells = be32(val);
            }
            off += 8 + align4(len);
        } else if (tok == FDT_END_NODE) {
            return;
        } else if (tok == FDT_NOP) {
            /* 넘어간다 */
        } else {
            return;
        }
    }
}

/* 셀 개수에 맞춰 숫자 하나를 쓴다. */
static u32 write_cells(u8 *out, u64 v, u32 cells)
{
    if (cells >= 2) {
        put_be32(out,     (u32)(v >> 32));
        put_be32(out + 4, (u32)v);
        return 8;
    }
    put_be32(out, (u32)v);
    return 4;
}

/* 노드 이름이 memory 인가. "memory@0" 처럼 주소가 붙어 있다. */
static bool is_memory_node(const char *name)
{
    if (strcmp(name, "memory") == 0)
        return true;
    return name[0] == 'm' && name[1] == 'e' && name[2] == 'm' &&
           name[3] == 'o' && name[4] == 'r' && name[5] == 'y' &&
           name[6] == '@';
}

bool fdt_copy_with_fixups(const void *src, void *dst, u32 capacity,
                          const fdt_fixup_t *fix, u32 *out_size)
{
    if (!fdt_valid(src) || !fix)
        return false;
    const char *args = fix->bootargs;

    const u8 *in = (const u8 *)src;
    u8 *out = (u8 *)dst;

    u32 addr_cells, size_cells;
    root_cells(in, &addr_cells, &size_cells);

    u32 in_struct  = be32(in + H_OFF_STRUCT);
    u32 in_ssize   = be32(in + H_SIZE_STRUCT);
    u32 in_strings = be32(in + H_OFF_STRINGS);
    u32 in_tsize   = be32(in + H_SIZE_STRINGS);
    u32 in_rsvmap  = be32(in + H_OFF_RSVMAP);

    /* bootargs 라는 이름이 문자열 블록에 이미 있는가. 없으면 새 이름을
     * 블록 끝에 덧붙여야 하고, 그만큼 크기가 는다. */
    u32 args_nameoff = find_string(in, "bootargs");
    bool add_name = (args_nameoff == 0xFFFFFFFFu);
    if (add_name)
        args_nameoff = in_tsize;

    u32 args_len = args ? (u32)strlen(args) + 1 : 0;

    /* 메모리 예약 블록의 길이를 잰다. 8바이트 쌍이 0,0 으로 끝난다. */
    u32 rsv_len = 0;
    for (;;) {
        const u8 *e = in + in_rsvmap + rsv_len;
        u64 addr = ((u64)be32(e) << 32) | be32(e + 4);
        u64 size = ((u64)be32(e + 8) << 32) | be32(e + 12);
        rsv_len += 16;
        if (addr == 0 && size == 0)
            break;
        if (rsv_len > 4096)             /* 말이 안 되는 길이 */
            return false;
    }

    wbuf w = { out, capacity, 0, false };

    /* 헤더는 나중에 채운다. 구조/문자열 블록의 크기가 나와야 하기
     * 때문이다. 자리만 잡아두고 지나간다. */
    w.pos = FDT_HEADER_SIZE;
    w_pad4(&w);

    u32 out_rsvmap = w.pos;
    w_bytes(&w, in + in_rsvmap, rsv_len);

    /* 구조 블록을 8바이트 경계에 둔다. 규격이 요구하는 것은 4바이트
     * 지만, 예약 블록이 8바이트 단위라 8로 맞춰두면 어느 쪽으로도
     * 안전하다. */
    while (w.pos & 7) {
        if (w.pos >= w.cap) return false;
        out[w.pos++] = 0;
    }
    u32 out_struct = w.pos;

    /* ── 구조 블록을 옮겨 적는다 ──────────────────────────────── */
    u32 off = in_struct;
    u32 end = in_struct + in_ssize;
    int depth = 0;
    bool in_chosen = false;
    bool in_memory = false;
    bool wrote_args = false;
    bool saw_chosen = false;
    bool wrote_mem = false;

    while (off < end && !w.overflow) {
        u32 tok = be32(in + off);

        if (tok == FDT_BEGIN_NODE) {
            const char *name = (const char *)(in + off + 4);
            u32 namelen = (u32)strlen(name) + 1;

            depth++;
            if (depth == 2 && strcmp(name, "chosen") == 0) {
                in_chosen = true;
                saw_chosen = true;
            }
            if (depth == 2 && fix->mem_size && is_memory_node(name))
                in_memory = true;

            w_u32(&w, FDT_BEGIN_NODE);
            w_bytes(&w, name, namelen);
            w_pad4(&w);

            /* /chosen 을 열자마자 bootargs 를 쓴다. 속성은 자식 노드
             * 앞에 와야 하므로 여기가 유일하게 안전한 자리다. */
            if (in_chosen && args && !wrote_args) {
                w_prop(&w, args_nameoff, args, args_len);
                wrote_args = true;
            }

            off += 4 + align4(namelen);
            continue;
        }

        if (tok == FDT_PROP) {
            u32 len     = be32(in + off + 4);
            u32 nameoff = be32(in + off + 8);
            const u8 *val = in + off + 12;
            const char *pname = string_at(in, nameoff);

            /* 원래 있던 bootargs 는 버린다. 우리 것을 이미 썼다. */
            if (in_chosen && args && strcmp(pname, "bootargs") == 0) {
                off += 12 + align4(len);
                continue;
            }

            /* /memory 의 reg 를 진짜 값으로 갈아끼운다.
             *
             * 배포되는 라즈베리파이 .dtb 는 여기를 <0 0> 으로 비워두고
             * start.elf 가 채우는 것을 전제한다. 우리가 그 자리를
             * 대신하면서 이 줄을 안 고치면, 커널은 램이 0바이트인
             * 기계라고 믿고 페이지 테이블을 만들다 패닉한다. 로그의
             * 첫 줄은 "Failed to allocate page table page" 인데,
             * 그것만 보고 원인이 메모리 노드라고 짐작하기는 어렵다. */
            if (in_memory && strcmp(pname, "reg") == 0) {
                u8 regbuf[16];
                u32 n = write_cells(regbuf, fix->mem_base, addr_cells);
                n += write_cells(regbuf + n, fix->mem_size, size_cells);
                w_prop(&w, nameoff, regbuf, n);
                wrote_mem = true;
                off += 12 + align4(len);
                continue;
            }

            w_u32(&w, FDT_PROP);
            w_u32(&w, len);
            w_u32(&w, nameoff);
            w_bytes(&w, val, len);
            w_pad4(&w);

            off += 12 + align4(len);
            continue;
        }

        if (tok == FDT_END_NODE) {
            if (in_chosen && depth == 2)
                in_chosen = false;
            if (in_memory && depth == 2)
                in_memory = false;
            depth--;
            w_u32(&w, FDT_END_NODE);
            off += 4;
            continue;
        }

        if (tok == FDT_NOP) {
            off += 4;                   /* 옮겨 적지 않는다 - 자리 낭비다 */
            continue;
        }

        if (tok == FDT_END) {
            /* /chosen 이 아예 없었으면 루트 안에 만들어 넣는다.
             * 루트의 END_NODE 는 방금 지나왔으므로 되감아야 한다. */
            break;
        }

        return false;                   /* 모르는 토큰 - 깨진 DTB */
    }

    /* /chosen 이 없던 경우. 루트를 닫는 END_NODE 를 도로 지우고,
     * chosen 노드를 넣은 뒤 다시 닫는다. */
    if (args && !saw_chosen && !w.overflow) {
        if (w.pos >= out_struct + 4 &&
            be32(out + w.pos - 4) == FDT_END_NODE) {
            w.pos -= 4;                 /* 루트 닫기를 회수 */

            u32 chosen_name = find_string(in, "chosen");
            (void)chosen_name;          /* 노드 이름은 문자열 블록이 아니다 */

            w_u32(&w, FDT_BEGIN_NODE);
            static const char nm[] = "chosen";
            w_bytes(&w, nm, sizeof nm);
            w_pad4(&w);
            w_prop(&w, args_nameoff, args, args_len);
            w_u32(&w, FDT_END_NODE);

            w_u32(&w, FDT_END_NODE);    /* 루트를 다시 닫는다 */
            wrote_args = true;
        }
    }

    w_u32(&w, FDT_END);
    if (w.overflow)
        return false;

    u32 out_ssize = w.pos - out_struct;

    /* ── 문자열 블록 ──────────────────────────────────────────── */
    u32 out_strings = w.pos;
    w_bytes(&w, in + in_strings, in_tsize);
    if (add_name) {
        static const char nm[] = "bootargs";
        w_bytes(&w, nm, sizeof nm);
    }
    u32 out_tsize = w.pos - out_strings;

    if (w.overflow)
        return false;

    /* ── 헤더 ─────────────────────────────────────────────────── */
    put_be32(out + H_MAGIC,        FDT_MAGIC);
    put_be32(out + H_TOTALSIZE,    w.pos);
    put_be32(out + H_OFF_STRUCT,   out_struct);
    put_be32(out + H_OFF_STRINGS,  out_strings);
    put_be32(out + H_OFF_RSVMAP,   out_rsvmap);
    put_be32(out + H_VERSION,      17);
    put_be32(out + H_LAST_COMP,    16);
    put_be32(out + H_BOOT_CPUID,   be32(in + H_BOOT_CPUID));
    put_be32(out + H_SIZE_STRINGS, out_tsize);
    put_be32(out + H_SIZE_STRUCT,  out_ssize);

    if (args && !wrote_args)
        kprintf("[fdt] 경고: /chosen 을 만들지 못해 커맨드라인이 비었다\n");
    if (fix->mem_size && !wrote_mem)
        kprintf("[fdt] 경고: /memory 를 찾지 못했다 - 커널이 램 크기를"
                " 모른 채 뜬다\n");

    if (out_size)
        *out_size = w.pos;
    return true;
}
