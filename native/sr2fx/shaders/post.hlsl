// Post effects for sr2fx (Shader Model 3.0, compiled offline by build.bat).
//
// Conventions shared with post.cpp:
//   depth: INTZ texture of the main scene, stored d = P33 + P43 / z for view
//          depth z (left-handed projection, row vectors). Cleared to 1.0 where
//          only the sky was drawn.
//   uv:    0..1, y down. View space: x right, y up, z forward.
//   velocities: pixels per frame (already scaled by exposure), x right, y down.

#define PI 3.14159265
#define HALF_PI 1.5707963

// ------------------------------------------------------------ full-screen VS

float4 vOffset : register(c0);   // (-1/W, 1/H) of the render target: D3D9 half-pixel fix

struct QuadOut { float4 pos : POSITION; float2 uv : TEXCOORD0; };

QuadOut VS_Quad(float4 pos : POSITION, float2 uv : TEXCOORD0)
{
    QuadOut o;
    o.pos = pos;
    o.pos.xy += vOffset.xy * pos.w;
    o.uv = uv;
    return o;
}

// ------------------------------------------------------------ helpers

sampler2D s0 : register(s0);
sampler2D s1 : register(s1);
sampler2D s2 : register(s2);
sampler2D s3 : register(s3);

float4 cProj : register(c0);     // P11, P22, P33, P43 of the main scene projection
float4 cRes  : register(c1);     // full resolution W, H, 1/W, 1/H

float4 T(sampler2D s, float2 uv) { return tex2Dlod(s, float4(uv, 0, 0)); }

bool IsSky(float d) { return d > 0.999999; }

float LinZ(float d) { return cProj.w / (d - cProj.z); }

float3 ViewPos(float2 uv, float z)
{
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    return float3(ndc.x * z / cProj.x, ndc.y * z / cProj.y, z);
}

float SceneZ(sampler2D sd, float2 uv)
{
    float d = T(sd, uv).r;
    return IsSky(d) ? 1e6 : LinZ(d);
}

float IGN(float2 p) { return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715)))); }

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float FastACos(float x)
{
    float a = abs(x);
    float r = (-0.156583 * a + HALF_PI) * sqrt(1 - a);
    return x >= 0 ? r : PI - r;
}

// ------------------------------------------------------------ copy

float4 PS_Copy(float2 uv : TEXCOORD0) : COLOR { return T(s0, uv); }

// ------------------------------------------------------------ ambient occlusion
// Horizon-based (GTAO, Jimenez et al. 2016, after Intel's XeGTAO), computed at
// the AO target's resolution from a linear depth copy at that resolution.
// Output: r = AO already faded with distance and fog (1 = open), g = view depth
// for the bilateral passes.
//
// Kept clean on purpose: a 4x4 interleaved pattern drives the slice angles and
// step offsets, and the blur averages exactly one period of it on flat
// surfaces, so no grain survives. A thickness heuristic stops thin geometry in
// front (a spoiler, a lamp post) from darkening what lies far behind it, and
// the radius is clamped in screen space so close-ups do not turn into a dark
// smudge.

float4 cAO  : register(c2);   // radius (world units), strength (0..1), max radius (target px), thickness (world units)
float4 cAO2 : register(c3);   // unused, fog start, fog end, fog on (0/1)
float4 cTgt : register(c4);   // target W, H, 1/W, 1/H
float4 cAO3 : register(c6);   // fade start, fade end (view depth, world units), half resolution (0/1), 0

// INTZ (s0) -> linear view depth (R32F), sky = 1e6. At half resolution each
// texel keeps one of its four depths: the nearest on one checkerboard colour
// and the farthest on the other, so both sides of a thin edge stay represented.
float4 PS_LinZ(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    if (cAO3.z < 0.5) return float4(SceneZ(s0, uv), 0, 0, 0);
    float2 h = cRes.zw * 0.5;
    float a = SceneZ(s0, uv + float2(-h.x, -h.y)), b = SceneZ(s0, uv + float2(h.x, -h.y));
    float c = SceneZ(s0, uv + float2(-h.x, h.y)), d = SceneZ(s0, uv + float2(h.x, h.y));
    float2 p = floor(vpos);
    bool even = frac((p.x + p.y) * 0.5) < 0.25;
    float z = even ? min(min(a, b), min(c, d)) : max(max(a, b), max(c, d));
    // a far sample next to the sky would read as sky: keep the nearest real surface
    if (z > 1e5) z = min(min(a, b), min(c, d));
    return float4(z, 0, 0, 0);
}

