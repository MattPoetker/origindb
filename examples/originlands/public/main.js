// OriginLands — M0: the toon world (single-player fidelity slice).
// Core three.js only: MeshToonMaterial cel shading + inverted-hull outlines +
// painterly sky/water shaders + fog. Deterministic terrain/props from worldgen.
import * as THREE from 'three';
import { heightAt, slopeAt, biomeColor, scatter, WORLD } from './worldgen.js';
import { Net } from './net.js';

const MODULE = 'originlands';
const CHUNK = 48;   // must match the reducer module

const canvas = document.getElementById('view');
const renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
renderer.outputColorSpace = THREE.SRGBColorSpace;
renderer.toneMapping = THREE.ACESFilmicToneMapping;
renderer.toneMappingExposure = 1.05;
renderer.shadowMap.enabled = true;
renderer.shadowMap.type = THREE.PCFSoftShadowMap;

const scene = new THREE.Scene();
const HORIZON = new THREE.Color(0xbfe3ff);
scene.fog = new THREE.FogExp2(HORIZON.getHex(), 0.0065);

const camera = new THREE.PerspectiveCamera(58, innerWidth / innerHeight, 0.1, 1200);

// ---- lighting ------------------------------------------------------------
const hemi = new THREE.HemisphereLight(0xdfefff, 0x6b7a55, 0.85);
scene.add(hemi);
const sun = new THREE.DirectionalLight(0xfff2d8, 2.1);
sun.position.set(60, 90, 40);
sun.castShadow = true;
sun.shadow.mapSize.set(2048, 2048);
const sc = sun.shadow.camera;
sc.near = 1; sc.far = 400; sc.left = -120; sc.right = 120; sc.top = 120; sc.bottom = -120;
sun.shadow.bias = -0.0004;
scene.add(sun);
scene.add(new THREE.AmbientLight(0x9fb0c8, 0.25));

// ---- toon gradient ramp (cel steps) -------------------------------------
function makeGradient(steps) {
  const data = new Uint8Array(steps);
  for (let i = 0; i < steps; i++) data[i] = Math.round((i / (steps - 1)) * 255);
  const tex = new THREE.DataTexture(data, steps, 1, THREE.RedFormat);
  tex.needsUpdate = true;
  tex.minFilter = tex.magFilter = THREE.NearestFilter;
  return tex;
}
const RAMP = makeGradient(4);
const toon = (color, opts = {}) =>
  new THREE.MeshToonMaterial({ color, gradientMap: RAMP, ...opts });

// ---- inverted-hull outline ----------------------------------------------
// Expand a geometry along its normals so a BackSide black copy reads as ink.
function expand(geo, t) {
  const g = geo.clone();
  const pos = g.attributes.position, nor = g.attributes.normal;
  for (let i = 0; i < pos.count; i++) {
    pos.setXYZ(i,
      pos.getX(i) + nor.getX(i) * t,
      pos.getY(i) + nor.getY(i) * t,
      pos.getZ(i) + nor.getZ(i) * t);
  }
  pos.needsUpdate = true;
  return g;
}
const INK = new THREE.MeshBasicMaterial({ color: 0x1c2230, side: THREE.BackSide });
function outlined(mesh, t = 0.04) {
  const o = new THREE.Mesh(expand(mesh.geometry, t), INK);
  mesh.add(o);
  return mesh;
}

// ---- sky dome ------------------------------------------------------------
const sky = new THREE.Mesh(
  new THREE.SphereGeometry(700, 32, 16),
  new THREE.ShaderMaterial({
    side: THREE.BackSide, depthWrite: false, fog: false,
    uniforms: {
      top: { value: new THREE.Color(0x3d84d6) },
      mid: { value: new THREE.Color(0xbfe3ff) },
      bot: { value: new THREE.Color(0xeaf6ff) },
      sunDir: { value: sun.position.clone().normalize() },
    },
    vertexShader: `varying vec3 vD; void main(){ vD = normalize(position); gl_Position = projectionMatrix * modelViewMatrix * vec4(position,1.0); }`,
    fragmentShader: `
      varying vec3 vD; uniform vec3 top, mid, bot, sunDir;
      void main(){
        float h = vD.y;
        vec3 c = h > 0.0 ? mix(mid, top, pow(clamp(h,0.0,1.0),0.7))
                         : mix(mid, bot, clamp(-h*3.0,0.0,1.0));
        float s = pow(max(dot(vD, normalize(sunDir)),0.0), 220.0);
        c += vec3(1.0,0.92,0.7) * s * 0.9;              // sun disc/glow
        float band = smoothstep(0.0,0.06,abs(h));       // soft horizon band
        c = mix(vec3(1.0), c, band);
        gl_FragColor = vec4(c,1.0);
      }`,
  })
);
sky.frustumCulled = false;
scene.add(sky);

