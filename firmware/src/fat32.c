/* fat32.c - MBR 파티션 테이블과 FAT32, 읽기 전용.
 *
 * ── FAT 를 고른 것은 우리가 아니다 ───────────────────────────────
 * 라즈베리파이의 GPU 부트롬이 SD 카드의 첫 파티션을 FAT 로 읽어서
 * bootcode.bin 과 start.elf 를 찾는다. 그래서 부트 파티션은 반드시
 * FAT 이고, 그 위에 커널과 디바이스트리가 놓인다. 우리 부트로더가
 * 그것들을 찾으려면 같은 파일시스템을 읽을 수 있어야 한다.
 *
 * ── 클러스터 체인 ───────────────────────────────────────────────
 * FAT 는 파일을 클러스터 단위로 흩어 놓고, "이 다음은 어디" 를 별도의
 * 표(FAT)에 적어둔다. 파일을 읽는다는 것은 그 표를 따라 링크드 리스트를
 * 걷는 일이다. 표 항목이 32비트라 FAT32 다.
 *
 * 표를 한 섹터씩 그때그때 읽는다. 캐시를 두면 빨라지겠지만, 부트로더가
 * 읽는 것은 커널 하나와 디바이스트리 하나뿐이라 그 복잡도를 살 이유가
 * 없다.
 */
#include "fat32.h"
#include "emmc.h"
#include "printf.h"
#include "string.h"
#include "types.h"

/* 정렬되지 않은 리틀엔디언 읽기.
 *
 * 구조체를 얹어서 읽지 않는 이유가 있다. 디스크 구조는 정렬을
 * 신경쓰지 않고 만들어졌고 - BPB 의 32비트 필드가 홀수 오프셋에 있다 -
 * AArch64 에서 정렬 안 된 접근은 그 자리에서 예외다. 바이트로 조립하면
 * 그런 일이 없고, 엔디언도 명시적이 된다. */
static u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static u32 rd32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

/* ── 마운트 상태 ─────────────────────────────────────────────────*/
static struct {
    bool ready;
    u64  part_lba;          /* 파티션 시작 (카드 기준 블록) */
    u32  sectors_per_cluster;
    u32  fat_lba;           /* 첫 FAT 의 시작 */
    u32  fat_sectors;
    u32  data_lba;          /* 첫 클러스터(=2번)의 시작 */
    u32  root_cluster;
    u32  total_clusters;
} fs;

static u8 sector_buf[512];

static u32 cluster_lba(u32 cluster)
{
    return fs.data_lba + (cluster - 2) * fs.sectors_per_cluster;
}

/* FAT 표에서 다음 클러스터를 찾는다. 끝이면 0 을 돌려준다. */
static u32 next_cluster(u32 cluster)
{
    u32 offset = cluster * 4;
    u32 sector = fs.fat_lba + offset / 512;
    u32 within = offset % 512;

    if (!emmc_read(fs.part_lba + sector, 1, sector_buf))
        return 0;

    u32 v = rd32(&sector_buf[within]) & 0x0FFFFFFFu;

    /* 0x0FFFFFF8 이상은 "파일 끝". 0x0FFFFFF7 은 불량 클러스터인데,
     * 그것도 여기서는 끝으로 취급한다 - 불량 표시를 만나서 읽기를
     * 멈추는 것이 계속 따라가는 것보다 낫다. */
    if (v >= 0x0FFFFFF7u)
        return 0;
    if (v < 2 || (fs.total_clusters && v >= fs.total_clusters + 2))
        return 0;               /* 표가 깨졌다 */
    return v;
}

/* ── 마운트 ──────────────────────────────────────────────────────*/