float LZ(float2 uv) { return T(s0, uv).r; }

// 2x2 and 4x4 ordered patterns, 0..3 and 0..15.
float Bayer2(float2 b) { return 2 * abs(b.x - b.y) + b.y; }
float2 Noise4x4(float2 vpos)
{
    float2 p = floor(vpos);
    float2 b0 = frac(p * 0.5) * 2;                  // bit 0 of x, y
    float2 b1 = frac(floor(p * 0.5) * 0.5) * 2;     // bit 1
    float m0 = 4 * Bayer2(b0) + Bayer2(b1);
    float m1 = 4 * Bayer2(b1) + Bayer2(1 - b0);
    return (float2(m0, m1) + 0.5) / 16;
}

#ifndef AO_SLICES
#define AO_SLICES 2
#endif
#ifndef AO_STEPS
#define AO_STEPS 4
#endif

float4 PS_AO(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    float z = LZ(uv);
    if (z > 1e5) return float4(1, 1e6, 0, 0);
    float fade = saturate((cAO3.y - z) / max(cAO3.y - cAO3.x, 1));
    if (cAO2.w > 0) fade *= saturate((cAO2.z - z) / max(cAO2.z - cAO2.y, 1));
    if (fade <= 0.001) return float4(1, z, 0, 0);

    float3 P = ViewPos(uv, z);
    float2 dx = float2(cTgt.z, 0), dy = float2(0, cTgt.w);
    float3 Pr = ViewPos(uv + dx, LZ(uv + dx));
    float3 Pl = ViewPos(uv - dx, LZ(uv - dx));
    float3 Pd = ViewPos(uv + dy, LZ(uv + dy));
    float3 Pu = ViewPos(uv - dy, LZ(uv - dy));
    float3 ex = abs(Pr.z - P.z) < abs(P.z - Pl.z) ? Pr - P : P - Pl;
    float3 ey = abs(Pd.z - P.z) < abs(P.z - Pu.z) ? Pd - P : P - Pu;
    float3 N = normalize(cross(ex, ey));
    if (dot(N, P) > 0) N = -N;
    float3 V = normalize(-P);

    float radius = cAO.x;
    float rpxFull = radius * cProj.y * 0.5 * cTgt.y / z;
    float rpx = min(rpxFull, cAO.z);
    // Far away the radius covers under two texels: nothing to find, fade out.
    fade *= saturate((rpx - 1.5) / 2.0);
    if (fade <= 0.001) return float4(1, z, 0, 0);
    // A clamped radius searches a smaller world distance too.
    radius *= rpx / rpxFull;
    const float steps = AO_STEPS;
    float stepPx = rpx / steps;
    float invR2 = 1.0 / (radius * radius);
    float thick = min(cAO.w, radius);
    float2 nz = Noise4x4(vpos);
    float tmin = 1.0 / max(stepPx, 1e-3);

    float vis = 0;
    const float slices = AO_SLICES;
    [unroll] for (int s = 0; s < AO_SLICES; s++) {
        float phi = (s + nz.x) * PI / slices;
        float2 om = float2(cos(phi), sin(phi));
        float3 dir = float3(om, 0);
        float3 ortho = dir - dot(dir, V) * V;
        float3 axis = normalize(cross(ortho, V));
        float3 pn = N - axis * dot(N, axis);
        float pnl = max(length(pn), 1e-4);
        float cosn = saturate(dot(pn, V) / pnl);
        float n = (dot(ortho, pn) >= 0 ? 1 : -1) * FastACos(cosn);
        float low0 = cos(n + HALF_PI), low1 = cos(n - HALF_PI);
        float hc0 = low0, hc1 = low1;
        float2 suv = om * float2(1, -1) * cTgt.zw * stepPx;
        [unroll] for (int i = 0; i < AO_STEPS; i++) {
            float t = i + frac(nz.y + i * 0.618034) + 0.3;
            // at least one texel out, so a sample never lands on the centre
            float2 o = suv * max(t, tmin);
            float3 S0 = ViewPos(uv + o, LZ(uv + o)) - P;
            float3 S1 = ViewPos(uv - o, LZ(uv - o)) - P;
            float l0 = dot(S0, S0), l1 = dot(S1, S1);
            float c0 = dot(S0, V) * rsqrt(l0 + 1e-6);
            float c1 = dot(S1, V) * rsqrt(l1 + 1e-6);
            // distance falloff, and thin occluders: a sample far in front of
            // this point (toward the camera) is likely a thin object with open
            // space behind it, not a wall
            float w0 = saturate(1 - l0 * invR2) * saturate(2 + S0.z / thick);
            float w1 = saturate(1 - l1 * invR2) * saturate(2 + S1.z / thick);
            hc0 = max(hc0, lerp(low0, c0, w0));
            hc1 = max(hc1, lerp(low1, c1, w1));
        }
        float h0 = -FastACos(hc1);
        float h1 = FastACos(hc0);
        h0 = n + clamp(h0 - n, -HALF_PI, HALF_PI);
        h1 = n + clamp(h1 - n, -HALF_PI, HALF_PI);
        float sn = sin(n);
        float a0 = (cosn + 2 * h0 * sn - cos(2 * h0 - n)) * 0.25;
        float a1 = (cosn + 2 * h1 * sn - cos(2 * h1 - n)) * 0.25;
        vis += pnl * (a0 + a1);
    }
    vis = saturate(vis / slices);
    float ao = 1 - (1 - vis) * cAO.y;
    return float4(lerp(1, ao, fade), z, 0, 0);
}

