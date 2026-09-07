#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprland/src/render/pass/PassElement.hpp>
#include <hyprland/src/render/pass/TextureMatteElement.hpp>
#include <hyprland/src/state/MonitorState.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/SharedDefs.hpp>
#include <json-c/json.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace Hyprutils::Memory;
using namespace Render;

namespace {
HANDLE PHANDLE = nullptr;
constexpr float REACH = 260.F;
// Extend the matte beneath the card so bilinear mask sampling and antialiased
// card edges can never expose an unblurred seam. This is real compositor blur;
// the later popup layer simply covers the underlap.
constexpr float UNDERLAP = 12.F;
constexpr int MASK_SCALE = 4;

struct Card {
    std::string output;
    float x = 0, y = 0, w = 0, h = 0, radius = 0, opacity = 1;
};

struct MaskResource {
    SP<IFramebuffer> fb;
    int width = 0, height = 0;
    uint64_t generation = 0;
};

std::mutex cardsMutex;
std::vector<Card> cards;
std::set<std::string> dirtyOutputs;
std::atomic<uint64_t> generation = 1;
std::jthread serverThread;
std::string socketPath;
int wakeRead = -1, wakeWrite = -1;
CHyprSignalListener renderListener;
std::unordered_map<std::string, MaskResource> masks;

float roundedDistance(float px, float py, const Card& c) {
    const float radius = std::clamp(c.radius, 0.F, std::min(c.w, c.h) * .5F);
    const float qx = std::abs(px - (c.x + c.w * .5F)) - (c.w * .5F - radius);
    const float qy = std::abs(py - (c.y + c.h * .5F)) - (c.h * .5F - radius);
    return std::hypot(std::max(qx, 0.F), std::max(qy, 0.F)) + std::min(std::max(qx, qy), 0.F) - radius;
}

bool sameCards(const std::vector<Card>& a, const std::vector<Card>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        const auto& x = a[i];
        const auto& y = b[i];
        if (x.output != y.output || x.x != y.x || x.y != y.y || x.w != y.w || x.h != y.h || x.radius != y.radius || x.opacity != y.opacity) return false;
    }
    return true;
}

std::vector<Card> cardsFor(const std::string& output) {
    std::scoped_lock lock(cardsMutex);
    std::vector<Card> result;
    for (const auto& card : cards)
        if (card.output == output && card.opacity > .01F)
            result.push_back(card);
    return result;
}

class CGradualBlurElement final : public IPassElement {
  public:
    CGradualBlurElement(PHLMONITOR monitor, std::vector<Card> cards_) : m_monitor(monitor), m_cards(std::move(cards_)) {}

    std::vector<UP<IPassElement>> draw() override {
        std::vector<UP<IPassElement>> result;
        if (!m_monitor || m_cards.empty()) return result;

        const int fullW = std::max(1, static_cast<int>(std::round(m_monitor->m_transformedSize.x)));
        const int fullH = std::max(1, static_cast<int>(std::round(m_monitor->m_transformedSize.y)));
        const int maskW = std::max(1, (fullW + MASK_SCALE - 1) / MASK_SCALE);
        const int maskH = std::max(1, (fullH + MASK_SCALE - 1) / MASK_SCALE);
        const float outputScale = std::max(.01F, static_cast<float>(m_monitor->m_scale));
        auto& resource = masks[m_monitor->m_name];
        const auto currentGeneration = generation.load();

        if (!resource.fb || resource.width != maskW || resource.height != maskH) {
            resource.fb = g_pHyprRenderer->createFB("gradual-blur-matte");
            if (!resource.fb->alloc(maskW, maskH, DRM_FORMAT_ARGB8888)) return result;
            resource.width = maskW;
            resource.height = maskH;
            resource.generation = 0;
        }

        if (resource.generation != currentGeneration) {
            std::vector<uint8_t> pixels(static_cast<size_t>(maskW) * maskH * 4, 0);
            for (int y = 0; y < maskH; ++y) {
                const float py = (y + .5F) * MASK_SCALE / outputScale;
                for (int x = 0; x < maskW; ++x) {
                    const float px = (x + .5F) * MASK_SCALE / outputScale;
                    float alpha = 0.F;
                    for (const auto& card : m_cards) {
                        const float d = roundedDistance(px, py, card);
                        if (d <= -UNDERLAP || d >= REACH) continue;
                        const float t = std::max(d, 0.F) / REACH;
                        const float smooth = t * t * (3.F - 2.F * t);
                        const float innerT = std::clamp((d + UNDERLAP) / (UNDERLAP * .6F), 0.F, 1.F);
                        const float innerSmooth = innerT * innerT * (3.F - 2.F * innerT);
                        const float strength = std::pow(1.F - smooth, 1.35F) * innerSmooth * card.opacity;
                        alpha = std::max(alpha, strength);
                    }
                    const auto value = static_cast<uint8_t>(std::clamp(alpha, 0.F, 1.F) * 255.F + .5F);
                    const size_t p = (static_cast<size_t>(y) * maskW + x) * 4;
                    pixels[p] = pixels[p + 1] = pixels[p + 2] = pixels[p + 3] = value;
                }
            }
            CRegion all{0., 0., static_cast<double>(maskW), static_cast<double>(maskH)};
            resource.fb->getTexture()->update(DRM_FORMAT_ARGB8888, pixels.data(), maskW * 4, all);
            resource.generation = currentGeneration;
        }

        // Hyprland's render pass expands this damage before drawing us because
        // needsLiveBlur() is true. Blur only that fully repainted region: the
        // offloaded framebuffer is intentionally undefined everywhere else.
        CRegion blurDamage = g_pHyprRenderer->m_renderData.damage.copy();
        auto blurred = g_pHyprRenderer->blurMainFramebuffer(1.F, &blurDamage);
        if (!blurred) return result;
        result.emplace_back(makeUnique<CTextureMatteElement>(CTextureMatteElement::STextureMatteData{
            .box = CBox{0., 0., static_cast<double>(fullW), static_cast<double>(fullH)},
            .tex = blurred, .fb = resource.fb, .disableTransformAndModify = true}));
        return result;
    }

