#include "engine/renderer.hpp"
#include "engine/shaders.hpp"
#define CGLTF_IMPLEMENTATION
#include "cgltf.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"
#include <array>
#include <cfloat>
#include <cstring>
namespace engine {
void Geometry::tri(Vertex a, Vertex b, Vertex c) {
    uint32_t k = v.size();
    v.insert(v.end(), {a, b, c});
    i.insert(i.end(), {k, k + 1, k + 2});
}
void Geometry::quad(V3 a, V3 b, V3 c, V3 d, V3 color, float kind) {
    V3 n = norm(cross(b - a, c - a));
    tri({a, n, 0, 0, color, kind}, {b, n, 1, 0, color, kind}, {c, n, 1, 1, color, kind});
    tri({a, n, 0, 0, color, kind}, {c, n, 1, 1, color, kind}, {d, n, 0, 1, color, kind});
}
static wgpu::Buffer buffer(wgpu::Device d, uint64_t size, wgpu::BufferUsage usage) {
    wgpu::BufferDescriptor bd{};
    bd.size = (size + 3) & ~3ull;
    bd.usage = usage;
    return d.CreateBuffer(&bd);
}
static float srgbToLinear(float v) {
    return v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
}
static float linearToSrgb(float v) {
    return v <= .0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - .055f;
}
void Renderer::init(void (*callback)(bool), HostCallbacks hostCallbacks) {
    host = std::move(hostCallbacks);
    if (!host.createSurface) {
        error = "No graphics surface provider was supplied";
        callback(false);
        return;
    }
    instance = wgpu::CreateInstance();
    wgpu::RequestAdapterOptions opts{};
    opts.powerPreference = wgpu::PowerPreference::HighPerformance;
    instance.RequestAdapter(
        &opts, wgpu::CallbackMode::AllowSpontaneous,
        [this, callback](wgpu::RequestAdapterStatus s, wgpu::Adapter a, wgpu::StringView msg) {
            if (s != wgpu::RequestAdapterStatus::Success) {
                error = std::string(msg.data, msg.length);
                callback(false);
                return;
            }
            adapter = a;
            wgpu::DeviceDescriptor desc{};
            desc.SetDeviceLostCallback(
                wgpu::CallbackMode::AllowSpontaneous,
                [this](const wgpu::Device&, wgpu::DeviceLostReason, wgpu::StringView) {
                    ready = false;
                    error = "The graphics device was lost. Restart the application.";
                    if (host.reportError)
                        host.reportError(error.c_str());
                });
            desc.SetUncapturedErrorCallback(
                [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView m, Renderer* self) {
                    self->ready = false;
                    self->error = std::string(m.data, m.length);
                    std::fprintf(stderr, "WebGPU: %s\n", self->error.c_str());
                    if (self->host.reportError)
                        self->host.reportError(self->error.c_str());
                },
                this);
            adapter.RequestDevice(&desc, wgpu::CallbackMode::AllowSpontaneous,
                                  [this, callback](wgpu::RequestDeviceStatus ds, wgpu::Device d,
                                                   wgpu::StringView message) {
                                      if (ds != wgpu::RequestDeviceStatus::Success) {
                                          error = std::string(message.data, message.length);
                                          callback(false);
                                          return;
                                      }
                                      device = d;
                                      queue = d.GetQueue();
                                      setup();
                                      ready = true;
                                      callback(true);
                                  });
        });
}
void Renderer::setup() {
    surface = host.createSurface(instance);
    wgpu::SurfaceCapabilities cap;
    surface.GetCapabilities(adapter, &cap);
    format = cap.formats[0];
    for (size_t i = 0; i < cap.formatCount; i++)
        if (cap.formats[i] == wgpu::TextureFormat::BGRA8Unorm ||
            cap.formats[i] == wgpu::TextureFormat::RGBA8Unorm) {
            format = cap.formats[i];
            break;
        }
    globals = buffer(device, 176, wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
    objects = buffer(device, 256 * 4096, wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
    wgpu::SamplerDescriptor samp{};
    samp.magFilter = wgpu::FilterMode::Linear;
    samp.minFilter = wgpu::FilterMode::Linear;
    samp.mipmapFilter = wgpu::MipmapFilterMode::Linear;
    samp.maxAnisotropy = 4;
    samp.addressModeU = samp.addressModeV = wgpu::AddressMode::Repeat;
    sampler = device.CreateSampler(&samp);
    // Small labels must integrate the large raster glyph, rather than sampling
    // occasional source pixels. Atlas gutters isolate these filtered mip levels.
    samp.maxAnisotropy = 1;
    samp.addressModeU = samp.addressModeV = wgpu::AddressMode::ClampToEdge;
    samp.mipmapFilter = wgpu::MipmapFilterMode::Linear;
    samp.lodMaxClamp = 4;
    uiSampler = device.CreateSampler(&samp);
    samp.maxAnisotropy = 1;
    samp.compare = wgpu::CompareFunction::LessEqual;
    samp.addressModeU = samp.addressModeV = wgpu::AddressMode::ClampToEdge;
    shadowSampler = device.CreateSampler(&samp);
    wgpu::TextureDescriptor tex{};
    tex.size = {2048, 2048, 1};
    tex.format = wgpu::TextureFormat::Depth32Float;
    tex.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
    shadow = device.CreateTexture(&tex);
    shadowView = shadow.CreateView();
    wgpu::BindGroupLayoutEntry ge[3]{};
    ge[0].binding = 0;
    ge[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    ge[0].buffer.type = wgpu::BufferBindingType::Uniform;
    ge[0].buffer.minBindingSize = 176;
    ge[1].binding = 1;
    ge[1].visibility = wgpu::ShaderStage::Fragment;
    ge[1].texture.sampleType = wgpu::TextureSampleType::Depth;
    ge[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    ge[2].binding = 2;
    ge[2].visibility = wgpu::ShaderStage::Fragment;
    ge[2].sampler.type = wgpu::SamplerBindingType::Comparison;
    wgpu::BindGroupLayoutDescriptor gld{};
    gld.entryCount = 3;
    gld.entries = ge;
    auto globalLayout = device.CreateBindGroupLayout(&gld);
    gld.entryCount = 1;
    auto shadowLayout = device.CreateBindGroupLayout(&gld);
    wgpu::BindGroupEntry gbe[3]{};
    gbe[0].binding = 0;
    gbe[0].buffer = globals;
    gbe[0].size = 176;
    gbe[1].binding = 1;
    gbe[1].textureView = shadowView;
    gbe[2].binding = 2;
    gbe[2].sampler = shadowSampler;
    wgpu::BindGroupDescriptor gbd{};
    gbd.layout = globalLayout;
    gbd.entryCount = 3;
    gbd.entries = gbe;
    globalBG = device.CreateBindGroup(&gbd);
    gbd.layout = shadowLayout;
    gbd.entryCount = 1;
    shadowBG = device.CreateBindGroup(&gbd);
    wgpu::BindGroupLayoutEntry oe[7]{};
    oe[0].binding = 0;
    oe[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    oe[0].buffer.type = wgpu::BufferBindingType::Uniform;
    oe[0].buffer.minBindingSize = 144;
    oe[0].buffer.hasDynamicOffset = true;
    oe[1].binding = 1;
    oe[1].visibility = wgpu::ShaderStage::Fragment;
    oe[1].texture.sampleType = wgpu::TextureSampleType::Float;
    oe[1].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    oe[2].binding = 2;
    oe[2].visibility = wgpu::ShaderStage::Fragment;
    oe[2].sampler.type = wgpu::SamplerBindingType::Filtering;
    for (int i = 3; i < 7; i++) {
        oe[i] = oe[1];
        oe[i].binding = i;
    }
    wgpu::BindGroupLayoutDescriptor old{};
    old.entryCount = 7;
    old.entries = oe;
    objectLayout = device.CreateBindGroupLayout(&old);
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = shader;
    wgpu::ShaderModuleDescriptor shd{};
    shd.nextInChain = &wgsl;
    auto module = device.CreateShaderModule(&shd);
    wgpu::VertexAttribute attrs[5]{};
    wgpu::VertexFormat formats[] = {wgpu::VertexFormat::Float32x3, wgpu::VertexFormat::Float32x3,
                                    wgpu::VertexFormat::Float32x2, wgpu::VertexFormat::Float32x3,
                                    wgpu::VertexFormat::Float32};
    uint64_t offsets[] = {0, 12, 24, 32, 44};
    for (int i = 0; i < 5; i++) {
        attrs[i].shaderLocation = i;
        attrs[i].format = formats[i];
        attrs[i].offset = offsets[i];
    }
    wgpu::VertexBufferLayout vb{};
    vb.arrayStride = sizeof(Vertex);
    vb.attributeCount = 5;
    vb.attributes = attrs;
    wgpu::BindGroupLayout layouts[] = {globalLayout, objectLayout};
    wgpu::PipelineLayoutDescriptor pld{};
    pld.bindGroupLayoutCount = 2;
    pld.bindGroupLayouts = layouts;
    wgpu::RenderPipelineDescriptor pd{};
    pd.layout = device.CreatePipelineLayout(&pld);
    pd.vertex.module = module;
    pd.vertex.entryPoint = "vs";
    pd.vertex.bufferCount = 1;
    pd.vertex.buffers = &vb;
    pd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    pd.primitive.cullMode = wgpu::CullMode::None;
    pd.multisample.count = 4;
    wgpu::DepthStencilState depthState{};
    depthState.format = wgpu::TextureFormat::Depth32Float;
    depthState.depthWriteEnabled = true;
    depthState.depthCompare = wgpu::CompareFunction::Less;
    pd.depthStencil = &depthState;
    wgpu::ColorTargetState ct{};
    ct.format = format;
    wgpu::FragmentState fs{};
    fs.module = module;
    fs.entryPoint = "fs";
    fs.targetCount = 1;
    fs.targets = &ct;
    pd.fragment = &fs;
    opaque = device.CreateRenderPipeline(&pd);
    wgpu::BlendState blend{};
    blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    blend.alpha.srcFactor = wgpu::BlendFactor::One;
    blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    ct.blend = &blend;
    depthState.depthWriteEnabled = false;
    transparent = device.CreateRenderPipeline(&pd);
    // Depth-only shadow pass has a separate group layout: the attachment is never sampled while
    // written.
    layouts[0] = shadowLayout;
    pd.layout = device.CreatePipelineLayout(&pld);
    pd.vertex.entryPoint = "vsShadow";
    fs.entryPoint = "fsShadow";
    fs.targetCount = 0;
    fs.targets = nullptr;
    pd.fragment = &fs;
    pd.multisample.count = 1;
    depthState.depthWriteEnabled = true;
    depthState.depthBias = 2;
    depthState.depthBiasSlopeScale = 2;
    shadowPipe = device.CreateRenderPipeline(&pd);
    pd.layout = nullptr;
    pd.vertex.entryPoint = "skyVS";
    pd.vertex.bufferCount = 0;
    pd.vertex.buffers = nullptr;
    depthState.depthWriteEnabled = false;
    depthState.depthCompare = wgpu::CompareFunction::Always;
    depthState.depthBias = 0;
    depthState.depthBiasSlopeScale = 0;
    pd.depthStencil = &depthState;
    pd.multisample.count = 4;
    ct.blend = nullptr;
    fs.entryPoint = "skyFS";
    fs.targetCount = 1;
    fs.targets = &ct;
    pd.fragment = &fs;
    skyPipe = device.CreateRenderPipeline(&pd);
    // A tiny standalone UI pipeline.  It consumes pixels already converted to
    // clip space by the interface module, so no game/UI state leaks into shaders.
    static const char* uiShader = R"(
@group(0) @binding(0) var uiMask: texture_2d<f32>;
@group(0) @binding(1) var uiSampler: sampler;
struct In {
    @location(0) p: vec2f,
    @location(1) uv: vec2f,
    @location(2) c: vec4f,
    @location(3) mode: f32
};
struct Out {
    @builtin(position) p: vec4f,
    @location(0) uv: vec2f,
    @location(1) c: vec4f,
    @location(2) @interpolate(flat) mode: f32
};
@vertex fn vs_ui(v: In) -> Out {
    var o: Out;
    o.p = vec4f(v.p, 0.0, 1.0);
    o.uv = v.uv;
    o.c = v.c;
    o.mode = v.mode;
    return o;
}
@fragment fn fs_ui(v: Out) -> @location(0) vec4f {
    let sampled = textureSample(uiMask, uiSampler, v.uv);
    let coverage = select(1.0, sampled.a, v.mode > 0.5 || v.c.a < 0.0);
    let rgb = select(v.c.rgb, v.c.rgb * sampled.rgb, v.mode > 1.5);
    return vec4f(rgb, abs(v.c.a) * coverage);
})";
    wgpu::ShaderSourceWGSL uiWgsl{};
    uiWgsl.code = uiShader;
    wgpu::ShaderModuleDescriptor uiShd{};
    uiShd.nextInChain = &uiWgsl;
    auto uiModule = device.CreateShaderModule(&uiShd);
    wgpu::VertexAttribute uiAttrs[4]{};
    uiAttrs[0].shaderLocation = 0;
    uiAttrs[0].format = wgpu::VertexFormat::Float32x2;
    uiAttrs[0].offset = 0;
    uiAttrs[1].shaderLocation = 1;
    uiAttrs[1].format = wgpu::VertexFormat::Float32x2;
    uiAttrs[1].offset = 8;
    uiAttrs[2].shaderLocation = 2;
    uiAttrs[2].format = wgpu::VertexFormat::Float32x4;
    uiAttrs[2].offset = 16;
    uiAttrs[3].shaderLocation = 3;
    uiAttrs[3].format = wgpu::VertexFormat::Float32;
    uiAttrs[3].offset = 32;
    wgpu::VertexBufferLayout uiVb{};
    uiVb.arrayStride = sizeof(UiVertex);
    uiVb.attributeCount = 4;
    uiVb.attributes = uiAttrs;
    wgpu::BindGroupLayoutEntry uiEntries[2]{};
    uiEntries[0].binding = 0;
    uiEntries[0].visibility = wgpu::ShaderStage::Fragment;
    uiEntries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    uiEntries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    uiEntries[1].binding = 1;
    uiEntries[1].visibility = wgpu::ShaderStage::Fragment;
    uiEntries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    wgpu::BindGroupLayoutDescriptor uiLayoutDesc{};
    uiLayoutDesc.entryCount = 2;
    uiLayoutDesc.entries = uiEntries;
    uiLayout = device.CreateBindGroupLayout(&uiLayoutDesc);
    wgpu::PipelineLayoutDescriptor uiPipelineLayout{};
    uiPipelineLayout.bindGroupLayoutCount = 1;
    uiPipelineLayout.bindGroupLayouts = &uiLayout;
    wgpu::RenderPipelineDescriptor uiPd{};
    uiPd.layout = device.CreatePipelineLayout(&uiPipelineLayout);
    uiPd.vertex.module = uiModule;
    uiPd.vertex.entryPoint = "vs_ui";
    uiPd.vertex.bufferCount = 1;
    uiPd.vertex.buffers = &uiVb;
    uiPd.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    uiPd.primitive.cullMode = wgpu::CullMode::None;
    uiPd.multisample.count = 4;
    // This pass shares the scene's depth attachment but always draws over it.
    // Declaring the attachment is required for WebGPU pipeline compatibility.
    wgpu::DepthStencilState uiDepth{};
    uiDepth.format = wgpu::TextureFormat::Depth32Float;
    uiDepth.depthWriteEnabled = false;
    uiDepth.depthCompare = wgpu::CompareFunction::Always;
    uiPd.depthStencil = &uiDepth;
    wgpu::BlendState uiBlend{};
    uiBlend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
    uiBlend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    uiBlend.alpha.srcFactor = wgpu::BlendFactor::One;
    uiBlend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    wgpu::ColorTargetState uiCt{};
    uiCt.format = format;
    uiCt.blend = &uiBlend;
    wgpu::FragmentState uiFs{};
    uiFs.module = uiModule;
    uiFs.entryPoint = "fs_ui";
    uiFs.targetCount = 1;
    uiFs.targets = &uiCt;
    uiPd.fragment = &uiFs;
    uiPipe = device.CreateRenderPipeline(&uiPd);
    uiVertices =
        buffer(device, 1024 * 1024, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
    unsigned char pixel[4] = {255, 255, 255, 255}, normalPixel[4] = {128, 128, 255, 255};
    white = uploadTexture(pixel, 1, 1, true);
    flatNormal = uploadTexture(normalPixel, 1, 1, false);
    setUiAtlas(pixel, 1, 1);
    addMaterial({});
}
void Renderer::setUiAtlas(const unsigned char* pixels, int w, int h) {
    auto atlas = uploadTexture(pixels, w, h, false);
    wgpu::BindGroupEntry entries[2]{};
    entries[0].binding = 0;
    entries[0].textureView = atlas;
    entries[1].binding = 1;
    entries[1].sampler = uiSampler;
    wgpu::BindGroupDescriptor descriptor{};
    descriptor.layout = uiLayout;
    descriptor.entryCount = 2;
    descriptor.entries = entries;
    uiBG = device.CreateBindGroup(&descriptor);
}
void Renderer::resize(int w, int h) {
    if (w == width && h == height)
        return;
    width = w;
    height = h;
    wgpu::SurfaceConfiguration conf{};
    conf.device = device;
    conf.format = format;
    conf.usage = wgpu::TextureUsage::RenderAttachment;
    conf.width = w;
    conf.height = h;
    conf.presentMode = wgpu::PresentMode::Fifo;
    conf.alphaMode = wgpu::CompositeAlphaMode::Opaque;
    surface.Configure(&conf);
}
wgpu::TextureView Renderer::uploadTexture(const unsigned char* pixels, int w, int h, bool srgb) {
    wgpu::TextureDescriptor td{};
    td.size = {uint32_t(w), uint32_t(h), 1};
    td.mipLevelCount = 1 + uint32_t(std::floor(std::log2(std::max(w, h))));
    td.format = srgb ? wgpu::TextureFormat::RGBA8UnormSrgb : wgpu::TextureFormat::RGBA8Unorm;
    td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    auto texture = device.CreateTexture(&td);
    std::vector<unsigned char> mip(pixels, pixels + size_t(w) * h * 4);
    for (uint32_t level = 0; level < td.mipLevelCount; level++) {
        wgpu::TexelCopyTextureInfo dest{};
        dest.texture = texture;
        dest.mipLevel = level;
        wgpu::TexelCopyBufferLayout layout{};
        layout.bytesPerRow = w * 4;
        layout.rowsPerImage = h;
        wgpu::Extent3D extent{uint32_t(w), uint32_t(h), 1};
        queue.WriteTexture(&dest, mip.data(), mip.size(), &layout, &extent);
        if (level + 1 == td.mipLevelCount)
            break;
        int nw = std::max(1, w / 2), nh = std::max(1, h / 2);
        std::vector<unsigned char> next(nw * nh * 4);
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    float sum = 0;
                    for (int dy = 0; dy < 2; dy++)
                        for (int dx = 0; dx < 2; dx++) {
                            float v = mip[(std::min(h - 1, y * 2 + dy) * w +
                                           std::min(w - 1, x * 2 + dx)) *
                                              4 +
                                          c] /
                                      255.f;
                            sum += srgb && c < 3 ? srgbToLinear(v) : v;
                        }
                    sum *= .25f;
                    next[(y * nw + x) * 4 + c] =
                        uint8_t(clamp(srgb && c < 3 ? linearToSrgb(sum) : sum, 0, 1) * 255 + .5f);
                }
        mip = std::move(next);
        w = nw;
        h = nh;
    }
    return texture.CreateView();
}
wgpu::TextureView Renderer::loadTexture(const std::string& path, bool srgb) {
    std::string key = path + (srgb ? ":srgb" : ":linear");
    auto found = textureCache.find(key);
    if (found != textureCache.end())
        return found->second;
    int w, h, c;
    auto* pixels = stbi_load(path.c_str(), &w, &h, &c, 4);
    if (!pixels) {
        std::fprintf(stderr, "Texture missing: %s\n", path.c_str());
        return {};
    }
    auto result = uploadTexture(pixels, w, h, srgb);
    stbi_image_free(pixels);
    textureCache[key] = result;
    return result;
}
int Renderer::loadSurface(const char* folder, float uvScale, V3 tint) {
    Material m;
    m.color = tint;
    m.uvScale = uvScale;
    m.texture = loadTexture(std::string(folder) + "/diff.jpg");
    m.normal = loadTexture(std::string(folder) + "/normal.jpg", false);
    m.orm = loadTexture(std::string(folder) + "/arm.jpg", false);
    m.occlusion = m.orm;
    m.aoStrength = 1;
    return addMaterial(m);
}
int Renderer::addMaterial(Material m, const unsigned char* pixels, int w, int h) {
    if (pixels)
        m.texture = uploadTexture(pixels, w, h, true);
    if (!m.texture)
        m.texture = white;
    materials.push_back(m);
    wgpu::BindGroupEntry e[7]{};
    e[0].binding = 0;
    e[0].buffer = objects;
    e[0].size = 144;
    e[1].binding = 1;
    e[1].textureView = m.texture;
    e[2].binding = 2;
    e[2].sampler = sampler;
    e[3].binding = 3;
    e[3].textureView = m.normal ? m.normal : flatNormal;
    e[4].binding = 4;
    e[4].textureView = m.orm ? m.orm : white;
    e[5].binding = 5;
    e[5].textureView = m.occlusion ? m.occlusion : white;
    e[6].binding = 6;
    e[6].textureView = m.emissive ? m.emissive : white;
    wgpu::BindGroupDescriptor bd{};
    bd.layout = objectLayout;
    bd.entryCount = 7;
    bd.entries = e;
    materialBG.push_back(device.CreateBindGroup(&bd));
    return int(materials.size() - 1);
}
int Renderer::addMesh(const Geometry& g, int mat, V3 center, float radius) {
    Mesh m;
    m.material = mat;
    m.center = center;
    m.radius = radius;
    m.count = g.i.size();
    m.vb = buffer(device, g.v.size() * sizeof(Vertex),
                  wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
    m.ib = buffer(device, g.i.size() * 4, wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst);
    queue.WriteBuffer(m.vb, 0, g.v.data(), g.v.size() * sizeof(Vertex));
    queue.WriteBuffer(m.ib, 0, g.i.data(), g.i.size() * 4);
    meshes.push_back(m);
    return meshes.size() - 1;
}
bool Renderer::loadModel(const char* path, std::vector<int>& out,
                         const ModelImportOptions& options) {
    cgltf_options parseOptions{};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&parseOptions, path, &data) != cgltf_result_success)
        return false;
    if (cgltf_load_buffers(&parseOptions, data, path) != cgltf_result_success) {
        cgltf_free(data);
        return false;
    }
    std::string folder = std::string(path).substr(0, std::string(path).find_last_of('/') + 1);
    auto imageTexture = [&](cgltf_texture* texture, bool srgb) -> wgpu::TextureView {
        if (!texture || !texture->image)
            return {};
        auto* image = texture->image;
        if (image->uri)
            return loadTexture(folder + image->uri, srgb);
        std::string key = std::string(path) + ":" + std::to_string(image - data->images) +
                          (srgb ? ":srgb" : ":linear");
        if (textureCache.count(key))
            return textureCache[key];
        if (!image->buffer_view)
            return {};
        auto* view = image->buffer_view;
        int w, h, c;
        auto* pixels = stbi_load_from_memory((unsigned char*)view->buffer->data + view->offset,
                                             int(view->size), &w, &h, &c, 4);
        if (!pixels)
            return {};
        auto result = uploadTexture(pixels, w, h, srgb);
        stbi_image_free(pixels);
        textureCache[key] = result;
        return result;
    };
    std::vector<int> importedMaterials;
    for (size_t i = 0; i < data->materials_count; i++) {
        auto& source = data->materials[i];
        auto& pbr = source.pbr_metallic_roughness;
        Material material;
        material.color = {pbr.base_color_factor[0], pbr.base_color_factor[1],
                          pbr.base_color_factor[2]};
        material.rough = pbr.roughness_factor;
        material.metal = pbr.metallic_factor;
        material.blend = source.alpha_mode == cgltf_alpha_mode_blend;
        material.alpha = material.blend ? pbr.base_color_factor[3] : 1;
        material.cutoff = source.alpha_mode == cgltf_alpha_mode_mask ? source.alpha_cutoff : 0;
        material.texture = imageTexture(pbr.base_color_texture.texture, true);
        material.normal = imageTexture(source.normal_texture.texture, false);
        material.normalScale = source.normal_texture.scale;
        material.orm = imageTexture(pbr.metallic_roughness_texture.texture, false);
        material.occlusion = imageTexture(source.occlusion_texture.texture, false);
        material.aoStrength = material.occlusion ? source.occlusion_texture.scale : 0;
        material.emissive = imageTexture(source.emissive_texture.texture, true);
        material.emissiveColor = {source.emissive_factor[0], source.emissive_factor[1],
                                  source.emissive_factor[2]};
        if (options.materialOverride)
            options.materialOverride(
                source.name ? std::string_view(source.name) : std::string_view{}, material);
        importedMaterials.push_back(addMaterial(material));
    }
    struct Part {
        Geometry geometry;
        int material = 0;
        std::string nodeName, parentName;
        V3 pivot;
    };
    std::vector<Part> parts;
    V3 low{FLT_MAX, FLT_MAX, FLT_MAX}, high{-FLT_MAX, -FLT_MAX, -FLT_MAX};
    for (size_t nodeIndex = 0; nodeIndex < data->nodes_count; nodeIndex++) {
        auto& node = data->nodes[nodeIndex];
        if (!node.mesh)
            continue;
        M4 transform;
        cgltf_node_transform_world(&node, transform.m);
        std::string nodeName = node.name ? node.name : "",
                    parentName = node.parent && node.parent->name ? node.parent->name : "";
        for (size_t primitiveIndex = 0; primitiveIndex < node.mesh->primitives_count;
             primitiveIndex++) {
            auto& primitive = node.mesh->primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles)
                continue;
            auto* positions = cgltf_find_accessor(&primitive, cgltf_attribute_type_position, 0);
            auto* normals = cgltf_find_accessor(&primitive, cgltf_attribute_type_normal, 0);
            auto* texcoords = cgltf_find_accessor(&primitive, cgltf_attribute_type_texcoord, 0);
            if (!positions)
                continue;
            Part part;
            part.material =
                primitive.material ? importedMaterials[primitive.material - data->materials] : 0;
            part.nodeName = nodeName;
            part.parentName = parentName;
            part.pivot = point(transform, {});
            for (size_t vertexIndex = 0; vertexIndex < positions->count; vertexIndex++) {
                float position[3], normal[3] = {0, 1, 0}, uv[2] = {0, 0};
                cgltf_accessor_read_float(positions, vertexIndex, position, 3);
                if (normals)
                    cgltf_accessor_read_float(normals, vertexIndex, normal, 3);
                if (texcoords)
                    cgltf_accessor_read_float(texcoords, vertexIndex, uv, 2);
                V3 vertex = point(transform, {position[0], position[1], position[2]});
                low = {std::min(low.x, vertex.x), std::min(low.y, vertex.y),
                       std::min(low.z, vertex.z)};
                high = {std::max(high.x, vertex.x), std::max(high.y, vertex.y),
                        std::max(high.z, vertex.z)};
                part.geometry.v.push_back(
                    {vertex,
                     norm(direction(transform, {normal[0], normal[1], normal[2]})),
                     uv[0],
                     uv[1],
                     {1, 1, 1},
                     0});
            }
            size_t indexCount = primitive.indices ? primitive.indices->count : positions->count;
            for (size_t index = 0; index < indexCount; index++)
                part.geometry.i.push_back(primitive.indices
                                              ? cgltf_accessor_read_index(primitive.indices, index)
                                              : index);
            parts.push_back(std::move(part));
        }
    }
    if (parts.empty()) {
        cgltf_free(data);
        return false;
    }
    V3 extent = high - low;
    float maximumExtent = std::max({extent.x, extent.y, extent.z});
    float factor =
        options.targetLength > 0 && maximumExtent > 0 ? options.targetLength / maximumExtent : 1.f;
    V3 center =
        options.centerAndGround ? V3{(low.x + high.x) * .5f, low.y, (low.z + high.z) * .5f} : V3{};
    V3 boundsCenter =
        (V3{(low.x + high.x) * .5f, (low.y + high.y) * .5f, (low.z + high.z) * .5f} - center) *
        factor;
    float radius = length(extent) * factor * .5f;
    for (auto& part : parts) {
        for (auto& vertex : part.geometry.v)
            vertex.p = (vertex.p - center) * factor;
        int meshIndex = addMesh(part.geometry, part.material, boundsCenter, radius);
        auto& mesh = meshes[meshIndex];
        mesh.source = std::move(part.geometry);
        mesh.nodeName = std::move(part.nodeName);
        mesh.parentName = std::move(part.parentName);
        mesh.pivot = (part.pivot - center) * factor;
        out.push_back(meshIndex);
    }
    std::printf("Loaded %s: %zu parts, scale %.3f\n", path, parts.size(), factor);
    cgltf_free(data);
    return !out.empty();
}
void Renderer::render(const std::vector<Draw>& draws, V3 eye, V3 target, V3 shadowCenter,
                      float time, float fovRadians, const std::vector<UiVertex>& ui) {
    if (!ready || !width || !height || draws.size() > 4096)
        return;
    struct Global {
        M4 vp, light;
        float eye[4], sun[4], fog[4];
    } g;
    V3 sun = norm({-.48f, .8f, -.24f});
    g.vp = perspective(fovRadians, float(width) / height, .15f, 2800) * lookAt(eye, target);
    g.light = ortho(100, 1, 480) * lookAt(shadowCenter + sun * 220, shadowCenter);
    g.eye[0] = eye.x;
    g.eye[1] = eye.y;
    g.eye[2] = eye.z;
    g.eye[3] = time;
    g.sun[0] = sun.x;
    g.sun[1] = sun.y;
    g.sun[2] = sun.z;
    g.sun[3] = 0;
    g.fog[0] = .48f;
    g.fog[1] = .67f;
    g.fog[2] = .77f;
    g.fog[3] = 1900;
    queue.WriteBuffer(globals, 0, &g, sizeof(g));
    std::vector<float> objData(draws.size() * 64);
    for (size_t i = 0; i < draws.size(); i++) {
        auto& m = materials[meshes[draws[i].mesh].material];
        float* p = objData.data() + i * 64;
        std::memcpy(p, draws[i].transform.m, 64);
        p[16] = m.color.x;
        p[17] = m.color.y;
        p[18] = m.color.z;
        p[19] = m.alpha;
        p[20] = m.rough;
        p[21] = m.metal;
        p[22] = m.foliage ? 2 : m.texture.Get() != white.Get() ? 1 : 0;
        p[23] = m.blend ? 1 : 0;
        p[24] = m.normal ? 1 : 0;
        p[25] = m.orm ? 1 : 0;
        p[26] = m.cutoff;
        p[27] = m.uvScale;
        p[28] = m.emissiveColor.x;
        p[29] = m.emissiveColor.y;
        p[30] = m.emissiveColor.z;
        p[31] = 0;
        p[32] = m.normalScale;
        p[33] = m.aoStrength;
        p[34] = m.clearcoat;
        p[35] = 0;
    }
    queue.WriteBuffer(objects, 0, objData.data(), objData.size() * 4);
    if (!ui.empty()) {
        const uint64_t bytes = ui.size() * sizeof(UiVertex);
        if (bytes > uiCapacity) {
            while (uiCapacity < bytes)
                uiCapacity *= 2;
            uiVertices =
                buffer(device, uiCapacity, wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst);
        }
        queue.WriteBuffer(uiVertices, 0, ui.data(), bytes);
    }
    wgpu::SurfaceTexture st{};
    surface.GetCurrentTexture(&st);
    if (!st.texture)
        return;
    auto surfaceView = st.texture.CreateView();
    auto encoder = device.CreateCommandEncoder();
    drawCalls = 0;
    using namespace engine::graphics;
    FrameGraph graph;
    const std::string colorFormat =
        format == wgpu::TextureFormat::BGRA8Unorm ? "bgra8unorm" : "rgba8unorm";
    auto shadowResource = graph.resource(
        "shadow",
        {Kind::Texture, "depth32float", 2048, 2048, 1, Usage::Sampled | Usage::DepthAttachment},
        true);
    auto colorResource = graph.resource(
        "multisample scene color", {Kind::Texture, colorFormat, uint32_t(width), uint32_t(height),
                                    4, Usage::ColorAttachment | Usage::ResolveSource});
    auto depthResource =
        graph.resource("scene depth", {Kind::Texture, "depth32float", uint32_t(width),
                                       uint32_t(height), 4, mask(Usage::DepthAttachment)});
    auto surfaceResource =
        graph.resource("surface",
                       {Kind::Texture, colorFormat, uint32_t(width), uint32_t(height), 1,
                        Usage::ResolveTarget | Usage::Present},
                       true);
    auto shadowVersion = graph.next(shadowResource), sceneColor = graph.next(colorResource),
         sceneDepth = graph.next(depthResource);
    auto uiColor = graph.next(colorResource), uiDepth = graph.next(depthResource),
         presentColor = graph.next(surfaceResource);
    auto draw = [&](wgpu::RenderPassEncoder& pass, size_t i) {
        auto& m = meshes[draws[i].mesh];
        uint32_t offset = i * 256;
        pass.SetBindGroup(1, materialBG[m.material], 1, &offset);
        pass.SetVertexBuffer(0, m.vb);
        pass.SetIndexBuffer(m.ib, wgpu::IndexFormat::Uint32);
        pass.DrawIndexed(m.count);
        drawCalls++;
    };
    graph.pass(
        "shadow", {Access::write(shadowVersion, Usage::DepthAttachment)},
        [&] {
            wgpu::RenderPassDepthStencilAttachment attachment{};
            attachment.view = shadowView;
            attachment.depthLoadOp = wgpu::LoadOp::Clear;
            attachment.depthStoreOp = wgpu::StoreOp::Store;
            attachment.depthClearValue = 1;
            wgpu::RenderPassDescriptor desc{};
            desc.depthStencilAttachment = &attachment;
            auto pass = encoder.BeginRenderPass(&desc);
            pass.SetPipeline(shadowPipe);
            pass.SetBindGroup(0, shadowBG);
            for (size_t i = 0; i < draws.size(); ++i) {
                auto& m = meshes[draws[i].mesh];
                if (materials[m.material].blend)
                    continue;
                if (length(point(draws[i].transform, m.center) - shadowCenter) > m.radius + 180)
                    continue;
                draw(pass, i);
            }
            pass.End();
        },
        false);
    graph.pass(
        "forward scene",
        {Access::read(shadowVersion, Usage::Sampled),
         Access::write(sceneColor, Usage::ColorAttachment),
         Access::write(sceneDepth, Usage::DepthAttachment, false)},
        [&] {
            wgpu::RenderPassColorAttachment color{};
            color.view = msaaView;
            color.loadOp = wgpu::LoadOp::Clear;
            color.storeOp = wgpu::StoreOp::Store;
            color.clearValue = {.7, .8, .85, 1};
            wgpu::RenderPassDepthStencilAttachment depthAttachment{};
            depthAttachment.view = depthView;
            depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
            depthAttachment.depthStoreOp = wgpu::StoreOp::Discard;
            depthAttachment.depthClearValue = 1;
            wgpu::RenderPassDescriptor desc{};
            desc.colorAttachmentCount = 1;
            desc.colorAttachments = &color;
            desc.depthStencilAttachment = &depthAttachment;
            auto pass = encoder.BeginRenderPass(&desc);
            pass.SetPipeline(skyPipe);
            pass.Draw(3);
            pass.SetPipeline(opaque);
            pass.SetBindGroup(0, globalBG);
            for (size_t i = 0; i < draws.size(); ++i)
                if (!materials[meshes[draws[i].mesh].material].blend)
                    draw(pass, i);
            pass.SetPipeline(transparent);
            for (size_t i = 0; i < draws.size(); ++i)
                if (materials[meshes[draws[i].mesh].material].blend)
                    draw(pass, i);
            pass.End();
        },
        false);
    graph.pass(
        "UI and resolve",
        {Access::read(sceneColor, Usage::ColorAttachment),
         Access::write(uiColor, Usage::ColorAttachment, false),
         Access::read(uiColor, Usage::ResolveSource),
         Access::write(uiDepth, Usage::DepthAttachment, false),
         Access::write(presentColor, Usage::ResolveTarget)},
        [&] {
            wgpu::RenderPassColorAttachment color{};
            color.view = msaaView;
            color.resolveTarget = surfaceView;
            color.loadOp = wgpu::LoadOp::Load;
            color.storeOp = wgpu::StoreOp::Discard;
            wgpu::RenderPassDepthStencilAttachment depthAttachment{};
            depthAttachment.view = depthView;
            depthAttachment.depthLoadOp = wgpu::LoadOp::Clear;
            depthAttachment.depthStoreOp = wgpu::StoreOp::Discard;
            depthAttachment.depthClearValue = 1;
            wgpu::RenderPassDescriptor desc{};
            desc.colorAttachmentCount = 1;
            desc.colorAttachments = &color;
            desc.depthStencilAttachment = &depthAttachment;
            auto pass = encoder.BeginRenderPass(&desc);
            if (!ui.empty()) {
                pass.SetPipeline(uiPipe);
                pass.SetBindGroup(0, uiBG);
                pass.SetVertexBuffer(0, uiVertices);
                pass.Draw(uint32_t(ui.size()));
            }
            pass.End();
        },
        false);
    graph.pass(
        "present", {Access::read(presentColor, Usage::Present)},
        [&] {
            auto command = encoder.Finish();
            queue.Submit(1, &command);
        },
        true);
    if (!graph.compile()) {
        error = "Frame graph: " + graph.error();
        ready = false;
        if (host.reportError)
            host.reportError(error.c_str());
        return;
    }
    frameGraphPasses = uint32_t(graph.schedule().size());
    frameGraphTextureSlots = uint32_t(graph.slots().size());
    // The compiled lifetimes select real whole-texture slots. Compatible slots
    // persist across frames; resize replaces only mismatching descriptions.
    const auto& slots = graph.slots();
    frameTextures.resize(slots.size());
    frameViews.resize(slots.size());
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i < frameTextureDescriptions.size() && frameTextureDescriptions[i] == slots[i] &&
            frameTextures[i])
            continue;
        wgpu::TextureDescriptor desc{};
        desc.size = {slots[i].width, slots[i].height, 1};
        desc.sampleCount = slots[i].samples;
        desc.format =
            slots[i].format == "depth32float" ? wgpu::TextureFormat::Depth32Float : format;
        desc.usage = wgpu::TextureUsage::RenderAttachment;
        frameTextures[i] = device.CreateTexture(&desc);
        frameViews[i] = frameTextures[i].CreateView();
    }
    frameTextureDescriptions = slots;
    for (auto& lifetime : graph.lifetimes())
        if (!lifetime.imported) {
            if (lifetime.resource == colorResource) {
                msaa = frameTextures[lifetime.slot];
                msaaView = frameViews[lifetime.slot];
            } else if (lifetime.resource == depthResource) {
                depth = frameTextures[lifetime.slot];
                depthView = frameViews[lifetime.slot];
            }
        }
    graph.execute();
}
} // namespace engine
