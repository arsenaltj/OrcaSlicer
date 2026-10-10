(function (root) {
  'use strict';
  const isModel = item => item.format === 'OBJ' || item.format === 'GLB';
  function filterItems(items, options = {}) {
    const query = String(options.query || '').trim().toLocaleLowerCase();
    const result = items.filter(item => {
      if (options.category === 'models' && !isModel(item)) return false;
      if (options.category === 'images' && isModel(item)) return false;
      if (options.format && options.format !== 'all' && item.format !== options.format) return false;
      return !query || [item.title, item.category, item.format].join(' ').toLocaleLowerCase().includes(query);
    });
    if (options.sort === 'size') result.sort((a, b) => a.bytes - b.bytes);
    else if (options.sort === 'title') result.sort((a, b) => a.title.localeCompare(b.title, 'zh-CN', { numeric: true }));
    return result;
  }
  function downloadURL(item, base) {
    if (!isModel(item)) return /^assets\/[a-z0-9-]+\.png$/i.test(item.image) ? 'https://arsenaltj.github.io/' + item.image : null;
    if (!/^[a-f0-9]{8}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{4}-[a-f0-9]{12}$/i.test(item.id)) return null;
    try {
      const origin = new URL(base);
      if (origin.protocol !== 'https:' || origin.username || origin.password || origin.pathname !== '/' || origin.search || origin.hash) return null;
      return new URL('/models/' + item.id, origin).href;
    } catch (_) { return null; }
  }
  function fileSize(bytes) {
    return bytes < 1024 * 1024 ? Math.ceil(bytes / 1024) + ' KB' : (bytes / (1024 * 1024)).toFixed(1) + ' MB';
  }
  const api = Object.freeze({ isModel, filterItems, downloadURL, fileSize });
  if (typeof module === 'object' && module.exports) module.exports = api;
  else root.OrcaGallery = api;
})(typeof globalThis !== 'undefined' ? globalThis : this);
