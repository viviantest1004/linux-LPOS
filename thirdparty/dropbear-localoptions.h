/* localoptions.h - LP-zero 용 dropbear 컴파일 옵션.
 *
 * 공개키 인증만 쓴다. 이유가 둘 있다:
 *   1) 크로스 환경에 crypt() 가 없어 비밀번호 인증을 빌드할 수 없다
 *   2) 애초에 네트워크에 노출되는 기기에서 비밀번호 인증은 나쁜 선택이다
 *      (무차별 대입 대상이 된다)
 */

#define DROPBEAR_SVR_PASSWORD_AUTH   0
#define DROPBEAR_SVR_PUBKEY_AUTH     1
#define DROPBEAR_CLI_PASSWORD_AUTH   0

/* 호스트 키: Ed25519 만. 작고 빠르고 안전하다.
 * RSA 는 키 생성이 느리고(첫 부팅이 수 초 길어진다) 파일도 크다. */
#define DROPBEAR_RSA                 0
#define DROPBEAR_DSS                 0
#define DROPBEAR_ECDSA               0
#define DROPBEAR_ED25519             1

/* 키 교환: Curve25519 만 */
#define DROPBEAR_CURVE25519          1
#define DROPBEAR_DH_GROUP14_SHA256   0
#define DROPBEAR_DH_GROUP16          0

/* 암호: ChaCha20-Poly1305 와 AES128-CTR.
 * 앞의 것이 Cortex-A53 처럼 AES 가속이 약한 CPU 에서 더 빠르다. */
#define DROPBEAR_CHACHA20POLY1305    1
#define DROPBEAR_AES128              1
#define DROPBEAR_AES256              0
#define DROPBEAR_3DES                0

/* 안 쓰는 기능 */
#define DROPBEAR_SVR_LOCALTCPFWD     0
#define DROPBEAR_SVR_REMOTETCPFWD    0
#define DROPBEAR_SVR_AGENTFWD        0
#define DROPBEAR_X11FWD              0
#define DROPBEAR_USE_ZLIB            0
