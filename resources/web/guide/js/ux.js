function CancelGuide() {
    SendWXMessage(JSON.stringify({ sequence_id: Math.round(Date.now() / 1000), command: 'user_guide_cancel' }));
}

// Guide-local strings missing from the shared legacy translation table.
Object.assign(LangText.en, { uxDeferSetup: 'Set up later', uxLoadProfiles: 'Loading printer and filament profiles…',
    uxLoadSlow: 'Profiles are still loading. You can keep waiting or cancel and reopen setup.',
    uxLoadUnavailable: 'This setup page is unavailable. Cancel and reopen setup.', uxFilter: 'Filter materials', uxFiltered: 'Filtered items',
    uxVisible: 'Visible items', uxChecked: 'Selected', uxUnchecked: 'Unselected' });
Object.assign(LangText.zh_CN, { uxDeferSetup: '稍后配置', uxLoadProfiles: '正在加载打印机和耗材配置…',
    uxLoadSlow: '配置仍在加载。可以继续等待，或取消后重新打开设置。',
    uxLoadUnavailable: '无法打开此设置页面，请取消后重新打开设置。', uxFilter: '筛选耗材', uxFiltered: '筛选结果',
    uxVisible: '当前可见', uxChecked: '已选择', uxUnchecked: '未选择' });
Object.assign(LangText.zh_TW, { uxDeferSetup: '稍後設定', uxLoadProfiles: '正在載入印表機和耗材設定…',
    uxLoadSlow: '設定仍在載入。可以繼續等待，或取消後重新開啟設定。',
    uxLoadUnavailable: '無法開啟此設定頁面，請取消後重新開啟設定。', uxFilter: '篩選耗材', uxFiltered: '篩選結果',
    uxVisible: '目前可見', uxChecked: '已選擇', uxUnchecked: '未選擇' });

// Existing generated div controls need the same keyboard affordance as native inputs.
document.addEventListener('DOMContentLoaded', () => {
    function prepareControls(root) {
        root.querySelectorAll('div[onclick]').forEach(control => {
            if (control.classList.contains('back')) return;
            control.setAttribute('role', 'button');
            control.tabIndex = 0;
        });
    }
    prepareControls(document);
    new MutationObserver(changes => {
        if (changes.some(change => change.addedNodes.length)) prepareControls(document);
    }).observe(document.body, { childList: true, subtree: true });
});
document.addEventListener('keydown', event => {
    const control = event.target.closest('[role="button"]');
    if (control && event.target === control && (event.key === 'Enter' || event.key === ' ')) {
        event.preventDefault();
        control.click();
    }
}, true);