    bool needsLiveBlur() override { return true; }
    bool needsPrecomputeBlur() override { return false; }
    const char* passName() override { return "GradualBlur"; }
    ePassElementType type() override { return EK_CUSTOM; }
    std::optional<CBox> boundingBox() override {
        if (!m_monitor || m_cards.empty()) return std::nullopt;

        float left = static_cast<float>(m_monitor->m_size.x);
        float top = static_cast<float>(m_monitor->m_size.y);
        float right = 0.F;
        float bottom = 0.F;
        for (const auto& card : m_cards) {
            left = std::min(left, card.x - REACH);
            top = std::min(top, card.y - REACH);
            right = std::max(right, card.x + card.w + REACH);
            bottom = std::max(bottom, card.y + card.h + REACH);
        }

        left = std::clamp(left, 0.F, static_cast<float>(m_monitor->m_size.x));
        top = std::clamp(top, 0.F, static_cast<float>(m_monitor->m_size.y));
        right = std::clamp(right, left, static_cast<float>(m_monitor->m_size.x));
        bottom = std::clamp(bottom, top, static_cast<float>(m_monitor->m_size.y));
        return CBox{left, top, right - left, bottom - top};
    }
    bool disableSimplification() override { return true; }

  private:
    PHLMONITOR m_monitor;
    std::vector<Card> m_cards;
};

// Main thread (dispatcher or exit): repaint every monitor whose card set
// changed. Full-monitor damage is required because the compositor's cached
// framebuffer outside a partial damage region still has the previous blur
// baked in — anything less leaves ghost halos behind on shrink.
void drainAndDamage() {
    std::set<std::string> dirty;
    {
        std::scoped_lock lock(cardsMutex);
        dirty.swap(dirtyOutputs);
    }
    if (dirty.empty()) return;
    const bool all = dirty.count("") > 0;
    dirty.erase("");
    for (const auto& monitor : State::monitorState()->monitors()) {
        if (!monitor || monitor->isMirror()) continue;
        if (all || dirty.contains(monitor->m_name)) g_pHyprRenderer->damageMonitor(monitor);
    }
}

