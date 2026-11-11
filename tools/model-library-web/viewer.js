// Installed browser adapter; no remote URL is accepted here.
import * as THREE from "three";
import { GLTFLoader } from "three/examples/jsm/loaders/GLTFLoader.js";
import { OrbitControls } from "three/examples/jsm/controls/OrbitControls.js";
let generation = 0,
  cleanup = () => {};
function dispose(root) {
  root?.traverse((o) => {
    o.geometry?.dispose();
    for (const m of Array.isArray(o.material) ? o.material : [o.material])
      if (m) {
        for (const v of Object.values(m))
          if (v instanceof THREE.Texture) {
            v.dispose();
            v.source?.data?.close?.();
          }
        m.dispose();
      }
  });
}
function validate(bytes) {
  const data = new DataView(bytes);
  if (
    bytes.byteLength > 67108864 ||
    bytes.byteLength < 20 ||
    data.getUint32(0, true) !== 0x46546c67 ||
    data.getUint32(4, true) !== 2 ||
    data.getUint32(8, true) !== bytes.byteLength
  )
    throw Error("交互模型数据无效");
  const length = data.getUint32(12, true);
  if (data.getUint32(16, true) !== 0x4e4f534a || length > bytes.byteLength - 20)
    throw Error("交互模型结构无效");
  const json = JSON.parse(
    new TextDecoder().decode(bytes.slice(20, 20 + length)),
  );
  // Deny external buffers/textures before GLTFLoader can make any resource request.
  for (const item of [...(json.buffers || []), ...(json.images || [])])
    if (
      item.uri &&
      !/^data:(image\/(png|jpeg|webp)|application\/octet-stream);base64,/.test(
        item.uri,
      )
    )
      throw Error("交互模型包含外部资源");
  if (
    (json.extensionsRequired || []).some((x) =>
      ["KHR_draco_mesh_compression", "EXT_meshopt_compression"].includes(x),
    )
  )
    throw Error("交互模型压缩格式暂不可用，请使用图片预览");
}
window.libraryViewer = {
  close() {
    ++generation;
    cleanup();
    cleanup = () => {};
  },
  async open(host, bytes) {
    this.close();
    const token = generation;
    validate(bytes);
    let root;
    try {
      root = (await new GLTFLoader().parseAsync(bytes, "")).scene;
      if (token !== generation) {
        dispose(root);
        return;
      }
      let vertices = 0,
        triangles = 0;
      root.traverse((o) => {
        if (o.isMesh) {
          vertices += o.geometry.attributes.position?.count || 0;
          triangles +=
            (o.geometry.index?.count ||
              o.geometry.attributes.position?.count ||
              0) / 3;
          if (!o.geometry.attributes.normal) o.geometry.computeVertexNormals();
        }
      });
      if (vertices > 2500000 || triangles > 4000000)
        throw Error("交互模型超出资源限制");
      const box = new THREE.Box3().setFromObject(root),
        size = box.getSize(new THREE.Vector3()).length();
      if (!Number.isFinite(size) || size <= 0)
        throw Error("交互模型没有有效几何");
      root.position.sub(box.getCenter(new THREE.Vector3()));
      const renderer = new THREE.WebGLRenderer({
        antialias: true,
        alpha: false,
      });
      renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
      renderer.setClearColor("#28292d");
      renderer.outputColorSpace = THREE.SRGBColorSpace;
      renderer.domElement.setAttribute("aria-label", "交互式3D模型");
      renderer.domElement.tabIndex = 0;
      host.replaceChildren(renderer.domElement);
      const scene = new THREE.Scene();
      scene.add(root, new THREE.HemisphereLight(0xffffff, 0x69717e, 2.5));
      const light = new THREE.DirectionalLight(0xffffff, 3);
      light.position.set(size, size, size);
      scene.add(light);
      const camera = new THREE.PerspectiveCamera(40, 1, size / 100, size * 100);
      camera.position.set(size, size * 0.65, size);
      const controls = new OrbitControls(camera, renderer.domElement);
      controls.enableDamping = true;
      controls.target.set(0, 0, 0);
      const resize = () => {
        const w = host.clientWidth,
          h = host.clientHeight;
        if (w > 0 && h > 0) {
          renderer.setSize(w, h, false);
          camera.aspect = w / h;
          camera.updateProjectionMatrix();
        }
      };
      const observer = new ResizeObserver(resize);
      observer.observe(host);
      resize();
      renderer.setAnimationLoop(() => {
        controls.update();
        renderer.render(scene, camera);
      });
      cleanup = () => {
        observer.disconnect();
        controls.dispose();
        renderer.setAnimationLoop(null);
        dispose(root);
        renderer.dispose();
        renderer.forceContextLoss();
        host.replaceChildren();
      };
    } catch (error) {
      dispose(root);
      throw error;
    }
  },
};
