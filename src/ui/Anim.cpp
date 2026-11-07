#include "ui/Anim.h"
#include "ui/Theme.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace cs::ui {

namespace {
struct Entry {
    float value = 0.0f;
    float velocity = 0.0f;
    double timer = -1.0;
    int lastFrame = 0;
};
std::unordered_map<ImGuiID, Entry> g_table;
int g_frame = 0;

Entry& Get(ImGuiID id, float init)
{
    auto it = g_table.find(id);
    if (it == g_table.end()) {
        Entry e;
        e.value = init;
        it = g_table.emplace(id, e).first;
    }
    it->second.lastFrame = g_frame;
    return it->second;
}
} // namespace

float Anim(ImGuiID id, float target, float speed)
{
    Entry& e = Get(id, target);
    if (!Animations()) {
        e.value = target;
        return target;
    }
    const float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    e.value += (target - e.value) * (1.0f - std::exp(-speed * dt));
    if (std::fabs(target - e.value) < 0.0005f) e.value = target;
    return e.value;
}

float AnimHover(ImGuiID id, bool on)
{
    // ~150 ms in, ~250 ms out: leaving feels softer than arriving.
    return Anim(id, on ? 1.0f : 0.0f, on ? 16.0f : 9.5f);
}

float Spring(ImGuiID id, float target, const SpringParams& sp)
{
    Entry& e = Get(id, target);
    if (!Animations()) {
        e.value = target;
        e.velocity = 0.0f;
        return target;
    }
    // Damped spring (unit mass), integrated in fixed 1/240 s steps so it behaves the same at any frame rate.
    float dt = std::min(ImGui::GetIO().DeltaTime, 0.1f);
    while (dt > 0.0f) {
        const float h = std::min(dt, 1.0f / 240.0f);
        const float force = sp.stiffness * (target - e.value) - sp.damping * e.velocity;
        e.velocity += force * h;
        e.value += e.velocity * h;
        dt -= h;
    }
    if (std::fabs(target - e.value) < 0.0002f && std::fabs(e.velocity) < 0.002f) {
        e.value = target;
        e.velocity = 0.0f;
    }
    return e.value;
}

float SpringVelocity(ImGuiID id) { return Get(id, 0.0f).velocity; }
void SpringKick(ImGuiID id, float velocity) { Get(id, 0.0f).velocity += velocity; }

void AnimSet(ImGuiID id, float value) { Get(id, value).value = value; }
ImGuiID AnimId(ImGuiID base, unsigned salt) { return base ^ (salt * 0x9E3779B1u) ^ 0x5bd1e995u; }

void TimerStart(ImGuiID id) { Get(id, 0.0f).timer = ImGui::GetTime(); }
float TimerElapsed(ImGuiID id)
{
    Entry& e = Get(id, 0.0f);
    return e.timer < 0.0 ? 1e6f : (float)(ImGui::GetTime() - e.timer);
}
void TimerStop(ImGuiID id) { Get(id, 0.0f).timer = -1.0; }

float EaseOutCubic(float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float u = 1.0f - t;
    return 1.0f - u * u * u;
}

float EaseInOutCubic(float t)
{
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    return t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) * 0.5f;
}

void AnimNewFrame()
{
    ++g_frame;
    if (g_frame % 300 != 0) return;
    for (auto it = g_table.begin(); it != g_table.end();) {
        const bool timerLive = it->second.timer >= 0.0 && ImGui::GetTime() - it->second.timer < 10.0;
        if (g_frame - it->second.lastFrame > 600 && !timerLive) it = g_table.erase(it);
        else ++it;
    }
}

} // namespace cs::ui