// ---- terrain -------------------------------------------------------------
const TERRAIN = 520, SEG = 300;
const tgeo = new THREE.PlaneGeometry(TERRAIN, TERRAIN, SEG, SEG);
tgeo.rotateX(-Math.PI / 2);
{
  const p = tgeo.attributes.position;
  const colors = new Float32Array(p.count * 3);
  for (let i = 0; i < p.count; i++) {
    const x = p.getX(i), z = p.getZ(i);
    const h = heightAt(x, z);
    p.setY(i, h);
    const c = biomeColor(h, slopeAt(x, z));
    colors[i * 3] = c[0]; colors[i * 3 + 1] = c[1]; colors[i * 3 + 2] = c[2];
  }
  tgeo.setAttribute('color', new THREE.BufferAttribute(colors, 3));
  tgeo.computeVertexNormals();
}
const terrain = new THREE.Mesh(tgeo, toon(0xffffff, { vertexColors: true }));
terrain.receiveShadow = true;
scene.add(terrain);

// ---- water ---------------------------------------------------------------
const water = new THREE.Mesh(
  new THREE.PlaneGeometry(TERRAIN, TERRAIN, 1, 1).rotateX(-Math.PI / 2),
  new THREE.ShaderMaterial({
    transparent: true, fog: true,
    uniforms: {
      t: { value: 0 },
      shallow: { value: new THREE.Color(0x8fe0d8) },
      deep: { value: new THREE.Color(0x2b7fb8) },
      fogColor: { value: HORIZON }, fogDensity: { value: 0.0065 },
    },
    vertexShader: `
      varying vec3 vW; uniform float t;
      void main(){
        vec3 p = position;
        p.y += sin(p.x*0.08 + t)*0.18 + cos(p.z*0.10 + t*1.3)*0.14;
        vW = (modelMatrix * vec4(p,1.0)).xyz;
        gl_Position = projectionMatrix * modelViewMatrix * vec4(p,1.0);
      }`,
    fragmentShader: `
      varying vec3 vW; uniform float t; uniform vec3 shallow, deep, fogColor; uniform float fogDensity;
      void main(){
        float ripple = sin(vW.x*0.6 + t*1.5) * cos(vW.z*0.5 - t*1.2);
        float band = smoothstep(0.3, 0.55, ripple*0.5+0.5);   // cel water
        vec3 col = mix(deep, shallow, band*0.6 + 0.2);
        col += band * 0.10;                                    // foam glint
        float d = length(vW - cameraPosition);
        float f = 1.0 - exp(-fogDensity*fogDensity*d*d);
        col = mix(col, fogColor, clamp(f,0.0,1.0));
        gl_FragColor = vec4(col, 0.86);
      }`,
  })
);
water.position.y = WORLD.seaLevel;
scene.add(water);

// ---- instanced props (trees + rocks) with instanced outlines -------------
function instanced(geo, mat, transforms, outlineT) {
  const m = new THREE.InstancedMesh(geo, mat, transforms.length);
  m.castShadow = true; m.receiveShadow = true;
  const dummy = new THREE.Object3D();
  transforms.forEach((tr, i) => {
    dummy.position.set(tr.x, tr.y, tr.z);
    dummy.rotation.set(0, tr.rot, 0);
    dummy.scale.setScalar(tr.scl);
    dummy.updateMatrix();
    m.setMatrixAt(i, dummy.matrix);
  });
  m.instanceMatrix.needsUpdate = true;
  scene.add(m);
  let o = null;
  if (outlineT) {
    o = new THREE.InstancedMesh(expand(geo, outlineT), INK, transforms.length);
    o.frustumCulled = false;
    for (let i = 0; i < transforms.length; i++) { m.getMatrixAt(i, dummy.matrix); o.setMatrixAt(i, dummy.matrix); }
    o.instanceMatrix.needsUpdate = true;
    scene.add(o);
  }
  return { mesh: m, outline: o };
}

