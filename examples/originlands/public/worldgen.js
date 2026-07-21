// Deterministic, seeded world generation. The SAME functions run on the client
// (to build terrain + scatter) and will run server-side later so every player
// sees the identical world with zero rows stored — only player-driven deltas
// (harvested nodes, structures) ever hit the database.

const F = 4294967296; // 2^32

// --- integer hash -> [0,1) -----------------------------------------------
function hash2(ix, iz, seed) {
  let h = (ix | 0) * 374761393 + (iz | 0) * 668265263 + (seed | 0) * 362437;
  h = (h ^ (h >>> 13)) >>> 0;
  h = (h * 1274126177) >>> 0;
  return h / F;
}

function smooth(t) { return t * t * (3 - 2 * t); }

// --- value noise ----------------------------------------------------------
function valueNoise(x, z, seed) {
  const ix = Math.floor(x), iz = Math.floor(z);
  const fx = x - ix, fz = z - iz;
  const a = hash2(ix, iz, seed);
  const b = hash2(ix + 1, iz, seed);
  const c = hash2(ix, iz + 1, seed);
  const d = hash2(ix + 1, iz + 1, seed);
  const ux = smooth(fx), uz = smooth(fz);
  return a * (1 - ux) * (1 - uz) + b * ux * (1 - uz) + c * (1 - ux) * uz + d * ux * uz;
}

// fractal brownian motion
function fbm(x, z, seed, octaves = 5) {
  let amp = 1, freq = 1, sum = 0, norm = 0;
  for (let o = 0; o < octaves; o++) {
    sum += amp * valueNoise(x * freq, z * freq, seed + o * 97);
    norm += amp;
    amp *= 0.5;
    freq *= 2.02;
  }
  return sum / norm;
}

export const WORLD = {
  seed: 1337,
  scale: 0.010,      // noise frequency of the terrain
  amplitude: 48,     // peak height in metres
  seaLevel: 8.0,     // high enough that basins flood into lakes/coastline
  beach: 10.5,
};

// Height (metres) at a world XZ. Continents from low-freq fbm, ridged detail
// on top, flattened near/under the sea for beaches.
export function heightAt(x, z) {
  const s = WORLD.seed;
  const continent = fbm(x * WORLD.scale * 0.35, z * WORLD.scale * 0.35, s, 4);
  let h = Math.pow(continent, 2.0);            // wide low basins, sharper peaks
  const detail = fbm(x * WORLD.scale, z * WORLD.scale, s + 11, 5) - 0.5;
  h = h * WORLD.amplitude + detail * 8.0;
  // gentle shelf around sea level for readable beaches
  if (h < WORLD.beach) h = WORLD.beach - (WORLD.beach - h) * 0.55;
  return h;
}

// Approximate surface normal via central differences (for slope-based shading).
export function slopeAt(x, z, eps = 1.2) {
  const hL = heightAt(x - eps, z), hR = heightAt(x + eps, z);
  const hD = heightAt(x, z - eps), hU = heightAt(x, z + eps);
  const nx = hL - hR, nz = hD - hU, ny = 2 * eps;
  const len = Math.hypot(nx, ny, nz);
  return ny / len; // 1 = flat, ->0 = vertical
}

// Painterly colour bands by height + slope. Returns [r,g,b] in 0..1.
export function biomeColor(h, slope) {
  const sand  = [0.86, 0.79, 0.55];
  const grass = [0.42, 0.66, 0.34];
  const grass2= [0.32, 0.56, 0.30];
  const rock  = [0.47, 0.45, 0.46];
  const snow  = [0.92, 0.94, 0.98];
  const lerp = (a, b, t) => a.map((v, i) => v + (b[i] - v) * t);

  if (h < WORLD.beach + 0.4) return sand;
  if (slope < 0.72) return rock;                       // cliffs = rock
  if (h > 30) return lerp(rock, snow, Math.min(1, (h - 30) / 10));
  const t = Math.min(1, (h - WORLD.beach) / 18);
  return lerp(grass, grass2, t * 0.7 + (slope < 0.9 ? 0.3 : 0));
}

// Deterministic scatter of props (trees, rocks) over an area. Cell-based so it
// tiles seamlessly and is stable across sessions. Skips water/beach/cliffs.
export function scatter(minX, minZ, maxX, maxZ, cell = 7) {
  const out = [];
  const s = WORLD.seed;
  for (let cx = Math.floor(minX / cell); cx <= Math.floor(maxX / cell); cx++) {
    for (let cz = Math.floor(minZ / cell); cz <= Math.floor(maxZ / cell); cz++) {
      const r = hash2(cx, cz, s + 555);
      if (r > 0.55) continue;                          // ~45% of cells populated
      const jx = hash2(cx, cz, s + 1) - 0.5;
      const jz = hash2(cx, cz, s + 2) - 0.5;
      const x = (cx + 0.5 + jx * 0.8) * cell;
      const z = (cz + 0.5 + jz * 0.8) * cell;
      const h = heightAt(x, z);
      if (h < WORLD.beach + 0.6 || h > 34) continue;   // no props on sand/snow
      if (slopeAt(x, z) < 0.86) continue;              // no props on steep rock
      const pick = hash2(cx, cz, s + 3);
      const kind = pick < 0.68 ? 'tree' : 'rock';
      const scl = 0.8 + hash2(cx, cz, s + 4) * 0.7;
      const rot = hash2(cx, cz, s + 5) * Math.PI * 2;
      out.push({ x, z, h, kind, scl, rot, id: `${cx}:${cz}` });
    }
  }
  return out;
}
