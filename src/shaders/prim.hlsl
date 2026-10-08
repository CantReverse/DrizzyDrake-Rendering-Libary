// drizzy_renderer primitive shader, shared by the D3D11 and D3D12 backends.
//
// Every GpuPrim record (include/drizzy/draw_list.h) becomes one quad. The index buffer holds recordIndex * 4 + corner,
// so the vertex shader pulls its record from a structured buffer by SV_VertexID and no vertex buffer or input layout
// is needed. Extended prims read a second, continuation record; continuation records themselves turn into degenerate
// quads that are never rasterized. Shapes are evaluated in the pixel shader as signed distances measured in pixels,
// which gives exact one-pixel anti-aliasing at any size.
//
// All colors leave the vertex shader premultiplied, so interpolation and blending (ONE, INV_SRC_ALPHA) are correct
// for translucent gradients and for UI composited into render textures.

#define PRIM_RECT    0
#define PRIM_ELLIPSE 1
#define PRIM_LINE    2
#define PRIM_QUAD    3
#define PRIM_SHADOW  4
#define PRIM_GLYPH   5
#define PRIM_STROKE  6
#define PRIM_POLY    7

#define FLAG_TEXTURED     (1u << 3)
#define FLAG_TEX_ALPHA    (1u << 4)
#define FLAG_GRAD_H       (1u << 5)
#define FLAG_GRAD_V       (1u << 6)
#define FLAG_CAP_ROUND    (1u << 7)
#define FLAG_CAP_SQUARE   (1u << 8)
#define FLAG_EXTENDED     (1u << 9)
#define FLAG_CONTINUATION (1u << 10)
#define FLAG_OUTLINE      (1u << 11)
#define FLAG_CUTOUT       (1u << 12)
#define FLAG_FADE_START   (1u << 13)  // Stroke: anti-aliased across p0
#define FLAG_FADE_END     (1u << 14)  // Stroke: anti-aliased across p1
#define POLY_MODE_MASK    (3u << 13)  // Polygon: the record's role
#define POLY_TRIANGLE     (0u << 13)  //   a lone triangle
#define POLY_EDGE         (1u << 13)  //   the anti-aliasing band along one edge
#define POLY_INTERIOR     (2u << 13)  //   two triangles of the inside
#define FLAG_POLY_CCW     (1u << 15)  // Polygon: winds counter-clockwise on screen
// Set by the vertex shader for the pixel shader, in the bits that hold the fp16 parameter in a record (the vertex
// shader passes the pixel shader only the low 16 bits of the record's flags).
#define PS_INTERIOR       (1u << 16)  // a big untextured rect: uv holds the position scaled to its interior, [-1, 1]
#define PARAM_SHIFT       16

#define GLOBAL_LINEAR (1u << 0)  // write linear color (sRGB views, scRGB, HDR10), scaled by g_colorScale
#define GLOBAL_PQ     (1u << 1)  // HDR10: encode Rec.2020 PQ on output

// Pixels added around SDF shapes so the anti-aliased edge is never cut off by the quad.
#define AA_MARGIN 1.0

struct Prim {
    float4 a;
    uint   color;
    uint2  data;
    uint   flags;
};

cbuffer Globals : register(b0) {
    float2 g_ndcScale;     // (2 / width, -2 / height) * g_pixelScale: list units to clip space
    uint   g_globalFlags;
    float  g_colorScale;   // linear outputs: SDR white in target units (scRGB: nits / 80, HDR10: nits / 10000)
    float  g_pixelScale;   // target pixels per list unit (the UI scale)
    float3 g_pad;
};

StructuredBuffer<Prim> g_prims   : register(t0);
Texture2D              g_texture : register(t1);
SamplerState           g_sampler : register(s0);

struct VSOut {
    float4 pos   : SV_Position;
    float4 color : COLOR0;     // premultiplied fill color
    float2 local : TEXCOORD0;  // offset from the shape's center in pixels, in the shape's own frame
    float2 uv    : TEXCOORD1;  // texture coordinates | cut-out shadow: offset from the caster to the shadow
    nointerpolation float4 border : COLOR1;     // premultiplied border / outline color
    nointerpolation float4 shape  : TEXCOORD2;  // xy: half extents, z: border width / blur / glyph px range,
                                                // w: line alpha scale
    nointerpolation float4 radii  : TEXCOORD3;  // tl, tr, br, bl | glyph: x = outline width, y = softness
    nointerpolation uint   flags  : FLAGS;
};

