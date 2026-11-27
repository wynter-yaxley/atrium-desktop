// Atrium glass material. One signed-distance shape; Liquid and Frosted are
// parameter presets (docs/DESIGN.md, material presets).

cbuffer GlassParams : register(b0)
{
    float2 Size;        // window size in px
    float2 RoiOrigin;   // window origin inside the background ROI texture
    float2 RoiSize;     // ROI texture size
    float Radius;      // corner radius in px
    float Edge;        // width of the refracting rim in px
    float Refract;     // max sample displacement in px
    float Dispersion;  // relative channel spread of the displacement
    float BlurLod;     // mip level used for the blurred backdrop
    float TintMix;
    float4 Tint;
    float Highlight;
    float Shadow;
    float2 LightDir;    // unit vector pointing towards the light
    float Inset;       // shadow room around the shape (sets the shadow's softness)
    float BlurMix;     // 0 = sharp backdrop, 1 = fully blurred
    float Fade;        // 0 = the glass as is, 1 = gone (a piece of glass fading out)
    float Rotate;      // radians, clockwise about the shape's centre (a tilted capsule)
    float4 Shape;       // the glass shape in window px: left, top, right, bottom
    float4 Ripple;      // press ripple: centre x, y (window px), ring radius, strength (0 = none)
    float4 PartRect[10]; // melting parts (PartCount > 0): rectangles in window px, left, top, right, bottom
    float4 PartRadii[10]; // their corner radii: top left, top right, bottom right, bottom left
    float PartCount;
    float Melt;        // width of the smooth union in px
    float PartMix;     // 0: the parts, 1: the rounded rectangle Shape (the parts' union has seams inside where they meet)
    float Pad1;
    float4 Tail;        // Dock label pointer: tip x, base half width, height below Shape.w, overlap inside the body
    float4 Wave;        // Dock hover: centre x, support half width, upward height, unused (px)
    float4 FromArea;    // EdgeFrame::FromAreaMatrix: _11, _12, _21, _22
    float2 FromAreaOffset; // _31, _32
    float EdgeSoftness; // the rim's transition width in px
    float EdgeLightBoost; // extra width over light content, where the shadow marks the rim
};

Texture2D Bg : register(t0);
SamplerState Linear : register(s0);

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut VS(uint id : SV_VertexID)
{
    VSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}

// Shared with corner_profile.h / CornerProfile.cs. All rounded rectangles,
// including pills, retain quartic straight-edge joins (k = dk/ds = 0).
float2 cornerProfile(float2 b, float r)
{
    r = clamp(r, 0, min(b.x, b.y));
    float reach = min(r * 1.6, min(b.x, b.y));
    float v = 1 - 0.2928932188134525 * r / max(reach, 1e-12);
    float v2 = v * v;
    float a = b.x == b.y && r == b.x ? 0 : max(0.02, (2 * v2 * v2 - v2) / max(1 - v2, 1e-12));
    return float2(reach, a);
}

float cornerEdge(float2 q, float2 profile)
{
    float r = profile.x, a = profile.y;
    if (r <= 0 || min(q.x, q.y) <= 0)
        return max(q.x, q.y) - r;
    float2 v = q / r, v2 = v * v;
    float2 den = a + (1 - a) * v2;
    float2 derivative = 2 * v * v2 * (2 * a + (1 - a) * v2) / (den * den);
    return (dot(v2 * v2 / den, 1) - 1) * r / max(length(derivative), 1e-12);
}

// Approximate metric distance only for refraction/shading. Coverage below is
// evaluated directly from the analytic implicit equation, not this iteration.
float sdCorner(float2 q, float r, float a)
{
    if (min(q.x, q.y) <= 0)
        return max(q.x, q.y) - r;
    if (r <= 0)
        return length(q);
    float n = length(q);
    float2 v, derivative;
    [unroll] for (int i = 0; i < 2; ++i)
    {
        v = q / n;
        float2 v2 = v * v;
        float2 denominator = a + (1 - a) * v2;
        float2 f = v2 * v2 / denominator;
        derivative = 2 * v * v2 * (2 * a + (1 - a) * v2) / (denominator * denominator);
        n *= 1 + (f.x + f.y - 1) / dot(v, derivative);
    }
    return (n - r) * dot(v, derivative) / length(derivative);
}

// A pill's end is a half circle: the exact rounded-rectangle distance has a unit
// gradient everywhere there, so its coverage anti-aliases cleanly. The implicit G3
// field loses that precision where a corner meets a straight side (a pill has none
// between its ends), which showed as a hard, ragged rim.
float sdRoundRectExact(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return length(max(q, 0)) + min(max(q.x, q.y), 0) - r;
}