/* BPB 하나를 검사하고 받아들인다. */
static bool try_bpb(u64 lba)
{
    if (!emmc_read(lba, 1, sector_buf))
        return false;

    /* 부트 시그니처. 이게 없으면 FAT 가 아니다. */
    if (rd16(&sector_buf[510]) != 0xAA55)
        return false;

    u32 bytes_per_sector = rd16(&sector_buf[0x0B]);
    u32 spc              = sector_buf[0x0D];
    u32 reserved         = rd16(&sector_buf[0x0E]);
    u32 num_fats         = sector_buf[0x10];
    u32 root_entries     = rd16(&sector_buf[0x11]);
    u32 fat_size_16      = rd16(&sector_buf[0x16]);
    u32 total_16         = rd16(&sector_buf[0x13]);
    u32 total_32         = rd32(&sector_buf[0x20]);
    u32 fat_size_32      = rd32(&sector_buf[0x24]);
    u32 root_cluster     = rd32(&sector_buf[0x2C]);

    /* 512바이트 섹터만 다룬다. 그 외는 SD 카드에서 사실상 없고,
     * 지원하는 척하다 조용히 틀리는 것보다 거절하는 편이 낫다. */
    if (bytes_per_sector != 512 || spc == 0 || num_fats == 0)
        return false;

    /* root_entries 가 0 이고 FAT 크기가 32비트 필드에 있으면 FAT32.
     * FAT12/16 은 루트 디렉터리 구조가 아예 달라서 지원하지 않는다. */
    if (root_entries != 0 || fat_size_16 != 0 || fat_size_32 == 0)
        return false;
    if (root_cluster < 2)
        return false;

    u32 total = total_32 ? total_32 : total_16;
    u32 data_start = reserved + num_fats * fat_size_32;
    if (total <= data_start)
        return false;

    fs.part_lba            = lba;
    fs.sectors_per_cluster = spc;
    fs.fat_lba             = reserved;
    fs.fat_sectors         = fat_size_32;
    fs.data_lba            = data_start;
    fs.root_cluster        = root_cluster;
    fs.total_clusters      = (total - data_start) / spc;
    fs.ready               = true;
    return true;
}

bool fat32_mount(void)
{
    memset(&fs, 0, sizeof fs);

    /* 먼저 MBR 을 본다. */
    if (!emmc_read(0, 1, sector_buf)) {
        kprintf("[fat] 0번 섹터를 읽지 못했다\n");
        return false;
    }

    if (rd16(&sector_buf[510]) == 0xAA55) {
        /* 파티션 항목 네 개를 훑는다. FAT 타입을 먼저 고르되,
         * 타입 바이트는 포맷한 도구가 적어둔 값일 뿐이라 최종 판단은
         * 실제 BPB 로 한다. */
        u8 table[64];
        memcpy(table, &sector_buf[0x1BE], sizeof table);

        for (int i = 0; i < 4; i++) {
            const u8 *e = &table[i * 16];
            u8  type  = e[4];
            u32 start = rd32(&e[8]);
            u32 count = rd32(&e[12]);

            if (start == 0 || count == 0)
                continue;

            bool looks_fat = (type == 0x0B || type == 0x0C ||   /* FAT32 */
                              type == 0x0E || type == 0x06 ||   /* FAT16 */
                              type == 0x01 || type == 0x04);
            if (!looks_fat)
                continue;

            if (try_bpb(start)) {
                kprintf("[fat] 파티션 %d 에서 FAT32 발견"
                        " (LBA %u, %u 섹터, 타입 %02x)\n",
                        i + 1, start, count, type);
                return true;
            }
        }
    }

    /* 파티션 테이블이 없거나 쓸 만한 항목이 없었다. 카드 전체가
     * 통째로 FAT 인 경우가 실제로 있다 - 특히 사람이 직접
     * mkfs.vfat /dev/sdX 를 한 카드가 그렇다. */
    if (try_bpb(0)) {
        kprintf("[fat] 파티션 테이블 없이 카드 전체가 FAT32 다\n");
        return true;
    }

    kprintf("[fat] FAT32 파티션을 찾지 못했다\n");
    return false;
}