// Depth-aware blur of (AO, z) along one axis. cBlur.xy = one texel step. Five
// taps weighted 1/2 1 1 1 1/2 average exactly one period of the 4x4 pattern;
// the depth test follows the surface's own slope, so ground seen at a grazing
// angle still blurs while a step to another surface does not.
float4 cBlur : register(c5);

float4 PS_AOBlur(float2 uv : TEXCOORD0) : COLOR
{
    float2 c = T(s0, uv).rg;
    if (c.y > 1e5) return float4(1, c.y, 0, 0);
    float zp = T(s0, uv + cBlur.xy).g, zm = T(s0, uv - cBlur.xy).g;
    float g = abs(zp - c.y) < abs(c.y - zm) ? zp - c.y : c.y - zm;
    if (abs(g) > c.y * 0.05) g = 0;   // an edge on both sides: no slope to trust
    float tol = 1.0 / max(c.y * 0.015, 0.5);
    float sum = c.x, wsum = 1;
    [unroll] for (int i = -2; i <= 2; i++) {
        if (i == 0) continue;
        float2 s = T(s0, uv + cBlur.xy * i).rg;
        float w = (abs(i) == 2 ? 0.5 : 1.0) * saturate(1 - abs(s.y - (c.y + g * i)) * tol);
        sum += s.x * w;
        wsum += w;
    }
    return float4(sum / wsum, c.y, 0, 0);
}

// ------------------------------------------------------------ sun shafts
// Mask: bright sky pixels near the sun (s0 scene colour, s1 depth), at half
// resolution. Blur: radial blur toward the sun's screen position.

float4 cSun  : register(c2);   // sun uv x, y, aspect W/H, threshold
float4 cSun2 : register(c3);   // step scale, decay, weight, disc radius (uv)