float sdRoundRect(float2 p, float2 b, float r, bool edge)
{
    r = clamp(r, 0, min(b.x, b.y));
    if (r >= min(b.x, b.y) - 0.01)
        return sdRoundRectExact(p, b, r);
    float2 profile = cornerProfile(b, r);
    float2 q = abs(p) - b + profile.x;
    return edge ? cornerEdge(q, profile) : sdCorner(q, profile.x, profile.y);
}

// A rounded rectangle whose corners differ (r: top left, top right, bottom right, bottom left; y down).
float sdRoundRect4(float2 p, float2 b, float4 r, bool edge)
{
    float rr = p.x < 0 ? (p.y < 0 ? r.x : r.w) : (p.y < 0 ? r.y : r.z);
    rr = min(rr, min(b.x, b.y));
    float2 profile = cornerProfile(b, rr);
    float2 q = abs(p) - b + profile.x;
    return edge ? cornerEdge(q, profile) : sdCorner(q, profile.x, profile.y);
}

// Smooth minimum: two shapes melt where they come within k px of each other.
float smin(float a, float b, float k)
{
    if (a - b <= -k)
        return a;
    if (a - b >= k)
        return b;
    float x = (a - b) / k, x2 = x * x;
    // C3 at both ends, with the original blend width and k/4 centre depth.
    float p = (8 + x2 * (3 + x2 * (13 + x2 * (-11 + 3 * x2)))) / 32;
    return (a + b) * 0.5 - k * p;
}

// Signed distance to the glass shape at window px `px` (negative inside).
float sdTail(float2 px)
{
    float width = Shape.z - Shape.x;
    float margin = max(0.5, Tail.w * 0.25);
    float halfWidth = min(Tail.y, max(0.5, width * 0.5 - margin));
    float centre = clamp(Tail.x, Shape.x + halfWidth + margin, max(Shape.x + halfWidth + margin, Shape.z - halfWidth - margin));
    float2 a = float2(centre - halfWidth, Shape.w - Tail.w);
    float2 b = float2(centre + halfWidth, a.y), c = float2(Tail.x, Shape.w + Tail.z);
    float2 e0 = b - a, e1 = c - b, e2 = a - c;
    float2 v0 = px - a, v1 = px - b, v2 = px - c;
    float2 q0 = v0 - e0 * saturate(dot(v0, e0) / max(dot(e0, e0), 1e-12));
    float2 q1 = v1 - e1 * saturate(dot(v1, e1) / max(dot(e1, e1), 1e-12));
    float2 q2 = v2 - e2 * saturate(dot(v2, e2) / max(dot(e2, e2), 1e-12));
    float distance = sqrt(min(dot(q0, q0), min(dot(q1, q1), dot(q2, q2))));
    bool inside = e0.x * v0.y - e0.y * v0.x >= 0 && e1.x * v1.y - e1.y * v1.x >= 0 && e2.x * v2.y - e2.y * v2.x >= 0;
    return inside ? -distance : distance;
}

float2 fromArea(float2 px) { return px.x * FromArea.xy + px.y * FromArea.zw + FromAreaOffset; }

float2 toArea(float2 p)
{
    p -= FromAreaOffset;
    return float2(p.x * FromArea.x + p.y * FromArea.y, p.x * FromArea.z + p.y * FromArea.w);
}

float ShapeDistance(float2 px, bool edge, bool basic)
{
    px = fromArea(px);
    if (Rotate != 0) {
        float2 o = (Shape.xy + Shape.zw) * 0.5, q = px - o;
        float c = cos(Rotate), s = sin(Rotate);
        px = o + float2(q.x * c + q.y * s, q.y * c - q.x * s);
    }
    // A C3 bump stretches the glass upwards while keeping its bottom fixed.
    if (!basic && Wave.z > 0) {
        float x = (px.x - Wave.x) / max(Wave.y, 1);
        float w = saturate(1 - x * x);
        float lift = Wave.z * w * w * w * w;
        float height = max(Shape.w - Shape.y, 1);
        px.y = Shape.w - (Shape.w - px.y) * height / (height + lift);
    }
    // C3 metric fields are used while shapes merge. The isolated outline
    // uses the exact implicit equation below; a blend needs a smooth field.
    edge = edge && (basic || PartCount < 0.5);
    float2 c = px - (Shape.xy + Shape.zw) * 0.5;
    float2 b = max((Shape.zw - Shape.xy) * 0.5, 1);
    float r = min(Radius, min(b.x, b.y));
    float d = sdRoundRect(c, b, r, edge);
    if (!basic && PartCount > 0.5) {
        float card = d;
        d = 1e6;
        [loop] for (int i = 0; i < 10; ++i)
        {
            if (float(i) < PartCount) {
                float4 pr = PartRect[i];
                float di = sdRoundRect4(px - (pr.xy + pr.zw) * 0.5, max((pr.zw - pr.xy) * 0.5, 1), PartRadii[i], edge);
                d = Melt > 0.01 ? smin(d, di, Melt) : min(d, di);
            }
        }
        d = lerp(d, card, PartMix);
    }
    if (!basic && Tail.z > 0)
        d = min(d, sdTail(px));
    return d;
}