float3 SrgbToLinear(float3 c) {
    float3 lo = c * (1.0 / 12.92);
    float3 hi = pow(abs((c + 0.055) * (1.0 / 1.055)), 2.4);
    return lerp(hi, lo, step(c, 0.04045));
}

// Colors are authored in sRGB. Targets that take linear color (sRGB views, where the hardware encodes on write, and
// HDR targets) get them converted, and scaled so SDR white lands at the configured paper white.
float4 PrepareColor(uint c) {
    float4 v = float4(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, c >> 24) * (1.0 / 255.0);
    if ((g_globalFlags & GLOBAL_LINEAR) != 0) v.rgb = SrgbToLinear(v.rgb) * g_colorScale;
    v.rgb *= v.a;
    return v;
}

float2 UnpackHalf2(uint v) { return float2(f16tof32(v), f16tof32(v >> 16)); }

static const float2 kCorner[4] = { float2(-1, -1), float2(1, -1), float2(1, 1), float2(-1, 1) };

// Polyline joins: miter length limit in half widths (kMaxMiter in draw_list.cpp).
#define MAX_MITER 4.0

float2 EdgeNormal(float2 a, float2 b) {
    float2 d = b - a;
    float lenSq = dot(d, d);
    d = lenSq > 1e-12 ? d * rsqrt(lenSq) : float2(0.0, 0.0);
    return float2(d.y, -d.x);
}

// The offset at `here` that moves both edges meeting there by one unit: the average of their normals lengthened into
// a miter, capped at MAX_MITER. Where the path doubles back on itself, the outgoing edge's normal.
float2 Miter(float2 before, float2 here, float2 after) {
    float2 n1 = EdgeNormal(here, after);
    float2 m = (EdgeNormal(before, here) + n1) * 0.5;
    float lenSq = dot(m, m);
    return lenSq > 1e-6 ? m * min(1.0 / lenSq, MAX_MITER * MAX_MITER) : n1;
}

