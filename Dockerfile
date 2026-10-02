# LP-zero OS 를 짓는 환경.
#
# 왜 있는가: 이 저장소는 커널, libc, 유저랜드, 부트 이미지를 전부
# 직접 짓는다. 그래서 필요한 도구가 스무 개쯤 되고, 그중 몇 개는
# 없으면 빌드가 7 분 뒤에 뜻을 알 수 없는 오류로 끝난다 (EFI_ZBOOT 가
# 부르는 hexdump 가 그랬다). 여기 다 적어두면 그 일이 없다.
#
#   docker build -t lpzero .
#   docker run --rm -it -v "$PWD":/src -w /src lpzero
#
# 안에서:
#   ./tools/build-sysroot.sh     # 한 번만. CPython 이 쓸 라이브러리들
#   ./tools/build-python.sh      # 한 번만. 오래 걸린다
#   make image                   # 유저랜드 -> 커널 -> SD 이미지
#
# 컨테이너에는 아무것도 남기지 않는다. 소스도 결과물도 -v 로 붙인
# 호스트 디렉터리에 있다.
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    \
    `# 우리 유저랜드: clang 으로 짓고 ld.lld 로 링크한다.` \
    `# gcc 가 아닌 이유는 --target 하나로 크로스 컴파일이 되기 때문이다.` \
    clang lld llvm \
    \
    `# 커널은 gcc 로 짓는다. 리눅스는 clang 으로도 지어지지만,` \
    `# 라즈베리파이 커널 트리에서 검증된 조합은 gcc 쪽이다.` \
    gcc-aarch64-linux-gnu libc6-dev-arm64-cross \
    \
    `# 커널 빌드가 요구하는 것들` \
    bc bison flex libssl-dev libelf-dev kmod \
    \
    `# EFI_ZBOOT 가 vmlinuz.efi 를 만들 때 이미지 크기를 hexdump 로 읽는다.` \
    `# 없으면 "truncate: Invalid number" 로 끝난다.` \
    bsdextrautils \
    \
    `# initramfs(cpio), FAT 부트 파티션, ext4 데이터 파티션` \
    cpio dosfstools mtools e2fsprogs \
    \
    `# CPython 크로스 빌드에는 같은 major.minor 의 호스트 파이썬이 필요하다.` \
    `# ubuntu:24.04 의 python3 는 3.12 이고, 우리가 짓는 것도 3.12 다.` \
    python3 python3-dev \
    \
    `# 소스를 받고 체크섬을 맞춘다` \
    curl ca-certificates git make xz-utils zip \
    \
    `# 루트 인증서: /usr/share/ca-certificates/mozilla 가 필요하다.` \
    `# 호스트의 합쳐진 번들이 아니라 이쪽을 쓴다 - CI 컨테이너의` \
    `# TLS 가로채기 CA 가 기기에 들어가지 않게.` \
    \
    `# 실기 없이 확인할 때` \
    qemu-system-arm qemu-efi-aarch64 \
    \
    && rm -rf /var/lib/apt/lists/*

# 빌드 산출물이 저장소 밖으로 나가는 곳. build.sh 와 build-python.sh 가
# 여기를 쓴다 (WORK, BUILD_DIR 환경변수로 바꿀 수 있다).
RUN mkdir -p /home/user/kernel-work
ENV WORK=/home/user/kernel-work/thirdparty

WORKDIR /src
CMD ["/bin/bash"]
