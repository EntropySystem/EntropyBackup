// bggen: renders Entropy Backup's built-in background videos as raw RGB frames on stdout.
// "bggen <scene> <frame>" renders a single frame, for previews.
// Every scene is a seamless loop: all motion is a function of the loop phase (0..2π), and rotations
// turn by exactly one symmetry step per loop. Pipe the frames into ffmpeg (see make-backgrounds.cmd):
//   bggen 1 | ffmpeg -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 30 -i - ... nebula-lattice.mp4
#include <io.h>
#include <fcntl.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

static const int W = 1280, H = 720, FPS = 30, SECONDS = 20, FRAMES = FPS * SECONDS;
static const float PI = 3.14159265f, TAU = 6.2831853f;

struct V3 { float r, g, b; };
static V3 operator+(V3 a, V3 b) { return {a.r + b.r, a.g + b.g, a.b + b.b}; }
static V3 operator*(V3 a, float k) { return {a.r * k, a.g * k, a.b * k}; }
static V3 Mix(V3 a, V3 b, float k) { return a * (1 - k) + b * k; }
static float Clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float Smooth(float a, float b, float x) { float t = Clamp01((x - a) / (b - a)); return t * t * (3 - 2 * t); }

// --- gradient noise and fractal sums ------------------------------------------------------
static inline uint32_t Hash(int x, int y) {
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static inline float Grad(int x, int y, float dx, float dy) {
    float a = (Hash(x, y) & 0xFFFF) / 65536.f * TAU;
    return cosf(a) * dx + sinf(a) * dy;
}
static float Noise(float x, float y) {  // -1..1 (roughly)
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - ix, fy = y - iy;
    float u = fx * fx * fx * (fx * (fx * 6 - 15) + 10), v = fy * fy * fy * (fy * (fy * 6 - 15) + 10);
    float a = Grad(ix, iy, fx, fy), b = Grad(ix + 1, iy, fx - 1, fy);
    float c = Grad(ix, iy + 1, fx, fy - 1), d = Grad(ix + 1, iy + 1, fx - 1, fy - 1);
    return 1.4f * (a + (b - a) * u + (c - a) * v + (a - b - c + d) * u * v);
}
static float Fbm(float x, float y, int oct = 5) {  // 0..1 (roughly)
    float s = 0, amp = 0.5f;
    for (int i = 0; i < oct; i++) {
        s += amp * Noise(x, y);
        float nx = 1.6f * x + 1.2f * y + 3.1f, ny = -1.2f * x + 1.6f * y + 7.7f;  // rotate and scale by 2
        x = nx; y = ny;
        amp *= 0.5f;
    }
    return 0.5f + 0.5f * s;
}
static void Rot(float& x, float& y, float a) { float c = cosf(a), s = sinf(a), nx = c * x - s * y; y = s * x + c * y; x = nx; }
// Folds the plane into a mirrored 60° wedge: the kaleidoscope, invariant under 60° rotation.
static void Fold6(float& x, float& y) {
    float r = sqrtf(x * x + y * y), a = atan2f(y, x);
    a = fmodf(a + 10 * TAU, PI / 3);
    a = fabsf(a - PI / 6);
    x = r * cosf(a); y = r * sinf(a);
}
// Distance to the nearest edge of a pointy-top hexagonal grid with unit cell width.
static float HexEdge(float x, float y) {
    const float sx = 1.f, sy = 1.7320508f;
    auto md = [](float v, float m) { return v - m * floorf(v / m); };
    float ax = md(x, sx) - sx / 2, ay = md(y, sy) - sy / 2;
    float bx = md(x - sx / 2, sx) - sx / 2, by = md(y - sy / 2, sy) - sy / 2;
    float gx = ax * ax + ay * ay < bx * bx + by * by ? ax : bx, gy = ax * ax + ay * ay < bx * bx + by * by ? ay : by;
    gx = fabsf(gx); gy = fabsf(gy);
    float hd = std::max(gx * 0.5f + gy * 0.8660254f, gx);
    return 0.5f - hd;
}
static float Line(float d, float w) { return expf(-(d * d) / (w * w)); }

static const V3 NAVY{0.020f, 0.040f, 0.085f}, DEEP{0.035f, 0.075f, 0.150f}, GOLD{0.95f, 0.78f, 0.44f}, TEAL{0.24f, 0.70f, 0.78f}, VIOLET{0.42f, 0.30f, 0.72f};

// --- 1. Nebula Lattice: a kaleidoscopic hex lattice turning slowly through a soft nebula ----
static V3 Nebula(float x, float y, float th) {
    float r = sqrtf(x * x + y * y);
    float ax = x, ay = y;
    Rot(ax, ay, th / 6);  // 60° per loop: the folded lattice repeats exactly
    Fold6(ax, ay);
    float haze = Fbm(ax * 1.4f + 2.f, ay * 1.4f + 5.f);
    float sa = 3.0f * (1 + 0.05f * sinf(th));
    float ea = HexEdge(ax * sa, ay * sa);
    float bx = x, by = y;
    Rot(bx, by, -th / 6);
    Fold6(bx, by);
    float sb = 5.4f * (1 - 0.04f * sinf(th));
    float eb = HexEdge(bx * sb + 0.25f, by * sb);
    float wave = 0.72f + 0.28f * sinf(th - r * 3.2f);  // a slow ripple travelling outward
    float fade = Smooth(1.75f, 0.1f, r), depth = 0.35f + 0.65f * Smooth(0.3f, 0.75f, haze);  // the nebula lights the lattice
    V3 c = Mix(NAVY * 0.7f, DEEP, Smooth(1.5f, 0.f, r));
    c = c + Mix(V3{0.03f, 0.08f, 0.20f}, V3{0.16f, 0.08f, 0.26f}, Smooth(0.45f, 0.8f, haze)) * (haze * haze * 1.1f * fade);
    c = c + GOLD * ((Line(ea, 0.02f) * 0.75f + Line(ea, 0.10f) * 0.12f) * wave * fade * depth);
    c = c + TEAL * ((Line(eb, 0.028f) * 0.40f + Line(eb, 0.13f) * 0.07f) * (1.4f - wave) * fade * (1.2f - depth) * 0.8f);
    return c;
}

// --- 2. Tidal Fractal: domain-warped fractal noise drifting like ink in water ----------------
static V3 Tidal(float x, float y, float th) {
    const float rr = 0.55f;  // the domain travels around a circle, so the loop closes exactly
    float c1 = cosf(th) * rr, s1 = sinf(th) * rr;
    float qx = Fbm(x + c1, y + s1), qy = Fbm(x + 5.2f - s1, y + 1.3f + c1);
    float px = Fbm(x + 3.5f * qx + 1.7f, y + 3.5f * qy + 9.2f), py = Fbm(x + 3.5f * qx + 8.3f, y + 3.5f * qy + 2.8f);
    float v = Fbm(x + 3.5f * px, y + 3.5f * py);
    V3 c = Mix(NAVY, V3{0.04f, 0.17f, 0.24f}, Smooth(0.25f, 0.75f, v));
    c = Mix(c, VIOLET * 0.55f, Smooth(0.45f, 0.95f, qx * qy * 1.6f) * 0.55f);
    c = Mix(c, TEAL * 0.75f, Smooth(0.55f, 0.85f, px) * 0.45f);
    c = c + GOLD * (Smooth(0.68f, 0.86f, v) * 0.10f);
    float vig = Smooth(2.3f, 0.3f, sqrtf((x - 1.6f) * (x - 1.6f) * 0.3f + (y - 1.1f) * (y - 1.1f)));
    return c * (0.55f + 0.45f * vig);
}

// --- 3. Sacred Geometry: flower-of-life linework and nested polygons turning slowly ----------
static float Poly(float x, float y, int n, float rad, float rot) {  // distance to a regular polygon's edge
    float r = sqrtf(x * x + y * y), a = atan2f(y, x) - rot, sec = TAU / n;
    a = fmodf(a + 20 * TAU, sec) - sec / 2;
    return r * cosf(a) - rad * cosf(PI / n);
}
static V3 Sacred(float x, float y, float th, float px) {
    float r = sqrtf(x * x + y * y);
    float breathe = 1 + 0.035f * sinf(th);
    float R = 0.27f * breathe;
    float fx = x, fy = y;
    Rot(fx, fy, th / 6);
    float flower = 0;
    for (int i = -2; i <= 2; i++)  // the 19 circles of the flower of life (hex lattice within two rings)
        for (int j = -2; j <= 2; j++) {
            if (abs(i + j) > 2) continue;
            float cx = R * (i + j * 0.5f), cy = R * j * 0.8660254f;
            float d = fabsf(sqrtf((fx - cx) * (fx - cx) + (fy - cy) * (fy - cy)) - R);
            flower = std::max(flower, Line(d, 1.3f * px));
        }
    flower = std::max(flower, Line(fabsf(r - 3 * R), 1.4f * px));
    flower = std::max(flower, Line(fabsf(r - 3 * R - 0.025f), 1.0f * px) * 0.7f);
    float polys = 0;
    const int n[4] = {3, 4, 6, 8};
    const float rad[4] = {1.02f, 1.17f, 1.32f, 1.47f}, turn[4] = {-1, 1, -1, 1};
    for (int k = 0; k < 4; k++)  // each polygon turns exactly one symmetry step per loop
        polys = std::max(polys, Line(Poly(x, y, n[k], rad[k] * breathe * 0.57f, turn[k] * th / n[k]), 1.2f * px));
    float wave = 0.7f + 0.3f * sinf(th - r * 4.f);
    float dust = Fbm(x * 2.2f + 11.f, y * 2.2f + 3.f);
    V3 c = Mix(NAVY, DEEP, Smooth(1.5f, 0.f, r));
    c = c + V3{0.03f, 0.06f, 0.14f} * (dust * dust);
    float fade = Smooth(1.95f, 0.2f, r);
    c = c + GOLD * (flower * 0.62f * wave * fade);
    c = c + TEAL * (polys * 0.42f * (1.3f - wave) * fade);
    float halo = expf(-fabsf(r - 3 * R) * 9.f) * 0.06f + expf(-r * 2.2f) * 0.05f;
    return c + GOLD * (halo * wave);
}

int main(int argc, char** argv) {
    int scene = argc > 1 ? atoi(argv[1]) : 1;
    if (scene < 1 || scene > 3) { fprintf(stderr, "usage: bggen <1|2|3>   (1 Nebula Lattice, 2 Tidal Fractal, 3 Sacred Geometry)\n"); return 3; }
    _setmode(_fileno(stdout), _O_BINARY);
    // Tidal Fractal is soft, so it is rendered at half size and upscaled; the others are rendered at full size.
    const int div = scene == 2 ? 2 : 1, rw = W / div, rh = H / div;
    std::vector<V3> img((size_t)rw * rh);
    std::vector<uint8_t> out((size_t)W * H * 3);
    static const int bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    unsigned nt = std::max(1u, std::thread::hardware_concurrency());
    int only = argc > 2 ? atoi(argv[2]) : -1;  // preview: render just this frame
    for (int f = only < 0 ? 0 : only; f < (only < 0 ? FRAMES : only + 1); f++) {
        float th = TAU * f / FRAMES;
        std::vector<std::thread> pool;
        for (unsigned t = 0; t < nt; t++)
            pool.emplace_back([&, t] {
                for (int y = (int)t; y < rh; y += (int)nt)
                    for (int x = 0; x < rw; x++) {
                        float u = (x + 0.5f - rw / 2.f) / (rh / 2.f), v = (y + 0.5f - rh / 2.f) / (rh / 2.f);
                        img[(size_t)y * rw + x] = scene == 1 ? Nebula(u, v, th) : scene == 2 ? Tidal(u * 1.25f, v * 1.25f, th) : Sacred(u, v, th, 2.f / rh);
                    }
            });
        for (auto& p : pool) p.join();
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                V3 c;
                if (div == 1) c = img[(size_t)y * W + x];
                else {  // bilinear upscale
                    float sx = std::clamp((x + 0.5f) / div - 0.5f, 0.f, rw - 1.001f), sy = std::clamp((y + 0.5f) / div - 0.5f, 0.f, rh - 1.001f);
                    int x0 = (int)sx, y0 = (int)sy;
                    float ax = sx - x0, ay = sy - y0;
                    V3 a = img[(size_t)y0 * rw + x0], b = img[(size_t)y0 * rw + x0 + 1], c0 = img[(size_t)(y0 + 1) * rw + x0], d = img[(size_t)(y0 + 1) * rw + x0 + 1];
                    c = Mix(Mix(a, b, ax), Mix(c0, d, ax), ay);
                }
                float dither = (bayer[y & 3][x & 3] + 0.5f) / 16.f - 0.5f;  // fixed pattern: no banding, no flicker
                uint8_t* o = &out[((size_t)y * W + x) * 3];
                o[0] = (uint8_t)std::clamp(powf(Clamp01(c.r), 0.72f) * 255.f + dither, 0.f, 255.f);  // tone curve: lifts the darks a little
                o[1] = (uint8_t)std::clamp(powf(Clamp01(c.g), 0.72f) * 255.f + dither, 0.f, 255.f);
                o[2] = (uint8_t)std::clamp(powf(Clamp01(c.b), 0.72f) * 255.f + dither, 0.f, 255.f);
            }
        fwrite(out.data(), 1, out.size(), stdout);
        if (f % 60 == 0) fprintf(stderr, "frame %d/%d\n", f, FRAMES);
    }
    return 0;
}
