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
      sort: "newest",
      q: "",
      offset: 0,
      cloud: [],
      local: [],
      images: new Map(),
      cardImages: new Map(),
      accountTag: "",
      taxonomy: [],
      signedIn: false,
      revision: 0,
      detail: null,
      detailKey: "",
      stats: {},
      downloaded: "",
      hasMore: false,
    };
  const icons = {
    search: '<circle cx="6" cy="6" r="4.5"/><path d="m9.5 9.5 4 4"/>',
    eye: '<path d="M1 7s2-4 6-4 6 4 6 4-2 4-6 4-6-4-6-4Z"/><circle cx="7" cy="7" r="1.5"/>',
    like: '<path d="M4 6 7 1c2 0 1 4 1 4h4l1 2-2 6H4ZM1 6h3v7H1Z"/>',
    heart: '<path d="M7 12C-4 5 3-1 7 4c4-5 11 1 0 8Z"/>',
    download: '<path d="M7 1v8m-3-3 3 3 3-3M1 9v4h12V9"/>',
  };
  function icon(name) {
    const span = document.createElement("span");
    span.innerHTML =
      '<svg viewBox="0 0 14 14" aria-hidden="true">' + icons[name] + "</svg>";
    return span;
  }
  $("searchIcon").append(icon("search"));
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
  function busy(active, stage = "", cancelable = false) {
    $("app").inert = !!active && !!cancelable;
    $("more").disabled = !!active;
    $("loading").hidden = !active;
    $("stage").textContent = stage;
    $("cancel").hidden = !cancelable;
    if (!active) $("bytes").textContent = "";
  }
  function query(reset = true) {
    if (reset) {
      state.offset = 0;
      const identity = JSON.stringify([state.scope,state.filter,state.q,state.category,state.sort]);
      if (identity !== state.catalogQuery) state.cloud = [];
      state.catalogQuery = identity;
      window.libraryState.retainLocalImages(state.images, state.local);
    }
    state.detail = null;
    state.detailKey = "";
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
        sort: state.sort,
        offset: String(state.offset),
      },
    });
  }
  function filters() {
    const host = $("filters");
    host.replaceChildren();
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
    const add = (label, fn, disabled = false) => {
      const b = el("button", "", label);
      b.setAttribute("role", "menuitem");
      b.disabled = disabled;
      b.onclick = () => {
        host.hidden = true;
        fn();
      };
      host.append(b);
    };
    if (item?.local) {
      add(item.designOnly ? "打开设计图" : "打开模型", () =>
        localAction(item, "open"),
      );
      if (!item.designOnly) {
        add("导入切片", () => localAction(item, "import"));
        if (window.libraryState.workbench(item.extension)) {
          add("配色匹配", () => localAction(item, "colors"));
          add("更换贴图", () => localAction(item, "retexture"));
        }
      }
      add("导出副本", () => localAction(item, "export"));
      if (item.imported)
        add("记录实际打印结果", () => localAction(item, "print"));
      add("删除本地", () => localAction(item, "remove"));
    } else if (item) {
      add("模型详情", () => open(item));
      for (const asset of item.linkedLocals || []) {
        const suffix =
          item.linkedLocals.length > 1
            ? "（" + asset.extension.slice(1).toUpperCase() + "）"
            : "";
        add("打开本地模型" + suffix, () => localAction(asset, "open"));
        add("导入切片" + suffix, () => localAction(asset, "import"));
      }
    }
    add("导入本地模型", () => send("chooseLocal"));
    add("打开模型文件夹", () => send("storage"));
    add(
      "保存当前工程（3MF）",
      () => send("saveProject"),
      !state.projectAvailable,
    );
    add("刷新", () => send("refresh"));
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
      state.detailKey = item.key;
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
  function renderNow() {
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
    for (const item of items) {
      const card = el("article", "card"),
        cover = el("button", "cover");
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
      overlay.append(
        el(
          "span",
          "",
          item.local
            ? item.printFeedback === "success"
              ? "打印成功"
              : item.printFeedback === "issue"
                ? "打印有问题"
                : "本机"
            : item.linkedLocal
              ? "已下载"
              : "",
        ),
      );
      if (!item.local) {
        const counts = el("span", "counts");
        counts.append(
          metric("eye", item.viewCount),
          metric("like", item.likeCount),
        );
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
      author.append(
        el("span", "avatar", authorName.slice(0, 1)),
        document.createTextNode(authorName),
      );
      if (window.libraryState.safeImage(item.author?.avatarUrl)) {
        const image = el("img");
        image.src = item.author.avatarUrl;
        image.alt = "";
        author.querySelector(".avatar").replaceChildren(image);
      }
      footer.append(author);
      if (!item.local)
        footer.append(
          metric("download", item.downloadCount),
          metric("heart", item.favoriteCount),
        );
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
    if (state.downloaded) {
      const item = state.local.find((x) => x.key === state.downloaded);
      $("openDownloaded").disabled = !item;
      $("importDownloaded").disabled = !item;
      if (item)
        $("openDownloaded").textContent = /\.(glb|obj)$/.test(
          item.extension || "",
        )
          ? "打开工作台"
          : "打开模型";
    }
  }
  function stats(value) {
    state.stats = value;
    $("like").replaceChildren(
      icon("like"),
      document.createTextNode(" 点赞 " + (value.likeCount ?? "—")),
    );
    $("favorite").replaceChildren(
      icon("heart"),
      document.createTextNode(" 收藏 " + (value.favoriteCount ?? "—")),
    );
    $("like").classList.toggle("active", !!value.liked);
    $("favorite").classList.toggle("active", !!value.favorited);
    $("like").disabled = !state.detail?.publicInteraction ||
      (state.signedIn && typeof value.liked !== "boolean");
    $("favorite").disabled = !state.detail?.publicInteraction ||
      (state.signedIn && typeof value.favorited !== "boolean");
  }
  function detail(value) {
    state.detail = value;
    state.downloaded = "";
    $("downloadComplete").hidden = true;
    closeViewer();
    $("viewer").hidden = true;
    $("closeGeometry").hidden = true;
    $("geometry").hidden = false;
    $("listing").hidden = true;
    $("detail").hidden = false;
    busy(false);
    $("title").textContent = value.title;
    $("author").textContent = value.author.displayName;
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
    for (const v of window.libraryState.accessibleVersions(value)) {
      const option = el("option", "", "V" + v.version);
      option.value = v.id;
      option.selected = v.id === value.version.id;
      versions.append(option);
    }
    stats(value.statistics || {});
    $("hero").removeAttribute("src");
    $("gallery").replaceChildren();
    $("files").replaceChildren();
    for (const file of value.files) {
      const row = el("div", "file");
      row.append(
        el(
          "span",
          "",
          file.filename +
            " · " +
            (Number(file.size) / 1048576).toFixed(2) +
            " MB",
        ),
      );
      const b = el(
        "button",
        "",
        window.libraryState.supported(file.filename) ? "下载" : "暂不支持",
      );
      b.disabled = !window.libraryState.supported(file.filename);
      b.onclick = () =>
        send("download", { file: file.id });
      row.append(b);
      $("files").append(row);
    }
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
      }
      filters();
      send("ready");
      return;
    }
    if (message.documentGeneration !== state.documentGeneration) return;
    if (message.type === "account") {
      if (message.accountGeneration < state.accountGeneration) return;
    } else if (message.accountTag !== state.accountTag || message.accountGeneration !== state.accountGeneration ||
               message.requestGeneration !== state.requestGeneration) return;
    switch (message.type) {
      case "account": {
        const changed = state.accountTag !== message.accountTag || state.accountGeneration !== message.accountGeneration;
        state.accountTag = message.accountTag;
        state.accountGeneration = message.accountGeneration;
        state.connected = true;
        state.signedIn = message.signedIn;
        state.projectAvailable = message.projectAvailable;
        if (changed) {
          busy(false);
          state.cloud = [];
          window.libraryState.retainLocalImages(state.images, state.local);
          state.detail = null;
          state.detailKey = "";
          state.stats = {};
          state.downloaded = "";
          closeViewer();
          $("hero").removeAttribute("src");
          $("gallery").replaceChildren();
          $("downloadComplete").hidden = true;
          $("detail").hidden = true;
          $("listing").hidden = false;
          notice("");
        }
        render();
        break;
      }
      case "inactive":
        closeViewer();
        busy(false);
        $("viewer").hidden = true;
        $("geometry").hidden = false;
        $("closeGeometry").hidden = true;
        $("menu").hidden = true;
        break;
      case "taxonomy":
        state.taxonomy = message.value?.categories || [];
        filters();
        break;
      case "local":
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
          const cover = state.cardImages.get(message.key);
          if (cover) {
            let img = cover.querySelector("img");
            if (!img) {
              img = el("img");
              img.alt = "";
              cover.querySelector(".placeholder")?.remove();
              cover.prepend(img);
            }
            img.src = message.data;
          }
        }
        break;
      case "detail":
        state.detailKey = message.key;
        detail(message.value);
        break;
      case "detailImage":
        if (state.detail && window.libraryState.safeImage(message.data)) {
          const img = el("img");
          img.src = message.data;
          img.alt = "模型图片";
          const b = el("button");
          b.append(img);
          b.onclick = () => ($("hero").src = message.data);
          $("gallery").append(b);
          if (!$("hero").getAttribute("src")) $("hero").src = message.data;
        }
        break;
      case "statistics":
        stats(message.value);
        break;
      case "loading":
        busy(message.active, message.stage, message.cancelable);
        if (message.done)
          $("bytes").textContent =
            (message.done / 1048576).toFixed(1) +
            " MB" +
            (message.total
              ? " / " + (message.total / 1048576).toFixed(1) + " MB"
              : "");
        break;
      case "error":
        busy(false);
        notice(message.message, !!message.loginRequired);
        toast(message.message);
        break;
      case "downloaded":
        busy(false);
        state.downloaded = message.key;
        $("downloadComplete").hidden = false;
        render();
        break;
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
          window.libraryViewer?.open($("viewer"), bytes).catch((error) => {
            if (state.detailKey !== selected || !state.detail) return;
            toast(error.message);
            $("viewer").hidden = true;
            $("geometry").hidden = false;
            $("closeGeometry").hidden = true;
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
      for (const tab of document.querySelectorAll("[data-scope]"))
        tab.classList.toggle("selected", tab === b);
      filters();
      query();
    };
  $("sort").onchange = () => {
    state.sort = $("sort").value;
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
  $("more").onclick = () => {
    state.offset += 24;
    query(false);
  };
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
  $("back").onclick = () => {
    closeViewer();
    state.detail = null;
    $("detail").hidden = true;
    $("listing").hidden = false;
    send("back");
  };
  $("cancel").onclick = () => {
    geometryTransfer.reset();
    send("cancel");
  };
  $("like").onclick = () =>
    send("toggle", { kind: "like", active: !state.stats.liked });
  $("favorite").onclick = () =>
    send("toggle", { kind: "favorite", active: !state.stats.favorited });
  $("versions").onchange = () => {
    state.detailKey = state.detail.id + ":" + $("versions").value;
    send("detail", { key: state.detailKey });
  };
  $("geometry").onclick = () => {
    if (!state.signedIn) send("login");
    else {
      toast("正在准备交互模型…");
      send("geometry");
    }
  };
  $("closeGeometry").onclick = () => {
    closeViewer();
    $("viewer").hidden = true;
    $("geometry").hidden = false;
    $("closeGeometry").hidden = true;
  };
  $("openDownloaded").onclick = () => {
    const item = state.local.find((x) => x.key === state.downloaded);
    if (item) localAction(item, "open");
  };
  $("importDownloaded").onclick = () => {
    const item = state.local.find((x) => x.key === state.downloaded);
    if (item) localAction(item, "import");
  };
})();