float Dist(float2 px, bool basic) { return ShapeDistance(px, false, basic); }

// Coverage at the rim: away from it one sample decides; along it the pixel is resolved on a 4x4
// grid, so a tilted capsule's edge steps by a sixteenth of a pixel instead of a whole one. Both
// share the same transition width, so the rim reads soft all the way round.
float EdgeCoverage(float2 px, bool basic, float edgeDistance, float aa)
{
    if (abs(edgeDistance) > aa * 0.75)
        return saturate(0.5 - edgeDistance / aa);
    const int n = 4;
    float sum = 0;
    [unroll] for (int j = 0; j < n; ++j)
    {
        [unroll] for (int k = 0; k < n; ++k)
        {
            const float2 sub = px + (float2(float(k), float(j)) + 0.5) / float(n) - 0.5;
            sum += saturate(0.5 - ShapeDistance(sub, true, basic) / aa);
        }
    }
    return sum * (1.0 / float(n * n));
}

float3 bg(float2 px, float lod) { return Bg.SampleLevel(Linear, (px + RoiOrigin) / RoiSize, lod).rgb; }

float3 blurred(float2 px)
{
    // A few taps at a coarse mip hide the mip's box filter structure.
    float s = exp2(BlurLod) * 0.75;
    float3 c = bg(px, BlurLod) * 0.4;
    c += bg(px + float2(s, 0), BlurLod) * 0.15;
    c += bg(px - float2(s, 0), BlurLod) * 0.15;
    c += bg(px + float2(0, s), BlurLod) * 0.15;
    c += bg(px - float2(0, s), BlurLod) * 0.15;
    return c;
}

float3 sharpBackdrop(float2 px, float2 off)
{
    float3 sharp;
    sharp.r = bg(px + off * (1 + Dispersion), 0).r;
    sharp.g = bg(px + off, 0).g;
    sharp.b = bg(px + off * (1 - Dispersion), 0).b;
    return sharp;
}

