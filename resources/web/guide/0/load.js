var TargetPage = null;
var ProfileLoaded = false;
var PageReady = false;
var LoadingClosed = false;
var SlowLoadTimer = null;
var GuidePages = ['1', '11', '21', '22', '23', '24'];

function OnInit()
{
    PageReady = true;
    TargetPage = GetQueryString('target');
    TranslatePage();
    if (GuidePages.indexOf(TargetPage) === -1) {
        SetLoadingHint('uxLoadUnavailable');
        return;
    }
    if (ProfileLoaded) {
        JumpToTarget();
        return;
    }
    // A slow load is still a load: never enter the wizard without its real completion.
    SlowLoadTimer = setTimeout(function () {
        if (!LoadingClosed && !ProfileLoaded) SetLoadingHint('uxLoadSlow');
    }, 180 * 1000);
}

function SetLoadingHint(key)
{
    document.getElementById('LoadTip').setAttribute('tid', key);
    TranslatePage();
}

function HandleStudio(pVal)
{
    if (!LoadingClosed && pVal && pVal.command === 'userguide_profile_load_finish') {
        ProfileLoaded = true;
        JumpToTarget();
    }
}

function JumpToTarget()
{
    if (!PageReady || !ProfileLoaded || LoadingClosed || GuidePages.indexOf(TargetPage) === -1) return;
    LoadingClosed = true; // Completion, including duplicate/late callbacks, navigates once.
    clearTimeout(SlowLoadTimer);
    document.getElementById('LoadBlock').setAttribute('aria-busy', 'false');
    var language = GetQueryString('lang');
    window.open('../' + TargetPage + '/index.html' + (language ? '?lang=' + encodeURIComponent(language) : ''), '_self');
}

function CancelLoading()
{
    if (LoadingClosed) return;
    LoadingClosed = true;
    clearTimeout(SlowLoadTimer);
    CancelGuide(); // Existing draft owner handles cancellation; no preset choices are committed here.
}