// paint a whole geometry one colour (baked into vertex colors so many species
// share a single instanced material)
function paint(geo, hex) {
  const c = new THREE.Color(hex);
  const n = geo.attributes.position.count;
  const col = new Float32Array(n * 3);
  for (let i = 0; i < n; i++) { col[i * 3] = c.r; col[i * 3 + 1] = c.g; col[i * 3 + 2] = c.b; }
  geo.setAttribute('color', new THREE.BufferAttribute(col, 3));
  return geo;
}
// merge pre-positioned geometries into one (no addons): concat position/normal/color
function mergeGeoms(geos) {
  const parts = geos.map(g => (g.index ? g.toNonIndexed() : g));
  let total = 0; parts.forEach(g => total += g.attributes.position.count);
  const pos = new Float32Array(total * 3), nor = new Float32Array(total * 3), col = new Float32Array(total * 3);
  let o = 0;
  for (const g of parts) {
    const p = g.attributes.position, nn = g.attributes.normal, cc = g.attributes.color;
    for (let i = 0; i < p.count; i++, o++) {
      pos[o * 3] = p.getX(i); pos[o * 3 + 1] = p.getY(i); pos[o * 3 + 2] = p.getZ(i);
      nor[o * 3] = nn.getX(i); nor[o * 3 + 1] = nn.getY(i); nor[o * 3 + 2] = nn.getZ(i);
      col[o * 3] = cc ? cc.getX(i) : 1; col[o * 3 + 1] = cc ? cc.getY(i) : 1; col[o * 3 + 2] = cc ? cc.getZ(i) : 1;
    }
  }
  const m = new THREE.BufferGeometry();
  m.setAttribute('position', new THREE.BufferAttribute(pos, 3));
  m.setAttribute('normal', new THREE.BufferAttribute(nor, 3));
  m.setAttribute('color', new THREE.BufferAttribute(col, 3));
  return m;
}

// species geometries (base at y=0, scaled per-instance)
const treeRound = () => mergeGeoms([
  paint(new THREE.CylinderGeometry(0.14, 0.22, 1.4, 6).translate(0, 0.7, 0), 0x6b4a2b),
  paint(new THREE.IcosahedronGeometry(1.1, 0).translate(0, 2.1, 0), 0x4f8f43),
]);
const treeCedar = () => mergeGeoms([
  paint(new THREE.CylinderGeometry(0.12, 0.2, 2.2, 6).translate(0, 1.1, 0), 0x5b3f26),
  paint(new THREE.ConeGeometry(1.05, 1.5, 7).translate(0, 2.5, 0), 0x2f6b3a),
  paint(new THREE.ConeGeometry(0.82, 1.35, 7).translate(0, 3.35, 0), 0x357040),
  paint(new THREE.ConeGeometry(0.55, 1.2, 7).translate(0, 4.15, 0), 0x3c7a46),
]);
const treeSapling = () => mergeGeoms([
  paint(new THREE.CylinderGeometry(0.06, 0.09, 0.5, 5).translate(0, 0.25, 0), 0x6b4a2b),
  paint(new THREE.IcosahedronGeometry(0.5, 0).translate(0, 0.85, 0), 0x6cc255),
]);
const rockBase = () => paint(new THREE.IcosahedronGeometry(0.9, 0).scale(1, 0.72, 1), 0x7d7a80);

const GEO = {
  sapling: treeSapling(), round: treeRound(), cedar: treeCedar(),
  pebble: rockBase(), rock: rockBase(), boulder: rockBase(),
};
const OUTLINE_T = { sapling: 0.03, round: 0.05, cedar: 0.05, pebble: 0.03, rock: 0.04, boulder: 0.07 };
const VC = toon(0xffffff, { vertexColors: true });

