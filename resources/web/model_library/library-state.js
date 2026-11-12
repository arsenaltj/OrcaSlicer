/* Pure presentation projection. It does not authorize or perform native operations. */
(function (root) {
  "use strict";
  function project(local, cloud, scope, filter, query, sort) {
    const q = String(query || "")
      .trim()
      .toLocaleLowerCase();
    let locals =
      scope === "mine"
        ? local.filter(
            (x) =>
              filter === "all" ||
              (filter === "models" && !x.designOnly) ||
              (filter === "prints" &&
                ["success", "issue"].includes(x.printFeedback)),
          )
        : [];
    if (!["all", "models", "prints"].includes(filter)) locals = [];
    locals = locals.filter(
      (x) => !q || (x.title + " " + x.details).toLocaleLowerCase().includes(q),
    );
    const remote = cloud.map((x) => ({ ...x, key: x.id + ":" + x.versionId }));
    const linked = new Set();
    for (const item of remote) {
      item.linkedLocals = local.filter(
        (x) =>
          x.sourceVerified &&
          x.modelId === item.id &&
          x.versionId === item.versionId &&
          /^[0-9a-f]{64}$/.test(x.sha256 || ""),
      );
      item.linkedLocal = item.linkedLocals[0];
      for (const record of item.linkedLocals) linked.add(record.key);
    }
    const values = [...locals.filter((x) => !linked.has(x.key)), ...remote];
    if (
      (scope === "mine" && filter === "all") ||
      filter === "models" ||
      filter === "prints"
    ) {
      const metric = {
        views: "viewCount",
        downloads: "downloadCount",
        likes: "likeCount",
        favorites: "favoriteCount",
      }[sort];
      if (metric)
        values.sort(
          (a, b) =>
            (Number.isFinite(b[metric]) ? b[metric] : -1) -
            (Number.isFinite(a[metric]) ? a[metric] : -1),
        );
      else if (sort === "title")
        values.sort((a, b) => a.title.localeCompare(b.title, "zh-CN"));
      else if (sort === "newest")
        values.sort(
          (a, b) =>
            (b.local ? b.generatedAt * 1000 : Date.parse(b.createdAt)) -
            (a.local ? a.generatedAt * 1000 : Date.parse(a.createdAt)),
        );
    }
    return values;
  }
  function retainLocalImages(images, local) {
    const keys = new Set(local.map((x) => x.key));
    for (const key of images.keys()) if (!keys.has(key)) images.delete(key);
  }
  function safeImage(value) {
    return (
      typeof value === "string" &&
      /^data:image\/(png|jpeg|webp|svg\+xml);base64,[A-Za-z0-9+/=]+$/.test(
        value,
      )
    );
  }
  function supported(filename) {
    return /\.(glb|obj|3mf|stl|step|stp)$/i.test(filename);
  }
  function workbench(filename) {
    return /\.(glb|obj)$/i.test(filename);
  }
  function statusLabel(item) {
    if (!item.owned) return "";
    const status = item.status || item.version?.status;
    const labels = [];
    if (item.withdrawn || status === "withdrawn") labels.push("已撤回");
    else if (status === "draft") labels.push("草稿");
    else if (status === "published") labels.push("已发布");
    if (item.visibility === "private") labels.push("私有");
    return labels.join(" · ");
  }
  function accessibleVersions(detail) {
    return detail.versions.filter(
      (x) => detail.owned || x.status === "published" || x.id === detail.version.id,
    );
  }
  class GeometryTransfer {
    begin(key, epoch, total) {
      this.reset();
      if (!Number.isSafeInteger(total) || total < 20 || total > 67108864)
        return false;
      this.key = key;
      this.epoch = epoch;
      this.bytes = new Uint8Array(total);
      this.offset = 0;
      return true;
    }
    append(key, epoch, offset, encoded) {
      if (!this.bytes || key !== this.key || epoch !== this.epoch) return false;
      if (
        offset !== this.offset ||
        typeof encoded !== "string" ||
        encoded.length > 262144
      ) {
        this.reset();
        return false;
      }
      let raw;
      try {
        raw = atob(encoded);
      } catch {
        this.reset();
        return false;
      }
      if (raw.length > this.bytes.length - this.offset) {
        this.reset();
        return false;
      }
      for (let i = 0; i < raw.length; i++)
        this.bytes[this.offset++] = raw.charCodeAt(i);
      return true;
    }
    finish(key, epoch) {
      if (
        !this.bytes ||
        key !== this.key ||
        epoch !== this.epoch ||
        this.offset !== this.bytes.length
      )
        return null;
      const bytes = this.bytes;
      this.reset();
      return bytes.buffer;
    }
    reset() {
      this.bytes = null;
      this.key = "";
      this.epoch = 0;
      this.offset = 0;
    }
  }
  const api = {
    project,
    retainLocalImages,
    safeImage,
    supported,
    workbench,
    statusLabel,
    accessibleVersions,
    GeometryTransfer,
  };
  if (typeof module === "object" && module.exports) module.exports = api;
  else root.libraryState = api;
})(typeof window === "object" ? window : globalThis);
