/* mbox.h - VideoCore 메일박스 프로퍼티 인터페이스.
 *
 * ARM 이 GPU 에게 요청을 보내는 유일한 창구다. 클럭 조회/설정,
 * 프레임버퍼 할당, 전원 관리, 보드 정보가 전부 여기를 통한다. */
#ifndef _MBOX_H
#define _MBOX_H

#include "types.h"

#define MBOX_CH_PROP            8       /* ARM -> VC 프로퍼티 채널 */

/* 프로퍼티 태그 (공식 mailbox property interface 문서 기준) */
#define TAG_GET_FIRMWARE_REV    0x00000001
#define TAG_GET_BOARD_MODEL     0x00010001
#define TAG_GET_BOARD_REV       0x00010002
#define TAG_GET_MAC_ADDRESS     0x00010003
#define TAG_GET_BOARD_SERIAL    0x00010004
#define TAG_GET_ARM_MEMORY      0x00010005
#define TAG_GET_VC_MEMORY       0x00010006
#define TAG_GET_CLOCK_RATE      0x00030002
#define TAG_GET_MAX_CLOCK_RATE  0x00030004
#define TAG_GET_TEMPERATURE     0x00030006
#define TAG_GET_MAX_TEMPERATURE 0x0003000A
#define TAG_SET_CLOCK_RATE      0x00038002

/* 클럭 ID */
#define MBOX_CLOCK_EMMC         1
#define MBOX_CLOCK_UART         2
#define MBOX_CLOCK_ARM          3
#define MBOX_CLOCK_CORE         4
#define MBOX_CLOCK_V3D          5
#define MBOX_CLOCK_SDRAM        8

/* 태그 하나짜리 요청을 보낸다.
 *   req/req_words  : 보낼 값 (없으면 NULL/0)
 *   resp/resp_words: 받을 값 버퍼
 * 성공 시 true. */
bool mbox_prop(u32 tag, const u32 *req, u32 req_words,
               u32 *resp, u32 resp_words);

/* 여러 태그를 한 메시지로 보낸다 (프레임버퍼 설정처럼 원자적으로 처리돼야
 * 하는 경우). 호출자가 msg 를 완전히 구성해야 한다:
 *   msg[0] = 전체 바이트 크기, msg[1] = 0, ...태그들..., 마지막 워드 = 0
 * 성공 시 응답이 msg 에 그대로 덮어써진다. */
bool mbox_send(u32 *msg, u32 words);

/* 자주 쓰는 것들 얇게 감싼 헬퍼. 실패 시 0 을 돌려준다. */
u32  mbox_get_clock_rate(u32 clock_id);
u32  mbox_get_max_clock_rate(u32 clock_id);
u32  mbox_get_board_revision(void);
u32  mbox_get_firmware_revision(void);
u64  mbox_get_board_serial(void);
bool mbox_get_arm_memory(u32 *base, u32 *size);
bool mbox_get_vc_memory(u32 *base, u32 *size);
s32  mbox_get_temperature_mc(void);   /* 밀리섭씨. 실패 시 -1 */

#endif /* _MBOX_H */