const props = scatter(-TERRAIN / 2 + 8, -TERRAIN / 2 + 8, TERRAIN / 2 - 8, TERRAIN / 2 - 8);
const bySub = {};
for (const p of props) {
  const y = p.h - (p.kind === 'rock' ? 0.18 * p.scl : 0);   // sink rocks into ground
  (bySub[p.sub] || (bySub[p.sub] = [])).push({ x: p.x, y, z: p.z, rot: p.rot, scl: p.scl, id: p.id });
}
// treeRefs lets us hide/restore an individual harvested tree instance
const treeRefs = new Map();   // treeId -> { mesh, outline, idx, mat, hidden }
const treeList = [];          // harvestable procedural trees: { id, x, z }
const TREE_SUBS = new Set(['sapling', 'round', 'cedar']);
for (const sub in bySub) {
  const arr = bySub[sub];
  const { mesh, outline } = instanced(GEO[sub], VC, arr, OUTLINE_T[sub]);
  if (TREE_SUBS.has(sub)) {
    const tmp = new THREE.Matrix4();
    arr.forEach((tr, i) => {
      mesh.getMatrixAt(i, tmp);
      treeRefs.set(tr.id, { mesh, outline, idx: i, mat: tmp.clone(), hidden: false });
      treeList.push({ id: tr.id, x: tr.x, z: tr.z });
    });
  }
}
const ZERO_M = new THREE.Matrix4().makeScale(0, 0, 0);
function hideTree(id) {
  const r = treeRefs.get(id); if (!r || r.hidden) return; r.hidden = true;
  r.mesh.setMatrixAt(r.idx, ZERO_M); r.mesh.instanceMatrix.needsUpdate = true;
  if (r.outline) { r.outline.setMatrixAt(r.idx, ZERO_M); r.outline.instanceMatrix.needsUpdate = true; }
}
function showTree(id) {
  const r = treeRefs.get(id); if (!r || !r.hidden) return; r.hidden = false;
  r.mesh.setMatrixAt(r.idx, r.mat); r.mesh.instanceMatrix.needsUpdate = true;
  if (r.outline) { r.outline.setMatrixAt(r.idx, r.mat); r.outline.instanceMatrix.needsUpdate = true; }
}

// ---- character -----------------------------------------------------------
function buildCharacter(skin = 0xf0c27a, shirt = 0x4aa3d8, pants = 0x394a6b) {
  const g = new THREE.Group();
  const add = (geo, mat, x, y, z) => { const m = new THREE.Mesh(geo, mat); m.position.set(x, y, z); m.castShadow = true; outlined(m, 0.03); g.add(m); return m; };
  add(new THREE.CapsuleGeometry(0.32, 0.5, 4, 10), toon(shirt), 0, 1.15, 0);       // torso
  const head = add(new THREE.SphereGeometry(0.34, 16, 14), toon(skin), 0, 1.85, 0);
  const eye = new THREE.Mesh(new THREE.SphereGeometry(0.05, 8, 8), new THREE.MeshBasicMaterial({ color: 0x1c2230 }));
  const eL = eye.clone(), eR = eye.clone(); eL.position.set(-0.12, 1.9, 0.3); eR.position.set(0.12, 1.9, 0.3); g.add(eL, eR);
  const armL = add(new THREE.CapsuleGeometry(0.11, 0.5, 4, 8), toon(skin), -0.46, 1.2, 0);
  const armR = add(new THREE.CapsuleGeometry(0.11, 0.5, 4, 8), toon(skin), 0.46, 1.2, 0);
  const legL = add(new THREE.CapsuleGeometry(0.13, 0.5, 4, 8), toon(pants), -0.18, 0.5, 0);
  const legR = add(new THREE.CapsuleGeometry(0.13, 0.5, 4, 8), toon(pants), 0.18, 0.5, 0);
  g.userData = { head, armL, armR, legL, legR };
  return g;
}
let player = buildCharacter();
scene.add(player);
const pstate = { x: 0, z: 0, y: 0, vy: 0, yaw: 0, grounded: true, speed: 0 };
pstate.y = heightAt(0, 0);
player.position.set(0, pstate.y, 0);

// ---- input ---------------------------------------------------------------
const keys = {};
const typing = e => e.target && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA');
addEventListener('keydown', e => { if (typing(e)) return; keys[e.key.toLowerCase()] = true; if (e.key === ' ') e.preventDefault(); });
addEventListener('keyup', e => { if (typing(e)) return; keys[e.key.toLowerCase()] = false; });
let camYaw = 0, camPitch = 0.5, camDist = 9, dragging = false, px = 0, py = 0;
canvas.addEventListener('pointerdown', e => { dragging = true; px = e.clientX; py = e.clientY; });
addEventListener('pointerup', () => dragging = false);
addEventListener('pointermove', e => {
  if (!dragging) return;
  camYaw -= (e.clientX - px) * 0.006; camPitch += (e.clientY - py) * 0.005;
  camPitch = Math.max(0.05, Math.min(1.3, camPitch)); px = e.clientX; py = e.clientY;
});
addEventListener('wheel', e => { camDist = Math.max(4, Math.min(20, camDist + e.deltaY * 0.01)); }, { passive: true });

