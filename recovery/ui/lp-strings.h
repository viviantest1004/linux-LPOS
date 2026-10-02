/* lp-strings.h - every word the boot menu and the recovery menu put on
 * the screen, in English and in Korean.
 *
 * Neither program has gettext, a locale or a font renderer: the boot menu
 * runs in the firmware before any operating system exists, and the
 * recovery menu is a static binary on our own libc. So the catalog is a
 * table compiled in, and it doubles as the input of mkglyphs.py - the
 * glyph atlas (lp-glyphs.h) holds exactly the characters these strings
 * use, at the size each one is drawn at. That is what keeps the atlas a
 * few hundred kilobytes instead of a whole Hangul font: add a string
 * here and rerun `python3.12 recovery/ui/mkglyphs.py`, or its new
 * characters draw as empty boxes.
 *
 * Each line is LPS(id, size, "English", "Korean"). The size class is the
 * face the string is drawn in (lp-ui.h):
 *   T  titles       SemiBold, 104 px on a 2160-line panel
 *   M  items        Medium,    60 px
 *   S  small print  Regular,   40 px
 * The M and S faces also carry all of printable ASCII, because they draw
 * text that is not in this file: account names, file names, fsck output.
 *
 * %d and %s are filled in by lp_ui_fmt(); they are the only two formats.
 * English is the default everywhere (COMMON.md "Language"); Korean is
 * chosen by the LPLang EFI variable in the boot menu and by lp.lang=ko on
 * the kernel command line (which the boot menu adds) in recovery.
 *
 * Korean is written without particles that depend on the word before
 * them (을/를), because %s can be anything. */

/* ── The boot menu (boot/efi/lpboot.c) ─────────────────────────────── */
LPS(B_LP,            T, "LP", "LP")
LPS(B_RECOVERY,      T, "LP Recovery", "LP 복구")
LPS(B_LP_SUB,        S, "Start LP", "LP 시작")
LPS(B_RECOVERY_SUB,  S, "Repair, reinstall or open a shell", "복구, 재설치, 관리자 셸")
LPS(B_COUNT_LP,      S, "Starting LP in %d s", "%d초 후 LP를 시작합니다")
LPS(B_COUNT_REC,     S, "Starting LP Recovery in %d s", "%d초 후 LP 복구를 시작합니다")
LPS(B_STOPPED,       S, "Automatic start stopped", "자동 시작을 멈췄습니다")
LPS(B_STARTING,      S, "Starting…", "시작하는 중…")
LPS(B_FAILED_BOOTS,  S, "LP did not start correctly the last %d times, so LP Recovery is selected.",
                        "LP가 최근 %d번 제대로 시작되지 않아 LP 복구를 선택했습니다.")
LPS(B_REINSTALL_OPEN,S, "A reinstall did not finish. LP Recovery can run it again.",
                        "재설치가 끝나지 않았습니다. LP 복구에서 다시 실행할 수 있습니다.")
LPS(B_ASKED,         S, "Restarting into LP Recovery, as asked.", "요청한 대로 LP 복구로 시작합니다.")
LPS(B_HINT,          S, "← →  Choose      Enter  Start      Esc  Stop the timer",
                        "← →  선택      Enter  시작      Esc  타이머 멈춤")
LPS(B_ERROR,         S, "Could not start it (%s). Choose again.", "시작하지 못했습니다 (%s). 다시 선택하세요.")

/* ── The recovery menu (recovery/lp-recovery.c) ────────────────────── */
LPS(R_TITLE,         T, "LP Recovery", "LP 복구")
LPS(R_CHOOSE,        S, "Choose what to do", "할 일을 선택하세요")
LPS(R_SHELL,         M, "Recovery shell (administrator)", "복구 셸 (관리자)")
LPS(R_SHELL_SUB,     S, "A root shell, after an administrator password", "관리자 암호를 확인한 뒤 루트 셸을 엽니다")
LPS(R_REINSTALL,     M, "Reinstall LP", "LP 재설치")
LPS(R_REINSTALL_SUB, S, "Put a fresh copy of LP on this computer", "이 컴퓨터에 LP를 새로 설치합니다")
LPS(R_CHECK,         M, "Check and repair disks", "디스크 검사 및 복구")
LPS(R_CHECK_SUB,     S, "Check the file systems and fix what can be fixed", "파일 시스템을 검사하고 고칠 수 있는 것을 고칩니다")
LPS(R_EXIT,          M, "Exit recovery and restart", "복구 끝내고 다시 시작")
LPS(R_EXIT_SUB,      S, "Restart into LP", "LP로 다시 시작합니다")
LPS(R_HINT,          S, "↑ ↓  Choose      Enter  Open      Esc  Back",
                        "↑ ↓  선택      Enter  열기      Esc  뒤로")