float4 PS_SunMask(float2 uv : TEXCOORD0) : COLOR
{
    float d = T(s1, uv).r;
    if (!IsSky(d)) return 0;
    float3 c = T(s0, uv).rgb;
    float l = Luma(c);
    float m = saturate((l - cSun.w) / max(1 - cSun.w, 0.05));
    float2 dv = (uv - cSun.xy) * float2(cSun.z, 1);
    float disc = saturate(1 - length(dv) / cSun2.w);
    return float4(c * m * disc * disc, 1);
}

float4 PS_SunBlur(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    float2 delta = (uv - cSun.xy) * cSun2.x / 16.0;
    float2 tc = uv - delta * IGN(vpos);
    float illum = 1;
    float3 sum = 0;
    [unroll] for (int i = 0; i < 16; i++) {
        sum += T(s0, tc).rgb * illum;
        illum *= cSun2.y;
        tc -= delta;
    }
    return float4(sum * cSun2.z, 1);
}

// ------------------------------------------------------------ composite
// s0 scene colour, s1 AO (r = AO, g = z; maybe half resolution), s2 shafts
// (half resolution, bilinear), s3 depth.

float4 cComp  : register(c2);   // AO on, shafts on, AO texture W, H
float4 cComp2 : register(c3);   // shaft colour rgb * intensity, 0

// AO at full resolution from the AO texture (s1) against the scene depth (s3):
// bilinear among the low-resolution texels on the same surface, and where none
// is (a thin edge, a spoiler against the sky), the one nearest in depth, so the
// shade of one surface never bleeds onto another.
float UpsampledAO(float2 uv)
{
    float d = T(s3, uv).r;
    if (IsSky(d)) return 1;
    float z = LinZ(d);
    float2 p = uv * cComp.zw - 0.5;
    float2 f = frac(p);
    float2 b = (floor(p) + 0.5) / cComp.zw;
    float2 tx = 1.0 / cComp.zw;
    float4 a00 = T(s1, b), a10 = T(s1, b + float2(tx.x, 0)), a01 = T(s1, b + float2(0, tx.y)), a11 = T(s1, b + tx);
    float4 ao = float4(a00.r, a10.r, a01.r, a11.r);
    float4 dz = abs(float4(a00.g, a10.g, a01.g, a11.g) - z);
    float4 w = float4((1 - f.x) * (1 - f.y), f.x * (1 - f.y), (1 - f.x) * f.y, f.x * f.y);
    w *= saturate(1 - dz / max(z * 0.02, 0.5));
    float sw = dot(w, 1);
    if (sw > 1e-3) return dot(w, ao) / sw;
    float m = min(min(dz.x, dz.y), min(dz.z, dz.w));
    float4 pick = dz <= m;
    float r = dot(pick, ao) / max(dot(pick, 1), 1);
    // nothing near this depth at all: leave it open rather than guess
    return m < z * 0.1 ? r : 1;
}

float4 PS_Composite(float2 uv : TEXCOORD0) : COLOR
{
    float4 c = T(s0, uv);
    if (cComp.x > 0) c.rgb *= UpsampledAO(uv);
    if (cComp.y > 0) {
        float3 s = T(s2, uv).rgb * cComp2.rgb;
        c.rgb = 1 - (1 - c.rgb) * (1 - saturate(s));
    }
    return c;
}

// AO alone, multiplied into the scene by the blend unit (dest * src) after the
// opaque draws and before the see-through ones (smoke, rain, glows, glass),
// so those are not darkened by the shade of what lies behind them.
float4 PS_AOMul(float2 uv : TEXCOORD0) : COLOR
{
    float ao = UpsampledAO(uv);
    return float4(ao, ao, ao, 1);
}

// ------------------------------------------------------------ motion blur
// McGuire, Hennessy, Bukowski, Osman 2012, "A Reconstruction Filter for
// Plausible Motion Blur": velocity buffer, tile max, neighbour max, gather.

float4x4 cReproj : register(c2);   // current NDC (x, y, d, 1) -> previous clip
float4 cMB : register(c6);         // velocity scale, max radius K (px), tile size, sample count