VSOut VSMain(uint vertexId : SV_VertexID) {
    uint index = vertexId >> 2;
    uint corner = vertexId & 3;
    Prim p = g_prims[index];

    VSOut o;
    o.pos    = float4(0.0, 0.0, 0.0, 1.0);
    o.color  = float4(0.0, 0.0, 0.0, 0.0);
    o.border = float4(0.0, 0.0, 0.0, 0.0);
    o.local  = float2(0.0, 0.0);
    o.uv     = float2(0.0, 0.0);
    o.shape  = float4(0.0, 0.0, 0.0, 1.0);
    o.radii  = float4(0.0, 0.0, 0.0, 0.0);
    o.flags  = p.flags & 0xFFFF;  // the pixel shader gets the type and flags; the high bits are free for PS_* hints
    // The second record of an extended prim: all four corners at one point, so the quad covers no pixels.
    if ((p.flags & FLAG_CONTINUATION) != 0) return o;

    bool extended = (p.flags & FLAG_EXTENDED) != 0;
    Prim e = p;
    if (extended) e = g_prims[index + 1];
    uint type = p.flags & 7;
    float param = f16tof32(p.flags >> PARAM_SHIFT);
    o.color = PrepareColor(p.color);

    float2 pos;
    if (type == PRIM_GLYPH) {
        // Glyph quads already include the distance field's padding, so they need no anti-aliasing margin.
        float2 t = kCorner[corner] * 0.5 + 0.5;
        pos = lerp(p.a.xy, p.a.zw, t);
        float4 uv = float4(p.data.x & 0xFFFF, p.data.x >> 16, p.data.y & 0xFFFF, p.data.y >> 16) * (1.0 / 65535.0);
        o.uv = lerp(uv.xy, uv.zw, t);
        o.shape.z = param;
        if (extended) {
            o.border = PrepareColor(e.color);
            o.radii.xy = UnpackHalf2(e.data.x);
        }
    } else if (type == PRIM_QUAD) {
        float2 v[4] = { p.a.xy, p.a.zw, e.a.xy, e.a.zw };
        uint c[4] = { p.color, p.data.x, p.data.y, e.color };
        pos = v[corner];
        o.color = PrepareColor(c[corner]);
    } else if (type == PRIM_LINE) {
        float2 delta = p.a.zw - p.a.xy;
        float len = length(delta);
        float2 dir = len > 1e-5 ? delta / len : float2(1.0, 0.0);
        float2 nrm = float2(-dir.y, dir.x);
        // Lines thinner than a target pixel draw one pixel wide with reduced alpha.
        float thickness = param * g_pixelScale;  // target pixels
        float halfThick = max(thickness, 1.0) * 0.5 / g_pixelScale;
        bool hasCap = (p.flags & (FLAG_CAP_ROUND | FLAG_CAP_SQUARE)) != 0;
        bool squareCap = (p.flags & FLAG_CAP_SQUARE) != 0;
        float2 local = kCorner[corner] * (float2(len * 0.5 + (hasCap ? halfThick : 0.0), halfThick) + AA_MARGIN);
        pos = (p.a.xy + p.a.zw) * 0.5 + dir * local.x + nrm * local.y;
        o.local = local;
        // Butt and square caps are boxes (square ones extended by half the thickness); round caps are a capsule
        // around the bare segment.
        o.shape = float4(len * 0.5 + (squareCap ? halfThick : 0.0), halfThick, 0.0, saturate(thickness));
    } else if (type == PRIM_STROKE || type == PRIM_POLY) {
        // Polylines and polygons: every corner is a point moved along the miter there, all through the one Miter call
        // below. Records that share a point (neighbouring segments; a polygon's bands and interior) pass it the same
        // three points, so they build bit-identical corners and meet without gaps or overlaps.
        float2 c = kCorner[corner];
        float2 before, here, after;
        float push;  // along the miter, list units
        float sgn = (p.flags & FLAG_POLY_CCW) != 0 ? -1.0 : 1.0;  // polygons: makes the miter point outward
        uint mode = p.flags & POLY_MODE_MASK;
        if (type == PRIM_STROKE) {
            // Corner x picks the end (-1: p0, +1: p1), y the side: half the stroke plus the anti-aliasing margin. The
            // neighbours are linked records; without one (an open end, or a neighbour skipped as invisible) the
            // polyline is taken to continue straight on.
            float thickness = param * g_pixelScale;  // target pixels
            float halfThick = max(thickness, 1.0) * 0.5 / g_pixelScale;
            bool atEnd = c.x > 0.0;
            here = atEnd ? p.a.zw : p.a.xy;
            before = p.a.xy;
            after = p.a.zw;
            int link = asint(atEnd ? p.data.y : p.data.x);
            if (link != 0) {
                Prim neighbour = g_prims[uint(int(index) + link)];
                if (atEnd) after = neighbour.a.zw;
                else before = neighbour.a.xy;
            } else if (atEnd) {
                after = here + (p.a.zw - p.a.xy);
            } else {
                before = here - (p.a.zw - p.a.xy);
            }
            push = c.y * (halfThick + AA_MARGIN / g_pixelScale);
            o.shape = float4(0.0, halfThick, 0.0, saturate(thickness));
        } else {
            // Polygons are anti-aliased across a band from half a pixel inside each edge to half a pixel outside.
            float h = sgn * 0.5 / g_pixelScale;
            if (mode == POLY_TRIANGLE) {
                // A lone triangle, every vertex pushed out. Corner 3 repeats vertex 2 (an empty second triangle).
                float2 v2 = asfloat(p.data);
                uint k = min(corner, 2u);
                here = k == 0 ? p.a.xy : (k == 1 ? p.a.zw : v2);
                before = k == 0 ? v2 : (k == 1 ? p.a.xy : p.a.zw);
                after = k == 0 ? p.a.zw : (k == 1 ? v2 : p.a.xy);
                push = h;
            } else if (mode == POLY_EDGE) {
                // The band along edge k (point k to k + 1): corners 0, 1 pulled in at k, k + 1; corners 2, 3 pushed
                // out at k + 1, k. The neighbouring edge records hold points k - 1 and k + 2.
                uint k = p.data.x, n = p.data.y;
                bool atEnd = corner == 1 || corner == 2;
                Prim neighbour = g_prims[atEnd ? (k + 1 == n ? index + 1 - n : index + 1)
                                               : (k == 0 ? index + n - 1 : index - 1)];
                here = atEnd ? p.a.zw : p.a.xy;
                before = atEnd ? p.a.xy : neighbour.a.xy;
                after = atEnd ? neighbour.a.zw : p.a.zw;
                push = corner >= 2 ? h : -h;
            } else {
                // Interior: points by index into the edge records (edge j starts at point j), pulled in to where the
                // bands begin.
                uint first = uint(int(index) + asint(p.data.x)), n = p.data.y;
                uint j = uint(p.a[corner]);  // stored as float values
                Prim edge = g_prims[first + j];
                here = edge.a.xy;
                after = edge.a.zw;
                before = g_prims[first + (j == 0 ? n - 1 : j - 1)].a.xy;
                push = -h;
            }
        }
        pos = here + Miter(before, here, after) * push;

        if (type == PRIM_STROKE) {
            float2 delta = p.a.zw - p.a.xy;
            float len = length(delta);
            float2 dir = len > 1e-5 ? delta / len : float2(1.0, 0.0);
            float2 nrm = float2(-dir.y, dir.x);
            // Faded (open) ends also get the anti-aliasing margin along the segment.
            if ((p.flags & (c.x > 0.0 ? FLAG_FADE_END : FLAG_FADE_START)) != 0) {
                pos += dir * (c.x * AA_MARGIN / g_pixelScale);
            }
            // Measured from the actual corner: exact under interpolation however the miters shaped the quad.
            float2 fromMid = pos - (p.a.xy + p.a.zw) * 0.5;
            o.local = float2(dot(fromMid, dir), dot(fromMid, nrm));
            o.shape.x = len * 0.5;
        } else {
            // Signed distances to the anti-aliased edges (outward positive; -1e4 for none), in local.xy and uv.x.
            // Affine in the position, so interpolation keeps them exact.
            float3 d = float3(-1e4, -1e4, -1e4);
            if (mode == POLY_TRIANGLE) {
                float2 v2 = asfloat(p.data);
                d.x = dot(pos - p.a.xy, EdgeNormal(p.a.xy, p.a.zw)) * sgn;
                d.y = dot(pos - p.a.zw, EdgeNormal(p.a.zw, v2)) * sgn;
                d.z = dot(pos - v2, EdgeNormal(v2, p.a.xy)) * sgn;
            } else if (mode == POLY_EDGE) {
                d.x = dot(pos - p.a.xy, EdgeNormal(p.a.xy, p.a.zw)) * sgn;
            }
            o.local = d.xy;
            o.uv = float2(d.z, 0.0);
        }
    } else {
        // Rect, ellipse, shadow.
        float2 halfSize = max((p.a.zw - p.a.xy) * 0.5, 0.0);
        float2 center = (p.a.xy + p.a.zw) * 0.5;
        float2 local;
        if (type == PRIM_SHADOW && extended) {
            // A cut-out shadow names the exact quad to cover (a strip around the caster).
            pos = lerp(e.a.xy, e.a.zw, kCorner[corner] * 0.5 + 0.5);
            local = pos - center;
            o.uv = UnpackHalf2(e.data.x);
        } else {
            float margin = AA_MARGIN + (type == PRIM_SHADOW ? param : 0.0);
            local = kCorner[corner] * (halfSize + margin);
            pos = center + local;
        }
        o.local = local;
        o.shape.xy = halfSize;
        o.shape.w = 0.0;
        if (type != PRIM_ELLIPSE) {
            o.radii = float4(UnpackHalf2(p.data.x), UnpackHalf2(p.data.y));
        }
        if (type == PRIM_SHADOW) {
            o.shape.z = param;  // blur radius
        } else {
            bool outline = (p.flags & FLAG_OUTLINE) != 0;
            o.shape.z = (outline || extended) ? param : 0.0;  // border / outline width
            if (outline) {
                o.border = o.color;
                o.color = float4(0.0, 0.0, 0.0, 0.0);
            } else if (extended) {
                // Position within the rect in [0, 1], extrapolated into the margin so interpolation stays exact.
                float2 t = local / max(halfSize * 2.0, 1e-5) + 0.5;
                o.uv = lerp(e.a.xy, e.a.zw, t);
                o.border = PrepareColor(e.data.x);
                if ((p.flags & (FLAG_GRAD_H | FLAG_GRAD_V)) != 0) {
                    float g = (p.flags & FLAG_GRAD_H) != 0 ? t.x : t.y;
                    o.color = lerp(o.color, PrepareColor(e.color), g);
                }
            }
            if (type == PRIM_RECT) {
                // Big rects get an inset past which the pixel shader skips the distance math (see PSMain).
                float inset = max(max(o.radii.x, o.radii.y), max(o.radii.z, o.radii.w)) + o.shape.z + 1.0;
                o.shape.w = min(halfSize.x, halfSize.y) - inset > 16.0 ? inset : 0.0;
            }
        }
    }

    // The pixel shader measures distances in target pixels (one-pixel anti-aliasing at any UI scale): scale every
    // length it reads from list units.
    float s = g_pixelScale;
    o.local *= s;
    if (type == PRIM_GLYPH) {
        o.shape.z *= s;    // distance range
        o.radii.xy *= s;   // outline width, softness
    } else if (type == PRIM_LINE || type == PRIM_STROKE) {
        o.shape.xy *= s;
    } else if (type == PRIM_POLY) {
        o.uv *= s;  // the third edge distance
    } else if (type != PRIM_QUAD) {
        o.shape.xyz *= s;  // half extents, border width / blur
        o.shape.w *= s;    // interior fast-path inset
        o.radii *= s;
        if (type == PRIM_SHADOW) o.uv *= s;  // cut-out caster offset
        if (type == PRIM_RECT && o.shape.w > 0.0 && (p.flags & FLAG_TEXTURED) == 0) {
            // The interior fast path (PSMain), through uv, which untextured rects leave unused.
            o.uv = o.local / (o.shape.xy - o.shape.w);
            o.flags |= PS_INTERIOR;
        }
    }

    o.pos = float4(pos * g_ndcScale + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Signed distance to a rounded box centered at the origin; radii are tl, tr, br, bl with +y down.
float SdRoundRect(float2 p, float2 halfSize, float4 radii) {
    float r = p.x < 0.0 ? (p.y < 0.0 ? radii.x : radii.w) : (p.y < 0.0 ? radii.y : radii.z);
    r = min(r, min(halfSize.x, halfSize.y));
    float2 q = abs(p) - halfSize + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

float SdEllipse(float2 p, float2 r) {
    if (abs(r.x - r.y) < 1e-3) return length(p) - r.x;
    // Inigo Quilez's approximation: accurate near the boundary, which is all anti-aliasing and borders need.
    float k1 = length(p / (r * r));
    if (k1 < 1e-6) return -min(r.x, r.y);
    float k0 = length(p / r);
    return k0 * (k0 - 1.0) / k1;
}

float Median(float a, float b, float c) {
    return max(min(a, b), min(max(a, b), c));
}

// Shape coverage and color for one pixel, premultiplied. Written with a single exit: fxc cannot follow early returns
// out of a function that is not the entry point.
float4 Shade(VSOut i) {
    uint type = i.flags & 7;
    float4 fill = i.color;
    float4 result = float4(0.0, 0.0, 0.0, 0.0);

    if (type == PRIM_GLYPH) {
        // RGB: multi-channel distance field (median gives sharp corners). A: true distance, used for outlines because
        // it stays correct further from the edge. Both encode 0.5 at the edge and span the px range.
        float4 t = g_texture.Sample(g_sampler, i.uv);
        float pxRange = i.shape.z;
        float edgeWidth = 1.0 + i.radii.y;
        float body = saturate((Median(t.r, t.g, t.b) - 0.5) * pxRange / edgeWidth + 0.5);
        float outlineWidth = i.radii.x;
        if (outlineWidth > 0.0) {
            float outer = saturate(((t.a - 0.5) * pxRange + outlineWidth) / edgeWidth + 0.5);
            result = lerp(i.border, fill, body) * outer;
        } else {
            result = fill * body;
        }
    } else {
        if ((i.flags & FLAG_TEXTURED) != 0) {
            float4 t = g_texture.Sample(g_sampler, i.uv);
            if ((i.flags & FLAG_TEX_ALPHA) != 0) {
                fill *= t.r;
            } else {
                if ((g_globalFlags & GLOBAL_LINEAR) != 0) t.rgb = SrgbToLinear(t.rgb);
                t.rgb *= t.a;
                fill *= t;
            }
        }

        if (type == PRIM_QUAD) {
            result = fill;  // tessellated geometry carries its own anti-aliasing fringe
        } else if (type == PRIM_POLY) {
            result = fill * saturate(0.5 - max(max(i.local.x, i.local.y), i.uv.x));
        } else if (type == PRIM_LINE) {
            float d;
            if ((i.flags & FLAG_CAP_ROUND) != 0) {
                d = length(float2(max(abs(i.local.x) - i.shape.x, 0.0), i.local.y)) - i.shape.y;
            } else {
                float2 q = abs(i.local) - i.shape.xy;
                d = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0);
            }
            result = fill * (saturate(0.5 - d) * i.shape.w);
        } else if (type == PRIM_STROKE) {
            // Across the stroke; along it only at an open polyline's ends (joins continue into the next segment).
            float d = abs(i.local.y) - i.shape.y;
            if ((i.flags & FLAG_FADE_START) != 0) d = max(d, -i.local.x - i.shape.x);
            if ((i.flags & FLAG_FADE_END) != 0) d = max(d, i.local.x - i.shape.x);
            result = fill * (saturate(0.5 - d) * i.shape.w);
        } else if (type == PRIM_SHADOW) {
            float d = SdRoundRect(i.local, i.shape.xy, i.radii);
            float blur = max(i.shape.z, 1.0);
            float coverage = 1.0 - smoothstep(-blur, blur, d);
            if ((i.flags & FLAG_CUTOUT) != 0) {
                // Nothing under the caster: same size and corners as the shadow, `offset` up and left of it.
                coverage *= saturate(0.5 + SdRoundRect(i.local + i.uv, i.shape.xy, i.radii));
            }
            result = fill * coverage;
        } else {
            float d;
            if (type == PRIM_ELLIPSE) {
                d = SdEllipse(i.local, i.shape.xy);
            } else {
                d = SdRoundRect(i.local, i.shape.xy, i.radii);
            }
            float borderWidth = i.shape.z;
            if (borderWidth > 0.0) {
                // The border occupies the outermost borderWidth pixels, drawn over the fill (like CSS: a translucent
                // border shows the background under it); blend to the plain fill across its inner edge.
                float4 overFill = i.border + fill * (1.0 - i.border.a);
                fill = lerp(overFill, fill, saturate(0.5 - (d + borderWidth)));
            }
            result = fill * saturate(0.5 - d);
        }
    }
    return result;
}

// HDR10 output: linear Rec.709 (already at paper white, in units of 10000 nits) to PQ-encoded Rec.2020. Blending then
// happens in PQ space, which is how an overlay composites over a finished HDR10 frame.
float3 Rec709ToRec2020(float3 c) {
    return float3(dot(float3(0.627404, 0.329283, 0.043313), c),
                  dot(float3(0.069097, 0.919540, 0.011362), c),
                  dot(float3(0.016391, 0.088013, 0.895595), c));
}

float3 PqEncode(float3 y) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 p = pow(max(y, 0.0), m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float4 PSMain(VSOut i) : SV_Target {
    float4 c;
    // Deep inside a big untextured rect (window backgrounds), deeper than every corner, the border and the
    // anti-aliasing ramp, there is nothing to compute. Tested before anything else, from the fewest interpolants: such
    // pixels are most of a menu's area.
    [branch] if ((i.flags & PS_INTERIOR) != 0 && all(abs(i.uv) < 1.0)) {
        c = i.color;
    } else {
        c = Shade(i);
    }
    // A real branch on a constant: SDR targets must not pay for the encode (flattened, it costs every pixel ~25%).
    [branch] if ((g_globalFlags & GLOBAL_PQ) != 0) {
        // Premultiplied in, premultiplied out: encode the straight color, then apply coverage again.
        c.rgb = PqEncode(Rec709ToRec2020(c.rgb / max(c.a, 1e-6))) * c.a;
    }
    return c;
}
