# Publishing 自在投影 / Zizai Cast to GitHub (v0.6.0)

Runbook for the integrator. Nothing here has been executed. Public repo:
`https://github.com/victor900106/ZizaiCast` (today: one README commit on `main`,
no LICENSE, one draft release).

Goals:

* The public `main` is a **fresh orphan history** — one snapshot commit of the
  release source — so no earlier commit (old mascot art from before `64c29cc`,
  rejected Yuyu art, private build notes) is ever pushed.
* README / images / issue templates / CONTRIBUTING from `launch/`.
* Description, topics, website, social preview.
* Release `v0.6.0`: `zizai-setup-0.6.0.exe`, `update.json`, the two source zips.
* Optional: GitHub Pages from `launch/site/`.

Shell: PowerShell 5.1 (`;` / `if ($?)`, no `&&`). Paths below assume the repo at
`C:\Users\victo\Desktop\AI\PhoneMirror`.

---

## 0. Prerequisites

```powershell
winget install --id GitHub.cli -e          # gh is not installed on this PC yet
gh auth login                              # account victor900106, scopes: repo, workflow
$Repo   = 'C:\Users\victo\Desktop\AI\PhoneMirror'
$Ver    = '0.6.0'
$Pub    = "$env:TEMP\zc-publish"           # throw-away working tree for the public snapshot
$Owner  = 'victor900106/ZizaiCast'
```

Decide before you start (owner):

1. **Rejected art in the source tree.** `git archive` of `eff9450` (and therefore
   `ZizaiCast-0.6.0-source.zip`) still contains `assets/public/yuyu/`,
   `assets/public/mascot_{a,b,c}_*.svg`, `docs/mascot/concepts.*`,
   `docs/mascot/yuyu/` and the old `launch/` (with Yuyu banners). They are original
   (not the unknown-copyright art), but the owner rejected Yuyu. Recommended:
   `git rm -r` them in a normal commit **before** step 1, then rebuild the source zip,
   so the zip and the public snapshot agree.
2. **Installer asset name.** Releases use `zizai-setup-0.6.0.exe`, but
   `docs/licenses/SOURCE.md` §1 lists `自在投影-安裝程式-<version>.exe`. Update that
   table (and the zh/en third-party notices if they name it) in the same commit.
   (The local-update pattern `自在投影-安裝程式-*.exe` in `安裝檔` is unrelated and stays.)

## 1. Source zips (exact release commit)

```powershell
cd $Repo
git status --short                          # must be clean for the release commit
powershell -ExecutionPolicy Bypass -File tools\release\make_source_zip.ps1 -Verify
# -> build-release\ZizaiCast-0.6.0-source.zip, ZizaiCast-0.6.0-deps-source.zip
#    (refuses fdk-aac builds and any old mascot / icon blob)
$Commit = git rev-parse HEAD
```

## 2. Public snapshot (orphan history)

Built in a separate folder from the verified source zip, so the private repo's
working tree, branches and history are never touched (no checkout / stash / reset).

```powershell
Remove-Item -Recurse -Force $Pub -ErrorAction SilentlyContinue
Expand-Archive "$Repo\build-release\ZizaiCast-$Ver-source.zip" "$env:TEMP\zc-src" -Force
Move-Item "$env:TEMP\zc-src\ZizaiCast-$Ver-source" $Pub
Remove-Item -Recurse -Force "$env:TEMP\zc-src"

# launch/ is marketing material, not source: drop it from the public tree
Remove-Item -Recurse -Force "$Pub\launch" -ErrorAction SilentlyContinue

# README, README images, community files
Copy-Item "$Repo\launch\README.md", "$Repo\launch\README.en.md" $Pub
New-Item -ItemType Directory -Force "$Pub\docs\readme" | Out-Null
Copy-Item -Recurse -Force "$Repo\launch\docs\readme\*" "$Pub\docs\readme\"
Copy-Item -Recurse -Force "$Repo\launch\repo-files\.github" $Pub
Copy-Item "$Repo\launch\repo-files\CONTRIBUTING.md" $Pub

# sanity: nothing rejected / private
Get-ChildItem -Recurse $Pub | Where-Object { $_.FullName -match 'yuyu|悠悠|mascot\.png|mascot_original|icon_source|_work' } | Select-Object FullName
# expected: no output

cd $Pub
git init -b main
git add -A
git -c user.name="v900106" -c user.email="victor900106@gmail.com" commit -m "自在投影 Zizai Cast $Ver" -m "Source snapshot of $Commit (private history not published)."
git remote add origin "https://github.com/$Owner.git"
```

Check the result locally before pushing: open `README.md` / `README.en.md` in a
Markdown preview (all images are relative paths under `docs/readme/`, and
`LICENSE`, `docs/licenses/*`, `assets/public/toutou/` exist in the snapshot).

**Push (replaces the README-only history on GitHub — intentional):**

```powershell
git push --force -u origin main
```