float2 ClampVel(float2 v)
{
    float l = length(v);
    return l > cMB.y ? v * (cMB.y / l) : v;
}

float4 PS_CamVel(float2 uv : TEXCOORD0) : COLOR
{
    float d = T(s0, uv).r;
    float2 ndc = float2(uv.x * 2 - 1, 1 - uv.y * 2);
    float4 pc = mul(float4(ndc, d, 1), cReproj);
    if (pc.w <= 1e-4) return 0;
    float2 v = (ndc - pc.xy / pc.w) * 0.5 * float2(cRes.x, -cRes.y) * cMB.x;
    return float4(ClampVel(v), 0, 0);
}

// Per-object velocity: redraws a moving object's triangles.
float4x4 vWVP    : register(c4);   // world * view * projection, this frame
float4x4 vWVPprev : register(c8);  // where the object was last frame, last frame's camera

struct ObjOut { float4 pos : POSITION; float4 c0 : TEXCOORD0; float4 c1 : TEXCOORD1; };

ObjOut VS_ObjVel(float3 p : POSITION)
{
    ObjOut o;
    float4 q = float4(p, 1);
    o.pos = mul(q, vWVP);
    o.c0 = o.pos;
    o.c1 = mul(q, vWVPprev);
    return o;
}

float4 PS_ObjVel(ObjOut i) : COLOR
{
    float2 n = i.c0.xy / i.c0.w;
    float2 prev = i.c1.w > 1e-4 ? i.c1.xy / i.c1.w : n;
    float2 v = (n - prev) * 0.5 * float2(cRes.x, -cRes.y) * cMB.x;
    return float4(ClampVel(v), 0, 0);
}

// Tile max, separable. cTile.xy = source texel step along the axis, z = count;
// cTile2.xy maps the output uv to the source uv (tiles cover K * tiles >= size).
float4 cTile  : register(c8);
float4 cTile2 : register(c9);

float4 PS_TileMax(float2 uv : TEXCOORD0) : COLOR
{
    // uv is the centre of the output texel; walk the K source texels it covers.
    float2 start = uv * cTile2.xy - cTile.xy * (cTile.z * 0.5 - 0.5);
    float2 m = 0;
    float ml = 0;
    [loop] for (int i = 0; i < 64; i++) {
        if (i >= cTile.z) break;
        float2 v = T(s0, start + cTile.xy * i).rg;
        float l = dot(v, v);
        if (l > ml) { ml = l; m = v; }
    }
    return float4(m, 0, 0);
}

float4 PS_NeighborMax(float2 uv : TEXCOORD0) : COLOR
{
    float2 m = 0;
    float ml = 0;
    [unroll] for (int y = -1; y <= 1; y++)
    [unroll] for (int x = -1; x <= 1; x++) {
        float2 v = T(s0, uv + float2(x, y) * cTile.xy).rg;
        float l = dot(v, v);
        if (l > ml) { ml = l; m = v; }
    }
    return float4(m, 0, 0);
}

float Cone(float dist, float len) { return saturate(1 - dist / len); }
float Cylinder(float dist, float len) { return 1 - smoothstep(0.95 * len, 1.05 * len, dist); }
float SoftDepth(float za, float zb, float ext) { return saturate(1 - (za - zb) / ext); }

// s0 colour, s1 velocity, s2 neighbour max (point), s3 depth.
float4 cMB2 : register(c7);   // screen uv -> neighbour max uv scale

