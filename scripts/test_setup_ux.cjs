// Run with node --test scripts/test_setup_ux.cjs. Exercises the production guide scripts.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const vm = require('node:vm');
const fs = require('node:fs');
const path = require('node:path');
function guide(...files) {
    const messages = [], controls = new Map(), selected = [];
    const location = { href: '' }, timers = new Map();
    const context = vm.createContext({
        SendWXMessage: message => messages.push(JSON.parse(message)),
        LangText: { en: {}, zh_CN: {}, zh_TW: {} },
        TranslatePage: () => {}, GetQueryString: name => name === 'target' ? '1' : 'zh_CN',
        setTimeout: callback => { const id = timers.size + 1; timers.set(id, callback); return id; },
        clearTimeout: id => timers.delete(id),
        document: { location, addEventListener: () => {}, getElementById: id => {
            if (!controls.has(id)) controls.set(id, { value: '', attributes: {}, setAttribute(key, value) { this.attributes[key] = value; } });
            return controls.get(id);
        } },
        window: { location, open: url => { location.href = url; } },
        $: selector => selector === '#ItemBlockArea input:checked' ? selected : {
            show: () => controls.set(selector, true), hide: () => controls.set(selector, false)
        }
    });
    for (const file of files) vm.runInContext(fs.readFileSync(path.join(__dirname, '../resources/web/guide', file), 'utf8'), context);
    context.ShowNotice = value => { context.notice = value; };
    return { context, messages, controls, selected, location, timers };
}
test('region draft restores and sends the current choice before proceeding', () => {
    const g = guide('11/11.js');
    g.context.HandleRegionlList({ region: 'Europe' });
    assert.equal(g.controls.get('RegionSelect').value, 'Europe');
    g.context.ChooseRegion('China'); g.context.GotoPolicyPage();
    assert.equal(g.messages[0].region, 'China');
    assert.equal(g.location.href, '../21/index.html');
});
test('unknown region has an explicit supported default', () => {
    const g = guide('11/11.js'); g.context.HandleRegionlList({ region: 'unknown' });
    assert.equal(g.controls.get('RegionSelect').value, 'Asia-Pacific');
});
test('printer back preserves an empty draft while next explains the required selection', () => {
    const g = guide('21/common.js', '21/21.js');
    g.context.GotoFilamentPage();
    assert.equal(g.location.href, ''); assert.equal(g.context.notice, 1);
    g.context.ReturnRegionPage();
    assert.deepEqual(g.messages.at(-1).data, {});
    assert.equal(g.location.href, '../11/index.html');
});
test('printer next sends selected real model identities', () => {
    const g = guide('21/common.js', '21/21.js');
    g.context.ModelNozzleSelected = { vendor: { 'Actual printer': true, 'Not selected': false } };
    g.context.GotoFilamentPage();
    assert.deepEqual(g.messages[0].data, { 'Actual printer': { model: 'Actual printer' } });
    assert.equal(g.location.href, '../22/index.html');
});
test('filament back preserves an empty draft without allowing empty finish', () => {
    const g = guide('22/common.js', '22/22.js');
    g.context.FinishGuide(); assert.equal(g.messages.length, 0); assert.equal(g.context.notice, 1);
    g.context.ReturnPreviewPage();
    assert.deepEqual(g.messages[0].data.filament, []);
    assert.equal(g.location.href, '../21/index.html');
});
test('offline finish sends actual filament choices before finishing', () => {
    const g = guide('22/common.js', '22/22.js');
    g.context.m_ProfileItem = { network_plugin_install: '0' };
    g.context.InstallNetworkPlugin();
    assert.equal(g.controls.get('#AcceptBtn'), true);
    assert.equal(g.controls.get('#GotoNetPluginBtn'), false);
    g.selected.push({ getAttribute: () => 'Generic PLA @ printer; Generic PETG @ printer' });
    g.context.FinishGuide();
    assert.deepEqual(g.messages.map(m => m.command), ['save_userguide_filaments', 'user_guide_finish']);
    assert.deepEqual(g.messages[0].data.filament, ['Generic PLA @ printer', 'Generic PETG @ printer']);
});
test('a checked row without a preset identity cannot finish configuration', () => {
    const g = guide('22/common.js', '22/22.js');
    g.selected.push({ getAttribute: () => ' ; ' });
    g.context.FinishGuide();
    assert.equal(g.messages.length, 0); assert.equal(g.context.notice, 1);
});