/* ── 이름 맞추기 ─────────────────────────────────────────────────*/

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

static bool name_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (upper(*a) != upper(*b))
            return false;
        a++; b++;
    }
    return *a == *b;
}

/* 8.3 항목을 "NAME.EXT" 형태로 풀어쓴다. */
static void expand_83(const u8 *entry, char *out)
{
    int n = 0;
    for (int i = 0; i < 8 && entry[i] != ' '; i++)
        out[n++] = (char)entry[i];
    if (entry[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && entry[i] != ' '; i++)
            out[n++] = (char)entry[i];
    }
    out[n] = '\0';
}

/* LFN 조각 하나에서 문자 13개를 꺼낸다.
 *
 * UTF-16 을 ASCII 로 떨어뜨린다. 부트 파티션의 파일 이름은 전부
 * ASCII 이고, 아니라면 그건 우리가 찾는 파일이 아니다. 0x7F 를 넘는
 * 문자는 '?' 로 두어 이름이 우연히 맞아떨어지는 일을 막는다. */
static void lfn_chars(const u8 *entry, char *out13)
{
    static const u8 pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int i = 0; i < 13; i++) {
        u16 c = rd16(&entry[pos[i]]);
        if (c == 0x0000 || c == 0xFFFF) { out13[i] = '\0'; return; }
        out13[i] = (c < 0x80) ? (char)c : '?';
    }
}

/* ── 디렉터리 훑기 ───────────────────────────────────────────────
 *
 * 항목을 하나씩 보면서 콜백에 넘긴다. LFN 조각은 이름이 완성될
 * 때까지 모았다가 8.3 항목이 나오는 순간 함께 전달한다. FAT 는 LFN
 * 조각을 역순으로 저장하므로(마지막 조각이 먼저) 앞에서부터 채워
 * 넣으려면 순서 번호를 봐야 한다. */
typedef bool (*dir_fn)(const char *name, const u8 *entry, void *ctx);

static bool walk_root(dir_fn fn, void *ctx)
{
    if (!fs.ready)
        return false;

    char lfn[261];
    bool have_lfn = false;
    memset(lfn, 0, sizeof lfn);

    u32 cluster = fs.root_cluster;
    while (cluster) {
        for (u32 s = 0; s < fs.sectors_per_cluster; s++) {
            if (!emmc_read(fs.part_lba + cluster_lba(cluster) + s, 1, sector_buf))
                return false;

            for (u32 off = 0; off < 512; off += 32) {
                const u8 *e = &sector_buf[off];

                if (e[0] == 0x00)
                    return true;            /* 디렉터리 끝 */
                if (e[0] == 0xE5) {         /* 지워진 항목 */
                    have_lfn = false;
                    continue;
                }

                u8 attr = e[11];
                if ((attr & 0x0F) == 0x0F) {
                    /* LFN 조각. 순서 번호는 1부터 시작한다. */
                    u32 seq = e[0] & 0x3F;
                    if (seq >= 1 && seq <= 20) {
                        char part[14];
                        memset(part, 0, sizeof part);
                        lfn_chars(e, part);
                        memcpy(&lfn[(seq - 1) * 13], part, 13);
                        have_lfn = true;
                    }
                    continue;
                }

                if (attr & 0x08) {          /* 볼륨 레이블 */
                    have_lfn = false;
                    continue;
                }

                char short_name[16];
                expand_83(e, short_name);

                const char *name = short_name;
                if (have_lfn && lfn[0]) {
                    lfn[260] = '\0';
                    name = lfn;
                }

                if (fn(name, e, ctx))
                    return true;

                have_lfn = false;
                memset(lfn, 0, sizeof lfn);
            }
        }
        cluster = next_cluster(cluster);
    }
    return true;
}

/* ── 찾기 ────────────────────────────────────────────────────────*/

typedef struct {
    const char *want;
    fat_file_t *out;
    bool found;
} find_ctx;