float4 PS_Reconstruct(float2 uv : TEXCOORD0, float2 vpos : VPOS) : COLOR
{
    float4 cX = T(s0, uv);
    float2 vn = T(s2, uv * cMB2.xy).rg;
    float lvn = length(vn);
    if (lvn < 0.5) return cX;
    float2 vX = T(s1, uv).rg;
    float lX = max(length(vX), 0.5);
    float zX = SceneZ(s3, uv);
    float ext = max(zX * 0.02, 5);
    float wsum = 1.0 / lX;
    float3 sum = cX.rgb * wsum;
    float j = IGN(vpos) - 0.5;
    float S = cMB.w;
    [loop] for (int i = 0; i < 24; i++) {
        if (i >= S) break;
        float t = lerp(-1.0, 1.0, (i + j + 1.0) / (S + 1.0));
        float2 Y = uv + vn * t * cRes.zw;
        float dist = abs(t) * lvn;
        float2 vY = T(s1, Y).rg;
        float lY = max(length(vY), 0.5);
        float zY = SceneZ(s3, Y);
        // z is a positive distance here (McGuire's paper uses negative view z):
        // f = Y is in front of X, b = Y is behind X.
        float f = SoftDepth(zY, zX, ext);
        float b = SoftDepth(zX, zY, ext);
        float a = f * Cone(dist, lY) + b * Cone(dist, lX) + Cylinder(dist, lY) * Cylinder(dist, lX) * 2;
        wsum += a;
        sum += a * T(s0, Y).rgb;
    }
    return float4(sum / wsum, cX.a);
}

// ------------------------------------------------------------ FXAA
// FXAA 3.11 (Timothy Lottes), PC quality preset 12, written out for SM3.
// s0 must be sampled with bilinear filtering.

float L(float2 uv) { return Luma(T(s0, uv).rgb); }

