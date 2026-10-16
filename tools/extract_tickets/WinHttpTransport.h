#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace OST::ExtractTickets {

struct HttpResponse {
    int statusCode{0};
    int32_t steamEResult{0};
    std::string body;
    std::string errorMessage;

    [[nodiscard]] bool IsSuccess() const noexcept {
        return statusCode >= 200 && statusCode < 300;
    }
};

class WinHttpTransport {
public:
    WinHttpTransport();
    ~WinHttpTransport();

    WinHttpTransport(const WinHttpTransport&) = delete;
    WinHttpTransport& operator=(const WinHttpTransport&) = delete;

    [[nodiscard]] HttpResponse Get(
        std::string_view url,
        const std::vector<std::string>& extraHeaders = {},
        uint32_t timeoutMs = 0);

    [[nodiscard]] HttpResponse Post(
        std::string_view url,
        std::string_view postData,
        std::string_view contentType = "application/x-www-form-urlencoded",
        const std::vector<std::string>& extraHeaders = {},
        uint32_t timeoutMs = 0);

    void SetTimeouts(int resolveMs, int connectMs, int sendMs, int receiveMs);

private:
    void* m_hSession{nullptr};
    std::wstring m_cachedConnectHost;
    uint16_t m_cachedConnectPort{0};
    void* m_hCachedConnect{nullptr};

    void* GetOrCreateConnection(const std::wstring& host, uint16_t port, bool* outWasCached = nullptr);
    void InvalidateConnection();
    [[nodiscard]] HttpResponse ExecuteRequest(
        const wchar_t* verb,
        std::string_view url,
        std::string_view postData,
        std::string_view contentType,
        const std::vector<std::string>& extraHeaders,
        uint32_t timeoutMs);
};

class WebSocketClient {
public:
    WebSocketClient();
    ~WebSocketClient();

    WebSocketClient(const WebSocketClient&) = delete;
    WebSocketClient& operator=(const WebSocketClient&) = delete;

    // Connects to a WebSocket endpoint (e.g. wss://cm.steampowered.com/cmsocket/)
    [[nodiscard]] bool Connect(std::string_view wssUrl, uint32_t timeoutMs = 10000);

    // Sends a message frame (binary or text)
    bool Send(std::span<const uint8_t> data, bool isBinary = true);

    // Receives a complete message frame
    [[nodiscard]] bool Receive(std::vector<uint8_t>& outData, bool& isBinary, uint32_t timeoutMs = 15000);

    void Close();

    [[nodiscard]] bool IsConnected() const noexcept { return m_hWebSocket != nullptr; }

private:
    void* m_hSession{nullptr};
    void* m_hConnect{nullptr};
    void* m_hRequest{nullptr};
    void* m_hWebSocket{nullptr};
    std::vector<uint8_t> m_recvChunk;
};

} // namespace OST::ExtractTickets