test('material search hides keyboard targets without dropping checked choices and restores them when cleared', () => {
    const g = guide('22/common.js');
    const listeners = new Map();
    const makeRow = (name, checked) => {
        const input = { checked };
        return { style: {}, input, querySelector: selector => selector === 'input' ? input : { textContent: name } };
    };
    const rows = [makeRow('Generic ABS', false), makeRow('WonderMaker PLA', true)];
    const empty = { style: {} };
    const list = { querySelectorAll: () => rows, querySelector: () => empty };
    const filter = { value: '', closest: () => ({ querySelector: () => list }),
        addEventListener: (name, handler) => listeners.set(name, handler) };
    const controls = { '.cbr-filter-bar': filter, '.cbr-filter-mode-filter': { style: {} },
        '.cbr-filter-mode-visible': { style: {} }, '#filter-tags': { addEventListener: () => {} } };
    g.context.document.querySelectorAll = () => [];
    g.context.document.querySelector = selector => controls[selector];
    g.context.UpdateStats = () => {};
    g.context.addClearBtnEvents = () => {};
    g.context.initInputEvents();
    const search = value => { filter.value = value; listeners.get('input').call(filter); };
    search('Generic ABS');
    assert.equal(rows[0].style.visibility, 'visible');
    assert.equal(rows[1].style.visibility, 'hidden');
    assert.equal(rows[1].input.checked, true);
    search('::checked');
    assert.equal(rows[0].style.visibility, 'hidden');
    assert.equal(rows[1].style.visibility, 'visible');
    search('');
    assert.ok(rows.every(row => row.style.visibility === 'visible'));
    assert.equal(rows[1].input.checked, true);
});

test('deferring setup only cancels the draft without committing printer or region choices', () => {
    const g = guide('js/ux.js');
    g.context.CancelGuide();
    assert.deepEqual(g.messages.map(m => m.command), ['user_guide_cancel']);
    assert.equal(g.location.href, '');
});


test('slow loading explains the wait without entering an unfinished wizard; late completion navigates once', () => {
    const g = guide('js/ux.js', '0/load.js');
    g.context.OnInit();
    const slow = g.timers.values().next().value;
    slow();
    assert.equal(g.location.href, '');
    assert.equal(g.controls.get('LoadTip').attributes.tid, 'uxLoadSlow');
    g.context.HandleStudio({ command: 'response_userguide_profile' });
    assert.equal(g.location.href, '');
    g.context.HandleStudio({ command: 'userguide_profile_load_finish' });
    assert.equal(g.location.href, '../1/index.html?lang=zh_CN');
    assert.equal(g.controls.get('LoadBlock').attributes['aria-busy'], 'false');
    assert.equal(g.timers.size, 0);
    g.location.href = 'unchanged';
    slow();
    g.context.HandleStudio({ command: 'userguide_profile_load_finish' });
    assert.equal(g.location.href, 'unchanged');
});

test('completion before body initialization is not lost', () => {
    const g = guide('0/load.js');
    g.context.HandleStudio({ command: 'userguide_profile_load_finish' });
    assert.equal(g.location.href, '');
    g.context.OnInit();
    assert.equal(g.location.href, '../1/index.html?lang=zh_CN');
    assert.equal(g.timers.size, 0);
});

test('loading cancellation is cancel-only and rejects subsequent completion and timeout', () => {
    const g = guide('js/ux.js', '0/load.js');
    g.context.OnInit();
    const slow = g.timers.values().next().value;
    g.context.CancelLoading();
    g.context.CancelLoading();
    g.context.HandleStudio({ command: 'userguide_profile_load_finish' });
    slow();
    assert.deepEqual(g.messages.map(m => m.command), ['user_guide_cancel']);
    assert.equal(g.location.href, '');
    assert.equal(g.timers.size, 0);
});

test('unknown or missing loading target cannot enter another page even after completion', () => {
    for (const target of [null, '../22', '99']) {
        const g = guide('0/load.js');
        g.context.GetQueryString = () => target;
        g.context.OnInit();
        g.context.HandleStudio({ command: 'userguide_profile_load_finish' });
        assert.equal(g.location.href, '');
        assert.equal(g.controls.get('LoadTip').attributes.tid, 'uxLoadUnavailable');
        assert.equal(g.timers.size, 0);
    }
});

test('all existing wizard targets remain reachable only after actual completion', () => {
    for (const target of ['1', '11', '21', '22', '23', '24']) {
        const g = guide('0/load.js');
        g.context.GetQueryString = name => name === 'target' ? target : null;
        g.context.OnInit();
        assert.equal(g.location.href, '');
        g.context.HandleStudio({ command: 'userguide_profile_load_finish' });
        assert.equal(g.location.href, '../' + target + '/index.html');
    }
});