float4 PS_FXAA(float2 uv : TEXCOORD0) : COLOR
{
    float2 rcp = cRes.zw;
    float4 rgbyM = T(s0, uv);
    float lumaM = Luma(rgbyM.rgb);
    float lumaS = L(uv + float2(0, 1) * rcp);
    float lumaE = L(uv + float2(1, 0) * rcp);
    float lumaN = L(uv + float2(0, -1) * rcp);
    float lumaW = L(uv + float2(-1, 0) * rcp);
    float maxSM = max(lumaS, lumaM), minSM = min(lumaS, lumaM);
    float maxESM = max(lumaE, maxSM), minESM = min(lumaE, minSM);
    float maxWN = max(lumaN, lumaW), minWN = min(lumaN, lumaW);
    float rangeMax = max(maxWN, maxESM), rangeMin = min(minWN, minESM);
    float range = rangeMax - rangeMin;
    if (range < max(0.0625, rangeMax * 0.166)) return rgbyM;

    float lumaNW = L(uv + float2(-1, -1) * rcp);
    float lumaSE = L(uv + float2(1, 1) * rcp);
    float lumaNE = L(uv + float2(1, -1) * rcp);
    float lumaSW = L(uv + float2(-1, 1) * rcp);
    float lumaNS = lumaN + lumaS, lumaWE = lumaW + lumaE;
    float subpixRcpRange = 1.0 / range;
    float subpixNSWE = lumaNS + lumaWE;
    float edgeHorz1 = -2.0 * lumaM + lumaNS;
    float edgeVert1 = -2.0 * lumaM + lumaWE;
    float lumaNESE = lumaNE + lumaSE, lumaNWNE = lumaNW + lumaNE;
    float edgeHorz2 = -2.0 * lumaE + lumaNESE;
    float edgeVert2 = -2.0 * lumaN + lumaNWNE;
    float lumaNWSW = lumaNW + lumaSW, lumaSWSE = lumaSW + lumaSE;
    float edgeHorz4 = abs(edgeHorz1) * 2.0 + abs(edgeHorz2);
    float edgeVert4 = abs(edgeVert1) * 2.0 + abs(edgeVert2);
    float edgeHorz3 = -2.0 * lumaW + lumaNWSW;
    float edgeVert3 = -2.0 * lumaS + lumaSWSE;
    float edgeHorz = abs(edgeHorz3) + edgeHorz4;
    float edgeVert = abs(edgeVert3) + edgeVert4;
    float subpixNWSWNESE = lumaNWSW + lumaNESE;
    float lengthSign = rcp.x;
    bool horzSpan = edgeHorz >= edgeVert;
    float subpixA = subpixNSWE * 2.0 + subpixNWSWNESE;
    if (!horzSpan) { lumaN = lumaW; lumaS = lumaE; }
    if (horzSpan) lengthSign = rcp.y;
    float subpixB = subpixA * (1.0 / 12.0) - lumaM;
    float gradientN = lumaN - lumaM, gradientS = lumaS - lumaM;
    float lumaNN = lumaN + lumaM, lumaSS = lumaS + lumaM;
    bool pairN = abs(gradientN) >= abs(gradientS);
    float gradient = max(abs(gradientN), abs(gradientS));
    if (pairN) lengthSign = -lengthSign;
    float subpixC = saturate(abs(subpixB) * subpixRcpRange);

    float2 posB = uv;
    float2 offNP = horzSpan ? float2(rcp.x, 0) : float2(0, rcp.y);
    if (!horzSpan) posB.x += lengthSign * 0.5;
    if (horzSpan) posB.y += lengthSign * 0.5;
    float2 posN = posB - offNP;
    float2 posP = posB + offNP;
    float subpixD = -2.0 * subpixC + 3.0;
    float lumaEndN = L(posN);
    float subpixE = subpixC * subpixC;
    float lumaEndP = L(posP);
    if (!pairN) lumaNN = lumaSS;
    float gradientScaled = gradient * 0.25;
    float lumaMM = lumaM - lumaNN * 0.5;
    float subpixF = subpixD * subpixE;
    bool lumaMLTZero = lumaMM < 0.0;
    lumaEndN -= lumaNN * 0.5;
    lumaEndP -= lumaNN * 0.5;
    bool doneN = abs(lumaEndN) >= gradientScaled;
    bool doneP = abs(lumaEndP) >= gradientScaled;
    if (!doneN) posN -= offNP * 1.5;
    if (!doneP) posP += offNP * 1.5;
    static const float stepsQ[4] = { 2.0, 2.0, 4.0, 12.0 };
    [unroll] for (int k = 0; k < 4; k++) {
        if (!(doneN && doneP)) {
            if (!doneN) lumaEndN = L(posN) - lumaNN * 0.5;
            if (!doneP) lumaEndP = L(posP) - lumaNN * 0.5;
            doneN = abs(lumaEndN) >= gradientScaled;
            doneP = abs(lumaEndP) >= gradientScaled;
            if (!doneN) posN -= offNP * stepsQ[k];
            if (!doneP) posP += offNP * stepsQ[k];
        }
    }
    float dstN = horzSpan ? uv.x - posN.x : uv.y - posN.y;
    float dstP = horzSpan ? posP.x - uv.x : posP.y - uv.y;
    bool goodSpanN = (lumaEndN < 0.0) != lumaMLTZero;
    bool goodSpanP = (lumaEndP < 0.0) != lumaMLTZero;
    float spanLength = dstP + dstN;
    bool directionN = dstN < dstP;
    float dst = min(dstN, dstP);
    bool goodSpan = directionN ? goodSpanN : goodSpanP;
    float subpixG = subpixF * subpixF;
    float pixelOffset = dst * (-1.0 / spanLength) + 0.5;
    float subpixH = subpixG * 0.75;
    float pixelOffsetGood = goodSpan ? pixelOffset : 0.0;
    float pixelOffsetSubpix = max(pixelOffsetGood, subpixH);
    float2 p = uv;
    if (!horzSpan) p.x += pixelOffsetSubpix * lengthSign;
    if (horzSpan) p.y += pixelOffsetSubpix * lengthSign;
    return float4(T(s0, p).rgb, rgbyM.a);
}

// ------------------------------------------------------------ dev views
// cDbg.x: 1 AO buffer, 2 velocity, 3 shafts. s0 = the buffer.
float4 cDbg : register(c2);

float4 PS_Debug(float2 uv : TEXCOORD0) : COLOR
{
    float4 v = T(s0, uv);
    if (cDbg.x == 1) return float4(v.rrr, 1);
    if (cDbg.x == 2) return float4(saturate(0.5 + v.rg / (2 * cDbg.y)), 0.5, 1);
    return float4(v.rgb, 1);
}
