#pragma once
#include "graphics/frame_graph/frame_graph.hpp"
#include "graphics/render_object.hpp"
#include "particles/particle_system.hpp"
#include "ui/vertex.hpp"
#include <engine/math.hpp>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <webgpu/webgpu_cpp.h>
namespace engine {
struct Vertex {
    V3 p, n;
    float u = 0, v = 0;
    V3 color{1, 1, 1};
    float kind = 0;
};
struct Geometry {
    std::vector<Vertex> v;
    std::vector<uint32_t> i;
    void tri(Vertex a, Vertex b, Vertex c);
    void quad(V3 a, V3 b, V3 c, V3 d, V3 color, float kind = 0);
};
struct Material {
    V3 color{1, 1, 1}, emissiveColor{0, 0, 0};
    float rough = 1.f, metal = 0, alpha = 1, normalScale = 1, aoStrength = 0, clearcoat = 0;
    wgpu::TextureView texture, normal, orm, occlusion, emissive;
    float cutoff = 0, uvScale = 1;
    bool foliage = false, blend = false;
};
struct Mesh {
    wgpu::Buffer vb, ib;
    uint32_t count = 0;
    int material = 0;
    V3 center;
    float radius = 100;
    std::string nodeName, parentName;
    V3 pivot;
    Geometry source;
};
struct ModelImportOptions {
    float targetLength = 0.f;
    bool centerAndGround = true;
    std::function<void(std::string_view, Material&)> materialOverride;
};
using Draw = engine::graphics::Draw;
// Screen-space vertices are deliberately separate from world geometry.  The UI
// is a normal WebGPU pass and has no browser layout or DOM dependency.
struct Renderer {
    struct HostCallbacks {
        std::function<wgpu::Surface(wgpu::Instance)> createSurface;
        std::function<void(const char*)> reportError;
    };
    HostCallbacks host;
    std::vector<wgpu::Texture> frameTextures;
    std::vector<wgpu::TextureView> frameViews;
    std::vector<engine::graphics::ResourceDesc> frameTextureDescriptions;
    uint64_t uiCapacity = 1024 * 1024;
    uint64_t particleCapacity = 4096 * 48;
    uint32_t frameGraphPasses = 0, frameGraphTextureSlots = 0;
    wgpu::Instance instance;
    wgpu::Adapter adapter;
    wgpu::Device device;
    wgpu::Queue queue;
    wgpu::Surface surface;
    wgpu::TextureFormat format;
    wgpu::RenderPipeline opaque, transparent, shadowPipe, skyPipe, uiPipe, particlePipe;
    wgpu::Buffer globals, objects, uiVertices, particleVertices, particleGlobals;
    wgpu::BindGroup globalBG, shadowBG, uiBG, particleGlobalBG;
    wgpu::BindGroupLayout objectLayout, uiLayout;
    wgpu::BindGroupLayout particleGlobalLayout;
    wgpu::Sampler sampler, shadowSampler, uiSampler;
    wgpu::Texture depth, msaa, shadow;
    wgpu::TextureView depthView, msaaView, shadowView, white, flatNormal;
    std::unordered_map<std::string, wgpu::TextureView> textureCache;
    std::vector<wgpu::BindGroup> materialBG;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    int width = 0, height = 0, drawCalls = 0;
    bool ready = false;
    std::string error;
    void init(void (*callback)(bool), HostCallbacks hostCallbacks);
    void setup();
    void resize(int w, int h);
    int addMaterial(Material m, const unsigned char* pixels = nullptr, int w = 1, int h = 1);
    int addMesh(const Geometry& g, int mat, V3 center = {}, float radius = 100);
    wgpu::TextureView uploadTexture(const unsigned char*, int, int, bool srgb);
    void setUiAtlas(const unsigned char* pixels, int w, int h);
    wgpu::TextureView loadTexture(const std::string&, bool srgb = true);
    int loadSurface(const char* folder, float uvScale = 1, V3 tint = {1, 1, 1});
    bool loadModel(const char* path, std::vector<int>& out, const ModelImportOptions& options = {});
    void render(const std::vector<Draw>& draws, V3 eye, V3 target, V3 shadowCenter, float time,
                float fovRadians, const std::vector<UiVertex>& ui = {},
                std::span<const particles::ParticleInstance> particles = {});
};
} // namespace engine
