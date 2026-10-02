# config.mk - 프로젝트 전역 설정. Makefile 과 tools/*.sh 가 같이 읽는다.
#
# 커널 이미지 파일명.
#   · 이 이름이 boot/config.txt 의 kernel= 값과 반드시 일치해야 한다.
#     어긋나면 GPU 가 커널을 못 찾고, 아무 출력도 없이 멈춘다.
#   · tools/mksdcard.sh 가 SD 이미지를 만들 때 둘이 같은지 검사한다.
#   · 이름을 바꾸려면 여기와 boot/config.txt 두 곳을 고친다.
KERNEL_IMAGE := test_a_123_LPzero2W.img

# 리눅스 커널 이미지 파일명 (make sdcard-linux 로 굽는 이미지).
# 마찬가지로 boot/config-linux.txt 의 kernel= 과 일치해야 한다.
LINUX_IMAGE := test_a_123_LPzero2W_linux.img

# 리눅스 커널 이미지 파일명, Pi Zero W (ARMv6) 용.
# boot/config-armv6.txt 의 kernel= 과 일치해야 한다.
#
# 이름이 다른 이유: 같은 SD 카드에 두 커널이 들어갈 일은 없지만, 파일을
# 받아놓고 어느 보드용인지 몰라 헤매는 일은 자주 있다.
LINUX_IMAGE_ARMV6 := test_a_123_LPzeroW_linux.img