LPS(R_INTERRUPTED,   S, "A reinstall was interrupted. Run Reinstall LP again to finish it.",
                        "재설치가 중단되었습니다. LP 재설치를 다시 실행해 마무리하세요.")

/* the status line */
LPS(R_DISK_OK,       S, "Disks: OK", "디스크: 정상")
LPS(R_DISK_CHECK,    S, "Disks: LP-ROOT needs a check", "디스크: LP-ROOT 검사 필요")
LPS(R_DISK_NOROOT,   S, "Disks: LP-ROOT not found", "디스크: LP-ROOT 없음")
LPS(R_BATTERY,       S, "Battery %d%%", "배터리 %d%%")
LPS(R_CHARGING,      S, "Battery %d%%, charging", "배터리 %d%%, 충전 중")

/* the administrator check */
LPS(R_AUTH_TITLE,    T, "Administrator", "관리자 확인")
LPS(R_AUTH_WHO,      S, "Choose an administrator of this computer and enter their password.",
                        "이 컴퓨터의 관리자를 고르고 그 암호를 입력하세요.")
LPS(R_AUTH_RECPW,    S, "The installed system cannot be read. Enter the recovery password set when LP was installed.",
                        "설치된 시스템을 읽을 수 없습니다. LP를 설치할 때 정한 복구 암호를 입력하세요.")
LPS(R_AUTH_RECNAME,  M, "Recovery password", "복구 암호")
LPS(R_AUTH_PASSWORD, S, "Password", "암호")
LPS(R_AUTH_OPEN,     M, "Open shell", "셸 열기")
LPS(R_BACK,          M, "Back", "뒤로")
LPS(R_KEYBOARD,      M, "Keyboard", "키보드")
LPS(R_AUTH_CHECKING, S, "Checking…", "확인하는 중…")
LPS(R_AUTH_WRONG,    S, "Wrong password. %d more wrong tries and you must wait.",
                        "암호가 틀렸습니다. %d번 더 틀리면 기다려야 합니다.")
LPS(R_AUTH_WAIT,     S, "Too many wrong passwords. Try again in %d s.", "암호가 여러 번 틀렸습니다. %d초 뒤에 다시 하세요.")
LPS(R_AUTH_NOBODY,   S, "No administrator account and no recovery password were found, so the shell cannot be opened.",
                        "관리자 계정도 복구 암호도 없어서 셸을 열 수 없습니다.")
LPS(R_AUTH_FORMAT,   S, "This account's password is in a format recovery cannot check (%s).",
                        "이 계정의 암호는 복구에서 확인할 수 없는 형식입니다 (%s).")
LPS(R_AUTH_LOCKED,   S, "This account has no password it can sign in with.", "이 계정은 로그인할 암호가 없습니다.")

/* reinstall */
LPS(R_RE_TITLE,      T, "Reinstall LP", "LP 재설치")
LPS(R_RE_KEEP,       M, "Keep my files", "내 파일 유지")
LPS(R_RE_KEEP_SUB,   S, "Keeps /home and the user accounts. Everything else is replaced.",
                        "/home과 사용자 계정은 그대로 두고 나머지를 새로 설치합니다.")
LPS(R_RE_ERASE,      M, "Erase everything", "모두 지우기")
LPS(R_RE_ERASE_SUB,  S, "Erases LP-ROOT completely, /home included, and installs a fresh LP.",
                        "/home을 포함해 LP-ROOT를 모두 지우고 LP를 새로 설치합니다.")