// ---- loop ----------------------------------------------------------------
const clock = new THREE.Clock();
const WALK = 9.5, GRAV = 26, JUMP = 9.5;
function tick() {
  const dt = Math.min(0.05, clock.getDelta());
  const t = clock.elapsedTime;

  // movement relative to camera yaw
  let ix = (keys['d'] || keys['arrowright'] ? 1 : 0) - (keys['a'] || keys['arrowleft'] ? 1 : 0);
  let iz = (keys['s'] || keys['arrowdown'] ? 1 : 0) - (keys['w'] || keys['arrowup'] ? 1 : 0);
  let moving = ix || iz;
  if (moving) {
    const len = Math.hypot(ix, iz); ix /= len; iz /= len;
    // Camera-relative basis: iz = forward/back (W/S), ix = strafe (A/D).
    // "forward" is the direction the camera faces (away from the camera),
    // derived from the camera azimuth so movement always tracks the view.
    // (The camera is client-only; in M1 the resulting world-space intent is
    // what gets sent to the authoritative move reducer.)
    const sinY = Math.sin(camYaw), cosY = Math.cos(camYaw);
    const dx = ix * cosY + iz * sinY;
    const dz = -ix * sinY + iz * cosY;
    pstate.x += dx * WALK * dt; pstate.z += dz * WALK * dt;
    pstate.yaw = Math.atan2(dx, dz);
  }
  pstate.speed += ((moving ? 1 : 0) - pstate.speed) * Math.min(1, dt * 12);

  // gravity + ground follow
  const ground = heightAt(pstate.x, pstate.z);
  if (pstate.grounded && keys[' ']) { pstate.vy = JUMP; pstate.grounded = false; }
  pstate.vy -= GRAV * dt; pstate.y += pstate.vy * dt;
  if (pstate.y <= ground) { pstate.y = ground; pstate.vy = 0; pstate.grounded = true; }

  player.position.set(pstate.x, pstate.y, pstate.z);
  player.rotation.y += (pstate.yaw - player.rotation.y) * Math.min(1, dt * 12);

  // walk bob
  const { armL, armR, legL, legR, head } = player.userData;
  const swing = Math.sin(t * 11) * 0.5 * pstate.speed;
  legL.rotation.x = swing; legR.rotation.x = -swing;
  armL.rotation.x = -swing; armR.rotation.x = swing;
  head.position.y = 1.85 + Math.abs(Math.sin(t * 11)) * 0.03 * pstate.speed;

  // third-person camera
  const tgt = new THREE.Vector3(pstate.x, pstate.y + 1.6, pstate.z);
  const cp = new THREE.Vector3(
    tgt.x + Math.sin(camYaw) * Math.cos(camPitch) * camDist,
    tgt.y + Math.sin(camPitch) * camDist,
    tgt.z + Math.cos(camYaw) * Math.cos(camPitch) * camDist
  );
  const minY = heightAt(cp.x, cp.z) + 1.2;             // don't clip into terrain
  if (cp.y < minY) cp.y = minY;
  camera.position.lerp(cp, Math.min(1, dt * 8));
  camera.lookAt(tgt);

  updateMultiplayer(dt, t);

  water.material.uniforms.t.value = t;
  sky.position.copy(camera.position);

  renderer.render(scene, camera);
  requestAnimationFrame(tick);
}

addEventListener('resize', () => {
  camera.aspect = innerWidth / innerHeight; camera.updateProjectionMatrix();
  renderer.setSize(innerWidth, innerHeight);
});
renderer.setSize(innerWidth, innerHeight);

// ======================================================================
// Multiplayer — accounts + AoI streaming over the OriginDB websocket
// ======================================================================
let net = null, session = null, started = false, myChunk = null, aoiSubIds = [];
let lastSendT = 0, lastSX = 1e9, lastSZ = 1e9, lastSYaw = 1e9;
const remotes = new Map();   // name -> { g, tx, tz, tyaw, chunk }

function chunkKey(x, z) { return Math.floor(x / CHUNK) + ':' + Math.floor(z / CHUNK); }
function aoiChunks() {
  const cx = Math.floor(pstate.x / CHUNK), cz = Math.floor(pstate.z / CHUNK);
  const out = [];
  for (let dx = -1; dx <= 1; dx++) for (let dz = -1; dz <= 1; dz++) out.push((cx + dx) + ':' + (cz + dz));
  return out;
}
const aoiWhere = (chunks) => "SELECT * FROM players WHERE " + chunks.map(c => `chunk='${c}'`).join(' OR ');

