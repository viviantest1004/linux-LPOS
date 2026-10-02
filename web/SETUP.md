# 다운로드 페이지 올리기 (GitHub Pages)

이미지 세 개를 내려받을 수 있는 페이지를 **무료로, 서버 없이** 띄웁니다.
GitHub Pages 가 이 저장소를 그대로 서빙하므로 배포할 것도, 접속할
서버도 없습니다.

한국어가 먼저, 영어가 뒤에 있습니다.

---

## 1. 켜기 (한 번, 클릭 네 번)

1. GitHub 저장소 → **Settings**
2. 왼쪽 메뉴 **Pages**
3. **Source** 를 **Deploy from a branch** 로
4. **Branch** 를 **main** / **/ (root)** 로 두고 **Save**

1~2분 뒤 주소가 나옵니다:

```
https://<아이디>.github.io/<레포>/
```

끝입니다. 페이지(`index.html`)와 이미지(`dist/`) 모두 저장소에 이미
들어 있으므로 따로 올릴 것이 없습니다.

> **`.nojekyll` 파일을 지우지 마세요.** GitHub Pages 는 기본으로
> Jekyll 이라는 정적 사이트 생성기를 돌리는데, 그러면 밑줄로 시작하는
> 파일을 무시하는 등 저장소를 제 마음대로 해석합니다. 이 빈 파일
> 하나가 "있는 그대로 내보내라" 는 뜻입니다.

---

## 2. 새 이미지를 빌드했을 때

```bash
./tools/mkdist.sh        # 이미지 + 체크섬 + 페이지를 한 번에
git add -A && git commit -m "새 이미지" && git push
```

`mkdist.sh` 가 마지막에 `tools/mkweb.sh` 를 불러 페이지를 다시
만듭니다. **크기와 sha256 은 손으로 적지 않습니다** — `dist/` 의 실제
파일에서 읽습니다. 이 저장소에서 체크섬 파일이 네 번의 재빌드 동안
방치돼 세 해시가 전부 틀렸던 일이 있었고, 안 맞는 체크섬은 없느니만
못합니다. 받는 쪽에서 파일이 깨진 건지 목록이 낡은 건지 구별할 수
없으니까요.

푸시하면 1~2분 안에 사이트가 바뀝니다.

문구를 고치고 싶으면 `web/template.html` 을 고친 뒤 `./tools/mkweb.sh`
를 돌리세요. `index.html` 은 결과물이라 직접 고쳐봐야 다음 빌드에
덮입니다.

---

## 3. cholab.kr 주소로 쓰고 싶다면 (선택)

`lp.cholab.kr` 같은 주소를 붙일 수 있습니다. **서버는 여전히 필요
없습니다** — DNS 레코드 하나만 추가하면 GitHub 이 인증서까지 알아서
발급합니다.

1. 도메인 산 곳의 DNS 에서 레코드 추가:

   | 종류 | 이름 | 값 |
   |---|---|---|
   | CNAME | `lp` | `<아이디>.github.io` |

2. 퍼질 때까지 기다린 뒤 확인:

   ```bash
   dig +short lp.cholab.kr
   ```

3. GitHub → Settings → Pages → **Custom domain** 에 `lp.cholab.kr` 입력
   → Save
4. **Enforce HTTPS** 가 켜질 때까지 기다립니다 (인증서 발급에 보통
   몇 분, 길면 한 시간)

> **순서를 지키세요.** DNS 를 먼저 하고 GitHub 설정을 나중에 합니다.
> 반대로 하면 GitHub 이 그 이름으로 접속해 확인하지 못해 인증서 발급이
> 실패하고, 그 사이 사이트가 안 열립니다.

GitHub 이 저장소 루트에 `CNAME` 파일을 자동으로 만들어 커밋합니다.
지우지 마세요 — 지우면 커스텀 도메인이 풀립니다.

---

## 4. 한계

| | |
|---|---|
| 저장소 크기 | 권장 1GB 이하. 지금 이미지 세 개가 85MB 라 여유 있습니다 |
| 대역폭 | 월 100GB (소프트 리밋). 전체 세트 기준 약 1,100회 다운로드 |
| 파일 하나 크기 | 100MB 초과 시 GitHub 이 푸시를 거부합니다. 지금 최대 30MB |
| 비용 | 공개 저장소는 무료입니다 |

월 100GB 를 넘길 만큼 퍼진다면 GitHub 이 메일로 알려줍니다. 그때는
**GitHub Releases** 로 파일을 옮기는 것이 정답입니다 — 릴리스 첨부
파일은 Pages 대역폭에 안 잡히고, 파일당 2GB 까지 됩니다. 페이지의
`dist/...` 링크를 릴리스 주소로 바꾸기만 하면 됩니다.

자체 서버(nginx)로 돌리는 설정과 배포 스크립트도 한 번 만들어
두었습니다. 지금은 쓰지 않아 지웠지만 커밋 `c7ba2e9` 에 남아 있으니,
나중에 필요하면 `git show c7ba2e9` 로 꺼내 쓰면 됩니다.

---

## 5. 잘 안 될 때

| 증상 | 원인 |
|---|---|
| 404 | Pages 가 아직 안 켜졌거나, Branch 가 `main` / `/ (root)` 가 아닙니다 |
| 페이지는 뜨는데 다운로드가 404 | `dist/` 가 커밋되지 않았습니다. `git status` 로 확인 |
| 스타일이 깨짐 | `.nojekyll` 이 지워졌습니다 |
| 커스텀 도메인에서 인증서 오류 | DNS 가 아직 안 퍼졌습니다. `dig` 부터 |
| 푸시했는데 안 바뀜 | 저장소 **Actions** 탭에서 pages-build-deployment 가 도는지 확인 |

---

## English

The page is served free by GitHub Pages straight out of this
repository. There is no server to reach and nothing to deploy - which
is the point: both the page (`index.html`) and the images (`dist/`) are
already committed.

**Turn it on, once:** Settings → Pages → Source: *Deploy from a branch*
→ Branch: **main** / **/ (root)** → Save. A minute later the site is at
`https://<owner>.github.io/<repo>/`.

Do not delete `.nojekyll`. Without it GitHub runs Jekyll over the
repository and starts reinterpreting files.

**After building new images:** `./tools/mkdist.sh`, then commit and
push. mkdist regenerates the page as its last step, reading the three
sizes and three sha256 sums out of the real files - they are never typed
by hand, because in this repository a hand-written checksum list once
sat through four rebuilds while all three of its hashes were wrong, and
a checksum that does not match is worse than none at all. Edit
`web/template.html` for wording; `index.html` is output and gets
overwritten.

**A custom domain** like `lp.cholab.kr` still needs no server: add a
CNAME record pointing `lp` at `<owner>.github.io`, wait for DNS,
then set it in Settings → Pages. In that order - GitHub connects to the
name to issue the certificate, so it cannot work before DNS does.

**Limits:** 1GB repository, 100GB/month soft bandwidth (about 1,100
full download sets), 100MB per file. Free for public repositories. If it
ever spreads far enough to matter, move the files to GitHub Releases -
release assets do not count against Pages bandwidth - and change the
`dist/...` links. The nginx config and rsync deploy script for
self-hosting were written and then removed when this approach was
chosen; they are in commit `c7ba2e9` if they are ever wanted.