// Server thread only: nudge the main loop. Waking Hyprland through its own
// pipe fd is the thread-safe way to get main-thread work scheduled the
// moment new geometry arrives; writes are atomic and coalesce freely.
void requestWakeup() {
    if (wakeWrite < 0) return;
    if (::write(wakeWrite, "w", 1) < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {}
}

// Main thread: registered through Hyprland's own event loop (doOnReadable).
// The waiter is one-shot; re-arm after every fire. On plugin exit we close
// the write end, which hangs the pipe up — Hyprland then discards the waiter
// through onFdReadableFail without ever calling our code, so nothing can
// touch this library after it is unloaded.
void armWakeup();

void onWakeup() {
    char drain[64];
    while (::read(wakeRead, drain, sizeof(drain)) > 0) {}
    drainAndDamage();
    armWakeup();
}

void armWakeup() {
    if (wakeRead < 0 || !g_pEventLoopManager) return;
    g_pEventLoopManager->doOnReadable(Hyprutils::OS::CFileDescriptor(::dup(wakeRead)), onWakeup);
}

void setCards(std::vector<Card> next) {
    {
        std::scoped_lock lock(cardsMutex);
        if (sameCards(cards, next)) return;
        for (const auto& card : cards) dirtyOutputs.insert(card.output);
        for (const auto& card : next) dirtyOutputs.insert(card.output);
        cards = std::move(next);
    }
    generation.fetch_add(1, std::memory_order_release);
    requestWakeup();
}

void serve(std::stop_token stop) {
    ::unlink(socketPath.c_str());
    const int server = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) return;
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", socketPath.c_str());
    if (::bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || ::listen(server, 2) < 0) {
        ::close(server); return;
    }
    timeval timeout{0, 250000};
    ::setsockopt(server, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(server, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    while (!stop.stop_requested()) {
        fd_set fds; FD_ZERO(&fds); FD_SET(server, &fds);
        timeval wait{0, 250000};
        if (::select(server + 1, &fds, nullptr, nullptr, &wait) <= 0) continue;
        const int client = ::accept4(server, nullptr, nullptr, SOCK_CLOEXEC);
        if (client < 0) continue;
        // accept() does not inherit timeouts: set them on the client socket
        // explicitly, otherwise a stop request would hang on a blocking recv.
        timeval clientTimeout{0, 250000};
        ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &clientTimeout, sizeof(clientTimeout));
        ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &clientTimeout, sizeof(clientTimeout));
        std::string pending;
        char buffer[8192];
        while (!stop.stop_requested()) {
            const auto count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count == 0) break; // orderly client disconnect
            if (count < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue; // idle, keep the connection
                break;
            }
            pending.append(buffer, static_cast<size_t>(count));
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos) {
                auto line = pending.substr(0, newline); pending.erase(0, newline + 1);
                json_object* payload = json_tokener_parse(line.c_str());
                json_object* array = nullptr;
                if (payload && json_object_object_get_ex(payload, "cards", &array) && json_object_is_type(array, json_type_array)) {
                    std::vector<Card> next;
                    const auto number = json_object_array_length(array);
                    for (size_t i = 0; i < number; ++i) {
                        json_object* item = json_object_array_get_idx(array, i);
                        auto stringValue = [item](const char* key) {
                            json_object* value = nullptr;
                            return json_object_object_get_ex(item, key, &value) ? std::string(json_object_get_string(value)) : std::string{};
                        };
                        auto floatValue = [item](const char* key, float fallback) {
                            json_object* value = nullptr;
                            return json_object_object_get_ex(item, key, &value) ? static_cast<float>(json_object_get_double(value)) : fallback;
                        };
                        next.push_back({stringValue("output"), floatValue("x", 0), floatValue("y", 0),
                            floatValue("w", 0), floatValue("h", 0), floatValue("radius", 0), floatValue("opacity", 1)});
                    }
                    setCards(std::move(next));
                }
                if (payload) json_object_put(payload);
            }
        }
        ::close(client);
        setCards({});
    }
    ::close(server);
    ::unlink(socketPath.c_str());
}
}

APICALL EXPORT std::string PLUGIN_API_VERSION() { return HYPRLAND_API_VERSION; }

APICALL EXPORT PLUGIN_DESCRIPTION_INFO PLUGIN_INIT(HANDLE handle) {
    PHANDLE = handle;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    socketPath = std::string(runtime ? runtime : "/tmp") + "/gradual-blur.sock";
    int fds[2] = {-1, -1};
    if (::pipe2(fds, O_CLOEXEC | O_NONBLOCK) == 0) {
        wakeRead  = fds[0];
        wakeWrite = fds[1];
        armWakeup();
    }
    renderListener = Event::bus()->m_events.render.stage.listen([](eRenderStage stage) {
        if (stage != RENDER_POST_WINDOWS) return;
        auto monitor = g_pHyprRenderer->m_renderData.pMonitor.lock();
        if (!monitor) return;
        auto current = cardsFor(monitor->m_name);
        if (!current.empty()) g_pHyprRenderer->currentPass().add(makeUnique<CGradualBlurElement>(monitor, std::move(current)));
    });
    serverThread = std::jthread(serve);
    return {"aura-blur", "Continuous live radial blur behind shell popups", "Yeshuah Franco", "1.0.1"};
}

APICALL EXPORT void PLUGIN_EXIT() {
    if (serverThread.joinable()) { serverThread.request_stop(); serverThread.join(); }
    renderListener.reset();
    if (wakeWrite >= 0) { ::close(wakeWrite); wakeWrite = -1; } // hang up: discards any armed waiter without running it
    if (wakeRead >= 0) { ::close(wakeRead); wakeRead = -1; }
    masks.clear();
    {
        std::scoped_lock lock(cardsMutex);
        cards.clear();
        dirtyOutputs.clear();
    }
    generation.fetch_add(1);
    // We are on the main thread here; repaint once without blur so a stale
    // matte never outlives the plugin.
    for (const auto& monitor : State::monitorState()->monitors())
        if (monitor && !monitor->isMirror()) g_pHyprRenderer->damageMonitor(monitor);
}