static bool find_cb(const char *name, const u8 *e, void *ctx)
{
    find_ctx *c = (find_ctx *)ctx;
    if (!name_eq(name, c->want))
        return false;
    if (e[11] & 0x10)                       /* 디렉터리는 파일이 아니다 */
        return false;

    c->out->first_cluster = ((u32)rd16(&e[20]) << 16) | rd16(&e[26]);
    c->out->size          = rd32(&e[28]);
    c->found = true;
    return true;                            /* 훑기를 멈춘다 */
}

bool fat32_find(const char *name, fat_file_t *out)
{
    find_ctx c = { name, out, false };
    walk_root(find_cb, &c);
    return c.found;
}

/* ── 읽기 ────────────────────────────────────────────────────────*/

bool fat32_read_file(const fat_file_t *f, void *dst, u32 max, u32 *read_out)
{
    if (!fs.ready || f->first_cluster < 2)
        return false;

    if (f->size > max) {
        kprintf("[fat] 파일이 %u 바이트인데 자리가 %u 뿐이다\n", f->size, max);
        return false;
    }

    u8  *out       = (u8 *)dst;
    u32  remaining = f->size;
    u32  cluster   = f->first_cluster;
    u32  bytes_per_cluster = fs.sectors_per_cluster * 512;

    while (cluster && remaining) {
        u32 chunk = remaining < bytes_per_cluster ? remaining : bytes_per_cluster;

        /* 클러스터가 통째로 필요하면 한 번의 다중 블록 읽기로 가져온다.
         * 마지막 클러스터만 부분이라 따로 처리한다 - 목적지 버퍼 밖으로
         * 넘겨 쓰지 않기 위해서다. */
        if (chunk == bytes_per_cluster) {
            if (!emmc_read(fs.part_lba + cluster_lba(cluster),
                           fs.sectors_per_cluster, out))
                return false;
        } else {
            u32 done = 0;
            for (u32 s = 0; s < fs.sectors_per_cluster && done < chunk; s++) {
                if (!emmc_read(fs.part_lba + cluster_lba(cluster) + s,
                               1, sector_buf))
                    return false;
                u32 take = (chunk - done) < 512 ? (chunk - done) : 512;
                memcpy(out + done, sector_buf, take);
                done += take;
            }
        }

        out       += chunk;
        remaining -= chunk;
        cluster    = next_cluster(cluster);
    }

    if (remaining) {
        kprintf("[fat] 체인이 %u 바이트 남기고 끊겼다\n", remaining);
        return false;
    }
    if (read_out)
        *read_out = f->size;
    return true;
}

/* ── 보여주기 ────────────────────────────────────────────────────*/

static bool list_cb(const char *name, const u8 *e, void *ctx)
{
    (void)ctx;
    u32 size = rd32(&e[28]);
    if (e[11] & 0x10)
        kprintf("    %-32s  <디렉터리>\n", name);
    else if (size >= 1024 * 1024)
        kprintf("    %-32s  %u MB\n", name, size / (1024 * 1024));
    else if (size >= 1024)
        kprintf("    %-32s  %u KB\n", name, size / 1024);
    else
        kprintf("    %-32s  %u B\n", name, size);
    return false;                           /* 끝까지 본다 */
}

void fat32_list(void)
{
    if (!fs.ready) {
        kprintf("  마운트되지 않았다\n");
        return;
    }
    kprintf("  루트 디렉터리:\n");
    walk_root(list_cb, NULL);
}

void fat32_describe(void)
{
    if (!fs.ready) {
        kprintf("  FAT32 파티션 없음\n");
        return;
    }
    kprintf("  파티션 LBA %llu, 클러스터 %u 섹터(%u KB),"
            " 클러스터 %u개\n",
            (unsigned long long)fs.part_lba, fs.sectors_per_cluster,
            fs.sectors_per_cluster / 2, fs.total_clusters);
}
