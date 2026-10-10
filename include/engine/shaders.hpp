#pragma once
namespace engine {
const char* shader = R"WGSL(
struct Globals {
    vp: mat4x4f,
    light: mat4x4f,
    eye: vec4f,
    sun: vec4f,
    fog: vec4f
};
struct Object {
    model: mat4x4f,
    color: vec4f,
    params: vec4f,
    maps: vec4f,
    emissive: vec4f,
    extra: vec4f
};
@group(0) @binding(0) var<uniform> g: Globals;
@group(0) @binding(1) var shadowMap: texture_depth_2d;
@group(0) @binding(2) var shadowSampler: sampler_comparison;
@group(1) @binding(0) var<uniform> obj: Object;
@group(1) @binding(1) var albedo: texture_2d<f32>;
@group(1) @binding(2) var texSampler: sampler;
@group(1) @binding(3) var normalMap: texture_2d<f32>;
@group(1) @binding(4) var materialMap: texture_2d<f32>;
@group(1) @binding(5) var occlusionMap: texture_2d<f32>;
@group(1) @binding(6) var emissiveMap: texture_2d<f32>;
struct In {
    @location(0) p: vec3f,
    @location(1) n: vec3f,
    @location(2) uv: vec2f,
    @location(3) color: vec3f,
    @location(4) kind: f32
};
struct Out {
    @builtin(position) clip: vec4f,
    @location(0) pos: vec3f,
    @location(1) n: vec3f,
    @location(2) uv: vec2f,
    @location(3) color: vec3f,
    @location(4) kind: f32,
    @location(5) light: vec4f
};
fn transformVertex(v: In) -> Out {
    var o: Out;
    let p = obj.model * vec4f(v.p, 1);
    o.clip = g.vp * p;
    o.pos = p.xyz;
    let m = mat3x3f(obj.model[0].xyz, obj.model[1].xyz, obj.model[2].xyz);
    o.n = normalize(m * (v.n / vec3f(dot(m[0], m[0]), dot(m[1], m[1]), dot(m[2], m[2]))));
    o.uv = v.uv;
    o.color = v.color;
    o.kind = v.kind;
    o.light = g.light * p;
    return o;
}
@vertex fn vs(v: In) -> Out {
    return transformVertex(v);
}
@vertex fn vsShadow(v: In) -> Out {
    var o = transformVertex(v);
    o.clip = o.light;
    return o;
}
@fragment fn fsShadow(v: Out) {
    if (textureSample(albedo, texSampler, v.uv * obj.maps.w).a * obj.color.a < obj.maps.z) {
        discard;
    }
}
fn sky(d: vec3f) -> vec3f {
    let h = clamp(d.y, 0., 1.);
    var c = mix(vec3f(.56, .75, .84), vec3f(.065, .27, .48), pow(h, .55));
    c += vec3f(1., .72, .38) * pow(max(dot(d, g.sun.xyz), 0.), 250.) * 1.5;
    return c;
}
fn tonemap(c: vec3f) -> vec3f {
    let x = max(c, vec3f(0));
    let y = clamp((x * (2.51 * x + .03)) / (x * (2.43 * x + .59) + .14), vec3f(0), vec3f(1));
    return select(1.055 * pow(y, vec3f(1. / 2.4)) - .055, 12.92 * y, y <= vec3f(.0031308));
}
fn fresnelSchlick(c: f32, f0: vec3f) -> vec3f {
    return f0 + (vec3f(1) - f0) * pow(1. - clamp(c, 0., 1.), 5.);
}
fn ggxDistribution(ndh: f32, r: f32) -> f32 {
    let a = r * r;
    let a2 = a * a;
    let d = ndh * ndh * (a2 - 1.) + 1.;
    return a2 / (3.14159265 * d * d + .000001);
}
fn smithVisibility(ndv: f32, ndl: f32, r: f32) -> f32 {
    let k = (r + 1.) * (r + 1.) * .125;
    return ndv / (ndv * (1. - k) + k) * ndl / (ndl * (1. - k) + k);
}
@fragment fn fs(v: Out, @builtin(front_facing) front: bool) -> @location(0) vec4f {
    let uv = v.uv * obj.maps.w;
    let tex = textureSample(albedo, texSampler, uv);
    let nm = textureSample(normalMap, texSampler, uv).xyz * 2. - 1.;
    let arm = textureSample(materialMap, texSampler, uv).rgb;
    let px = dpdx(v.pos);
    let py = dpdy(v.pos);
    let ux = dpdx(uv);
    let uy = dpdy(uv);
    if (tex.a * obj.color.a < obj.maps.z) {
        discard;
    }
    var base = v.color * obj.color.rgb * mix(vec3f(1), tex.rgb, min(1., obj.params.z));
    var n = normalize(v.n) * select(-1., 1., front);
    let view = normalize(g.eye.xyz - v.pos);
    if (obj.params.z > 1.5) {
        let detail = dot(tex.rgb, vec3f(.299, .587, .114));
        base = v.color * obj.color.rgb * vec3f(.12, .23, .045) * (.6 + detail * 3.);
    }
    if (v.kind > 3.5 && v.kind < 4.5) {
        base = mix(base, v.color * obj.color.rgb * .28, smoothstep(160., 900., distance(v.pos,
            g.eye.xyz)));
    }
    let det = ux.x * uy.y - ux.y * uy.x;
    if (obj.maps.x > .5 && abs(det) > .0000001) {
        let td = (px * uy.y - py * ux.y) / det;
        let bd = (py * ux.x - px * uy.x) / det;
        let t = normalize(td - n * dot(n, td));
        let b = normalize(bd - n * dot(n, bd));
        let mapped = normalize(vec3f(nm.xy * obj.extra.x, nm.z));
        n = normalize(t * mapped.x + b * mapped.y + n * mapped.z);
    }
    var rough = clamp(obj.params.x * mix(1., arm.g, obj.maps.y), .055, 1.);
    var metal = clamp(obj.params.y * mix(1., arm.b, obj.maps.y), 0., 1.);
    var ao = 1.;
    if (obj.extra.y > .001) {
        ao = 1. - obj.extra.y * (1. - textureSample(occlusionMap, texSampler, uv).r);
    }
    var water = false;
    if (v.kind > 1.5 && v.kind < 2.5) {
        water = true;
        let t = g.eye.w;
        let p = v.pos.xz;
        n = normalize(vec3f(.035 * sin(dot(p, vec2f(.11, .07)) + t * .8) + .018 * sin(dot(p,
            vec2f(.37, -.21)) + t * 1.3), 1., .035 * cos(dot(p, vec2f(-.06, .13)) + t * .7)));
        rough = .12;
        metal = 0.;
        ao = 1.;
        base = mix(vec3f(.012, .16, .20), vec3f(.022, .34, .32), .5 + .5 * sin(dot(p, vec2f(.004,
            .008))));
    }
    let l = normalize(g.sun.xyz);
    let h = normalize(l + view);
    let ndl = max(dot(n, l), 0.);
    let ndv = max(dot(n, view), .001);
    let ndh = max(dot(n, h), 0.);
    let vdh = max(dot(view, h), 0.);
    let reflected = reflect(-view, n);
    let lp = v.light.xyz / v.light.w;
    let shadowUV = lp.xy * vec2f(.5, -.5) + vec2f(.5);
    var shade = 1.;
    if (all(shadowUV > vec2f(.002)) && all(shadowUV < vec2f(.998)) && lp.z > 0. && lp.z < 1.) {
        shade = 0.;
        for (var x: i32 = -1; x <= 1; x++) {
            for (var y: i32 = -1; y <= 1; y++) {
                shade += textureSampleCompareLevel(shadowMap, shadowSampler, shadowUV + vec2f(f32(x),
                    f32(y)) / 2048., lp.z - .0007);
            }
        }
        shade /= 9.;
    }
    let f0 = mix(vec3f(.04), base, metal);
    let f = fresnelSchlick(vdh, f0);
    let specular = f * ggxDistribution(ndh, rough) * smithVisibility(ndv, ndl, rough) / max(4. * ndv * ndl,
        .001);
    let diffuse = (vec3f(1) - f) * (1. - metal) * base / 3.14159265;
    let sunlight = vec3f(3.8, 3.35, 2.85);
    var direct = (diffuse + specular) * sunlight * ndl * shade;
    let fresnelAmbient = f0 + (max(vec3f(1. - rough), f0) - f0) * pow(1. - ndv, 5.);
    let irradiance = mix(vec3f(.12, .105, .09), sky(n) * .62, clamp(n.y * .5 + .5, 0.,
        1.));
    let reflection = mix(sky(reflected) * 1.65, sky(n) * 1.2, rough * rough);
    var color = direct + ao * (base * (1. - metal) * (vec3f(1) - fresnelAmbient) * irradiance
        + reflection * fresnelAmbient * .82);
    if (obj.extra.z > .001) {
        let coatF = fresnelSchlick(vdh, vec3f(.04));
        let coat = coatF * ggxDistribution(ndh, .12) * smithVisibility(ndv, ndl, .12) / max(4. * ndv * ndl,
            .001);
        color = color * (1. - .12 * obj.extra.z)
            + obj.extra.z * (coat * sunlight * ndl * shade + sky(reflected) * coatF * .65);
    }
    if (obj.maps.z > 0.) {
        color += base * max(dot(-n, l), 0.) * .12;
    }
    let edge = pow(1. - ndv, 5.);
    if (water) {
        color = mix(color, sky(reflected) * 1.3, .18 + edge * .52);
    }
    if (obj.params.w > .5) {
        color = color * .16 + sky(reflected) * (vec3f(.12) + vec3f(edge * .65)) + direct * .35;
    }
    if (dot(obj.emissive.xyz, vec3f(1)) > .001) {
        color += obj.emissive.xyz * textureSample(emissiveMap, texSampler, uv).rgb;
    }
    let fog = 1. - exp(-pow(distance(v.pos, g.eye.xyz) / g.fog.w, 1.6));
    color = mix(color, g.fog.rgb, clamp(fog, 0., 1.));
    return vec4f(tonemap(color), select(1., obj.color.a * tex.a, obj.params.w > .5));
}
struct SkyOut {
    @builtin(position) p: vec4f,
    @location(0) uv: vec2f
};
@vertex fn skyVS(@builtin(vertex_index) i: u32) -> SkyOut {
    let x = f32((i << 1u) & 2u);
    let y = f32(i & 2u);
    var o: SkyOut;
    o.p = vec4f(x * 2. - 1., y * 2. - 1., .99999, 1);
    o.uv = vec2f(x, y);
    return o;
}
@fragment fn skyFS(v: SkyOut) -> @location(0) vec4f {
    let h = clamp(v.uv.y, 0., 1.);
    var c = mix(vec3f(.55, .74, .84), vec3f(.055, .25, .48), pow(h, .75));
    let wisps = pow(max(0., sin(v.uv.x * 9. + h * 25.) * sin(v.uv.x * 3. - h * 31.)),
        10.) * .065;
    c += wisps * smoothstep(.4, .8, h);
    return vec4f(tonemap(c), 1);
}
)WGSL";
}