Later releases: repeat steps 1–2 and push as a normal (non-force) commit on top
of the public `main`, e.g. by cloning the public repo into `$Pub`, deleting
everything except `.git`, expanding the new source zip into it and committing.

## 3. Repository settings

Text is in `launch/GITHUB_SETTINGS.md`.

```powershell
gh repo edit $Owner `
  --description "自在投影：免費開源的 Windows 手機投影 — iPhone/iPad 用 AirPlay 免裝 App，Android 用投放(Miracast)或無線偵錯並可用電腦操控；錄影、截圖、低延遲、最高 4K60。Free open-source AirPlay receiver & Android screen mirroring for Windows." `
  --homepage "https://victor900106.github.io/ZizaiCast/" `
  --enable-issues --enable-wiki=false
gh repo edit $Owner --add-topic airplay,airplay-receiver,screen-mirroring,screen-mirror,iphone,ipad,android,miracast,scrcpy,adb,wireless-debugging,windows,windows-11,screen-recorder,cpp,directx,uxplay,mirroring,cast,taiwan
```

* **Social preview** has no API: Settings → General → Social preview → upload
  `launch/assets/social-preview.png` (1280×640).
* Labels used by the issue forms: `bug`, `needs-triage`, `enhancement`,
  `source-request` —
  `gh label create needs-triage -R $Owner -c FBCA04; gh label create source-request -R $Owner -c 0E8A16`
  (`bug` / `enhancement` exist by default).
* Set the homepage only after Pages works (step 5), or leave it on the releases page.

## 4. Release v0.6.0

```powershell
cd $Repo
New-Item -ItemType Directory -Force "$env:TEMP\zc-rel" | Out-Null
Copy-Item "installer\Output\自在投影-安裝程式-$Ver.exe" "$env:TEMP\zc-rel\zizai-setup-$Ver.exe"
$Sha = (Get-FileHash "$env:TEMP\zc-rel\zizai-setup-$Ver.exe" -Algorithm SHA256).Hash.ToLower()
@{
  version = $Ver
  url     = "https://github.com/$Owner/releases/download/v$Ver/zizai-setup-$Ver.exe"
  sha256  = $Sha
  notes   = "中文 / English 介面、關於視窗、新吉祥物投投、授權與原始碼。"
} | ConvertTo-Json | Out-File -Encoding utf8 "$env:TEMP\zc-rel\update.json"
# UTF-8 with BOM (PowerShell 5.1) is fine: app/updater.cpp strips a BOM
```

Release notes: fill the template in `launch/GITHUB_SETTINGS.md` (zh first, then
en) into `$env:TEMP\zc-rel\notes.md`, including `$Sha`.

The existing draft: delete it or edit it (`gh release list -R $Owner`).

```powershell
gh release create "v$Ver" -R $Owner --target main --title "自在投影 v$Ver" --notes-file "$env:TEMP\zc-rel\notes.md" --latest `
  "$env:TEMP\zc-rel\zizai-setup-$Ver.exe" `
  "$env:TEMP\zc-rel\update.json" `
  "build-release\ZizaiCast-$Ver-source.zip" `
  "build-release\ZizaiCast-$Ver-deps-source.zip"
```

Notes:

* The tag is created on the public orphan `main` commit (the one that equals the
  source zip), which is what the GPL "same place" offer points at.
* `update.json` is fetched from `releases/latest/download/update.json`, so this
  release must be marked **Latest** (not pre-release, not draft).
* Verify after publishing:
  - `Invoke-WebRequest https://github.com/$Owner/releases/latest/download/update.json` → the JSON above;
  - an installed 0.5.x shows 「更新到 v0.6.0」, downloads, logs `sha256 ok`
    (or a `--dev --test-no-install` build with an older `PM_APP_VERSION_OVERRIDE`);
  - the four assets download; the README badges (release / downloads / license) resolve.

## 5. GitHub Pages (optional)

The landing page is self-contained in `launch/site/` (`index.html` + `img/`).

```powershell
$Site = "$env:TEMP\zc-site"
Remove-Item -Recurse -Force $Site -ErrorAction SilentlyContinue
Copy-Item -Recurse "$Repo\launch\site" $Site
cd $Site
New-Item .nojekyll -ItemType File | Out-Null
git init -b gh-pages; git add -A
git -c user.name="v900106" -c user.email="victor900106@gmail.com" commit -m "Landing page"
git remote add origin "https://github.com/$Owner.git"
git push -f origin gh-pages
gh api -X POST "repos/$Owner/pages" -f "source[branch]=gh-pages" -f "source[path]=/"
```

→ `https://victor900106.github.io/ZizaiCast/` (a few minutes). Then set the
repository homepage to it (step 3). `og:image` is relative (`img/og.jpg`); if a
link preview needs an absolute URL, change it to
`https://victor900106.github.io/ZizaiCast/img/og.jpg`.

## 6. After publishing

* Pin the repo on the profile; enable Discussions if desired.
* Follow `launch/STRATEGY.md` for announcements (posts in `launch/posts/`).
* Never push the private repository or its history (`git push` only from `$Pub`
  / `$Site`).