LPS(R_RE_WHAT,       M, "What will happen", "진행 내용")
LPS(R_RE_KEPT,       S, "Kept: /home, user accounts and passwords, computer name, time zone, language, keyboard, Wi-Fi networks, SSH keys.",
                        "유지: /home, 사용자 계정과 암호, 컴퓨터 이름, 시간대, 언어, 키보드, Wi-Fi 네트워크, SSH 키.")
LPS(R_RE_REPLACED,   S, "Replaced: everything else on LP-ROOT, and the boot files on the EFI partition.",
                        "교체: LP-ROOT의 나머지 전부와 EFI 파티션의 부팅 파일.")
LPS(R_RE_ERASED,     S, "Erased: everything on LP-ROOT, including /home and every account. The first-start setup runs again.",
                        "삭제: /home과 모든 계정을 포함한 LP-ROOT의 전부. 처음 시작 설정을 다시 합니다.")
LPS(R_RE_PAYLOAD,    S, "Installs: %s", "설치할 버전: %s")
LPS(R_RE_TYPE,       S, "Type REINSTALL to continue.", "계속하려면 REINSTALL을 입력하세요.")
LPS(R_RE_GO,         M, "Reinstall", "재설치")
LPS(R_CANCEL,        M, "Cancel", "취소")
LPS(R_RE_VERIFY,     S, "Checking the installation files", "설치 파일을 확인하는 중")
LPS(R_RE_SAVE,       S, "Saving your settings", "설정을 보관하는 중")
LPS(R_RE_REMOVE,     S, "Removing the old system", "이전 시스템을 지우는 중")
LPS(R_RE_FORMAT,     S, "Formatting LP-ROOT", "LP-ROOT를 포맷하는 중")
LPS(R_RE_COPY,       S, "Copying LP", "LP를 복사하는 중")
LPS(R_RE_RESTORE,    S, "Restoring your settings", "설정을 되돌리는 중")
LPS(R_RE_BOOT,       S, "Writing the boot files", "부팅 파일을 쓰는 중")
LPS(R_RE_FINISH,     S, "Finishing", "마무리하는 중")
LPS(R_RE_DONE,       M, "LP is reinstalled.", "LP를 다시 설치했습니다.")
LPS(R_RE_FAILED,     S, "Reinstall failed: %s", "재설치 실패: %s")
LPS(R_RE_DAMAGED,    S, "The installation files are damaged (%s). Nothing was changed.",
                        "설치 파일이 손상되었습니다 (%s). 아무것도 바꾸지 않았습니다.")
LPS(R_RE_AGAIN,      S, "Power loss or an error leaves a state Reinstall LP can finish: run it again.",
                        "전원이 꺼지거나 오류가 나도 LP 재설치를 다시 실행하면 마무리됩니다.")
LPS(R_RESTART,       M, "Restart", "다시 시작")
LPS(R_NO_PAYLOAD,    S, "This recovery partition carries no installation files.", "이 복구 파티션에는 설치 파일이 없습니다.")

/* disk check */
LPS(R_CK_TITLE,      T, "Check disks", "디스크 검사")
LPS(R_CK_RUNNING,    S, "Checking %s…", "%s 검사 중…")
LPS(R_CK_CLEAN,      S, "%s: no problems", "%s: 문제 없음")
LPS(R_CK_FIXED,      S, "%s: problems were repaired", "%s: 문제를 고쳤습니다")
LPS(R_CK_LEFT,       S, "%s: problems remain (code %d)", "%s: 문제가 남아 있습니다 (코드 %d)")
LPS(R_CK_MISSING,    S, "%s: not found", "%s: 찾을 수 없음")
LPS(R_CK_DONE,       M, "Done", "완료")

/* the on-screen keyboard and the recovery shell */
LPS(R_OSK_CLOSE,     M, "Close", "닫기")
LPS(R_SHELL_BAR,     S, "Recovery shell · root · type exit to return to the menu",
                        "복구 셸 · 루트 · exit를 입력하면 메뉴로 돌아갑니다")
LPS(R_SHELL_FAILED,  S, "The shell could not be started (%s).", "셸을 시작하지 못했습니다 (%s).")

/* exit */
LPS(R_RESTARTING,    M, "Restarting…", "다시 시작하는 중…")
