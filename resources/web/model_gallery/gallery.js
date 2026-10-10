(() => {
  'use strict';
  const $ = id => document.getElementById(id);
  const items = window.ORCA_GALLERY_CATALOG || [];
  const base = window.ORCA_GALLERY_DOWNLOAD_BASE || '';
  const state = { category: 'models', format: 'all', sort: 'default', query: '', download: 'checking', selected: null };
  const categories = [{ key: 'all', title: '全部作品', path: 'M3 3h7v7H3zM14 3h7v7h-7zM3 14h7v7H3zM14 14h7v7h-7z' }, { key: 'models', title: '3D 模型', path: 'm12 2 9 5v10l-9 5-9-5V7zM3 7l9 5 9-5M12 12v10' }, { key: 'images', title: '平面设计', path: 'M3 4h18v16H3zM3 16l6-6 4 4 3-3 5 5M16 7h1' }];
  let healthRevision = 0;

  function element(tag, className, text) {
    const node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }
  function showDetail(item) {
    state.selected = item;
    $('detail-title').textContent = item.title;
    $('detail-image').src = item.image;
    $('detail-image').alt = item.title;
    $('detail-kind').textContent = OrcaGallery.isModel(item) ? '3D 模型' : '平面设计';
    $('detail-category').textContent = item.category;
    $('detail-format').textContent = item.format;
    $('detail-size').textContent = OrcaGallery.fileSize(item.bytes);
    $('detail-help').textContent = OrcaGallery.isModel(item)
      ? '下载模型后，在“我的资产”导入，继续美颜、配色和切片。此文件不含 Orca 打印配置。'
      : '这是一张设计图，可查看原图，作为下一次 3D 创作的参考。';
    updateDownload();
    $('detail').showModal();
  }
  function updateDownload() {
    const item = state.selected;
    if (!item) return;
    const url = OrcaGallery.downloadURL(item, base);
    const blocked = !url || (OrcaGallery.isModel(item) && (state.download === 'offline' || state.download === 'checking'));
    const link = $('detail-download');
    if (blocked) link.removeAttribute('href'); else link.href = url;
    link.setAttribute('aria-disabled', String(blocked));
    link.tabIndex = blocked ? -1 : 0;
    link.textContent = blocked ? (state.download === 'checking' ? '正在检查下载服务…' : '模型下载暂不可用')
      : OrcaGallery.isModel(item) ? '浏览器下载 · ' + item.format + ' ↗' : '查看原图 · PNG ↗';
    $('detail-download-note').textContent = !OrcaGallery.isModel(item) ? '原图将在默认浏览器中打开。'
      : state.download === 'offline' ? '下载服务暂时不可用，可重新检查或在公开图库中重试。'
      : state.download === 'online' ? '文件将在默认浏览器中下载。'
      : '下载状态尚未确认，可在默认浏览器中尝试。';
  }
  function render() {
    const filtered = OrcaGallery.filterItems(items, state);
    document.querySelectorAll('.category-button').forEach(button => button.setAttribute('aria-pressed', String(button.dataset.category === state.category)));
    $('results-title').textContent = categories.find(x => x.key === state.category).title;
    $('result-count').textContent = filtered.length + ' 件作品';
    $('empty').hidden = filtered.length !== 0;
    $('clear-search').hidden = !state.query;
    $('reset-filters').hidden = state.category === 'models' && state.format === 'all' && !state.query && state.sort === 'default';
    const fragment = document.createDocumentFragment();
    for (const item of filtered) {
      const card = element('article', 'model-card');
      const preview = element('button', 'preview-button');
      preview.type = 'button'; preview.setAttribute('aria-label', '查看 ' + item.title + ' 详情');
      const image = element('img'); image.src = item.image; image.alt = item.title; image.loading = 'lazy'; image.decoding = 'async';
      preview.append(image, element('span', 'format-badge', item.format));
      preview.addEventListener('click', () => showDetail(item));
      const content = element('div', 'card-content');
      const title = element('button', 'card-title', item.title); title.type = 'button'; title.title = item.title;
      title.addEventListener('click', () => showDetail(item));
      const meta = element('div', 'card-meta'); meta.append(element('span', '', item.category), element('span', '', OrcaGallery.fileSize(item.bytes)));
      const bottom = element('div', 'card-bottom');
      const details = element('button', 'details-button', '查看详情'); details.type = 'button'; details.setAttribute('aria-label', '查看 ' + item.title + ' 详情');
      details.addEventListener('click', () => showDetail(item));
      bottom.append(element('span', 'kind-label', OrcaGallery.isModel(item) ? '模型文件' : '设计图片'), details);
      content.append(title, meta, bottom); card.append(preview, content); fragment.append(card);
    }
    $('grid').replaceChildren(fragment);
  }
  function resetFilters() {
    Object.assign(state, { category: 'all', format: 'all', query: '', sort: 'default' });
    $('search').value = ''; $('format').value = 'all'; $('sort').value = 'default'; render();
  }
  for (const category of categories) {
    const button = element('button', 'category-button'); button.type = 'button'; button.dataset.category = category.key;
    const icon = document.createElementNS('http://www.w3.org/2000/svg', 'svg'); icon.classList.add('category-icon'); icon.setAttribute('viewBox', '0 0 24 24'); icon.setAttribute('aria-hidden', 'true');
    const path = document.createElementNS('http://www.w3.org/2000/svg', 'path'); path.setAttribute('d', category.path); icon.append(path);
    const count = OrcaGallery.filterItems(items, { category: category.key }).length;
    button.append(icon, element('span', '', category.title), element('span', 'category-count', count));
    button.addEventListener('click', () => { state.category = category.key; render(); }); $('categories').append(button);
  }
  $('search').addEventListener('input', event => { state.query = event.target.value; render(); });
  $('format').addEventListener('change', event => { state.format = event.target.value; render(); });
  $('sort').addEventListener('change', event => { state.sort = event.target.value; render(); });
  $('clear-search').addEventListener('click', () => { state.query = ''; $('search').value = ''; render(); $('search').focus(); });
  $('reset-filters').addEventListener('click', resetFilters); $('empty-reset').addEventListener('click', resetFilters);
  $('close-detail').addEventListener('click', () => $('detail').close());
  $('detail').addEventListener('click', event => { if (event.target === $('detail')) { const rect = $('detail').getBoundingClientRect(); if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) $('detail').close(); } });
  $('detail-download').addEventListener('click', event => { if ($('detail-download').getAttribute('aria-disabled') === 'true') event.preventDefault(); });

  async function checkDownload() {
    const revision = ++healthRevision;
    state.download = 'checking'; $('check-download').disabled = true;
    $('download-status').textContent = '正在检查模型下载服务…'; $('download-status').dataset.state = 'checking'; updateDownload();
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), 6000);
    try {
      if (!OrcaGallery.downloadURL(items.find(OrcaGallery.isModel) || {}, base)) throw new Error('Invalid download endpoint');
      const response = await fetch(base.replace(/\/$/, '') + '/health', { credentials: 'omit', cache: 'no-store', signal: controller.signal });
      if (revision !== healthRevision) return;
      state.download = response.ok ? 'online' : 'offline';
    } catch (_) { if (revision === healthRevision) state.download = 'unknown'; }
    finally {
      clearTimeout(timer);
      if (revision === healthRevision) {
        $('check-download').disabled = false;
        $('download-status').dataset.state = state.download;
        $('download-status').textContent = state.download === 'online' ? '模型下载服务在线' : state.download === 'offline' ? '模型下载服务暂不可用' : '模型下载状态未确认';
        updateDownload();
      }
    }
  }
  $('check-download').addEventListener('click', checkDownload);
  render(); checkDownload();
})();
