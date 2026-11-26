/* Installed local page. All platform and filesystem operations belong to C++. */
(() => {
  "use strict";
  const $ = (id) => document.getElementById(id),
    state = {
      nonce: "",
      documentGeneration: 0,
      accountGeneration: 0,
      requestGeneration: 0,
      connected: false,
      catalogQuery: "",
      scope: "library",
      filter: "all",
      category: "",
      format: "",
      sort: "newest",
      q: "",
      offset: 0,
      cloud: [],
      local: [],
      images: new Map(),
      cardImages: new Map(),
      recommendationImages: new Map(),
      detailImages: new Set(),
      accountAvatars: new Map(),
      avatarData: "",
      accountTag: "",
      taxonomy: [],
      signedIn: false,
      refreshing: false,
      loginRequired: false,
      revision: 0,
      detail: null,
      detailKey: "",
      stats: {},
      fileActions: null,
      selectedFile: "",
      download: null,
      comments: [],
      commentsLoaded: false,
      recommendationsLoaded: false,
      moreComments: false,
      following: null,
      mutation: "",
      listPosition: null,
      hasMore: false,
    };
  const icons = {
    search: '<circle cx="6" cy="6" r="4.5"/><path d="m9.5 9.5 4 4"/>',
    eye: '<path d="M1 7s2-4 6-4 6 4 6 4-2 4-6 4-6-4-6-4Z"/><circle cx="7" cy="7" r="1.5"/>',
    like: '<path d="M4 6 7 1c2 0 1 4 1 4h4l1 2-2 6H4ZM1 6h3v7H1Z"/>',
    heart: '<path d="M7 12C-4 5 3-1 7 4c4-5 11 1 0 8Z"/>',
    download: '<path d="M7 1v8m-3-3 3 3 3-3M1 9v4h12V9"/>',
    upload: '<path d="M7 10V1m-3 3 3-3 3 3M1 9v4h12V9"/>',
    share: '<circle cx="2.5" cy="7" r="1.5"/><circle cx="11.5" cy="2.5" r="1.5"/><circle cx="11.5" cy="11.5" r="1.5"/><path d="m4 6 6-3m-6 5 6 3"/>',
  };
  function icon(name) {
    const span = document.createElement("span");
    span.innerHTML =
      '<svg viewBox="0 0 14 14" aria-hidden="true">' + icons[name] + "</svg>";
    return span;
  }
  $("searchIcon").append(icon("search"));
  $("uploadIcon").append(icon("upload"));
  const geometryTransfer = new window.libraryState.GeometryTransfer();
  function closeViewer() {
    geometryTransfer.reset();
    window.libraryViewer?.close();
  }
  function el(tag, cls, text) {
    const n = document.createElement(tag);
    if (cls) n.className = cls;
    if (text !== undefined) n.textContent = String(text);
    return n;
  }
  function displayAvatar(node, data, fallback) {
    if (!window.libraryState.safeImage(data)) {
      node.textContent = fallback;
      return;
    }
    const image = el("img");
    image.src = data;
    image.alt = "";
    node.replaceChildren(image);
  }
  function updateAccountAvatar(data) {
    const png = typeof data === "string" && data.length <= 65536 &&
      data.startsWith("data:image/png;base64,") && window.libraryState.safeImage(data) ? data : "";
    if (png === state.avatarData) return;
    state.avatarData = png;
    for (const [node, fallback] of state.accountAvatars) displayAvatar(node, png, fallback);
  }
  function send(command, rest = {}) {
    if (!state.nonce || !window.wx?.postMessage || (command !== "ready" && !state.connected)) {
      notice("模型库尚未连接完成，请稍候或重新进入模型库。");
      return false;
    }
    if (command !== "ready") ++state.requestGeneration;
    window.wx.postMessage(JSON.stringify({nonce: state.nonce, command,
      documentGeneration: state.documentGeneration, accountTag: state.accountTag,
      accountGeneration: state.accountGeneration, requestGeneration: state.requestGeneration, ...rest}));
    return true;
  }
  function notice(text, login = false) {
    $("notice").replaceChildren(document.createTextNode(text));
    $("notice").hidden = !text;
    if (login) {
      const b = el("button", "", "登录账号");
      b.onclick = () => send("login");
      $("notice").append(" ", b);
    }
  }
  function toast(text) {
    $("toast").textContent = text;
    $("toast").hidden = false;
    clearTimeout(toast.timer);
    toast.timer = setTimeout(() => ($("toast").hidden = true), 6500);
  }
  function busy(active, stage = "", cancelable = false, importing = false) {
    $("app").inert = !!active && (!!cancelable || importing);
    $("more").disabled = !!active;
    $("loading").hidden = !active;
    $("loading").dataset.background = stage === "正在查询模型…" ? "true" : "false";
    $("loading").dataset.importing = importing ? "true" : "false";
    $("stage").textContent = stage;
    $("cancel").hidden = !cancelable;
    if (!active) $("bytes").textContent = "";
  }
  function query(reset = true) {
    if (reset) {
      state.offset = 0;
      $("listing").scrollTop = 0;
      state.listPosition = null;
      const identity = JSON.stringify([state.scope,state.filter,state.q,state.category,state.format,state.sort]);
      if (identity !== state.catalogQuery) state.cloud = [];
      state.catalogQuery = identity;
      window.libraryState.retainLocalImages(state.images, state.local);
    }
    state.detail = null;
    state.detailKey = "";
    state.download = null;
    closeViewer();
    $("detail").hidden = true;
    $("listing").hidden = false;
    notice("");
    render();
    $("more").disabled = true;
    send("query", {
      scope: state.scope,
      filter: state.filter,
      query: {
        q: state.q,
        category: state.scope === "library" ? state.category : "",
        ...(state.scope === "library" && state.format ? { format: state.format } : {}),
        sort: state.sort,
        offset: String(reset ? state.offset : state.offset + 24),
      },
    });
  }
  function filters() {
    const host = $("filters");
    host.replaceChildren();
    $("format").hidden = state.scope !== "library";
    let values;
    if (state.scope === "mine")
      values = [
        ["all", "全部"],
        ["models", "我的模型"],
        ["likes", "点赞"],
        ["favorites", "收藏"],
        ["downloads", "下载"],
        ["prints", "打印"],
        ["views", "浏览历史"],
      ];
    else
      values = [
        ["", "全部"],
        ...state.taxonomy
          .filter((x) => !x.parent)
          .map((x) => [x.slug, x.label]),
      ];
    for (const [value, label] of values) {
      const b = el(
        "button",
        (state.scope === "mine" ? state.filter : state.category) === value
          ? "selected"
          : "",
        label,
      );
      b.setAttribute("aria-pressed", b.className === "selected");
      b.onclick = () => {
        if (state.scope === "mine") state.filter = value;
        else state.category = value;
        filters();
        query();
      };
      host.append(b);
    }
  }
  function localAction(item, action) {
    send("local", { key: item.key, revision: state.revision, action });
  }
  function menu(item, anchor) {
    const host = $("menu");
    host.replaceChildren();
    const add = (label, fn) => {
      const b = el("button", "", label);
      b.setAttribute("role", "menuitem");
      b.onclick = () => {
        host.hidden = true;
        fn();
      };
      host.append(b);
    };
    if (item.local) {
      add(item.designOnly ? "打开设计图" : "打开模型", () => open(item));
      if (!item.designOnly && item.canDirectImport === true)
        add("导入切片", () => localAction(item, "import"));
    } else {
      add("模型详情", () => open(item));
    }
    host.hidden = false;
    const rect = anchor.getBoundingClientRect();
    host.style.left =
      Math.max(
        8,
        Math.min(
          rect.right - host.offsetWidth,
          innerWidth - host.offsetWidth - 8,
        ),
      ) + "px";
    host.style.top =
      Math.max(
        8,
        Math.min(rect.bottom + 6, innerHeight - host.offsetHeight - 8),
      ) + "px";
    host.querySelector("button")?.focus();
  }
  function open(item) {
    if (item.local) localAction(item, "open");
    else {
      if ($("detail").hidden) state.listPosition = listPosition();
      state.download = null;
      state.detailKey = item.key;
      state.fileActions = null;
      selectFile("");
      renderDownloadFeedback();
      send("detail", { key: item.key });
    }
  }
  function metric(name, count) {
    const n = el("span", "metric");
    n.append(
      icon(name),
      document.createTextNode(
        Number.isFinite(count) ? count.toLocaleString() : "—",
      ),
    );
    return n;
  }
  const frameUpdates = new window.libraryState.FrameUpdates(
    (callback) => window.requestAnimationFrame(callback), renderNow,
  );
  function render() {
    frameUpdates.request();
  }
  function listPosition() {
    const viewport = $("listing");
    const top = viewport.getBoundingClientRect().top;
    const card = Array.from($("grid").children).find((item) => item.getBoundingClientRect().bottom > top);
    return { scrollTop: viewport.scrollTop || 0, key: card?.dataset.key,
      offset: card ? card.getBoundingClientRect().top - top : 0 };
  }
  function restoreListPosition(position) {
    if (!position) return;
    const viewport = $("listing");
    const card = Array.from($("grid").children).find((item) => position.key && item.dataset.key === position.key);
    viewport.scrollTop = position.scrollTop;
    if (card && Number.isFinite(position.offset))
      viewport.scrollTop += card.getBoundingClientRect().top - viewport.getBoundingClientRect().top - position.offset;
  }
  function renderNow() {
    const position = $("listing").hidden ? null : listPosition();
    const items = window.libraryState.project(
        state.local,
        state.cloud,
        state.scope,
        state.filter,
        state.q,
        state.sort,
      ),
      grid = $("grid");
    grid.replaceChildren();
    state.cardImages.clear();
    state.accountAvatars.clear();
    for (const item of items) {
      const card = el("article", "card"),
        cover = el("button", "cover");
      card.dataset.key = item.key;
      cover.setAttribute("aria-label", "打开 " + item.title);
      cover.onclick = () => open(item);
      state.cardImages.set(item.key, cover);
      const image = state.images.get(item.key);
      if (image) {
        const img = el("img");
        img.src = image;
        img.alt = "";
        cover.append(img);
      } else
        cover.append(
          el(
            "span",
            "placeholder",
            item.designOnly ? "设计图" : item.local ? "本地模型" : "",
          ),
        );
      const overlay = el("div", "stats-overlay");
      const status = item.local
        ? ({ success: "打印成功", issue: "打印有问题" }[item.printFeedback] || "本机")
        : item.withdrawn ? "已撤回" : item.linkedLocal ? "已下载"
          : item.visibility === "public" && (!item.status || item.status === "published") ? "在线" : "";
      overlay.append(el("span", status === "在线" ? "status online" : "status", status));
      if (!item.local) {
        const counts = el("span", "counts");
        if (Number.isFinite(item.viewCount)) counts.append(metric("eye", item.viewCount));
        if (Number.isFinite(item.likeCount)) counts.append(metric("like", item.likeCount));
        overlay.append(counts);
      }
      cover.append(overlay);
      const caption = el("div", "caption"),
        title = el("div", "title-row"),
        name = el("button", "name", item.title);
      name.title = item.title;
      name.onclick = () => open(item);
      title.append(name);
      const category =
        state.taxonomy.find((x) => x.slug === item.category)?.label ||
        item.category;
      const tag = window.libraryState.statusLabel(item) || category;
      if (tag) title.append(el("span", "tag", tag));
      const footer = el("div", "card-footer"),
        author = el("span", "author");
      const authorName = item.local
        ? "本地资产"
        : item.author?.displayName || "作者";
      const avatar = el("span", "avatar");
      const useAccountAvatar = item.local || (state.scope === "mine" && item.owned === true);
      const fallback = authorName.slice(0, 1);
      if (useAccountAvatar) state.accountAvatars.set(avatar, fallback);
      displayAvatar(avatar, useAccountAvatar ? state.avatarData : item.author?.avatarUrl, fallback);
      author.append(avatar, document.createTextNode(authorName));
      footer.append(author);
      if (!item.local) {
        if (Number.isFinite(item.downloadCount)) footer.append(metric("download", item.downloadCount));
        if (Number.isFinite(item.favoriteCount)) footer.append(metric("heart", item.favoriteCount));
      }
      const more = el("button", "menu-button", "···");
      more.setAttribute("aria-label", item.title + " 的操作");
      more.onclick = (event) => {
        event.stopPropagation();
        menu(item, more);
      };
      footer.append(more);
      caption.append(title, footer);
      card.append(cover, caption);
      grid.append(card);
    }
    $("empty").hidden = items.length !== 0;
    $("emptySaveProject").hidden = !state.projectAvailable;
    $("emptyLogin").hidden =
      state.signedIn ||
      state.scope !== "mine" ||
      ["all", "models", "prints"].includes(state.filter);
    $("emptyText").textContent =
      state.scope === "mine" &&
      !["all", "models", "prints"].includes(state.filter) &&
      !state.signedIn
        ? "登录后查看账号记录"
        : "暂无模型";
    $("more").hidden = !state.hasMore;
    restoreListPosition(position);
  }
  function stats(value) {
    state.stats = value;
    $("like").replaceChildren(
      icon("like"),
      document.createTextNode(Number.isFinite(value.likeCount) ? " " + value.likeCount : " 点赞"),
    );
    $("favorite").replaceChildren(
      icon("heart"),
      document.createTextNode(Number.isFinite(value.favoriteCount) ? " " + value.favoriteCount : ""),
    );
    $("detailCounts").replaceChildren();
    for (const [symbol, count] of [["eye", value.viewCount], ["like", value.likeCount], ["download", value.downloadCount]])
      if (Number.isFinite(count)) $("detailCounts").append(metric(symbol, count));
    $("like").classList.toggle("active", !!value.liked);
    $("favorite").classList.toggle("active", !!value.favorited);
    updateCommunityButtons();
  }
  const usableFiles = () => (state.detail?.files || []).filter((file) =>
    window.libraryState.supported(file.filename) && (!file.status || file.status === "ready"));
  const downloadCancelable = () => ["checking", "transfer", "verify", "refreshing"].includes(state.download?.phase);
  const downloadActive = () => downloadCancelable() || state.download?.phase === "registering";

  function updateDownloadButtons() {
    const usable = usableFiles();
    const allDownloaded = usable.length > 0 && usable.every((file) => state.fileActions?.[file.id]);
    const disabled = !state.fileActions || !!state.mutation || downloadActive() || allDownloaded || !usable.length;
    const status = downloadActive() ? "下载中…" : !state.fileActions ? "正在检查…" : allDownloaded ? "已下载" :
      !usable.length ? "暂不可下载" : usable.length > 1 ? "选择文件" : "";
    $("downloadPreview").textContent = status || "下载";
    $("downloadDetail").textContent = status || "下载模型文件";
    $("downloadPreview").disabled = $("downloadDetail").disabled = disabled;
  }

  function downloadIssue(message) {
    $("downloadIssue").textContent = message;
    $("downloadIssue").hidden = !message;
  }

  function selectFile(file) {
    state.selectedFile = file;
    const actions = state.fileActions?.[file];
    $("openDownloaded").disabled = downloadActive() || actions?.open !== true;
    $("importDownloaded").disabled = downloadActive() || actions?.import !== true;
    $("openDownloaded").title = $("openDownloaded").disabled ? "请先下载可编辑的模型文件" : "在工作台编辑";
    $("importDownloaded").title = $("importDownloaded").disabled ? "请先下载可导入的模型文件" : "导入切片";
    const original = state.detail?.files.find((item) => item.id === file);
    $("downloadComplete").hidden = !actions || (state.download?.phase === "complete" && state.download.file === file);
    $("downloadComplete").textContent = original ? "已下载 · " + original.filename : "";
    renderFiles();
    updateDownloadButtons();
  }
  function renderFiles() {
    const host = $("files");
    host.replaceChildren();
    const usable = new Set(usableFiles().map((file) => file.id));
    const canSelect = Object.keys(state.fileActions || {}).length > 1;
    for (const file of state.detail?.files || []) {
      const row = el("div", "file");
      const size = Number(file.size);
      row.append(el("span", "", file.filename + (size > 0 ? " · " + (size / 1048576).toFixed(2) + " MB" : "")));
      if (state.fileActions?.[file.id]) {
        const actions = el("div", "file-actions");
        actions.append(el("span", "file-status", "已下载"));
        if (canSelect) {
          const selected = state.selectedFile === file.id;
          const select = el("button", "", selected ? "已选中" : "选择");
          select.disabled = selected;
          select.setAttribute("aria-label", (selected ? "已选中 " : "选择 ") + file.filename);
          select.onclick = () => selectFile(file.id);
          actions.append(select);
        }
        row.append(actions);
      } else {
        const button = el("button", "", usable.has(file.id) ? "下载" : "暂不可用");
        if (downloadActive() && state.download.file === file.id) button.textContent = "下载中…";
        button.disabled = !state.fileActions || !!state.mutation || downloadActive() || !usable.has(file.id);
        button.onclick = () => download(file.id);
        row.append(button);
      }
      host.append(row);
    }
  }
  function renderDownloadFeedback() {
    const download = state.download;
    const login = !state.refreshing && (download?.loginRequired || state.loginRequired);
    const footer = $("downloadFeedback");
    footer.hidden = (!download && !login) || $("detail").hidden;
    footer.dataset.phase = download?.phase || (login ? "failed" : "");
    const total = download?.total || 0;
    const active = downloadActive();
    const stages = {
      checking: total ? "正在验证下载资格…" : "正在获取文件大小…",
      transfer: "正在下载模型…",
      verify: "正在校验模型…",
      refreshing: "正在刷新登录状态…",
      registering: "正在登记本地模型…",
      complete: "下载完成",
      cancelled: "已取消下载",
    };
    const status = download?.message || stages[download?.phase] || (login ? "登录已失效，请重新登录。" : "");
    if ($("downloadPhase").textContent !== status) $("downloadPhase").textContent = status;
    const mb = (bytes) => (bytes / 1048576).toFixed(1) + "MB";
    $("downloadBytes").textContent = total ? mb(download.done) + "/" + mb(total) : "";
    const progress = $("downloadProgress");
    progress.hidden = !active && download?.phase !== "complete";
    if (total) progress.setAttribute("value", Math.min(download.done / total, 1));
    else progress.removeAttribute("value");
    $("cancelDownload").hidden = !downloadCancelable();
    $("cancelDownload").disabled = !!download?.cancelling;
    $("retryDownload").hidden = login || state.refreshing || !!state.fileActions?.[download?.file] ||
      !["failed", "cancelled"].includes(download?.phase);
    $("downloadLogin").hidden = !login;
  }
  function applyDownload(message) {
    const current = state.download;
    if (!current || message.key !== state.detailKey || message.key !== current.key || message.file !== current.file ||
        message.downloadRequest !== current.downloadRequest || !Number.isSafeInteger(message.downloadUpdate) ||
        message.downloadUpdate <= current.downloadUpdate) return;
    const wasActive = downloadActive();
    state.download = message;
    if (message.phase === "complete") {
      state.fileActions = { ...state.fileActions, [message.file]: message.actions };
      downloadIssue("");
      selectFile(message.file);
    } else if (wasActive !== downloadActive()) selectFile(state.selectedFile);
    renderDownloadFeedback();
  }
  function download(file) {
    const selected = usableFiles().find((item) => item.id === file);
    if (!selected || !state.fileActions || state.mutation || $("app").inert || downloadActive() || state.fileActions[file]) return;
    downloadIssue("");
    if (!send("download", { key: state.detailKey, file })) return;
    const total = typeof selected.size === "string" && !/^[0-9]{1,12}$/.test(selected.size) ? 0 : Number(selected.size);
    state.download = { key: state.detailKey, file, downloadRequest: state.requestGeneration, downloadUpdate: 0,
      phase: state.refreshing ? "refreshing" : "checking", done: 0, total: Number.isSafeInteger(total) && total > 0 ? total : 0 };
    selectFile(file);
    renderDownloadFeedback();
  }
  function chooseDownload() {
    if ($("downloadDetail").disabled || $("app").inert) return;
    const usable = usableFiles();
    if (usable.length === 1) {
      download(usable[0].id);
      return;
    }
    setFilePanel(true);
    $("filePanel").scrollIntoView?.({ block: "nearest" });
  }
  function setFilePanel(open) {
    $("filePanel").hidden = !open;
    $("filePanelToggle").setAttribute("aria-expanded", open);
  }

  function updateCommunityButtons() {
    const enabled = state.detail?.publicInteraction;
    $("follow").hidden = !enabled || state.detail.owned === true;
    $("follow").textContent = state.following === true ? "已关注" : "＋ 关注";
    $("follow").disabled = !!state.mutation || (state.signedIn && state.following === null);
    $("postComment").disabled = !!state.mutation || !$("commentInput").value.trim();
    $("like").disabled = !enabled || !!state.mutation ||
      (state.signedIn && typeof state.stats.liked !== "boolean");
    $("favorite").disabled = !enabled || !!state.mutation ||
      (state.signedIn && typeof state.stats.favorited !== "boolean");
    $("share").disabled = !enabled;
    $("sharePreview").disabled = !enabled;
    updateDownloadButtons();
  }
  function changeCommunity(command, payload) {
    if (state.mutation) return;
    if (!state.signedIn) send("login");
    else if (send(command, payload)) {
      state.mutation = command;
      updateCommunityButtons();
    }
  }
  function detailTab(recommendations) {
    $("commentsTab").setAttribute("aria-pressed", !recommendations);
    $("recommendationsTab").setAttribute("aria-pressed", recommendations);
    $("commentsPanel").hidden = recommendations || !state.detail?.publicInteraction;
    $("recommendationsPanel").hidden = !recommendations || !state.detail?.publicInteraction;
  }
  function updateImage(cover, data) {
    if (!cover) return;
    let image = cover.querySelector("img");
    if (!image) {
      image = el("img");
      image.alt = "";
      cover.querySelector(".placeholder")?.remove();
      cover.prepend(image);
    }
    image.src = data;
  }
  function renderRecommendations(items) {
    const host = $("recommendations");
    host.replaceChildren();
    state.recommendationImages.clear();
    for (const item of items) {
      const key = item.id + ":" + item.versionId;
      const cover = el("button", "recommendation-card");
      cover.title = item.title;
      cover.setAttribute("aria-label", item.title);
      cover.onclick = () => open({ key });
      state.recommendationImages.set(key, cover);
      if (state.images.has(key)) updateImage(cover, state.images.get(key));
      else cover.append(el("span", "placeholder", item.title));
      host.append(cover);
    }
    if (!items.length) host.append(el("p", "detail-empty", "暂无相似推荐"));
  }
  function renderComments() {
    const host = $("comments");
    host.replaceChildren();
    for (const item of state.comments) {
      const comment = el("article", "comment"),
        avatar = el("span", "avatar"),
        name = item.author_name || "用户",
        content = el("div", "comment-content");
      displayAvatar(avatar, null, name.slice(0, 1));
      content.append(el("p", "comment-author", name), el("p", "comment-body", item.body));
      const date = new Date(item.created_at);
      if (Number.isFinite(date.getTime()))
        content.append(el("time", "", date.toLocaleDateString()));
      comment.append(avatar, content);
      host.append(comment);
    }
    if (!state.comments.length) host.append(el("p", "detail-empty", "暂无评论"));
    $("moreComments").hidden = !state.moreComments;
    $("moreComments").disabled = false;
    $("commentsTab").textContent = state.moreComments ? "评论" : "评论 (" + state.comments.length + ")";
  }
  function hideViewer() {
    closeViewer();
    $("hero").hidden = false;
    $("viewer").hidden = true;
    $("geometry").hidden = false;
    $("closeGeometry").hidden = true;
    $("viewTools").hidden = true;
    $("meshStats").hidden = true;
    $("gallery").hidden = false;
  }
  function showMeshStats(info) {
    $("meshStats").replaceChildren();
    for (const [label, value] of [["拓扑", "三角面"], ["面数", info.triangles], ["顶点数", info.vertices]]) {
      const line = el("div");
      line.append(el("dt", "", label), el("dd", "", typeof value === "number" ? value.toLocaleString() : value));
      $("meshStats").append(line);
    }
    $("meshStats").hidden = false;
  }
  function detail(value) {
    const keepFilePanel = state.detail?.id === value.id && !$("filePanel").hidden;
    state.detail = value;
    state.download = null;
    state.fileActions = null;
    downloadIssue("");
    state.comments = [];
    state.commentsLoaded = false;
    state.recommendationsLoaded = false;
    state.detailImages.clear();
    state.moreComments = false;
    state.following = null;
    state.mutation = "";
    hideViewer();
    $("listing").hidden = true;
    $("detail").hidden = false;
    $("detailContent").scrollTop = 0;
    $("detailSidebar").scrollTop = 0;
    busy(false);
    $("title").textContent = value.title;
    $("author").textContent = value.author.displayName;
    displayAvatar($("detailAvatar"), value.author.avatarUrl, value.author.displayName.slice(0, 1));
    $("detailTags").replaceChildren();
    for (const tag of Array.isArray(value.tags) ? value.tags : [])
      $("detailTags").append(el("span", "tag", tag));
    $("description").textContent = value.description;
    $("license").textContent = value.version.licenseName;
    $("licenseDescription").textContent = value.version.licenseDescription;
    $("eligibility").textContent = value.access.allowed
      ? ""
      : value.access.reason === "login_required"
        ? "登录后可查看下载资格"
        : value.access.reason === "membership_required"
          ? "此模型需要会员资格"
          : "此模型需要购买资格";
    const versions = $("versions");
    versions.replaceChildren();
    const accessibleVersions = window.libraryState.accessibleVersions(value);
    for (const v of accessibleVersions) {
      const option = el("option", "", "V" + v.version);
      option.value = v.id;
      option.selected = v.id === value.version.id;
      versions.append(option);
    }
    stats(value.statistics || {});
    $("hero").removeAttribute("src");
    $("gallery").replaceChildren();
    const cover = state.images.get(state.detailKey);
    if (cover) $("hero").src = cover;
    $("filePanelToggle").hidden = accessibleVersions.length <= 1 && value.files.length <= 1;
    setFilePanel(keepFilePanel);
    const usable = usableFiles();
    selectFile(usable.length === 1 ? usable[0].id : "");
    renderDownloadFeedback();
    $("detailTabs").hidden = !value.publicInteraction;
    $("commentsTab").textContent = "评论";
    $("comments").replaceChildren(el("p", "detail-empty", "正在读取评论…"));
    $("recommendations").replaceChildren(el("p", "detail-empty", "正在读取推荐…"));
    $("commentInput").value = "";
    $("moreComments").hidden = true;
    state.recommendationImages.clear();
    updateCommunityButtons();
    detailTab(false);
  }
  window.modelLibraryReceive = (message) => {
    if (!message || typeof message !== "object") return;
    if (message.type === "init") {
      if (!message.nonce || !Number.isSafeInteger(message.documentGeneration) ||
          message.documentGeneration < state.documentGeneration) return;
      if (message.documentGeneration !== state.documentGeneration) {
        state.nonce = message.nonce;
        state.documentGeneration = message.documentGeneration;
        state.accountGeneration = state.requestGeneration = 0;
        state.connected = false;
        state.download = null;
        updateAccountAvatar("");
      }
      filters();
      send("ready");
      return;
    }
    if (message.documentGeneration !== state.documentGeneration) return;
    if (message.type === "account") {
      if (message.accountGeneration < state.accountGeneration) return;
    } else if (message.accountTag !== state.accountTag || message.accountGeneration !== state.accountGeneration ||
               (message.type !== "downloadState" && message.requestGeneration !== state.requestGeneration)) return;
    switch (message.type) {
      case "account": {
        const changed = state.accountTag !== message.accountTag || state.accountGeneration !== message.accountGeneration;
        const redraw = changed || !state.connected || state.signedIn !== message.signedIn ||
          state.projectAvailable !== message.projectAvailable;
        state.accountTag = message.accountTag;
        state.accountGeneration = message.accountGeneration;
        state.connected = true;
        state.signedIn = message.signedIn;
        state.refreshing = !!message.refreshing;
        state.loginRequired = !!message.loginRequired;
        state.projectAvailable = message.projectAvailable;
        updateAccountAvatar(message.avatarData);
        if (changed) {
          busy(false);
          state.cloud = [];
          window.libraryState.retainLocalImages(state.images, state.local);
          state.detail = null;
          state.detailKey = "";
          state.download = null;
          state.stats = {};
          state.fileActions = null;
          state.selectedFile = "";
          state.comments = [];
          state.following = null;
          state.mutation = "";
          hideViewer();
          $("hero").removeAttribute("src");
          $("gallery").replaceChildren();
          $("downloadComplete").hidden = true;
          $("detail").hidden = true;
          $("listing").hidden = false;
          notice("");
        }
        if (state.refreshing && state.mutation) {
          state.mutation = "";
          updateCommunityButtons();
        }
        renderDownloadFeedback();
        if (redraw) render();
        break;
      }
      case "inactive":
        hideViewer();
        busy(false);
        if (downloadActive()) {
          state.download = null;
          selectFile(state.selectedFile);
          renderDownloadFeedback();
        }
        $("menu").hidden = true;
        break;
      case "taxonomy":
        state.taxonomy = message.value?.categories || [];
        filters();
        break;
      case "local":
        $("menu").hidden = true;
        state.local = message.items;
        state.revision = message.revision;
        render();
        break;
      case "catalog": {
        const map = new Map(
          (message.replace ? [] : state.cloud).map((x) => [
            x.id + ":" + x.versionId,
            x,
          ]),
        );
        for (const x of message.items) map.set(x.id + ":" + x.versionId, x);
        state.cloud = [...map.values()];
        state.offset = Number.isInteger(message.offset) ? message.offset : 0;
        state.hasMore = !!message.hasMore;
        if (message.taxonomy)
          state.taxonomy = message.taxonomy.categories || [];
        filters();
        busy(false);
        notice(
          message.loginRequired ? "登录后查看账号记录" :
            message.unavailable?.length ? "部分账号记录暂不可用，已显示可取得的模型与记录。" : "",
          message.loginRequired,
        );
        render();
        break;
      }
      case "image":
        if (window.libraryState.safeImage(message.data)) {
          state.images.set(message.key, message.data);
          updateImage(state.cardImages.get(message.key), message.data);
          updateImage(state.recommendationImages.get(message.key), message.data);
        }
        break;
      case "detail":
        state.detailKey = message.key;
        detail(message.value);
        break;
      case "detailImage":
        if (state.detail && message.key === state.detailKey && !state.detailImages.has(message.data) &&
            window.libraryState.safeImage(message.data)) {
          state.detailImages.add(message.data);
          const img = el("img");
          img.src = message.data;
          img.alt = "模型图片";
          const b = el("button");
          b.append(img);
          b.onclick = () => ($("hero").src = message.data);
          $("gallery").append(b);
          if (!$("hero").src) $("hero").src = message.data;
        }
        break;
      case "statistics":
        if (state.mutation === "toggle") state.mutation = "";
        stats(message.value);
        break;
      case "loading":
        busy(message.active, message.stage, message.cancelable, Number.isInteger(message.importPercent));
        if (Number.isInteger(message.importPercent))
          $("bytes").textContent = message.importPercent < 99 ? message.importPercent + "%" : "";
        break;
      case "error":
        busy(false);
        state.mutation = "";
        updateCommunityButtons();
        notice(message.message, !!message.loginRequired);
        if (state.detail && message.downloadError) downloadIssue(message.message);
        toast(message.message);
        break;
      case "toast":
        toast(message.message);
        break;
      case "fileActions": {
        if (message.key !== state.detailKey || message.key !== state.detail?.id + ":" + state.detail?.version.id) break;
        state.fileActions = message.files;
        downloadIssue(message.missing?.length ? "本地模型文件缺失，请重新下载。" : "");
        if (state.download?.phase === "complete" && !state.fileActions[state.download.file]) {
          state.download = null;
          renderDownloadFeedback();
        }
        const registered = Object.keys(state.fileActions);
        selectFile(state.fileActions[state.selectedFile] ? state.selectedFile :
          registered.length === 1 ? registered[0] : state.selectedFile);
        break;
      }
      case "downloadState":
        applyDownload(message);
        break;
      case "comments": {
        if (!state.detail || message.key !== state.detailKey) break;
        const { items, offset, recovery } = message.value;
        if (!Array.isArray(items) || (recovery && state.commentsLoaded)) break;
        const known = new Map((offset ? state.comments : []).map((item) => [item.id, item]));
        for (const item of items) known.set(item.id, item);
        state.comments = [...known.values()];
        state.commentsLoaded = true;
        state.moreComments = items.length === 50;
        renderComments();
        break;
      }
      case "recommendations":
        if (state.detail && message.key === state.detailKey &&
            !(message.value.recovery && state.recommendationsLoaded)) {
          renderRecommendations(message.value.items);
          state.recommendationsLoaded = true;
        }
        break;
      case "author":
      case "follow":
        if (state.detail && message.key === state.detailKey) {
          state.following = message.value.following === true;
          if (message.type === "follow") state.mutation = "";
          updateCommunityButtons();
        }
        break;
      case "postComment":
        if (state.detail && message.key === state.detailKey) {
          state.mutation = "";
          $("commentInput").value = "";
          updateCommunityButtons();
          toast("评论已发表");
        }
        break;
      case "communityError": {
        if (!state.detail || message.key !== state.detailKey) break;
        if (message.kind === "author") {
          // Following uses an explicit requested state and can safely retry.
          $("follow").disabled = false;
          break;
        }
        const target = $(message.kind);
        if (message.kind === "comments" && state.comments.length) {
          $("moreComments").disabled = false;
          toast(message.message);
          break;
        }
        const retry = el("button", "detail-empty", "加载失败，点击重试");
        retry.onclick = () => send(message.kind);
        target.replaceChildren(retry);
        break;
      }
      case "geometryBegin":
        if (message.key === state.detailKey && state.detail)
          geometryTransfer.begin(message.key, message.epoch, message.total);
        break;
      case "geometryChunk":
        geometryTransfer.append(
          message.key,
          message.epoch,
          message.offset,
          message.data,
        );
        break;
      case "geometryEnd": {
        const bytes = geometryTransfer.finish(message.key, message.epoch);
        if (bytes && message.key === state.detailKey && state.detail) {
          const selected = state.detailKey;
          $("viewer").hidden = false;
          $("geometry").hidden = true;
          $("closeGeometry").hidden = false;
          const detailValue = state.detail;
          window.libraryViewer?.open($("viewer"), bytes).then((info) => {
            if (state.detailKey !== selected || state.detail !== detailValue || !info) return;
            $("hero").hidden = true;
            $("viewTools").hidden = false;
            $("gallery").hidden = true;
            showMeshStats(info);
          }).catch((error) => {
            if (state.detailKey !== selected || state.detail !== detailValue) return;
            toast(error.message);
            hideViewer();
          });
        }
        break;
      }
    }
  };
  window.modelLibraryReceiveBatch = (messages) => {
    if (!Array.isArray(messages)) return;
    frameUpdates.batch(() => {
      for (const message of messages) window.modelLibraryReceive(message);
    });
  };
  for (const b of document.querySelectorAll("[data-scope]"))
    b.onclick = () => {
      state.scope = b.dataset.scope;
      state.filter = "all";
      state.category = "";
      for (const tab of document.querySelectorAll("[data-scope]")) {
        tab.classList.toggle("selected", tab === b);
        tab.setAttribute("aria-pressed", tab === b);
      }
      filters();
      query();
    };
  $("sort").onchange = () => {
    state.sort = $("sort").value;
    query();
  };
  $("format").onchange = () => {
    state.format = $("format").value;
    query();
  };
  let searchTimer;
  $("search").oninput = () => {
    clearTimeout(searchTimer);
    searchTimer = setTimeout(() => {
      state.q = $("search").value;
      query();
    }, 300);
  };
  $("more").onclick = () => query(false);
  document.addEventListener("scroll", () => { $("menu").hidden = true; }, true);
  document.addEventListener("click", (e) => {
    if (!$("menu").contains(e.target)) $("menu").hidden = true;
  });
  document.addEventListener("keydown", (e) => {
    if (e.key === "Escape") {
      $("menu").hidden = true;
    }
  });
  for (const b of document.querySelectorAll("[data-command]"))
    b.onclick = () => send(b.dataset.command);
  $("back").onclick = $("closeDetail").onclick = () => {
    hideViewer();
    state.detail = null;
    state.detailKey = "";
    state.download = null;
    $("detail").hidden = true;
    $("listing").hidden = false;
    restoreListPosition(state.listPosition);
    send("back");
  };
  $("cancel").onclick = () => {
    if ($("loading").dataset.importing !== "true") geometryTransfer.reset();
    send("cancel");
  };
  $("like").onclick = () =>
    changeCommunity("toggle", { kind: "like", active: !state.stats.liked });
  $("favorite").onclick = () =>
    changeCommunity("toggle", { kind: "favorite", active: !state.stats.favorited });
  $("versions").onchange = () => {
    state.detailKey = state.detail.id + ":" + $("versions").value;
    state.download = null;
    renderDownloadFeedback();
    state.fileActions = null;
    selectFile("");
    send("detail", { key: state.detailKey });
  };
  $("geometry").onclick = () => {
    if (!state.signedIn && !state.refreshing) send("login");
    else {
      toast("正在准备交互模型…");
      send("geometry");
    }
  };
  $("closeGeometry").onclick = hideViewer;
  $("downloadDetail").onclick = $("downloadPreview").onclick = chooseDownload;
  $("cancelDownload").onclick = () => {
    if (!downloadCancelable() || state.download.cancelling) return;
    const { key, file, downloadRequest } = state.download;
    if (send("cancelDownload", { key, file, downloadRequest })) {
      state.download.cancelling = true;
      renderDownloadFeedback();
    }
  };
  $("retryDownload").onclick = () => { if (state.download && !$("retryDownload").hidden) download(state.download.file); };
  $("downloadLogin").onclick = () => { if (!$("downloadLogin").hidden) send("login"); };
  $("filePanelToggle").onclick = () => setFilePanel($("filePanel").hidden);
  $("share").onclick = $("sharePreview").onclick = () => send("share");
  $("openDownloaded").onclick = () => {
    if (!$("openDownloaded").disabled)
      send("fileAction", { file: state.selectedFile, action: "open" });
  };
  $("importDownloaded").onclick = () => {
    if (!$("importDownloaded").disabled)
      send("fileAction", { file: state.selectedFile, action: "import" });
  };
  $("commentsTab").onclick = () => detailTab(false);
  $("recommendationsTab").onclick = () => detailTab(true);
  $("follow").onclick = () => changeCommunity("follow", { active: state.following !== true });
  $("commentInput").oninput = updateCommunityButtons;
  $("postComment").onclick = () => {
    const comment = $("commentInput").value.trim();
    if (comment) changeCommunity("postComment", { comment });
  };
  $("moreComments").onclick = () => {
    if (send("comments", { query: { offset: state.comments.length } })) $("moreComments").disabled = true;
  };
  for (const button of document.querySelectorAll("[data-view-mode]"))
    button.onclick = () => window.libraryViewer?.view(button.dataset.viewMode);
})();