function applyAppearance(a) {
  a = a || {};
  const pos = player.position.clone(), ry = player.rotation.y;
  scene.remove(player);
  player = buildCharacter(a.skin ?? 0xf0c27a, a.shirt ?? 0x4aa3d8, a.pants ?? 0x394a6b);
  player.position.copy(pos); player.rotation.y = ry;
  scene.add(player);
}

// initial_state hands us the row object; changefeed hands a JSON string
// (optionally wrapped as {columns:{...}})
function coerceRow(raw) {
  if (typeof raw === 'string') { try { raw = JSON.parse(raw); } catch { return null; } }
  if (raw && raw.columns) raw = raw.columns;
  return raw;
}
function upsertRemote(raw) {
  const row = coerceRow(raw);
  if (!row || !row.name) return;
  if (session && row.name === session.name) return;          // that's me
  const x = +row.x || 0, z = +row.z || 0, yaw = +row.yaw || 0;
  const chunk = row.chunk || chunkKey(x, z);
  let r = remotes.get(row.name);
  if (!r) {
    const a = row.appearance || {};
    const g = buildCharacter(a.skin ?? 0xd9a066, a.shirt ?? 0x9c5bd6, a.pants ?? 0x394a6b);
    g.position.set(x, heightAt(x, z), z); g.rotation.y = yaw;
    scene.add(g);
    remotes.set(row.name, { g, tx: x, tz: z, tyaw: yaw, chunk });
  } else {
    r.tx = x; r.tz = z; r.tyaw = yaw; r.chunk = chunk;
  }
}
function removeRemote(name) { const r = remotes.get(name); if (r) { scene.remove(r.g); remotes.delete(name); } }

// ---- M2 sustainability: harvest / seeds / plant / grow --------------------
const GROW_MS = 45000, RESPAWN_MS = 180000, REACH = 4.5;   // GROW/RESPAWN match the module
const inv = { wood: 0, seeds: 0 };
const harvestedAt = new Map();   // procedural treeId -> epoch ms harvested
const plantedTrees = new Map();  // plantId -> { group, plantedAt, x, z }

function makePlantedTree() {
  const g = new THREE.Group();
  const m = new THREE.Mesh(GEO.round, VC); m.castShadow = true;
  g.add(m, new THREE.Mesh(expand(GEO.round, 0.05), INK));
  return g;
}
function onHarvested(id, row) {
  const at = +row.at || Date.now();
  harvestedAt.set(id, at);
  if (Date.now() - at < RESPAWN_MS) hideTree(id); else showTree(id);
}
function onPlanted(id, row) {
  const x = +row.x || 0, z = +row.z || 0, plantedAt = +row.plantedAt || Date.now();
  let pt = plantedTrees.get(id);
  if (!pt) {
    const group = makePlantedTree();
    group.position.set(x, heightAt(x, z), z);
    group.rotation.y = (x * 13.1 + z * 7.7) % 6.28;
    scene.add(group);
    plantedTrees.set(id, { group, plantedAt, x, z });
  } else { pt.plantedAt = plantedAt; pt.x = x; pt.z = z; }
}
function removePlanted(id) { const pt = plantedTrees.get(id); if (pt) { scene.remove(pt.group); plantedTrees.delete(id); } }

function routeRow(key, raw, aoi) {
  const pfx = key ? key[0] : '';
  const row = coerceRow(raw);
  if (!row) return;
  if (aoi && row.chunk && !aoi.has(row.chunk)) return;   // AoI filter for snapshots
  if (pfx === 'p') { if (row.name) upsertRemote(row); }
  else if (pfx === 'h') onHarvested(key.slice(2), row);
  else if (pfx === 'g') onPlanted(key.slice(2), row);
}

function onNetEvent(m) {
  if (m.type === 'initial_state') {
    const aoi = new Set(aoiChunks());
    for (const row of (m.rows || [])) routeRow(row.key, row.data != null ? row.data : row, aoi);
  } else if (m.type === 'sql_changefeed_event') {
    const key = m.key || '';
    if (m.operation === 'DELETE') {
      const pfx = key[0];
      if (pfx === 'p') removeRemote(key.slice(2));
      else if (pfx === 'g') removePlanted(key.slice(2));
      else if (pfx === 'h') { harvestedAt.delete(key.slice(2)); showTree(key.slice(2)); }
      return;
    }
    routeRow(key, m.new_value, null);
  }
}

