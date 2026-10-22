// 0.8.4 (feat-084-privacy): 隱私黑屏 + 電腦的檔案 on the phone page. A file of its own (inlined by the PC like
// trackpad.js / vkbd.js), loaded after the page's main script and using its globals ($, t, T, S, send, toast,
// BASE, closeSheets, syncSheetFlag, mb). index.html only has two hooks: this <script> and one line in onDc().
// Everything it shows is added here (toolbar buttons, the tabs in the 檔案 sheet, its CSS).
//
// Transfers go through P84.transfer (today: the PC's one-time dl/<key> over HTTP, the browser saves the file,
// the PC reports the bytes sent). A DataChannel transfer can replace start() without touching the rest.
(() => {
  'use strict';
  const S84 = {
    pvBtn: ['黑幕', 'Cover', '画面隠し', '가림'],
    pvOn: ['電腦螢幕已遮住：電腦前看到「正在遠端操控中」，你這邊照常', 'The PC screen is covered: people at the PC see “Remote control in progress”; you still see everything',
      'パソコンの画面を隠しました（パソコンの前では「リモート操作中」と表示、こちらは普段どおり）', 'PC 화면을 가렸습니다. PC 앞에서는 “원격 제어 중”이 보이고, 여기서는 그대로 보입니다'],
    pvOff: ['已解除電腦的黑幕', 'The PC screen is no longer covered', 'パソコンの画面隠しを解除しました', 'PC 화면 가림을 해제했습니다'],
    pvLocal: ['電腦前的人解除了黑幕，這次連線不能再開', 'Someone at the PC removed the cover; it cannot be turned on again in this connection',
      'パソコンの前の人が画面隠しを解除しました。この接続ではもう使えません', 'PC 앞의 사람이 가림 화면을 해제했습니다. 이번 연결에서는 다시 켤 수 없습니다'],
    pvFailed: ['電腦無法顯示黑幕', 'The PC could not show the cover', 'パソコンで画面隠しを表示できませんでした', 'PC에서 가림 화면을 표시하지 못했습니다'],
    pvViewOnly: ['「僅觀看」時不能開黑幕', 'The cover is off in “View only”', '「表示のみ」では画面隠しを使えません', '“보기 전용”에서는 가림 화면을 쓸 수 없습니다'],
    pvGuest: ['臨時帳號不能開黑幕（電腦前的人要看得到你在做什麼）', 'Temporary accounts cannot cover the PC screen (the person at the PC must see what happens)',
      '一時アカウントでは画面を隠せません（パソコンの前の人が操作を見られるように）', '임시 계정은 화면을 가릴 수 없습니다(PC 앞의 사람이 작업을 볼 수 있어야 합니다)'],
    pvOffPc: ['電腦端沒有開放黑幕', 'The PC does not allow the cover', 'パソコン側で画面隠しが許可されていません', 'PC에서 가림 화면을 허용하지 않았습니다'],
    pvHint: ['電腦前按 Ctrl+Alt+Shift+B 或連按三次 Esc 可以解除', 'At the PC, Ctrl+Alt+Shift+B or Esc three times removes it', 'パソコンの前で Ctrl+Alt+Shift+B か Esc 3 回で解除できます', 'PC 앞에서 Ctrl+Alt+Shift+B 또는 Esc 세 번으로 해제됩니다'],
    fsTabMine: ['傳檔', 'Transfer', '送受信', '전송'],
    fsTabPc: ['電腦的檔案', 'PC files', 'パソコンのファイル', 'PC 파일'],
    fsHint: ['只能看電腦允許的資料夾。資料夾不能整包下載，可以勾選多個檔案。', 'Only the folders the PC allows. Folders cannot be downloaded as a whole; tick several files instead.',
      'パソコンが許可したフォルダーだけ見られます。フォルダーごとのダウンロードはできません。複数のファイルを選べます。', 'PC가 허용한 폴더만 볼 수 있습니다. 폴더 전체는 받을 수 없고, 여러 파일을 선택할 수 있습니다.'],
    fsRoots: ['最上層', 'Top level', '最上位', '최상위'],
    fsGuestOff: ['電腦端沒有開放臨時帳號瀏覽電腦的檔案', 'The PC does not let temporary accounts browse its files', 'パソコン側で一時アカウントによるファイルの閲覧が許可されていません', 'PC에서 임시 계정의 파일 열람을 허용하지 않았습니다'],
    fsUp: ['上一層', 'Up', '上へ', '위로'],
    fsEmpty: ['這裡沒有檔案', 'Nothing here', 'ファイルはありません', '파일이 없습니다'],
    fsMore: ['項目太多，只顯示前面一部分', 'Too many items: only the first ones are shown', '項目が多いため一部だけ表示しています', '항목이 많아 일부만 표시합니다'],
    fsGet: ['下載選取的 ({0})', 'Download selected ({0})', '選んだファイルをダウンロード ({0})', '선택한 파일 받기 ({0})'],
    fsPick: ['勾選要下載的檔案', 'Tick the files to download', 'ダウンロードするファイルを選んでください', '받을 파일을 선택하세요'],
    fsWait: ['下載中…', 'Downloading…', 'ダウンロード中…', '받는 중…'],
    fsDone: ['完成', 'Done', '完了', '완료'],
    fsCut: ['中斷了', 'Stopped', '中断しました', '중단됨'],
    fsTap: ['點這裡下載', 'Tap to download', 'タップしてダウンロード', '눌러서 받기'],
    fsNo: ['不能下載這個檔案（不在允許的資料夾內，或已不存在）', 'This file cannot be downloaded (outside the allowed folders, or gone)',
      'このファイルはダウンロードできません（許可されたフォルダーの外か、もうありません）', '이 파일은 받을 수 없습니다(허용된 폴더 밖이거나 없음)'],
    fsBad: ['這個位置不能開啟', 'This place cannot be opened', 'この場所は開けません', '이 위치는 열 수 없습니다'],
    fsOff: ['電腦端沒有開放「電腦的檔案」', 'The PC does not share its files', 'パソコン側で「パソコンのファイル」が許可されていません', 'PC에서 “PC 파일”을 허용하지 않았습니다'],
    rDesktop: ['桌面', 'Desktop', 'デスクトップ', '바탕 화면'],
    rDocuments: ['文件', 'Documents', 'ドキュメント', '문서'],
    rDownloads: ['下載', 'Downloads', 'ダウンロード', '다운로드'],
    rPictures: ['圖片', 'Pictures', 'ピクチャ', '사진'],
    rVideos: ['影片', 'Videos', 'ビデオ', '동영상'],
    rShots: ['截圖', 'Screenshots', 'スクリーンショット', '스크린샷'],
    rRecordings: ['錄影', 'Recordings', '録画', '녹화'],
  };
  Object.assign(T, S84);
  const st = {privacy: false, ok: false, why: '', supported: false, pcfs: false, pcfsWhy: '', asked: false};
  const P84 = window.P84 = {st, keys: Object.keys(S84), stats: {lists: 0, keys: 0, started: 0, done: 0, cut: 0, refused: 0, bytes: 0}};

  // ---- CSS ----
  const css = document.createElement('style');
  css.textContent = `
#bPriv svg,#pbPriv svg{fill:none}
#fsTabs{display:flex;gap:6px;margin:0 0 10px}
#fsTabs button{flex:1 1 0;padding:8px 6px;border-radius:10px;border:1px solid rgba(255,255,255,.18);background:rgba(255,255,255,.05);color:var(--fg);font-size:14px}
#fsTabs button.on{background:var(--ac);border-color:var(--ac);color:#2a1a1c;font-weight:600}
#fsPc .crumb{display:flex;align-items:center;gap:8px;margin:2px 0 6px;font-size:13px;min-height:30px}
#fsPc .crumb .path{flex:1 1 auto;min-width:0;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;opacity:.85;direction:rtl;text-align:left}
#fsPc .crumb button{padding:5px 10px;border-radius:8px;border:1px solid rgba(255,255,255,.2);background:none;color:var(--fg);font-size:13px}
#fsPc .fl{max-height:42vh}
#fsPc .fl li{cursor:pointer}
#fsPc .fl li.dir .nm::before{content:'📁 '}
#fsPc .fl li.root .nm::before{content:'📂 '}
#fsPc .fl input{width:20px;height:20px;accent-color:var(--ac);flex:0 0 auto;margin:0}
#fsPc .fl li.sel{background:rgba(245,167,167,.10)}
#fsPc .fl .gone{opacity:.6;font-size:12px}
#fsGetRow{display:flex;gap:8px;align-items:center;padding-top:8px}
#fsGet{flex:1 1 auto}
#fsGet:disabled{opacity:.45}
#fsDl{max-height:22vh!important}
#fsDl a{color:var(--ac)}
`;
  document.head.appendChild(css);

  // ---- toolbar: 黑幕 (phone bar and the PC browser's bar) ----
  const ICON = '<svg viewBox="0 0 24 24"><rect x="3" y="4" width="18" height="12" rx="2"/><path d="M8 20h8M12 16v4"/><path d="M7 8l10 4M7 12l10-4" stroke-opacity=".9"/></svg>';
  const bar = $('bar'), setBtn = $('bSet');
  const bPriv = document.createElement('button');
  bPriv.className = 'ib'; bPriv.id = 'bPriv'; bPriv.style.display = 'none';
  bPriv.innerHTML = ICON + '<span class="bl" id="lbPriv"></span>';
  if (bar && setBtn) bar.insertBefore(bPriv, setBtn);
  $('lbPriv').textContent = t('pvBtn');
  bPriv.title = t('pvBtn') + ' — ' + t('pvHint');
  bPriv.setAttribute('aria-label', t('pvBtn'));
  let pbPriv = null;
  const pbSet = $('pbSet');
  if (pbSet && pbSet.parentNode) {
    pbPriv = document.createElement('button');
    pbPriv.id = 'pbPriv'; pbPriv.style.display = 'none';
    pbPriv.innerHTML = ICON + '<span class="pl"></span>';
    pbPriv.querySelector('.pl').textContent = t('pvBtn');
    pbPriv.title = t('pvBtn') + ' — ' + t('pvHint');
    pbSet.parentNode.insertBefore(pbPriv, pbSet);
  }
  const whyKey = w => w === 'viewOnly' ? 'pvViewOnly' : w === 'guest' ? 'pvGuest' : w === 'local' ? 'pvLocal' : w === 'off' ? 'pvOffPc' : 'pvFailed';
  function syncPriv() {
    for (const b of [bPriv, pbPriv]) {
      if (!b) continue;
      b.style.display = st.supported ? '' : 'none';
      b.classList.toggle('on', st.privacy);
      b.classList.toggle('off', !st.privacy && !st.ok);
      b.setAttribute('aria-pressed', st.privacy ? 'true' : 'false');
    }
  }
  const togglePriv = () => {
    if (!st.privacy && !st.ok) { toast(t(whyKey(st.why)), 5000); return; }
    st.pending = !st.privacy;
    send({t: 'privacy', on: !st.privacy});
  };
  bPriv.onclick = togglePriv;
  if (pbPriv) pbPriv.onclick = togglePriv;

  // ---- 檔案 sheet: two tabs (傳檔 = what was there, 電腦的檔案) ----
  const sheet = $('fileS'), h3 = sheet.querySelector('h3');
  const tabs = document.createElement('div'); tabs.id = 'fsTabs';
  const tMine = document.createElement('button'), tPc = document.createElement('button');
  tMine.textContent = t('fsTabMine'); tPc.textContent = t('fsTabPc'); tMine.id = 'fsTabMine'; tPc.id = 'fsTabPc';
  tabs.append(tMine, tPc);
  const mine = document.createElement('div'); mine.id = 'fsMine';
  while (h3.nextSibling) mine.appendChild(h3.nextSibling);
  const pcv = document.createElement('div'); pcv.id = 'fsPc'; pcv.style.display = 'none';
  pcv.innerHTML = '<span class="sub" id="fsHint"></span><div class="crumb"><button id="fsUp"></button><span class="path" id="fsPath"></span></div>' +
    '<ul class="fl" id="fsList"></ul><div id="fsGetRow"><button class="btn" id="fsGet" disabled></button></div><ul class="fl" id="fsDl"></ul>';
  sheet.append(tabs, mine, pcv);
  $('fsHint').textContent = t('fsHint');
  $('fsUp').textContent = '‹ ' + t('fsUp');
  const nav = {root: '', path: '', roots: [], sel: new Set(), entries: []};
  function showTab(pc) {
    tMine.classList.toggle('on', !pc); tPc.classList.toggle('on', pc);
    mine.style.display = pc ? 'none' : ''; pcv.style.display = pc ? '' : 'none';
    if (pc && !nav.root) send({t: 'pcfs-roots'});
  }
  tMine.onclick = () => showTab(false);
  tPc.onclick = () => showTab(true);
  function syncTabs() {
    tabs.style.display = st.pcfs ? '' : 'none';
    if (!st.pcfs) showTab(false);
  }
  showTab(false); syncTabs();

  const rootName = r => {
    const k = {desktop: 'rDesktop', documents: 'rDocuments', downloads: 'rDownloads', pictures: 'rPictures', videos: 'rVideos', shots: 'rShots', recordings: 'rRecordings'}[r.kind];
    return k ? t(k) + (r.kind === 'shots' || r.kind === 'recordings' ? ' · ' + (PM.app || '') : '') : r.label;
  };
  const curRoot = () => nav.roots.find(r => r.id === nav.root);
  function syncGet() {
    const n = nav.sel.size;
    $('fsGet').disabled = !n;
    $('fsGet').textContent = n ? tf('fsGet', n) : t('fsPick');
  }
  function crumb() {
    const r = curRoot();
    $('fsUp').style.visibility = nav.root ? '' : 'hidden';
    $('fsPath').textContent = nav.root ? t('fsRoots') + ' / ' + (r ? rootName(r) : nav.root) + (nav.path ? ' / ' + nav.path.split('/').join(' / ') : '') : t('fsRoots');
  }
  const li = (cls, name, right) => {
    const e = document.createElement('li'), nm = document.createElement('span');
    if (cls) e.className = cls;
    nm.className = 'nm'; nm.textContent = name; e.appendChild(nm);
    if (right !== undefined) { const s = document.createElement('span'); s.className = 'st'; s.textContent = right; e.appendChild(s); }
    return e;
  };
  function showRoots() {
    nav.root = ''; nav.path = ''; nav.sel.clear(); crumb(); syncGet();
    const ul = $('fsList'); ul.textContent = '';
    for (const r of nav.roots) {
      const e = li('root', rootName(r));
      e.dataset.root = r.id;
      e.onclick = () => open(r.id, '');
      ul.appendChild(e);
    }
    if (!nav.roots.length) ul.appendChild(li('sub', t('fsEmpty')));
  }
  function open(root, path) {
    nav.root = root; nav.path = path; nav.sel.clear(); crumb(); syncGet();
    send({t: 'pcfs-list', root, path});
  }
  $('fsUp').onclick = () => {
    if (!nav.root) return;
    if (!nav.path) return showRoots();
    const p = nav.path.split('/'); p.pop();
    open(nav.root, p.join('/'));
  };
  const join = n => nav.path ? nav.path + '/' + n : n;
  function showList(m) {
    if (m.root !== nav.root || m.path !== nav.path) return;
    P84.stats.lists++;
    const ul = $('fsList'); ul.textContent = '';
    if (m.err) { ul.appendChild(li('sub', t('fsBad'))); return; }
    nav.entries = m.entries || [];
    for (const x of nav.entries) {
      if (x.d) {
        const e = li('dir', x.n);
        e.onclick = () => open(nav.root, join(x.n));
        ul.appendChild(e);
        continue;
      }
      const e = li('file', x.n, mb(x.s)), cb = document.createElement('input');
      cb.type = 'checkbox';
      e.prepend(cb);
      const p = join(x.n);
      const flip = on => { if (on) nav.sel.add(p); else nav.sel.delete(p); cb.checked = on; e.classList.toggle('sel', on); syncGet(); };
      cb.onclick = ev => { ev.stopPropagation(); flip(cb.checked); };
      e.onclick = () => flip(!nav.sel.has(p));
      ul.appendChild(e);
    }
    if (!nav.entries.length) ul.appendChild(li('sub', t('fsEmpty')));
    if (m.more) ul.appendChild(li('sub', t('fsMore')));
  }
  $('fsGet').onclick = () => {
    if (!nav.sel.size) return;
    const items = Array.from(nav.sel);
    for (let i = 0; i < items.length; i += 20) send({t: 'pcfs-get', root: nav.root, items: items.slice(i, i + 20)});
    nav.sel.clear();
    for (const cb of $('fsList').querySelectorAll('input')) { cb.checked = false; cb.parentNode.classList.remove('sel'); }
    syncGet();
  };

  // ---- transfers (the one piece to swap for a DataChannel transfer) ----
  const rows = new Map();  // key -> {li, pr, st, size}
  P84.transfer = {
    // Today: the PC's one-time link; the browser saves the file. Later: a DataChannel transfer with the same rows.
    start(k, name) {
      const a = document.createElement('a');
      a.href = BASE + 'dl/' + encodeURIComponent(k); a.download = name; a.rel = 'noopener'; a.style.display = 'none';
      document.body.appendChild(a); a.click(); setTimeout(() => a.remove(), 1000);
    },
  };
  let queue = Promise.resolve();
  function addDownloads(list) {
    const ul = $('fsDl');
    for (const f of list) {
      if (!f.k) {
        P84.stats.refused++;
        const e = li('', (f.p || '').split('/').pop(), ''); e.querySelector('.st').textContent = t('fsNo'); e.querySelector('.st').className = 'st gone';
        ul.prepend(e); continue;
      }
      P84.stats.keys++;
      const e = li('', f.n), pr = document.createElement('progress'), s = document.createElement('span');
      pr.max = 1; pr.value = 0; s.className = 'st'; s.textContent = mb(f.s);
      e.append(pr, s); ul.prepend(e);
      const row = {li: e, pr, st: s, size: f.s, started: false};
      rows.set(f.k, row);
      // One after another (browsers ask before several downloads at once); a tap on the row starts it if the browser held it back.
      queue = queue.then(() => new Promise(res => {
        if (!row.started) { row.started = true; P84.stats.started++; s.textContent = t('fsWait'); P84.transfer.start(f.k, f.n); }
        setTimeout(res, 700);
      }));
      e.onclick = () => { if (row.started && !row.got) { row.got = 0; P84.transfer.start(f.k, f.n); } };
    }
  }
  function progress(m) {
    const row = rows.get(m.k);
    if (!row) return;
    row.got = m.got;
    row.pr.value = m.total ? m.got / m.total : 1;
    if (m.end) {
      row.pr.remove();
      const ok = m.got === m.total;
      row.st.textContent = ok ? t('fsDone') + ' · ' + mb(m.total) : t('fsCut');
      if (ok) { P84.stats.done++; P84.stats.bytes += m.total; } else P84.stats.cut++;
      rows.delete(m.k);
    } else row.st.textContent = Math.round(m.got * 100 / Math.max(1, m.total)) + '% · ' + mb(m.got);
  }

  // ---- messages (called first by the page's onDc; true = handled here) ----
  P84.onMsg = m => {
    if (m.t === 'mode') { send({t: 'p84-get'}); return false; }  // the page handles it too; then ask what may be used now
    if (m.t === 'p84') {
      const was = st.privacy;
      st.privacy = !!m.privacy; st.ok = !!m.privacyOk; st.why = m.privacyWhy || ''; st.supported = !!m.privacySupported;
      st.pcfs = !!m.pcfs; st.pcfsWhy = m.pcfsWhy || '';
      if (m.ok === false && m.why) toast(t(whyKey(m.why)), 6000, true);
      else if (m.why === 'local' && was && !st.privacy) toast(t('pvLocal'), 8000, true);
      else if (m.why === 'failed' && was && !st.privacy) toast(t('pvFailed'), 6000, true);
      else if (st.pending !== undefined && st.privacy === st.pending) toast(t(st.privacy ? 'pvOn' : 'pvOff'), st.privacy ? 6000 : 3000);
      st.pending = undefined;
      syncPriv(); syncTabs();
      return true;
    }
    if (m.t === 'pcfs-roots') { nav.roots = m.roots || []; showRoots(); return true; }
    if (m.t === 'pcfs-list') { showList(m); return true; }
    if (m.t === 'pcfs-keys') { addDownloads(m.list || []); return true; }
    if (m.t === 'pcfs-prog') { progress(m); return true; }
    if (m.t === 'pcfs-err') { toast(m.why === 'off' ? t('fsOff') : m.why === 'guestOff' ? t('fsGuestOff') : t(filesDenyKey(m.why === 'guest' ? 'guest' : 'viewOnly')), 6000); showTab(false); return true; }
    return false;
  };
})();
