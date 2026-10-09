const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const folder = path.resolve(__dirname, '../../resources/web/model_gallery');
const gallery = require(path.join(folder, 'gallery-model.js'));
const context = { window: {} };
vm.runInNewContext(fs.readFileSync(path.join(folder, 'catalog-data.js'), 'utf8'), context);
const catalog = JSON.parse(JSON.stringify(context.window.ORCA_GALLERY_CATALOG));
const base = context.window.ORCA_GALLERY_DOWNLOAD_BASE;

test('The bundled catalog preserves all public previews without packing model files', () => {
  assert.equal(catalog.length, 43);
  assert.equal(new Set(catalog.map(x => x.id)).size, catalog.length);
  for (const item of catalog) {
    assert.match(item.image, /^assets\/[a-z0-9-]+\.(jpg|png)$/);
    assert.ok(fs.statSync(path.join(folder, item.image)).size > 0);
    assert.ok(gallery.downloadURL(item, base));
  }
  assert.ok(fs.readdirSync(path.join(folder, 'assets')).every(name => /\.(jpg|png)$/.test(name)));
});

test('Category and format filters compose with Chinese search and clear empty results', () => {
  assert.equal(gallery.filterItems(catalog, { category: 'models' }).length, 31);
  assert.equal(gallery.filterItems(catalog, { category: 'images' }).length, 12);
  assert.equal(gallery.filterItems(catalog, { category: 'images', format: 'OBJ' }).length, 0);
  const fox = gallery.filterItems(catalog, { query: ' 狐狸探险家 ' });
  assert.equal(fox.length, 2);
  assert.equal(gallery.filterItems(catalog, { query: '狐狸探险家', category: 'models' }).length, 1);
  assert.equal(gallery.filterItems(catalog, { query: 'does-not-exist' }).length, 0);
  assert.equal(gallery.filterItems(catalog, { category: 'all', query: '' }).length, 43);
  assert.ok(gallery.filterItems(catalog, { format: 'GLB', query: 'glb' }).every(x => x.format === 'GLB'));
});

test('Sorting returns each filtered item once and leaves catalog order unchanged', () => {
  const originalIds = catalog.map(x => x.id);
  const ordered = gallery.filterItems(catalog, { category: 'models', sort: 'size' });
  assert.equal(new Set(ordered.map(x => x.id)).size, 31);
  for (let i = 1; i < ordered.length; i++) assert.ok(ordered[i - 1].bytes <= ordered[i].bytes);
  assert.deepEqual(catalog.map(x => x.id), originalIds);
});

test('Download links keep model identifiers and flat images in their allowed locations', () => {
  const model = catalog.find(gallery.isModel);
  assert.equal(gallery.downloadURL(model, base), base + '/models/' + model.id);
  const image = catalog.find(x => x.format === 'PNG');
  assert.equal(gallery.downloadURL(image, ''), 'https://arsenaltj.github.io/' + image.image);
  for (const invalid of ['file:///C:/data', 'javascript:alert(1)', 'http://models.example.com', 'https://user:password@models.example.com', 'https://models.example.com/path', 'https://models.example.com/?x=1'])
    assert.equal(gallery.downloadURL(model, invalid), null);
  assert.equal(gallery.downloadURL({ ...model, id: '../private' }, base), null);
  assert.equal(gallery.downloadURL({ ...image, image: '../private.png' }, base), null);
  assert.equal(gallery.downloadURL({ ...image, image: 'https://other.example/image.png' }, base), null);
});