// interaction ---------------------------------------------------------------
function nearestProcTree() {
  let best = null, bd = REACH * REACH;
  for (const t of treeList) {
    const r = treeRefs.get(t.id); if (r && r.hidden) continue;
    const dx = t.x - pstate.x, dz = t.z - pstate.z, d = dx * dx + dz * dz;
    if (d < bd) { bd = d; best = t; }
  }
  return best;
}
function nearestMaturePlanted() {
  let best = null, bd = REACH * REACH;
  for (const [id, pt] of plantedTrees) {
    if (Date.now() - pt.plantedAt < GROW_MS) continue;
    const dx = pt.x - pstate.x, dz = pt.z - pstate.z, d = dx * dx + dz * dz;
    if (d < bd) { bd = d; best = { id, pt }; }
  }
  return best;
}
function onGain(res) {
  if (!res || res.success === false) { if (res && res.error) popup(res.error.replace(/^aborted:\s*/, '')); return; }
  let j = null; try { j = JSON.parse(res.result); } catch {}
  if (!j || j.ok === false) { popup((j && j.error) || 'failed'); return; }
  if (j.wood != null) inv.wood = j.wood;
  if (j.seeds != null) inv.seeds = j.seeds;
  updateHud();
  if (j.gotWood || j.gotSeeds) popup(`+${j.gotWood || 0} wood   +${j.gotSeeds || 0} seeds`);
  else if (j.plantId) popup('planted a seed 🌱');
}
async function tryHarvest() {
  if (!session) return;
  const mp = nearestMaturePlanted();
  if (mp) { onGain(await net.call('harvestPlanted', [session.name, mp.id], true)); return; }
  const t = nearestProcTree();
  if (t) onGain(await net.call('harvestTree', [session.name, t.id, t.x, t.z], true));
  else popup('nothing to harvest nearby');
}
async function tryPlant() {
  if (!session) return;
  if (inv.seeds < 1) { popup('no seeds — harvest a tree first'); return; }
  onGain(await net.call('plantSeed', [session.name, pstate.x, pstate.z], true));
}
addEventListener('keydown', e => {
  if (typing(e) || !started) return;
  const k = e.key.toLowerCase();
  if (k === 'e') tryHarvest(); else if (k === 'q') tryPlant();
});

let hudEl = null, popEl = null, popT = 0;
function updateHud() {
  if (!hudEl) hudEl = document.getElementById('hud');
  if (hudEl) hudEl.innerHTML = `🪵 <b>${inv.wood | 0}</b>&nbsp;&nbsp;🌰 <b>${inv.seeds | 0}</b>`;
}
function popup(text) {
  if (!popEl) popEl = document.getElementById('pop');
  if (!popEl) return;
  popEl.textContent = text; popEl.style.opacity = '1'; popT = performance.now();
}

async function refreshAoI() {
  if (!net) return;
  const chunks = aoiChunks();
  const where = chunks.map(c => `chunk='${c}'`).join(' OR ');
  const old = aoiSubIds;
  // one AoI subscription per table — players, harvested trees, planted trees
  aoiSubIds = await Promise.all(['players', 'harvested', 'planted'].map(
    tb => net.subscribe(`SELECT * FROM ${tb} WHERE ${where}`)));
  for (const id of old) net.unsubscribe(id);
  // prune anything now outside the AoI
  const set = new Set(chunks);
  for (const [name, r] of remotes) if (!set.has(r.chunk)) removeRemote(name);
  for (const [id, pt] of plantedTrees) if (!set.has(chunkKey(pt.x, pt.z))) removePlanted(id);
}