float4 PSImpl(VSOut i, bool basic)
{
    float2 px = i.uv * Size;
    float d = Dist(px, basic);

    // Soft drop shadow outside the shape, fading smoothly to zero before the window inset boundary.
    float shadowA = 0;
    if (d >= 0) {
        float ds = Dist(px - float2(0, Inset * 0.16), basic);
        float edgeFade = 1 - smoothstep(Inset * 0.50, max(Inset - 0.5, 1.0), d);
        float dist = (max(ds, 0) * 0.75 + d * 0.25) / max(Inset * 0.34, 1.0);
        shadowA = Shadow * exp(-dist * (0.85 + 0.30 * dist)) * edgeFade;
    }

    // The implicit edge distance is exact only near the outline: where a corner
    // meets a straight side, deep inside, it jumps by hundreds of px between
    // neighbours, and fwidth would leave a half-covered line there. Past 2 px in
    // the shape is plainly covered.
    float edgeDistance = ShapeDistance(px, true, basic);
    // Over light content the glass reflects almost white, so only the shadow marks the rim; there the
    // transition grows by EdgeLightBoost. A strong shadow raises the edge's contrast just as much, so
    // it widens the transition too (Shadow). All are settings, tuned from the material page.
    float backdropLuma = dot(bg(px, BlurLod), float3(0.2126, 0.7152, 0.0722));
    // The slider follows a squared curve through its 200% default: that point stays as it was, while
    // the ends move apart (0.32 px at 0%, 4.5 px at 300%), so the whole travel is felt. The shadow's
    // share is halved so it cannot dilute the slider.
    float softness = EdgeSoftness * EdgeSoftness * 0.5;
    float aa = clamp(fwidth(edgeDistance) * (softness + EdgeLightBoost * smoothstep(0.35, 0.55, backdropLuma) + Shadow * 0.4), 0.3, 6.0);
    float cov = d < -2 ? 1 : EdgeCoverage(px, basic, edgeDistance, aa);
    float keep = saturate(1 - Fade);
    shadowA *= keep;
    if (cov <= 0)
        return float4(0, 0, 0, shadowA);

    // A wider step steadies the normal along a tilted rim: one pixel jumps between neighbours there,
    // and the rim's highlight would follow the jump as specks.
    float e = 1.5;
    float2 g = float2(Dist(px + float2(e, 0), basic) - Dist(px - float2(e, 0), basic), Dist(px + float2(0, e), basic) - Dist(px - float2(0, e), basic));
    float gl = length(g);
    float2 n = gl > 1e-5 ? g / gl : float2(0, 0);

    float depth = max(-d, 0);
    float t = saturate(1 - depth / max(Edge, 1));
    float profile = 1 - sqrt(saturate(1 - t * t)); // circular lens cross-section
    float2 off = -n * Refract * profile;

    // Press ripple: a ring of displacement running outwards from the point that
    // was pressed, as if the glass gave way under the finger and sprang back.
    float ring = 0;
    if (Ripple.w > 0) {
        float2 dv = px - toArea(Ripple.xy);
        float dl = length(dv);
        float w = max(Ripple.z * 0.30, 6);
        float x = (dl - Ripple.z) / w;
        ring = exp(-x * x) * Ripple.w;
        float2 dir = dl > 1e-3 ? dv / dl : float2(0, 0);
        off += dir * ring * (6 + Refract * 0.35);
        off -= dv * 0.06 * Ripple.w * saturate(1 - dl / max(Ripple.z, 1)); // the dent inside the ring
    }

    float3 col;
    if (BlurMix <= 0)
        col = sharpBackdrop(px, off);
    else if (BlurMix >= 1)
        col = blurred(px + off);
    else
        col = lerp(sharpBackdrop(px, off), blurred(px + off), BlurMix);

    // Vibrancy: content under the glass reads a little richer and brighter,
    // as on macOS, before the tint is mixed in.
    float luma = dot(col, float3(0.2126, 0.7152, 0.0722));
    col = lerp(luma.xxx, col, 1.0 + 0.22 * Highlight) * (1.0 + 0.04 * Highlight);

    col = lerp(col, Tint.rgb, TintMix);

    // Light: a crisp specular line along the rim, strongest facing the light,
    // a softer glow just inside it, and a faint sheen from the top.
    float lit = saturate(dot(n, LightDir));
    float back = saturate(dot(n, -LightDir));
    // Wider than a pixel and a half, so a tilted rim's highlight spans several samples instead of
    // speckling along the coverage steps.
    float rimLine = 1 - smoothstep(0.0, 2.4, depth);
    float glow = saturate(1 - depth / (Edge * 1.6));
    col += Highlight * rimLine * (0.22 + 0.78 * lit + 0.35 * back);
    col += Highlight * glow * glow * lit * 0.16;
    float2 shapeA = toArea(Shape.xy), shapeB = toArea(Shape.zw);
    float shapeTop = min(shapeA.y, shapeB.y), shapeBottom = max(shapeA.y, shapeB.y);
    float sheen = saturate(1 - (px.y - shapeTop) / max(shapeBottom - shapeTop, 1));
    col += Highlight * 0.045 * sheen * sheen;

    // Depth: subtle bevel shade on dark/translucent glass, faded out on light or high-tint panels to avoid a muddy rim.
    float band = smoothstep(0.0, Edge * 0.5, depth) * (1 - smoothstep(Edge * 0.5, Edge * 1.3, depth));
    float colLuma = dot(saturate(col), float3(0.2126, 0.7152, 0.0722));
    float shadeWeight = 0.025 * (1 - smoothstep(0.50, 0.88, colLuma)) * (1 - 0.75 * TintMix);
    col *= 1 - shadeWeight * band * (1 - lit);
    col += ring * 0.10 * (0.4 + Highlight);

    float4 glass = float4(saturate(col) * cov, cov) * keep;
    return glass + (1 - cov) * float4(0, 0, 0, shadowA);
}

float4 PS(VSOut i) : SV_Target { return PSImpl(i, false); }
float4 PSBasic(VSOut i) : SV_Target
{
    // Keep the original arithmetic near the rim. The specialized distance
    // path can round a sampled channel by one 8-bit level at the boundary.
    float2 px = fromArea(i.uv * Size);
    if (Rotate != 0)
        return PSImpl(i, false);
    float boundary = min(min(px.x - Shape.x, Shape.z - px.x), min(px.y - Shape.y, Shape.w - px.y));
    if (boundary <= max(Edge, 2))
        return PSImpl(i, false);
    return PSImpl(i, true);
}