function updateMultiplayer(dt, t) {
  if (!started) return;
  // interpolate remote avatars toward their last authoritative position
  for (const r of remotes.values()) {
    const k = Math.min(1, dt * 10);
    const moving = Math.hypot(r.tx - r.g.position.x, r.tz - r.g.position.z) > 0.05;
    r.g.position.x += (r.tx - r.g.position.x) * k;
    r.g.position.z += (r.tz - r.g.position.z) * k;
    r.g.position.y += (heightAt(r.g.position.x, r.g.position.z) - r.g.position.y) * k;
    let d = r.tyaw - r.g.rotation.y; while (d > Math.PI) d -= 6.28318; while (d < -Math.PI) d += 6.28318;
    r.g.rotation.y += d * k;
    const sw = moving ? Math.sin(t * 11) * 0.5 : 0;
    const u = r.g.userData;
    if (u.legL) { u.legL.rotation.x = sw; u.legR.rotation.x = -sw; u.armL.rotation.x = -sw; u.armR.rotation.x = sw; }
  }
  // grow planted trees — a pure function of elapsed time, no server tick needed
  const nowMs = Date.now();
  for (const pt of plantedTrees.values()) {
    pt.group.scale.setScalar(Math.max(0.08, Math.min(1, (nowMs - pt.plantedAt) / GROW_MS)));
  }
  // natural respawn of harvested procedural trees
  for (const [id, at] of harvestedAt) if (nowMs - at > RESPAWN_MS) { harvestedAt.delete(id); showTree(id); }
  // fade the action popup
  if (popEl && popEl.style.opacity === '1' && performance.now() - popT > 1400) popEl.style.opacity = '0';
  // AoI: re-subscribe when we cross a chunk boundary
  const ck = chunkKey(pstate.x, pstate.z);
  if (ck !== myChunk) { myChunk = ck; refreshAoI(); }
  // send our position ~12 Hz when it changed (server-authoritative write)
  const now = performance.now();
  if (session && now - lastSendT > 80 &&
      (Math.hypot(pstate.x - lastSX, pstate.z - lastSZ) > 0.02 || Math.abs(pstate.yaw - lastSYaw) > 0.03)) {
    net.call('move', [session.name, pstate.x, pstate.z, pstate.yaw]);
    lastSX = pstate.x; lastSZ = pstate.z; lastSYaw = pstate.yaw; lastSendT = now;
  }
}

window.OriginLands = {
  state: pstate,
  heightAt,
  setCam(yaw, pitch, dist) { camYaw = yaw; camPitch = pitch; camDist = dist; },
  teleport(x, z) { pstate.x = x; pstate.z = z; pstate.y = heightAt(x, z); pstate.vy = 0; },
  setAppearance: applyAppearance,
  playerCount: () => remotes.size + (started ? 1 : 0),
  dbg: () => ({
    inv: { ...inv }, planted: plantedTrees.size, remotes: remotes.size,
    nearTree: treeList.reduce((b, t) => { const d = Math.hypot(t.x - pstate.x, t.z - pstate.z); return d < b.d ? { d, id: t.id, x: t.x, z: t.z } : b; }, { d: 1e9 }),
  }),
  tryHarvest: () => tryHarvest(), tryPlant: () => tryPlant(),

  // called by the login / creator UI
  async auth(mode, name, password, appearance) {
    try {
      if (!net) {
        const scheme = location.protocol === 'https:' ? 'wss' : 'ws';
        net = new Net(`${scheme}://${location.host}/`, MODULE);
        net.onEvent = onNetEvent;
        net.onClose = () => { started = false; };
        await net.connect();
      }
      const reducer = mode === 'create' ? 'createCharacter' : 'login';
      // appearance goes as a JSON string — the ws call path only forwards scalars
      const args = mode === 'create' ? [name, password, JSON.stringify(appearance)] : [name, password];
      const res = await net.call(reducer, args, true);
      if (!res.success) return { ok: false, error: res.error || 'call failed' };
      let pub = null; try { pub = JSON.parse(res.result); } catch {}
      if (!pub || pub.ok === false) return { ok: false, error: (pub && pub.error) || 'failed' };
      // enter the world at the persisted position
      session = { name: pub.name, appearance: pub.appearance || appearance };
      inv.wood = +pub.wood || 0; inv.seeds = +pub.seeds || 0; updateHud();
      applyAppearance(session.appearance);
      pstate.x = +pub.x || 0; pstate.z = +pub.z || 0; pstate.y = heightAt(pstate.x, pstate.z);
      pstate.yaw = +pub.yaw || 0; pstate.vy = 0;
      camera.position.set(pstate.x + Math.sin(camYaw) * 9, pstate.y + 6, pstate.z + Math.cos(camYaw) * 9);
      myChunk = chunkKey(pstate.x, pstate.z);
      started = true;
      await refreshAoI();
      addEventListener('beforeunload', () => { try { net.call('leaveWorld', [session.name]); } catch {} });
      return { ok: true };
    } catch (e) {
      return { ok: false, error: String((e && e.message) || e) };
    }
  },
};

// start the render loop last, once every module-level binding is initialized
tick();
